// games/spyro-1/features/polybuf_guard.c - polygon-buffer overflow detector.
//
// The game bump-allocates display primitives out of a polygon buffer whose
// cursor lives at D_800757B0 and whose end is D_800756FC. Nothing tests the
// cursor against the end. The world ordering table (g_WorldOT) is allocated
// immediately after the buffer, so once a view emits more primitives than the
// buffer holds, the cursor walks straight into the OT:
//
//   polybuf cursor 0x801C397C   <- 15,820 bytes past the end
//   polybuf end    0x801BFBB0
//   g_WorldOT      0x801BFBB8   <- OT starts 8 bytes after the buffer
//
// Primitives then land on OT buckets. The OT tail-insert in the world
// renderer (r_environment.s 0x800270C8) reads a bucket for "the previous
// primitive in this bucket" and writes a 24-bit link through it without
// checking it - so primitive bytes get dereferenced as a pointer, and three
// bytes are written to a effectively arbitrary address. Observed consequences
// include model vertex streams being scribbled (animated models then decode
// displaced vertices and visibly spike) and, once a write lands on a Moby's
// m_Props, a hang in the moby-free collision-chain unlink.
//
// The buffer is whatever RAM is left after the level data loads, so its size
// varies per level: Haunted Towers has roughly a third the headroom of Tree
// Tops, which is consistent with it being where the hang reproduces.
//
// This module is read-only and always falls through. It samples at
// BuildOrderTable (0x80016784), which runs once per frame after every
// renderer has queued its primitives - so the cursor is at its peak for the
// frame. Reporting is edge-triggered: one line when the buffer first
// overflows, a rate-limited line while it stays overflowed, and a summary
// when it recovers.
//
// OFF by default - this is a diagnostic, and the overflow it reports is
// original-game behaviour rather than a regression, so it would otherwise
// log during ordinary play. Enable with OPENPETE_POLYBUF_GUARD=1, or
// =verbose to also log the per-frame high-water while the buffer is healthy
// (the graded signal: how close a level runs to the edge without tipping).

#include "openpete_knobs.h"   /* knob_env_epoch_stale */
#include "feature_registry.h"

#include "openpete_fiber.h"  /* openpete_frame (VBL counter) */
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read callbacks */
#include "game_layout.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ADDR_BUILD_ORDER_TABLE  0x80016784u

#define ADDR_G_WORLDOT          0x80075820u  /* g_WorldOT   */

/* One primitive is 0x34 bytes in the paths that overrun (POLY_GT4). Only used
 * to render byte counts in a unit that maps to what the scene is drawing. */
#define PRIM_BYTES 0x34

enum { GUARD_OFF = 0, GUARD_ON = 1, GUARD_VERBOSE = 2 };

static int      s_overflowing;    /* edge state */
static uint32_t s_worst_over;     /* peak bytes past the end this episode */
static uint32_t s_first_frame;    /* frame the episode started */
static uint32_t s_report_at;      /* next frame allowed to re-log while over */
static uint32_t s_episodes;       /* overflow episodes since boot */
static int32_t  s_min_headroom = INT32_MAX;  /* closest approach while healthy */

/* Rate limit for the "still overflowing" line, in frames. The interesting
 * event is the transition; the repeats only exist so a sustained overflow
 * doesn't look like a single blip in the log. */
#define REPEAT_EVERY 120u

/** @brief Reads the OPENPETE_POLYBUF_GUARD mode from the environment.
 *
 * Re-reads the environment when the registry env epoch changes, so the
 * dev-overlay row (knob_registry.c, ENVROW_LIVE) toggles the guard at
 * runtime with one getenv per edit.
 * @return GUARD_OFF when unset, empty, or starting with 0, n or N;
 *     GUARD_VERBOSE when starting with v or V; else GUARD_ON. */
static int guard_mode(void) {
    static int mode = GUARD_OFF;
    static unsigned seen = 0;
    if (!knob_env_epoch_stale(&seen)) return mode;
    const char* e = getenv("OPENPETE_POLYBUF_GUARD");
    const char c = e ? e[0] : '\0';
    mode = (c == 'v' || c == 'V') ? GUARD_VERBOSE
         : (c == '\0' || c == '0' || c == 'n' || c == 'N') ? GUARD_OFF : GUARD_ON;
    return mode;
}

