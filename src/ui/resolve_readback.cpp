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

namespace {
std::atomic<uint64_t> g_dump_swap{0};
std::atomic<uint64_t> g_last_swap{0};
std::atomic<DumpObserver> g_dump_observer{nullptr};
}  // namespace

void ArmDump(uint64_t swap, DumpObserver observer) {
  g_dump_observer = observer;
  g_dump_swap = swap;
}

void NoteSwap(uint64_t swap) {
  g_last_swap = swap;
  if (g_dump_swap.load(std::memory_order_relaxed) && swap >= g_dump_swap.load()) {
    g_dump_swap = 0;  // the armed frame is complete
  }
}

bool DumpingFrame() {
  const uint64_t armed = g_dump_swap.load(std::memory_order_relaxed);
  return armed && g_last_swap.load(std::memory_order_relaxed) + 1 == armed &&
         g_dump_observer.load(std::memory_order_relaxed);
}

void ObserveDump(uint32_t address, uint32_t length, const uint8_t* data, uint32_t copy_dest_info) {
  if (DumpObserver observer = g_dump_observer.load()) {
    observer(address, length, data, copy_dest_info);
  }
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
