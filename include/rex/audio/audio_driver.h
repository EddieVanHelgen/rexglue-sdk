/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

// The host audio output's clock, any thread: every frame the device took, counted at the driver's callback. A frame
// is 256 samples per channel at 48 kHz (5.33 ms). `silent` frames were played as silence because the guest had none
// queued (an underrun): each one puts the guest's sound 5.33 ms further behind real time. PDZ uses it for the
// cutscene picture (it follows the sound; game_pace.cpp). All zero with no audio device.
struct OutputClock {
  uint64_t played = 0;  // guest frames played
  uint64_t silent = 0;  // silence frames played in their place
};
OutputClock GetOutputClock();
constexpr double kOutputFrameMs = 256.0 * 1000.0 / 48000.0;

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
