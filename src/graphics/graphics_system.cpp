/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/gpu_census.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/frame_pacing.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU",
                      "Swap post effect: none, fxaa, fxaa_extreme, smaa")
    .allowed({"none", "fxaa", "fxaa_extreme", "smaa"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  if (lowered == "smaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kSmaa;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

namespace {
int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode, paced like
  // xenia-canary's frame limiter: on Windows, missed vblanks are dropped rather
  // than delivered in a burst, and 90% of the period is slept with the rest
  // spun; elsewhere, paced to a deadline, giving up beats more than
  // kMaxVblankCatchUp late.
  // With host_vblank_pacing, each vblank also latches the newest guest frame
  // for presentation, and is phase-locked to the display's vblank when that
  // is known (see Presenter::IsHostVblankPacingEnabled).
#if REX_PLATFORM_WIN32
  constexpr int32_t kVsyncWorkerTimerPriority = rex::thread::ThreadPriority::kLowest;
#else
  constexpr int32_t kVsyncWorkerTimerPriority = rex::thread::ThreadPriority::kNormal;
#endif

  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        const double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        const uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        const uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
#if REX_PLATFORM_WIN32
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
#else
        using steady_time_point = std::chrono::steady_clock::time_point;
        steady_time_point next_vblank_deadline{};
        constexpr auto kMaxVblankCatchUp = std::chrono::seconds(1);
#endif
        const bool host_vblank_pacing =
            presenter_ && ui::Presenter::IsHostVblankPacingEnabled();
        int64_t last_guest_vblank_ns = 0;
        while (vsync_worker_running_) {
          if (!REXCVAR_GET(vsync)) {
            // No vsync: as many vblanks as the guest can take.
            MarkVblank();
            rex::thread::Sleep(std::chrono::milliseconds(1));
            continue;
          }
          if (host_vblank_pacing) {
            const int64_t now_ns = SteadyNowNs();
            int64_t target_ns;
            ui::Presenter::HostVblankGrid grid;
            if (presenter_->GetHostVblankGrid(grid) && grid.period_ns > 0) {
              // The host vblank about one guest period after the last guest
              // vblank (or the next one after a stall), plus a margin so the
              // frame latched now is presented for the host vblank after it.
              constexpr int64_t kLatchDelayNs = 1'000'000;
              const int64_t base_ns = grid.vblank_ns + kLatchDelayNs;
              int64_t earliest_ns = now_ns;
              if (last_guest_vblank_ns) {
                earliest_ns = std::max(earliest_ns, last_guest_vblank_ns +
                                                        int64_t(grid.divisor) * grid.period_ns -
                                                        grid.period_ns / 2);
              }
              const int64_t vblanks =
                  (earliest_ns - base_ns + grid.period_ns - 1) / grid.period_ns;
              target_ns = base_ns + vblanks * grid.period_ns;
            } else {
              // Variable refresh rate (or no lock yet): evenly timed vblanks,
              // with presents following them.
              const int64_t period_ns = int64_t(
                  1000000000.0 / (refresh_rate_hz * chrono::Clock::guest_time_scalar()));
              target_ns = last_guest_vblank_ns ? last_guest_vblank_ns + period_ns : now_ns;
              if (target_ns < now_ns - period_ns) {
                target_ns = now_ns;  // Drop the missed beats after a stall.
              }
            }
            // Sleep most of the way, spin the rest.
            for (int64_t remaining_ns; vsync_worker_running_ &&
                                       (remaining_ns = target_ns - SteadyNowNs()) > 0;) {
              if (remaining_ns > 2'000'000) {
                rex::thread::Sleep(std::chrono::microseconds((remaining_ns - 1'500'000) / 1000));
              } else {
                rex::thread::MaybeYield();
              }
            }
            last_guest_vblank_ns = target_ns;
            presenter_->OnGuestVblankLatch();
            MarkVblank();
#if REX_PLATFORM_WIN32
            last_frame_time = chrono::Clock::QueryGuestTickCount();
#endif
            continue;
          }
          // Real time of one vblank period at the current guest speed.
          const double period_ns =
              1000000000.0 / (refresh_rate_hz * chrono::Clock::guest_time_scalar());
#if REX_PLATFORM_WIN32
          const uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          if (current_time - last_frame_time >= vsync_interval_ticks) {
            // Drop the missed beats: a burst of vblanks after a stall would make
            // the title fast-forward through several frames at once.
            last_frame_time = current_time;
            MarkVblank();
            // Sleep for 90% of the period, spin for the rest.
            rex::thread::Sleep(std::chrono::microseconds(int64_t(period_ns * 0.9 / 1000.0)));
          }
#else
          MarkVblank();
          const auto period = std::chrono::nanoseconds(std::max(int64_t(1), int64_t(period_ns)));
          auto now = std::chrono::steady_clock::now();
          // A pause is not missed beats: start again from now after one.
          if (vblank_deadline_reset_.exchange(false) ||
              next_vblank_deadline == steady_time_point()) {
            next_vblank_deadline = now;
          }
          next_vblank_deadline += period;
          if (next_vblank_deadline > now) {
            rex::thread::Sleep(
                std::chrono::duration_cast<std::chrono::microseconds>(next_vblank_deadline - now));
          } else if (now - next_vblank_deadline > kMaxVblankCatchUp) {
            // Further behind than catching up could hide: give the missed
            // beats up rather than running the guest fast through what was a
            // real freeze.
            next_vblank_deadline = now + period;
            rex::thread::Sleep(std::chrono::duration_cast<std::chrono::microseconds>(period));
          }
          // Otherwise loop straight round without sleeping, delivering the
          // missed beats until the guest has had as many as real time says it
          // should.
#endif
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();
  // The Windows branch spins for part of each period, which is what the lowest
  // priority is for. The deadline branch only sleeps, so there it would only
  // add wake-up latency.
  // With host vblank pacing, precise timing matters more than the CPU time of
  // the short spin.
  vsync_worker_thread_->thread()->set_priority(
      presenter_ && ui::Presenter::IsHostVblankPacingEnabled()
          ? rex::thread::ThreadPriority::kAboveNormal
          : kVsyncWorkerTimerPriority);

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  // Stop the vblank thread first: MarkVblank calls into the command processor,
  // which must not be destroyed under it. A command processor waiting in
  // WAIT_REG_MEM still ends: the waits time out and check worker_running_.
  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  if (gpu_census::Enabled()) {
    // The CPU's MMIO reads of GPU registers (rex/graphics/gpu_census.h).
    gpu_census::AddShared(gpu_census::Get().mmio_reads);
    gpu_census::CountRow(gpu_census::RowKind::kMmioRead, r);
  }

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x0F2D:  // RB_HSIO_INTERFACE_ALIGNER_VALUE
      return 0x00BBBBBB;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  if (gpu_census::Enabled()) {
    // The CPU's MMIO writes to GPU registers (rex/graphics/gpu_census.h).
    gpu_census::AddShared(gpu_census::Get().mmio_writes);
    if (gpu_census::Row* row = gpu_census::CountRow(gpu_census::RowKind::kMmioWrite, r)) {
      row->last0.store(value, std::memory_order_relaxed);
    }
  }

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x0F01:  // RB_BC_CONTROL
      break;
    case 0x0F2D:  // RB_HSIO_INTERFACE_ALIGNER_VALUE
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  // Recorded whether or not the census is on yet (the title sets this up once, at boot).
  gpu_census::Census& census = gpu_census::Get();
  gpu_census::AddShared(census.set_interrupt_callback);
  census.interrupt_callback.store(callback, std::memory_order_relaxed);
  census.interrupt_callback_data.store(user_data, std::memory_order_relaxed);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  const bool census = gpu_census::Enabled();
  if (!interrupt_callback_) {
    if (census) {
      gpu_census::AddShared(gpu_census::Get().interrupts_no_callback);
    }
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  const auto census_start =
      census ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
  if (census) {
    // By CPU and source, with the time spent in the title's handler.
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - census_start)
                                     .count());
    gpu_census::AddShared(gpu_census::Get().interrupts[source ? 1 : 0]);
    if (gpu_census::Row* row =
            gpu_census::CountRow(gpu_census::RowKind::kInterrupt, cpu, source, us)) {
      gpu_census::MaxShared(row->max, us);
    }
  }
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");
  ui::frame_pacing::Record(ui::frame_pacing::Event::kGuestVblank);
  if (gpu_census::Enabled()) {
    gpu_census::AddShared(gpu_census::Get().vblanks);
  }

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
  if (command_processor_) {
    command_processor_->OnVblankDispatched();
  }
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking,
                                             std::function<void()> completion_callback) {
  if (!REXCVAR_GET(store_shaders)) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true,
                                                  std::move(completion_callback));
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread(
          [this, cache_root, title_id, &fence,
           completion_callback = std::move(completion_callback)]() mutable {
            command_processor_->InitializeShaderStorage(cache_root, title_id, true,
                                                        std::move(completion_callback));
            fence.Signal();
          });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread(
        [this, cache_root, title_id,
         completion_callback = std::move(completion_callback)]() mutable {
          command_processor_->InitializeShaderStorage(cache_root, title_id, false,
                                                      std::move(completion_callback));
        });
  }
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  vblank_deadline_reset_ = true;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
