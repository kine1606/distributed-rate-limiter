-- Fixed window counter: atomic check-and-update, one Redis round trip.
--
-- KEYS[1]  hash-tagged key, e.g. {user:42}:api_default:fw  (single slot => Redis Cluster safe)
-- ARGV[1]  limit: max units per window
-- ARGV[2]  window_ms
-- ARGV[3]  requested units (1 for a direct check, a chunk for a lease refill)
-- Returns: { allowed (0|1), remaining, retry_after_ms }
--
-- Time is cut into windows aligned to the epoch ([0, w), [w, 2w), ...) and each window counts
-- from zero. The cheapest of the three algorithms, but a client can spend `limit` at the end of
-- one window and `limit` again at the start of the next: up to 2x limit inside one window_ms span.
--
-- State: HASH { win, count }, integers only. Time comes from redis.call('TIME') so every limiter
-- instance agrees on where a window starts.

local limit     = tonumber(ARGV[1])
local window    = tonumber(ARGV[2])
local requested = tonumber(ARGV[3])

-- 1. Current Redis time and the window it falls in.
local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local win = math.floor(now_ms / window)

-- 2. Load state. Another (older) window or no key at all means a fresh count. A stored window
--    in the future means the clock moved back: keep counting in it, never hand out free capacity.
local st = redis.call('HMGET', KEYS[1], 'win', 'count')
local stored = tonumber(st[1])
local count = 0
if stored ~= nil and stored >= win then
  win = stored
  count = tonumber(st[2]) or 0
end
local reset_after = (win + 1) * window - now_ms

-- 3. Decide. A request larger than the limit can never pass, so waiting would not help
--    (retry_after = 0); otherwise the caller can retry when this window ends.
local allowed = 0
local retry_after = 0
if count + requested <= limit then
  count = count + requested
  allowed = 1
elseif requested <= limit then
  retry_after = reset_after
end

-- 4. Persist. The key expires when its window ends; the next window starts from zero anyway.
redis.call('HSET', KEYS[1], 'win', win, 'count', count)
redis.call('PEXPIRE', KEYS[1], reset_after)

local remaining = limit - count
if remaining < 0 then remaining = 0 end   -- the limit was lowered below the current count
return { allowed, remaining, retry_after }
