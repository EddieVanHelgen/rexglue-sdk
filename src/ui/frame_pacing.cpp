/**
 * Frame pacing trace (diagnostic). See rex/ui/frame_pacing.h.
 */

#include <rex/ui/frame_pacing.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>
#endif

REXCVAR_DEFINE_BOOL(frame_pacing_log, false, "UI/Presenter",
                    "Write guest vblank/swap and host present/vblank timestamps to "
                    "logs/frame_pacing.csv next to the executable");

namespace rex::ui::frame_pacing {

namespace {

class Trace {
 public:
  static Trace& Get() {
    static Trace trace;
    return trace;
  }

  void Record(Event event, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
              uint64_t f) {
    const auto now = std::chrono::steady_clock::now();
    std::call_once(open_once_, [this]() { Open(); });
    // By the first present every cvar is registered and both config passes
    // have run, so this shows the values actually in effect.
    if (event == Event::kHostPresent && !config_dumped_.exchange(true)) {
      DumpConfig();
    }
    const double t_us = std::chrono::duration<double, std::micro>(now - start_).count();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
      return;
    }
    std::fprintf(file_, "%c,%.1f,%llu,%llu,%llu,%llu,%llu,%llu\n", char(event), t_us,
                 (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                 (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
  }

  // Runs at process exit, after the vblank thread is gone (it's detached, as
  // joining under the loader lock could deadlock).
  ~Trace() {
    running_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

 private:
  void Open() {
    start_ = std::chrono::steady_clock::now();
    std::error_code ec;
    const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "logs";
    std::filesystem::create_directories(dir, ec);
    file_ = std::fopen((dir / "frame_pacing.csv").string().c_str(), "w");
    if (!file_) {
      return;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    std::fprintf(file_, "type,t_us,a,b,c,d,e,f\n");
    running_ = true;
    std::thread([this]() { HostVblankThread(); }).detach();
  }

  void DumpConfig() {
    static constexpr const char* kNames[] = {
        "vsync", "pdz_fps60_unlock", "pdz_gpu_wait_mode", "graphics_backend",
        "render_target_path_d3d12", "resolution_scale", "video_mode_refresh_rate",
        "d3d12_allow_variable_refresh_rate_and_tearing", "host_present_from_non_ui_thread",
        "d3d12_submit_on_primary_buffer_end", "framerate_limit", "host_vblank_pacing"};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
      return;
    }
    for (const char* name : kNames) {
      std::fprintf(file_, "C,0,%s=%s,%d\n", name, rex::cvar::GetFlagByName(name).c_str(),
                   int(rex::cvar::GetFlagSource(name)));
    }
  }

  // Timestamps the real display vblanks of the primary monitor and flushes the
  // file about once a second.
  void HostVblankThread() {
#if REX_PLATFORM_WIN32
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      for (UINT ai = 0; !output && SUCCEEDED(factory->EnumAdapters1(ai, &adapter)); ++ai) {
        Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
        for (UINT oi = 0; SUCCEEDED(adapter->EnumOutputs(oi, &candidate)); ++oi) {
          DXGI_OUTPUT_DESC desc;
          if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == primary) {
            output = candidate;
            break;
          }
        }
      }
    }
    auto last_flush = std::chrono::steady_clock::now();
    uint64_t count = 0;
    while (running_) {
      if (!output || FAILED(output->WaitForVBlank())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      } else {
        Record(Event::kHostVblank, ++count, 0, 0, 0, 0, 0);
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last_flush > std::chrono::seconds(1)) {
        last_flush = now;
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) {
          std::fflush(file_);
        }
      }
    }
#endif
  }

  std::once_flag open_once_;
  std::chrono::steady_clock::time_point start_;
  std::mutex mutex_;
  std::FILE* file_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> config_dumped_{false};
};

}  // namespace

bool Enabled() {
  return REXCVAR_GET(frame_pacing_log);
}

void Record(Event event, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
            uint64_t f) {
  if (!Enabled()) {
    return;
  }
  Trace::Get().Record(event, a, b, c, d, e, f);
}

}  // namespace rex::ui::frame_pacing
