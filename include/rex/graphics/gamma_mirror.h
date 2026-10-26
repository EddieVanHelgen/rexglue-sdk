/**
 * Gamma table mirror.
 *
 * The 256-entry gamma table the GPU plugin applies to the guest output (the
 * DC_LUT registers D3D wrote: 10:10:10 per entry), published at every
 * IssueSwap so a renderer in the title (Perfect Dark Zero's native renderer)
 * can apply exactly the same table instead of the ramp the game passed to
 * D3D, which D3D transforms before writing the registers.
 *
 * Lives in rexruntime (shared by the app and the GPU plugin).
 */

#pragma once

#include <cstdint>

namespace rex::graphics::gamma_mirror {

// table: 256 entries, blue bits 0:9, green 10:19, red 20:29 (DC_LUT_30_COLOR).
void Publish(const uint32_t* table);

// Copies the last published table as 16-bit unorm per channel (red, green,
// blue rows) and returns its generation (0 = nothing published yet).
uint64_t Get(uint16_t out[3][256]);

// Generation of the last published table that differed from the previous one.
uint64_t Generation();

}  // namespace rex::graphics::gamma_mirror
