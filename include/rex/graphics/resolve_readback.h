/**
 * Resolve readback ranges.
 *
 * Some titles read the result of an EDRAM resolve on the CPU (Perfect Dark
 * Zero's death effect samples a snapshot of the body). With readback_resolve
 * = "none" resolves stay on the GPU and the CPU sees stale memory, while
 * reading every resolve back ("full") stalls the GPU many times a frame. A
 * title (or its mod code) can instead name the guest physical ranges whose
 * resolves must reach guest memory: a resolve that writes into one of them is
 * read back synchronously (the command processor waits for the GPU and copies
 * the data, downscaled first with resolution scaling) before the next
 * command is processed, whatever readback_resolve says. Other resolves are
 * unaffected.
 *
 * Lives in rexruntime (shared by the app and the GPU plugin).
 */

#pragma once

#include <cstdint>

namespace rex::graphics::resolve_readback {

// Reads back resolves into [physical_address, physical_address + length)
// from now on (until Remove). Adding a range that is already there is a
// no-op.
void Add(uint32_t physical_address, uint32_t length);

// Stops reading back the range that starts at physical_address.
void Remove(uint32_t physical_address);

// True if any range is registered (cheap).
bool Any();

// True if [address, address + length) overlaps a registered range.
bool Overlaps(uint32_t address, uint32_t length);

// Pass dump (a native renderer's per-pass comparison): every resolve the GPU
// processes in the frame that ends with guest swap `swap` (the GPU plugin's
// IssueSwap count, 1-based) is read back synchronously and handed to the
// observer on the GPU thread (address and length of the written extent, the
// 1x data, RB_COPY_DEST_INFO). Guest memory is only written for ranges added
// with Add. swap 0 disarms.
using DumpObserver = void (*)(uint32_t address, uint32_t length, const uint8_t* data,
                              uint32_t copy_dest_info);
void ArmDump(uint64_t swap, DumpObserver observer);
// IssueSwap: the swap just issued (its 1-based index).
void NoteSwap(uint64_t swap);
// True while the GPU processes the armed frame (cheap).
bool DumpingFrame();
// Hands one resolve to the observer (when DumpingFrame).
void ObserveDump(uint32_t address, uint32_t length, const uint8_t* data, uint32_t copy_dest_info);

// Per-draw companion of the dump (same armed frame): every draw the GPU
// processes is reported with its primitive type, index count, the vertex
// shader's ucode hash and the vertex shader float constants c0..c63
// (registers 0x4000.., 256 dwords) as the register file holds them.
using DrawObserver = void (*)(uint32_t primitive_type, uint32_t index_count, uint64_t vs_hash,
                              const uint32_t* vs_constants_256);
void SetDrawObserver(DrawObserver observer);
// The indirect buffer (PM4 INDIRECT_BUFFER, e.g. a D3D precompiled command buffer) the GPU is executing, 0 for
// the primary ring; set by the command processor, read by draw observers.
void SetCurrentIndirect(uint32_t guest_address);
uint32_t CurrentIndirect();
void ObserveDraw(uint32_t primitive_type, uint32_t index_count, uint64_t vs_hash,
                 const uint32_t* vs_constants_256);

}  // namespace rex::graphics::resolve_readback
