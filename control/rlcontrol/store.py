"""Everything the control plane keeps in Redis, in one place: key names, the plan-hash fields the C++
server reads (src/plan/plan.h), and the atomic writes. Every change to a customer is written by one
script on the customer's own hash, then announced on plan-changes, as tools/rlctl does, so every
limiter drops its cached plan and local denial at once."""
import hashlib
import re
import secrets
import time

KEYS_HASH = "{rl:merchants}:keys"    # sha256(check key) -> merchant (read by src/server/merchants.h)
ADMIN_HASH = "{rl:merchants}:admin"  # sha256(management key) -> merchant
MERCHANTS = "{rl:merchants}:list"    # merchant -> created at, Unix ms
CAPACITY_HASH = "{rl:merchants}:capacity"      # merchant -> "rate R burst B reserve P" (src/server/merchants.h)
CHECK_RATE_HASH = "{rl:merchants}:check_rate"  # merchant -> "rate R burst B", set by the operator
CHANNEL = "plan-changes"
MERCHANTS_CHANGED = "!merchants"
ALERTS = "rl-alerts"                 # published by lua/quota.lua
QUOTA_RULE = "daily_quota"           # config/rules.yaml: the customer rule's quota

MERCHANT_RE = re.compile(r"[A-Za-z0-9_-]{1,64}")    # as ValidMerchant in src/server/merchants.h
CUSTOMER_RE = re.compile(r"[A-Za-z0-9._:@-]{1,128}")
PLAN_RE = re.compile(r"[a-z0-9_-]{1,32}")
UINT32 = 4_294_967_295

PLAN_FIELDS = ("daily", "burst", "refill", "period", "overage", "extra", "failure")
# Prices (P3), read only by the billing job: the plan's monthly fee, and 1,000 billed overage units
# or 1,000 top-up units, each in the currency's minor unit (cents). The limiter ignores them.
PRICE_FIELDS = ("fee", "overage_price", "topup_price")
OVERAGE_TO_API = {"hard": "hard", "grace": "grace", "cap": "postpaid"}
OVERAGE_FROM_API = {v: k for k, v in OVERAGE_TO_API.items()}
ALERT_BITS = ((1, "quota.80"), (2, "quota.100"), (4, "overage.started"), (8, "quota.denied"))


def catalog_key(merchant):
    return f"{{rl:m:{merchant}}}:catalog"


def webhook_key(merchant):
    return f"{{rl:m:{merchant}}}:webhook"


def purchases_key(merchant):  # a stream of top-ups, for the invoice (P3)
    return f"{{rl:m:{merchant}}}:purchases"


def customers_key(merchant):  # a sorted set of the merchant's customer ids (score 0: ordered by id)
    return f"{{rl:m:{merchant}}}:customers"


def invoices_key(merchant):  # invoice id -> invoice (JSON), written by rlcontrol.billing
    return f"{{rl:m:{merchant}}}:invoices"


def payers_key(merchant):  # customer -> the payment provider's customer id; never a card
    return f"{{rl:m:{merchant}}}:payers"


def settings_key(merchant):  # the merchant's own settings: "periods" (calendar | subscription, P5)
    return f"{{rl:m:{merchant}}}:settings"


def audit_key(merchant):  # a stream of every change to the merchant, through the API or tools/rlctl (P5)
    return f"{{rl:m:{merchant}}}:audit"


AUDIT_MAXLEN = 10_000  # the latest changes kept, about; XADD trims the rest


def payment_event_key(event_id):  # a provider's webhook event already handled (P5)
    return f"{{rl:payments}}:event:{event_id}"


def authfail_key(address):  # failed authentications from one client address, this minute (P5)
    return f"{{rl:authfail:{address}}}"


def idempotency_key(merchant, key):
    return f"{{rl:m:{merchant}}}:idem:{hashlib.sha256(key.encode()).hexdigest()}"


def customer_id(merchant, customer):  # what the limiter calls it (src/server/merchants.h, TenantKey)
    return f"{merchant}|{customer}"


def plan_key(merchant, customer):
    return f"{{{customer_id(merchant, customer)}}}:plan"


def quota_key(merchant, customer):  # src/redis/keys.h, RedisKey
    return counter_key(customer_id(merchant, customer))


def counter_key(limiter_id):  # the quota counter of a customer as the limiter names it ("acme|u42")
    return f"{{{limiter_id}}}:{QUOTA_RULE}:q"


def sha256_hex(s):
    return hashlib.sha256(s.encode()).hexdigest()


def new_key(prefix):
    return prefix + secrets.token_hex(24)


