"""The management API (Phase 2, P2; docs/PRODUCT_PLAN.md section 8). REST over HTTP, documented by its
OpenAPI page (/docs).

Two kinds of caller, each with its own credential (Authorization: Bearer ...):
- the platform operator, with RL_ADMIN_TOKEN: creates merchants and rotates or revokes their keys;
- a merchant, with its management key: defines its plans, creates customers, sets their limits,
  grants boosts and top-ups, suspends, reads usage, registers its webhook. The merchant always comes
  from the key, never from the URL, so no request can reach another merchant's customers.

A merchant's check key (for its gateway's calls to the limiter) and its management key are separate:
a gateway that leaks its key cannot change plans.

    RL_ADMIN_TOKEN=... RL_REDIS=127.0.0.1:7001 control/.venv/bin/uvicorn rlcontrol.api:app --app-dir control
"""
import hmac
import json
import os
from typing import Literal, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Path, Response, status
from pydantic import BaseModel, ConfigDict, Field
from redis.cluster import RedisCluster

from . import periods, store, webhooks

MERCHANT = r"^[A-Za-z0-9_-]{1,64}$"
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
                            "`allowance` more as billed overage (the spending cap, in calls)")
    allowance: int = Field(0, ge=0, le=store.UINT32)
    failure: Literal["open", "closed"] = Field("closed", description="when Redis cannot be asked: admit (paid) or refuse")

    def fields(self):
        return {"daily": self.daily, "burst": self.burst, "refill": self.refill, "period": self.period,
                "overage": store.OVERAGE_FROM_API[self.overage], "extra": self.allowance, "failure": self.failure}


class LimitsPatch(Strict):
    daily: Optional[int] = Field(None, ge=0, le=store.UINT32)
    burst: Optional[int] = Field(None, ge=0, le=store.UINT32)
    refill: Optional[int] = Field(None, ge=0, le=store.UINT32)
    period: Optional[Literal["day", "month"]] = None
    overage: Optional[Literal["hard", "grace", "postpaid"]] = None
    allowance: Optional[int] = Field(None, ge=0, le=store.UINT32)
    failure: Optional[Literal["open", "closed"]] = None

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
            "allowance": int(fields.get("extra", 0)), "failure": "open" if fields.get("failure") == "open" else "closed"}


def create_app(redis=None, admin_token=None):
    admin_token = admin_token if admin_token is not None else os.environ.get("RL_ADMIN_TOKEN", "")
    if len(admin_token) < 16:
        raise RuntimeError("RL_ADMIN_TOKEN must be set, at least 16 characters")
    if redis is None:
        host, port = os.environ.get("RL_REDIS", "127.0.0.1:7001").rsplit(":", 1)
        redis = RedisCluster(host=host, port=int(port), decode_responses=True)
    r = redis
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
        if not run("CREATE", m, body.id, store.flatten(plan_fields(m, body.plan))):
            raise HTTPException(status.HTTP_409_CONFLICT, "customer exists")
        store.announce(r, m, body.id)
        return customer_view(m, body.id)

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
        total, period = int(result[0]), result[1]
        # The purchase, for the invoice (P3). shortcut: written after the units, so a crash between the
        # two gives units away unbilled, never bills units not given; make both one record if needed.
        r.xadd(store.purchases_key(m), {"customer": cid, "units": body.units, "period": period, "at_ms": now,
                                        "idempotency_key": idempotency_key or ""})
        store.announce(r, m, cid)
        answer = {"customer": cid, "units": body.units, "period": period, "period_total": total}
        if idem:
            r.set(idem, json.dumps(answer), ex=86_400)
        return answer

    @app.post("/v1/customers/{cid}/suspend")
    def suspend(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return changed(m, cid, run("SUSPEND", m, cid))

    @app.post("/v1/customers/{cid}/resume")
    def resume(cid: str = Path(pattern=CUSTOMER), m: str = Depends(merchant)):
        return changed(m, cid, run("RESUME", m, cid))

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
