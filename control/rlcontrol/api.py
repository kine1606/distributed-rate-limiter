"""The management API (Phase 2, P2 and P3; docs/PRODUCT_PLAN.md section 8). REST over HTTP, documented
by its OpenAPI page (/docs).

Two kinds of caller, each with its own credential (Authorization: Bearer ...):
- the platform operator, with RL_ADMIN_TOKEN: creates merchants, rotates or revokes their keys, and
  closes a month for billing;
- a merchant, with its management key: defines its plans and their prices, creates customers, sets
  their limits, grants boosts and top-ups, suspends, reads usage, keeps its customers' payment methods
  (as the provider's tokens), reads invoices, registers its webhook. The merchant always comes from the
  key, never from the URL, so no request can reach another merchant's customers.

A merchant's check key (for its gateway's calls to the limiter) and its management key are separate:
a gateway that leaks its key cannot change plans.

    RL_ADMIN_TOKEN=... RL_REDIS=127.0.0.1:7001 control/.venv/bin/uvicorn rlcontrol.api:app --app-dir control
"""
import hmac
import json
import os
import pathlib
from typing import Literal, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Path, Query, Response, status
from fastapi.responses import FileResponse
from pydantic import BaseModel, ConfigDict, Field
from redis.cluster import RedisCluster

from . import billing, payments, periods, store, webhooks

MERCHANT = r"^[A-Za-z0-9_-]{1,64}$"
# The merchant portal, served by the API so it needs no cross-origin access. Its CSP lets it load and call
# nothing but this server, so neither an injected script nor a stolen page can send the key elsewhere.
PORTAL = pathlib.Path(__file__).with_name("portal")
PORTAL_FILES = {"": ("index.html", "text/html; charset=utf-8"), "portal.js": ("portal.js", "text/javascript"),
                "portal.css": ("portal.css", "text/css")}
PORTAL_HEADERS = {"Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; "
                                             "connect-src 'self'; base-uri 'none'; form-action 'self'; "
                                             "frame-ancestors 'none'",
                  "X-Content-Type-Options": "nosniff", "Referrer-Policy": "no-referrer"}
CUSTOMER = r"^[A-Za-z0-9._:@-]{1,128}$"
PLAN = r"^[a-z0-9_-]{1,32}$"


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid")


class PlanTerms(Strict):
    daily: int = Field(ge=0, le=store.UINT32, description="the quota per period")
    burst: int = Field(ge=0, le=store.UINT32, description="burst capacity, in calls")
    refill: int = Field(ge=0, le=store.UINT32, description="burst refill, calls per second")
    period: Literal["day", "month"] = "day"
    overage: Literal["hard", "grace", "postpaid"] = Field(
        "hard", description="at the quota: hard blocks; grace admits `allowance` more, free; postpaid admits "
                            "`allowance` more as billed overage (the spending cap, in calls per quota period: per day on a "
                            "daily plan)")
    allowance: int = Field(0, ge=0, le=store.UINT32)
    failure: Literal["open", "closed"] = Field("closed", description="when Redis cannot be asked: admit (paid) or refuse")
    fee: int = Field(0, ge=0, le=store.UINT32, description="the monthly fee, in cents")
    overage_price: int = Field(0, ge=0, le=store.UINT32, description="the price of 1,000 billed overage calls, in cents")
    topup_price: int = Field(0, ge=0, le=store.UINT32, description="the price of 1,000 prepaid top-up calls, in cents")

    def fields(self):
        return {"daily": self.daily, "burst": self.burst, "refill": self.refill, "period": self.period,
                "overage": store.OVERAGE_FROM_API[self.overage], "extra": self.allowance, "failure": self.failure,
                "fee": self.fee, "overage_price": self.overage_price, "topup_price": self.topup_price}


class LimitsPatch(Strict):
    daily: Optional[int] = Field(None, ge=0, le=store.UINT32)
    burst: Optional[int] = Field(None, ge=0, le=store.UINT32)
    refill: Optional[int] = Field(None, ge=0, le=store.UINT32)
    period: Optional[Literal["day", "month"]] = None
    overage: Optional[Literal["hard", "grace", "postpaid"]] = None
    allowance: Optional[int] = Field(None, ge=0, le=store.UINT32)
    failure: Optional[Literal["open", "closed"]] = None
    fee: Optional[int] = Field(None, ge=0, le=store.UINT32)
    overage_price: Optional[int] = Field(None, ge=0, le=store.UINT32)
    topup_price: Optional[int] = Field(None, ge=0, le=store.UINT32)

    def fields(self):
        out = {}
        for name, value in self.model_dump(exclude_none=True).items():
            if name == "overage":
                out["overage"] = store.OVERAGE_FROM_API[value]
            elif name == "allowance":
                out["extra"] = value
            else:
                out[name] = value
        return out


