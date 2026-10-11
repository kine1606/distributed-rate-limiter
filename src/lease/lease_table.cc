#include "lease/lease_table.h"

#include <algorithm>
#include <bit>
#include <functional>

namespace ratelimiter {

LeaseTable::LeaseTable(std::size_t capacity)
    : mask_(std::bit_ceil(std::max(capacity, kMaxProbe)) - 1),
      slots_(std::make_unique<std::atomic<Lease*>[]>(mask_ + 1)) {}

LeaseTable::~LeaseTable() {
  for (std::size_t i = 0; i <= mask_; ++i) delete slots_[i].load(std::memory_order_relaxed);
}

Lease* LeaseTable::FindOrCreate(const Rule& rule, std::string_view key) {
  const std::hash<std::string_view> hash;
  const std::size_t h = hash(key) ^ (hash(rule.name) * 0x9e3779b97f4a7c15ULL);
  std::unique_ptr<Lease> fresh;  // built at most once, only for a key seen for the first time

  // Linear probing. Slots are never cleared, so a key is always at the first slot of its probe
  // sequence that was empty when it was inserted, and every later lookup walks the same prefix.
  for (std::size_t i = 0; i < kMaxProbe; ++i) {
    std::atomic<Lease*>& slot = slots_[(h + i) & mask_];
    Lease* l = slot.load(std::memory_order_acquire);  // pairs with the release below: sees key/rule
    if (l == nullptr) {
      if (!fresh) fresh = std::make_unique<Lease>(rule, key);
      if (slot.compare_exchange_strong(l, fresh.get(), std::memory_order_release,
                                       std::memory_order_acquire)) {
        return fresh.release();  // owned by the table from now on
      }
      // Another thread filled this slot first; `l` is its lease, compared like any other.
    }
    if (&l->rule == &rule && l->key == key) return l;
  }
  return nullptr;
}

}  // namespace ratelimiter
