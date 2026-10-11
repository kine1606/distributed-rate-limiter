-- Daily quota: counts usage per UTC day, atomically.
--
-- KEYS[1]  hash-tagged quota key, e.g. {cust:42}:quota
-- ARGV[1]  daily limit (comes from the customer's CURRENT plan, so an upgrade
--          takes effect on the very next request and used is preserved)
-- ARGV[2]  cost of this request
-- Returns: { allowed (0|1), used, remaining, reset_after_ms }
--
-- State stores `used` (not remaining) so a plan change never rewrites history.
-- The day number comes from Redis TIME, so all instances agree on rollover.

local limit = tonumber(ARGV[1])
local cost  = tonumber(ARGV[2])

local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local day_ms = 86400000
local day = math.floor(now_ms / day_ms)
local reset_after = (day + 1) * day_ms - now_ms

local st = redis.call('HMGET', KEYS[1], 'day', 'used')
local used = tonumber(st[2]) or 0
if tonumber(st[1]) ~= day then used = 0 end   -- new UTC day (or first use)

local allowed = 0
if used + cost <= limit then
  used = used + cost
  allowed = 1
end

redis.call('HSET', KEYS[1], 'day', day, 'used', used)
-- Keep the key until the day ends plus a margin, then Redis cleans it up.
redis.call('PEXPIRE', KEYS[1], reset_after + 3600000)

local remaining = limit - used
if remaining < 0 then remaining = 0 end   -- after a downgrade used can exceed limit
return { allowed, used, remaining, reset_after }
