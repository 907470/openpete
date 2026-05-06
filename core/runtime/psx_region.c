/* core/runtime/psx_region.c — PSX RAM virtualization (presentation-side
 * region table). See notes/plans/openpete-ram-virtualization.md.
 *
 * Strategy: substitute-only at hot paths, region table at cold paths.
 * The hot recompiler-emitted `g_ram[addr & 0x1FFFFF]` accesses are NOT
 * routed through this table — consumers redirect those via psxrecomp
 * instruction substitutions. This file backs:
 *   - cpu.c's mem_ptr / openpete_psx_to_host_ptr (cold-path resolvers)
 *   - psx_runtime_canonicalize debug guard
 *   - tooling that wants to inspect a region (savestate, harness)
 *
 * Region storage: small static array (cap MAX_REGIONS). Linear scan is
 * fine — we expect single-digit registrations. Bases are stored as
 * physical addresses (vaddr & 0x1FFFFFFFu) so KSEG0/KSEG1 mirrors of
 * the same buffer resolve to the same region. */

#include "psx_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REGIONS        16u
#define CANONICAL_RAM_END  0x00200000u   /* 2 MB main RAM */
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

static uint32_t to_phys(uint32_t addr) { return addr & 0x1FFFFFFFu; }

/* Inclusive-overlap test: [a_lo, a_lo+a_sz) ∩ [b_lo, b_lo+b_sz) ≠ ∅. */
static int ranges_overlap(uint32_t a_lo, uint32_t a_sz,
                          uint32_t b_lo, uint32_t b_sz) {
    return a_lo < b_lo + b_sz && b_lo < a_lo + a_sz;
}

uint32_t psx_runtime_region_count(void) { return s_region_count; }

int psx_runtime_register_region(uint32_t psx_base,
                                uint32_t size,
                                void*    host_buffer) {
    if (size == 0u || host_buffer == NULL) return 1;
    if (s_region_count >= MAX_REGIONS) return 2;

    uint32_t phys = to_phys(psx_base);

    /* Reject overlap with canonical 2 MB. */
    if (ranges_overlap(phys, size, 0u, CANONICAL_RAM_END)) return 3;
    /* Reject overlap with scratchpad. */
    if (ranges_overlap(phys, size, SCRATCH_BASE,
                       SCRATCH_END - SCRATCH_BASE)) return 4;
    /* Reject overlap with any existing region. */
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!s_regions[i].in_use) continue;
        if (ranges_overlap(phys, size,
                           s_regions[i].phys_base, s_regions[i].size)) {
            return 5;
        }
    }

    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (s_regions[i].in_use) continue;
        s_regions[i].phys_base = phys;
        s_regions[i].size      = size;
        s_regions[i].host      = (uint8_t*)host_buffer;
        s_regions[i].in_use    = 1;
        ++s_region_count;
        return 0;
    }
    return 2; /* unreachable while count check holds */
}

void psx_runtime_unregister_region(uint32_t psx_base) {
    uint32_t phys = to_phys(psx_base);
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!s_regions[i].in_use) continue;
        if (s_regions[i].phys_base != phys) continue;
        s_regions[i].in_use = 0;
        s_regions[i].host   = NULL;
        s_regions[i].size   = 0;
        if (s_region_count) --s_region_count;
        return;
    }
}

int psx_runtime_resolve_region(uint32_t psx_addr,
                               void**   out_host,
                               uint32_t* out_remaining) {
    if (s_region_count == 0u) return 0;
    uint32_t phys = to_phys(psx_addr);
    for (uint32_t i = 0; i < MAX_REGIONS; ++i) {
        if (!s_regions[i].in_use) continue;
        uint32_t base = s_regions[i].phys_base;
        uint32_t size = s_regions[i].size;
        if (phys < base || phys >= base + size) continue;
        uint32_t off = phys - base;
        if (out_host)      *out_host      = s_regions[i].host + off;
        if (out_remaining) *out_remaining = size - off;
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
