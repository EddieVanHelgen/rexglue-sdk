/**
 * @file        rex/ppc/lockstep.h
 * @brief       Lockstep verify for the local_registers code generation option (proof builds only)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * With local_registers_verify, every localized function F is generated twice: __imp__F is F as
 * generated without the option (every register through ctx), instrumented; __lvshadow__F is the
 * localized body, built against the shadow macros below. A sampled call of F runs F's ctx-only
 * body for real and records, in order, every guest memory access it makes (loads with the value
 * they read, stores with the value they wrote, dcbz) and every time-base read. At each of its calls
 * (direct, indirect, trap) and at its exit, the shadow replays the same stretch on a fiber: it starts
 * from the context the real code had at the stretch's start, and each of its memory accesses must be
 * the real code's next one (same kind, address, size, and for a store the same value); a load gets
 * the value the real load read, so other threads writing meanwhile cannot make the two differ. At
 * the shadow's own next call or exit the two are compared: the call (target), the whole register
 * file (the shadow's write-back against the real context), the host MXCSR control bits, and that
 * the shadow made all of the real accesses. The callee then runs once, for real, and the shadow
 * continues from the real context it returns with.
 */

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace rex::lockstep {

using ShadowFn = void(PPCContext& __restrict ctx, uint8_t* base);

// Real (ctx-only) side. Enter returns whether this call of the function is verified; the other
// calls are made only when it did.
// Enter returns a nonzero token when this call of the function is verified (0 otherwise); the token
// goes with every later call, so one cut short (after many stretches) cannot touch a later one.
uint32_t Enter(PPCContext& ctx, uint8_t* base, ShadowFn* shadow, uint32_t function, uint32_t& calls);
void Call(uint32_t token, PPCContext& ctx, const char* callee, uint32_t target);
void Return(uint32_t token, PPCContext& ctx);
void Exit(uint32_t token, PPCContext& ctx);
// The real accesses (performed exactly as REX_LOAD_* / REX_STORE_* / the vector forms do, and
// recorded). Values are the bytes as they lie in guest memory (little-endian assembly).
uint64_t RealLoad(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size);
void RealStore(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size, uint64_t raw);
void RealLoad16(uint32_t token, uint8_t* base, uint32_t addr, void* out);
void RealStore16(uint32_t token, uint8_t* base, uint32_t addr, const void* in);
void RealZero(uint32_t token, uint8_t* base, uint32_t addr, uint32_t size);
uint64_t RealTimebase(uint32_t token);

// Shadow side (only ever runs on the verify fiber).
void ShadowCall(PPCContext& ctx, const char* callee, uint32_t target);
uint64_t ShadowLoad(uint32_t addr, uint32_t size);
void ShadowStore(uint32_t addr, uint32_t size, uint64_t raw);
void ShadowLoad16(uint32_t addr, void* out);
void ShadowStore16(uint32_t addr, const void* in);
void ShadowZero(uint32_t addr, uint32_t size);
uint64_t ShadowTimebase();

}  // namespace rex::lockstep

// ---- real (instrumented ctx-only) body: rex_lv_on is the function's own flag ----
#define REX_LV_ENTER(shadow, addr)                                                       \
  static uint32_t rex_lv_calls_ = 0;                                                     \
  const uint32_t rex_lv_on = rex::lockstep::Enter(ctx, base, shadow, addr, rex_lv_calls_)
#define REX_LVR_CALL(name, target) \
  do { if (rex_lv_on) rex::lockstep::Call(rex_lv_on, ctx, name, (uint32_t)(target)); } while (0)
#define REX_LVR_RET() do { if (rex_lv_on) rex::lockstep::Return(rex_lv_on, ctx); } while (0)
#define REX_LVR_EXIT() do { if (rex_lv_on) rex::lockstep::Exit(rex_lv_on, ctx); } while (0)

#define REX_LVR_LOAD_U8(x) (rex_lv_on ? (u8)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 1) : REX_LOAD_U8(x))
#define REX_LVR_LOAD_U16(x) \
  (rex_lv_on ? __builtin_bswap16((u16)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 2)) : REX_LOAD_U16(x))
#define REX_LVR_LOAD_U32(x) \
  (rex_lv_on ? __builtin_bswap32((u32)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 4)) : REX_LOAD_U32(x))
#define REX_LVR_LOAD_U64(x) \
  (rex_lv_on ? __builtin_bswap64((u64)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 8)) : REX_LOAD_U64(x))
#define REX_LVR_SLOAD_U8(x) (rex_lv_on ? (u8)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 1) : REX_SLOAD_U8(x))
#define REX_LVR_SLOAD_U16(x) \
  (rex_lv_on ? __builtin_bswap16((u16)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 2)) : REX_SLOAD_U16(x))
#define REX_LVR_SLOAD_U32(x) \
  (rex_lv_on ? __builtin_bswap32((u32)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 4)) : REX_SLOAD_U32(x))
#define REX_LVR_SLOAD_U64(x) \
  (rex_lv_on ? __builtin_bswap64((u64)rex::lockstep::RealLoad(rex_lv_on, base, (uint32_t)(x), 8)) : REX_SLOAD_U64(x))
#define REX_LVR_STORE_U8(x, y) \
  do { if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 1, (u8)(y)); else REX_STORE_U8(x, y); } while (0)
#define REX_LVR_STORE_U16(x, y)                                                                     \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 2, __builtin_bswap16((u16)(y)));   \
    else REX_STORE_U16(x, y);                                                                       \
  } while (0)
