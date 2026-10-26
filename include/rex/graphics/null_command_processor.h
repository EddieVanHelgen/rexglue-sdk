/**
 * Null command processor (graphics_backend = "null", rex/graphics/null_gpu.h).
 *
 * The base CommandProcessor unchanged - the ring and indirect buffers, the register file with the
 * scratch write-back, COHER_STATUS_HOST, the DC_LUT gamma decode, WAIT_REG_MEM (vblank-paced
 * sleeps), INTERRUPT (the guest's interrupt handler on this thread), EVENT_WRITE / _SHD / _EXT /
 * _ZPD (fake), MEM_WRITE, COND_WRITE, REG_RMW, REG_TO_MEM, the read-pointer write-back and XE_SWAP -
 * with no host GPU work: draws and resolves (copies) are counted and dropped, shader loads get a
 * placeholder (the IM_LOAD handlers store a non-null shader; nothing is translated), and the swap
 * hands nothing to the presenter (a renderer in the title presents). IssueSwap keeps what the
 * D3D12 backend publishes for such a renderer: the swap index (resolve_readback::NoteSwap) and the
 * 256-entry gamma table (gamma_mirror). Occlusion queries are always fake (no host queries).
 *
 * For hang diagnosis the overrides also count what the command processor did (bursts, swaps, the
 * InsertCallback sequences of scratch registers, INTERRUPT and CALLBACK_ACK, the vblank-paced
 * waits) and log the first of each ("null_gpu:" lines); GetStatus hands the counters to the null
 * graphics system's monitor thread (d3d12/null_graphics_system.h).
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include <rex/graphics/command_processor.h>

namespace rex::graphics {

class NullCommandProcessor : public CommandProcessor {
 public:
  NullCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NullCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  // What the command processor did so far and what it is doing now (any thread; the counters are
  // written by the command processor thread only). Times are steady_clock microseconds, 0 = never.
  struct Status {
    uint64_t setup_us = 0;           // SetupContext: the command processor thread is up
    uint64_t bursts = 0;             // ring bursts executed (OnPrimaryBufferEnd)
    uint64_t last_burst_end_us = 0;
    uint64_t swaps = 0;              // XE_SWAP packets (IssueSwap)
    uint64_t last_swap_us = 0;
    uint64_t draws = 0;              // dropped draws
    uint64_t copies = 0;             // dropped resolves
    uint64_t shader_loads = 0;       // IM_LOAD / IM_LOAD_IMMEDIATE (a placeholder returned)
    uint64_t idle_waits = 0;         // PrepareForWait with the ring empty (the idle loop)
    uint64_t reg_waits = 0;          // PrepareForWait with ring work left: a vblank-paced WAIT_REG_MEM sleep
    bool in_wait = false;            // between PrepareForWait and ReturnFromWait
    uint64_t wait_since_us = 0;      // the current (or last) PrepareForWait
    uint64_t callbacks_begun = 0;    // CALLBACK_ADDRESS (SCRATCH_REG4) written with a callback
    uint64_t callbacks_acked = 0;    // CALLBACK_ACK: the INTERRUPT before it ran the guest's handler (source 1)
    uint64_t callbacks_done = 0;     // CALLBACK_ADDRESS = 0x0BADF00D: the wait for the CPU-mask clear passed
    uint32_t last_callback = 0;
    uint32_t last_callback_context = 0;
    uint64_t gates_armed = 0;        // SCRATCH_REG1 writes (the Swap's presentation-interval gate)
    uint64_t scratch_writebacks = 0; // scratch registers written back to SCRATCH_ADDR (SCRATCH_UMSK)
    uint32_t vblank_counter = 0;     // the vblank counter (MarkVblank)
    uint32_t read_ptr = 0;           // ring read pointer (dwords; updated at the end of each burst)
    uint32_t write_ptr = 0;          // ring write pointer (CP_RB_WPTR)
  };
  Status GetStatus() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void PrepareForWait() override;
  void ReturnFromWait() override;
  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  // Single-writer counter (the command processor thread): no locked read-modify-write.
  static void Bump(std::atomic<uint64_t>& counter) {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  static uint64_t NowUs();
  void NoteCallbackAddress(uint32_t value);

  // The placeholders LoadShader hands out (vertex, pixel); never translated.
  std::unique_ptr<Shader> placeholder_shaders_[2];

  // IssueSwap's swap index (command processor thread), as the D3D12 backend counts it.
  uint64_t swap_index_ = 0;

  std::atomic<uint64_t> setup_us_{0};
  std::atomic<uint64_t> bursts_{0};
  std::atomic<uint64_t> last_burst_end_us_{0};
  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> last_swap_us_{0};
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint64_t> shader_loads_{0};
  std::atomic<uint64_t> idle_waits_{0};
  std::atomic<uint64_t> reg_waits_{0};
  std::atomic<bool> in_wait_{false};
  std::atomic<uint64_t> wait_since_us_{0};
  std::atomic<uint64_t> callbacks_begun_{0};
  std::atomic<uint64_t> callbacks_acked_{0};
  std::atomic<uint64_t> callbacks_done_{0};
  std::atomic<uint32_t> last_callback_{0};
  std::atomic<uint32_t> last_callback_context_{0};
  std::atomic<uint64_t> gates_armed_{0};
  std::atomic<uint64_t> scratch_writebacks_{0};

  // First-event log lines and the distinct InsertCallback callbacks (command processor thread).
  bool logged_first_burst_ = false;
  bool logged_first_swap_ = false;
  bool logged_first_draw_ = false;
  bool logged_first_copy_ = false;
  bool logged_first_shader_ = false;
  bool logged_first_reg_wait_ = false;
  bool logged_first_gate_ = false;
  bool logged_first_ack_ = false;
  bool logged_first_done_ = false;
  bool logged_capture_ = false;
  static constexpr size_t kLoggedCallbacks = 8;
  std::array<uint32_t, kLoggedCallbacks> logged_callbacks_{};
  size_t logged_callback_count_ = 0;
};

}  // namespace rex::graphics
