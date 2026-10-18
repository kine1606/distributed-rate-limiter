-- Gives tokens back to a token bucket: a burst token whose request the daily quota then denied
-- (CompositeCore, Week 4), or leased tokens never used (graceful shutdown, Week 5). Best effort.
--
-- KEYS[1]  the bucket key token_bucket.lua uses, e.g. {cust:42}:burst:tb
-- ARGV[1]  capacity (burst), in whole tokens
-- ARGV[2]  tokens to return
-- Returns: { 1, tokens now (whole), 0 }  (token_bucket.lua's reply shape)
--
-- A missing bucket is full (token_bucket.lua treats it so), so there is nothing to return to. The
-- stored tokens are as of last_refill_ms; adding to them and clamping at the capacity gives the same
-- bucket token_bucket.lua would see, so returned tokens can never exceed the capacity.

local cap_m = tonumber(ARGV[1]) * 1000
local tokens = tonumber(redis.call('HGET', KEYS[1], 'tokens'))
if tokens == nil then
  return { 1, math.floor(cap_m / 1000), 0 }
end
tokens = math.min(cap_m, tokens + tonumber(ARGV[2]) * 1000)
redis.call('HSET', KEYS[1], 'tokens', tokens)  -- HSET keeps the key's TTL
return { 1, math.floor(tokens / 1000), 0 }
