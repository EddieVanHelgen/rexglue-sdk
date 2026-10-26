/**
 * Null GPU backend (see include/rex/graphics/d3d12/null_graphics_system.h).
 */

#include <rex/graphics/d3d12/null_graphics_system.h>

#include <algorithm>
#include <chrono>

#include <fmt/format.h>

#include <rex/graphics/flags.h>
#include <rex/graphics/null_command_processor.h>
#include <rex/graphics/null_gpu.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

namespace rex::graphics::d3d12 {

namespace {

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Monitor cadence: a heartbeat every second for the first 20 s (boot, first ring init, first
// frames), every 10 s after; stall checks every 250 ms, a stall line repeated every 5 s.
constexpr uint64_t kTickMs = 250;
constexpr uint64_t kFastBeatMs = 1000;
constexpr uint64_t kFastBeatUntilMs = 20000;
constexpr uint64_t kSlowBeatMs = 10000;
constexpr uint64_t kVblankStallMs = 1000;
constexpr uint64_t kRingStallMs = 2000;
// A level load can hold the Swap for about 5 s (Datacore's first level frame took 4.5 s).
constexpr uint64_t kSwapStallMs = 3000;
constexpr uint64_t kRepeatMs = 5000;

uint32_t Volatile32(const uint32_t& value) {
  return *reinterpret_cast<const volatile uint32_t*>(&value);
}

}  // namespace

D3D12NullGraphicsSystem::D3D12NullGraphicsSystem() {
  null_gpu::SetActive(true);
  REXGPU_INFO(
      "null_gpu: graphics_backend null: the GPU plugin consumes the PM4 ring and services the "
      "synchronisation (fences, scratch write-back, WAIT_REG_MEM, interrupts, read-pointer "
      "write-back, vblank) but executes no draws, resolves or shaders; the title's renderer is the "
      "only output");
}

D3D12NullGraphicsSystem::~D3D12NullGraphicsSystem() {
  // Shutdown normally stopped it; never leave it reading a destroyed command processor.
  if (monitor_thread_) {
    monitor_running_ = false;
    monitor_stop_->Set();
    rex::thread::Wait(monitor_thread_.get(), false);
    monitor_thread_.reset();
  }
  null_gpu::SetActive(false);
}

std::string D3D12NullGraphicsSystem::name() const {
  // Not D3D12GraphicsSystem::name(): that casts the command processor to D3D12CommandProcessor.
  return "Direct3D 12 - null GPU (no emulated rendering)";
}

std::unique_ptr<CommandProcessor> D3D12NullGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new NullCommandProcessor(this, kernel_state_));
}

NullCommandProcessor* D3D12NullGraphicsSystem::null_command_processor() const {
  // Always ours (CreateCommandProcessor).
  return static_cast<NullCommandProcessor*>(command_processor());
}

X_STATUS D3D12NullGraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                                system::KernelState* kernel_state) {
  start_us_ = NowUs();
  const X_STATUS status = D3D12GraphicsSystem::SetupGuestGpu(function_dispatcher, kernel_state);
  if (XFAILED(status)) {
    return status;
  }
  ui::Presenter* presenter_object = presenter();
  REXGPU_INFO(
      "null_gpu: guest GPU set up: vsync {}, host_vblank_pacing {} (the vsync worker latches the "
      "presenter's newest frame at each guest vblank), presenter {}; the monitor logs a heartbeat "
      "every second for 20 s, then every 10 s",
      REXCVAR_GET(vsync), ui::Presenter::IsHostVblankPacingEnabled(),
      presenter_object ? "present" : "MISSING (headless)");
  monitor_stop_ = rex::thread::Event::CreateManualResetEvent(false);
  monitor_running_ = true;
  rex::thread::Thread::CreationParameters params;
  params.stack_size = 256 * 1024;
  monitor_thread_ = rex::thread::Thread::Create(params, [this]() { MonitorThreadMain(); });
  if (monitor_thread_) {
    monitor_thread_->set_name("GPU Null Monitor");
    monitor_thread_->set_priority(rex::thread::ThreadPriority::kBelowNormal);
  } else {
    monitor_running_ = false;
    REXGPU_WARN("null_gpu: the monitor thread could not be created (no heartbeat / stall lines)");
  }
  return status;
}

