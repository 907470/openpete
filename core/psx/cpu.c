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
#include <stdio.h>
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

/* --- Above-RAM access census (OPENPETE_ARENA_CENSUS) ----------------------
 * Counts accesses whose physical address lies above the 2 MB of main RAM
 * and below expansion region 1 (0x1F000000), reached through the KUSEG,
 * KSEG0 or KSEG1 alias, in three bands around the guest arena window
 * (mod_arena.c ARENA_BASE 0x80700000, ARENA_SIZE 8 MB). Kinds: a bus read
 * or write no host region maps (miss), a host span lookup that resolves
 * nothing, a lookup a registered region resolves (span_ptr_region, bus and
 * host together), and the host share of those hits. Counters are relaxed
 * atomics because host span lookups also run off the simulation thread;
 * the first distinct bus-miss word addresses are recorded with no lock, so
 * the list may hold a duplicate. Off unless the variable is set; the
 * report runs at exit (census_report). */
#define CENSUS_LO       0x00200000u
#define CENSUS_ARENA_LO 0x00700000u
#define CENSUS_ARENA_HI 0x00F00000u
#define CENSUS_HI       0x1F000000u
#define CENSUS_SEEN_MAX 32u

enum { CK_READ_MISS, CK_WRITE_MISS, CK_HOST_MISS, CK_REGION_HIT, CK_HOST_HIT, CK_KINDS };
enum { CB_BELOW, CB_ARENA, CB_ABOVE, CB_BANDS };
enum { CS_KUSEG, CS_KSEG0, CS_KSEG1, CS_SEGS };

typedef struct {
    uint64_t n[CK_KINDS][CB_BANDS][CS_SEGS];
} census_counts_t;

static int             s_census_on = 0;
static census_counts_t s_census;
static uint32_t        s_census_seen_addr[CENSUS_SEEN_MAX];
static uint8_t         s_census_seen_kind[CENSUS_SEEN_MAX];
static uint64_t        s_census_seen_n[CENSUS_SEEN_MAX];
static uint32_t        s_census_seen_count = 0;

/** @brief Classify an address into a census band and alias segment.
 *
 * @param addr Guest virtual address.
 * @param band Receives CB_BELOW, CB_ARENA or CB_ABOVE.
 * @param seg Receives CS_KUSEG, CS_KSEG0 or CS_KSEG1.
 * @return 1 when the address is a RAM-segment alias above 2 MB and below
 *         0x1F000000, else 0. */
static int census_classify(uint32_t addr, unsigned* band, unsigned* seg) {
    uint32_t top = addr >> 29;
    if (top == 0u) *seg = CS_KUSEG;
    else if (top == 4u) *seg = CS_KSEG0;
    else if (top == 5u) *seg = CS_KSEG1;
    else return 0;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < CENSUS_LO || phys >= CENSUS_HI) return 0;
    *band = phys < CENSUS_ARENA_LO ? CB_BELOW : (phys < CENSUS_ARENA_HI ? CB_ARENA : CB_ABOVE);
    return 1;
}

/** @brief Record the word address of a bus miss in the first-seen list.
 *
 * @param addr Guest address of the access.
 * @param kind CK_READ_MISS or CK_WRITE_MISS. */
static void census_seen(uint32_t addr, unsigned kind) {
    uint32_t word = addr & ~3u;
    uint32_t n = __atomic_load_n(&s_census_seen_count, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; i++) {
        if (s_census_seen_addr[i] == word && s_census_seen_kind[i] == kind) {
            __atomic_fetch_add(&s_census_seen_n[i], 1u, __ATOMIC_RELAXED);
            return;
        }
    }
    if (n >= CENSUS_SEEN_MAX) return;
    s_census_seen_addr[n] = word;
    s_census_seen_kind[n] = (uint8_t)kind;
    s_census_seen_n[n] = 1u;
    __atomic_store_n(&s_census_seen_count, n + 1u, __ATOMIC_RELAXED);
}

/** @brief Count one access of @p kind at @p addr when the census is on and the address is in its range.
 *
 * Out of line and cold: every caller sits on a path main RAM and the
 * scratchpad never take.
 * @param addr Guest address of the access.
 * @param kind One of the CK_ kinds. */
static __attribute__((noinline, cold)) void census_note(uint32_t addr, unsigned kind) {
    unsigned band, seg;
    if (!s_census_on || !census_classify(addr, &band, &seg)) return;
    __atomic_fetch_add(&s_census.n[kind][band][seg], 1u, __ATOMIC_RELAXED);
    if (kind == CK_READ_MISS || kind == CK_WRITE_MISS) census_seen(addr, kind);
}

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
    if (psx_runtime_resolve_region(addr, &host, &rem) && len <= rem) {
        census_note(addr, CK_REGION_HIT);
        return (uint8_t*)host;
    }
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
    uint8_t* p = span_ptr(addr, len);
    if ((addr & 0x1FFFFFFFu) >= RAM_SIZE) census_note(addr, p ? CK_HOST_HIT : CK_HOST_MISS);
    return p;
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
    census_note(addr, CK_READ_MISS);
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
    if (!p) { census_note(addr, CK_WRITE_MISS); return; }
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
    if (!p) { census_note(addr, CK_WRITE_MISS); return; }
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
    census_note(addr, CK_READ_MISS);
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
    census_note(addr, CK_WRITE_MISS);
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

/* --- Above-RAM census report ---------------------------------------------- */

static const char* const k_census_kind[CK_KINDS] = {
    "read_miss", "write_miss", "host_miss", "region_hit", "host_hit"
};
static const char* const k_census_band[CB_BANDS] = {
    "below_arena", "arena", "above_arena"
};