/** @brief Samples the polygon buffer cursor at BuildOrderTable and logs overflow past its end, edge-triggered.
 *
 * Feature dispatch hook on 0x80016784; reads guest RAM only and always
 * returns 0 so the original runs. Does nothing while OPENPETE_POLYBUF_GUARD
 * is off, or while the cursor or end word is 0 (buffers not set up).
 * Tracks headroom as end - cursor: on the first overflow it logs the
 * measured relationship to g_WorldOT (an error when the cursor has reached
 * the OT, else a warning) with level and camera position, re-logs every 120
 * openpete_frame counts while overflowed, and logs a summary on recovery;
 * in verbose mode it also logs the headroom of every healthy sample.
 * @param cpu The guest CPU state.
 * @param addr Unused.
 * @return 0. */
static int on_build_order_table(CPUState* cpu, uint32_t addr) {
    (void)addr;
    const int mode = guard_mode();
    if (mode == GUARD_OFF) return 0;

    const uint32_t cursor = cpu->read_word(game_layout.polybuf.cursor);
    const uint32_t end    = cpu->read_word(game_layout.polybuf.end);

    /* Before the first level's buffers exist both reads are 0/garbage; a zero
     * end means "not set up yet", not "overflowed by 2GB". */
    if (end == 0u || cursor == 0u) return 0;

    const uint32_t frame  = (uint32_t)openpete_frame;
    const int32_t  slack  = (int32_t)(end - cursor);   /* >0 healthy, <0 over */

    if (slack >= 0) {
        if (s_overflowing) {
            OPL_WARN_F("polybuf",
                       "RECOVERED @f%u - episode lasted %u frames, peak %u bytes "
                       "(%u prims) past the end",
                       frame, frame - s_first_frame,
                       s_worst_over, s_worst_over / PRIM_BYTES);
            s_overflowing = 0;
            s_worst_over  = 0;
        }
        if (slack < s_min_headroom) s_min_headroom = slack;
        if (mode == GUARD_VERBOSE) {
            OPL_INFO_F("polybuf",
                       "f%u ok - %d bytes free (%d prims), closest ever %d",
                       frame, slack, slack / PRIM_BYTES, s_min_headroom);
        }
        return 0;
    }

    const uint32_t over = (uint32_t)(-slack);
    if (over > s_worst_over) s_worst_over = over;

    if (!s_overflowing) {
        s_overflowing = 1;
        s_first_frame = frame;
        s_report_at   = frame + REPEAT_EVERY;
        s_episodes++;

        const uint32_t ot  = cpu->read_word(ADDR_G_WORLDOT);
        const uint32_t lvl = cpu->read_word(game_layout.level_id);
        const int32_t  cx  = (int32_t)cpu->read_word(game_layout.cam.position);
        const int32_t  cy  = (int32_t)cpu->read_word(game_layout.cam.position + 4u);
        const int32_t  cz  = (int32_t)cpu->read_word(game_layout.cam.position + 8u);

        /* Whether the overrun is actively corrupting depends on how far the
         * cursor got, not merely that it passed the end: the OT is not always
         * adjacent. Its distance from the buffer end changes as the level's
         * allocations settle, so report the measured relationship rather than
         * assuming the worst. */
        if (cursor >= ot) {
            OPL_ERROR_F("polybuf",
                        "OVERFLOW #%u @f%u level=%u - cursor %08X is %u bytes "
                        "(%u prims) PAST end %08X, and %u bytes INTO g_WorldOT "
                        "%08X: primitives are overwriting OT buckets, so the "
                        "OT tail-insert will dereference primitive bytes as a "
                        "node pointer. camera=(%d, %d, %d)",
                        s_episodes, frame, lvl, cursor, over, over / PRIM_BYTES,
                        end, cursor - ot, ot, cx, cy, cz);
        } else {
            OPL_WARN_F("polybuf",
                       "OVERFLOW #%u @f%u level=%u - cursor %08X is %u bytes "
                       "(%u prims) PAST end %08X, but still %u bytes short of "
                       "g_WorldOT %08X: scribbling whatever follows the buffer, "
                       "not yet OT buckets. camera=(%d, %d, %d)",
                       s_episodes, frame, lvl, cursor, over, over / PRIM_BYTES,
                       end, ot - cursor, ot, cx, cy, cz);
        }
        return 0;
    }

    if (frame >= s_report_at) {
        s_report_at = frame + REPEAT_EVERY;
        OPL_WARN_F("polybuf",
                   "still overflowing @f%u - %u bytes (%u prims) past the end, "
                   "peak %u this episode",
                   frame, over, over / PRIM_BYTES, s_worst_over);
    }
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_BUILD_ORDER_TABLE, on_build_order_table, 0u },
    { 0u, NULL, 0u },
};

const feature_module_t polybuf_guard_module = {
    "polybuf_guard",
    NULL,            /* no options to apply; environment-gated */
    k_dispatch,
    NULL,            /* no threads to join */
};
