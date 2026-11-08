// games/common/features/polybuf_guard.c - polygon-buffer overflow detector.
//
// Every game bump-allocates display primitives out of a per-buffer polygon
// buffer: a cursor word and an end word (game_layout.polybuf). No emitter of
// the 3D passes tests the cursor against the end, so a view that emits more
// primitives than the buffer holds walks the cursor past it.
//
// Spyro 1: the world ordering table (g_WorldOT) is allocated directly after
// the buffer, so the overrun lands primitives on OT buckets:
//
//   polybuf cursor 0x801C397C   <- 15,820 bytes past the end
//   polybuf end    0x801BFBB0
//   g_WorldOT      0x801BFBB8   <- OT starts 8 bytes after the buffer
//
// The OT tail-insert in the world renderer (r_environment.s 0x800270C8) reads
// a bucket for "the previous primitive in this bucket" and writes a 24-bit
// link through it without checking it, so primitive bytes get dereferenced as
// a pointer and three bytes are written to an effectively arbitrary address.
// The buffer is whatever RAM is left after the level data loads, so its size
// varies per level.
//
// Spyro 2 and 3: the two buffers sit back to back below a 2D OT entry and
// the world OT (Spyro 2 init/func_80011DD4.s), and the moby cull, the world
// draw and the player draw carve their per-tick draw scratch (the moby record
// queues, the terrain draw lists) at end - 0x3000 inside the buffer. The
// cursor crossing that carve overwrites scratch the same present's passes
// still read, so the carve, not the end, is the danger line.
//
// This module is read-only and always falls through. It samples on entry to
// game_layout.polybuf.sample_fn, a call every emitter of the present has
// returned before (Spyro 1 BuildOrderTable; Spyro 2 and 3 the ordering-table
// stitch that runs before DrawOTag), so the cursor is at its peak for the
// present. The sample function must be a recompiled guest function: a PsyQ
// call such as DrawSync runs as a native thunk and never reaches the dispatch.
// Reporting is edge-triggered against the danger line: one line when the
// cursor first crosses it, a rate-limited line while it stays across, and a
// summary when it recovers; a line per new closest approach, and a summary of
// the run at shutdown.
//
// OFF by default: the overflow it reports is original-game behaviour rather
// than a regression. Enable with OPENPETE_POLYBUF_GUARD=1, or =verbose to
// also log every sample.

#include "openpete_knobs.h"   /* knob_env_epoch_stale */
#include "feature_registry.h"

#include "openpete_fiber.h"  /* openpete_frame (VBL counter) */
#include "openpete_log.h"
#include "psx_runtime_internal.h"   /* CPUState + its read callbacks */
#include "game_layout.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/* One primitive is 0x34 bytes in the paths that overrun (POLY_GT4). Only used
 * to render byte counts in a unit that maps to what the scene is drawing. */
#define PRIM_BYTES 0x34

enum { GUARD_OFF = 0, GUARD_ON = 1, GUARD_VERBOSE = 2 };

static int      s_overflowing;    /* edge state */
static uint32_t s_worst_over;     /* peak bytes past the danger line this episode */
static uint32_t s_first_frame;    /* VBL the episode started */
static uint32_t s_report_at;      /* next VBL allowed to re-log while over */
static uint32_t s_episodes;       /* overflow episodes since boot */
static uint32_t s_samples;        /* samples taken (engagement) */
static uint32_t s_stale;          /* samples skipped as a stale cursor/end pair */
static int32_t  s_min_line = INT32_MAX;   /* closest approach to the danger line */
static int32_t  s_min_end  = INT32_MAX;   /* closest approach to the end */
static uint32_t s_min_frame, s_min_level, s_min_cursor, s_min_bufend;
static int32_t  s_min_cam[3];

/* Rate limit for the "still overflowing" line, in VBLs. */
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

/** @brief Reads the camera position words into out.
 * @param cpu The guest CPU state.
 * @param out Receives x, y, z. */
static void read_cam(CPUState* cpu, int32_t out[3]) {
    for (uint32_t i = 0; i < 3u; i++)
        out[i] = (int32_t)cpu->read_word(game_layout.cam.position + 4u * i);
}

/** @brief Reports whether the end word belongs to the draw buffer that holds the cursor.
 *
 * The two buffers sit back to back at equal size, their bases in the draw
 * buffer records at db_base_off. The buffer holding the cursor is the higher
 * one when the cursor is at or above its base; its end is its base plus the
 * distance between the bases. A writer that moves the cursor to the other
 * buffer without the end word (the loading-screen emitters) leaves a pair
 * that measures neither buffer. With db_base_off 0 every pair counts.
 * @param cpu The guest CPU state.
 * @param cursor The sampled cursor.
 * @param end The sampled buffer end.
 * @return 1 when the pair is consistent, else 0. */
