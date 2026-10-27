/**
 * Null command processor (see include/rex/graphics/null_command_processor.h).
 */

#include <rex/graphics/null_command_processor.h>

#include <bit>
#include <chrono>

#include <rex/graphics/gamma_mirror.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/null_gpu.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/resolve_readback.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/frame_pacing.h>
#include <rex/ui/presenter.h>

namespace rex::graphics {

namespace {

// The placeholder shaders' microcode (never analyzed or translated).
constexpr uint32_t kPlaceholderUcode[1] = {0};

// D3D's InsertCallback body (Xbox 360 D3D, e.g. PDZ sub_826DE408) writes SCRATCH_REG4 back to this
// once the wait for the interrupt handler's CPU-mask clear has passed.
constexpr uint32_t kCallbackCleared = 0x0BADF00D;

}  // namespace

NullCommandProcessor::NullCommandProcessor(GraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {
  placeholder_shaders_[0] = std::make_unique<Shader>(xenos::ShaderType::kVertex, 0,
                                                     kPlaceholderUcode, 1, std::endian::native);
  placeholder_shaders_[1] = std::make_unique<Shader>(xenos::ShaderType::kPixel, 0,
                                                     kPlaceholderUcode, 1, std::endian::native);
}

NullCommandProcessor::~NullCommandProcessor() = default;

uint64_t NullCommandProcessor::NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool NullCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  // No host queries: EVENT_WRITE_ZPD gets the fake counter walk. The fast and strict modes queue
  // reports that only host query resolves retire (strict would even wait for them). PDZ issues
  // no ZPD (M0 census), so this is a guard.
  zpd_mode_ = ZPDMode::kFake;
  setup_us_.store(NowUs(), std::memory_order_relaxed);
  REXGPU_INFO(
      "null_gpu: command processor thread up: ring, indirect buffers, register file with the scratch "
      "write-back, COHER, DC_LUT gamma, WAIT_REG_MEM, INTERRUPT, EVENT_WRITE*, read-pointer write-back "
      "and XE_SWAP as the D3D12 backend (the base command processor); draws, resolves and shader "
      "loads do no host GPU work; occlusion queries fake");
  return true;
}

void NullCommandProcessor::ShutdownContext() {
  CommandProcessor::ShutdownContext();
}

void NullCommandProcessor::NoteCallbackAddress(uint32_t value) {
  if (value == kCallbackCleared) {
    Bump(callbacks_done_);
    if (!logged_first_done_) {
      logged_first_done_ = true;
      REXGPU_INFO(
          "null_gpu: first InsertCallback sequence complete (SCRATCH_REG4 = 0BADF00D): the wait for "
          "the guest interrupt handler's CPU-mask clear passed ({:.1f} ms after the CP thread started)",
          double(NowUs() - setup_us_.load(std::memory_order_relaxed)) / 1000.0);
    }
    return;
  }
  Bump(callbacks_begun_);
  last_callback_.store(value, std::memory_order_relaxed);
}

void NullCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  // Diagnostics only (the base did the write and its side effects): D3D's synchronisation
  // registers. SCRATCH_REG0..7 = 0x578..0x57F (4 = CALLBACK_ADDRESS, 5 = CALLBACK_CONTEXT).
  const uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
  if (scratch < 8) {
    if ((register_file_->values[XE_GPU_REG_SCRATCH_UMSK] >> scratch) & 1) {
      Bump(scratch_writebacks_);
    }
    if (index == XE_GPU_REG_CALLBACK_ADDRESS) {
      NoteCallbackAddress(value);
    } else if (index == XE_GPU_REG_CALLBACK_CONTEXT) {
      last_callback_context_.store(value, std::memory_order_relaxed);
      // A callback not seen before (the context follows the callback in the same register run).
      const uint32_t callback = register_file_->values[XE_GPU_REG_CALLBACK_ADDRESS];
      if (callback != kCallbackCleared && logged_callback_count_ < kLoggedCallbacks) {
        bool seen = false;
        for (size_t i = 0; i < logged_callback_count_; ++i) {
          seen = seen || logged_callbacks_[i] == callback;
        }
        if (!seen) {
          logged_callbacks_[logged_callback_count_++] = callback;
          REXGPU_INFO(
              "null_gpu: InsertCallback sequence with a new callback {:08X} (context {:08X}; #{} "
              "distinct): SCRATCH_REG0 = CPU mask, WAIT_REG_MEM, INTERRUPT, CALLBACK_ACK, "
              "WAIT_REG_MEM, SCRATCH_REG4 = 0BADF00D follow",
              callback, value, logged_callback_count_);
        }
      }
    } else if (index == XE_GPU_REG_SCRATCH_REG1) {
      Bump(gates_armed_);
      if (!logged_first_gate_) {
        logged_first_gate_ = true;
        REXGPU_INFO(
            "null_gpu: first presentation-interval gate armed (SCRATCH_REG1 = {}): the CP waits on "
            "scratch +4 until the guest's vblank interrupt (or the late-frame callback) clears it",
            value);
      }
    }
  } else if (index == XE_GPU_REG_CALLBACK_ACK) {
    Bump(callbacks_acked_);
    if (!logged_first_ack_) {
      logged_first_ack_ = true;
      REXGPU_INFO(
          "null_gpu: first INTERRUPT dispatched to the guest and returned (source 1; CALLBACK_ACK "
          "{:08X} after it) for callback {:08X}, {:.1f} ms after the CP thread started",
          value, last_callback_.load(std::memory_order_relaxed),
          double(NowUs() - setup_us_.load(std::memory_order_relaxed)) / 1000.0);
    }
  }
}

