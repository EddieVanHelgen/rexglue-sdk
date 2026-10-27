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

// Present queue depth override (PDZ stage 2 M5). With host vblank pacing (one Present(1, 0) per guest vblank) and
// guest vblanks at the display's own rate (120 on a 120 Hz display), presents are as frequent as refreshes: one extra
// present in a refresh (a late-latched frame after the vblank's own paint while the UI repaints at every tick) leaves
// the DXGI queue a frame deeper for good, and once it holds DXGI's default 3, every Present blocks inside the runtime
// until the next flip. The game thread's ExecuteCommandLists on the shared direct queue and its resource creation
// then wait behind that call (PDZ: ~4.4 ms of submit and ~2.3 ms of texture creation per frame at 120). A swap chain
// with a frame latency waitable object of depth N waits for room before painting, outside the runtime, instead.
// - SetPresentMaxFrameLatency: N (1-3; larger is taken as 3) for the D3D12 presenter's swap chains when the
//   present_max_frame_latency cvar is 0 (the cvar, when set, wins); 0 = none (the default: DXGI's own 3, no waitable
//   object, as before). Read when a swap chain is created: set it before the window's surface connects.
void SetPresentMaxFrameLatency(uint32_t frames);
uint32_t PresentMaxFrameLatency();

// Host present statistics (PDZ stage 2 M5), gathered by the D3D12 presenter only while enabled (default off: nothing
// is timed then). Sums are cumulative; the max fields are since the previous TakePresentStats (one reader).
struct PresentStats {
  uint64_t presents = 0;             // Present calls
  uint64_t present_us = 0;           // their total duration
  uint64_t present_max_us = 0;       // the longest (since the previous take)
  uint64_t presents_over_2ms = 0;    // calls that took more than 2 ms (blocked in the runtime)
  uint64_t repeats = 0;              // presents of the same guest frame as the previous present
  uint64_t ui_paints = 0;            // presents that also drew the UI (overlay drawers)
  uint64_t latency_waits = 0;        // frame latency waitable waits (none without a waitable object)
  uint64_t latency_wait_us = 0;      // their total duration
  uint64_t latency_wait_max_us = 0;  // the longest (since the previous take)
  uint64_t latency_timeouts = 0;     // waits that ran into the 100 ms bound
  uint32_t frame_latency = 0;        // the current swap chain's waitable depth (0 = none)
};
void EnablePresentStats(bool enable);
bool PresentStatsEnabled();
// Called by the presenter's painting thread after each Present. latency_wait_us < 0: no waitable wait.
void RecordPresent(uint64_t present_us, int64_t latency_wait_us, bool latency_timeout, bool repeat,
                   bool ui_paint);
// Called by the presenter when it creates a swap chain: its waitable depth (0 = none).
void SetPresentFrameLatencyInUse(uint32_t frames);
PresentStats TakePresentStats();

}  // namespace rex::graphics::null_gpu
