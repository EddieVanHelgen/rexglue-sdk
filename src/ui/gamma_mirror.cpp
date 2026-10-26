/**
 * Gamma table mirror (see include/rex/graphics/gamma_mirror.h).
 */

#include <rex/graphics/gamma_mirror.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace rex::graphics::gamma_mirror {

namespace {

std::mutex g_mutex;
uint32_t g_table[256] = {};
std::atomic<uint64_t> g_generation{0};

}  // namespace

void Publish(const uint32_t* table) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_generation.load() && std::memcmp(g_table, table, sizeof(g_table)) == 0) {
    return;
  }
  std::memcpy(g_table, table, sizeof(g_table));
  ++g_generation;
}

uint64_t Get(uint16_t out[3][256]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t generation = g_generation.load();
  if (!generation) {
    return 0;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = g_table[i];
    const uint32_t c[3] = {(v >> 20) & 0x3FF, (v >> 10) & 0x3FF, v & 0x3FF};
    for (uint32_t k = 0; k < 3; ++k) {
      out[k][i] = uint16_t((c[k] << 6) | (c[k] >> 4));
    }
  }
  return generation;
}

uint64_t Generation() {
  return g_generation.load();
}

}  // namespace rex::graphics::gamma_mirror
