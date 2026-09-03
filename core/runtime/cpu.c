// core/runtime/cpu.c — PSX memory map + CPUState callback bindings.
//
// Game-agnostic. Implements the small slice of the PSX bus that the
// recompiled C touches: 2MB main RAM (KSEG0/KSEG1/KUSEG aliases),
// 1KB scratchpad, and unaligned LWL/LWR/SWL/SWR helpers. No I/O
// emulation here — IRQ/CD/GPU/SPU live in PsyCross (Phase 1c+).

#include "openpete_psx.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE      OPENPETE_PSX_RAM_SIZE
#define SCRATCH_SIZE  OPENPETE_PSX_SCRATCH_SIZE

/* g_ram is non-static so per-game decomp builds can use linker
 * `--defsym=<game-symbol>=g_ram+<offset>` to alias native C globals to
 * fixed PSX RAM addresses (plan §8 strategy A). Outside that link-time
 * trick, all access still flows through `openpete_psx_get_ram()`. */
/* Page-aligned so the sub-tick RAM guard can mprotect it (see
 * openpete_subtick_view.h). Alignment is free here and preserves every
 * --defsym alias, which are all g_ram+offset. */
uint8_t g_ram[RAM_SIZE] __attribute__((aligned(4096)));
static uint8_t g_scratch[SCRATCH_SIZE];

uint8_t* openpete_psx_get_ram(void)     { return g_ram; }
uint8_t* openpete_psx_get_scratch(void) { return g_scratch; }

void* openpete_psx_to_host_ptr(uint32_t addr) {
    if (addr == 0u) return NULL;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < RAM_SIZE) return &g_ram[phys];
    if (phys >= 0x1F800000u && phys < 0x1F800000u + SCRATCH_SIZE) {
        return &g_scratch[phys - 0x1F800000u];
    }
    /* Cold-path: consult the renderer-side region table. Empty-table
     * fast path handled inside resolve_region. */
    if (psx_runtime_region_count() != 0u) {
        void* host = NULL;
        if (psx_runtime_resolve_region(addr, &host, NULL)) return host;
    }
    return NULL;
}

/* Inverse of openpete_psx_to_host_ptr: maps a host pointer that lives
 * inside g_ram (or scratch) back to its PSX KSEG0 vaddr. NULL → 0;
 * any host pointer outside the recognized regions also → 0 (callers
 * treat that as PSX NULL, which matches PSX-side semantics for "not a
 * valid game-RAM address"). Used by the Phase 3/4 source rewriter when
 * native code assigns a host pointer to a PSX-aliased storage slot. */
uint32_t openpete_host_to_psx(const void* host_ptr) {
    if (host_ptr == NULL) return 0u;
    const uint8_t* p = (const uint8_t*)host_ptr;
    if (p >= g_ram && p < g_ram + RAM_SIZE) {
        return OPENPETE_PSX_KSEG0_BASE | (uint32_t)(p - g_ram);
    }
    if (p >= g_scratch && p < g_scratch + SCRATCH_SIZE) {
        return OPENPETE_PSX_SCRATCH_BASE | (uint32_t)(p - g_scratch);
    }
    /* Integer-cast-to-pointer fallback: matching-decomp source occasionally
     * computes a "pointer" by arithmetic on a vaddr literal — e.g.
     *   levelVram = (char *)0x80200000 - _stacksize - 0x40000;
     * On the host this evaluates to a uintptr whose low 32 bits hold the
     * intended PSX vaddr but whose value lies outside g_ram. If those low
     * bits already look like a KSEG0/KSEG1/KUSEG vaddr, propagate them
     * through; the rewriter's cleaner Pass-4 cast handling is not yet in. */
    uintptr_t raw = (uintptr_t)host_ptr;
    uint32_t lo = (uint32_t)raw;
    uint32_t phys = lo & 0x1FFFFFFFu;
    if ((lo >= OPENPETE_PSX_KSEG0_BASE && lo < OPENPETE_PSX_KSEG0_END) ||
        (lo >= OPENPETE_PSX_KSEG1_BASE && lo < OPENPETE_PSX_KSEG1_END) ||
        (lo < RAM_SIZE && phys < RAM_SIZE)) {
        return lo;
    }
    return 0u;
}

/* Map a PSX virtual address to a host pointer; NULL if unmapped. */
static uint8_t* mem_ptr(uint32_t addr) {
    /* Strip KSEG0/KSEG1/KUSEG segment bits. PSX uses unmapped segments
     * 0x80xxxxxx (cached) and 0xA0xxxxxx (uncached) over the same RAM. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < RAM_SIZE) return &g_ram[phys];
    if (phys >= 0x1F800000u && phys < 0x1F800000u + SCRATCH_SIZE) {
        return &g_scratch[phys - 0x1F800000u];
    }
    /* Cold-path: registered presentation regions. See
     * notes/plans/openpete-ram-virtualization.md. Hot recompiler-
     * emitted accesses bypass this path and should be redirected via
     * per-site psxrecomp instruction substitutions. */
    if (psx_runtime_region_count() != 0u) {
        void* host = NULL;
        if (psx_runtime_resolve_region(addr, &host, NULL)) {
            return (uint8_t*)host;
        }
    }
    return NULL;
}

