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

#include <cstdint>

namespace rex::graphics::null_gpu {

// Set by the null backend's graphics system when it is created.
void SetActive(bool active);

// True when the null backend is the graphics system of this process.
bool Active();

// XE_SWAP listener (PDZ stage 2 M3, present timing): the null command processor
// calls it at every XE_SWAP packet, on the command processor thread, after its
// own swap bookkeeping (the frame_pacing 'S' row, the gamma table, the swap
// index). swap_index counts from 1, as the D3D12 backend's IssueSwap does;
// frontbuffer_ptr / width / height are the packet's. A title that presents its
// own frames can hand frame N to the presenter here, at the moment the D3D12
// backend would refresh it. It must not wait on a guest thread (the guest may
// be waiting on the ring this thread consumes). nullptr = none (the default).
using SwapListener = void (*)(uint64_t swap_index, uint32_t frontbuffer_ptr, uint32_t width,
                              uint32_t height);
void SetSwapListener(SwapListener listener);
// Called by the null command processor.
void NotifySwap(uint64_t swap_index, uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);

// Guest vblank pacing overrides (PDZ stage 2 M5, the title's pdz_null_refresh_hz). Honoured by the vsync worker of
// any backend and by the presenter's host-vblank divisor; the video mode the guest reads (VdQueryVideoMode,
// video_mode_refresh_rate) is not changed. Defaults: 0 Hz (= video_mode_refresh_rate, today's behaviour) and
// divider 1, so nothing changes unless a title sets them.
// - SetGuestVblankHz: the guest vblank rate in Hz (0 = video_mode_refresh_rate). Set it before the GPU setup: the
//   vsync worker reads it once when it starts, the presenter whenever the display output changes.
// - SetGuestVblankDivider: deliver one guest vblank every N periods (N >= 1; 0 is taken as 1), e.g. 2 at a 120 Hz
//   guest rate = 60 vblanks/s. Read at every vblank: it may change at any time, from any thread.
void SetGuestVblankHz(double hz);
double GuestVblankHz();
void SetGuestVblankDivider(uint32_t divider);
uint32_t GuestVblankDivider();

}  // namespace rex::graphics::null_gpu
