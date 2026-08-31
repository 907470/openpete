// core/runtime/openpete_menu.c — in-game settings menu state + logic.
// See openpete_menu.h and notes/plans/openpete-ingame-settings-menu.md.
//
// PURE PRESENTATION: every row maps to a native-renderer / host-pacer setter
// (openpete_native_set_aspect/ssaa/vsync, openpete_set_render_fps,
// openpete_no_lod_set). It never writes gameplay-readable state. The one
// deliberate exception is FREECAM: the debug free camera (the F key) is an
// opt-in gameplay-state divergence by design (openpete_freecam.h); the row is
// just a second front-end for the same toggle. The draw half
// (panel/border/glyphs) lands in a later step; this file is state + apply.

#include "openpete_menu.h"
#include "openpete.h"                /* openpete_options_t, openpete_set_render_fps */
#include "openpete_native_render.h"  /* set_aspect / set_ssaa / set_vsync */
#include "openpete_log.h"
#include "openpete_config.h"        /* saved-settings persistence (SETTINGS row) */
#include "openpete_knobs.h"         /* knob_registry_save_edited */
#include "openpete_freecam.h"        /* freecam active/toggle (FREECAM row) */
#include "openpete_mods.h"           /* loaded-mod enumeration (MOD rows) */
#include "features/no_lod.h"         /* openpete_no_lod_set */

#include <stddef.h>
#include <stdio.h>

/* --- rows ---------------------------------------------------------------- */
enum { ROW_DISPLAY, ROW_ASPECT, ROW_FPS, ROW_SHOWFPS, ROW_VSYNC, ROW_SSAA,
       ROW_WOBBLE, ROW_TEXMAP, ROW_DITHER, ROW_LOD, ROW_FREECAM, ROW_SAVE,
       ROW_QUIT,
       ROW_COUNT,
       /* MODSEL/MODEN and FOG are parked past ROW_COUNT: hidden from display
        * + navigation for now, but all their plumbing stays live and
        * compiled — the case labels below, the mod enumeration, s_fog_idx,
        * openpete_fog_set/enabled and the LOD→fog autopilot coupling in
        * no_lod.c (LOD switches still move the fog scale). Move a row back
        * before ROW_COUNT (and restore its ROW_LABEL entry) to re-show it. */
       ROW_MODSEL, ROW_MODEN, ROW_FOG };

static const char* const ROW_LABEL[ROW_COUNT] = {
    "DISPLAY", "ASPECT", "FPS", "SHOW FPS", "VSYNC", "SSAA", "WOBBLE",
    "TEXTURES", "DITHER", "LOD", "FREECAM", "SETTINGS", "EXIT GAME",
};

/* EXIT is a CENTERED row: no value column, its label spans the panel centre
 * (openpete_menu_row_centered), and it sits below a gap so it reads as an
 * action separate from the settings above it.
 *
 * It arms on the first press and quits on the second. "Increase value" is the
 * same input the player uses on every other row (Right / d-pad / cross) and the
 * cursor wraps, so a one-press exit sits one stray keypress away from throwing
 * out the progress since their last save. Moving the cursor disarms. */
static int s_quit_armed;

/* SETTINGS is an action row, not a value: left/right pick what pressing it
 * will do, and the press does it. Persisting is explicit — closing the menu
 * must never rewrite the user's config file behind them. */
enum { SAVE_IDLE, SAVE_DO, SAVE_CLEAR };
static int s_save_idx;      /* SAVE_* */
static int s_save_result;   /* 0 none, >0 saved n, -1 failed, -2 cleared */

/* Aspect presets (label + width/height). */
static const struct { const char* label; float wh; } s_aspect[] = {
    { "AUTO",  0.0f          },   /* follow the window/fullscreen shape (default) */
    { "4:3",   4.0f  / 3.0f  },
    { "16:10", 16.0f / 10.0f },
    { "16:9",  16.0f / 9.0f  },
    { "21:9",  21.0f / 9.0f  },
    { "32:9",  32.0f / 9.0f  },
};
#define N_ASPECT ((int)(sizeof(s_aspect) / sizeof(s_aspect[0])))

