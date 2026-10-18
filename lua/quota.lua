-- Daily quota: counts usage per UTC day, atomically, with the plan's overage policy and request-ID
-- deduplication (lua/README.md, Week 4).
--
-- KEYS[1]  hash-tagged quota key, e.g. {cust:42}:daily_quota:q
-- KEYS[2]  optional, same hash tag: the request's stored answer, e.g. {cust:42}:daily_quota:req:<id>
-- ARGV[1]  daily limit (comes from the customer's CURRENT plan, so an upgrade
--          takes effect on the very next request and used is preserved)
-- ARGV[2]  cost of this request
-- ARGV[3]  overage policy at the limit: "hard" (default) | "grace" | "cap"
-- ARGV[4]  grace amount (grace) or overage cap (cap); default 0
-- Returns: { allowed (0|1), used, remaining, reset_after_ms, overage (0|1) }
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

local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local day_ms = 86400000
local day = math.floor(now_ms / day_ms)
local reset_after = (day + 1) * day_ms - now_ms

-- 1. A retry of a request already decided gets the stored answer and is not counted again.
if KEYS[2] then
  local stored = redis.call('GET', KEYS[2])
  if stored then
    local a, u, r, o = string.match(stored, '^(%d+),(%d+),(%d+),(%d+)$')
    return { tonumber(a), tonumber(u), tonumber(r), reset_after, tonumber(o) }
  end
end

-- 2. Load today's counters; a new UTC day (or first use) starts both at zero.
local st = redis.call('HMGET', KEYS[1], 'day', 'used', 'overage_used')
local used = tonumber(st[2]) or 0
local over_used = tonumber(st[3]) or 0
if tonumber(st[1]) ~= day then
  used = 0
  over_used = 0
end

-- 3. Decide. Within the limit the policy does not matter.
local allowed, overage = 0, 0
if used + cost <= limit then
  used = used + cost
  allowed = 1
elseif policy == 'grace' then
  if used + cost <= limit + extra then
    used = used + cost
    allowed = 1
  end
elseif policy == 'cap' then
  local in_plan = math.max(0, math.min(cost, limit - used))  -- a request can straddle the limit
  if over_used + cost - in_plan <= extra then
    used = used + in_plan
    over_used = over_used + cost - in_plan
    allowed = 1
    overage = 1
  end
end

redis.call('HSET', KEYS[1], 'day', day, 'used', used, 'overage_used', over_used)
-- Keep the key until the day ends plus a margin, then Redis cleans it up.
redis.call('PEXPIRE', KEYS[1], reset_after + 3600000)

local remaining = limit - used
if remaining < 0 then remaining = 0 end   -- after a downgrade, or in grace, used can exceed limit

-- 4. Store the answer, denials too, so a retry within 5 minutes gets exactly this result.
if KEYS[2] then
  redis.call('SET', KEYS[2], allowed .. ',' .. used .. ',' .. remaining .. ',' .. overage, 'PX', 300000)
end
return { allowed, used, remaining, reset_after, overage }
