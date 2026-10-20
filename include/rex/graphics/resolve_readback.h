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

}  // namespace rex::graphics::resolve_readback