/* Ladder stops at OPENPETE_SSAA_MAX — 8X/16X ask for more render-target VRAM
 * than an 8GB card has (see openpete.h). 3X is a genuine rung, not a
 * power-of-two gap-filler: the resolve pass box-filters the exact NxN source
 * block, so an odd factor is as well defined as an even one, and 3X costs
 * ~44% less render-target area than 4X.
 *
 * Factor 1 reads OFF, not "1X": it is not a supersampling amount but the
 * absence of one — no supersampled target, no resolve pass. (The WOBBLE
 * ladder below spells its own no-op step the same way, where 1X IS a
 * setting.) */
static const int   s_ssaa_vals[]   = { 1, 2, 3, 4 };
static const char* s_ssaa_labels[] = { "OFF", "2X", "3X", "4X" };
#define N_SSAA ((int)(sizeof(s_ssaa_vals) / sizeof(s_ssaa_vals[0])))

/* WOBBLE ladder: one row drives the PS1-look PAIR — the integer vertex snap
 * plus the raster quantize whose grid it snaps to. Every ON step is
 * snap + quantize at the given multiplier of the 512x240 PSX raster: the
 * snap's residual cracks are bounded/absorbed by the quantize cells, so the
 * menu never offers the sharp-wobble combo whose hairlines pulled this row
 * the first time. (The independent CLI knobs --vertex-precision /
 * --raster-quantize keep the full space, e.g. quantize-only or 16x.) */
static const int   s_wob_vals[]   = { 0, 1, 2, 4, 8 };
static const char* s_wob_labels[] = { "OFF", "1X", "2X", "4X", "8X" };
#define N_WOB ((int)(sizeof(s_wob_vals) / sizeof(s_wob_vals[0])))

/* FPS ladder, built from the monitor refresh at init. The final entry is an
 * UNLIMITED sentinel: a render-fps so high the wall-clock pacer never sleeps on
 * the render deadline, so presenting is GPU-bound (uncapped). It's a normal
 * render-fps value to the pacer — the sim still ticks at its fixed ~29.913 Hz;
 * only presentation is uncapped (subject to vsync). Displayed as "UNLIMITED". */
#define FPS_UNLIMITED 1000000
static int s_fps_vals[16];
static int s_fps_count = 0;

/* --- live selection ------------------------------------------------------ */
static int s_open       = 0;
static int s_cursor     = 0;
static int s_aspect_idx = 0;   /* AUTO     */
static int s_fps_idx    = 0;   /* first ladder entry (30) */
static int s_showfps_idx = 1;  /* ON — mirrors the live vk FPS overlay */
static int s_vsync_idx  = 1;   /* ON       */
static int s_ssaa_idx   = 0;   /* 1X       */
static int s_wobble_idx = 0;   /* OFF — re-synced from the live pair on open */
static int s_texmap_idx = 0;   /* CORRECT (perspective) — re-synced from the live mode on open */
static int s_dither_idx = 0;   /* OFF — re-synced from the live flag on open */
static int s_lod_idx    = 0;   /* ORIGINAL */
static int s_fog_idx    = 1;   /* ON — re-seeded from the resolved scale at init */
static int s_freecam_idx = 0;  /* OFF — re-synced from the live state on open */
static unsigned s_tick  = 0;   /* wobble/shimmer phase (draw step) */

/* --- helpers ------------------------------------------------------------- */
static int wrap_idx(int i, int n) {
    if (n <= 0) return 0;
    i %= n;
    if (i < 0) i += n;
    return i;
}