class MerchantCreate(Strict):
    id: str = Field(pattern=MERCHANT)


class CustomerCreate(Strict):
    id: str = Field(pattern=CUSTOMER)
    plan: str = Field(pattern=PLAN)


class PlanChoice(Strict):
    plan: str = Field(pattern=PLAN)


class Boost(Strict):
    units: int = Field(ge=1, le=1_000_000_000)
    minutes: int = Field(ge=1, le=525_600)


class TopUp(Strict):
    units: int = Field(ge=1, le=1_000_000_000)


class Capacity(Strict):
    rate: int = Field(ge=1, le=store.UINT32, description="calls a second the merchant's backend takes from all its customers")
    burst: Optional[int] = Field(None, ge=1, le=store.UINT32,
                                 description="calls it takes at once; default one second's worth. At least the largest cost a "
                                             "check sends, or such a check is always refused")
    reserve: int = Field(0, ge=0, le=100, description="percent of rate and burst only plans with a fee may use")


class CheckRate(Strict):
    rate: int = Field(ge=1, le=store.UINT32, description="checks a second the platform takes from the merchant")
    burst: Optional[int] = Field(None, ge=1, le=store.UINT32, description="checks at once; default one second's worth")


class PaymentMethod(Strict):
    token: str = Field(max_length=128, description="the payment provider's token for the card (never a card number)")


class Webhook(Strict):
    url: str = Field(max_length=2048)
    secret: Optional[str] = Field(None, min_length=16, max_length=128)


def plan_from_catalog(text):
    """A catalogue entry ("daily 50 burst 5 ...", as tools/rlctl writes it too) as plan-hash fields."""
    words = text.split()
    return dict(zip(words[::2], words[1::2]))


def terms_view(fields):
    """Plan-hash fields as the API shows them; a missing field is the Free default (src/plan/plan.h)."""
    return {"daily": int(fields.get("daily", 200)), "burst": int(fields.get("burst", 2)),
            "refill": int(fields.get("refill", 2)), "period": "month" if fields.get("period") == "month" else "day",
            "overage": store.OVERAGE_TO_API.get(fields.get("overage", "hard"), "hard"),
            "allowance": int(fields.get("extra", 0)), "failure": "open" if fields.get("failure") == "open" else "closed",
            **{name: int(fields.get(name, 0)) for name in store.PRICE_FIELDS}}