void NullCommandProcessor::PrepareForWait() {
  CommandProcessor::PrepareForWait();
  // The idle loop calls this with the ring empty; WAIT_REG_MEM calls it before each vblank-paced
  // sleep, inside a burst (the read pointer is the burst's start until the burst ends).
  const uint32_t write_ptr = write_ptr_index_.load(std::memory_order_relaxed);
  const bool ring_empty = write_ptr == 0xBAADF00D || read_ptr_index_ == write_ptr;
  Bump(ring_empty ? idle_waits_ : reg_waits_);
  wait_since_us_.store(NowUs(), std::memory_order_relaxed);
  in_wait_.store(true, std::memory_order_relaxed);
  if (!ring_empty && !logged_first_reg_wait_) {
    logged_first_reg_wait_ = true;
    REXGPU_INFO(
        "null_gpu: first vblank-paced WAIT_REG_MEM sleep (a memory or register poll that has to "
        "wait: the presentation-interval gate, a display-controller poll); the guest's vblank "
        "interrupt ends each sleep");
  }
}

void NullCommandProcessor::ReturnFromWait() {
  in_wait_.store(false, std::memory_order_relaxed);
  CommandProcessor::ReturnFromWait();
}

void NullCommandProcessor::OnPrimaryBufferEnd() {
  CommandProcessor::OnPrimaryBufferEnd();
  Bump(bursts_);
  last_burst_end_us_.store(NowUs(), std::memory_order_relaxed);
  if (!logged_first_burst_) {
    logged_first_burst_ = true;
    REXGPU_INFO(
        "null_gpu: first ring burst executed ({:.1f} ms after the CP thread started; read-pointer "
        "write-back {:08X} every {} dwords)",
        double(NowUs() - setup_us_.load(std::memory_order_relaxed)) / 1000.0,
        read_ptr_writeback_ptr_, read_ptr_update_freq_);
  }
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                         const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  (void)host_address;
  Bump(shader_loads_);
  if (!logged_first_shader_) {
    logged_first_shader_ = true;
    REXGPU_INFO(
        "null_gpu: first shader load ({} shader, {} dwords): a placeholder is bound, nothing is "
        "translated",
        shader_type == xenos::ShaderType::kPixel ? "pixel" : "vertex", dword_count);
  }
  return placeholder_shaders_[shader_type == xenos::ShaderType::kPixel ? 1 : 0].get();
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  (void)index_buffer_info;
  (void)major_mode_explicit;
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++frame_stats_.draws;
  Bump(draws_);
  if (!logged_first_draw_) {
    logged_first_draw_ = true;
    REXGPU_INFO("null_gpu: first draw dropped (primitive {}, {} indices); no draw reaches the host GPU",
                uint32_t(prim_type), index_count);
  }
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  ++frame_stats_.copies;
  Bump(copies_);
  if (!logged_first_copy_) {
    logged_first_copy_ = true;
    REXGPU_INFO(
        "null_gpu: first resolve dropped (RB_COPY_DEST_BASE {:08X}); no EDRAM copy reaches guest memory "
        "(resolve_readback ranges are not served: the title's renderer reads back what it needs)",
        register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE]);
  }
  return true;
}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  EndZPDFrame();
  // What the D3D12 IssueSwap publishes for a renderer in the title, before its presenter work: the
  // swap index (counted from 1; the resolve readback dump) and the 256-entry gamma table (the
  // native gamma pass applies it).
  const uint64_t swap_index = ++swap_index_;
  resolve_readback::NoteSwap(swap_index);
  gamma_mirror::Publish(reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table()));
  // The 'F' row the D3D12 backend writes when it opens a frame: no host GPU frame in flight here.
  ui::frame_pacing::Record(ui::frame_pacing::Event::kGpuFrameOpen, 0);
  // Nothing is handed to the presenter here: the guest output is the title's (its renderer
  // refreshes the presenter from the guest Swap, or from the XE_SWAP listener called at the end;
  // the vsync worker's host vblank latch is the base graphics system's, as with the D3D12 backend).
  // A same-frame capture of the emulated output (Presenter::RequestGuestOutputCapture) has no
  // emulated frame to capture: answered at once with an empty image so a title waiting for it gets
  // no capture instead of waiting.
  if (ui::Presenter::RequestedGuestOutputCapture() == swap_index) {
    ui::Presenter::StoreGuestOutputCapture(swap_index, ui::RawImage());
    if (!logged_capture_) {
      logged_capture_ = true;
      REXGPU_WARN(
          "null_gpu: a same-frame capture of swap {} was requested; there is no emulated frame, an "
          "empty capture was stored",
          swap_index);
    }
  }
  const uint64_t now_us = NowUs();
  Bump(swaps_);
  last_swap_us_.store(now_us, std::memory_order_relaxed);
  if (!logged_first_swap_) {
    logged_first_swap_ = true;
    REXGPU_INFO(
        "null_gpu: first XE_SWAP at the command processor: front buffer {:08X} {}x{}, {:.1f} s after "
        "the CP thread started, {} vblanks, {} draws and {} resolves dropped so far; nothing is "
        "presented here (the title's renderer presents)",
        frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
        double(now_us - setup_us_.load(std::memory_order_relaxed)) / 1000000.0,
        uint32_t(*reinterpret_cast<const volatile uint32_t*>(&counter_)),
        draws_.load(std::memory_order_relaxed), copies_.load(std::memory_order_relaxed));
  }
  // The title's XE_SWAP listener (null_gpu.h; PDZ: pdz_native_present_timing 1 hands the native
  // frame of this swap to the presenter here, where the D3D12 backend refreshes it).
  null_gpu::NotifySwap(swap_index, frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
}

