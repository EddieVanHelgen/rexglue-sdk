/**
 * Frame pacing trace (diagnostic). See rex/ui/frame_pacing.h.
 */

#include <rex/ui/frame_pacing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
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

std::atomic<int> g_output_pairing{int(OutputPairing::kCpSwap)};

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
    std::string summary, reaction;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!file_) {
        return;
      }
      WriteRowLocked(char(event), now, a, b, c, d, e, f);
      TrackLatencyLocked(event, now, a, b, d, summary, reaction);
    }
    if (!reaction.empty()) {
      REXLOG_INFO("frame_pacing: {}", reaction);
    }
    if (!summary.empty()) {
      REXLOG_INFO("frame_pacing: {}", summary);
    }
  }

  // MarkReaction: the newest VdSwap's frame, wherever the pairing has carried it so far.
  void MarkReaction(uint32_t id, std::chrono::steady_clock::time_point change) {
    std::call_once(open_once_, [this]() { Open(); });
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || last_vdswap_ == TimePoint{}) {
      return;
    }
    const auto mark = [&](FrameLatency& frame) {
      if (frame.vdswap == last_vdswap_ && frame.present == TimePoint{}) {
        frame.change = change;
        frame.change_id = id;
      }
    };
    mark(newest_vdswap_);
    mark(cp_swap_);
    for (FrameLatency& frame : pending_swaps_) {
      mark(frame);
    }
    for (FrameLatency& frame : frames_) {
      mark(frame);
    }
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
  using TimePoint = std::chrono::steady_clock::time_point;

  // A guest frame on its way from the input read to the screen.
  struct FrameLatency {
    uint64_t seq = 0;
    TimePoint input = {};  // Newest input read before its VdSwap, or empty.
    int64_t input_age_us = 0;
    TimePoint vdswap = {};
    TimePoint present = {};
    uint64_t present_count = 0;
    bool displayed = false;
    // MarkReaction: the input change this frame is the first reaction to, or empty.
    TimePoint change = {};
    uint32_t change_id = 0;
  };
  static constexpr size_t kFrameHistory = 64;
  // A summary goes to the log about every this long (600 frames at 60 fps).
  static constexpr std::chrono::seconds kSummaryPeriod{10};

  static int64_t Us(TimePoint from, TimePoint to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
  }

  void WriteRowLocked(char type, TimePoint time, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                      uint64_t e, uint64_t f) {
    const double t_us = std::chrono::duration<double, std::micro>(time - start_).count();
    std::fprintf(file_, "%c,%.1f,%llu,%llu,%llu,%llu,%llu,%llu\n", type, t_us,
                 (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                 (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
  }

  void TrackLatencyLocked(Event event, TimePoint now, uint64_t a, uint64_t b, uint64_t d,
                          std::string& summary_out, std::string& reaction_out) {
    switch (event) {
      case Event::kInputPoll:
        last_input_ = now;
        last_input_age_us_ = int64_t(b);
        break;
      case Event::kGuestVdSwap: {
        // VdSwaps reach the command processor in order, as XE_SWAP packets.
        FrameLatency frame;
        if (last_input_ != TimePoint{} && now - last_input_ < std::chrono::milliseconds(200)) {
          frame.input = last_input_;
          frame.input_age_us = last_input_age_us_;
        }
        frame.vdswap = now;
        last_vdswap_ = now;
        newest_vdswap_ = frame;
        newest_vdswap_valid_ = true;
        pending_swaps_[pending_swap_end_++ % kPendingSwaps] = frame;
        if (pending_swap_end_ - pending_swap_begin_ > kPendingSwaps) {
          pending_swap_begin_ = pending_swap_end_ - kPendingSwaps;
        }
      } break;
      case Event::kGuestSwap:
        cp_swap_valid_ = pending_swap_begin_ < pending_swap_end_;
        if (cp_swap_valid_) {
          cp_swap_ = pending_swaps_[pending_swap_begin_++ % kPendingSwaps];
        }
        break;
      case Event::kGuestOutput: {
        FrameLatency& frame = frames_[a % kFrameHistory];
        if (g_output_pairing.load(std::memory_order_relaxed) ==
            int(OutputPairing::kNewestVdSwap)) {
          // A title renderer presenting from the guest's Swap: the newest VdSwap, once.
          frame = newest_vdswap_valid_ ? newest_vdswap_ : FrameLatency{};
          newest_vdswap_valid_ = false;
        } else {
          frame = cp_swap_valid_ ? cp_swap_ : FrameLatency{};
          cp_swap_valid_ = false;
        }
        frame.seq = a;
      } break;
      case Event::kHostPresent: {
        FrameLatency& frame = frames_[a % kFrameHistory];
        if (!a || frame.seq != a || frame.present != TimePoint{} || frame.vdswap == TimePoint{}) {
          break;
        }
        frame.present = now;
        frame.present_count = d;
        const bool has_input = frame.input != TimePoint{};
        WriteRowLocked('L', now, a, has_input ? Us(frame.input, now) : 0, Us(frame.vdswap, now),
                       frame.input_age_us, has_input ? Us(frame.input, frame.vdswap) : 0, 0);
        if (has_input) {
          input_to_present_ms_.push_back(Us(frame.input, now) / 1000.0);
          input_age_ms_.push_back(frame.input_age_us / 1000.0);
        }
        vdswap_to_present_ms_.push_back(Us(frame.vdswap, now) / 1000.0);
        if (frame.change != TimePoint{}) {
          WriteRowLocked('C', now, frame.change_id, Us(frame.change, now), Us(frame.vdswap, now), a,
                         0, 0);
          reaction_to_present_ms_.push_back(Us(frame.change, now) / 1000.0);
          if (reactions_awaiting_refresh_.size() < 16) {
            reactions_awaiting_refresh_.push_back({frame.change_id, frame.change, now});
          }
          reaction_out = fmt::format(
              "reaction #{} presented: input change -> present {:.2f} ms (VdSwap -> present {:.2f})",
              frame.change_id, Us(frame.change, now) / 1000.0, Us(frame.vdswap, now) / 1000.0);
        }
        if (last_summary_ == TimePoint{}) {
          last_summary_ = now;
        } else if (now - last_summary_ >= kSummaryPeriod) {
          last_summary_ = now;
          summary_out = SummarizeLocked();
        }
      } break;
      case Event::kHostDisplay: {
#if REX_PLATFORM_WIN32
        if (!qpc_frequency_) {
          break;
        }
        const TimePoint shown =
            qpc_origin_ + std::chrono::nanoseconds(int64_t(double(int64_t(b) - qpc_origin_ticks_) *
                                                           1e9 / double(qpc_frequency_)));
        for (FrameLatency& frame : frames_) {
          if (frame.present_count != a || frame.displayed || frame.present == TimePoint{}) {
            continue;
          }
          frame.displayed = true;
          // Statistics from another present (after a swap chain reset).
          if (shown < frame.present - std::chrono::milliseconds(1) ||
              shown > frame.present + std::chrono::milliseconds(200)) {
            break;
          }
          const bool has_input = frame.input != TimePoint{};
          WriteRowLocked('D', shown, frame.seq, has_input ? Us(frame.input, shown) : 0,
                         Us(frame.present, shown), Us(frame.vdswap, shown), 0, 0);
          if (has_input) {
            input_to_display_ms_.push_back(Us(frame.input, shown) / 1000.0);
          }
          present_to_display_ms_.push_back(Us(frame.present, shown) / 1000.0);
          if (frame.change != TimePoint{}) {
            WriteRowLocked('E', shown, frame.change_id, Us(frame.change, shown),
                           Us(frame.present, shown), frame.seq, 0, 0);
            reaction_to_display_ms_.push_back(Us(frame.change, shown) / 1000.0);
            reaction_out = fmt::format(
                "reaction #{} on screen: input change -> screen {:.2f} ms (present -> screen {:.2f})",
                frame.change_id, Us(frame.change, shown) / 1000.0,
                Us(frame.present, shown) / 1000.0);
          }
          break;
        }
#endif
      } break;
      case Event::kHostVblank:
        // The first display refresh after a reaction's present (the earliest it can be shown).
        for (auto it = reactions_awaiting_refresh_.begin(); it != reactions_awaiting_refresh_.end();) {
          if (it->present > now) {
            ++it;
            continue;
          }
          reaction_to_refresh_ms_.push_back(Us(it->change, now) / 1000.0);
          reaction_out = fmt::format(
              "reaction #{} at the next display refresh: input change -> refresh {:.2f} ms (present -> "
              "refresh {:.2f})",
              it->id, Us(it->change, now) / 1000.0, Us(it->present, now) / 1000.0);
          it = reactions_awaiting_refresh_.erase(it);
        }
        break;
      default:
        break;
    }
  }

  std::string SummarizeLocked() {
    const auto describe = [](const char* name, std::vector<double>& values) {
      if (values.empty()) {
        return fmt::format("{} n/a", name);
      }
      std::sort(values.begin(), values.end());
      double sum = 0.0;
      for (double value : values) {
        sum += value;
      }
      const std::string text = fmt::format(
          "{} {:.1f} (p95 {:.1f})", name, sum / double(values.size()),
          values[std::min(values.size() - 1, values.size() * 95 / 100)]);
      values.clear();
      return text;
    };
    // Field order: what matters most first.
    std::string text = describe("input->screen ms", input_to_display_ms_);
    text += ", " + describe("input->present", input_to_present_ms_);
    text += ", " + describe("vdswap->present", vdswap_to_present_ms_);
    text += ", " + describe("present->screen", present_to_display_ms_);
    text += ", " + describe("input state age", input_age_ms_);
    // Reactions (MarkReaction): p50 / p95 / max, kept for the whole run (few per second at most).
    const auto spread = [](const char* name, std::vector<double> values) {
      std::sort(values.begin(), values.end());
      const auto at = [&](size_t percent) {
        return values[std::min(values.size() - 1, values.size() * percent / 100)];
      };
      return fmt::format("{} p50 {:.1f} p95 {:.1f} max {:.1f} (n {})", name, at(50), at(95),
                         values.back(), values.size());
    };
    if (!reaction_to_present_ms_.empty()) {
      text += "; reaction ms: " + spread("input change->present", reaction_to_present_ms_);
      if (!reaction_to_refresh_ms_.empty()) {
        text += ", " + spread("input change->next refresh", reaction_to_refresh_ms_);
      }
      if (!reaction_to_display_ms_.empty()) {
        text += ", " + spread("input change->screen", reaction_to_display_ms_);
      }
    }
    return text;
  }

  void Open() {
    start_ = std::chrono::steady_clock::now();
#if REX_PLATFORM_WIN32
    LARGE_INTEGER frequency, ticks;
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&ticks)) {
      qpc_origin_ = std::chrono::steady_clock::now();
      qpc_origin_ticks_ = ticks.QuadPart;
      qpc_frequency_ = frequency.QuadPart;
    }
#endif
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
        "d3d12_submit_on_primary_buffer_end", "framerate_limit", "host_vblank_pacing",
        "host_vblank_late_latch_ms", "gpu_max_frames_in_flight", "present_max_frame_latency",
        "input_backend", "input_poll_on_read", "pdz_null_refresh_hz", "pdz_null_present_latency"};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
      return;
    }
    for (const char* name : kNames) {
      std::fprintf(file_, "C,0,%s=%s,%d\n", name, rex::cvar::GetFlagByName(name).c_str(),
                   int(rex::cvar::GetFlagSource(name)));
    }
    std::fprintf(file_, "C,0,output_pairing=%s,0\n",
                 g_output_pairing.load(std::memory_order_relaxed) ==
                         int(OutputPairing::kNewestVdSwap)
                     ? "newest_vdswap"
                     : "cp_swap");
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
    int immediate = 0;  // WaitForVBlank calls in a row that didn't block
    while (running_) {
      const auto before = std::chrono::steady_clock::now();
      if (!output || FAILED(output->WaitForVBlank())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      } else if (std::chrono::steady_clock::now() - before >= std::chrono::microseconds(250)) {
        immediate = 0;
        Record(Event::kHostVblank, ++count, 0, 0, 0, 0, 0);
      } else if (++immediate >= 3) {
        // A display in standby (TV off) answers at once: no vblank to record, and no busy loop
        // (PDZ log 160: 171 million rows, 5.8 GB in 200 s).
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      } else {
        Record(Event::kHostVblank, ++count, 0, 0, 0, 0, 0);  // called right at a refresh
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

  // Input latency tracking, under mutex_.
  static constexpr size_t kPendingSwaps = 8;
  TimePoint last_input_ = {};
  int64_t last_input_age_us_ = 0;
  std::array<FrameLatency, kPendingSwaps> pending_swaps_ = {};
  uint64_t pending_swap_begin_ = 0;
  uint64_t pending_swap_end_ = 0;
  FrameLatency cp_swap_ = {};
  bool cp_swap_valid_ = false;
  // OutputPairing::kNewestVdSwap: the newest VdSwap, until a published frame takes it.
  FrameLatency newest_vdswap_ = {};
  bool newest_vdswap_valid_ = false;
  std::array<FrameLatency, kFrameHistory> frames_ = {};
  std::vector<double> input_to_display_ms_;
  std::vector<double> input_to_present_ms_;
  std::vector<double> vdswap_to_present_ms_;
  std::vector<double> present_to_display_ms_;
  std::vector<double> input_age_ms_;
  std::vector<double> reaction_to_present_ms_;
  std::vector<double> reaction_to_display_ms_;
  std::vector<double> reaction_to_refresh_ms_;
  struct AwaitingRefresh {
    uint32_t id;
    TimePoint change;
    TimePoint present;
  };
  std::vector<AwaitingRefresh> reactions_awaiting_refresh_;
  TimePoint last_vdswap_ = {};
  TimePoint last_summary_ = {};
#if REX_PLATFORM_WIN32
  TimePoint qpc_origin_ = {};
  int64_t qpc_origin_ticks_ = 0;
  int64_t qpc_frequency_ = 0;
#endif
};

}  // namespace