void D3D12NullGraphicsSystem::Shutdown() {
  if (monitor_thread_) {
    monitor_running_ = false;
    monitor_stop_->Set();
    rex::thread::Wait(monitor_thread_.get(), false);
    monitor_thread_.reset();
  }
  if (NullCommandProcessor* cp = null_command_processor()) {
    const NullCommandProcessor::Status s = cp->GetStatus();
    const double swaps = s.swaps ? double(s.swaps) : 1.0;
    REXGPU_INFO(
        "null_gpu: totals at shutdown ({:.0f} s): vblanks {}, XE_SWAP {}, ring bursts {}, dropped "
        "draws {} resolves {}, shader loads {} (placeholders), InsertCallback sequences begun {} / "
        "interrupts acked {} / done {}, gates armed {}, scratch write-backs {}, waits: idle {} / "
        "WAIT_REG_MEM sleeps {} | per XE_SWAP: vblanks {:.2f}, interrupts {:.3f} (M0 with the "
        "plugin: 1.00 + 1 per InsertCallback), gates {:.3f}, scratch write-backs {:.2f} (M0: 5.00)",
        double(NowUs() - start_us_) / 1000000.0, s.vblank_counter, s.swaps, s.bursts, s.draws,
        s.copies, s.shader_loads, s.callbacks_begun, s.callbacks_acked, s.callbacks_done,
        s.gates_armed, s.scratch_writebacks, s.idle_waits, s.reg_waits,
        double(s.vblank_counter) / swaps, double(s.callbacks_acked) / swaps,
        double(s.gates_armed) / swaps, double(s.scratch_writebacks) / swaps);
  }
  D3D12GraphicsSystem::Shutdown();
}

void D3D12NullGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  REXGPU_INFO("null_gpu: ring buffer at {:08X}, {} dwords ({:.1f} ms after the guest GPU setup)",
              ptr, (uint32_t(1) << (size_log2 + 3)) / 4,
              double(NowUs() - start_us_) / 1000.0);
  D3D12GraphicsSystem::InitializeRingBuffer(ptr, size_log2);
}

void D3D12NullGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  REXGPU_INFO("null_gpu: read-pointer write-back to {:08X}, block size log2 {}", ptr,
              block_size_log2);
  D3D12GraphicsSystem::EnableReadPointerWriteBack(ptr, block_size_log2);
}

std::string D3D12NullGraphicsSystem::SyncState() {
  const RegisterFile* regs = register_file();
  auto reg = [regs](uint32_t index) { return Volatile32(regs->values[index]); };
  std::string s = fmt::format(
      "scratch registers 0-7 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} (UMSK {:02X}, "
      "ADDR {:08X})",
      reg(XE_GPU_REG_SCRATCH_REG0), reg(XE_GPU_REG_SCRATCH_REG1), reg(XE_GPU_REG_SCRATCH_REG2),
      reg(XE_GPU_REG_SCRATCH_REG3), reg(XE_GPU_REG_CALLBACK_ADDRESS),
      reg(XE_GPU_REG_CALLBACK_CONTEXT), reg(XE_GPU_REG_SCRATCH_REG6), reg(XE_GPU_REG_SCRATCH_REG7),
      reg(XE_GPU_REG_SCRATCH_UMSK), reg(XE_GPU_REG_SCRATCH_ADDR));
  const uint32_t scratch_address = reg(XE_GPU_REG_SCRATCH_ADDR);
  if (scratch_address && memory_) {
    // Xbox 360 D3D's scratch block: +0 the CPU mask of a pending callback (the interrupt handler
    // clears its bit), +4 the presentation-interval gate (the vblank handler stores 0), +16 / +20
    // the callback and its context.
    const uint8_t* block = memory_->TranslatePhysical(scratch_address);
    s += fmt::format(" | scratch block in memory: +0 {:08X}, +4 {:08X}, +16 {:08X}, +20 {:08X}",
                     rex::memory::load_and_swap<uint32_t>(block),
                     rex::memory::load_and_swap<uint32_t>(block + 4),
                     rex::memory::load_and_swap<uint32_t>(block + 16),
                     rex::memory::load_and_swap<uint32_t>(block + 20));
  }
  s += fmt::format(" | COHER_STATUS_HOST {:08X} | interrupt callback {:08X} data {:08X}{}",
                   reg(XE_GPU_REG_COHER_STATUS_HOST), Volatile32(interrupt_callback_),
                   Volatile32(interrupt_callback_data_), is_paused() ? " | PAUSED" : "");
  return s;
}

