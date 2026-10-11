-- Sliding window counter: atomic check-and-update, one Redis round trip, O(1) memory.
--
-- KEYS[1]  hash-tagged key, e.g. {user:42}:login_attempts:sw  (single slot => Redis Cluster safe)
-- ARGV[1]  limit: max units in any window_ms-long span (estimated, see below)
-- ARGV[2]  window_ms
-- ARGV[3]  requested units (1 for a direct check, a chunk for a lease refill)
-- Returns: { allowed (0|1), remaining, retry_after_ms }
--
-- Keeps two fixed-window counters: the current window and the one before it. The usage over the
-- last window_ms is estimated as
--     prev * (share of the previous window still inside [now - window_ms, now]) + cur
-- which assumes the previous window's requests were spread evenly. That removes the fixed
-- window's 2x burst at window edges without storing one timestamp per request (the sliding
-- window *log* does that, with O(limit) memory and O(log n) work per call).
--
-- State: HASH { win, cur, prev }, integers only. Time comes from redis.call('TIME').

local limit     = tonumber(ARGV[1])
local window    = tonumber(ARGV[2])
local requested = tonumber(ARGV[3])

-- 1. Current Redis time and the fixed window it falls in.
local t = redis.call('TIME')
local now_ms = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local win = math.floor(now_ms / window)

-- 2. Load state and roll it forward to the current window.
local st = redis.call('HMGET', KEYS[1], 'win', 'cur', 'prev')
local stored = tonumber(st[1])
local cur  = tonumber(st[2]) or 0
local prev = tonumber(st[3]) or 0
if stored == nil then
  cur, prev = 0, 0              -- first use, or expired while idle
elseif stored > win then
  win = stored                  -- clock moved back: keep the stored window, never free capacity
elseif stored == win - 1 then
  prev, cur = cur, 0            -- moved into the next window
elseif stored < win - 1 then
  prev, cur = 0, 0              -- idle for at least a whole window
end

-- 3. Weight the previous window by how much of it is still inside the sliding span.
local elapsed = now_ms - win * window   -- ms into the current window (< 0 only after a clock jump)
if elapsed < 0 then elapsed = 0 end
local estimate = prev * (window - elapsed) / window + cur

-- 4. Decide. A request larger than the limit can never pass (retry_after = 0). Otherwise
--    retry_after is when the previous window's weight has decayed enough:
--      prev * (window - e) / window + cur + requested <= limit   =>   e = window * (1 - (limit - cur - requested) / prev)
--    If the current window alone is already too full, that point lies in the next window,
--    where today's cur becomes prev and nothing new is counted yet.
local allowed = 0
local retry_after = 0
if estimate + requested <= limit then
  cur = cur + requested
  estimate = estimate + requested
  allowed = 1
elseif requested <= limit then
  if cur + requested <= limit then   -- here prev > 0, or the estimate would have fit
    retry_after = math.ceil(window * (1 - (limit - cur - requested) / prev) - elapsed)
  else                               -- here cur > 0, since requested <= limit
    retry_after = (window - elapsed) + math.ceil(window * (1 - (limit - requested) / cur))
  end
end

-- 5. Persist. The state matters until the end of the next window (as `prev`), then it is dead.
redis.call('HSET', KEYS[1], 'win', win, 'cur', cur, 'prev', prev)
redis.call('PEXPIRE', KEYS[1], (win + 2) * window - now_ms)

local remaining = math.floor(limit - estimate)
if remaining < 0 then remaining = 0 end
return { allowed, remaining, retry_after }
