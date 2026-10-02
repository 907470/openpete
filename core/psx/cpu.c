// core/psx/cpu.c - PSX memory map + CPUState callback bindings.
//
// Game-agnostic. Implements the small slice of the PSX bus that the
// recompiled C touches: 2MB main RAM (KSEG0/KSEG1/KUSEG aliases),
// 1KB scratchpad, and unaligned LWL/LWR/SWL/SWR helpers. The only I/O
// modelled here is the CD-ROM controller registers, the BIOS ROM
// identification bytes and root counter 2's count; IRQ/GPU/SPU live in
// PsyCross.

#include "openpete_psx.h"
#include "openpete_cdrom_mmio.h"
#include "openpete_memcard.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE      OPENPETE_PSX_RAM_SIZE
#define SCRATCH_SIZE  OPENPETE_PSX_SCRATCH_SIZE

/* g_ram is non-static so per-game decomp builds can use linker
 * `--defsym=<game-symbol>=g_ram+<offset>` to alias native C globals to
 * fixed PSX RAM addresses. Recompiler-emitted accesses index it directly;
 * host code reaches it through `openpete_psx_get_ram()`. */
/* Page-aligned so the sub-tick RAM guard can mprotect it (see
 * openpete_subtick_view.h). Alignment is free here and preserves every
 * --defsym alias, which are all g_ram+offset. */
uint8_t g_ram[RAM_SIZE] __attribute__((aligned(4096)));
static uint8_t g_scratch[SCRATCH_SIZE];

uint8_t* openpete_psx_get_ram(void)     { return g_ram; }
uint8_t* openpete_psx_get_scratch(void) { return g_scratch; }

/** @brief Resolve a span through the presentation region table, or NULL.
 *
 * The cold half of span_ptr, kept out of line: its out-parameters are
 * address-taken locals, which inlined into span_ptr would give every bus
 * accessor a stack frame and a stack-protector canary on the main-RAM path.
 * Hot recompiler-emitted accesses to these regions bypass it through
 * per-site psxrecomp instruction substitutions.
 * @param addr Guest address.
 * @param len Span length in bytes.
 * @return The host pointer when the whole span lies in one region, else NULL. */
static __attribute__((noinline, cold)) uint8_t* span_ptr_region(uint32_t addr, uint32_t len) {
    if (psx_runtime_region_count() == 0u) return NULL;
    void* host = NULL;
    uint32_t rem = 0;
    if (psx_runtime_resolve_region(addr, &host, &rem) && len <= rem)
        return (uint8_t*)host;
    return NULL;
}

/** @brief Resolve a guest address to a host pointer valid for @p len bytes, or NULL.
 *
 * The span [addr, addr + len) must lie inside one host region: main RAM
 * (any segment), the scratchpad, or one entry of the presentation region
 * table (span_ptr_region). A span that starts inside a region and runs past
 * its end returns NULL rather than a pointer the caller would overrun. The
 * main-RAM and scratchpad tests take no local's address, so the bus
 * accessors that inline this need no stack frame. */
static uint8_t* span_ptr(uint32_t addr, uint32_t len) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < RAM_SIZE)
        return (len <= RAM_SIZE - phys) ? &g_ram[phys] : NULL;
    if (phys >= 0x1F800000u && phys - 0x1F800000u < SCRATCH_SIZE) {
        uint32_t off = phys - 0x1F800000u;
        return (len <= SCRATCH_SIZE - off) ? &g_scratch[off] : NULL;
    }
    return span_ptr_region(addr, len);
}

void* openpete_psx_to_host_span(uint32_t addr, uint32_t len) {
    if (addr == 0u) return NULL;
    return span_ptr(addr, len);
}

void* openpete_psx_to_host_ptr(uint32_t addr) {
    return openpete_psx_to_host_span(addr, 1u);
}

/** @brief Resolve a guest address to its host byte and the bytes left in its region, or NULL.
 *
 * The region is main RAM (any segment), the scratchpad, or one entry of the
 * presentation region table, as span_ptr() classifies it.
 * @param addr PSX virtual address.
 * @param room Receives the byte count from @p addr to the region's end. */
static uint8_t* region_room(uint32_t addr, uint32_t* room) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < RAM_SIZE) { *room = RAM_SIZE - phys; return &g_ram[phys]; }
    if (phys >= 0x1F800000u && phys - 0x1F800000u < SCRATCH_SIZE) {
        uint32_t off = phys - 0x1F800000u;
        *room = SCRATCH_SIZE - off;
        return &g_scratch[off];
    }
    if (psx_runtime_region_count() != 0u) {
        void* host = NULL;
        uint32_t rem = 0;
        if (psx_runtime_resolve_region(addr, &host, &rem)) { *room = rem; return (uint8_t*)host; }
    }
    return NULL;
}

