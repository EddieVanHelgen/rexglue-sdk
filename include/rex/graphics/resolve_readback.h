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

}  // namespace rex::graphics::resolve_readback