/** @brief Sum one kind of one band over the three alias segments.
 *
 * @param c Counts to read.
 * @param kind One of the CK_ kinds.
 * @param band One of the CB_ bands.
 * @return The total. */
static uint64_t census_sum(const census_counts_t* c, unsigned kind, unsigned band) {
    return c->n[kind][band][CS_KUSEG] + c->n[kind][band][CS_KSEG0] + c->n[kind][band][CS_KSEG1];
}

/** @brief Write the census report to @p f.
 *
 * One line per band with each kind as total(kuseg/kseg0/kseg1), one line
 * per recorded bus-miss word address, the probe line, and a summary line
 * whose game_miss counts bus reads and writes in every band.
 * @param f Destination stream.
 * @param c Counts taken before the probe.
 * @param probe Count deltas the probe produced, arena band summed over segments.
 * @param control The probe verdict text. */
static void census_emit(FILE* f, const census_counts_t* c, const uint64_t probe[CK_KINDS],
                        const char* control) {
    uint64_t game_miss = 0;
    for (unsigned b = 0; b < CB_BANDS; b++) {
        fprintf(f, "[arena-census] band=%s", k_census_band[b]);
        for (unsigned k = 0; k < CK_KINDS; k++) {
            const uint64_t* s = c->n[k][b];
            fprintf(f, " %s=%llu(%llu/%llu/%llu)", k_census_kind[k],
                    (unsigned long long)census_sum(c, k, b), (unsigned long long)s[CS_KUSEG],
                    (unsigned long long)s[CS_KSEG0], (unsigned long long)s[CS_KSEG1]);
        }
        fputc('\n', f);
        game_miss += census_sum(c, CK_READ_MISS, b) + census_sum(c, CK_WRITE_MISS, b);
    }
    uint32_t seen = __atomic_load_n(&s_census_seen_count, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < seen; i++)
        fprintf(f, "[arena-census] addr=0x%08X kind=%s n=%llu\n", s_census_seen_addr[i],
                k_census_kind[s_census_seen_kind[i]], (unsigned long long)s_census_seen_n[i]);
    fprintf(f, "[arena-census] probe read_miss=+%llu write_miss=+%llu host_miss=+%llu "
               "region_hit=+%llu host_hit=+%llu control=%s\n",
            (unsigned long long)probe[CK_READ_MISS], (unsigned long long)probe[CK_WRITE_MISS],
            (unsigned long long)probe[CK_HOST_MISS], (unsigned long long)probe[CK_REGION_HIT],
            (unsigned long long)probe[CK_HOST_HIT], control);
    fprintf(f, "[arena-census] summary game_miss=%llu arena_miss=%llu arena_region_hit=%llu "
               "arena_host_hit=%llu control=%s\n",
            (unsigned long long)game_miss,
            (unsigned long long)(census_sum(c, CK_READ_MISS, CB_ARENA) +
                                 census_sum(c, CK_WRITE_MISS, CB_ARENA)),
            (unsigned long long)census_sum(c, CK_REGION_HIT, CB_ARENA),
            (unsigned long long)census_sum(c, CK_HOST_HIT, CB_ARENA), control);
}

/** @brief Report the census at exit after a probe control inside the arena window.
 *
 * Snapshots the counts, then reads the window's first words through the
 * KUSEG, KSEG0 and KSEG1 aliases, rewrites one word with the value it
 * holds (no byte changes; dropped when no region maps it) and makes one
 * host span lookup. With the arena unregistered the probe must add 4 read
 * misses, 1 write miss and 1 host miss; with it registered, 6 region hits
 * (4 reads, 1 write, 1 host) of which 1 host hit. Anything else is FAIL.
 * The report goes to stderr and, when OPENPETE_ARENA_CENSUS is a path
 * rather than 1, is appended to that file. */
static void census_report(void) {
    census_counts_t before = s_census;
    (void)read_word(0x00700000u);
    (void)read_word(0x80700004u);
    (void)read_word(0xA0700008u);
    write_word(0x8070000Cu, read_word(0x8070000Cu));
    (void)openpete_psx_to_host_span(0x80700010u, 4u);
    uint64_t probe[CK_KINDS];
    for (unsigned k = 0; k < CK_KINDS; k++)
        probe[k] = census_sum(&s_census, k, CB_ARENA) - census_sum(&before, k, CB_ARENA);
    const int miss = probe[CK_READ_MISS] == 4u && probe[CK_WRITE_MISS] == 1u &&
                     probe[CK_HOST_MISS] == 1u && probe[CK_REGION_HIT] == 0u;
    const int hit = probe[CK_REGION_HIT] == 6u && probe[CK_HOST_HIT] == 1u &&
                    probe[CK_READ_MISS] == 0u && probe[CK_WRITE_MISS] == 0u;
    const char* control = miss ? "PASS(unregistered)" : (hit ? "PASS(registered)" : "FAIL");
    census_emit(stderr, &before, probe, control);
    const char* e = getenv("OPENPETE_ARENA_CENSUS");
    if (e && strcmp(e, "1") != 0) {
        FILE* f = fopen(e, "a");
        if (f) { census_emit(f, &before, probe, control); fclose(f); }
    }
}

/** @brief Turn the census on when OPENPETE_ARENA_CENSUS is set and not 0, once per process. */
static void census_init(void) {
    static int armed = 0;
    if (armed) return;
    armed = 1;
    const char* e = getenv("OPENPETE_ARENA_CENSUS");
    if (!e || !*e || *e == '0') return;
    s_census_on = 1;
    atexit(census_report);
}

/* --- Public API ---------------------------------------------------------- */

void openpete_psx_runtime_init(CPUState* cpu) {
    census_init();
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