void D3D12NullGraphicsSystem::MonitorThreadMain() {
  NullCommandProcessor* cp = null_command_processor();
  if (!cp) {
    return;
  }
  using Status = NullCommandProcessor::Status;
  Status last_beat = cp->GetStatus();
  uint64_t last_beat_ms = 0;
  uint64_t next_beat_ms = kFastBeatMs;
  // Stall tracking: the value last seen, when it last changed, when the stall was last reported
  // (0 = not reported).
  uint32_t vblank_seen = last_beat.vblank_counter;
  uint64_t vblank_change_ms = 0, vblank_reported_ms = 0;
  uint64_t bursts_seen = last_beat.bursts, reg_waits_at_burst = last_beat.reg_waits;
  uint64_t burst_change_ms = 0, ring_reported_ms = 0;
  uint64_t swaps_seen = last_beat.swaps;
  uint64_t swap_change_ms = 0, swap_reported_ms = 0;

  auto cp_now = [](const Status& s, uint64_t now_us, bool ring_work) {
    const uint64_t wait_ms = s.wait_since_us && now_us > s.wait_since_us
                                 ? (now_us - s.wait_since_us) / 1000
                                 : 0;
    const uint64_t burst_ms = s.last_burst_end_us && now_us > s.last_burst_end_us
                                  ? (now_us - s.last_burst_end_us) / 1000
                                  : 0;
    if (s.in_wait && !ring_work) {
      return fmt::format("CP idle, ring empty, for {} ms", wait_ms);
    }
    if (s.in_wait) {
      return fmt::format("CP sleeping in a vblank-paced WAIT_REG_MEM (this sleep {} ms; last burst "
                         "ended {} ms ago)",
                         wait_ms, burst_ms);
    }
    if (ring_work) {
      return fmt::format("CP executing a ring burst (last burst ended {} ms ago)", burst_ms);
    }
    return std::string("CP between bursts");
  };
  auto cp_state = [&](const Status& s, uint64_t now_us, bool ring_work) {
    const uint64_t swap_ms =
        s.last_swap_us && now_us > s.last_swap_us ? (now_us - s.last_swap_us) / 1000 : 0;
    return fmt::format(
        "{} | ring rptr {:X} wptr {:X} | bursts {} | XE_SWAP {} (last {} ms ago) | InsertCallback "
        "sequences begun {} / interrupts acked {} / done {} (last callback {:08X} context {:08X}) | "
        "gates armed {} | vblanks {} | waits: idle {} / WAIT_REG_MEM sleeps {} | {}",
        cp_now(s, now_us, ring_work), s.read_ptr, s.write_ptr, s.bursts, s.swaps, swap_ms,
        s.callbacks_begun, s.callbacks_acked, s.callbacks_done, s.last_callback,
        s.last_callback_context, s.gates_armed, s.vblank_counter, s.idle_waits, s.reg_waits,
        SyncState());
  };

  while (monitor_running_.load()) {
    if (rex::thread::Wait(monitor_stop_.get(), false, std::chrono::milliseconds(kTickMs)) ==
        rex::thread::WaitResult::kSuccess) {
      break;
    }
    const uint64_t now_us = NowUs();
    const uint64_t t_ms = (now_us - start_us_) / 1000;
    const Status s = cp->GetStatus();
    const bool ring_work = s.write_ptr != 0xBAADF00D && s.read_ptr != s.write_ptr;

    // Vblanks: the vsync worker's MarkVblank (counter, interrupt source 0, the presenter latch).
    if (s.vblank_counter != vblank_seen) {
      if (vblank_reported_ms) {
        REXGPU_INFO("null_gpu: vblanks resumed (stopped for at least {} ms)",
                    vblank_reported_ms - vblank_change_ms);
        vblank_reported_ms = 0;
      }
      vblank_seen = s.vblank_counter;
      vblank_change_ms = t_ms;
    } else if (s.vblank_counter && t_ms - vblank_change_ms >= kVblankStallMs &&
               (!vblank_reported_ms || t_ms - vblank_reported_ms >= kRepeatMs)) {
      vblank_reported_ms = t_ms;
      REXGPU_WARN(
          "null_gpu: STALL: no vblank for {} ms (the vsync worker is not delivering guest vblanks: "
          "the guest's vblank interrupt handler blocked?) | {}",
          t_ms - vblank_change_ms, cp_state(s, now_us, ring_work));
    }

    // The ring: work pending and no burst end.
    if (s.bursts != bursts_seen || !ring_work) {
      if (ring_reported_ms) {
        REXGPU_INFO("null_gpu: the command processor moved on (stuck for at least {} ms)",
                    ring_reported_ms - burst_change_ms);
        ring_reported_ms = 0;
      }
      bursts_seen = s.bursts;
      reg_waits_at_burst = s.reg_waits;
      burst_change_ms = t_ms;
    } else if (t_ms - burst_change_ms >= kRingStallMs &&
               (!ring_reported_ms || t_ms - ring_reported_ms >= kRepeatMs)) {
      ring_reported_ms = t_ms;
      REXGPU_WARN(
          "null_gpu: STALL: ring work pending and no burst finished for {} ms ({}) | {}",
          t_ms - burst_change_ms,
          s.reg_waits != reg_waits_at_burst
              ? "WAIT_REG_MEM sleeps keep repeating: a memory or register poll that never matches "
                "(see the scratch block)"
              : "no vblank-paced sleep: a short-wait WAIT_REG_MEM spin or a packet that does not "
                "return",
          cp_state(s, now_us, ring_work));
    }

    // Swaps.
    if (s.swaps != swaps_seen) {
      if (swap_reported_ms) {
        REXGPU_INFO("null_gpu: XE_SWAP resumed (the gap lasted at least {} ms)",
                    swap_reported_ms - swap_change_ms);
        swap_reported_ms = 0;
      }
      swaps_seen = s.swaps;
      swap_change_ms = t_ms;
    } else if (s.swaps && t_ms - swap_change_ms >= kSwapStallMs &&
               (!swap_reported_ms || t_ms - swap_reported_ms >= kRepeatMs)) {
      swap_reported_ms = t_ms;
      REXGPU_WARN(
          "null_gpu: no XE_SWAP for {} ms (a level load can take about 5 s; longer is a hang: the "
          "guest submits no Swap, or the command processor has not reached it) | {}",
          t_ms - swap_change_ms, cp_state(s, now_us, ring_work));
    }

    // Heartbeat.
    if (t_ms >= next_beat_ms) {
      const double seconds = double(std::max<uint64_t>(t_ms - last_beat_ms, 1)) / 1000.0;
      REXGPU_INFO(
          "null_gpu: t={}s | vblanks {} ({:.1f}/s) | XE_SWAP {} ({:.1f}/s) | ring bursts +{} | "
          "dropped draws +{} resolves +{} | shader loads +{} | InsertCallback sequences begun +{} "
          "interrupts acked +{} done +{} | gates armed +{} | waits: idle +{} WAIT_REG_MEM sleeps "
          "+{} | scratch write-backs +{} | {}",
          t_ms / 1000, s.vblank_counter,
          double(uint32_t(s.vblank_counter - last_beat.vblank_counter)) / seconds, s.swaps,
          double(s.swaps - last_beat.swaps) / seconds, s.bursts - last_beat.bursts,
          s.draws - last_beat.draws, s.copies - last_beat.copies,
          s.shader_loads - last_beat.shader_loads, s.callbacks_begun - last_beat.callbacks_begun,
          s.callbacks_acked - last_beat.callbacks_acked,
          s.callbacks_done - last_beat.callbacks_done, s.gates_armed - last_beat.gates_armed,
          s.idle_waits - last_beat.idle_waits, s.reg_waits - last_beat.reg_waits,
          s.scratch_writebacks - last_beat.scratch_writebacks, cp_now(s, now_us, ring_work));
      last_beat = s;
      last_beat_ms = t_ms;
      next_beat_ms = t_ms + (t_ms < kFastBeatUntilMs ? kFastBeatMs : kSlowBeatMs);
    }
  }
}

}  // namespace rex::graphics::d3d12