def now_ms(redis):
    seconds, micros = redis.time()
    return int(seconds) * 1000 + int(micros) // 1000


# ---- atomic writes, each on one customer's plan hash ----

# Creates the hash unless it exists. ARGV: field value ...
CREATE = """
if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end
redis.call('HSET', KEYS[1], unpack(ARGV))
return 1
"""

# Replaces the hash with a plan's fields, keeping a top-up already paid for and the subscription's
# anchor day (P5), which belong to the customer, not the plan. ARGV: field value ...
APPLY = """
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
local keep = redis.call('HMGET', KEYS[1], 'topup', 'topup_period', 'anchor')
redis.call('DEL', KEYS[1])
redis.call('HSET', KEYS[1], unpack(ARGV))
if keep[1] and keep[2] then redis.call('HSET', KEYS[1], 'topup', keep[1], 'topup_period', keep[2]) end
if keep[3] then redis.call('HSET', KEYS[1], 'anchor', keep[3]) end
return 1
"""

# Sets some fields of an existing hash. ARGV: field value ...
UPDATE = """
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
redis.call('HSET', KEYS[1], unpack(ARGV))
return 1
"""

# Boost of ARGV[1] units for ARGV[2] minutes from now by the Redis clock. Returns its end, or 0.
BOOST = """
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
local t = redis.call('TIME')
local until_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000) + tonumber(ARGV[2]) * 60000
redis.call('HSET', KEYS[1], 'boost', ARGV[1], 'boost_until', until_ms)
return until_ms
"""

# Adds ARGV[1] prepaid units for the current period: ARGV[2] if the plan counts per day, ARGV[3] if
# per month. A top-up for an earlier period is replaced. Returns the period's total, the period and the
# plan's price of 1,000 top-up units (for the invoice), or -1.
TOPUP = """
if redis.call('EXISTS', KEYS[1]) == 0 then return -1 end
local period = redis.call('HGET', KEYS[1], 'period') == 'month' and ARGV[3] or ARGV[2]
local cur = redis.call('HMGET', KEYS[1], 'topup', 'topup_period')
local units = tonumber(ARGV[1])
if cur[2] == period then units = units + (tonumber(cur[1]) or 0) end
redis.call('HSET', KEYS[1], 'topup', units, 'topup_period', period)
return {units, period, redis.call('HGET', KEYS[1], 'topup_price') or '0'}
"""

# One failed authentication more from an address; the count lives ARGV[1] seconds from the first.
AUTH_FAILED = """
local n = redis.call('INCR', KEYS[1])
if n == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end
return n
"""

# Replaces management key hash ARGV[1] by ARGV[2], for merchant ARGV[3], in one step.
ROTATE = """
if redis.call('HGET', KEYS[1], ARGV[1]) ~= ARGV[3] then return 0 end
redis.call('HDEL', KEYS[1], ARGV[1])
redis.call('HSET', KEYS[1], ARGV[2], ARGV[3])
return 1
"""

# Suspends, remembering the plan, or resumes it; the numbers are kept either way.
SUSPEND = """
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
local plan = redis.call('HGET', KEYS[1], 'plan') or 'free'
if plan ~= 'suspended' then redis.call('HSET', KEYS[1], 'resume_plan', plan, 'plan', 'suspended') end
return 1
"""
RESUME = """
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
if redis.call('HGET', KEYS[1], 'plan') == 'suspended' then
  redis.call('HSET', KEYS[1], 'plan', redis.call('HGET', KEYS[1], 'resume_plan') or 'free')
  redis.call('HDEL', KEYS[1], 'resume_plan')
end
return 1
"""


def flatten(fields):
    out = []
    for k, v in fields.items():
        out += [k, str(v)]
    return out


def announce(redis, merchant, customer):
    redis.publish(CHANNEL, customer_id(merchant, customer))


def bucket_text(rate, burst=None, reserve=None):
    """A merchant bucket's terms as the limiter reads them (ParseBucketTerms in src/server/merchants.h)."""
    words = ["rate", rate, "burst", burst or rate] + (["reserve", reserve] if reserve is not None else [])
    return " ".join(str(w) for w in words)


def bucket_view(text):
    words = text.split()
    terms = {k: int(v) for k, v in zip(words[::2], words[1::2]) if v.isdigit()}
    return {"rate": terms.get("rate", 0), "burst": terms.get("burst", terms.get("rate", 0)),
            **({"reserve": terms["reserve"]} if "reserve" in terms else {})}


def decode_alerts(bits):
    return [name for bit, name in ALERT_BITS if int(bits) & bit]