static int pair_consistent(CPUState* cpu, uint32_t cursor, uint32_t end) {
    const uint32_t off = game_layout.polybuf.db_base_off;
    if (!off) return 1;
    const uint32_t b0 = cpu->read_word(game_layout.db.base + off);
    const uint32_t b1 = cpu->read_word(game_layout.db.base + game_layout.db.stride + off);
    const uint32_t lo = b0 < b1 ? b0 : b1, hi = b0 < b1 ? b1 : b0;
    if (lo == hi) return 1;
    const uint32_t own = cursor >= hi ? hi : lo;
    return end - own == hi - lo;
}

/** @brief Logs a buffer's placement the first time its end word is sampled after a change: end, carve, OT and the span of the level's sector records.
 *
 * Two ends are remembered, one per buffer of the double-buffered pair, so a
 * level logs two lines and a level load that moves the buffers logs again.
 * @param cpu The guest CPU state.
 * @param end The sampled buffer end. */
static void note_layout(CPUState* cpu, uint32_t end) {
    static uint32_t seen[2];
    if (end == seen[0] || end == seen[1]) return;
    seen[1] = seen[0];
    seen[0] = end;
    const uint32_t ot  = game_layout.polybuf.ot_ptr
                       ? cpu->read_word(game_layout.polybuf.ot_ptr) : 0u;
    uint32_t lo = 0u, hi = 0u, n = 0u, tab = 0u;
    if (game_layout.env.sector_ptr && game_layout.env.sector_count) {
        tab = cpu->read_word(game_layout.env.sector_ptr);
        n   = cpu->read_word(game_layout.env.sector_count);
        if (n > 0x1000u) n = 0u;   /* header not populated yet */
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t s = cpu->read_word(tab + 4u * i);
            if (!lo || s < lo) lo = s;
            if (s > hi) hi = s;
        }
    }
    OPL_INFO_F("polybuf",
               "layout level=%u end %08X carve %08X OT %08X; sector table %08X, "
               "%u sector records in [%08X, %08X]",
               cpu->read_word(game_layout.level_id), end,
               end - game_layout.polybuf.scratch, ot, tab, n, lo, hi);
}

/** @brief Records a sample as the closest approach when it beats the previous one, logging the new low.
 * @param cpu The guest CPU state.
 * @param cursor The sampled cursor.
 * @param end The sampled buffer end.
 * @param to_line Bytes from the cursor to the danger line.
 * @param frame The sample's VBL count. */
static void note_low(CPUState* cpu, uint32_t cursor, uint32_t end,
                     int32_t to_line, uint32_t frame) {
    const int32_t to_end = (int32_t)(end - cursor);
    if (to_end < s_min_end) s_min_end = to_end;
    if (to_line >= s_min_line) return;
    s_min_line   = to_line;
    s_min_frame  = frame;
    s_min_level  = cpu->read_word(game_layout.level_id);
    s_min_cursor = cursor;
    s_min_bufend = end;
    read_cam(cpu, s_min_cam);
    OPL_INFO_F("polybuf",
               "low @f%u level=%u cursor %08X end %08X carve %08X: %d bytes to "
               "the %s, %d to the end. camera=(%d, %d, %d)",
               frame, s_min_level, cursor, end, end - game_layout.polybuf.scratch,
               to_line, game_layout.polybuf.scratch ? "carve" : "end", to_end,
               s_min_cam[0], s_min_cam[1], s_min_cam[2]);
}

/** @brief Logs the first sample across the danger line with the measured distance to the ordering table.
 * @param cpu The guest CPU state.
 * @param cursor The sampled cursor.
 * @param end The sampled buffer end.
 * @param over Bytes past the danger line.
 * @param frame The sample's VBL count. */
static void log_episode_start(CPUState* cpu, uint32_t cursor, uint32_t end,
                              uint32_t over, uint32_t frame) {
    const uint32_t ot  = game_layout.polybuf.ot_ptr
                       ? cpu->read_word(game_layout.polybuf.ot_ptr) : 0u;
    const uint32_t lvl = cpu->read_word(game_layout.level_id);
    int32_t cam[3];
    read_cam(cpu, cam);
    const char* line = game_layout.polybuf.scratch ? "draw-scratch carve" : "end";
    if (ot && cursor >= ot) {
        OPL_ERROR_F("polybuf",
                    "OVERFLOW #%u @f%u level=%u - cursor %08X is %u bytes (%u prims) "
                    "past the %s %08X (end %08X), and %u bytes INTO the OT %08X: "
                    "the OT insert will dereference primitive bytes as a node "
                    "pointer. camera=(%d, %d, %d)",
                    s_episodes, frame, lvl, cursor, over, over / PRIM_BYTES, line,
                    end - game_layout.polybuf.scratch, end, cursor - ot, ot,
                    cam[0], cam[1], cam[2]);
    } else {
        OPL_WARN_F("polybuf",
                   "OVERFLOW #%u @f%u level=%u - cursor %08X is %u bytes (%u prims) "
                   "past the %s %08X (end %08X, OT %08X). camera=(%d, %d, %d)",
                   s_episodes, frame, lvl, cursor, over, over / PRIM_BYTES, line,
                   end - game_layout.polybuf.scratch, end, ot,
                   cam[0], cam[1], cam[2]);
    }
}

