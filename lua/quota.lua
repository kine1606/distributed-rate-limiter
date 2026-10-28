-- Quota: counts usage per UTC day or UTC calendar month, atomically, with the plan's overage policy
-- and request-ID deduplication (lua/README.md, Week 4; months and boosts: docs/PRODUCT_PLAN.md, P1).
--
-- KEYS[1]  hash-tagged quota key, e.g. {cust:42}:daily_quota:q
-- KEYS[2]  optional, same hash tag: the request's stored answer, e.g. {cust:42}:daily_quota:req:<id>
-- ARGV[1]  daily limit (comes from the customer's CURRENT plan, so an upgrade
--          takes effect on the very next request and used is preserved)
-- ARGV[2]  cost of this request
-- ARGV[3]  overage policy at the limit: "hard" (default) | "grace" | "cap"
-- ARGV[4]  grace amount (grace) or overage cap (cap); default 0
-- ARGV[5]  period: "day" (default) | "month" (UTC calendar month; the limit is per month)
-- ARGV[6]  boost: extra units on top of the limit while it lasts; default 0
-- ARGV[7]  boost end, Unix ms by the Redis clock (the same clock as the reset); default 0 = none
-- ARGV[8]  prepaid top-up: extra units for one period (P2); default 0
-- ARGV[9]  the period the top-up was bought for, "d<day>" or "m<year * 12 + month - 1>"
-- Returns: { allowed (0|1), used, remaining, reset_after_ms, overage (0|1), replayed (0|1), limit }
--          replayed = 1: the stored answer of a retried request ID, not counted again
--          limit = the limit in force, the boost and a top-up for this period included
-- Alerts (P2): the first time in a period that used reaches 80% of the limit, reaches the limit, goes
-- into billed overage, or is denied, the script publishes one JSON message on the channel rl-alerts
-- (tools: control/notifier.py). Bits in the counter's `alerts` field make each one once a period;
-- a replayed answer sends none.
-- Billing (P3): the counter also holds the UTC calendar month's totals (bm, bu, bo, below) and files
-- a finished month that had billed overage as closed:m<month> (read by control/rlcontrol/billing.py).
--
-- State stores `used` (not remaining) so a plan change never rewrites history.
-- The day number comes from Redis TIME, so all instances agree on rollover.
-- grace admits while used + cost <= limit + extra and counts it in `used` (free, so overage = 0).
-- cap admits while overage_used + (the part beyond the limit) <= extra and counts that part in
-- `overage_used`, billed separately (overage = 1).

local limit  = tonumber(ARGV[1])
local cost   = tonumber(ARGV[2])
local policy = ARGV[3] or 'hard'
local extra  = tonumber(ARGV[4]) or 0
local monthly = ARGV[5] == 'month'
local boost = tonumber(ARGV[6]) or 0
local boost_until = tonumber(ARGV[7]) or 0
local topup = tonumber(ARGV[8]) or 0
local topup_period = ARGV[9] or ''

local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local day_ms = 86400000
local day = math.floor(now_ms / day_ms)
if now_ms < boost_until then limit = limit + boost end

-- The UTC calendar month, numbered year * 12 + month - 1, and the time until it ends: the period of a
-- monthly quota, and the billing month of every quota. Days since 1970-01-01 to the month, and back
-- to the first day of the next one (H. Hinnant's civil-calendar algorithms, valid for every date
-- Redis can return).
local z = day + 719468
local era = math.floor(z / 146097)
local doe = z - era * 146097
local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
local mp = math.floor((5 * doy + 2) / 153)
local m = mp < 10 and mp + 3 or mp - 9
local y = yoe + era * 400 + (m <= 2 and 1 or 0)
local month = y * 12 + m - 1
local ny, nm = y, m + 1  -- the next month
if nm > 12 then ny, nm = y + 1, 1 end
if nm <= 2 then ny = ny - 1 end
local nera = math.floor(ny / 400)
local nyoe = ny - nera * 400
local nmp = nm > 2 and nm - 3 or nm + 9
local ndoe = nyoe * 365 + math.floor(nyoe / 4) - math.floor(nyoe / 100) + math.floor((153 * nmp + 2) / 5)
local month_left = (nera * 146097 + ndoe - 719468) * day_ms - now_ms

-- The period this call counts in, the counter field that holds it, and the time until it ends.
local field, period, reset_after = 'day', day, (day + 1) * day_ms - now_ms
if monthly then field, period, reset_after = 'month', month, month_left end
local period_name = (monthly and 'm' or 'd') .. period
if topup > 0 and topup_period == period_name then limit = limit + topup end

