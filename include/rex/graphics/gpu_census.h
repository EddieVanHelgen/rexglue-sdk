/**
 * GPU census (diagnostic): what a title uses of the emulated GPU path.
 *
 * Counts, per run, every PM4 packet the command processor executes (type-0
 * register runs by register range, type-1, type-2, type-3 by opcode from the
 * primary ring or an indirect buffer, predicated or not), every register the
 * ring writes below the constant ranges, the memory the command processor
 * writes for the CPU (fences, scratch and read-pointer write-back, screen
 * extents, ZPD reports, MEM_WRITE / COND_WRITE / REG_TO_MEM) by address, every
 * WAIT_REG_MEM by poll target with the time waited, CP and vblank interrupts,
 * the CPU's MMIO accesses to GPU registers, draws by primitive / index source /
 * EDRAM mode, resolves and the resolves read back to the CPU, memexport draws.
 * A renderer that replaces the GPU plugin has to provide all of it (or prove it
 * unused).
 *
 * Off by default (SetEnabled, set by the title). While off, the command
 * processor reads the switch once per ring burst and nothing else changes.
 * Lives in rexruntime (shared by the app and the GPU plugin); the title reads
 * the counters directly (Get) and formats them.
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics::gpu_census {

// The switch (relaxed atomic).
void SetEnabled(bool enabled);
bool Enabled();

using Counter = std::atomic<uint64_t>;

// A counter with a single writer thread (the command processor's unless noted): relaxed load and store, no
// locked read-modify-write. Readers on other threads see monotonic values.
inline void Add(Counter& counter, uint64_t n = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
// A counter with several writer threads.
inline void AddShared(Counter& counter, uint64_t n = 1) {
  counter.fetch_add(n, std::memory_order_relaxed);
}
inline void MaxShared(Counter& counter, uint64_t value) {
  uint64_t seen = counter.load(std::memory_order_relaxed);
  while (seen < value &&
         !counter.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

// Sources of a packet.
constexpr uint32_t kPrimary = 0;   // the primary ring
constexpr uint32_t kIndirect = 1;  // inside an INDIRECT_BUFFER (any depth)

// Register ranges of type-0 runs (and of the constant packets).
enum Range : uint32_t {
  kRangeConfig = 0,  // 0x0000-0x1FFF: CP, RBBM, scratch, coherency, display controller (DC_LUT, D1*)
  kRangeContext,     // 0x2000-0x3FFF: render state
  kRangeAluVertex,   // 0x4000-0x43FF: vertex shader float constants
  kRangeAluPixel,    // 0x4400-0x47FF: pixel shader float constants
  kRangeFetch,       // 0x4800-0x48FF: fetch constants
  kRangeBool,        // 0x4900-0x4907: bool constants
  kRangeLoop,        // 0x4908-0x4927: loop constants
  kRangeOther,       // 0x4928 and above
  kRangeCount,
};
inline Range RangeOf(uint32_t index) {
  if (index < 0x2000) return kRangeConfig;
  if (index < 0x4000) return kRangeContext;
  if (index < 0x4400) return kRangeAluVertex;
  if (index < 0x4800) return kRangeAluPixel;
  if (index < 0x4900) return kRangeFetch;
  if (index < 0x4908) return kRangeBool;
  if (index < 0x4928) return kRangeLoop;
  return kRangeOther;
}
// First register of the next range (end of index's range, exclusive).
inline uint32_t RangeEnd(uint32_t index) {
  if (index < 0x2000) return 0x2000;
  if (index < 0x4000) return 0x4000;
  if (index < 0x4400) return 0x4400;
  if (index < 0x4800) return 0x4800;
  if (index < 0x4900) return 0x4900;
  if (index < 0x4908) return 0x4908;
  if (index < 0x4928) return 0x4928;
  return 0xFFFFFFFFu;
}

// Registers whose writes are counted one by one (through CommandProcessor::WriteRegister): everything below the
// constants. Constants are counted per packet (ranges above).
constexpr uint32_t kCountedRegisters = 0x4000;

// Keyed rows: (kind, a, b) -> counters. Insert-only, any thread; a full table drops the event (rows_dropped).
enum class RowKind : uint8_t {
  kNone = 0,
  // a = poll address (memory, endian bits cleared) or register index; b = WAIT_REG_MEM info & 0x17 (function,
  // bit 4 = memory). count = packets, aux = packets that had to wait, sum = microseconds waited, max = longest
  // wait (us), last0 = reference, last1 = mask.
  kWaitRegMem,
  // a = address; b = initiator event (bits 0-5) | 0x80 when the vblank counter is written (initiator bit 31).
  // last0 = value written (host order, before the endian swap).
  kEventWriteShd,
  // a = address; b = initiator event.
  kEventWriteExt,
  // a = 0; b = initiator event. aux = packets that carried an address (count > 1).
  kEventWrite,
  // a = RB_SAMPLE_COUNT_ADDR (0: no report); b = initiator event.
  kEventWriteZpd,
  // a = first address; sum = dwords; last0 = first dword.
  kMemWrite,
  // a = write target (memory address or register); b = info & 0x117 (bit 8 = memory target, bit 4 = memory poll).
  // aux = writes done (condition met); last0 = poll address / register; last1 = reference.
  kCondWrite,
  // a = address; b = register.
  kRegToMem,
  // a = register.
  kRegRmw,
  // a = address; b = scratch register 0-7. last0 = value.
  kScratchWrite,
  // a = address. last0 = read index written.
  kRptrWrite,
  // XE_SWAP: a = frontbuffer address; b = format of fetch constant 0. last0 = width, last1 = height (packet).
  kSwap,
  // a = written address; b = reason (0 = a resolve_readback range, 1 = readback_resolve mode, 2 = pass dump).
  // sum = bytes.
  kResolveReadback,
  // CPU MMIO access (GraphicsSystem::ReadRegister / WriteRegister, any thread): a = register. last0 = value.
  kMmioRead,
  kMmioWrite,
  // Interrupt dispatched to the title (any thread): a = CPU; b = source (0 = vblank, 1 = command processor).
  // sum = microseconds in the guest handler, max = longest.
  kInterrupt,
  // a = INVALIDATE_STATE mask.
  kInvalidateState,
  // IM_LOAD_IMMEDIATE (D3D's internal shaders): a = size in dwords; b = shader type (0 vertex, 1 pixel).
  kImLoadImmediate,
  // A resolve (draw with EDRAM mode copy): a = destination format (RB_COPY_DEST_INFO bits 7-12) |
  // copy_src_select << 8 (4 = depth) | colour clear << 12 | depth clear << 13 (RB_COPY_CONTROL bits 8, 9).
  // last0 = RB_COPY_DEST_BASE.
  kResolve,
};

struct Row {
  std::atomic<uint64_t> key;  // 0 = empty; (kind << 56) | (b << 32) | a
  Counter count;
  Counter aux;
  Counter sum;
  Counter max;
  std::atomic<uint32_t> last0;
  std::atomic<uint32_t> last1;

  RowKind kind() const { return RowKind(key.load(std::memory_order_acquire) >> 56); }
  uint32_t a() const { return uint32_t(key.load(std::memory_order_acquire)); }
  uint32_t b() const { return uint32_t(key.load(std::memory_order_acquire) >> 32) & 0xFFFFFF; }
};
constexpr uint32_t kRowCount = 4096;  // power of two
// Each kind keeps at most this many keys; later keys of the kind go to its "other" row (kOtherA, kOtherB), so
// one kind with many addresses (screen extents, say) cannot crowd the others out.
constexpr uint32_t kRowsPerKind = 128;
constexpr uint32_t kOtherA = 0xFFFFFFFFu;
constexpr uint32_t kOtherB = 0xFFFFFFu;

struct Census {
  // Ring bursts (ExecutePrimaryBuffer) and their dwords; read-pointer write-backs are rows (kRptrWrite).
  Counter bursts;
  Counter burst_dwords;

  // Packets by type and source.
  Counter type0_packets[2];
  Counter type0_range_packets[kRangeCount];  // by the range of the run's first register
  Counter type0_range_dwords[kRangeCount];   // dwords split over the ranges they land in
  Counter type0_single_register_packets;     // the one-register form (bit 15)
  Counter type1_packets[2];
  Counter type2_packets[2];
  Counter type3_packets[128][2];  // [opcode][source]
  Counter type3_dwords[128];      // payload dwords
  Counter type3_predicated[128];  // header bit 0 set
  Counter type3_skipped[128];     // predicated and skipped (bin check failed, or a predicated XE_SWAP)
  Counter type3_unhandled[128];   // opcodes the command processor does not implement
  Counter zero_headers;           // header 0 (skipped as a NOP)

  // Registers below the constants, written through CommandProcessor::WriteRegister (any packet: type-0/1,
  // REG_RMW, COND_WRITE, SET_CONSTANT REGISTERS, the CP's own writes such as VGT_EVENT_INITIATOR).
  Counter register_writes[kCountedRegisters];

  // Constant packets: [type: 0 ALU, 1 FETCH, 2 BOOL, 3 LOOP, 4 REGISTERS, 5 other][0 packets, 1 dwords].
  Counter set_constant[6][2];
  Counter load_alu_constant[6][2];
  Counter set_constant2[2];
  Counter set_shader_constants[2];

  // Indirect buffers executed (INDIRECT_BUFFER and _PFD: type3_packets tells them apart), their dwords and the
  // deepest nesting.
  Counter indirect_buffers;
  Counter indirect_dwords;
  Counter indirect_max_depth;

  // Draws (DRAW_INDX / DRAW_INDX_2 packets that reached the backend or failed in it).
  Counter draws[2];                 // [source]
  Counter draws_indx2;              // DRAW_INDX_2 (no viz token)
  Counter draw_primitive[64];       // VGT_DRAW_INITIATOR prim_type
  Counter draw_source_select[4];    // DMA, immediate, auto-index, invalid
  Counter draw_edram_mode[8];       // RB_MODECONTROL edram_mode (6 = copy: the resolves)
  Counter draw_msaa[4];             // RB_SURFACE_INFO msaa_samples (non-copy draws)
  Counter draw_index32;             // 32-bit indices (DMA)
  Counter draw_primitive_reset;     // PA_SU_SC_MODE_CNTL multi_prim_ib_ena with DMA indices
  Counter draw_viz_conditional;     // DRAW_INDX viz token bit 8 (draw predicated on a VIZ query)
  Counter draw_tessellated;         // VGT_OUTPUT_PATH_CNTL path_select = tessellation
  Counter draw_explicit_major;      // explicit major mode (VGT_DRAW_INITIATOR or prim >= 0x10)
  Counter draw_single_point;        // POINTLIST with 1 index (D3D's GPR-allocation flush draws)
  Counter draw_indices;             // sum of num_indices
  Counter draw_failed;              // the backend returned false
  Counter draw_memexport;           // the backend saw a memexport shader (D3D12 IssueDraw)

  // Resolves (draws with EDRAM mode copy); rows kResolve by format, kResolveReadback for CPU readbacks.
  Counter resolves;
  Counter resolve_color_clear;
  Counter resolve_depth_clear;
  Counter resolve_depth_source;
  Counter resolve_readbacks[3];       // [reason, see kResolveReadback]
  Counter resolve_readback_bytes[3];

  // Shaders.
  Counter im_load[2];            // [0 vertex, 1 pixel]
  Counter im_load_immediate[2];
  Counter im_load_dwords;

  // Synchronisation.
  Counter wait_reg_mem;           // packets
  Counter wait_reg_mem_waited;    // packets whose first poll did not match
  Counter wait_reg_mem_us;        // microseconds waited
  Counter wait_reg_mem_paced;     // waited with wait >= 0x100 (sleeps until the next vblank)
  Counter interrupt_packets[6];   // INTERRUPT packet CPU mask bits
  Counter coherency_requests;     // COHER_STATUS_HOST writes (bit 31 set)
  Counter coherency_flushes;      // MakeCoherent found the status set
  Counter scratch_writes;         // scratch write-back stores (rows kScratchWrite)
  Counter rptr_writes;            // read-pointer write-back stores (rows kRptrWrite)
  Counter xe_swaps;               // XE_SWAP packets

  // Type-3 packets with side effects worth a count of their own (the rest is in type3_packets).
  Counter viz_query[2];           // [0 begin, 1 end]
  Counter zpd_events;             // EVENT_WRITE_ZPD with a report address
  Counter zpd_reports_queued;     // ... sent to the host query path (non-fake modes)
  Counter gamma_256_writes;       // DC_LUT 256-entry table components written
  Counter gamma_pwl_writes;       // DC_LUT PWL components written

  // Graphics system (several writer threads: AddShared).
  Counter vblanks;                 // GraphicsSystem::MarkVblank
  Counter interrupts[2];           // dispatched [0 vblank, 1 command processor]
  Counter interrupts_no_callback;  // dispatch with no callback registered
  Counter set_interrupt_callback;  // VdSetGraphicsInterruptCallback
  std::atomic<uint32_t> interrupt_callback;
  std::atomic<uint32_t> interrupt_callback_data;
  Counter mmio_reads;
  Counter mmio_writes;

  // Keyed rows.
  Row rows[kRowCount];
  Counter rows_dropped;
};

// The census (static storage in rexruntime).
Census& Get();

// Finds or inserts the row (kind, a, b), or the kind's "other" row once the kind has kRowsPerKind keys; nullptr
// when the table is full (counted in rows_dropped). b keeps 24 bits.
Row* FindRow(RowKind kind, uint32_t a, uint32_t b);

// One event on a row (any thread). Returns the row (nullptr when full).
inline Row* CountRow(RowKind kind, uint32_t a, uint32_t b = 0, uint64_t sum = 0) {
  Row* row = FindRow(kind, a, b);
  if (row) {
    row->count.fetch_add(1, std::memory_order_relaxed);
    if (sum) {
      row->sum.fetch_add(sum, std::memory_order_relaxed);
    }
  }
  return row;
}

}  // namespace rex::graphics::gpu_census
