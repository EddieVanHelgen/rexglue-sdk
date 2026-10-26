/**
 * Null GPU backend (graphics_backend = "null", rex/graphics/null_gpu.h).
 *
 * The D3D12 graphics system - provider, presenter (overlay, FSR, letterbox, capture), the MMIO
 * range, ReadRegister / WriteRegister, the vsync worker with host vblank pacing and the presenter's
 * vblank latch, interrupt dispatch, the plugin cvars - with a NullCommandProcessor
 * (rex/graphics/null_command_processor.h): the PM4 ring is consumed and every synchronisation is
 * serviced, but no draw, copy or shader runs on the host GPU. A renderer in the title draws and
 * presents (it finds the device and the presenter through provider() / presenter() as before).
 *
 * name() is overridden because D3D12GraphicsSystem::name() casts the command processor to
 * D3D12CommandProcessor. A low-priority monitor thread logs "null_gpu:" lines: a heartbeat
 * (vblanks per second, ring bursts, swaps, the InsertCallback / interrupt sequences, the waits)
 * every second for the first 20 s and every 10 s after, a STALL line when vblanks, the ring or the
 * swaps stop (with the scratch registers, the scratch block in memory and the command processor's
 * state), and totals at shutdown.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/thread.h>

namespace rex::graphics {
class NullCommandProcessor;
}  // namespace rex::graphics

namespace rex::graphics::d3d12 {

class D3D12NullGraphicsSystem : public D3D12GraphicsSystem {
 public:
  D3D12NullGraphicsSystem();
  ~D3D12NullGraphicsSystem() override;

  std::string name() const override;

  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  void Shutdown() override;

  // Logged, then the base (the ring and the read-pointer write-back of the command processor).
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

 private:
  NullCommandProcessor* null_command_processor() const;
  void MonitorThreadMain();
  // The scratch registers, the scratch block in guest memory, COHER and the interrupt callback.
  std::string SyncState();

  std::unique_ptr<rex::thread::Event> monitor_stop_;
  std::unique_ptr<rex::thread::Thread> monitor_thread_;
  std::atomic<bool> monitor_running_{false};
  uint64_t start_us_ = 0;
};

}  // namespace rex::graphics::d3d12