-- 1. A retry of a request already decided gets the stored answer and is not counted again.
if KEYS[2] then
  local stored = redis.call('GET', KEYS[2])
  if stored then
    local a, u, r, o = string.match(stored, '^(%d+),(%d+),(%d+),(%d+)$')
    return { tonumber(a), tonumber(u), tonumber(r), reset_after, tonumber(o), 1, limit }
  end
end

-- 2. Load this period's counters; a new period (or first use) starts both at zero. A customer moved
-- between daily and monthly plans starts the new kind of period at zero.
local st = redis.call('HMGET', KEYS[1], field, 'used', 'overage_used', 'alerts', 'bm', 'bu', 'bo', 'keep_until')
local used = tonumber(st[2]) or 0
local over_used = tonumber(st[3]) or 0
local alerts = tonumber(st[4]) or 0
if tonumber(st[1]) ~= period then
  used = 0
  over_used = 0
  alerts = 0
end
-- The billing month's totals (P3), whatever the quota period: `bu` units admitted within the plan
-- (free grace included) and `bo` billed overage units. A new month files the last one's totals as
-- closed:m<month> = "bu,bo" for the billing job, if it had billed overage: nothing else is billed by
-- usage. The billing job removes the field once the month is invoiced.
local bm, bu, bo, keep = tonumber(st[5]), tonumber(st[6]) or 0, tonumber(st[7]) or 0, tonumber(st[8]) or 0
local retain_ms = 35 * day_ms  -- how long after its month a billable total waits for the billing job
if bm ~= month then
  if bm and bo > 0 then
    redis.call('HSET', KEYS[1], 'closed:m' .. bm, bu .. ',' .. bo)
    keep = now_ms + retain_ms
  end
  bu, bo = 0, 0
end

-- 3. Decide. Within the limit the policy does not matter.
local allowed, overage = 0, 0
if used + cost <= limit then
  used = used + cost
  bu = bu + cost
  allowed = 1
elseif policy == 'grace' then
  if used + cost <= limit + extra then
    used = used + cost
    bu = bu + cost
    allowed = 1
  end
elseif policy == 'cap' then
  local in_plan = math.max(0, math.min(cost, limit - used))  -- a request can straddle the limit
  if over_used + cost - in_plan <= extra then
    used = used + in_plan
    over_used = over_used + cost - in_plan
    bu = bu + in_plan
    bo = bo + cost - in_plan
    allowed = 1
    overage = 1
  end
end

if monthly then
  redis.call('HDEL', KEYS[1], 'day')
else
  redis.call('HDEL', KEYS[1], 'month')
end
-- 4. Alerts: each threshold once a period, against the limit in force.
local sent = {}
local function alert(b, event)
  if bit.band(alerts, b) == 0 then
    alerts = bit.bor(alerts, b)
    sent[#sent + 1] = event
  end
end
if limit > 0 and used * 5 >= limit * 4 then alert(1, 'quota.80') end
if limit > 0 and used >= limit then alert(2, 'quota.100') end
if over_used > 0 then alert(4, 'overage.started') end
if allowed == 0 then alert(8, 'quota.denied') end

redis.call('HSET', KEYS[1], field, period, 'used', used, 'overage_used', over_used, 'alerts', alerts,
           'bm', month, 'bu', bu, 'bo', bo, 'keep_until', keep)
-- Keep the key until the period ends plus a margin, then Redis cleans it up. Billed overage keeps it
-- until `retain_ms` after its month, so the billing job finds it even if the customer never calls
-- again; a key without any (a Free customer, a flood of unknown ids) still goes after its period.
local ttl = reset_after + 3600000
if bo > 0 then ttl = math.max(ttl, month_left + retain_ms) end
if keep > now_ms then ttl = math.max(ttl, keep - now_ms) end
redis.call('PEXPIRE', KEYS[1], ttl)

local remaining = limit - used
if remaining < 0 then remaining = 0 end   -- after a downgrade, or in grace, used can exceed limit

-- 5. Store the answer, denials too, so a retry within 5 minutes gets exactly this result.
if KEYS[2] then
  redis.call('SET', KEYS[2], allowed .. ',' .. used .. ',' .. remaining .. ',' .. overage, 'PX', 300000)
end
if #sent > 0 then
  local customer = string.match(KEYS[1], '^{(.*)}:') or KEYS[1]  -- the hash tag: the customer
  for _, event in ipairs(sent) do
    redis.call('PUBLISH', 'rl-alerts', cjson.encode({customer = customer, event = event, used = used,
      overage_used = over_used, limit = limit, period = period_name, at_ms = now_ms}))
  end
end
return { allowed, used, remaining, reset_after, overage, 0, limit }
