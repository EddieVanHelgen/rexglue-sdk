/**
 * Resolve readback ranges (see include/rex/graphics/resolve_readback.h).
 * Built into rexruntime with the other shared ui objects so both the app and
 * the GPU plugin see the same registry.
 */

#include <rex/graphics/resolve_readback.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace rex::graphics::resolve_readback {

namespace {

std::mutex g_mutex;
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;  // start, length
std::atomic<uint32_t> g_count{0};

}  // namespace

void Add(uint32_t physical_address, uint32_t length) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& range : g_ranges) {
    if (range.first == physical_address) {
      range.second = std::max(range.second, length);
      return;
    }
  }
  g_ranges.emplace_back(physical_address, length);
  g_count = uint32_t(g_ranges.size());
}

void Remove(uint32_t physical_address) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ranges.erase(std::remove_if(g_ranges.begin(), g_ranges.end(),
                                [&](const auto& range) { return range.first == physical_address; }),
                 g_ranges.end());
  g_count = uint32_t(g_ranges.size());
}

bool Any() {
  return g_count.load(std::memory_order_relaxed) != 0;
}

bool Overlaps(uint32_t address, uint32_t length) {
  if (!Any()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t end = uint64_t(address) + length;
  for (const auto& range : g_ranges) {
    if (address < uint64_t(range.first) + range.second && range.first < end) {
      return true;
    }
  }
  return false;
}

}  // namespace rex::graphics::resolve_readback
