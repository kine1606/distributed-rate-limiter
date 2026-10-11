-- Token bucket: atomic check-and-update, one Redis round trip.
--
-- KEYS[1]  hash-tagged bucket key, e.g. {cust:42}:tb  (single slot => Redis Cluster safe)
-- ARGV[1]  capacity (burst), in whole tokens
-- ARGV[2]  refill_per_sec, whole tokens per second
-- ARGV[3]  requested tokens (1 for a direct check, a chunk for a lease refill)
-- ARGV[4]  ttl_ms for PEXPIRE (idle buckets disappear on their own)
-- Returns: { allowed (0|1), remaining, retry_after_ms }
--
-- Why TIME inside the script: every limiter instance sees the same clock (Redis's),
-- so skew between instances cannot corrupt state. Redis 7 replicates script effects,
-- so calling TIME before a write is allowed.
--
-- Fixed-point: tokens are stored as milli-tokens (integers) so repeated refills
-- never accumulate floating point drift.

local capacity  = tonumber(ARGV[1])
local rate      = tonumber(ARGV[2])
local requested = tonumber(ARGV[3])
local ttl_ms    = tonumber(ARGV[4])

-- 1. Current Redis time in milliseconds.
local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)

-- 2. Load state. A missing key means a full bucket (first use or expired while idle).
local st = redis.call('HMGET', KEYS[1], 'tokens', 'last_refill_ms')
local cap_m  = capacity * 1000
local tokens = tonumber(st[1])
local last   = tonumber(st[2])
if tokens == nil or last == nil then
  tokens = cap_m
  last = now_ms
end

-- 3. Refill. rate tokens/s == rate milli-tokens per ms. A clock that jumped back
--    (failover to a replica with an older clock) yields elapsed = 0, never negative.
local elapsed = now_ms - last
if elapsed < 0 then elapsed = 0 end
tokens = math.min(cap_m, tokens + elapsed * rate)

-- 4. Decide. A request larger than the capacity can never succeed, so it is denied
--    with retry_after = 0 (waiting would not help; the caller sees remaining and limit).
local need = requested * 1000
local allowed = 0
local retry_after = 0
if need <= tokens then
  tokens = tokens - need
  allowed = 1
elseif need <= cap_m and rate > 0 then
  retry_after = math.ceil((need - tokens) / rate)
end

-- 5. Persist and refresh TTL.
redis.call('HSET', KEYS[1], 'tokens', tokens, 'last_refill_ms', now_ms)
redis.call('PEXPIRE', KEYS[1], ttl_ms)

return { allowed, math.floor(tokens / 1000), retry_after }
