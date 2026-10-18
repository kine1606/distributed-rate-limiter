#pragma once
// How many tokens one lease refill asks Redis for (src/lease/README.md, step 5). Pure functions, so
// the policy is tested apart from the engine.

#include <algorithm>
#include <cstdint>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// One instance never holds more than 10% of a bucket: the cap is what the accuracy bound
// limit + N x C counts with.
inline std::uint32_t ChunkCap(const Rule& r) { return std::max<std::uint32_t>(1, r.limit / 10); }
inline std::uint32_t ChunkMin(const Rule& r) { return std::min<std::uint32_t>(8, ChunkCap(r)); }

// A chunk that lasted longer than this held its tokens unused (and invisible to other instances)
// for too long. 10 s keeps a key at about 3 checks/s on a chunk of 25, a 96% hit ratio.
inline constexpr std::int64_t kChunkIdleNs = 10'000'000'000;

// The next chunk, decided when a full refill lands. `emptied`: checks found the lease dry while
// the refill was out, so demand outran the chunk within one round trip: double. `idle`: the
// previous chunk lasted longer than kChunkIdleNs: halve. Both or neither: keep.
inline std::uint32_t NextChunk(std::uint32_t chunk, bool emptied, bool idle, const Rule& r) {
  std::uint32_t next = chunk;
  if (emptied && !idle) next = chunk * 2;
  if (idle && !emptied) next = chunk / 2;
  return std::clamp(next, ChunkMin(r), ChunkCap(r));
}

}  // namespace ratelimiter
