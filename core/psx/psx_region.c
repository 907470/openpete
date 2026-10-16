/* core/psx/psx_region.c - PSX RAM virtualization (presentation-side
 * region table).
 *
 * Strategy: substitute-only at hot paths, region table at cold paths.
 * The hot recompiler-emitted `g_ram[addr & 0x1FFFFF]` accesses are NOT
 * routed through this table - consumers redirect those via psxrecomp
 * instruction substitutions. This file backs:
 *   - cpu.c's mem_ptr / openpete_psx_to_host_ptr (cold-path resolvers)
 *   - psx_runtime_canonicalize debug guard
 *   - tooling that wants to inspect a region (savestate, harness)
 *
 * Region storage: small static array (cap MAX_REGIONS). Linear scan is
 * fine - we expect single-digit registrations. Bases are stored as
 * physical addresses (vaddr & 0x1FFFFFFFu) so the KUSEG, KSEG0 and KSEG1
 * spellings of the same buffer resolve to the same region; a KSEG2
 * address or a KUSEG address at or above 0x20000000 resolves to none.
 *
 * Host span lookups run off the simulation thread too, so a slot is
 * published with release stores (fields first, then in_use, then the
 * count) and read with acquire loads; a region that grows in place
 * (psx_runtime_grow_region) publishes its new size with a release store,
 * so a lookup sees either the old extent or the new one, never a size
 * whose bytes are not yet backed. */

#include "psx_runtime_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REGIONS        16u
#define CANONICAL_RAM_END  0x00200000u   /* 2 MB main RAM */
#define KSEG0_BASE         0x80000000u   /* unmapped cached segment */
#define SCRATCH_BASE       0x1F800000u
#define SCRATCH_END        0x1F800400u   /* 1 KB scratchpad */

typedef struct {
    uint32_t phys_base;   /* vaddr & 0x1FFFFFFFu */
    uint32_t size;
    uint8_t* host;
    int      in_use;
} psx_region_t;

static psx_region_t s_regions[MAX_REGIONS];
static uint32_t     s_region_count = 0;

/** @brief Strip the KSEG segment bits from a PSX address.
 *
 * @param addr Virtual address.
 * @return The physical address, addr masked to 29 bits. */
static uint32_t to_phys(uint32_t addr) { return addr & 0x1FFFFFFFu; }

/** @brief Test whether an address names physical memory: KUSEG below 0x20000000, KSEG0 or KSEG1.
 *
 * KSEG2 (0xC0000000 and up) is not RAM on the PSX, and a KUSEG address at
 * or above 0x20000000 does not alias the 29-bit physical space; both
 * would otherwise fold onto a region through to_phys.
 * @param addr Virtual address.
 * @return Nonzero when the address may resolve to a region. */
static int names_physical(uint32_t addr) {
    const uint32_t top = addr >> 29;
    return top == 0u || top == 4u || top == 5u;
}

/** @brief Read a slot's in_use flag with acquire ordering, so its other fields are published. */
static int slot_live(const psx_region_t* r) {
    return __atomic_load_n(&r->in_use, __ATOMIC_ACQUIRE);
}

/** @brief Read a slot's size with acquire ordering, so the bytes it covers are backed. */
static uint32_t slot_size(const psx_region_t* r) {
    return __atomic_load_n(&r->size, __ATOMIC_ACQUIRE);
}

/** @brief Test whether the half-open ranges [a_lo, a_lo+a_sz) and [b_lo, b_lo+b_sz) intersect.
 *
 * The end sums are 32-bit and not checked for wrap.
 * @param a_lo Start of the first range.
 * @param a_sz Size of the first range.
 * @param b_lo Start of the second range.
 * @param b_sz Size of the second range.
 * @return Nonzero when they intersect. */
static int ranges_overlap(uint32_t a_lo, uint32_t a_sz,
                          uint32_t b_lo, uint32_t b_sz) {
    return a_lo < b_lo + b_sz && b_lo < a_lo + a_sz;
}

/** @brief Test a physical range against main RAM, the scratchpad and every registered region but one.
 *
 * @param phys Physical start of the range.
 * @param size Length in bytes.
 * @param skip Index of a slot left out of the test (the slot being grown),
 *        or MAX_REGIONS to test every slot.
 * @return 0 when the range is free, 3 when it overlaps main RAM, 4 the
 *         scratchpad, 5 another region. */
