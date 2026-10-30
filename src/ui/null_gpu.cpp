/**
 * Null GPU backend flag (see include/rex/graphics/null_gpu.h).
 */

#include <rex/graphics/null_gpu.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace rex::graphics::null_gpu {

namespace {

std::atomic<bool> g_active{false};
std::atomic<SwapListener> g_swap_listener{nullptr};
std::atomic<double> g_guest_vblank_hz{0.0};
std::atomic<uint32_t> g_guest_vblank_divider{1};
std::atomic<uint32_t> g_present_pacing{uint32_t(PresentPacing::kLatch)};
std::atomic<double> g_guest_vblank_cap_hz{0.0};
std::atomic<double> g_display_refresh_hz{0.0};
std::atomic<uint32_t> g_present_max_frame_latency{0};
std::atomic<bool> g_present_stats_enabled{false};
std::atomic<WritePointerFilter> g_write_pointer_filter{nullptr};
// The sink is set / cleared by the graphics system and used by the title's thread: one mutex keeps a delivery from
// running into a command processor being destroyed (one lock per kick, uncontended).
std::mutex g_write_pointer_sink_mutex;
WritePointerSink g_write_pointer_sink = nullptr;
void* g_write_pointer_sink_context = nullptr;

struct PresentCounters {
  std::atomic<uint64_t> presents{0}, present_us{0}, present_max_us{0}, presents_over_2ms{0}, repeats{0},
      ui_paints{0}, latency_waits{0}, latency_wait_us{0}, latency_wait_max_us{0}, latency_timeouts{0};
  std::atomic<uint32_t> frame_latency{0};
};
PresentCounters g_present_counters;

void StoreMax(std::atomic<uint64_t>& slot, uint64_t value) {
  uint64_t prev = slot.load(std::memory_order_relaxed);
  while (value > prev && !slot.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

void SetActive(bool active) {
  g_active.store(active, std::memory_order_release);
}

bool Active() {
  return g_active.load(std::memory_order_acquire);
}

void SetSwapListener(SwapListener listener) {
  g_swap_listener.store(listener, std::memory_order_release);
}

void NotifySwap(uint64_t swap_index, uint32_t frontbuffer_ptr, uint32_t width, uint32_t height) {
  if (const SwapListener listener = g_swap_listener.load(std::memory_order_acquire)) {
    listener(swap_index, frontbuffer_ptr, width, height);
  }
}

void SetGuestVblankHz(double hz) {
  g_guest_vblank_hz.store(hz > 0.0 ? hz : 0.0, std::memory_order_release);
}

double GuestVblankHz() {
  return g_guest_vblank_hz.load(std::memory_order_acquire);
}

void SetGuestVblankDivider(uint32_t divider) {
  g_guest_vblank_divider.store(divider ? divider : 1, std::memory_order_relaxed);
}

uint32_t GuestVblankDivider() {
  return g_guest_vblank_divider.load(std::memory_order_relaxed);
}

void SetPresentPacing(PresentPacing pacing) {
  g_present_pacing.store(uint32_t(pacing), std::memory_order_release);
}

PresentPacing GetPresentPacing() {
  return PresentPacing(g_present_pacing.load(std::memory_order_relaxed));
}

void SetGuestVblankCapHz(double hz) {
  g_guest_vblank_cap_hz.store(hz > 0.0 ? hz : 0.0, std::memory_order_relaxed);
}

double GuestVblankCapHz() {
  return g_guest_vblank_cap_hz.load(std::memory_order_relaxed);
}

void SetDisplayRefreshHz(double hz) {
  g_display_refresh_hz.store(hz > 1.0 ? hz : 0.0, std::memory_order_release);
}

double DisplayRefreshHz() {
  return g_display_refresh_hz.load(std::memory_order_acquire);
}

void SetPresentMaxFrameLatency(uint32_t frames) {
  g_present_max_frame_latency.store(std::min(frames, 3u), std::memory_order_release);
}

uint32_t PresentMaxFrameLatency() {
  return g_present_max_frame_latency.load(std::memory_order_acquire);
}

void EnablePresentStats(bool enable) {
  g_present_stats_enabled.store(enable, std::memory_order_release);
}

bool PresentStatsEnabled() {
  return g_present_stats_enabled.load(std::memory_order_relaxed);
}

void RecordPresent(uint64_t present_us, int64_t latency_wait_us, bool latency_timeout, bool repeat,
                   bool ui_paint) {
  PresentCounters& c = g_present_counters;
  c.presents.fetch_add(1, std::memory_order_relaxed);
  c.present_us.fetch_add(present_us, std::memory_order_relaxed);
  StoreMax(c.present_max_us, present_us);
  if (present_us > 2000) {
    c.presents_over_2ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (repeat) {
    c.repeats.fetch_add(1, std::memory_order_relaxed);
  }
  if (ui_paint) {
    c.ui_paints.fetch_add(1, std::memory_order_relaxed);
  }
  if (latency_wait_us >= 0) {
    c.latency_waits.fetch_add(1, std::memory_order_relaxed);
    c.latency_wait_us.fetch_add(uint64_t(latency_wait_us), std::memory_order_relaxed);
    StoreMax(c.latency_wait_max_us, uint64_t(latency_wait_us));
    if (latency_timeout) {
      c.latency_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void SetPresentFrameLatencyInUse(uint32_t frames) {
  g_present_counters.frame_latency.store(frames, std::memory_order_relaxed);
}

PresentStats TakePresentStats() {
  PresentCounters& c = g_present_counters;
  PresentStats s;
  s.presents = c.presents.load(std::memory_order_relaxed);
  s.present_us = c.present_us.load(std::memory_order_relaxed);
  s.present_max_us = c.present_max_us.exchange(0, std::memory_order_relaxed);
  s.presents_over_2ms = c.presents_over_2ms.load(std::memory_order_relaxed);
  s.repeats = c.repeats.load(std::memory_order_relaxed);
  s.ui_paints = c.ui_paints.load(std::memory_order_relaxed);
  s.latency_waits = c.latency_waits.load(std::memory_order_relaxed);
  s.latency_wait_us = c.latency_wait_us.load(std::memory_order_relaxed);
  s.latency_wait_max_us = c.latency_wait_max_us.exchange(0, std::memory_order_relaxed);
  s.latency_timeouts = c.latency_timeouts.load(std::memory_order_relaxed);
  s.frame_latency = c.frame_latency.load(std::memory_order_relaxed);
  return s;
}

void SetWritePointerFilter(WritePointerFilter filter) {
  g_write_pointer_filter.store(filter, std::memory_order_release);
}

bool FilterWritePointer(uint32_t value) {
  const WritePointerFilter filter = g_write_pointer_filter.load(std::memory_order_acquire);
  return filter && filter(value);
}

void SetWritePointerSink(WritePointerSink sink, void* context) {
  std::lock_guard<std::mutex> lock(g_write_pointer_sink_mutex);
  g_write_pointer_sink = sink;
  g_write_pointer_sink_context = context;
}

bool DeliverWritePointer(uint32_t value) {
  std::lock_guard<std::mutex> lock(g_write_pointer_sink_mutex);
  if (!g_write_pointer_sink) {
    return false;
  }
  g_write_pointer_sink(g_write_pointer_sink_context, value);
  return true;
}

}  // namespace rex::graphics::null_gpu