NullCommandProcessor::Status NullCommandProcessor::GetStatus() const {
  Status s;
  s.setup_us = setup_us_.load(std::memory_order_relaxed);
  s.bursts = bursts_.load(std::memory_order_relaxed);
  s.last_burst_end_us = last_burst_end_us_.load(std::memory_order_relaxed);
  s.swaps = swaps_.load(std::memory_order_relaxed);
  s.last_swap_us = last_swap_us_.load(std::memory_order_relaxed);
  s.draws = draws_.load(std::memory_order_relaxed);
  s.copies = copies_.load(std::memory_order_relaxed);
  s.shader_loads = shader_loads_.load(std::memory_order_relaxed);
  s.idle_waits = idle_waits_.load(std::memory_order_relaxed);
  s.reg_waits = reg_waits_.load(std::memory_order_relaxed);
  s.in_wait = in_wait_.load(std::memory_order_relaxed);
  s.wait_since_us = wait_since_us_.load(std::memory_order_relaxed);
  s.callbacks_begun = callbacks_begun_.load(std::memory_order_relaxed);
  s.callbacks_acked = callbacks_acked_.load(std::memory_order_relaxed);
  s.callbacks_done = callbacks_done_.load(std::memory_order_relaxed);
  s.last_callback = last_callback_.load(std::memory_order_relaxed);
  s.last_callback_context = last_callback_context_.load(std::memory_order_relaxed);
  s.gates_armed = gates_armed_.load(std::memory_order_relaxed);
  s.scratch_writebacks = scratch_writebacks_.load(std::memory_order_relaxed);
  // Plain members of the base written by other threads (the vblank counter by the vsync worker,
  // the read pointer by the command processor thread): aligned 32-bit reads for a diagnostic.
  s.vblank_counter = *reinterpret_cast<const volatile uint32_t*>(&counter_);
  s.read_ptr = *reinterpret_cast<const volatile uint32_t*>(&read_ptr_index_);
  s.write_ptr = write_ptr_index_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace rex::graphics