static int range_clashes(uint32_t phys, uint32_t size, uint32_t skip) {
    if (ranges_overlap(phys, size, 0u, CANONICAL_RAM_END)) return 3;
    if (ranges_overlap(phys, size, SCRATCH_BASE, SCRATCH_END - SCRATCH_BASE)) return 4;
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (i == skip || !s_regions[i].in_use) continue;
        if (ranges_overlap(phys, size, s_regions[i].phys_base, s_regions[i].size)) return 5;
    }
    return 0;
}

uint32_t psx_runtime_region_count(void) {
    return __atomic_load_n(&s_region_count, __ATOMIC_ACQUIRE);
}

int psx_runtime_register_region(uint32_t psx_base,
                                uint32_t size,
                                void*    host_buffer) {
    if (size == 0u || host_buffer == NULL) return 1;
    if (s_region_count >= MAX_REGIONS) return 2;

    uint32_t phys = to_phys(psx_base);

    const int clash = range_clashes(phys, size, MAX_REGIONS);
    if (clash) return clash;

    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (s_regions[i].in_use) continue;
        s_regions[i].phys_base = phys;
        s_regions[i].host      = (uint8_t*)host_buffer;
        __atomic_store_n(&s_regions[i].size, size, __ATOMIC_RELEASE);
        __atomic_store_n(&s_regions[i].in_use, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&s_region_count, s_region_count + 1u, __ATOMIC_RELEASE);
        return 0;
    }
    return 2; /* unreachable while count check holds */
}

int psx_runtime_grow_region(uint32_t psx_base, uint32_t new_size) {
    const uint32_t phys = to_phys(psx_base);
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!s_regions[i].in_use || s_regions[i].phys_base != phys) continue;
        if (new_size < s_regions[i].size) return 6;
        const int clash = range_clashes(phys, new_size, i);
        if (clash) return clash;
        __atomic_store_n(&s_regions[i].size, new_size, __ATOMIC_RELEASE);
        return 0;
    }
    return 7;
}

void psx_runtime_unregister_region(uint32_t psx_base) {
    uint32_t phys = to_phys(psx_base);
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!s_regions[i].in_use) continue;
        if (s_regions[i].phys_base != phys) continue;
        __atomic_store_n(&s_regions[i].in_use, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_regions[i].size, 0u, __ATOMIC_RELEASE);
        s_regions[i].host = NULL;
        if (s_region_count)
            __atomic_store_n(&s_region_count, s_region_count - 1u, __ATOMIC_RELEASE);
        return;
    }
}

int psx_runtime_resolve_region(uint32_t psx_addr,
                               void**   out_host,
                               uint32_t* out_remaining) {
    if (psx_runtime_region_count() == 0u || !names_physical(psx_addr)) return 0;
    uint32_t phys = to_phys(psx_addr);
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!slot_live(&s_regions[i])) continue;
        uint32_t base = s_regions[i].phys_base;
        uint32_t size = slot_size(&s_regions[i]);
        if (phys < base || phys >= base + size) continue;
        uint32_t off = phys - base;
        if (out_host)      *out_host      = s_regions[i].host + off;
        if (out_remaining) *out_remaining = size - off;
        return 1;
    }
    return 0;
}

int psx_runtime_region_host_to_vaddr(const void* host, uint32_t* out_vaddr) {
    if (!host || psx_runtime_region_count() == 0u) return 0;
    const uint8_t* p = (const uint8_t*)host;
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!slot_live(&s_regions[i])) continue;
        const uint8_t* base = s_regions[i].host;
        if (p < base || p >= base + slot_size(&s_regions[i])) continue;
        if (out_vaddr)
            *out_vaddr = KSEG0_BASE | (s_regions[i].phys_base
                                       + (uint32_t)(p - base));
        return 1;
    }
    return 0;
}

uint32_t psx_runtime_canonicalize(uint32_t addr) {
#ifdef OPENPETE_DEBUG_REGION_GUARD
    if (s_region_count != 0u) {
        uint32_t phys = to_phys(addr);
        for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
            if (!s_regions[i].in_use) continue;
            if (phys >= s_regions[i].phys_base &&
                phys <  s_regions[i].phys_base + s_regions[i].size) {
                fprintf(stderr,
                        "psx_runtime_canonicalize: vaddr 0x%08X falls in "
                        "registered region [0x%08X, +0x%X). A canonical-"
                        "only mask site would mirror this back into "
                        "g_ram and corrupt both sides.\n",
                        addr, s_regions[i].phys_base, s_regions[i].size);
                abort();
            }
        }
    }
#endif
    return addr & 0x1FFFFFu;
}
