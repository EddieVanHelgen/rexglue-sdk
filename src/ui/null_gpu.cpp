/**
 * Null GPU backend flag (see include/rex/graphics/null_gpu.h).
 */

#include <rex/graphics/null_gpu.h>

#include <atomic>

namespace rex::graphics::null_gpu {

namespace {

std::atomic<bool> g_active{false};
std::atomic<SwapListener> g_swap_listener{nullptr};
std::atomic<double> g_guest_vblank_hz{0.0};
std::atomic<uint32_t> g_guest_vblank_divider{1};

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

}  // namespace rex::graphics::null_gpu
