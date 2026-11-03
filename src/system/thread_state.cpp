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

#include <atomic>
#include <cstdlib>

#include <rex/assert.h>
#include <rex/kernel.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>

namespace rex::runtime {

thread_local ThreadState* thread_state_ = nullptr;

ThreadState::ThreadState(uint32_t thread_id, uint32_t stack_base, uint32_t pcr_address,
                         memory::Memory* memory)
    : memory_(memory), pcr_address_(pcr_address), thread_id_(thread_id) {
  if (thread_id_ == UINT_MAX) {
    // System thread. Assign the system thread ID with a high bit
    // set so people know what's up.
    uint32_t system_thread_handle = rex::thread::current_thread_system_id();
    thread_id_ = 0x80000000 | system_thread_handle;
  }

  // Set initial registers
  context_->r1.u64 = stack_base;    // Stack pointer
  context_->r13.u64 = pcr_address;  // PCR address

  // Note(tomc): the rexglue abi passes memory base via 'base' parameter to each function call,
  // so we don't need to store it in the context like JIT did.
}

ThreadState::~ThreadState() {
  if (thread_state_ == this) {
    thread_state_ = nullptr;
  }
}

void ThreadState::Bind(ThreadState* thread_state) {
  thread_state_ = thread_state;
}

ThreadState* ThreadState::Get() {
  return thread_state_;
}

uint32_t ThreadState::GetThreadID() {
  auto* thread_state = Get();
  return thread_state ? thread_state->thread_id_ : 0xFFFFFFFF;
}

}  // namespace rex::runtime

// Codegen option stack_direct_access_verify (proof runs only): the generated REX_SLOAD_* /
// REX_SSTORE_* macros call this for an r1-based guest access at or above 0xE0000000, which the
// option assumes never happens (guest stacks are in 0x70000000-0x7F000000). The access itself still
// goes through REX_PHYS_HOST_OFFSET, as without the option.
extern "C" void rex_codegen_stack_fault(uint32_t addr) {
  static std::atomic<uint32_t> faults{0};
  const uint32_t n = ++faults;
  if (n <= 16 || (n & (n - 1)) == 0) {
    REXLOG_ERROR("codegen stack_direct_access_verify: r1-based access at 0x{:08X} ({} so far)", addr,
                 n);
  }
}