/* --- Aligned reads/writes (caller is responsible for alignment) ---------- */

static uint32_t read_word(uint32_t addr) {
    uint8_t* p = mem_ptr(addr);
    if (!p) return 0;
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void write_word(uint32_t addr, uint32_t value) {
    uint8_t* p = mem_ptr(addr);
    if (!p) return;
    memcpy(p, &value, 4);
}
static uint16_t read_half(uint32_t addr) {
    uint8_t* p = mem_ptr(addr);
    if (!p) return 0;
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void write_half(uint32_t addr, uint16_t value) {
    uint8_t* p = mem_ptr(addr);
    if (!p) return;
    memcpy(p, &value, 2);
}
static uint8_t read_byte(uint32_t addr) {
    uint8_t* p = mem_ptr(addr);
    return p ? *p : 0u;
}
static void write_byte(uint32_t addr, uint8_t value) {
    uint8_t* p = mem_ptr(addr);
    if (p) *p = value;
}

/* --- Unaligned LWL/LWR/SWL/SWR --------------------------------------------
 * PSX is little-endian. The standard MIPS unaligned-load patterns:
 *   LWL/LWR pair builds a misaligned 32-bit word from two aligned reads.
 * We implement on top of read_word so segment/MMIO routing stays unified. */

/* MIPS R3000 little-endian LWL/LWR/SWL/SWR semantics (psx-spx, "CPU
 * Load/Store Opcodes"): LWL and SWL shift by (3 - (addr & 3)) * 8, LWR and
 * SWR by (addr & 3) * 8.
 * The previous formulas had the (addr&3) and (3 - addr&3) shift expressions
 * swapped between L and R within each pair. The canonical 4-byte unaligned-
 * load idiom `lwr 0(s); lwl 3(s)` only worked at certain alignments,
 * leaving 1-2 bytes of stale data from prior `rt` mixed into the result.
 * Bit `func_80022A2C`'s shaded-moby vertex loop (3-byte vertex stride):
 * once per loop, s2 hit 4-aligned and lwl over-wrote rt with the next
 * word's bytes — producing 0x7F-padding-sourced spike apex vertices in
 * HUD digits, gems, and the loading-text fly-in. */
static uint32_t do_lwl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word    = read_word(aligned);
    uint32_t shift   = (3u - (addr & 3u)) * 8u;
    uint32_t mask    = 0xFFFFFFFFu << shift;
    return (rt & ~mask) | (word << shift);
}
static uint32_t do_lwr(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word    = read_word(aligned);
    uint32_t shift   = (addr & 3u) * 8u;
    uint32_t mask    = 0xFFFFFFFFu >> shift;
    return (rt & ~mask) | (word >> shift);
}
static void do_swl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t existing = read_word(aligned);
    uint32_t shift   = (3u - (addr & 3u)) * 8u;
    uint32_t mask    = 0xFFFFFFFFu >> shift;
    write_word(aligned, (existing & ~mask) | (rt >> shift));
}
static void do_swr(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t existing = read_word(aligned);
    uint32_t shift   = (addr & 3u) * 8u;
    uint32_t mask    = 0xFFFFFFFFu << shift;
    write_word(aligned, (existing & ~mask) | (rt << shift));
}

/* --- Public API ---------------------------------------------------------- */

void openpete_psx_runtime_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));
    memset(g_ram, 0, sizeof(g_ram));
    memset(g_scratch, 0, sizeof(g_scratch));
    cpu->read_word  = read_word;
    cpu->write_word = write_word;
    cpu->read_half  = read_half;
    cpu->write_half = write_half;
    cpu->read_byte  = read_byte;
    cpu->write_byte = write_byte;
    cpu->lwl        = do_lwl;
    cpu->lwr        = do_lwr;
    cpu->swl        = do_swl;
    cpu->swr        = do_swr;
}

void openpete_psx_load(uint32_t addr, const uint8_t* data, uint32_t size) {
    uint8_t* p = mem_ptr(addr);
    if (!p) {
        OPL_ERROR_F("boot", "psx_load: unmapped target addr=0x%08X size=%u", addr, size);
        return;
    }
    /* size may straddle the end of RAM; clamp on the host side. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t avail = (phys < RAM_SIZE) ? (RAM_SIZE - phys) : 0u;
    if (size > avail) {
        OPL_WARN_F("boot", "psx_load: clamped %u→%u bytes at 0x%08X", size, avail, addr);
        size = avail;
    }
    memcpy(p, data, size);
}