const char* openpete_psx_to_host_str(uint32_t addr, uint32_t max, uint32_t* len_out) {
    uint32_t room = 0;
    const uint8_t* p = (addr == 0u) ? NULL : region_room(addr, &room);
    if (p == NULL) return NULL;
    const uint32_t scan = (max < room) ? max : room;
    const uint8_t* nul = (const uint8_t*)memchr(p, 0, scan);
    uint32_t len;
    if (nul != NULL) len = (uint32_t)(nul - p);
    else if (max <= room) len = max;
    else return NULL;
    if (len_out != NULL) *len_out = len;
    return (const char*)p;
}

/* Inverse of openpete_psx_to_host_ptr for RAM and scratchpad pointers. */
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
     * computes a "pointer" by arithmetic on a vaddr literal - e.g.
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

/** @brief Map a PSX access of @p n bytes to a host pointer for the bus callbacks; NULL when unmapped.
 *
 * Main RAM and the scratchpad resolve under any segment (the top three
 * address bits are dropped); any other address resolves through the
 * registered presentation region table (psx_region.c). An access whose
 * last byte falls past the end of its region (an unaligned word or half
 * at the top of RAM or the scratchpad) is unmapped. Unlike
 * openpete_psx_to_host_ptr(), address 0 maps to RAM offset 0. */
static uint8_t* mem_ptr(uint32_t addr, uint32_t n) {
    return span_ptr(addr, n);
}

/* --- Aligned reads/writes (caller is responsible for alignment) ---------- */

/* Root counter 2's current count (0x1F801120), a constant per game. */
static uint16_t s_root_counter2 = 0;
void     openpete_psx_set_root_counter2(uint16_t value) { s_root_counter2 = value; }
uint16_t openpete_psx_root_counter2(void) { return s_root_counter2; }
#define ROOT_COUNTER2_COUNT 0x1F801120u

/** @brief Read an unmapped address: the BIOS ROM window, root counter 2's count, or 0.
 *
 * Root counter 2's count register (0x1F801120) answers the per-game
 * constant. Otherwise the value composes little-endian from the ROM
 * identification model (psx_bios.c) one byte at a time; when any byte
 * lies outside the ROM window (physical 0x1FC00000..0x1FC7FFFF) the whole
 * read is 0. Out of line so that the address-taken byte it reads into
 * gives no bus accessor a stack frame on the mapped path.
 * @param addr Guest address.
 * @param n Width in bytes (2 or 4).
 * @return The value, zero-extended. */
