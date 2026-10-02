// core/psx/vstream.c - see openpete_vstream.h.

#include "openpete_vstream.h"
#include "openpete_log.h"
#include "openpete_psx.h"    /* openpete_psx_get_ram */
#include "psx_runtime_internal.h"     /* psx_runtime_resolve_region; hook prototypes */

#include <stdlib.h>
#include <string.h>

static uint32_t s_table[VSTREAM_HANDLE_MAX]; /* index -> vaddr; [0] unused */
static openpete_vstream_census_t s_census;
static int s_fatal = -1;

/** @brief Record a frame-word field below the handle bound that no handle backs.
 *
 * Counts the miss in the census; the first one logs an error and keeps the
 * field value. Aborts the process when OPENPETE_VSTREAM_FATAL is set to a
 * value not starting with 0, read once on the first call.
 * @param masked The 21-bit field value that has no table entry. */
static void unresolved(uint32_t masked) {
    if (s_census.unresolved++ == 0) {
        s_census.first_unresolved = masked;
        OPL_ERROR_F("vstream", "frame word field %#x is below the handle "
                    "bound and no handle is bound to it - a stream the "
                    "game will read from address 0 (garbage vertices); "
                    "either a mod wrote a handle it never requested, or a "
                    "stock stream sits in the kernel area", masked);
    }
    if (s_fatal < 0) {
        const char* e = getenv("OPENPETE_VSTREAM_FATAL");
        s_fatal = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_fatal) abort();
}

uint32_t openpete_vstream_addr(uint32_t masked, int halved) {
    masked &= 0x1FFFFFu;
    if (masked >= VSTREAM_HANDLE_BOUND) {
        s_census.stock++;
        return halved ? masked << 1 : masked;   /* stock: bit-identical */
    }
    uint32_t v = s_table[masked];
    if (!v) { unresolved(masked); return 0; }
    s_census.handle++;
    return v;
}

uint32_t openpete_vstream_resolve(uint32_t masked, int halved,
                                  const uint8_t** host, uint32_t* remaining) {
    uint32_t va = openpete_vstream_addr(masked, halved);
    if (host) *host = NULL;
    if (remaining) *remaining = 0;
    if (!va) return 0;
    uint32_t phys = va & 0x1FFFFFFFu;
    if (phys < OPENPETE_PSX_RAM_SIZE) {
        if (host) *host = openpete_psx_get_ram() + phys;
        if (remaining) *remaining = OPENPETE_PSX_RAM_SIZE - phys;
        return va;
    }
    void* h = NULL; uint32_t rem = 0;
    if (psx_runtime_resolve_region(va, &h, &rem)) {
        if (host) *host = (const uint8_t*)h;
        if (remaining) *remaining = rem;
    }
    return va;
}

int openpete_vstream_table_set(uint32_t index, uint32_t vaddr) {
    if (index == 0 || index >= VSTREAM_HANDLE_MAX || vaddr == 0) return 0;
    if (s_table[index] && s_table[index] != vaddr) {
        OPL_ERROR_F("vstream", "handle %u is bound to 0x%08X; refusing to "
                    "rebind it to 0x%08X", index, s_table[index], vaddr);
        return 0;
    }
    s_table[index] = vaddr;
    return 1;
}

uint32_t openpete_vstream_table_find(uint32_t vaddr) {
    if (!vaddr) return 0;
    for (uint32_t i = 1; i < VSTREAM_HANDLE_MAX; i++)
        if (s_table[i] == vaddr) return i;
    return 0;
}

uint32_t openpete_vstream_table_get(uint32_t index) {
    return index < VSTREAM_HANDLE_MAX ? s_table[index] : 0;
}

void openpete_vstream_census(openpete_vstream_census_t* out) {
    if (out) *out = s_census;
}

/* The substituted `srl` sites receive the frame word already shifted
 * left by 11 in the shift's source register; undo that and resolve. */
uint32_t psx_runtime_vstream_moby(uint32_t rt)   { return openpete_vstream_addr(rt >> 11, 0); }
uint32_t psx_runtime_vstream_halved(uint32_t rt) { return openpete_vstream_addr(rt >> 11, 1); }
