/**
 * Null GPU backend flag (see include/rex/graphics/null_gpu.h).
 */

#include <rex/graphics/null_gpu.h>

#include <atomic>

namespace rex::graphics::null_gpu {

namespace {

std::atomic<bool> g_active{false};
std::atomic<SwapListener> g_swap_listener{nullptr};

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

}  // namespace rex::graphics::null_gpu