static __attribute__((noinline, cold)) uint32_t rom_read(uint32_t addr, unsigned n) {
    uint32_t v = 0;
    if ((addr & 0x1FFFFFFFu) == ROOT_COUNTER2_COUNT) return s_root_counter2;
    for (unsigned i = 0; i < n; i++) {
        uint8_t b;
        if (!openpete_psx_bios_rom_read8((addr + i) & 0x1FFFFFFFu, &b)) return 0;
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}
/** @brief Read a word from the PSX bus; an address with no four mapped bytes reads the ROM model. */
static uint32_t read_word(uint32_t addr) {
    uint8_t* p = mem_ptr(addr, 4u);
    if (!p) return rom_read(addr, 4);
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
/** @brief Write a word to the PSX bus; dropped unless all four bytes map. */
static void write_word(uint32_t addr, uint32_t value) {
    uint8_t* p = mem_ptr(addr, 4u);
    if (!p) return;
    memcpy(p, &value, 4);
}
/** @brief Read a half from the PSX bus; an address with no two mapped bytes reads the ROM model. */
static uint16_t read_half(uint32_t addr) {
    uint8_t* p = mem_ptr(addr, 2u);
    if (!p) return (uint16_t)rom_read(addr, 2);
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
/** @brief Write a half to the PSX bus; dropped unless both bytes map. */
static void write_half(uint32_t addr, uint16_t value) {
    uint8_t* p = mem_ptr(addr, 2u);
    if (!p) return;
    memcpy(p, &value, 2);
}
/** @brief Read one byte no host region maps: a CD-ROM controller register, the BIOS ROM window, or 0.
 *
 * A read of the controller's registers (0x1F801800 to 0x1F801803) goes to
 * the controller model (cdrom_mmio.c), one in the BIOS ROM window to the
 * ROM identification model; every other address reads 0. Out of line so
 * that its address-taken byte gives read_byte no stack frame on the mapped
 * path.
 * @param addr Guest address.
 * @return The byte. */
static __attribute__((noinline, cold)) uint8_t read_byte_unmapped(uint32_t addr) {
    uint8_t v;
    if (((addr & 0x1FFFFFFFu) & ~3u) == 0x1F801800u && openpete_cdrom_mmio_read8(addr & 0x1FFFFFFFu, &v)) return v;
    if (openpete_psx_bios_rom_read8(addr & 0x1FFFFFFFu, &v)) return v;
    return 0u;
}
/** @brief Read one byte from the PSX bus; main RAM, the scratchpad and the region table resolve through mem_ptr(), the rest through read_byte_unmapped. */
static uint8_t read_byte(uint32_t addr) {
    uint8_t* p = mem_ptr(addr, 1u);
    if (p) return *p;
    return read_byte_unmapped(addr);
}
/** @brief Write one byte to the PSX bus; an unmapped CD-ROM register byte goes to the controller model. */
static void write_byte(uint32_t addr, uint8_t value) {
    uint8_t* p = mem_ptr(addr, 1u);
    if (p) { *p = value; return; }
    if (((addr & 0x1FFFFFFFu) & ~3u) == 0x1F801800u) (void)openpete_cdrom_mmio_write8(addr & 0x1FFFFFFFu, value);
}

/* --- Unaligned LWL/LWR/SWL/SWR --------------------------------------------
 * PSX is little-endian. The standard MIPS unaligned-load patterns:
 *   LWL/LWR pair builds a misaligned 32-bit word from two aligned reads.
 * We implement on top of read_word so segment/MMIO routing stays unified. */

/** @brief Execute LWL: merge the high-order bytes of an unaligned word into @p rt.
 *
 * Little-endian R3000 semantics (psx-spx, "CPU Load/Store Opcodes"): LWL
 * and SWL shift by (3 - (addr & 3)) * 8, LWR and SWR by (addr & 3) * 8.
 * The byte at @p addr and those below it down to the aligned word's start
 * replace the top bytes of @p rt, so the unaligned-load idiom `lwr 0(s); lwl 3(s)`
 * assembles the word at every alignment, including func_80022A2C's
 * shaded-moby vertex loop over a 3-byte stride.
 * @return The merged register value. */
static uint32_t do_lwl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word    = read_word(aligned);
    uint32_t shift   = (3u - (addr & 3u)) * 8u;
    uint32_t mask    = 0xFFFFFFFFu << shift;
    return (rt & ~mask) | (word << shift);
}
/** @brief Execute LWR: merge the low-order bytes of an unaligned word into @p rt.
 *
 * The bytes from @p addr up to the aligned word's end replace the low
 * bytes of @p rt, shifted down by (addr & 3) * 8; the rest of @p rt stays.
 * @return The merged register value. */
static uint32_t do_lwr(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t word    = read_word(aligned);
    uint32_t shift   = (addr & 3u) * 8u;
    uint32_t mask    = 0xFFFFFFFFu >> shift;
    return (rt & ~mask) | (word >> shift);
}
/** @brief Execute SWL: store the high-order bytes of @p rt up to @p addr's byte.
 *
 * Reads the aligned word, replaces its bytes from the word start through
 * @p addr with the top bytes of @p rt (shifted down by (3 - (addr & 3)) * 8),
 * and writes it back through write_word. */
static void do_swl(uint32_t addr, uint32_t rt) {
    uint32_t aligned = addr & ~3u;
    uint32_t existing = read_word(aligned);
    uint32_t shift   = (3u - (addr & 3u)) * 8u;
    uint32_t mask    = 0xFFFFFFFFu >> shift;
    write_word(aligned, (existing & ~mask) | (rt >> shift));
}
/** @brief Execute SWR: store the low-order bytes of @p rt from @p addr's byte.
 *
 * Reads the aligned word, replaces its bytes from @p addr through the word
 * end with @p rt shifted up by (addr & 3) * 8, and writes it back through
 * write_word. */
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
    uint8_t* p = mem_ptr(addr, 1u);
    if (!p) {
        OPL_ERROR_F("boot", "psx_load: unmapped target addr=0x%08X size=%u", addr, size);
        return;
    }
    /* size may straddle the end of RAM; clamp on the host side. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t avail = (phys < RAM_SIZE) ? (RAM_SIZE - phys) : 0u;
    if (size > avail) {
        OPL_WARN_F("boot", "psx_load: clamped %u->%u bytes at 0x%08X", size, avail, addr);
        size = avail;
    }
    memcpy(p, data, size);
}