namespace {

// GetTotals: written by the command processor thread (the only writer of 'K' and 'T' rows).
struct TotalsCounters {
  std::atomic<uint64_t> cp_frames{0}, cp_wall_us{0}, cp_idle_us{0}, cp_wait_reg_us{0},
      cp_gpu_await_us{0};
  std::atomic<uint64_t> gpu_frames{0}, gpu_busy_us{0}, gpu_submissions{0};
};
TotalsCounters g_totals;

}  // namespace

bool Enabled() {
  return REXCVAR_GET(frame_pacing_log);
}

void SetOutputPairing(OutputPairing pairing) {
  g_output_pairing.store(int(pairing), std::memory_order_relaxed);
}

void Record(Event event, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
            uint64_t f) {
  if (!Enabled()) {
    return;
  }
  if (event == Event::kCpFrame) {
    g_totals.cp_frames.fetch_add(1, std::memory_order_relaxed);
    g_totals.cp_wall_us.fetch_add(a, std::memory_order_relaxed);
    g_totals.cp_idle_us.fetch_add(b, std::memory_order_relaxed);
    g_totals.cp_wait_reg_us.fetch_add(c, std::memory_order_relaxed);
    g_totals.cp_gpu_await_us.fetch_add(d, std::memory_order_relaxed);
  } else if (event == Event::kGpuFrame) {
    g_totals.gpu_frames.fetch_add(1, std::memory_order_relaxed);
    g_totals.gpu_busy_us.fetch_add(b, std::memory_order_relaxed);
    g_totals.gpu_submissions.fetch_add(d, std::memory_order_relaxed);
  }
  Trace::Get().Record(event, a, b, c, d, e, f);
}

void MarkReaction(uint32_t id, std::chrono::steady_clock::time_point change) {
  if (!Enabled()) {
    return;
  }
  Trace::Get().MarkReaction(id, change);
}

void GetTotals(Totals& out) {
  out.cp_frames = g_totals.cp_frames.load(std::memory_order_relaxed);
  out.cp_wall_us = g_totals.cp_wall_us.load(std::memory_order_relaxed);
  out.cp_idle_us = g_totals.cp_idle_us.load(std::memory_order_relaxed);
  out.cp_wait_reg_us = g_totals.cp_wait_reg_us.load(std::memory_order_relaxed);
  out.cp_gpu_await_us = g_totals.cp_gpu_await_us.load(std::memory_order_relaxed);
  out.gpu_frames = g_totals.gpu_frames.load(std::memory_order_relaxed);
  out.gpu_busy_us = g_totals.gpu_busy_us.load(std::memory_order_relaxed);
  out.gpu_submissions = g_totals.gpu_submissions.load(std::memory_order_relaxed);
}

}  // namespace rex::ui::frame_pacing
