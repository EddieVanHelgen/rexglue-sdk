/**
 * GPU census (see include/rex/graphics/gpu_census.h). Built into rexruntime
 * with the other shared ui objects so the app and the GPU plugin see the same
 * counters.
 */

#include <rex/graphics/gpu_census.h>

namespace rex::graphics::gpu_census {

namespace {

std::atomic<bool> g_enabled{false};
Census g_census;  // zero-initialised static storage (atomics are constant-initialised)
std::atomic<uint32_t> g_kind_rows[256];  // keys inserted per kind (the "other" rows not included)

uint64_t MakeKey(RowKind kind, uint32_t a, uint32_t b) {
  return (uint64_t(uint8_t(kind)) << 56) | (uint64_t(b & 0xFFFFFFu) << 32) | uint64_t(a);
}

}  // namespace

void SetEnabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

bool Enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

Census& Get() {
  return g_census;
}

Row* FindRow(RowKind kind, uint32_t a, uint32_t b) {
  const uint64_t other_key = MakeKey(kind, kOtherA, kOtherB);
  std::atomic<uint32_t>& kind_rows = g_kind_rows[uint8_t(kind)];
  uint64_t key = MakeKey(kind, a, b);
  constexpr uint32_t kMask = kRowCount - 1;
  for (;;) {
    bool retry_as_other = false;
    uint32_t index = uint32_t((key * UINT64_C(0x9E3779B97F4A7C15)) >> 40) & kMask;
    for (uint32_t probe = 0; probe < kRowCount; ++probe, index = (index + 1) & kMask) {
      Row& row = g_census.rows[index];
      uint64_t seen = row.key.load(std::memory_order_acquire);
      if (seen == key) {
        return &row;
      }
      if (seen != 0) {
        continue;
      }
      // An empty slot: the key is new.
      if (key != other_key && kind_rows.load(std::memory_order_relaxed) >= kRowsPerKind) {
        retry_as_other = true;
        break;
      }
      if (row.key.compare_exchange_strong(seen, key, std::memory_order_acq_rel)) {
        if (key != other_key) {
          kind_rows.fetch_add(1, std::memory_order_relaxed);
        }
        return &row;
      }
      if (seen == key) {
        return &row;  // another thread inserted the same key
      }
    }
    if (!retry_as_other) {
      break;
    }
    key = other_key;
  }
  g_census.rows_dropped.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

}  // namespace rex::graphics::gpu_census