/** @brief Samples the polygon buffer cursor at the present's last emitter and logs crossings of the danger line, edge-triggered.
 *
 * Feature dispatch hook on game_layout.polybuf.sample_fn; reads guest RAM
 * only and always returns 0 so the original runs. Does nothing while
 * OPENPETE_POLYBUF_GUARD is off, while the caller is not
 * game_layout.polybuf.sample_ra (when nonzero), or while the cursor or end
 * word is 0 (buffers not set up). The danger line is end - scratch.
 * @param cpu The guest CPU state.
 * @param addr Unused.
 * @return 0. */
static int on_polybuf_sample(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    const int mode = guard_mode();
    if (mode == GUARD_OFF) return 0;
    if (game_layout.polybuf.sample_ra && cpu->ra != game_layout.polybuf.sample_ra) return 0;

    const uint32_t cursor = cpu->read_word(game_layout.polybuf.cursor);
    const uint32_t end    = cpu->read_word(game_layout.polybuf.end);
    if (end == 0u || cursor == 0u) return 0;

    if (!pair_consistent(cpu, cursor, end)) {
        if (!s_stale++)
            OPL_INFO_F("polybuf", "stale pair @f%u level=%u: cursor %08X is not in the buffer "
                       "ending at %08X; such samples are skipped",
                       (uint32_t)openpete_frame, cpu->read_word(game_layout.level_id),
                       cursor, end);
        return 0;
    }
    s_samples++;
    const uint32_t frame = (uint32_t)openpete_frame;
    const uint32_t line  = end - game_layout.polybuf.scratch;
    const int32_t  slack = (int32_t)(line - cursor);   /* >0 healthy, <0 over */
    note_layout(cpu, end);
    note_low(cpu, cursor, end, slack, frame);
    if (mode == GUARD_VERBOSE) {
        OPL_INFO_F("polybuf", "f%u cursor %08X end %08X: %d to the line, %d to the end",
                   frame, cursor, end, slack, (int32_t)(end - cursor));
    }

    if (slack >= 0) {
        if (s_overflowing) {
            OPL_WARN_F("polybuf",
                       "RECOVERED @f%u - episode lasted %u VBLs, peak %u bytes "
                       "(%u prims) past the line",
                       frame, frame - s_first_frame,
                       s_worst_over, s_worst_over / PRIM_BYTES);
            s_overflowing = 0;
            s_worst_over  = 0;
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
        log_episode_start(cpu, cursor, end, over, frame);
        return 0;
    }
    if (frame >= s_report_at) {
        s_report_at = frame + REPEAT_EVERY;
        OPL_WARN_F("polybuf",
                   "still overflowing @f%u - %u bytes (%u prims) past the line, "
                   "peak %u this episode",
                   frame, over, over / PRIM_BYTES, s_worst_over);
    }
    return 0;
}

/* The sample address comes from the game's layout, so the table is filled at
 * boot. A game that names none registers nothing. */
static feature_dispatch_entry_t s_dispatch[2];

/** @brief Fills the dispatch table with the game's sample address.
 * @param opts Unused. */
static void polybuf_guard_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    const feature_dispatch_entry_t hook = { .addr = game_layout.polybuf.sample_fn,
                                            .fn = on_polybuf_sample };
    int n = 0;
    if (hook.addr) s_dispatch[n++] = hook;
    s_dispatch[n].addr = 0u;
    s_dispatch[n].fn   = NULL;
}

/** @brief Logs the run's sample count, overflow episodes and closest approach when the guard took any sample. */
static void polybuf_guard_shutdown(void) {
    OP_ROLE_ASSERT(sim);
    if (!s_samples) return;
    OPL_INFO_F("polybuf",
               "summary: %u samples (%u stale pairs skipped), %u overflow episodes; closest %d bytes to the "
               "%s and %d to the end, @f%u level=%u cursor %08X end %08X "
               "camera=(%d, %d, %d)",
               s_samples, s_stale, s_episodes, s_min_line,
               game_layout.polybuf.scratch ? "carve" : "end", s_min_end,
               s_min_frame, s_min_level, s_min_cursor, s_min_bufend,
               s_min_cam[0], s_min_cam[1], s_min_cam[2]);
}

const feature_module_t polybuf_guard_module = {
    .name           = "polybuf_guard",
    .apply_options  = polybuf_guard_apply_options,
    .dispatch_table = s_dispatch,
    .shutdown       = polybuf_guard_shutdown,
};