/* Index of the value in `vals` nearest to `want` (for seeding from opts). */
static int nearest_idx(const int* vals, int n, int want) {
    int best = 0, bestd = -1;
    for (int i = 0; i < n; i++) {
        int d = vals[i] - want; if (d < 0) d = -d;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* Map the live snap+quantize pair onto the WOBBLE ladder: OFF unless the
 * vertex snap is integer, else the ON step nearest the live multiplier. A
 * custom CLI/M-overlay pair (quantize-only, sharp wobble, 16x) shows its
 * nearest rung and is only rewritten when the user cycles the row. */
static int wobble_idx_from_live(void) {
    if (!openpete_native_vertex_snap_active()) return 0;
    return 1 + nearest_idx(s_wob_vals + 1, N_WOB - 1,
                           openpete_native_get_raster_quantize());
}

static void build_fps_list(int refresh) {
    const int cap = (int)(sizeof(s_fps_vals) / sizeof(s_fps_vals[0]));
    if (refresh < 60) refresh = 60;   /* sane floor / unknown */
    int n = 0;
    s_fps_vals[n++] = 30;
    s_fps_vals[n++] = 60;
    for (int m = 120; m < refresh && n < cap; m += 60)
        s_fps_vals[n++] = m;
    if (refresh > 60 && n < cap && s_fps_vals[n - 1] != refresh)
        s_fps_vals[n++] = refresh;
    if (n < cap)
        s_fps_vals[n++] = FPS_UNLIMITED;   /* uncapped presenting, after refresh */
    s_fps_count = n;
}

/* Mod menu rows (modding P4): MOD cycles the loaded-mod selection;
 * MOD ENABLED shows + live-toggles the selected mod (applied at the
 * next tick boundary by the loader). Data comes straight from the
 * loader enumeration — zero mods shows NONE and the rows no-op. */
static int s_modsel_idx = 0;

/* Current value string for a row (for the log + later the draw). */
static const char* value_str(int row, char* buf, int buflen) {
    switch (row) {
    /* DISPLAY reads the LIVE window state each frame (no shadow index) — like
     * MOD ENABLED — so Alt+Enter / a double-click can never leave it stale. */
    case ROW_DISPLAY:
        return openpete_native_fullscreen_active() ? "FULLSCREEN" : "WINDOWED";
    case ROW_ASPECT: return s_aspect[s_aspect_idx].label;
    case ROW_FPS: {
        int v = (s_fps_idx < s_fps_count) ? s_fps_vals[s_fps_idx] : 0;
        if (v >= FPS_UNLIMITED) return "UNLIMITED";
        /* tiny itoa (no snprintf dependency needed, but it's available) */
        snprintf(buf, (size_t)buflen, "%d", v);
        return buf;
    }
    case ROW_SHOWFPS:  return s_showfps_idx ? "ON" : "OFF";
    case ROW_VSYNC:    return s_vsync_idx ? "ON" : "OFF";
    case ROW_SSAA:     return s_ssaa_labels[s_ssaa_idx];
    case ROW_WOBBLE:   return s_wob_labels[s_wobble_idx];
    case ROW_TEXMAP:   return s_texmap_idx ? "AFFINE" : "CORRECT";
    case ROW_DITHER:   return s_dither_idx ? "15-BIT" : "OFF";
    case ROW_LOD: return s_lod_idx ? "EXTENDED" : "ORIGINAL";
    case ROW_FOG:      return s_fog_idx ? "ON" : "OFF";
    case ROW_FREECAM:  return s_freecam_idx ? "ON" : "OFF";
    case ROW_SAVE:
        if (s_save_result == -1) return "FAILED";
        if (s_save_result == -2) return "CLEARED";
        if (s_save_result > 0) {
            snprintf(buf, (size_t)buflen, "SAVED %d", s_save_result);
            return buf;
        }
        return s_save_idx == SAVE_CLEAR ? "CLEAR SAVED" : "SAVE";
    case ROW_QUIT:     return "";   /* centered: the label IS the row */
    case ROW_MODSEL: {
        const char* id = openpete_mods_id(s_modsel_idx);
        return id ? id : "NONE";
    }
    case ROW_MODEN: {
        const char* id = openpete_mods_id(s_modsel_idx);
        if (!id) return "-";
        return openpete_mods_is_enabled(id) ? "ON" : "OFF";
    }
    default:           return "?";
    }
}

static void log_state(const char* what) {
    char vb[16];
    OPL_INFO_F("menu", "%s: [%s] = %s", what,
               ROW_LABEL[s_cursor], value_str(s_cursor, vb, sizeof vb));
}

/* Apply the focused row's current value via its setter. */
static void apply_row(int row) {
    switch (row) {
    case ROW_ASPECT:   openpete_native_set_aspect(s_aspect[s_aspect_idx].wh); break;
    case ROW_FPS:      openpete_set_render_fps(s_fps_vals[s_fps_idx]);         break;
    case ROW_SHOWFPS:  openpete_native_set_fps_overlay(s_showfps_idx);         break;
    case ROW_VSYNC:    openpete_native_set_vsync(s_vsync_idx);                 break;
    case ROW_SSAA:     openpete_native_set_ssaa(s_ssaa_vals[s_ssaa_idx]);      break;
    case ROW_WOBBLE: {
        int m = s_wob_vals[s_wobble_idx];
        openpete_native_set_vertex_precision(m > 0);
        openpete_native_set_raster_quantize(m);
        break;
    }
    /* Queued: the renderer re-bakes its pipelines at the next present
     * (a one-time device-idle hitch), so the value string reads the local
     * index, not the not-yet-applied live getter. */
    case ROW_TEXMAP:   openpete_native_set_texture_mapping(s_texmap_idx);      break;
    case ROW_DITHER:   openpete_native_set_dither(s_dither_idx);               break;
    case ROW_LOD:
        openpete_no_lod_set(s_lod_idx, -1.0f);
        /* Fog follows the LOD mode unless the user owns it (explicit
         * --fog-scale / a manual FOG toggle — no_lod.c decides); mirror
         * whatever the setter resolved so the FOG row never shows stale. */
        s_fog_idx = openpete_fog_enabled();
        break;
    case ROW_FOG:      openpete_fog_set(s_fog_idx);                           break;
    case ROW_SAVE:
        /* Fires on apply (the same keypress every other row uses), so the
         * file is only ever written when the user asked for it. */
        if (s_save_idx == SAVE_CLEAR) {
            s_save_result = (openpete_config_reset_saved() == 0) ? -2 : -1;
        } else if (s_save_idx == SAVE_DO) {
            int n = knob_registry_save_edited();
            s_save_result = (n < 0) ? -1 : (n > 0 ? n : -2);
        }
        s_save_idx = SAVE_IDLE;
        break;
    case ROW_FREECAM:
        /* The freecam API is a toggle (shared with the F key), not a setter:
         * flip only when the row disagrees with the live state. The toggle
         * clears the latched axes, so no stale intent carries over. */
        if ((s_freecam_idx != 0) != (openpete_freecam_active() ? 1 : 0))
            openpete_freecam_toggle();
        break;
    case ROW_QUIT:
        /* Cooperative: the host-fiber loop drains and tears down on its next
         * iteration, so the memory card flushes like any other exit. */
        if (s_quit_armed >= 2) {
            OPL_INFO_F("menu", "%s", "exit requested");
            openpete_request_shutdown();
        }
        break;

    default: break;
    }
}

/* --- public -------------------------------------------------------------- */
void openpete_menu_init(const openpete_options_t* opts, int refresh_hz) {
    build_fps_list(refresh_hz);

    if (!opts) return;

    /* Aspect: nearest FIXED preset to an explicit launch --aspect (the i=1
     * start skips the AUTO entry — its 0.0 sentinel isn't a ratio); unset /
     * `--aspect auto` → AUTO (follow the window, the default). */
    if (opts->aspect_w_over_h > 0.0f) {
        int best = 1; float bestd = -1.0f;
        for (int i = 1; i < N_ASPECT; i++) {
            float d = s_aspect[i].wh - opts->aspect_w_over_h;
            if (d < 0.0f) d = -d;
            if (bestd < 0.0f || d < bestd) { bestd = d; best = i; }
        }
        s_aspect_idx = best;
    } else {
        s_aspect_idx = 0;   /* AUTO */
    }

    /* FPS: exact/nearest in the ladder, seeded from the LIVE pacer target —
     * run.c resolves the --render-fps default (display refresh when unset,
     * windowed) into s_render_fps before this init, so opts->render_fps may
     * still hold the -1 sentinel. 0 = legacy match-sim → the 30 entry. */
    {
        int live_fps = openpete_get_render_fps();
        s_fps_idx = nearest_idx(s_fps_vals, s_fps_count,
                                live_fps > 0 ? live_fps : 30);
    }

    /* VSync: -1 (unset) defaults to ON (FIFO); 0/1 as given. */
    s_vsync_idx = (opts->vsync == 0) ? 0 : 1;

    /* SHOW FPS is the two-state view of the three-level stats overlay: ON
     * whenever anything is drawn. The M overlay's Performance stats row picks
     * WHICH level, and toggling this row off and on restores it. */
    s_showfps_idx = (opts->perf_stats != OPENPETE_PERF_OFF) ? 1 : 0;

    /* SSAA: nearest supported factor. */
    s_ssaa_idx = nearest_idx(s_ssaa_vals, N_SSAA, opts->ssaa > 0 ? opts->ssaa : 1);

    /* WOBBLE: the native renderer has already resolved --vertex-precision and
     * the --raster-quantize auto default — read the live pair. */
    s_wobble_idx = wobble_idx_from_live();

    /* TEXTURE: the renderer resolved --texture-mapping at its init. */
    s_texmap_idx = openpete_native_get_texture_mapping();

    /* DITHER (PS1 15-bit colour). */
    s_dither_idx = opts->dither ? 1 : 0;

    /* LOD (draw distance / detail policy). */
    s_lod_idx = (opts->lod == OPENPETE_LOD_EXTENDED) ? 1 : 0;

    /* FOG: read the scale no_lod_apply_options already resolved (it runs
     * before this init) — reflects --fog-scale and the LOD-mode default. */
    s_fog_idx = openpete_fog_enabled();

    s_cursor = 0;
}

int openpete_menu_toggle(void) {
    s_open = !s_open;
    if (s_open) {
        /* Re-sync the rows a hotkey can change while the menu is closed, so the
         * menu never shows a stale value: SHOW FPS (--perf-stats / the M
         * overlay's Performance stats row), LOD (the "L" toggle flips openpete_no_lod directly), FOG
         * (the L toggle moves it while fog is on autopilot), and FREECAM
         * (the "F" key / left-stick click). */
        s_showfps_idx = openpete_native_fps_overlay_enabled();
        s_lod_idx     = openpete_no_lod ? 1 : 0;
        s_fog_idx     = openpete_fog_enabled();
        s_freecam_idx = openpete_freecam_active() ? 1 : 0;
        s_wobble_idx  = wobble_idx_from_live();   /* M-overlay knobs move the pair */
        s_texmap_idx  = openpete_native_get_texture_mapping();   /* M-overlay row moves it */
        s_dither_idx  = openpete_native_get_dither();            /* M-overlay row moves it */
        s_quit_armed  = 0;   /* never re-open onto a primed EXIT */
        log_state("opened");
    } else {
        OPL_INFO_F("menu", "%s", "closed");
    }
    return s_open;
}

int openpete_menu_is_open(void) { return s_open; }

int openpete_menu_cursor(void) { return s_cursor; }

void openpete_menu_get_layout(openpete_menu_layout_t* out) {
    if (!out) return;
    /* Full-height, flush-left sidebar. Overlay space: x = window-height
     * fractions (0 = left window edge), y = [0,1] = the FULL window height
     * (window-relative, not the letterboxed game viewport). */
    out->panel_x0 = 0.0f;  out->panel_x1 = 0.62f;
    out->panel_y0 = 0.0f;  out->panel_y1 = 1.0f;
    /* Fill a y for EVERY row the draw loop will visit (row_count), capped only
     * by the row_y array's real capacity. */
    const int cap = (int)(sizeof(out->row_y) / sizeof(out->row_y[0]));
    const int N = (ROW_COUNT < cap) ? ROW_COUNT : cap;
    out->row_count = N;
    /* Styled after the game's pause menu: a compact block (title + rows) with
     * tight line spacing, TOP-ALIGNED in the panel (a small margin from the top).
     * The title is a bold header, noticeably larger than the rows. Row font is
     * capped so the widest row ("SHOW FPS" label / "EXTENDED" value) still fits
     * the narrow sidebar. */
    const float title_cap = 0.080f, row_cap = 0.044f;   /* header > rows */
    /* row_cap is the CEILING: the glyph builder shrinks every row uniformly
     * to fit the widest value ("FULLSCREEN") in the value column (~0.82×).
     * Spacing is scaled by the same ratio from the pause-menu-tuned 0.072 so
     * the shrunk rows keep the original visual density. */
    const float spacing   = 0.050f;   /* row-to-row (tightened from the pause-menu 0.072 with the font shrink) */
    const float top_pad   = 0.055f;   /* panel top → title top */
    const float title_gap = 0.070f;   /* title top → first row (larger header needs more) */
    const float quit_gap  = 0.030f;   /* extra space above EXIT: it acts, it doesn't set */
    out->title_cap_h = title_cap;
    out->title_y     = out->panel_y0 + top_pad + title_cap * 0.5f;
    out->row_cap_h   = row_cap;
    out->row_h       = spacing;        /* (selection bar removed; kept ≈ line pitch) */
    const float first = out->panel_y0 + top_pad + title_cap + title_gap + row_cap * 0.5f;
    for (int i = 0; i < N; i++)
        out->row_y[i] = first + spacing * (float)i
                      + (i >= ROW_QUIT ? quit_gap : 0.0f);
    /* Split point (labels right-align to label_x, values left-align at value_x).
     * Kept left of centre — reduces the left padding and gives the widest row's
     * value ("EXTENDED") room before the right border. */
    out->label_x = 0.300f;   /* label RIGHT-edge column */
    out->value_x = 0.335f;   /* value LEFT column        */
}

const char* openpete_menu_title(void) { return "SETTINGS"; }

const char* openpete_menu_row_label(int row) {
    if (row == ROW_QUIT && s_quit_armed) return "REALLY EXIT?";
    return (row >= 0 && row < ROW_COUNT) ? ROW_LABEL[row] : "";
}

int openpete_menu_row_centered(int row) { return row == ROW_QUIT; }

const char* openpete_menu_row_value(int row, char* buf, int buflen) {
    if (row < 0 || row >= ROW_COUNT) return "";
    return value_str(row, buf, buflen);
}

unsigned openpete_menu_wobble_phase(void) { return s_tick; }

void openpete_menu_nav(int dy) {
    if (!s_open) return;
    s_cursor = wrap_idx(s_cursor + (dy > 0 ? 1 : -1), ROW_COUNT);
    s_quit_armed = 0;   /* leaving the row (or arriving) always re-arms safe */
    log_state("cursor");
}

void openpete_menu_adjust(int dx) {
    if (!s_open) return;
    const int step = (dx > 0 ? 1 : -1);
    switch (s_cursor) {
    case ROW_DISPLAY:
        /* Two-state cycle: either direction flips windowed <-> fullscreen via
         * the same request Alt+Enter / double-click use; the SDL call runs on
         * the window-owning thread, not here. The value string reads the
         * resulting live state back (nothing to apply below). */
        openpete_native_request_fullscreen_toggle();
        break;
    case ROW_ASPECT:   s_aspect_idx = wrap_idx(s_aspect_idx + step, N_ASPECT);   break;
    case ROW_FPS:      s_fps_idx    = wrap_idx(s_fps_idx    + step, s_fps_count); break;
    case ROW_SHOWFPS:  s_showfps_idx = wrap_idx(s_showfps_idx + step, 2);         break;
    case ROW_VSYNC:    s_vsync_idx  = wrap_idx(s_vsync_idx  + step, 2);           break;
    case ROW_SSAA:     s_ssaa_idx   = wrap_idx(s_ssaa_idx   + step, N_SSAA);      break;
    case ROW_WOBBLE:   s_wobble_idx = wrap_idx(s_wobble_idx + step, N_WOB);       break;
    case ROW_TEXMAP:   s_texmap_idx = wrap_idx(s_texmap_idx + step, 2);           break;
    case ROW_DITHER:   s_dither_idx = wrap_idx(s_dither_idx + step, 2);           break;
    case ROW_LOD:      s_lod_idx    = wrap_idx(s_lod_idx    + step, 2);           break;
    case ROW_FOG:      s_fog_idx    = wrap_idx(s_fog_idx    + step, 2);           break;
    case ROW_FREECAM:  s_freecam_idx = wrap_idx(s_freecam_idx + step, 2);         break;
    case ROW_SAVE:
        /* Cycling only chooses which action the next apply performs; it
         * writes nothing. Any move also clears a stale result string. */
        s_save_idx    = wrap_idx(s_save_idx + step, 3);
        s_save_result = 0;
        break;
    case ROW_MODSEL:
        if (openpete_mods_count() > 0)
            s_modsel_idx = wrap_idx(s_modsel_idx + step,
                                    openpete_mods_count());
        break;
    case ROW_MODEN: {
        /* Queue the flip of the SELECTED mod; the loader applies it at
         * the next tick boundary and records the state the value_str
         * reads back — no local shadow index to go stale. */
        const char* id = openpete_mods_id(s_modsel_idx);
        if (id)
            (void)openpete_mods_set_mod_enabled(
                id, !openpete_mods_is_enabled(id));
        break;
    }
    /* Action rows have no value to cycle — left/right are inert on them.
     * openpete_menu_activate() is the only way in. */
    case ROW_QUIT: return;
    default: break;
    }
    apply_row(s_cursor);
    log_state("set");
}

void openpete_menu_activate(void) {
    if (!s_open) return;
    if (!openpete_menu_row_centered(s_cursor)) return;   /* value rows: no-op */
    if (s_quit_armed < 2) s_quit_armed++;   /* first press arms, second fires */
    apply_row(s_cursor);
    log_state("activate");
}

void openpete_menu_tick(void) { s_tick++; }

void openpete_menu_draw(void) {
    /* Panel + gold border + 3D glyph rows land in the draw step. */
}
