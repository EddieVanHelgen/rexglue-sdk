/**
 * Frame pacing trace (diagnostic).
 *
 * With frame_pacing_log = true, timestamps of every guest vblank, guest swap,
 * host present and host display vblank are written as CSV to
 * <exe_dir>/logs/frame_pacing.csv, so frame pacing can be measured rather
 * than guessed. Off by default; costs one branch per event when off.
 *
 * Input latency: the title's newest controller read before each VdSwap is
 * carried with that frame to its first host present and, from DXGI frame
 * statistics, to the refresh it was first shown at. Each frame gets an 'L'
 * row at its present and a 'D' row once it's on screen, and a summary goes
 * to the log about every 10 seconds.
 */

#pragma once

#include <cstdint>

namespace rex::ui::frame_pacing {

enum class Event : char {
  kGuestVblank = 'V',     // GraphicsSystem::MarkVblank (guest vblank interrupt).
  kGuestVdSwap = 'W',     // VdSwap called by the title (guest CPU thread).
  kGuestSwap = 'S',       // XE_SWAP packet reached the command processor.
  kGuestOutput = 'R',     // Guest frame published to the presenter mailbox. a = seq.
  kHostPresent = 'P',     // Host Present. a = guest seq, b = present call us, c = flags
                          // (1 UI, 2 tearing, 4 stats, 8 paced, 16 grid locked),
                          // d = DXGI last present count, e/f = stats present/sync count.
  kHostVblank = 'H',      // Host display vblank (IDXGIOutput::WaitForVBlank).
  kGpuAwait = 'Q',        // Command processor blocked on host GPU completion. a = us.
  kCpWait = 'M',          // CP WAIT_REG_MEM had to wait. a = addr, b = ref, c = info<<32|mask,
                          // d = wait field, e = us.
  kCpInterrupt = 'I',     // CP INTERRUPT packet. a = cpu mask.
  kGuestRingWait = 'G',   // Guest polled for command buffer space. a = us, b = polls.
  kInputPoll = 'X',       // Controller state read for the title. a = device, b = age of the
                          // host state in us (0 = read from the device just now).
  kGpuFrameOpen = 'F',    // CP opened a frame. a = frames still in flight on the host GPU.
  kHostDisplay = 'O',     // DXGI frame statistics. a = present count on screen, b = its QPC.
  // Derived, written by the trace itself:
  // 'L' a = seq, b = input read -> present us, c = VdSwap -> present us,
  //     d = input state age us, e = input read -> VdSwap us.
  // 'D' a = seq, b = input read -> on screen us, c = present -> on screen us,
  //     d = VdSwap -> on screen us.
};

bool Enabled();

void Record(Event event, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
            uint64_t e = 0, uint64_t f = 0);

}  // namespace rex::ui::frame_pacing
