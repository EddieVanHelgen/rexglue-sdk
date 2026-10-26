/**
 * Null GPU backend flag.
 *
 * graphics_backend = "null" (rexgpu-xenos, D3D12NullGraphicsSystem with a
 * NullCommandProcessor): the plugin consumes the PM4 ring and services every
 * synchronisation the guest needs (register writes with the scratch
 * write-back, fences, WAIT_REG_MEM, INTERRUPT, the read-pointer write-back,
 * the vblank interrupt and counter, the DC_LUT gamma table) but executes no
 * draws, copies or shaders on the host GPU: a renderer in the title is the
 * only output. The plugin sets the flag when it creates that backend, before
 * the guest runs; the title reads it (no RTTI across the plugin boundary).
 *
 * Lives in rexruntime (shared by the app and the GPU plugin).
 */

#pragma once

namespace rex::graphics::null_gpu {

// Set by the null backend's graphics system when it is created.
void SetActive(bool active);

// True when the null backend is the graphics system of this process.
bool Active();

}  // namespace rex::graphics::null_gpu
