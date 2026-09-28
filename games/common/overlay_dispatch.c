// games/common/overlay_dispatch.c — overlay-aware dispatch.
//
// `psx_dispatch_compiled` fans out to per-binary dispatchers. Only the overlay
// whose bytes are actually resident may match — every overlay of a game shares
// one vaddr, so dispatching them concurrently would run code that isn't there.
//
// The id-to-dispatcher mapping lives in the auto-generated
// spyro_overlay_table.c, driven by scripts/regen-recomp.sh from the decomp's
// build/wad/*.ovl.

#include "psx_runtime_internal.h"
#include "spyro_overlay_table.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Local declaration instead of openpete_log.h: this TU is part of the
 * recomp-module source kit, which must stay self-contained (openpete_log.h
 * drags in openpete.h). The level value matches OPL_INFO. */
void openpete_logf(int level, const char* subsystem, const char* fmt, ...);
#define DISPATCH_LOG_INFO 2

extern int psx_dispatch_main_exe(CPUState* cpu, uint32_t addr);

static const spyro_overlay_entry_t* s_active = NULL;

/* Host-side re-derivation of the resident overlay, registered at boot by a
 * game whose overlays can arrive without a disc read announcing them. The
 * overlay vaddr and the RAM base are the executable's, so the module reaches
 * them through this pointer rather than importing them. */
static void (*s_resync)(void);

void spyro_overlay_set_resync(void (*fn)(void)) { s_resync = fn; }

const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic) {
    for (size_t i = 0; i < spyro_overlay_table_count; ++i) {
        if (spyro_overlay_table[i].magic == magic) {
            return &spyro_overlay_table[i];
        }
    }
    return NULL;
}

void spyro_set_active_overlay_by_magic(uint32_t magic) {
    const spyro_overlay_entry_t* e = spyro_overlay_find(magic);
    if (!e) return;          /* unknown id — leave current active alone */
    if (e == s_active) return;
    openpete_logf(DISPATCH_LOG_INFO, "dispatch", "active overlay: %s -> %s",
                  s_active ? s_active->name : "none", e->name);
    s_active = e;
}

uint32_t spyro_get_active_overlay_magic(void) {
    return s_active ? s_active->magic : 0u;
}

/* Set by the recompiled code at every `jalr` site, immediately before its
 * call_by_address, and cleared by the first dispatch that follows: the
 * census cannot otherwise tell a `jalr` from a `jr <reg>` that follows a
 * `jal` in the same function, since cpu->ra is the last link, not the site. */
int psx_dispatch_from_jalr = 0;

/* OPENPETE_DISPATCH_CENSUS=<file>: every dispatch whose target lies in the
 * overlay window is appended as
 *   <resident overlay name> <target> <ra> <class> <resolved|missed>
 * where class is `jalr` (a computed call), `jal` (the EXE calling an overlay
 * function by constant), `host` (a BIOS event or PSYQ callback thunk, which
 * dispatch with ra = 0xDEADBEEF) or `jr` (anything else). The window is
 * [spyro_overlay_window_lo, spyro_overlay_window_hi): every overlay of a game
 * loads at one address, and the range runs from that base over the largest
 * overlay's extent. */
static FILE* s_census = NULL;
static int   s_census_checked = 0;

/** @brief Open the dispatch census file on the first dispatch.
 *
 * Opens the path in OPENPETE_DISPATCH_CENSUS for writing; an unset or empty
 * variable, or a failed open, leaves the census off. Runs once. */
static void census_open(void) {
    const char* path;
    s_census_checked = 1;
    path = getenv("OPENPETE_DISPATCH_CENSUS");
    if (!path || !path[0]) return;
    s_census = fopen(path, "w");
}

/** @brief Append one census line for a dispatch into the overlay window.
 *
 * Classifies the call as host for a link register of 0xDEADBEEF, jalr when
 * the recompiled code flagged the site, jal when the instruction two words
 * before the link register is a jal whose target is the address, else jr.
 * Writes nothing for an address outside the overlay window.
 * @param cpu The guest CPU state, read for the jal check.
 * @param addr The dispatch target.
 * @param ra The link register captured before the callee ran.
 * @param from_jalr Non-zero when a jalr site preceded the dispatch.
 * @param resolved Non-zero when a dispatcher ran the target. */
static void census_record(CPUState* cpu, uint32_t addr, uint32_t ra,
                          int from_jalr, int resolved) {
    const char* cls;
    if (addr < spyro_overlay_window_lo || addr >= spyro_overlay_window_hi) return;
    if (ra == 0xDEADBEEFu) {
        cls = "host";
    } else if (from_jalr) {
        cls = "jalr";
    } else if ((ra & 3u) == 0u && ra - 8u >= 0x80000000u &&
               ra - 8u < 0x80200000u) {
        uint32_t site = cpu->read_word(ra - 8u);
        cls = ((site >> 26) == 3u &&
               (((ra - 8u) & 0xF0000000u) | ((site & 0x03FFFFFFu) << 2)) == addr)
              ? "jal" : "jr";
    } else {
        cls = "jr";
    }
    fprintf(s_census, "%s %08X %08X %s %s\n",
            s_active ? s_active->name : "none", addr, ra, cls,
            resolved ? "resolved" : "missed");
}

/** @brief Run the compiled function at a guest address.
 *
 * Tries the main executable's dispatcher first, then the resident overlay's.
 * A dispatch into the overlay window first calls the registered resync hook,
 * because the overlay tracker can name the outgoing overlay, whose table can
 * resolve the address to the wrong function. Captures and clears
 * psx_dispatch_from_jalr and the link register before the callee runs, and
 * appends a census line when the census is open.
 * @param cpu The guest CPU state.
 * @param addr The guest address to run.
 * @return 1 when a dispatcher ran the address, else 0. */
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    /* The class and the link register are read before the callee runs: the
     * callee's own jalr sites rewrite both. */
    int      from_jalr = psx_dispatch_from_jalr;
    uint32_t ra        = cpu->ra;
    int hit;
    psx_dispatch_from_jalr = 0;
    if (psx_dispatch_main_exe(cpu, addr)) return 1;
    /* Confirm the tracker still names the resident overlay before its table
     * decides this call. Two overlays of one game share the vaddr and their
     * entry addresses overlap, so a stale tracker does not reliably miss: it
     * can resolve the address in the outgoing overlay's table and run the
     * wrong function against the incoming overlay's data. Only a dispatch
     * into the overlay window can be wrong this way, and those are a handful
     * per tick, so the check costs one word read where it matters and
     * nothing anywhere else. */
    if (s_resync && addr >= spyro_overlay_window_lo &&
        addr < spyro_overlay_window_hi)
        s_resync();
    hit = s_active ? s_active->fn(cpu, addr) : 0;
    if (!s_census_checked) census_open();
    if (s_census) census_record(cpu, addr, ra, from_jalr, hit);
    return hit;
}