def create_app(redis=None, admin_token=None, provider=None):
    admin_token = admin_token if admin_token is not None else os.environ.get("RL_ADMIN_TOKEN", "")
    if len(admin_token) < 16:
        raise RuntimeError("RL_ADMIN_TOKEN must be set, at least 16 characters")
    if redis is None:
        host, port = os.environ.get("RL_REDIS", "127.0.0.1:7001").rsplit(":", 1)
        redis = RedisCluster(host=host, port=int(port), decode_responses=True)
    r = redis
    provider = provider if provider is not None else payments.SandboxProvider(r)
    scripts = {name: r.register_script(getattr(store, name))
               for name in ("CREATE", "APPLY", "UPDATE", "BOOST", "TOPUP", "SUSPEND", "RESUME")}

    app = FastAPI(title="Rate limiter management API",
                  description="Plans, customers, limits, boosts and top-ups of one merchant; merchants for the operator.",
                  version="1.0")

    def bearer(authorization):
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing bearer credential")
        return authorization[len("Bearer "):]

    def operator(authorization: Optional[str] = Header(None)):
        if not hmac.compare_digest(bearer(authorization).encode(), admin_token.encode()):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "not the operator")

    def merchant(authorization: Optional[str] = Header(None)) -> str:
        m = r.hget(store.ADMIN_HASH, store.sha256_hex(bearer(authorization)))
        if m is None:
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "unknown management key")
        return m

    def run(name, m, customer, args=()):
        return scripts[name](keys=[store.plan_key(m, customer)], args=list(args))

    def customer_view(m, customer):
        plan = r.hgetall(store.plan_key(m, customer))
        if not plan:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such customer")
        counter = r.hgetall(store.quota_key(m, customer))
        now = store.now_ms(r)
        monthly = plan.get("period") == "month"
        period = periods.period_name(now, monthly)
        current = counter.get("month" if monthly else "day") == period[1:]  # the counter of this period
        used = int(counter.get("used", 0)) if current else 0
        over = int(counter.get("overage_used", 0)) if current else 0
        boost = int(plan.get("boost", 0)) if now < int(plan.get("boost_until", 0)) else 0
        topup = int(plan.get("topup", 0)) if plan.get("topup_period") == period else 0
        limit = int(plan.get("daily", 200)) + boost + topup
        return {
            "id": customer,
            "plan": plan.get("plan", "free"),
            "suspended": plan.get("plan") == "suspended",
            "terms": terms_view(plan),
            "boost": {"units": int(plan.get("boost", 0)), "until_ms": int(plan.get("boost_until", 0)),
                      "active": boost > 0},
            "topup": {"units": topup, "period": period},
            "usage": {"period": period, "used": used, "overage_used": over, "limit": limit,
                      "remaining": max(0, limit - used), "resets_in_ms": periods.reset_after_ms(now, monthly),
                      "alerts": store.decode_alerts(counter.get("alerts", 0)) if current else []},
        }

    # ---- the merchant portal ----

    @app.get("/portal", include_in_schema=False)
    @app.get("/portal/{name}", include_in_schema=False)
    def portal(name: str = ""):
        if name not in PORTAL_FILES:
            raise HTTPException(status.HTTP_404_NOT_FOUND)
        file, media_type = PORTAL_FILES[name]
        return FileResponse(PORTAL / file, media_type=media_type, headers=PORTAL_HEADERS)

    # ---- the operator ----

    @app.post("/v1/merchants", status_code=201, dependencies=[Depends(operator)])
    def create_merchant(body: MerchantCreate):
        if not r.hsetnx(store.MERCHANTS, body.id, store.now_ms(r)):
            raise HTTPException(status.HTTP_409_CONFLICT, "merchant exists")
        check, manage = store.new_key("rl_"), store.new_key("rlm_")
        r.hset(store.KEYS_HASH, store.sha256_hex(check), body.id)
        r.hset(store.ADMIN_HASH, store.sha256_hex(manage), body.id)
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
        return {"id": body.id, "check_key": check, "management_key": manage,
                "note": "the keys are shown once; only their hashes are stored"}

    @app.get("/v1/merchants", dependencies=[Depends(operator)])
    def list_merchants():
        return sorted(r.hkeys(store.MERCHANTS))

    @app.post("/v1/merchants/{mid}/check-keys", status_code=201, dependencies=[Depends(operator)])
    def add_check_key(mid: str = Path(pattern=MERCHANT)):
        if not r.hexists(store.MERCHANTS, mid):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such merchant")
        check = store.new_key("rl_")
        r.hset(store.KEYS_HASH, store.sha256_hex(check), mid)
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
        return {"id": mid, "check_key": check}

    @app.put("/v1/merchants/{mid}/check-rate", dependencies=[Depends(operator)])
    def put_check_rate(body: CheckRate, mid: str = Path(pattern=MERCHANT)):
        """How many checks a second the platform takes from this merchant, so its flood cannot slow the
        others; beyond it every limiter answers RATE_LIMITED at once."""
        if not r.hexists(store.MERCHANTS, mid):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such merchant")
        r.hset(store.CHECK_RATE_HASH, mid, store.bucket_text(body.rate, body.burst))
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
        return {"id": mid, **store.bucket_view(r.hget(store.CHECK_RATE_HASH, mid))}

    @app.get("/v1/merchants/{mid}/check-rate", dependencies=[Depends(operator)])
    def get_check_rate(mid: str = Path(pattern=MERCHANT)):
        text = r.hget(store.CHECK_RATE_HASH, mid)
        if text is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no check rate")
        return {"id": mid, **store.bucket_view(text)}

    @app.delete("/v1/merchants/{mid}/check-rate", status_code=204, dependencies=[Depends(operator)])
    def delete_check_rate(mid: str = Path(pattern=MERCHANT)):
        r.hdel(store.CHECK_RATE_HASH, mid)
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
        return Response(status_code=204)

    @app.delete("/v1/merchants/{mid}/check-keys", dependencies=[Depends(operator)])
    def revoke_check_keys(mid: str = Path(pattern=MERCHANT)):
        mine = [h for h, owner in r.hgetall(store.KEYS_HASH).items() if owner == mid]
        if mine:
            r.hdel(store.KEYS_HASH, *mine)
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)  # every limiter drops them at once
        return {"id": mid, "revoked": len(mine)}

    # ---- a merchant: plans ----

    @app.put("/v1/plans/{name}")
    def put_plan(body: PlanTerms, name: str = Path(pattern=PLAN), m: str = Depends(merchant)):
        if name == "suspended":
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "the plan name 'suspended' is reserved")
        r.hset(store.catalog_key(m), name, " ".join(f"{k} {v}" for k, v in body.fields().items()))
        return {"name": name, "terms": body}

    @app.get("/v1/plans")
    def list_plans(m: str = Depends(merchant)):
        return {name: terms_view(plan_from_catalog(text)) for name, text in r.hgetall(store.catalog_key(m)).items()}

    @app.delete("/v1/plans/{name}", status_code=204)
    def delete_plan(name: str = Path(pattern=PLAN), m: str = Depends(merchant)):
        if not r.hdel(store.catalog_key(m), name):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such plan")
        return Response(status_code=204)  # customers on it keep their numbers

    def plan_fields(m, name):
        text = r.hget(store.catalog_key(m), name)
        if text is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such plan")
        return {"plan": name, **plan_from_catalog(text)}

    # ---- a merchant: customers ----

    @app.post("/v1/customers", status_code=201)
    def create_customer(body: CustomerCreate, m: str = Depends(merchant)):
        created = run("CREATE", m, body.id, store.flatten(plan_fields(m, body.plan)))
        # The index lives in the merchant's slot, the hash in the customer's: two writes. If a crash
        # falls between them, POST /v1/customers/reindex (or creating it again) puts it back.
        r.zadd(store.customers_key(m), {body.id: 0})
        if not created:
            raise HTTPException(status.HTTP_409_CONFLICT, "customer exists")
        store.announce(r, m, body.id)
        return customer_view(m, body.id)

    @app.get("/v1/customers")
    def list_customers(after: str = Query("", pattern=r"^[A-Za-z0-9._:@-]{0,128}$",
                                          description="the last id of the previous page"),
                       limit: int = Query(50, ge=1, le=200), m: str = Depends(merchant)):
        """The merchant's customers in id order, a page at a time, each as GET /v1/customers/{id} shows it."""
        ids = r.zrange(store.customers_key(m), f"({after}" if after else "-", "+", bylex=True, offset=0, num=limit)
        rows = []
        for cid in ids:
            try:
                rows.append(customer_view(m, cid))
            except HTTPException:  # in the index, but its hash is gone
                continue
        return {"customers": rows, "next": ids[-1] if len(ids) == limit else None}

    @app.get("/v1/usage")
    def usage(m: str = Depends(merchant)):
        """The merchant's calls admitted this calendar month over all its customers, from the counters the
        limiters' quota script keeps (every instance's calls). Polled twice, the change of `admitted`
        over the change of `at_ms` (Redis's clock) is its calls a second. Refused calls are not counted;
        a large plain daily plan, served from leases, counts the units its leases took."""
        # shortcut: one read per customer on every call (the portal polls every 5 s); keep a running
        # total per merchant, sharded to avoid a hot key, once a merchant has more than ~10,000 customers.
        now = store.now_ms(r)
        month = periods.month_of(now)
        ids = r.zrange(store.customers_key(m), 0, -1)
        pipe = r.pipeline()
        for cid in ids:
            pipe.hmget(store.quota_key(m, cid), "bm", "bu", "bo")
        admitted = billed = 0
        for bm, bu, bo in pipe.execute() if ids else []:
            if bm is not None and int(bm) == month:
                admitted += int(bu or 0) + int(bo or 0)
                billed += int(bo or 0)
        return {"month": periods.month_label(month), "at_ms": now, "customers": len(ids), "admitted": admitted,
                "billed_overage": billed}

    @app.get("/v1/customers/{cid}")
    def get_customer(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return customer_view(m, cid)

    def changed(m, cid, ok):
        if not ok:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such customer")
        store.announce(r, m, cid)
        return customer_view(m, cid)

    @app.put("/v1/customers/{cid}/plan")
    def apply_plan(body: PlanChoice, cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        """Moves the customer to a plan of the catalogue: its terms replace custom limits and boosts; a
        top-up already bought is kept."""
        return changed(m, cid, run("APPLY", m, cid, store.flatten(plan_fields(m, body.plan))))

    @app.patch("/v1/customers/{cid}/limits")
    def set_limits(body: LimitsPatch, cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        """Custom limits for this customer alone: any term of its plan, the overage policy included."""
        fields = body.fields()
        if not fields:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "no field to change")
        return changed(m, cid, run("UPDATE", m, cid, store.flatten(fields)))

    @app.post("/v1/customers/{cid}/boost")
    def boost(body: Boost, cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        """Extra units on top of the quota for `minutes` from now (replaces a boost in force)."""
        return changed(m, cid, run("BOOST", m, cid, [body.units, body.minutes]))

    @app.delete("/v1/customers/{cid}/boost")
    def end_boost(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return changed(m, cid, run("UPDATE", m, cid, ["boost", "0", "boost_until", "0"]))

    @app.post("/v1/customers/{cid}/topups", status_code=201)
    def top_up(body: TopUp, response: Response, cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant),
               idempotency_key: Optional[str] = Header(None, max_length=128)):
        """Prepaid units for the current period, added to any bought earlier in it. Send an
        Idempotency-Key: a retry with the same key returns the first answer and buys nothing more."""
        idem = store.idempotency_key(m, idempotency_key) if idempotency_key else None
        if idem and not r.set(idem, "pending", nx=True, ex=86_400):
            first = r.get(idem)
            if first == "pending":
                raise HTTPException(status.HTTP_409_CONFLICT, "the same top-up is in progress")
            response.headers["Idempotent-Replayed"] = "true"
            return json.loads(first)
        now = store.now_ms(r)
        result = run("TOPUP", m, cid, [body.units, periods.period_name(now, False), periods.period_name(now, True)])
        if result == -1:
            if idem:
                r.delete(idem)
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such customer")
        total, period, price = int(result[0]), result[1], int(result[2])
        # The purchase, at the plan's price now, for the invoice (P3). shortcut: written after the units,
        # so a crash between the two gives units away unbilled, never bills units not given; make both
        # one record if needed.
        r.xadd(store.purchases_key(m), {"customer": cid, "units": body.units, "period": period, "at_ms": now,
                                        "price": price, "idempotency_key": idempotency_key or ""})
        store.announce(r, m, cid)
        answer = {"customer": cid, "units": body.units, "period": period, "period_total": total,
                  "price_per_1000": price, "amount": billing.line_amount(body.units, price)}
        if idem:
            r.set(idem, json.dumps(answer), ex=86_400)
        return answer

    @app.post("/v1/customers/{cid}/suspend")
    def suspend(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return changed(m, cid, run("SUSPEND", m, cid))

    @app.post("/v1/customers/{cid}/resume")
    def resume(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return changed(m, cid, run("RESUME", m, cid))

    # ---- a merchant: its backend's capacity (P3) ----

    @app.put("/v1/capacity")
    def put_capacity(body: Capacity, m: str = Depends(merchant)):
        """How many calls a second the merchant's backend takes from all its customers together. Beyond it
        calls are refused (RATE_LIMITED) before their quota, so nothing refused is counted or billed;
        `reserve` percent of it is kept for plans with a fee, so a flood of Free calls cannot take it."""
        burst = body.burst or body.rate
        if body.reserve < 100 and burst * (100 - body.reserve) // 100 < 1:  # as the limiter splits it (merchants.h)
            least = -(-100 // (100 - body.reserve))
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY,
                                f"a burst of {burst} leaves no whole call outside a {body.reserve}% reserve: at least {least}")
        r.hset(store.CAPACITY_HASH, m, store.bucket_text(body.rate, body.burst, body.reserve))
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)  # every limiter reloads it at once
        return store.bucket_view(r.hget(store.CAPACITY_HASH, m))

    @app.get("/v1/capacity")
    def get_capacity(m: str = Depends(merchant)):
        text = r.hget(store.CAPACITY_HASH, m)
        if text is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no capacity set")
        return store.bucket_view(text)

    @app.delete("/v1/capacity", status_code=204)
    def delete_capacity(m: str = Depends(merchant)):
        r.hdel(store.CAPACITY_HASH, m)
        r.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
        return Response(status_code=204)

    # ---- a merchant: payment methods and invoices (P3) ----

    @app.put("/v1/customers/{cid}/payment-method")
    def put_payment_method(body: PaymentMethod, cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        """The card the customer's invoices are charged to when a month closes, as the payment provider's
        token. A card number is refused: only the provider ever holds one."""
        if not r.exists(store.plan_key(m, cid)):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such customer")
        try:
            payments.check_token(body.token)
            payer = r.hget(store.payers_key(m), cid) or provider.create_customer(store.customer_id(m, cid))
            card = provider.attach(payer, body.token)
        except payments.PaymentError as e:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, {"code": e.code, "message": str(e)}) from e
        r.hset(store.payers_key(m), cid, payer)
        return {"customer": cid, "provider": provider.name, "card": card}

    @app.get("/v1/customers/{cid}/payment-method")
    def get_payment_method(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        payer = r.hget(store.payers_key(m), cid)
        if payer is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no payment method")
        return {"customer": cid, "provider": provider.name, "card": provider.card(payer)}

    @app.delete("/v1/customers/{cid}/payment-method", status_code=204)
    def delete_payment_method(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        r.hdel(store.payers_key(m), cid)  # its invoices stay open for the merchant to collect
        return Response(status_code=204)

    @app.get("/v1/customers/{cid}/invoice-preview")
    def invoice_preview(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        """This month's invoice so far: the fee, billed overage and top-ups, as it would close now."""
        if not r.exists(store.plan_key(m, cid)):
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no such customer")
        return billing.preview(r, m, cid)

    def month_param(month):
        try:
            return periods.month_id(month)
        except ValueError as e:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, str(e)) from e

    @app.get("/v1/invoices")
    def list_invoices(month: str = Query(description="YYYY-MM"), m: str = Depends(merchant)):
        return billing.invoices(r, m, month_param(month))

    @app.get("/v1/invoices.csv")
    def invoices_csv(month: str = Query(description="YYYY-MM"), m: str = Depends(merchant)):
        return Response(billing.to_csv(billing.invoices(r, m, month_param(month))), media_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="invoices-{month}.csv"'})

    @app.post("/v1/customers/reindex", dependencies=[Depends(operator)])
    def reindex_customers():
        """Rebuilds every merchant's customer index from the plan hashes (a scan of the cluster): for
        customers made before the index, by tools/rlctl, or across a crash between the two writes."""
        found = billing.customers(r)
        for m in r.hkeys(store.MERCHANTS):
            if found.get(m):
                r.zadd(store.customers_key(m), dict.fromkeys(found[m], 0))
        return {m: len(c) for m, c in found.items()}

    @app.post("/v1/billing/close", dependencies=[Depends(operator)])
    def close_month(month: str = Query(description="YYYY-MM, a month that has ended"), charge: bool = True,
                    merchant: Optional[list[str]] = Query(None, description="only these merchants; default all")):
        """Invoices the merchants' customers for the month and charges those with a payment method; a
        declined charge suspends the customer. Safe to repeat (rlcontrol.billing runs the same from cron)."""
        try:
            return billing.close(r, month_param(month), provider if charge else None, merchant)
        except ValueError as e:
            raise HTTPException(status.HTTP_409_CONFLICT, str(e)) from e

    # ---- a merchant: its webhook ----

    @app.put("/v1/webhook")
    def put_webhook(body: Webhook, m: str = Depends(merchant)):
        """Where alerts are sent (quota.80, quota.100, overage.started, quota.denied), signed with the
        secret (X-RL-Signature). Without a secret one is made; it is shown here only."""
        try:
            webhooks.check_url(body.url)
        except webhooks.UnsafeUrl as e:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, str(e)) from e
        secret = body.secret or store.new_key("whsec_")
        r.hset(store.webhook_key(m), mapping={"url": body.url, "secret": secret})
        return {"url": body.url, "secret": secret}

    @app.get("/v1/webhook")
    def get_webhook(m: str = Depends(merchant)):
        url = r.hget(store.webhook_key(m), "url")
        if url is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "no webhook")
        return {"url": url}

    @app.delete("/v1/webhook", status_code=204)
    def delete_webhook(m: str = Depends(merchant)):
        r.delete(store.webhook_key(m))
        return Response(status_code=204)

    return app


def __getattr__(name):  # `uvicorn rlcontrol.api:app` builds the app from the environment on first use
    if name == "app":
        return create_app()
    raise AttributeError(name)