#define REX_LVR_STORE_U32(x, y)                                                                     \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 4, __builtin_bswap32((u32)(y)));   \
    else REX_STORE_U32(x, y);                                                                       \
  } while (0)
#define REX_LVR_STORE_U64(x, y)                                                                     \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 8, __builtin_bswap64((u64)(y)));   \
    else REX_STORE_U64(x, y);                                                                       \
  } while (0)
#define REX_LVR_SSTORE_U8(x, y) \
  do { if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 1, (u8)(y)); else REX_SSTORE_U8(x, y); } while (0)
#define REX_LVR_SSTORE_U16(x, y)                                                                    \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 2, __builtin_bswap16((u16)(y)));   \
    else REX_SSTORE_U16(x, y);                                                                      \
  } while (0)
#define REX_LVR_SSTORE_U32(x, y)                                                                    \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 4, __builtin_bswap32((u32)(y)));   \
    else REX_SSTORE_U32(x, y);                                                                      \
  } while (0)
#define REX_LVR_SSTORE_U64(x, y)                                                                    \
  do {                                                                                              \
    if (rex_lv_on) rex::lockstep::RealStore(rex_lv_on, base, (uint32_t)(x), 8, __builtin_bswap64((u64)(y)));   \
    else REX_SSTORE_U64(x, y);                                                                      \
  } while (0)
#define REX_LVR_LOAD16(x)                                                          \
  ([&]() {                                                                         \
    simde__m128i rex_lv_v_;                                                        \
    if (rex_lv_on)                                                                 \
      rex::lockstep::RealLoad16(rex_lv_on, base, (uint32_t)(x), &rex_lv_v_);                  \
    else                                                                           \
      rex_lv_v_ = simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(x));             \
    return rex_lv_v_;                                                              \
  }())
#define REX_LVR_STORE16(x, v)                                                      \
  do {                                                                             \
    simde__m128i rex_lv_v_ = (v);                                                  \
    if (rex_lv_on)                                                                 \
      rex::lockstep::RealStore16(rex_lv_on, base, (uint32_t)(x), &rex_lv_v_);                 \
    else                                                                           \
      simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(x), rex_lv_v_);             \
  } while (0)
#define REX_LVR_ZERO(x, zero, n)                                                                   \
  do {                                                                                             \
    if (rex_lv_on) rex::lockstep::RealZero(rex_lv_on, base, (uint32_t)(x), n);                                \
    else memset((void*)REX_RAW_ADDR(x), zero, n);                                                  \
  } while (0)
#define REX_LVR_TIMEBASE() (rex_lv_on ? rex::lockstep::RealTimebase(rex_lv_on) : REX_QUERY_TIMEBASE())

// ---- shadow (localized) body ----
#define REX_LVS_CALL(name, target) rex::lockstep::ShadowCall(ctx, name, (uint32_t)(target))
#define REX_LVS_LOAD_U8(x) ((u8)rex::lockstep::ShadowLoad((uint32_t)(x), 1))
#define REX_LVS_LOAD_U16(x) __builtin_bswap16((u16)rex::lockstep::ShadowLoad((uint32_t)(x), 2))
#define REX_LVS_LOAD_U32(x) __builtin_bswap32((u32)rex::lockstep::ShadowLoad((uint32_t)(x), 4))
#define REX_LVS_LOAD_U64(x) __builtin_bswap64((u64)rex::lockstep::ShadowLoad((uint32_t)(x), 8))
#define REX_LVS_SLOAD_U8(x) REX_LVS_LOAD_U8(x)
#define REX_LVS_SLOAD_U16(x) REX_LVS_LOAD_U16(x)
#define REX_LVS_SLOAD_U32(x) REX_LVS_LOAD_U32(x)
#define REX_LVS_SLOAD_U64(x) REX_LVS_LOAD_U64(x)
#define REX_LVS_STORE_U8(x, y) rex::lockstep::ShadowStore((uint32_t)(x), 1, (u8)(y))
#define REX_LVS_STORE_U16(x, y) rex::lockstep::ShadowStore((uint32_t)(x), 2, __builtin_bswap16((u16)(y)))
#define REX_LVS_STORE_U32(x, y) rex::lockstep::ShadowStore((uint32_t)(x), 4, __builtin_bswap32((u32)(y)))
#define REX_LVS_STORE_U64(x, y) rex::lockstep::ShadowStore((uint32_t)(x), 8, __builtin_bswap64((u64)(y)))
#define REX_LVS_SSTORE_U8(x, y) REX_LVS_STORE_U8(x, y)
#define REX_LVS_SSTORE_U16(x, y) REX_LVS_STORE_U16(x, y)
#define REX_LVS_SSTORE_U32(x, y) REX_LVS_STORE_U32(x, y)
#define REX_LVS_SSTORE_U64(x, y) REX_LVS_STORE_U64(x, y)
#define REX_LVS_LOAD16(x)                                          \
  ([&]() {                                                         \
    simde__m128i rex_lvs_v_;                                       \
    rex::lockstep::ShadowLoad16((uint32_t)(x), &rex_lvs_v_);       \
    return rex_lvs_v_;                                             \
  }())
#define REX_LVS_STORE16(x, v)                                      \
  do {                                                             \
    simde__m128i rex_lvs_v_ = (v);                                 \
    rex::lockstep::ShadowStore16((uint32_t)(x), &rex_lvs_v_);      \
  } while (0)
#define REX_LVS_ZERO(x, zero, n) rex::lockstep::ShadowZero((uint32_t)(x), n)
#define REX_LVS_TIMEBASE() rex::lockstep::ShadowTimebase()
