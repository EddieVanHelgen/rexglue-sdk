/**
 * Null GPU backend flag (see include/rex/graphics/null_gpu.h).
 */

#include <rex/graphics/null_gpu.h>

#include <atomic>

namespace rex::graphics::null_gpu {

namespace {

std::atomic<bool> g_active{false};

}  // namespace

void SetActive(bool active) {
  g_active.store(active, std::memory_order_release);
}

bool Active() {
  return g_active.load(std::memory_order_acquire);
}

}  // namespace rex::graphics::null_gpu
