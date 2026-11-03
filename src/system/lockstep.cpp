/**
 * @file        system/lockstep.cpp
 * @brief       Lockstep verify for the local_registers code generation option (see rex/ppc/lockstep.h)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Only builds generated with local_registers_verify call into this file. Per host thread: one
 * verified call at a time (nested and recursive calls run unverified), one shadow fiber.
 */

#include <rex/ppc/lockstep.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <fmt/format.h>

#if defined(_WIN32)
#include <windows.h>
#include <immintrin.h>
#endif

REXCVAR_DEFINE_UINT32(local_registers_verify_every, 64, "Codegen",
                      "local_registers_verify builds: after its first 8 calls, every Nth call of each "
                      "localized function is verified in lockstep (1 = every call)");

REXCVAR_DEFINE_BOOL(local_registers_verify_selftest, false, "Codegen",
                    "local_registers_verify builds, test of the checker: in every 1000th verified call the "
                    "shadow's first store is written with its lowest bit flipped (each must be reported)");

namespace rex::lockstep {
namespace {

#if defined(_WIN32)

// The MXCSR / fpscr bits compared: everything but the sticky exception flags (bits 0-5), which are
// host-only (mffs returns the rounding mode) and which the verify's own bookkeeping can set.
constexpr uint32_t kCsrCompareMask = ~0x3Fu;

struct Field {
  const char* name;
  size_t offset;
  size_t size;
};

// The context's fields, for the field-wise compare (padding between fields is never compared).
const std::vector<Field>& Fields() {
  static const std::vector<Field> fields = [] {
    std::vector<Field> f;
#define REX_LV_FIELD(n) f.push_back({#n, offsetof(PPCContext, n), sizeof(PPCContext::n)})
    REX_LV_FIELD(r0); REX_LV_FIELD(r1); REX_LV_FIELD(r2); REX_LV_FIELD(r3); REX_LV_FIELD(r4);
    REX_LV_FIELD(r5); REX_LV_FIELD(r6); REX_LV_FIELD(r7); REX_LV_FIELD(r8); REX_LV_FIELD(r9);
    REX_LV_FIELD(r10); REX_LV_FIELD(r11); REX_LV_FIELD(r12); REX_LV_FIELD(r13); REX_LV_FIELD(r14);
    REX_LV_FIELD(r15); REX_LV_FIELD(r16); REX_LV_FIELD(r17); REX_LV_FIELD(r18); REX_LV_FIELD(r19);
    REX_LV_FIELD(r20); REX_LV_FIELD(r21); REX_LV_FIELD(r22); REX_LV_FIELD(r23); REX_LV_FIELD(r24);
    REX_LV_FIELD(r25); REX_LV_FIELD(r26); REX_LV_FIELD(r27); REX_LV_FIELD(r28); REX_LV_FIELD(r29);
    REX_LV_FIELD(r30); REX_LV_FIELD(r31);
    REX_LV_FIELD(lr); REX_LV_FIELD(ctr); REX_LV_FIELD(xer); REX_LV_FIELD(reserved); REX_LV_FIELD(msr);
    REX_LV_FIELD(cr0); REX_LV_FIELD(cr1); REX_LV_FIELD(cr2); REX_LV_FIELD(cr3); REX_LV_FIELD(cr4);
    REX_LV_FIELD(cr5); REX_LV_FIELD(cr6); REX_LV_FIELD(cr7);
    REX_LV_FIELD(vscr_sat); REX_LV_FIELD(last_indirect_target);
#undef REX_LV_FIELD
    // f0..f31 and v0..v127 are contiguous runs of fields in the struct.
    for (int i = 0; i < 32; ++i) {
      static std::string names[32];
      names[i] = "f" + std::to_string(i);
      f.push_back({names[i].c_str(), offsetof(PPCContext, f0) + i * sizeof(PPCRegister), sizeof(PPCRegister)});
    }
    for (int i = 0; i < 128; ++i) {
      static std::string names[128];
      names[i] = "v" + std::to_string(i);
      f.push_back({names[i].c_str(), offsetof(PPCContext, v0) + i * sizeof(PPCVRegister), sizeof(PPCVRegister)});
    }
    return f;
  }();
  return fields;
}

enum class YieldKind { None, Call, Exit };

enum class AccessKind : uint8_t { Load, Store, Zero, Timebase };

const char* AccessName(AccessKind k) {
  switch (k) {
    case AccessKind::Load: return "load";
    case AccessKind::Store: return "store";
    case AccessKind::Zero: return "dcbz";
    default: return "time-base read";
  }
}

struct Access {
  AccessKind kind;
  uint8_t size;
  uint32_t addr;
  uint8_t data[16];  // load: the bytes read; store: the bytes written; time base: the value
};

struct State {
  alignas(64) PPCContext sctx;  // the shadow's context
  bool active = false;          // a verified call is in progress on this thread
  uint32_t token = 0;           // its token (Enter's result)
  bool bad = false;             // it already mismatched: the rest of it is not compared
  void* shadowFiber = nullptr;
  void* realFiber = nullptr;
  ShadowFn* shadow = nullptr;
  uint8_t* base = nullptr;
  uint32_t function = 0;
  uint32_t checkpoint = 0;
  uint32_t segCsr = 0;     // host MXCSR at the start of the current stretch
  uint32_t shadowCsr = 0;  // the shadow's host MXCSR where it stopped
  std::string csrMismatch;
  YieldKind kind = YieldKind::None;
  const char* callee = nullptr;
  uint32_t target = 0;
  std::vector<Access> accesses;  // the real stretch's memory accesses and time-base reads, in order
  size_t next = 0;               // the shadow's position in them
  uint64_t storeBytes = 0;       // bytes the shadow stored that matched
  uint64_t accessCount = 0;      // accesses the shadow matched
  bool corruptNextStore = false; // local_registers_verify_selftest
};

// Per thread: the verified calls in progress, innermost last (a call made while an outer one waits at
// its own call is verified as well, up to kMaxDepth deep). An outer call's body never runs while an
// inner one is on top, so each records and replays only its own stretches. t_state is the top.
constexpr size_t kMaxDepth = 8;
thread_local std::vector<State*> t_stack;
thread_local std::vector<State*> t_free;
thread_local State* t_state = nullptr;
thread_local bool t_threadReady = false;
std::atomic<uint64_t> g_lost{0};  // verified calls dropped: a guest fiber switch put another one on top
std::atomic<uint64_t> g_corrupted{0};  // local_registers_verify_selftest: stores the shadow wrote wrong

std::atomic<uint64_t> g_calls{0};        // verified calls
std::atomic<uint64_t> g_checkpoints{0};  // calls / exits compared
std::atomic<uint64_t> g_accesses{0};     // memory accesses / time-base reads matched
std::atomic<uint64_t> g_storeBytes{0};   // stored bytes matched
std::atomic<uint64_t> g_mismatches{0};
std::atomic<uint64_t> g_skippedThreads{0};
std::atomic<uint64_t> g_truncated{0};  // verified calls let go after kMaxStretches
std::atomic<uint32_t> g_nextToken{0};
constexpr uint32_t kMaxStretches = 256;
std::mutex g_functionsMutex;
std::unordered_set<uint32_t> g_functions;
std::atomic<int64_t> g_lastReport{0};

inline uint64_t Key(uint32_t addr) {
  return uint64_t(addr) + (addr >= 0xE0000000u ? 0x1000u : 0u);  // REX_PHYS_HOST_OFFSET
}

uint32_t LimitLog() {
  static std::atomic<uint32_t> logged{0};
  return logged.fetch_add(1);
}

void Mismatch(State* s, const std::string& what) {
  if (s->bad)
    return;
  g_mismatches.fetch_add(1);
  s->bad = true;
  if (LimitLog() < 64) {
    REXLOG_ERROR("local_registers verify: MISMATCH sub_{:08X} stretch {}: {}", s->function, s->checkpoint,
                 what);
  }
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i)
    out += fmt::format("{:02X}", p[i]);
  return out;
}

// A fault in the shadow (it went through an address the real code never used) is a mismatch.
bool RunShadowFunction(State* s) {
  __try {
    s->shadow(s->sctx, s->base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

void WINAPI ShadowMain(void* param) {
  State* s = static_cast<State*>(param);
  for (;;) {
    // x64 fibers keep their own MXCSR: the stretch's is set here, on the shadow's side.
    _mm_setcsr(s->segCsr);
    if (!RunShadowFunction(s)) {
      s->kind = YieldKind::None;
      s->callee = "(the shadow faulted)";
      s->target = 0;
    } else {
      s->kind = YieldKind::Exit;
      s->callee = nullptr;
      s->target = 0;
    }
    s->shadowCsr = _mm_getcsr();
    ::SwitchToFiber(s->realFiber);
  }
}

void NewShadowFiber(State* s) {
  if (s->shadowFiber)
    ::DeleteFiber(s->shadowFiber);
  s->shadowFiber = ::CreateFiber(1 << 20, ShadowMain, s);
}

State* Find(uint32_t token) {
  for (auto it = t_stack.rbegin(); it != t_stack.rend(); ++it)
    if ((*it)->token == token)
      return *it;
  return nullptr;
}

// The verified call for this token is on top: it may go on. One below the top (a guest fiber switch
// inside a callee started another verified call on this thread, which is still open) can no longer
// be followed: it is dropped from the comparison (not a mismatch) and leaves the stack at its exit.
State* Top(uint32_t token) {
  State* s = t_state;
  if (s && s->token == token)
    return s;
  if (State* lost = Find(token); lost && !lost->bad) {
    lost->bad = true;
    g_lost.fetch_add(1, std::memory_order_relaxed);
  }
  return nullptr;
}

void Release(State* s) {
  // A shadow stopped anywhere but at its own exit is left mid-function: give it a fresh fiber.
  if (s->bad || s->kind != YieldKind::Exit)
    NewShadowFiber(s);
  s->active = false;
  s->token = 0;
  t_stack.erase(std::find(t_stack.begin(), t_stack.end(), s));
  t_free.push_back(s);
  t_state = t_stack.empty() ? nullptr : t_stack.back();
}

// Runs the shadow from where it stopped to its next call or its exit.
void RunShadow(State* s) {
  s->realFiber = ::GetCurrentFiber();
  const uint32_t realCsr = _mm_getcsr();
  s->kind = YieldKind::None;
  ::SwitchToFiber(s->shadowFiber);
  _mm_setcsr(realCsr);
  if ((s->shadowCsr & kCsrCompareMask) != (realCsr & kCsrCompareMask))
    s->csrMismatch = fmt::format("host MXCSR real {:08X} shadow {:08X}", realCsr, s->shadowCsr);
}

const char* KindName(YieldKind k) {
  return k == YieldKind::Exit ? "exits" : k == YieldKind::Call ? "calls" : "stops";
}

// The shadow stopped where the real code is (kind, callee, target); compare everything it can see.
void Compare(State* s, const PPCContext& real, YieldKind kind, const char* callee, uint32_t target) {
  if (s->bad)
    return;
  if (s->kind != kind) {
    Mismatch(s, fmt::format("real {} {} {:08X}, shadow {} {} {:08X}", KindName(kind), callee ? callee : "", target,
                            KindName(s->kind), s->callee ? s->callee : "", s->target));
    return;
  }
  if (kind == YieldKind::Call) {
    bool same = s->target == target &&
                ((!callee && !s->callee) || (callee && s->callee && std::strcmp(callee, s->callee) == 0));
    if (!same) {
      Mismatch(s, fmt::format("call real {} {:08X}, shadow {} {:08X}", callee ? callee : "(indirect)", target,
                              s->callee ? s->callee : "(indirect)", s->target));
      return;
    }
  }
  if (!s->csrMismatch.empty()) {
    Mismatch(s, s->csrMismatch);
    return;
  }
  const auto* r = reinterpret_cast<const uint8_t*>(&real);
  const auto* h = reinterpret_cast<const uint8_t*>(&s->sctx);
  for (const auto& f : Fields()) {
    if (std::memcmp(r + f.offset, h + f.offset, f.size) != 0) {
      Mismatch(s, fmt::format("{} at {} {}: real {} shadow {}", f.name, kind == YieldKind::Exit ? "exit" : "call",
                              callee ? callee : "", Hex(r + f.offset, f.size), Hex(h + f.offset, f.size)));
      return;
    }
  }
  if ((real.fpscr.csr & kCsrCompareMask) != (s->sctx.fpscr.csr & kCsrCompareMask)) {
    Mismatch(s, fmt::format("fpscr real {:08X} shadow {:08X}", real.fpscr.csr, s->sctx.fpscr.csr));
    return;
  }
  // The shadow made every one of the real stretch's accesses (each one it made was checked as it went).
  if (s->next != s->accesses.size()) {
    const Access& a = s->accesses[s->next];
    Mismatch(s, fmt::format("the real code made {} memory accesses / time-base reads here, the shadow {} (next "
                            "real: {} of {} bytes at {:08X})",
                            s->accesses.size(), s->next, AccessName(a.kind), a.size, a.addr));
    return;
  }
  g_checkpoints.fetch_add(1, std::memory_order_relaxed);
  g_accesses.fetch_add(s->accessCount, std::memory_order_relaxed);
  g_storeBytes.fetch_add(s->storeBytes, std::memory_order_relaxed);
}

void NextStretch(State* s) {
  s->accesses.clear();
  s->next = 0;
  s->storeBytes = 0;
  s->accessCount = 0;
  s->csrMismatch.clear();
  ++s->checkpoint;
}

// Shadow side, at a mismatch: hand back to the real code now and never resume (a shadow fed wrong
// values could otherwise loop for ever on the real code's thread). The frame gets a fresh fiber at
// its exit.
[[noreturn]] void StopShadow(State* s) {
  s->kind = YieldKind::None;
  s->callee = "(stopped at a mismatch)";
  s->shadowCsr = _mm_getcsr();
  for (;;)
    ::SwitchToFiber(s->realFiber);
}

// The shadow's next access must be the real code's next one.
const Access* Expect(State* s, AccessKind kind, uint32_t addr, uint32_t size) {
  if (s->bad)
    StopShadow(s);
  if (s->next >= s->accesses.size()) {
    Mismatch(s, fmt::format("the shadow made a {} of {} bytes at {:08X} after the real code's last access ({})",
                            AccessName(kind), size, addr, s->accesses.size()));
    StopShadow(s);
  }
  const Access& a = s->accesses[s->next++];
  if (a.kind != kind || a.addr != addr || a.size != size) {
    Mismatch(s, fmt::format("access {}: real {} of {} bytes at {:08X}, shadow {} of {} bytes at {:08X}", s->next - 1,
                            AccessName(a.kind), a.size, a.addr, AccessName(kind), size, addr));
    StopShadow(s);
  }
  ++s->accessCount;
  return &a;
}

Access& Record(State* s, AccessKind kind, uint32_t addr, uint32_t size) {
  Access& a = s->accesses.emplace_back();
  a.kind = kind;
  a.size = uint8_t(size);
  a.addr = addr;
  return a;
}

inline bool Recording(State* s, uint32_t token) { return s && s->active && s->token == token && !s->bad; }

void Report() {
  using namespace std::chrono;
  const int64_t now = duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
  int64_t last = g_lastReport.load();
  if (now - last < 10)
    return;
  if (!g_lastReport.compare_exchange_strong(last, now))
    return;
  size_t functions;
  {
    std::lock_guard lock(g_functionsMutex);
    functions = g_functions.size();
  }
  REXLOG_INFO("local_registers verify: {} calls of {} functions verified, {} calls / exits compared, {} memory "
              "accesses matched ({} stored bytes), {} mismatches (every {} call(s); let go after {} stretches: {}; "
              "dropped at a guest fiber switch: {}; threads without a fiber: {}; self-test stores corrupted: {})",
              g_calls.load(), functions, g_checkpoints.load(), g_accesses.load(), g_storeBytes.load(),
              g_mismatches.load(), REXCVAR_GET(local_registers_verify_every), kMaxStretches,
              g_truncated.load(), g_lost.load(), g_skippedThreads.load(), g_corrupted.load());
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

uint32_t Enter(PPCContext& ctx, uint8_t* base, ShadowFn* shadow, uint32_t function, uint32_t& calls) {
  if (!t_threadReady) {
    if (!::IsThreadAFiber() && !::ConvertThreadToFiber(nullptr)) {
      g_skippedThreads.fetch_add(1);
      return 0;
    }
    t_threadReady = true;
  }
  if (t_stack.size() >= kMaxDepth)
    return 0;
  const uint32_t n = calls++;
  const uint32_t every = std::max<uint32_t>(1, REXCVAR_GET(local_registers_verify_every));
  if (n >= 8 && (n % every) != 0)
    return 0;
  State* s;
  if (!t_free.empty()) {
    s = t_free.back();
    t_free.pop_back();
  } else {
    s = new State();
    NewShadowFiber(s);
  }
  if (!s->shadowFiber) {
    t_free.push_back(s);
    return 0;
  }
  t_stack.push_back(s);
  t_state = s;
  uint32_t token = g_nextToken.fetch_add(1) + 1;
  if (!token)
    token = g_nextToken.fetch_add(1) + 1;
  s->active = true;
  s->token = token;
  s->bad = false;
  s->shadow = shadow;
  s->base = base;
  s->function = function;
  s->checkpoint = 0;
  std::memcpy(static_cast<void*>(&s->sctx), &ctx, sizeof(PPCContext));
  s->segCsr = _mm_getcsr();
  s->accesses.clear();
  s->next = 0;
  s->storeBytes = 0;
  s->accessCount = 0;
  s->csrMismatch.clear();
  const uint64_t verified = g_calls.fetch_add(1, std::memory_order_relaxed);
  s->corruptNextStore = REXCVAR_GET(local_registers_verify_selftest) && verified % 1000 == 999;
  {
    std::lock_guard lock(g_functionsMutex);
    g_functions.insert(function);
  }
  return token;
}

void Call(uint32_t token, PPCContext& ctx, const char* callee, uint32_t target) {
  State* s = Top(token);
  if (!s || s->bad)
    return;
  RunShadow(s);
  Compare(s, ctx, YieldKind::Call, callee, target);
  NextStretch(s);
  // A long-running call (a thread's main loop) is let go after kMaxStretches compared stretches.
  if (s->checkpoint >= kMaxStretches && !s->bad) {
    g_truncated.fetch_add(1, std::memory_order_relaxed);
    s->kind = YieldKind::None;  // its shadow is mid-function
    Release(s);
  }
}

void Return(uint32_t token, PPCContext& ctx) {
  State* s = Top(token);
  if (!s || s->bad)
    return;
  std::memcpy(static_cast<void*>(&s->sctx), &ctx, sizeof(PPCContext));
  s->segCsr = _mm_getcsr();
}

void Exit(uint32_t token, PPCContext& ctx) {
  State* s = Find(token);
  if (!s)
    return;
  if (s == t_state && !s->bad) {
    RunShadow(s);
    Compare(s, ctx, YieldKind::Exit, nullptr, 0);
  } else if (!s->bad) {
    s->bad = true;
    g_lost.fetch_add(1, std::memory_order_relaxed);
  }
  Release(s);
  Report();
}

uint64_t RealLoad(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size) {
  const volatile uint8_t* p = base + Key(addr);
  uint64_t v = 0;
  switch (size) {  // one access of the guest's width, as REX_LOAD_* makes it
    case 1: v = *(const volatile uint8_t*)p; break;
    case 2: v = *(const volatile uint16_t*)p; break;
    case 4: v = *(const volatile uint32_t*)p; break;
    default: v = *(const volatile uint64_t*)p; break;
  }
  State* s = t_state;
  if (Recording(s, token))
    std::memcpy(Record(s, AccessKind::Load, addr, size).data, &v, size);
  return v;
}

void RealStore(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size, uint64_t raw) {
  volatile uint8_t* p = base + Key(addr);
  switch (size) {
    case 1: *(volatile uint8_t*)p = uint8_t(raw); break;
    case 2: *(volatile uint16_t*)p = uint16_t(raw); break;
    case 4: *(volatile uint32_t*)p = uint32_t(raw); break;
    default: *(volatile uint64_t*)p = raw; break;
  }
  State* s = t_state;
  if (Recording(s, token))
    std::memcpy(Record(s, AccessKind::Store, addr, size).data, &raw, size);
}

void RealLoad16(uint32_t token, uint8_t* base, uint32_t addr, void* out) {
  const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(base + Key(addr)));
  std::memcpy(out, &v, 16);
  State* s = t_state;
  if (Recording(s, token))
    std::memcpy(Record(s, AccessKind::Load, addr, 16).data, &v, 16);
}

void RealStore16(uint32_t token, uint8_t* base, uint32_t addr, const void* in) {
  __m128i v;
  std::memcpy(&v, in, 16);
  _mm_store_si128(reinterpret_cast<__m128i*>(base + Key(addr)), v);
  State* s = t_state;
  if (Recording(s, token))
    std::memcpy(Record(s, AccessKind::Store, addr, 16).data, in, 16);
}

void RealZero(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size) {
  std::memset(base + Key(addr), 0, size);
  State* s = t_state;
  if (Recording(s, token))
    Record(s, AccessKind::Zero, addr, size);
}

uint64_t RealTimebase(uint32_t token) {
  const uint64_t v = rex::chrono::Clock::QueryGuestTickCount();
  State* s = t_state;
  if (Recording(s, token))
    std::memcpy(Record(s, AccessKind::Timebase, 0, 8).data, &v, 8);
  return v;
}

void ShadowCall(PPCContext& /*ctx*/, const char* callee, uint32_t target) {
  State* s = t_state;
  s->kind = YieldKind::Call;
  s->callee = callee;
  s->target = target;
  s->shadowCsr = _mm_getcsr();
  ::SwitchToFiber(s->realFiber);
  // Resumed with s->sctx = the real context after the callee returned, and its MXCSR.
  _mm_setcsr(s->segCsr);
}

uint64_t ShadowLoad(uint32_t addr, uint32_t size) {
  State* s = t_state;
  uint64_t v = 0;
  if (const Access* a = Expect(s, AccessKind::Load, addr, size))
    std::memcpy(&v, a->data, size);
  return v;
}

void ShadowStore(uint32_t addr, uint32_t size, uint64_t raw) {
  State* s = t_state;
  if (s->corruptNextStore) {
    s->corruptNextStore = false;
    g_corrupted.fetch_add(1, std::memory_order_relaxed);
    raw ^= 1;
  }
  if (const Access* a = Expect(s, AccessKind::Store, addr, size)) {
    if (std::memcmp(a->data, &raw, size) != 0) {
      Mismatch(s, fmt::format("store of {} bytes at {:08X}: real {} shadow {}", size, addr, Hex(a->data, size),
                              Hex(reinterpret_cast<const uint8_t*>(&raw), size)));
      StopShadow(s);
    } else
      s->storeBytes += size;
  }
}

void ShadowLoad16(uint32_t addr, void* out) {
  State* s = t_state;
  std::memset(out, 0, 16);
  if (const Access* a = Expect(s, AccessKind::Load, addr, 16))
    std::memcpy(out, a->data, 16);
}

void ShadowStore16(uint32_t addr, const void* in) {
  State* s = t_state;
  if (const Access* a = Expect(s, AccessKind::Store, addr, 16)) {
    if (std::memcmp(a->data, in, 16) != 0) {
      Mismatch(s, fmt::format("vector store at {:08X}: real {} shadow {}", addr, Hex(a->data, 16),
                              Hex(static_cast<const uint8_t*>(in), 16)));
      StopShadow(s);
    } else
      s->storeBytes += 16;
  }
}

void ShadowZero(uint32_t addr, uint32_t size) {
  State* s = t_state;
  if (Expect(s, AccessKind::Zero, addr, size))
    s->storeBytes += size;
}

uint64_t ShadowTimebase() {
  State* s = t_state;
  uint64_t v = 0;
  if (const Access* a = Expect(s, AccessKind::Timebase, 0, 8))
    std::memcpy(&v, a->data, 8);
  return v;
}

#else  // !_WIN32: the verify build is Windows-only; these keep a non-Windows SDK build linking.

uint32_t Enter(PPCContext&, uint8_t*, ShadowFn*, uint32_t, uint32_t&) { return 0; }
void Call(uint32_t, PPCContext&, const char*, uint32_t) {}
void Return(uint32_t, PPCContext&) {}
void Exit(uint32_t, PPCContext&) {}
uint64_t RealLoad(uint32_t, uint8_t*, uint32_t, uint32_t) { return 0; }
void RealStore(uint32_t, uint8_t*, uint32_t, uint32_t, uint64_t) {}
void RealLoad16(uint32_t, uint8_t*, uint32_t, void*) {}
void RealStore16(uint32_t, uint8_t*, uint32_t, const void*) {}
void RealZero(uint32_t, uint8_t*, uint32_t, uint32_t) {}
uint64_t RealTimebase(uint32_t) { return rex::chrono::Clock::QueryGuestTickCount(); }
void ShadowCall(PPCContext&, const char*, uint32_t) {}
uint64_t ShadowLoad(uint32_t, uint32_t) { return 0; }
void ShadowStore(uint32_t, uint32_t, uint64_t) {}
void ShadowLoad16(uint32_t, void*) {}
void ShadowStore16(uint32_t, const void*) {}
void ShadowZero(uint32_t, uint32_t) {}
uint64_t ShadowTimebase() { return 0; }

#endif

}  // namespace rex::lockstep
