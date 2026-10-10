/*
 * Knob registry - the table behind the M-key dev config overlay.
 *
 * One row per config knob. LIVE rows write through the same public setters
 * the O-menu uses (renderer/host state only - never gameplay-readable RAM);
 * RESTART_ONLY rows are display-only until each is individually audited and
 * given a proper live channel (a single boot-latched consumer is enough to
 * keep a row read-only: cf. the fog-scale latch history in no_lod.h).
 *
 * Adding a permanent CLI flag or OPENPETE_* env var? Add its row here in the
 * same change - the overlay doubles as the living catalog of every knob.
 */

#include "openpete_knobs.h"
#include "openpete_osd.h"
#include "openpete_workpool.h"
#include "openpete_terrain_cache.h"
#include "openpete_disc_cache.h"
#include "openpete.h"
#include "openpete_config.h"
#include "config_schema.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_camlock.h"
#include "openpete_fillpick.h"
#include "openpete_freecam.h"
#include "openpete_host_mouse.h"
#include "openpete_record.h"
#include "openpete_rewind.h"
#include "no_lod.h"

#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_options_t *g_opts;

/* ------------------------------------------------------------------ */
/* value plumbing helpers                                             */
/* ------------------------------------------------------------------ */

/** @brief Numeric getter that returns the row's stored value. */
static double get_stored(const knob_desc_t *k) { return k->stored_num; }

/** @brief Index of the entry of `vals[0..n)` nearest to `v`.
 *
 * The first entry wins a tie. Returns 0 when `n` is 0 or less, or when `v` is
 * NaN. */
static int nearest_enum_idx(const float *vals, int n, float v) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(vals[i] - v);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Rows the O menu moves through the renderer's own setters must read the
 * renderer back, not a registry-side shadow: the menu bypasses set_num, so a
 * shadowed row would still report its boot value and "save edited settings"
 * would skip the pick entirely. Until the native renderer is up (its state
 * settles after the registry stamps boot values, and never at all under
 * --headless) the stored boot value is the live value. Keyed on the DISPLAY
 * predicate: these rows are display state, and in psycross mode the native
 * window and its accessors are real even though no native scene is walked. */
#define KNOB_LIVE_OR_STORED(k) \
    do { if (!openpete_native_render_enabled()) return (k)->stored_num; } while (0)

/* ---- Display ---- */

/** @brief 1 when the native window is fullscreen, else 0. */
static double fullscreen_get(const knob_desc_t *k) {
    (void)k; return openpete_native_fullscreen_active() ? 1.0 : 0.0;
}
/** @brief Queue a fullscreen toggle when `v` differs from the window's current state; the window's thread applies it, so the state reads unchanged until then. */
static int fullscreen_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_native_fullscreen_active() != 0))
        openpete_native_request_fullscreen_toggle();   /* SDL call runs on the window's thread */
    return 0;
}

static const char *const k_aspect_names[] = { "auto", "4:3", "16:10", "16:9", "21:9", "32:9" };
static const float       k_aspect_vals[]  = { 0.0f, 4.f/3.f, 16.f/10.f, 16.f/9.f, 21.f/9.f, 32.f/9.f };
/** @brief Index in k_aspect_names of the renderer's live aspect ratio.
 *
 * Returns the stored value until the native renderer is up. Index 0 is
 * auto, whose 0.0 is a sentinel rather than a ratio: a renderer aspect of
 * 0 or less reads as 0, and a positive one as the nearest of the five fixed
 * ratios plus 1 (the same offset knob_registry_init uses). */
static double aspect_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    float wh = openpete_native_get_aspect();
    if (wh <= 0.0f) return 0;
    return nearest_enum_idx(k_aspect_vals + 1, 5, wh) + 1;
}
/** @brief Set the renderer aspect ratio to entry `v` of k_aspect_vals (entry 0 is auto).
 *
 * Stores the index in the row. Returns -1, changing nothing, when the truncated
 * index is outside the table; else 0. */
static int aspect_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= (int)(sizeof(k_aspect_vals)/sizeof(k_aspect_vals[0]))) return -1;
    openpete_native_set_aspect(k_aspect_vals[i]);
    k->stored_num = i;
    return 0;
}

/* An explicit factor table, not a power-of-two series: the resolve pass box-
 * filters the exact NxN source block, so every integer factor is well defined
 * and 3x is a real rung (9 samples/pixel, ~44% less render-target area than
 * 4x). The ladder stops at OPENPETE_SSAA_MAX because the supersampled targets
 * outgrow an 8GB card beyond it (see openpete.h). */
static const int         k_ssaa_vals[]  = { 1, 2, 3, 4 };
static const char *const k_ssaa_names[] = { "off", "2x", "3x", "4x" };
#define K_SSAA_N ((int)(sizeof(k_ssaa_vals)/sizeof(k_ssaa_vals[0])))
/** @brief Index of the k_ssaa_vals rung nearest to a supersampling factor.
 *
 * An --ssaa factor the ladder does not list still lands on a row; a tie
 * picks the lower rung. */
static int ssaa_idx_of(int factor) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < K_SSAA_N; i++) {
        int d = k_ssaa_vals[i] - factor;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
/** @brief The supersampling row index: the k_ssaa_vals rung nearest to the renderer's factor.
 *
 * Returns the stored value until the native renderer is up. */
static double ssaa_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return ssaa_idx_of(openpete_native_get_ssaa());
}
/** @brief Set the supersampling factor to entry `v` of k_ssaa_vals.
 *
 * Stores the index in the row. Returns -1, changing nothing, when the truncated
 * index is outside the ladder; else 0. */
static int ssaa_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= K_SSAA_N) return -1;
    openpete_native_set_ssaa(k_ssaa_vals[i]);
    k->stored_num = i;
    return 0;
}

/** @brief The renderer's vsync preference as 0 or 1.
 *
 * Returns the stored value until the native renderer is up, and also when
 * the renderer reports -1: the device never got an explicit preference (the
 * env or the surface picked the present mode), which the boot value already
 * records. */
static double vsync_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    int pref = openpete_native_get_vsync();
    return (pref < 0) ? k->stored_num : (pref ? 1 : 0);
}
/** @brief Set the renderer's vsync preference from `v` (nonzero enables) and store it in the row. */
static int vsync_set(knob_desc_t *k, double v) {
    openpete_native_set_vsync(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

static const char *const k_anchor_names[] = { "top-left", "top-center", "top-right",
                                              "bottom-left", "bottom-center",
                                              "bottom-right" };
/** @brief The performance-stats block anchor index into k_anchor_names.
 *
 * Returns the stored value until the native renderer is up. */
static double perfpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_anchor();
}
/** @brief Set the performance-stats block anchor to index `v` of k_anchor_names.
 *
 * The renderer ignores an index outside the anchor range, while the row stores
 * the truncated index regardless. */
static int perfpos_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}

/** @brief 1 when the on-screen notifications are enabled, else 0; the stored value until the native renderer is up. */
static double osd_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_osd_enabled() ? 1.0 : 0.0;
}
/** @brief Enable or disable on-screen notifications from `v` (nonzero enables).
 *
 * Stores the resulting 0 or 1 in the row. */
static int osd_set(knob_desc_t *k, double v) {
    openpete_osd_set_enabled(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
/** @brief 1 when the version OSD is shown, else 0. */
static double versionosd_get(const knob_desc_t *k) {
    /* No stored fallback: the backing flag is a plain host-side int, seeded
     * from the parsed options at boot whether or not the native renderer came
     * up, so the live read is always the resolved value. */
    (void)k;
    return openpete_native_version_osd() ? 1.0 : 0.0;
}
/** @brief Show or hide the version OSD from `v` (nonzero shows).
 *
 * Stores the resulting 0 or 1 in the row. */
static int versionosd_set(knob_desc_t *k, double v) {
    openpete_native_set_version_osd(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
/** @brief The notification anchor index into k_anchor_names.
 *
 * Returns the stored value until the native renderer is up. */
static double osdpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_anchor();
}
/** @brief Set the notification anchor to index `v` of k_anchor_names.
 *
 * The OSD ignores an index outside the anchor range, while the row stores the
 * truncated index regardless. */
static int osdpos_set(knob_desc_t *k, double v) {
    openpete_osd_set_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}
/** @brief The default notification timeout in seconds.
 *
 * The OSD holds milliseconds; the row shows seconds, the unit --osd-timeout
 * takes. Returns the stored value until the native renderer is up. */
static double osdtimeout_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_default_timeout_ms() / 1000.0;
}
/** @brief Set the default notification timeout from `v` seconds.
 *
 * Returns -1, changing nothing, when `v` is 0 or less. Otherwise converts to
 * milliseconds rounded to nearest, passes them to the OSD (which clamps to its
 * maximum), stores `v` in the row and returns 0. */
static int osdtimeout_set(knob_desc_t *k, double v) {
    if (v <= 0.0) return -1;
    openpete_osd_set_default_timeout_ms((uint32_t)(v * 1000.0 + 0.5));
    k->stored_num = v;
    return 0;
}

static const char *const k_perf_names[] = { "off", "fps", "detailed" };
/** @brief The performance-stats level as an index into k_perf_names.
 *
 * Returns the stored value until the native renderer is up. */
static double perfstats_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_stats_level();
}
/** @brief Set the performance-stats level to `v`, truncated to int.
 *
 * The renderer clamps the level to its valid range; the row stores the
 * unclamped truncated value. */
static int perfstats_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_stats((int)v);
    k->stored_num = (int)v;
    return 0;
}

static const char *const k_vprec_names[] = { "subpixel", "integer" };
static const char *const k_texmap_names[] = { "perspective", "affine" };
/** @brief 1 when the renderer snaps vertices to the integer grid, 0 when it keeps subpixel positions.
 *
 * Returns the stored value until the native renderer is up. */
static double vprec_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_vertex_snap_active() ? 1.0 : 0.0;
}
/** @brief Select vertex precision from `v`: 1 is the integer grid, else subpixel.
 *
 * The row stores the truncated `v` unchanged. */
static int vprec_set(knob_desc_t *k, double v) {
    openpete_native_set_vertex_precision((int)v == 1);
    k->stored_num = (int)v;
    return 0;
}

/** @brief The renderer's applied texture mapping: 0 perspective, 1 affine.
 *
 * Returns the stored value until the native renderer is up, the renderer's
 * state after that. The setter only queues the mode, which the renderer
 * applies with a pipeline rebuild at the next present, and the O menu moves
 * the same mode, so the row reads the renderer rather than stored_num. */
static double texmap_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_texture_mapping();
}
/** @brief Queue the texture mapping mode from `v`: 1 is affine, any other value perspective.
 *
 * The renderer applies it with a pipeline rebuild at the next present. The row
 * stores nothing, since texmap_get reads the renderer. */
static int texmap_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_texture_mapping((int)v == 1);
    return 0;
}

/** @brief The raster-quantize multiplier, 0 when off.
 *
 * Returns the stored value until the native renderer is up. */
static double rquant_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_raster_quantize();
}
/** @brief The resolved worker-thread count of the extraction work pool, with auto expanded. */
static double exthreads_get(const knob_desc_t *k) {
    (void)k;
    return openpete_workpool_threads();   /* resolved count (auto expanded) */
}
/** @brief 1 when the disc cache is active, else 0. */
static double disccache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_disc_cache_active() ? 1 : 0;
}
/* The resolved level-load delay (core/psx/fiber.c), declared here rather
 * than through openpete_fiber.h, whose SDL atomics the unit test does not
 * link. */
extern int openpete_cd_load_delay;
/** @brief The resolved level-load delay, read from openpete_cd_load_delay. */
static double loaddelay_get(const knob_desc_t *k) {
    (void)k;
    return openpete_cd_load_delay;
}
/** @brief 1 when the terrain cache came up, else 0. */
static double tercache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_tercache_ready() ? 1 : 0;   /* cache actually came up */
}
/** @brief Set the raster-quantize multiplier to `v`, truncated to int.
 *
 * The renderer treats a value of 0 or less as off and applies the change at the
 * next scene split; the row stores the unclamped truncated value. */
static int rquant_set(knob_desc_t *k, double v) {
    openpete_native_set_raster_quantize((int)v);
    k->stored_num = (int)v;
    return 0;
}

/** @brief The renderer's applied untextured-lighting view flag.
 *
 * A change the setter queues reads as the old value until the next present
 * applies it. */
static double notex_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_notex();
}
/** @brief Set the renderer's untextured-lighting flag from `v` (nonzero is on); the renderer applies it at the next present. */
static int notex_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_notex(v != 0.0);
    return 0;
}

/** @brief The renderer's applied wireframe view flag.
 *
 * A change the setter queues reads as the old value until the next present
 * applies it. */
static double wireframe_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_wireframe();
}
/** @brief Queue the renderer's wireframe view flag from `v` (nonzero is on).
 *
 * The renderer applies it at the next present. */
static int wireframe_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_wireframe(v != 0.0);
    return 0;
}

/** @brief The renderer's dither flag, which the O menu's DITHER row also sets.
 *
 * Returns the stored value until the native renderer is up. */
static double dither_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_dither();
}
/** @brief Set the renderer's dither flag from `v` (nonzero is on). */
static int dither_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_dither(v != 0.0);
    return 0;
}

static const char *const k_lod_names[] = { "original", "extended" };
static const char *const k_tervis_names[] = { "game", "frustum" };
/** @brief 1 when terrain visibility uses the view frustum, 0 when it follows the game's own verdict. */
static double tervis_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_terrain_visibility() == OPENPETE_TERRAIN_VIS_FRUSTUM ? 1.0 : 0.0;
}
/** @brief Select terrain visibility: nonzero `v` is frustum, zero is the game's verdict. */
static int tervis_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_terrain_visibility(v != 0.0 ? OPENPETE_TERRAIN_VIS_FRUSTUM
                                                    : OPENPETE_TERRAIN_VIS_GAME);
    return 0;
}
/** @brief 1 when the extended level-of-detail mode is on (openpete_no_lod), else 0. */
static double lod_get(const knob_desc_t *k) { (void)k; return openpete_no_lod ? 1.0 : 0.0; }
/** @brief Switch level-of-detail mode: nonzero is extended, zero original.
 *
 * Passes no fog scale, so the fog scale follows the mode unless fog was set
 * explicitly. */
static int lod_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_no_lod_set(v != 0.0, -1.0f);   /* fog follows the mode unless touched */
    return 0;
}

/** @brief 1 when fog is enabled, else 0. */
static double fog_get(const knob_desc_t *k) { (void)k; return openpete_fog_enabled() ? 1.0 : 0.0; }
/** @brief Enable or disable fog from `v` (nonzero is on). */
static int fog_set(knob_desc_t *k, double v) { (void)k; openpete_fog_set(v != 0.0); return 0; }

/** @brief The fog-distance multiplier, read from openpete_fog_scale. */
static double fogscale_get(const knob_desc_t *k) { (void)k; return openpete_fog_scale; }
/** @brief Set the fog-distance multiplier from the fog-scale row; a non-finite value is refused. */
static int fogscale_set(knob_desc_t *k, double v) {
    (void)k;
    if (!isfinite(v)) return -1;
    openpete_fog_scale_set((float)v);
    return 0;
}

static const char *const k_renderer_names[] = { "psycross", "vk" };
/** @brief The renderer selected at boot: 0 psycross, 1 vk.
 *
 * Returns 1 when options are unbound. */
static double renderer_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->renderer : 1.0; }

/* The game's default window size, handed in by the host loop once the game
 * layout is known. The registry has no game layout of its own: it is compiled
 * into console tests that link no game. Zero until set. */
static int s_default_win_w, s_default_win_h;

void openpete_knob_default_window_set(int w, int h) {
    s_default_win_w = w;
    s_default_win_h = h;
}

/** @brief Write the window size into `buf`.
 *
 * Shows "WxH" for a --window size, "WxH (default)" for the game's default size
 * once the host has supplied it, else "(default)". */
static void window_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->window_w > 0)
        snprintf(buf, cap, "%dx%d", g_opts->window_w, g_opts->window_h);
    else if (s_default_win_w > 0 && s_default_win_h > 0)
        snprintf(buf, cap, "%dx%d (default)", s_default_win_w, s_default_win_h);
    else
        snprintf(buf, cap, "(default)");
}

/* ---- Pacing ---- */

/** @brief The pacer's live render-fps target (0 is the match-sim pacer). */
static double renderfps_get(const knob_desc_t *k) { (void)k; return openpete_get_render_fps(); }
/** @brief Set the pacer's render-fps target to `v`, truncated to int.
 *
 * Values below 0 are clamped to 0 (match-sim pacing) by the pacer, which
 * applies the change at the host loop's next quiescent point. */
static int renderfps_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_set_render_fps((int)v);
    return 0;
}

/** @brief The pacer's live tick-scale factor. */
static double tickscale_get(const knob_desc_t *k) { (void)k; return openpete_get_tick_scale(); }
/** @brief Set the pacer's tick-scale factor to `v`.
 *
 * Returns -1, changing nothing, when `v` is 0 or less. Otherwise the pacer
 * clamps it to [0.05, 8] and applies it at the host loop's next quiescent
 * point; only wall-clock pacing changes, not the ticks executed. */
static int tickscale_set(knob_desc_t *k, double v) {
    (void)k;
    if (v <= 0.0) return -1;
    openpete_set_tick_scale(v);
    return 0;
}

/** @brief The applied runahead depth, which is 0 while the mod-capability gate forces it off. */
static double runahead_get(const knob_desc_t *k) { (void)k; return (double)openpete_runahead_get(); }
/** @brief Request runahead depth `v`, truncated to int.
 *
 * Returns -1, changing nothing, when `v` is negative. Otherwise the request is
 * clamped to 3 by the runtime and applied at the host loop's next quiescent
 * point. */
static int runahead_set(knob_desc_t *k, double v) {
    (void)k;
    if (v < 0) return -1;
    openpete_runahead_set((int)v);
    return 0;
}

/** @brief The native field of view in degrees; 0 selects the game's own. */
static double fov_get(const knob_desc_t *k) { (void)k; return (double)openpete_native_fov_deg(); }
/** @brief Set the native field of view from the fov row: 0 (the game's own) or [30, 120] degrees; anything else, NaN included, is refused. */
static int fov_set(knob_desc_t *k, double v) {
    if (!isfinite(v) || v < 0.0 || (v > 0.0 && v < 30.0) || v > 120.0) return -1;
    openpete_native_set_fov((float)v);
    k->stored_num = v;
    return 0;
}

/** @brief 1 while the host loop has the sim paused, else 0. */
static double pause_get(const knob_desc_t *k) { (void)k; return openpete_sim_paused(); }
/** @brief Request the sim paused (`v` nonzero) or resumed, as the pause key does. */
static int pause_set(knob_desc_t *k, double v) { (void)k; openpete_sim_pause_request(v != 0.0); return 0; }
/** @brief The Step tick button's gate: 1 while the sim is paused, else 0. */
static double step_ready(const knob_desc_t *k) { (void)k; return openpete_sim_paused(); }
/** @brief Request one tick of the paused sim, as the step key does. */
static int step_fire(knob_desc_t *k, double v) { (void)k; (void)v; openpete_sim_step_request(); return 0; }

/** @brief Request a rewind window of `v` seconds, truncated to int.
 *
 * Returns -1, changing nothing, when `v` is negative. Otherwise the host loop
 * resizes the ring at its next iteration, discarding the accumulated rewind
 * history (0 disables rewind); the row stores the value and 0 is returned. */
static int rewind_set(knob_desc_t *k, double v) {
    if (v < 0) return -1;
    openpete_rewind_window_set((int)v);
    k->stored_num = (int)v;
    return 0;
}

/* ---- Audio ---- */

/** @brief 1 when host audio is muted, else 0. */
static double mute_get(const knob_desc_t *k) { (void)k; return openpete_get_mute() ? 1.0 : 0.0; }
/** @brief Mute or unmute host audio from `v` (nonzero mutes). */
static int mute_set(knob_desc_t *k, double v) { (void)k; openpete_set_mute(v != 0.0); return 0; }

/** @brief The requested in-game SFX slider preset.
 *
 * The SFX and music setters below move only a host-side request; the game's
 * tick hook writes the guest slider, so the rows keep the live-row contract
 * although the value ends up in gameplay-readable RAM. */
static double sfxvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_sfx_volume(); }
/** @brief Request SFX slider preset `v`, truncated to int; the runtime clamps it. */
static int sfxvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_sfx_volume((int)v); return 0; }
/** @brief The requested music volume, read from the runtime. */
static double musicvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_music_volume(); }
/** @brief Request music volume `v`, truncated to int; the runtime clamps it. */
static int musicvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_music_volume((int)v); return 0; }

/* ---- Input ---- */

/** @brief The freecam mouse sensitivity. */
static double mousesens_get(const knob_desc_t *k) { (void)k; return (double)openpete_freecam_mouse_sensitivity(); }
/** @brief Set the freecam mouse sensitivity to `v`.
 *
 * Returns -1, changing nothing, unless `v` lies in [0, 100] (NaN fails); else
 * stores it in the row and returns 0. */
static int mousesens_set(knob_desc_t *k, double v) {
    if (!(v >= 0.0) || v > 100.0) return -1;
    openpete_freecam_set_mouse_sensitivity((float)v);
    k->stored_num = v;
    return 0;
}
/** @brief 1 when host mouse input is enabled, else 0. */
static double mouse_get(const knob_desc_t *k) { (void)k; return openpete_host_mouse_enabled() ? 1.0 : 0.0; }
/** @brief Enable or disable host mouse input from `v` (nonzero enables). */
static int mouse_set(knob_desc_t *k, double v) { (void)k; openpete_host_mouse_set_enabled(v != 0.0); return 0; }
/** @brief The host rumble strength.
 *
 * The setter moves this host request; the per-game feature writes the guest
 * enable flag on its tick hook. */
static double vibration_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_vibration(); }
/** @brief Request rumble strength `v`, truncated to int; the runtime clamps it. */
static int vibration_set(knob_desc_t *k, double v) { (void)k; openpete_set_vibration((int)v); return 0; }

/* ---- Recording ---- */

/** @brief 1 when the Recording rows are shown: an encoder is built in and --record named a path.
 *
 * --record is latched at boot, so a run with no path never encodes and every
 * row in the section is inert; on a build with no video encoder --record only
 * warns. The rows are hidden rather than disabled because neither case can
 * be changed from the overlay. */
static int recording_visible(const knob_desc_t *k) {
    (void)k;
    if (!openpete_record_supported()) return 0;
    return g_opts && g_opts->record_path && g_opts->record_path[0];
}

/** @brief Enable or disable drawing the fps readout into recorded video from `v` (nonzero enables).
 *
 * Stores the resulting 0 or 1 in the row. */
static int recfps_set(knob_desc_t *k, double v) {
    openpete_native_set_fps_in_record(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

/* ---- Paths (boot-resolved; display the same precedence the consumer used) */

/* The options string field behind each row whose value is a parsed string
 * with no live channel, by row id. */
static const struct { const char *id; size_t off; } k_optstr_fields[] = {
    { "disc",                 offsetof(openpete_options_t, disc_path) },
    { "texpack_dir",          offsetof(openpete_options_t, texpack_dir) },
    { "sfxpack_dir",          offsetof(openpete_options_t, sfxpack_dir) },
    { "controller_db",        offsetof(openpete_options_t, controller_db) },
    { "mods_dir",             offsetof(openpete_options_t, mods_dir) },
    { "mods",                 offsetof(openpete_options_t, mods_enabled) },
    { "card_dir",             offsetof(openpete_options_t, card_dir) },
    { "card1",                offsetof(openpete_options_t, card1_path) },
    { "card2",                offsetof(openpete_options_t, card2_path) },
    { "ingest",               offsetof(openpete_options_t, ingest_path) },
    { "game",                 offsetof(openpete_options_t, game_selector) },
    { "record",               offsetof(openpete_options_t, record_path) },
    { "record_encoder",       offsetof(openpete_options_t, record_encoder) },
    { "record_pixfmt",        offsetof(openpete_options_t, record_pixfmt) },
    { "record_audio_encoder", offsetof(openpete_options_t, record_audio_encoder) },
    { "bk2",                  offsetof(openpete_options_t, bk2_path) },
    { "bk2_record",           offsetof(openpete_options_t, bk2_record_path) },
};

/** @brief The parsed options string behind row `k`, or NULL when it is unset, the options are unbound, or the row has no k_optstr_fields entry. */
static const char *optstr_field(const knob_desc_t *k) {
    if (!g_opts) return NULL;
    for (size_t i = 0; i < sizeof k_optstr_fields / sizeof k_optstr_fields[0]; i++)
        if (!strcmp(k->id, k_optstr_fields[i].id))
            return *(const char *const *)((const char *)g_opts + k_optstr_fields[i].off);
    return NULL;
}

/** @brief Writes a string row's boot value: the parsed value, else the row's env var, else "(default)".
 *
 * The parsed value comes from the options field k_optstr_fields names for
 * the row id; an explicit empty value (such as --mods "") displays as
 * "(none)". */
static void optstr_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *cli = optstr_field(k);
    if (cli && !*cli) { /* explicit empty value, e.g. --mods "" */
        snprintf(buf, cap, "(none)");
        return;
    }
    const char *env = (k->env && !cli) ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", cli ? cli : (env ? env : "(default)"));
}

/* ---- Launch-only rows over parsed options ----
 * Flags read once at startup with no live channel: each getter returns the
 * parsed value, 0 before the options are bound. */

/** @brief 1 when --no-log-file turned the log file off, else 0. */
static double nologfile_get(const knob_desc_t *k) {
    (void)k;
    return (g_opts && g_opts->log_file && !strcmp(g_opts->log_file, "off")) ? 1.0 : 0.0;
}
/** @brief 1 when --compile-mods was given, else 0. */
static double compilemods_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->compile_mods : 0.0; }
/** @brief 1 when --no-card removed both memory cards, else 0. */
static double nocard_get(const knob_desc_t *k) { (void)k; return (g_opts && !g_opts->card_a_present) ? 1.0 : 0.0; }
/** @brief 1 when --card-unformatted was given, else 0. */
static double cardunf_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->card_unformatted : 0.0; }
/** @brief 1 when --adopt-saves was given, else 0. */
static double adopt_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->adopt_saves : 0.0; }
/** @brief The parsed --bk2-frame-offset value. */
static double bk2offset_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->bk2_frame_offset : 0.0; }
/** @brief 1 when --bk2-stop-on-end was given, else 0. */
static double bk2stop_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->bk2_stop_on_end) ? 1.0 : 0.0; }
/** @brief The parsed --record-crf value. */
static double reccrf_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->record_crf : 0.0; }
/** @brief The parsed --record-gop value. */
static double recgop_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->record_gop : 0.0; }
/** @brief 1 when --no-record-audio was given, else 0. */
static double norecaudio_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->record_no_audio) ? 1.0 : 0.0; }
/** @brief The parsed --psycross-native-raster value. */
static double pcnative_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->psycross_native_raster : 0.0; }

/* ---- CLI debug rows (boot-parsed; display-only) ---- */

/** @brief The row's env var value, or "(off)" when it is unset or empty. */
static void injectmouse_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *env = k->env ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", (env && *env) ? env : "(off)");
}

/** @brief Write the --render-paths list into `buf`.
 *
 * Writes "(all)" when options are unbound or the list is empty. */
static void renderpaths_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = (g_opts && g_opts->render_paths && *g_opts->render_paths)
                        ? g_opts->render_paths : NULL;
    snprintf(buf, cap, "%s", v ? v : "(all)");
}

/** @brief Write the active log level spec into `buf`, or "(default: info)" when the spec is empty. */
static void loglevel_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_get_spec();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(default: info)");
}

/** @brief Write the log file path into `buf`, or "(no log file)" when none is open. */
static void logfile_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_file_path();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(no log file)");
}

/** @brief Apply `v` as the log level spec.
 *
 * A value starting with `(` is the display placeholder and clears the spec, as
 * does NULL. Always returns 0. */
static int loglevel_set_str(knob_desc_t *k, const char *v) {
    (void)k;
    if (v && *v == '(') v = "";   /* "(default: info)" placeholder = clear */
    openpete_log_set_spec(v);
    return 0;
}

static const char *const k_logfmt_names[] = { "plain", "json" };
/** @brief The active log format as its k_logfmt_names index. */
static double logfmt_get(const knob_desc_t *k) {
    (void)k; return (double)openpete_log_get_format();
}
/** @brief Select the log format: `v` of 1 is JSON, any other value is plain. */
static int logfmt_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_log_set_format((int)v == 1 ? OPENPETE_LOG_JSON : OPENPETE_LOG_PLAIN);
    return 0;
}

/* ---- Debug env rows ----
 *
 * Generic rows over raw OPENPETE_* env strings: display is the live env
 * value ("(unset)" when absent). Editability follows the consumer pattern,
 * proven by grep per var:
 *  - FRESH consumers call getenv per use, so setenv IS their live channel
 *    (editable, KNOB_LIVE);
 *  - CACHED consumers latch the value into a function-local static on first
 *    use - setenv would silently do nothing, so those rows stay display-only
 *    (KNOB_RESTART_ONLY) until the latch is converted to an accessor.
 *    Promoting one is a per-knob audited change, not an overlay tweak. */

/* Bumped on every live env edit. A consumer that re-resolves a knob "once
 * per VBL" pays a getenv per VBL on every thread that reads it; keyed on
 * this epoch it pays one per EDIT. On Windows that matters twice over: the
 * CRT's getenv is ~46x glibc's, and concurrent getenv/putenv from two
 * threads has been observed to wedge the process. */
static atomic_uint s_env_epoch = 1;
unsigned knob_registry_env_epoch(void) {
    return atomic_load_explicit(&s_env_epoch, memory_order_acquire);
}

/* The guard every live env write runs inside (knob_registry_set_env_guard). */
static void (*s_env_guard)(int begin);

void knob_registry_set_env_guard(void (*guard)(int begin)) { s_env_guard = guard; }

/** @brief Write or unset an env var inside the env guard, then bump the registry env epoch.
 *
 * The epoch moves after the write with release order, so a consumer on
 * another thread that loads the new epoch (acquire) and then calls getenv
 * reads the new value; bumped before the write, a consumer could cache the
 * old value under the new epoch and keep it until the next edit.
 * @param name Variable name.
 * @param val New value; NULL or "" unsets the variable.
 * @return 0 on success, else the platform call's failure value. */
static int portable_setenv(const char *name, const char *val) {
    if (s_env_guard) s_env_guard(1);
#ifdef _WIN32
    const int rc = _putenv_s(name, val ? val : "");
#else
    const int rc = (!val || !*val) ? unsetenv(name) : setenv(name, val, 1);
#endif
    if (s_env_guard) s_env_guard(0);
    atomic_fetch_add_explicit(&s_env_epoch, 1u, memory_order_release);
    return rc;
}

/** @brief Write the row's env var value into `buf`.
 *
 * An unset variable shows as "(unset - <default>)" when the row has a non-empty
 * default string, else "(unset)". */
static void env_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *v = k->env ? getenv(k->env) : NULL;
    if (v)
        snprintf(buf, cap, "%s", v);
    else if (k->def_str && *k->def_str)
        snprintf(buf, cap, "(unset - %s)", k->def_str);
    else
        snprintf(buf, cap, "(unset)");
}

/** @brief Set the row's env var to `v`, through portable_setenv.
 *
 * Returns -1 for a row with no env var. A value starting with "(unset" (the
 * display placeholder), an empty value and NULL unset the variable. Otherwise
 * returns the portable_setenv result. */
static int env_set_str(knob_desc_t *k, const char *v) {
    if (!k->env) return -1;
    /* Editing back to the "(unset...)" placeholder (or clearing) unsets. */
    if (v && !strncmp(v, "(unset", 6)) v = "";
    return portable_setenv(k->env, v);
}

/* Compact row builders for the Debug sections. LIVE = FRESH consumer. */
#define ENVROW_LIVE(name, sect, def, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_LIVE, NULL, 0, 0, 0, NULL, NULL, env_get_str, env_set_str, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, def }
#define ENVROW_LATCHED(name, sect, def, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_RESTART_ONLY, NULL, 0, 0, 0, NULL, NULL, env_get_str, NULL, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, def }
/* An ENVROW_LATCHED row read while the device is created: device-creation
 * class 1 (openpete_knobs.h device_creation). */
#define ENVROW_DEVICE(name, sect, def, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_RESTART_ONLY, NULL, 0, 0, 0, NULL, NULL, env_get_str, NULL, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, def, .device_creation = 1 }
/* An ENVROW_LATCHED row whose unset value the program decides: `note` says
 * how, and the overlay shows "(unset)". */
#define ENVROW_LATCHED_NOTE(name, sect, note, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_RESTART_ONLY, NULL, 0, 0, 0, NULL, NULL, env_get_str, NULL, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, NULL, .default_note = note }

/* ---- Gameplay / debug ---- */

/** @brief The parsed --skip-to-level value, 0 before options are bound. */
static double skiplevel_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_level : 0.0; }
/** @brief The parsed --skip-to-cutscene value, 0 before options are bound. */
static double skipcutscene_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_cutscene : 0.0; }

/** @brief Write the parsed --skip-to-area value into `buf`.
 *
 * The form is "<level>:<area>"; "(none)" when options are unbound or no level
 * was given. */
static void skiparea_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->skip_to_area_level > 0)
        snprintf(buf, cap, "%d:%d", g_opts->skip_to_area_level, g_opts->skip_to_area);
    else
        snprintf(buf, cap, "(none)");
}

/** @brief Write the parsed --cheats string into `buf`, or "(none)" when options are unbound or it is empty. */
static void cheats_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    snprintf(buf, cap, "%s",
             (g_opts && g_opts->cheats && g_opts->cheats[0]) ? g_opts->cheats
                                                             : "(none)");
}

/** @brief 1 when the freecam is active, else 0. */
static double freecam_get(const knob_desc_t *k) { (void)k; return openpete_freecam_active() ? 1.0 : 0.0; }
/** @brief Toggle the freecam when `v` differs from its current state. */
static int freecam_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_freecam_active() != 0))
        openpete_freecam_toggle();
    return 0;
}

/** @brief 1 when the fill-tag picker is active, else 0. */
static double fillpick_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_active() ? 1.0 : 0.0; }
/** @brief Activate or deactivate the fill-tag picker from `v` (nonzero activates). */
static int fillpick_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_active(v != 0.0);
    return 0;
}

/** @brief 1 when the fill-tag picker preview is on, else 0. */
static double fillpick_prev_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_preview() ? 1.0 : 0.0; }
/** @brief Turn the fill-tag picker preview on or off from `v` (nonzero is on). */
static int fillpick_prev_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_preview(v != 0.0);
    return 0;
}

/** @brief 1 when the camera lock is active, else 0. */
static double camlock_get(const knob_desc_t *k) { (void)k; return openpete_camlock_active() ? 1.0 : 0.0; }
/** @brief Toggle the camera lock when `v` differs from its current state.
 *
 * Engaging the lock while the freecam is active first toggles the freecam off,
 * because both drive the fly axes. */
static int camlock_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_camlock_active() != 0)) {
        if (v != 0.0 && openpete_freecam_active())
            openpete_freecam_toggle();   /* the two share the fly axes */
        openpete_camlock_toggle();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the table                                                          */
/* ------------------------------------------------------------------ */

#define ENUM_ROW(vals) vals, (int)(sizeof(vals)/sizeof((vals)[0]))

/* Built-in rows outside the debug block. The registry hands out g_rows (these,
 * the debug rows, and whatever game modules contributed through
 * openpete_knob_add), never this array directly. */
static const knob_desc_t g_builtin[] = {
    /* -------- Display -------- */
    { "fullscreen", "Fullscreen", "Display", "--fullscreen", NULL,
      "Borderless-desktop fullscreen (same toggle as Alt+Enter / double-click).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fullscreen_get, fullscreen_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 110,
      .help_text = "launch in borderless-desktop fullscreen (default {def} = windowed;\n"
          "on|off also accepted). Alt+Enter / double-click / the O-menu\n"
          "DISPLAY row toggle it live. Display-only." },

    { "aspect", "Aspect ratio", "Display", "--aspect", NULL,
      "Hor+ render aspect; auto follows the window shape. Picking a fixed "
      "ratio windowed resizes the window to match.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_aspect_names), 0, 0,
      aspect_get, aspect_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "auto",
      .help_arg = "W:H|auto", .help_order = 70,
      .help_text = "Hor+ widescreen. {def} (default) = follow the window/fullscreen\n"
          "shape, filling it live on resize; W:H (e.g. 16:9, 21:9) = fixed\n"
          "render aspect (sizes the window; bars when the window mismatches)" },

    { "fov", "Field of view", "Display", "--fov", NULL,
      "Vertical FOV of the visible band in degrees, 30-120 (0 = the game's "
      "projection, 55). The horizontal FOV follows the Hor+ aspect: 74 at "
      "4:3, 90 at 16:9 for the default. Native renderer only; applies at "
      "the next present.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 30, 120,
      fov_get, fov_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .zero_label = "Original",
      .def_str = "0",
      .help_arg = "DEG", .help_order = 80,
      .help_text = "vertical FOV of the visible band, native renderer (default 55°,\n"
          "range 30-120; the horizontal FOV follows the Hor+ aspect: 74° at\n"
          "4:3, 90° at 16:9 for the default). Inert under --renderer psycross." },

    { "ssaa", "Supersampling", "Display", "--ssaa", NULL,
      "Native supersampling factor. Applying reallocates the render target "
      "next present (brief hitch at large factors; GPU-max capped).",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_ssaa_names), 0, 0,
      ssaa_get, ssaa_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "N", .help_order = 90,
      .help_text = "native vk supersampling 1..4 (default {def}=off); renders the live\n"
          "window at NxN res and box-downsamples. The 4x ceiling keeps the\n"
          "render targets inside an 8GB card at 1080p; higher factors are\n"
          "clamped, and the extent is further capped to the GPU's max image\n"
          "size. Display-only." },

    { "vsync", "VSync", "Display", "--vsync", "OPENPETE_PRESENT_MODE",
      "FIFO (on) vs immediate/mailbox (off). Applying rebuilds the swapchain "
      "on the next frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      vsync_get, vsync_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "on (FIFO), unless OPENPETE_PRESENT_MODE or the device selects another present mode",
      .device_creation = 2,
      .help_arg = "0|1", .help_order = 100,
      .help_text = "native vk present mode (default 1 = vsync on / FIFO; 0 = vsync off\n"
          "for uncapped fps / low latency; on|off also accepted). Display-only." },

    { "perf_stats", "Performance stats", "Display", "--perf-stats", NULL,
      "Corner overlay (native renderer): off, the FPS counter, or the "
      "counter plus a pacing readout - VBL rate, deadline slips, late ticks "
      "per 1000, and the shed / interpolation share of presents.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_perf_names), 0, 0,
      perfstats_get, perfstats_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "fps",
      .help_arg = "LEVEL", .help_order = 130,
      .help_text = "on-screen performance stats: off | {def} (default) | detailed.\n"
          "detailed adds the VBL rate, slips, late ticks per 1000 and the\n"
          "shed / interpolation share. 0|1|2 also accepted. Display-only." },

    { "perf_position", "Stats position", "Display", "--perf-position", NULL,
      "Which corner or edge the performance-stats block anchors to. Shares "
      "the anchor ledger with the OSD: whichever surface claims a corner "
      "first, the other stacks clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      perfpos_get, perfpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "top-right",
      .help_arg = "ANCHOR", .help_order = 150,
      .help_text = "where the stats block sits: {def} (default) | top-left | top-center | bottom-left | bottom-center | bottom-right" },

    { "osd", "Notifications", "Display", "--osd", NULL,
      "On-screen notifications when a setting changes, in the game's own gold "
      "chrome. Mods raise them too.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      osd_get, osd_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "on",
      .help_arg = "on|off", .help_order = 160,
      .help_text = "on-screen notifications when a setting changes (default {def})" },

    { "version_osd", "Version line", "Display", "--version-osd", NULL,
      "A release build's two-second version line at the bottom left. A "
      "distributed build that is not a clean tag keeps its version up for the "
      "whole session and this does not turn it off, so a screenshot always "
      "names the build it came from; a locally built binary shows no line.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      versionosd_get, versionosd_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "on",
      .help_arg = "on|off", .help_order = 190,
      .help_text = "the release build's two-second version line at the bottom\n"
          "left (default {def}). A distributed build that is not a clean\n"
          "tag shows its version for the whole session and this does\n"
          "not suppress it; a locally built binary shows none." },

    { "osd_position", "Notification position", "Display", "--osd-position", NULL,
      "Which corner or edge notifications stack at. Sharing a corner with the "
      "stats block is fine: the stats block claims first and the toasts stack "
      "clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      osdpos_get, osdpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "top-left",
      .help_arg = "ANCHOR", .help_order = 170,
      .help_text = "where notifications stack; same six anchors (default {def})" },

    { "osd_timeout", "Notification timeout", "Display", "--osd-timeout", NULL,
      "Seconds a notification stays up before it fades. A caller may ask for "
      "its own; this is what it gets when it does not.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.5, 30.0,
      osdtimeout_get, osdtimeout_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "3.0",
      .help_arg = "SECONDS", .help_order = 180,
      .help_text = "how long a notification stays up (default {def}, max 30)" },

    { "vertex_precision", "Vertex precision", "Display", "--vertex-precision", NULL,
      "PSX integer-grid vertex snap (authentic polygon wobble) vs smooth "
      "subpixel. Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_vprec_names), 0, 0,
      vprec_get, vprec_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "subpixel",
      .help_arg = "MODE", .help_order = 280,
      .help_text = "native vk vertex screen-coordinate precision: {def} (default =\n"
          "smooth sub-pixel projection) | integer (snap 3D-world vertices to the\n"
          "PSX 512x240 pixel grid = the original PS1 polygon wobble). 2D HUD stays\n"
          "smooth; depth/UVs untouched. Native renderer only. Display-only." },

    { "raster_quantize", "Raster quantize", "Display", "--raster-quantize", NULL,
      "Point-resample the 3D scene to N x the PSX raster (1 = the authentic "
      "512x240 grid - 224 lines visible on screen, letterbands trimmed; "
      "0 = off). One sample per cell, as the hardware rasterizer produced, "
      "always aligned with the integer vertex snap. Display-only.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 16,
      rquant_get, rquant_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "0 (off), or 1 under --vertex-precision integer",
      .help_arg = "N", .help_order = 290,
      .help_text = "point-resample the 3D scene (pre-HUD) to N x the PSX raster (1 =\n"
          "authentic 512x240 grid, of which the 224 visible lines span the\n"
          "screen - the letterband rows are trimmed, so on-screen this is\n"
          "512x224 as on a real TV; every N stays aligned with the\n"
          "--vertex-precision integer grid). 0 = off. Default: 1 under\n"
          "--vertex-precision integer, else off. Display-only." },

    { "extract_threads", "Extraction threads", "Display", "--extract-threads", NULL,
      "Worker threads for the native render extraction (terrain sector walk). "
      "Shows the resolved count (0 = auto at launch). Output is byte-identical "
      "at any count; 1 = serial. Boot-latched; display-only.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 1, 8,
      exthreads_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 300,
      .help_text = "worker threads for the native render extraction (terrain sector\n"
          "walk). 0 = auto (logical cores / 2, max 8), 1 = serial (no\n"
          "threads). Output is byte-identical at any count (item-order\n"
          "merge). Boot-latched. Display-only." },

    { "terrain_cache", "Terrain GPU cache", "Display", "--terrain-cache", NULL,
      "Retained GPU terrain (default on): unclaimed sectors render from "
      "world-space vertex chunks with the per-face verdicts mirrored in the "
      "vertex stage; claimed sectors (near / blended-over-portal / safety) "
      "stay on the CPU walk. Shows whether the cache came up (needs a "
      "hardware GPU with fp64; software vk runs the CPU walk). 0 = kill "
      "switch. Boot-latched; display-only.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      tercache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 260,
      .help_text = "retained-terrain GPU cache: static sectors render from persistent\n"
          "GPU meshes with the per-face verdicts mirrored in the vertex\n"
          "stage (default {def}; display-only; needs a hardware GPU with fp64,\n"
          "else the CPU walk runs as before). 0 = kill switch." },

    { "terrain_visibility", "Terrain visibility", "Display", "--terrain-visibility",
      "OPENPETE_TERRAIN_VISIBILITY",
      "Which terrain sectors are candidates to draw. frustum (default) takes "
      "every sector whose bounding sphere is inside the displayed frame, "
      "hidden or not, so the geometry the original hides - the world outside "
      "the room the camera sits in, the side wedges of a frame wider than "
      "4:3 - draws and the depth buffer decides what is seen; a departure "
      "from the original presentation. game takes the game's own visibility "
      "verdict (faithful). Either way the frustum test, the per-sector tier "
      "verdicts, the retained GPU cache and the occlusion pre-pass still cull. "
      "Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_tervis_names), 0, 0,
      tervis_get, tervis_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "frustum",
      .help_arg = "MODE", .help_order = 250,
      .help_text = "terrain candidate set: {def} (default) = every sector whose\n"
          "bounding sphere is inside the displayed frame, hidden or not, so\n"
          "the geometry the original hides (inside-geometry views, the side\n"
          "wedges wider than 4:3) draws and the depth buffer decides what is\n"
          "seen - a departure from the original presentation | game = the\n"
          "game's own visibility verdict (faithful). Native renderer only." },

    ENVROW_LATCHED("TERRAIN_HDFOLD", "Display", "1",
      "HD texture-replacement fold (default on): with a texture pack active, "
      "pack-matched terrain faces bake replacement entries into the retained "
      "GPU cache (atlas + per-entry fog) instead of claiming their sectors "
      "back to the CPU walk; unbakeable faces (blended targets, overflow) "
      "still claim. Inert without a pack. 0 = kill switch (tier-1 per-sector "
      "claims). Boot-latched; display-only."),

    { "notex", "Untextured (lighting)", "Display", "--notex", "OPENPETE_NOTEX",
      "Debug view: draw the 3D scene with textures replaced by neutral grey, "
      "showing the raw gouraud vertex lighting. Cutout silhouettes stay real. "
      "Applying re-bakes every scene pipeline (one-time hitch). Display-only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      notex_get, notex_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 330,
      .help_text = "1 = draw the 3D scene with textures replaced by neutral grey,\n"
          "showing the raw vertex lighting; cutout silhouettes stay real.\n"
          "Default: env OPENPETE_NOTEX, else {def}. Native renderer only.\n"
          "Display-only." },

    { "wireframe", "Wireframe (lighting)", "Display", "--wireframe", "OPENPETE_WIREFRAME",
      "Debug view: draw every 3D scene triangle as its three edges, coloured "
      "by the gouraud vertex lighting (textures replaced by neutral grey, as "
      "in the untextured view), so the terrain subdivision shows as drawn. "
      "Depth-tested: a line hides the lines behind it, not the faces. HUD and "
      "menus stay filled. Falls back to the filled untextured view on a GPU "
      "without line rasterization. Applying re-bakes every scene pipeline "
      "(one-time hitch). Display-only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      wireframe_get, wireframe_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 340,
      .help_text = "1 = draw every 3D scene triangle as its three edges, coloured by the\n"
          "vertex lighting (textures replaced by neutral grey, as NOTEX); the\n"
          "HUD and menus stay filled. Shows the terrain subdivision as drawn.\n"
          "Default: env OPENPETE_WIREFRAME, else {def}. Native renderer only.\n"
          "Display-only." },

    { "dither", "Color depth", "Display", "--dither", NULL,
      "PS1 15-bit colour: quantize the 3D scene to RGB555 with the GPU's 4x4 "
      "ordered dither matrix on the PSX framebuffer grid (authentic banding + "
      "crosshatch). Display-only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      dither_get, dither_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 310,
      .help_text = "1 = PS1 15-bit colour: quantize the 3D scene to RGB555 with the\n"
          "GPU's 4x4 ordered dither matrix on the PSX 512x240 framebuffer\n"
          "grid (the authentic banding + crosshatch the smooth float\n"
          "pipeline removed). {def} = off (default). Display-only." },

    { "texture_mapping", "Texture mapping", "Display", "--texture-mapping", NULL,
      "perspective = modern perspective-correct interpolation; affine = the "
      "PSX screen-linear texture swim/warp. Applying re-bakes every scene "
      "pipeline at the next present (brief hitch). Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_texmap_names), 0, 0,
      texmap_get, texmap_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "perspective",
      .help_arg = "MODE", .help_order = 320,
      .help_text = "texture/colour interpolation: {def} (default = modern\n"
          "perspective-correct) | affine (PSX screen-linear = the original\n"
          "PS1 texture swim/warp). Native renderer only. Display-only." },

    { "lod", "LOD", "Display", "--lod", NULL,
      "original = authentic PSX detail bands; extended = full detail at any "
      "distance (triangle-bound in dense vistas). Fog follows the mode "
      "unless explicitly set.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_lod_names), 0, 0,
      lod_get, lod_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "original",
      .help_arg = "MODE", .help_order = 240,
      .help_text = "sector level-of-detail: {def} (default = authentic PSX LOD band) |\n"
          "extended (draw every visible sector at high-poly, out to the cull\n"
          "distance and beyond). Native renderer only. (--no-lod = extended.)" },

    { "fog", "Fog", "Display", NULL, NULL,
      "Distance fog on/off (off = the 100x far-scale sentinel). LOD switches "
      "move this automatically until it is touched.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fog_get, fog_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "on" },

    { "fog_scale", "Fog scale", "Display", "--fog-scale", NULL,
      "Fog-distance multiplier (1 = authentic curve, 100 = effectively off). "
      "Terrain re-resolves it every walk; a manual value takes fog off the "
      "LOD autopilot.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.25f, 128.0f,
      fogscale_get, fogscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "1, or 100 under --lod extended",
      .help_arg = "N", .help_order = 270,
      .help_text = "terrain fog-distance multiplier: 1 = authentic PSX curve, N = fog\n"
          "saturates N x farther (100 ~= fog off, the far-detail look).\n"
          "Default: 1, or 100 under --lod extended. Range [0.25,128]." },

    { "renderer", "Renderer", "Display", "--renderer", NULL,
      "Which rasterizer draws the picture: the native Vulkan renderer (vk, "
      "default) or the PsyCross GL reference (psycross). The native window "
      "displays either one.",
      KNOB_ENUM, KNOB_RESTART_ONLY, ENUM_ROW(k_renderer_names), 0, 0,
      renderer_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "vk",
      .help_arg = "MODE", .help_order = 50,
      .help_text = "which rasterizer draws: vk (native Vulkan{default_mark:vk}) | psycross\n"
          "(the GL reference{default_mark:psycross}). The native window displays either." },

    { "window", "Window size", "Display", "--window", NULL,
      "Launch window size (drag the window to resize live). The default is "
      "the game's drawn viewport at two window rows per VRAM row.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, window_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the game's drawn viewport at two window rows per VRAM row, widened to a fixed --aspect",
      .help_arg = "WxH", .help_order = 60,
      .help_text = "window size (default: the game's drawn viewport at 2x,\n"
          "640x448 on Spyro 1, 640x432 on Spyro 2 and 3)" },

    { "background", "Background window", "Display", "--background", "OPENPETE_BACKGROUND",
      "Open the window visible and at its normal size but unfocused, not "
      "activated on show, and behind the other windows where the platform "
      "allows it; clicking it or its taskbar button focuses it. Presents, pacing and "
      "screenshots are unchanged. Wins over Fullscreen. Read once while the "
      "window is created, so the row shows the boot value; raising or "
      "focusing the window afterwards does not change it.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .device_creation = 1,
      .help_arg = "[0|1]", .help_order = 120,
      .help_text = "open the window at its normal size and visible, but do not take\n"
          "focus, do not activate on show, and sit behind the other windows\n"
          "where the platform allows it. Presents, pacing and screenshots are\n"
          "unchanged; clicking the window or its taskbar button focuses it. Wins over\n"
          "--fullscreen. For an unattended windowed run alongside\n"
          "--input-lock (default {def} = off; ignored with --headless).\n"
          "Display-only." },

    /* -------- Pacing -------- */
    { "render_fps", "Render FPS", "Pacing", "--render-fps", NULL,
      "Target present rate (0 = legacy match-sim pacer). Above ~30 arms "
      "sub-tick interpolation; vsync clamps to refresh. Shows the live "
      "pacer target (an unset launch value resolves to display refresh).",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 1000,
      renderfps_get, renderfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the display's refresh rate in a windowed run; 0 (the match-sim pacer) under --headless",
      .help_arg = "N", .help_order = 220,
      .help_text = "target render Hz (default: the display's refresh rate; 0=match\n"
          "sim/30fps legacy pacer, >0=wall-clock dual-pacer + interpolation)" },

    { "tick_scale", "Tick scale", "Pacing", "--tick-scale", NULL,
      "Engine speed multiplier (slow-mo / speed-up; sim and presents "
      "together). Applies on the next host iteration; pacing only, the sim "
      "runs identical ticks on a stretched wall clock.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.05f, 8.0f,
      tickscale_get, tickscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1.0",
      .help_arg = "F", .help_order = 230,
      .help_text = "engine speed x F (default {def}); scales sim AND present cadence\n"
          "together so sub-tick frames-per-tick is invariant. 0.5 = half-\n"
          "speed slow-mo to inspect high-FPS interpolation. Display-only." },

    { "pause", "Pause sim", "Pacing", NULL, NULL,
      "Pause the game's ticks while presents continue (same toggle as the "
      "pause key, F7 by default). Audio mutes and rumble stops while paused.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      pause_get, pause_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off" },

    { "frame_step", "Step tick", "Pacing", NULL, NULL,
      "Advance the paused game by one tick (same as the step key, F8 by "
      "default).",
      KNOB_ACTION, KNOB_LIVE, NULL, 0, 0, 0,
      step_ready, step_fire, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .action_label = "Step tick" },

    { "runahead", "Runahead", "Pacing", "--runahead", NULL,
      "Present-timeline lead of N sim ticks (latency cut, ~N extra ticks of "
      "CPU). Applies next host iteration; the mod-capability gate can "
      "refuse (row falls back to 0). Presents snap for one tick on change.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 3,
      runahead_get, runahead_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 210,
      .help_text = "lead the presented timeline by N sim ticks (default {def} = off).\n"
          "1 recommended: cancels the interpolation tick exactly - real-\n"
          "PSX input feel with high-fps motion, every animation frame\n"
          "shown. 2 = sub-console latency but input edges skip one\n"
          "animation onset frame (reads as 'uncanny'). Costs ~N extra\n"
          "sim ticks/tick of CPU; gameplay state is UNCHANGED." },

    { "rewind", "Rewind window", "Pacing", "--rewind", NULL,
      "Backspace-rewind window in seconds (0 = off). APPLYING CLEARS the "
      "accumulated rewind history; ~3.1 MiB per second.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 600,
      get_stored, rewind_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 200,
      .help_text = "hold Backspace to rewind up to N seconds (default {def} = off;\n"
          "~2 MiB RAM per second of window; same-level only)" },

    /* -------- Audio -------- */
    { "mute", "Mute", "Audio", "--mute", NULL,
      "Mute all audio at the OpenAL layer (SPU SFX + XA music + WAV "
      "override). Live; the F7 pause exit restores this baseline.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mute_get, mute_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 470,
      .help_text = "mute all audio at the OpenAL layer (default {def} = audio on; on|off also accepted)" },

    { "sfx", "SFX volume", "Audio", "--sfx", NULL,
      "Preset the game's own SOUND EFFECTS slider, 0 (OFF) to 10 (max); "
      "-1 leaves it alone. Stamped at a tick boundary and re-stamped over "
      "the title screen / save-file load, until the player moves that "
      "slider in the game's options menu.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      sfxvol_get, sfxvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 480,
      .help_text = "preset the game's SOUND EFFECTS slider: 0 (OFF) .. 10 (max), or on|off.\n"
          "Survives the title screen and a save-file load; the game's own options\n"
          "menu takes it back the moment you move that slider. -1 = don't preset" },

    { "music", "Music volume", "Audio", "--music", NULL,
      "Preset the game's own MUSIC slider, 0 (OFF) to 10 (max); -1 leaves "
      "it alone. Same stamping rules as the SFX row.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      musicvol_get, musicvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 490,
      .help_text = "preset the game's MUSIC slider, same scale and rules as --sfx" },

    /* -------- Input -------- */
    { "vibration", "Vibration", "Input", "--vibration", "OPENPETE_VIBRATION",
      "Controller rumble strength, 0 (off) to 10 (max); -1 leaves the game's "
      "own vibration setting alone (the default, which means on). 10 applies "
      "the measured motor response unscaled. The game's enable flag is stamped at a "
      "tick boundary and released if the player toggles vibration in the "
      "options menu; the motors are always silent headless or unfocused.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      vibration_get, vibration_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 500,
      .help_text = "controller rumble: 0 (off) .. 10 (max), or on|off.\n"
          "Default {def} leaves the game's own setting alone (= on);\n"
          "0 silences the motors without changing that setting.\n"
          "10 is the full measured response. Always silent when\n"
          "headless or when the window is not focused." },

    { "mouse", "Mouse", "Input", "--mouse", "OPENPETE_MOUSE",
      "The host mouse channel. Off drops every mouse motion, button and wheel "
      "event from a real device before any consumer sees it and refuses "
      "pointer capture, so the pointer over the window stays an ordinary "
      "desktop cursor and mouse look, the settings menu and the dev overlay "
      "receive nothing. The scripted injection stream is unaffected.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mouse_get, mouse_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 510,
      .help_text = "host mouse channel: 0 ignores every mouse movement, button and\n"
          "wheel event and leaves the pointer a free desktop cursor over the\n"
          "window; mouse look, the settings menu pointer and the dev overlay\n"
          "all go quiet (default {def} = on; on|off also accepted)" },

    { "input_lock", "Input lock", "Input", "--input-lock", "OPENPETE_INPUT_LOCK",
      "Ignore every host input device: the keyboard (game keys and every "
      "hotkey, this overlay's included), the mouse (as Mouse off) and "
      "gamepads. A bk2 replay and the scripted injection stream still drive "
      "the run, and closing the window still exits. Display-only: switched on "
      "from here it would take away the input needed to switch it off.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[0|1]", .help_order = 520,
      .help_text = "ignore every host input device: keyboard (game keys and every\n"
          "hotkey), mouse (as --mouse 0) and gamepads. A --bk2 replay still\n"
          "drives the game and closing the window still exits. For a window\n"
          "that must not react to host input (default {def} = off)" },

    { "mouse_sensitivity", "Mouse sensitivity", "Input", "--mouse-sensitivity", NULL,
      "Mouse look scale for the free camera and the camlock: angle units per "
      "count = 0.5 * this, so 1.0 is one full turn per 8192 counts. Read at "
      "every apply; a change rebases the mouse term, so it never rotates the "
      "view retroactively. Mouse look itself binds through the look_axis_x/y "
      "keymap actions (mouse:x, mouse:-y).",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.0, 100.0,
      mousesens_get, mousesens_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1.0",
      .help_arg = "S", .help_order = 530,
      .help_text = "mouse look scale for the free camera and camlock (default {def};\n"
          "0.5 = one full turn per 8192 counts). Mouse look binds through\n"
          "[keys.host] look_axis_x = \"pad:rightx,mouse:x\" and\n"
          "look_axis_y = \"pad:-righty,mouse:-y\"; \"mouse:y\" inverts, \"none\" unbinds" },

    /* -------- Paths -------- */
    { "disc", "Disc image", "Paths", "--disc", NULL,
      "BIN/CUE the game boots from.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game library's most recent disc",
      .help_arg = "PATH", .help_order = 10,
      .help_text = "disc image: the .cue, or the .bin/.img/.iso itself. A cue\n"
          "whose FILE line names a missing bin falls back to the cue's\n"
          "own basename and then to the lone .bin beside it." },

    { "disc_cache", "Disc cache", "Paths", "--disc-cache", "OPENPETE_DISC_CACHE",
      "Disc image resident in memory (default on): a background loader "
      "fills a host array in the game's boot order; a resident sector is a "
      "memcpy and a missing one is read from the file at once (a read over "
      "a network share costs tens of ms per burst and lands as a late tick). "
      "Shows whether "
      "the cache attached. 0 = read the file per sector (A/B). Boot-latched; "
      "bytes identical either way.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      disccache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 30,
      .help_text = "keep the disc image resident in memory, filled by a background\n"
          "loader in the game's boot order (default {def}): a resident sector is\n"
          "a copy, a missing one is read from the file at once. 0 = read\n"
          "the file per sector (A/B). Bytes identical either way." },

    { "load_delay", "Load delay (VBLs)", "Paths", "--load-delay", "OPENPETE_LOAD_DELAY",
      "VBLs of guest time every level load waits: the load's disc reads land "
      "at once and their completion is deferred, so the game's own wait runs "
      "for the same count on every machine (0 = instant). Omitted = the game's "
      "default. A bk2 records the value and replays only under it. Boot-"
      "latched; a load in flight reads the value it started with.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 3600,
      loaddelay_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "N", .help_order = 40,
      .help_text = "VBLs of guest time every level load waits (the game's default\n"
          "when omitted; 0 = instant). Recorded into a bk2's header and\n"
          "checked on replay." },

    { "texpack_dir", "Texture pack dir", "Paths", "--texpack-dir", "OPENPETE_TEXPACK_DIR",
      "HD texture-replacement PNG directory (scanned once at renderer init).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_TEXPACK_DIR, else assets/textures next to the executable",
      .help_arg = "PATH", .help_order = 360,
      .help_text = "directory of HD texture-replacement PNGs (default: env OPENPETE_TEXPACK_DIR,\n"
          "else <exe>/assets/textures/). Native renderer only." },

    { "controller_db", "Controller DB", "Paths", "--controller-db", "OPENPETE_CONTROLLER_DB",
      "Extra SDL controller-mapping database, loaded after the shipped "
      "gamecontrollerdb.txt and the per-user one and outranking both. A pad "
      "SDL has no mapping for never reaches the decode path; one line for one "
      "GUID is enough to fix that. Read once while the gamepad subsystem "
      "comes up.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_CONTROLLER_DB, else none",
      .help_arg = "PATH", .help_order = 370,
      .help_text = "extra SDL controller-mapping database, loaded after the shipped one and\n"
          "the per-user gamecontrollerdb.txt, so it outranks both (default: env\n"
          "OPENPETE_CONTROLLER_DB, else neither)." },

    { "sfxpack_dir", "Sound pack dir", "Paths", "--sfxpack-dir", "OPENPETE_SFXPACK_DIR",
      "Drop-in sound-effect WAV directory (rescanned at every level load).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_SFXPACK_DIR, else assets/sfx next to the executable",
      .help_arg = "PATH", .help_order = 350,
      .help_text = "directory of drop-in sound-effect WAVs named by the sfx census\n"
          "(default: env OPENPETE_SFXPACK_DIR, else assets/sfx next to the exe)" },

    { "mods_dir", "Mods dir", "Paths", "--mods-dir", "OPENPETE_MODS_DIR",
      "Mods root (mod.toml subdirs; load order via enabled.txt).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_MODS_DIR, else mods next to the executable",
      .help_arg = "PATH", .help_order = 380,
      .help_text = "mods root: each subdir with a mod.toml is a mod, compiled on load;\n"
          "enabled.txt fixes order (= priority). Default: env OPENPETE_MODS_DIR,\n"
          "else <exe>/mods. No directory = no mods, byte-identical vanilla." },

    { "mods", "Enabled mods", "Paths", "--mods", "OPENPETE_MODS",
      "Enabled modset override (comma-separated ids; order = priority). "
      "Empty = no mods. Unset = the mods dir's enabled.txt.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_MODS, else the mods directory's enabled.txt",
      .help_arg = "ID[,ID...]", .help_order = 390,
      .help_text = "run exactly these mods, overriding mods/enabled.txt (order IS\n"
          "priority, later = higher). --mods \"\" enables none (vanilla run).\n"
          "Default: env OPENPETE_MODS, else enabled.txt." },

    { "card_dir", "Memory card dir", "Paths", "--card-dir", "OPENPETE_CARD_DIR",
      "Directory holding the .mcr memory-card images.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_CARD_DIR, else cards next to the executable in a portable install, else the per-user state directory",
      .help_arg = "PATH", .help_order = 430,
      .help_text = "directory holding the memory-card .mcr images (default: env OPENPETE_CARD_DIR,\n"
          "else <exe>/cards for a portable install, or the per-user state dir\n"
          "openpete/cards. Two formatted cards inserted by default." },

    { "log_file", "Log file", "Paths", "--log-file", "OPENPETE_LOG_FILE",
      "Path stderr is mirrored to for bug reports; empty = disabled "
      "(--no-log-file). Opened once at startup, so read-only here.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, logfile_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_LOG_FILE, else <state dir>/logs/openpete.log",
      .device_creation = 1,
      .help_arg = "PATH", .help_order = 640,
      .help_text = "mirror stderr to PATH instead of the default\n"
          "<state>/logs/openpete.log; env OPENPETE_LOG_FILE=..." },

    { "no_log_file", "No log file", "Paths", "--no-log-file", NULL,
      "Write no log file at all. Read once at startup.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nologfile_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .device_creation = 1,
      .help_arg = NULL, .help_order = 650,
      .help_text = "do not write a log file at all" },

    { "ingest", "Ingest disc image", "Paths", "--ingest", NULL,
      "Disc image copied into the game library next to the executable and "
      "booted; later runs boot it with no --disc. A bare disc-image path on "
      "the command line means the same. Read once at startup.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none",
      .cli_only = 1,
      .help_arg = "PATH", .help_order = 20,
      .help_text = "copy a disc image (.cue/.bin) into the library\n"
          "next to the exe and boot it; later runs need no\n"
          "--disc at all (the original file can be deleted)" },

    { "game", "Game", "Paths", "--game", NULL,
      "The game this session runs (spyro-1, spyro-2, spyro-3), which "
      "selects the [game.<id>] scope of openpete.toml. A disc of another "
      "game refuses the launch, as does a game this executable does not "
      "run. Read once at startup.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the disc's game, else the library entry's, else this executable's",
      .cli_only = 1,
      .help_arg = "ID", .help_order = 25,
      .help_text = "the game to run: spyro-1, spyro-2 or spyro-3 (default: the\n"
          "disc's game); a disc of another game refuses the launch" },

    { "compile_mods", "Compile mods and exit", "Paths", "--compile-mods", NULL,
      "Compile every mod with a src/ directory in the mods directory against "
      "the shipped SDK, then exit; nonzero exit on a failed compile.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      compilemods_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .cli_only = 1,
      .help_arg = NULL, .help_order = 400,
      .help_text = "compile EVERY mod with src/ in the mods dir (see --mods-dir;\n"
          "ignores enabled.txt) against <exe>/sdk and exit; nonzero exit\n"
          "on any failed compile (the cmake build-time mods gate)" },

    { "card1", "Memory card 1", "Paths", "--card1", NULL,
      "The .mcr memory-card image in port A. Read once at startup.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "card1.mcr in the memory-card directory",
      .help_arg = "PATH", .help_order = 410,
      .help_text = "use a specific .mcr memory-card image for port A (default: <card-dir>/card1.mcr)" },

    { "card2", "Memory card 2", "Paths", "--card2", NULL,
      "The .mcr memory-card image in port B. Read once at startup.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "card2.mcr in the memory-card directory",
      .help_arg = "PATH", .help_order = 420,
      .help_text = "use a specific .mcr memory-card image for port B (default: <card-dir>/card2.mcr)" },

    { "no_card", "No memory cards", "Paths", "--no-card", NULL,
      "Present no memory card in either port, so the game cannot save. Read "
      "once at startup.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nocard_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 440,
      .help_text = "present no memory card in either port (saving disabled)" },

    { "card_unformatted", "Unformatted card", "Paths", "--card-unformatted", NULL,
      "Provision a blank unformatted card, so the game's own format screen "
      "runs. Read once at startup.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      cardunf_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 450,
      .help_text = "provision a blank unformatted card so the in-game format screen runs" },

    { "adopt_saves", "Adopt saves", "Paths", "--adopt-saves", NULL,
      "Re-tag the memory-card saves to the running modset; a save written "
      "under another modset warns on load until adopted. Read once at "
      "startup.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      adopt_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .cli_only = 1,
      .help_arg = NULL, .help_order = 460,
      .help_text = "re-tag memory-card saves to the RUNNING modset (save safety: a card played\n"
          "under a different modset warns on load until explicitly adopted)" },

    /* -------- Gameplay entry -------- */
    { "skip_to_level", "Skip to level", "Gameplay", "--skip-to-level", NULL,
      "Boot straight into a level id, fast-forwarding logos/title. One-shot "
      "at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 99,
      skiplevel_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "L", .help_order = 540,
      .help_text = "load directly into level L: the game's numeric level id or its\n"
          "lower_snake_case name; no value or an unknown name prints this\n"
          "game's list. Windowed runs fast-forward the logos/title/loading\n"
          "(black window, audio muted) until the level is playable" },

    { "skip_to_cutscene", "Skip to cutscene", "Gameplay", "--skip-to-cutscene", NULL,
      "Play a named cutscene (Spyro 1: intro from the title; ending, "
      "ending_loot from Gnasty's levels; credits, credits_100 from a live "
      "level; Spyro 2: intro, intro_arrival, summer_forest, crush_intro, "
      "crush_defeated, autumn_plains, gulp_intro, gulp_defeated, "
      "winter_tundra, ripto_intro, ending from Dragon Shores, and "
      "LEVEL_intro / LEVEL_outro for the 17 levels with a level cutscene "
      "(idol_springs_intro, metropolis_outro, ...), from the homeworld "
      "portal / the completed level; Spyro 3: the "
      "19 story cutscenes by their loading captions, an_evil_plot_unfolds "
      "and its chain from the title, the rest from their trigger's level). "
      "One-shot at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 99,
      skipcutscene_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "C", .help_order = 550,
      .help_text = "play the named cutscene C. No value or an unknown name prints this\n"
          "game's list. Spyro 1: intro (the opening, from the title through\n"
          "New Game; not with --skip-to-level), ending (Gnasty defeated, from\n"
          "level 63), ending_loot (Gnasty's Loot, from level 64; the credits\n"
          "return to Artisans), credits / credits_100 (the short / full 100%\n"
          "roll, from --skip-to-level's target or Artisans; the roll returns\n"
          "to the level it started from). Spyro 2: intro, intro_arrival,\n"
          "summer_forest, crush_intro, crush_defeated, autumn_plains,\n"
          "gulp_intro, gulp_defeated, winter_tundra, ripto_intro, ending (the\n"
          "Dragon Shores theater's replay, from level 65; playback returns\n"
          "to Dragon Shores); LEVEL_intro (entered from the homeworld\n"
          "portal, first visit) and LEVEL_outro (the level completed and\n"
          "left) for LEVEL idol_springs, colossus, hurricos, aquaria_towers,\n"
          "sunny_beach, skelos_badlands, crystal_glacier, breeze_harbor,\n"
          "zephyr, scorch, shady_oasis, magma_cone, fracture_hills,\n"
          "mystic_marsh, cloud_temples, robotica_farms, metropolis. Spyro 3\n"
          "(the game's loading captions; each from its trigger's level, the\n"
          "chain continues as in the game):\n"
          "an_evil_plot_unfolds, a_powerful_villain_emerges,\n"
          "a_desperate_rescue_begins (from the title through New Game; not\n"
          "with --skip-to-level), bianca_strikes_back, the_second_warning,\n"
          "no_hard_feelings (10), spike_is_born, hunters_tussle,\n"
          "byrd_james_byrd (20), a_monster_to_end_all_monsters,\n"
          "an_apology_and_lunch, a_duplicitous_larcenous_ursine (30),\n"
          "the_escape (37), the_dancing_bear (40), deja_vu,\n"
          "a_familiar_face, billy_in_the_wall, one_less_noble_warrior (47),\n"
          "the_end (50)" },

    { "skip_to_area", "Skip to sub-area", "Gameplay", "--skip-to-area", NULL,
      "Boot into a level's sub-area, level:area (Spyro 3: the skate parks and "
      "the areas behind a level's portals), entered through the game's own "
      "sub-area transition once the level is live. One-shot at boot.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, skiparea_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "L:A", .help_order = 560,
      .help_text = "load level L (number or name) and enter its sub-area A through the\n"
          "game's own sub-area transition, as its area-0 portal does. A is\n"
          "an index from 1; no value or an invalid target prints each level's\n"
          "valid indices. Replaces --skip-to-level's level (Spyro 3)" },

    { "freecam", "Freecam", "Gameplay", NULL, "OPENPETE_FREECAM",
      "Debug free camera (same toggle as the F key / stick click). "
      "Intentionally divergent debug tool - the one non-presentation row.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      freecam_get, freecam_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off" },

    /* Both pose flags set their env spelling from the parser, so the env
     * getter shows either spelling. One-shot at boot. */
    { "freecam_pose", "Freecam pose", "Gameplay", "--freecam-pose", "OPENPETE_FREECAM_POSE",
      "Engage the debug free camera at this pose once the level is live "
      "(\"pos=(x, y, z) yaw=N pitch=N\"). Intentionally divergent debug tool.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, env_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the camera follows the player",
      .help_arg = "STR", .help_order = 580,
      .help_text = "engage the debug free camera at a pose once the level is\n"
          "live (same format as the freecam log: \"pos=(x, y, z) yaw=N\n"
          "pitch=N\"). CLI twin of OPENPETE_FREECAM_POSE - env vars don't\n"
          "cross the WSL->Windows interop boundary." },

    { "player_pose", "Player pose", "Gameplay", "--player-pose", "OPENPETE_PLAYER_POSE",
      "Place the player at this position and facing once gameplay is live "
      "and under pad control (\"x,y,z[,yaw]\"). Intentionally divergent "
      "gameplay state.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, env_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the player starts where the game places him",
      .help_arg = "STR", .help_order = 590,
      .help_text = "place the player once gameplay is live and under pad control:\n"
          "\"x,y,z[,yaw]\" or the exit pose line (\"pos=(x, y, z) yaw=N\",\n"
          "yaw 4096 per turn); the follow camera is re-seated behind him.\n"
          "CLI twin of OPENPETE_PLAYER_POSE. Every run prints its end\n"
          "state as openpete-pose: after openpete-exit:." },

    { "camlock", "Camera lock (render)", "Gameplay", NULL, "OPENPETE_CAMLOCK",
      "Lock/fly the presented view while the sim keeps running (G key). "
      "Render-only: gameplay state is untouched, so cutscenes play on "
      "underneath - the close-up inspection tool.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      camlock_get, camlock_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off" },

    /* A movie drives the pad, so it belongs with what shapes the session
     * rather than with the video-capture rows it used to sit among. */
    { "bk2", "bk2 replay", "Gameplay", "--bk2", NULL,
      "Replay a BizHawk movie into the pad (loaded at loop start).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the pad is live",
      .help_arg = "PATH", .help_order = 600,
      .help_text = "replay BizHawk movie (Octoshock P1 pad: buttons + analog sticks)" },

    { "bk2_frame_offset", "bk2 frame offset", "Gameplay", "--bk2-frame-offset", NULL,
      "Map bk2 frame 0 to this openpete_frame (VBL) of the replay. Read once "
      "when the movie loads.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      bk2offset_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "0: bk2 frame 0 plays at openpete_frame 0",
      .help_arg = "N", .help_order = 610,
      .help_text = "map bk2 frame 0 to openpete_frame N (default 0)" },

    { "bk2_stop_on_end", "Exit at bk2 end", "Gameplay", "--bk2-stop-on-end", NULL,
      "Exit after the last bk2 frame instead of holding its input.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      bk2stop_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 620,
      .help_text = "exit after last bk2 frame instead of holding last input" },

    { "bk2_record", "bk2 recording", "Gameplay", "--bk2-record", NULL,
      "BizHawk movie the live pad input is written to, with a .args sidecar "
      "of the typed launch flags. Read once at startup.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none; the flag without a path records to "
                      "<bk2 dir>/<timestamp>.bk2",
      .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[PATH]", .help_order = 630,
      .help_text = "write live pad input to a BizHawk .bk2 (combinable with --bk2 to extend a movie).\n"
          "With no PATH, records to <exe_dir>/bk2/<timestamp>.bk2. Also writes a\n"
          "<PATH>.args sidecar of the launch flags (minus --bk2*) for replay via --bk2." },

    /* The launch selection; the game module contributes one button row per
     * cheat through openpete_knob_add, in the Cheats section. */
    { "cheats", "Cheats at launch", "Gameplay", "--cheats", NULL,
      "Built-in cheat codes this run starts each session with. The rows below "
      "fire the same cheats on demand; this one only shows the launch "
      "selection. Intentionally divergent gameplay state.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, cheats_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none",
      .help_arg = "A,B,...", .help_order = 570,
      .help_text = "apply the game's own built-in cheat codes without entering them on\n"
          "the pad: 99_lives, unlock_all_levels (an unknown name prints the\n"
          "list). Applied once a level is live, and again for each new session\n"
          "started from the title screen. Changes gameplay state by design" },

    /* -------- Recording --------
     * Every row here is hidden unless this run is recording; see
     * recording_visible. */
    { "record", "Record video", "Recording", "--record", NULL,
      "True-VFR gameplay capture to a video file (latched at boot).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "none: no video is recorded",
      .help_arg = "PATH", .help_order = 10,
      .help_text = "capture true-VFR gameplay video (.mkv recommended; .mp4 ok); reads the present FBO (no letterbox bars)" },

    { "record_cfr", "Constant-rate capture", "Recording", "--record-cfr", NULL,
      "Capture at a fixed frame rate (an integer multiple of the 29.913 Hz "
      "tick rate) on an offline clock, instead of true VFR. 0 = VFR. "
      "Latched at boot.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0.0f, 480.0f,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "none: true variable-rate capture",
      .help_arg = "RATE", .help_order = 90,
      .help_text = "capture at a constant frame rate instead of true VFR. RATE must be an\n"
          "integer multiple of the NTSC tick rate: 29.913, 59.826, 119.652, ...\n"
          "Drives the host clock from the pacer, so the same span of GAME time is\n"
          "captured however slowly the machine renders. Implies --no-record-audio." },

    { "record_frames", "Capture frame budget", "Recording", "--record-frames", NULL,
      "Finish the recording and exit after this many encoded frames. "
      "0 = until the run ends. Latched at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 1000000,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "0",
      .help_arg = "N", .help_order = 100,
      .help_text = "finish the recording and exit after exactly N encoded frames (with\n"
          "--record-cfr, a window of exactly N/RATE seconds of game time)" },

    { "record_fps_overlay", "FPS overlay in recording", "Recording",
      "--record-fps-overlay", "OPENPETE_FPS_IN_RECORD",
      "Burn the FPS overlay into --record video output. Live: re-resolved "
      "on the next recorded frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      get_stored, recfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "env OPENPETE_FPS_IN_RECORD, else 0",
      .help_arg = "0|1", .help_order = 80,
      .help_text = "burn the native FPS overlay into --record video (default 0 = clean\n"
          "capture; on|off also accepted). Display-only." },

    { "record_encoder", "Video encoder", "Recording", "--record-encoder", NULL,
      "libavcodec video encoder of the recording. Latched at boot.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "libx264",
      .help_arg = "NAME", .help_order = 20,
      .help_text = "libavcodec encoder (default {def}; e.g. h264_nvenc, libopenh264)" },

    { "record_crf", "Encoder quality", "Recording", "--record-crf", NULL,
      "Constant-rate-factor quality of libx264 and libx265 (lower is better). "
      "Latched at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 51,
      reccrf_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "23",
      .help_arg = "N", .help_order = 30,
      .help_text = "encoder quality for libx264/libx265 (default {def}; lower = better)" },

    { "record_gop", "Keyframe interval", "Recording", "--record-gop", NULL,
      "Encoded frames between keyframes. Latched at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 1000,
      recgop_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "120",
      .help_arg = "N", .help_order = 40,
      .help_text = "keyframe interval in frames (default {def})" },

    { "record_pixfmt", "Pixel format", "Recording", "--record-pixfmt", NULL,
      "Encoder pixel format. Latched at boot.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "yuv420p",
      .help_arg = "NAME", .help_order = 50,
      .help_text = "encoder pixel format (default {def})" },

    { "no_record_audio", "No audio track", "Recording", "--no-record-audio", NULL,
      "Record video only, without the mixed SPU audio. Latched at boot.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      norecaudio_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 60,
      .help_text = "record video only (default: also capture mixed SPU audio)" },

    { "record_audio_encoder", "Audio encoder", "Recording", "--record-audio-encoder", NULL,
      "libavcodec audio encoder of the recording. Latched at boot.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "aac",
      .help_arg = "NAME", .help_order = 70,
      .help_text = "libavcodec audio encoder (default {def})" },

};

#ifndef OPENPETE_HIDE_DEBUG_FLAGS
/* Rows compiled only into builds that carry the debug flags. rows_init marks
 * each copy debug_block, which is how the surface dump tells them apart. */
static const knob_desc_t g_builtin_debug[] = {
    /* -------- Debug (toggles / tunables) -------- */
    { "render_paths", "Render paths", "Debug", "--render-paths", NULL,
      "Render only the named paths in isolation (cyclorama,spyro,moby,"
      "terrain,shadow,flame,particles,tracer,transition,portal,glow,sparkle,"
      "moby_shadow,ui); none renders no path. Empty or \"(all)\" clears the "
      "selection. Live once the game module binds its setter.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, renderpaths_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "LIST", .help_order = 100,
      .help_text = "render ONLY these paths in both renderers, e.g.\n"
          "cyclorama,spyro,moby,terrain,shadow,flame,particles,glow,sparkle,\n"
          "tracer,transition,portal,moby_shadow,ui. Lets a path be\n"
          "compared in isolation. Presentation-only; gameplay state unaffected." },

    { "log_level", "Log level", "Debug", "--log-level", "OPENPETE_LOG",
      "Per-subsystem stderr log levels, e.g. cd:debug,*:info. Applying "
      "REPLACES all boot rules with the typed spec (empty = *:info).",
      KNOB_STRING, KNOB_LIVE, NULL, 0, 0, 0,
      NULL, NULL, loglevel_get_str, loglevel_set_str, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "SPEC", .help_order = 140,
      .help_text = "e.g. cd:debug,*:info; env OPENPETE_LOG=..." },

    { "psycross_native_raster", "PsyCross native raster", "Debug",
      "--psycross-native-raster", NULL,
      "1 renders the PsyCross reference on the console's own 512x240 display "
      "grid instead of a square-pixel 4:3 grid sized from the window. "
      "Reference renderer only. Read once at startup.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      pcnative_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 110,
      .help_text = "1 = render the PsyCross reference on the console's own 512x240\n"
          "display grid (one texel per PSX pixel, authentic aliasing) instead\n"
          "of a square-pixel 4:3 grid sized from the window. The window blit\n"
          "scales it up. Default {def}. Reference renderer only." },

    ENVROW_LATCHED("RUN_RECOMP", "Debug", "",
        "Guest address (hex) the game fiber dispatches instead of the EXE "
        "header's entry point. Read once at boot."),
    ENVROW_LATCHED_NOTE("SURFACE_DUMP", "Debug", "none: the game runs",
        "Path the binary writes its flags, registry rows, menu rows, key "
        "actions and vocabularies to as JSON, with their declared defaults "
        "and the defaults selfcheck, before exiting; read at the start of "
        "main, before option parsing."),

    { "log_format", "Log format", "Debug", "--log-format", NULL,
      "stderr log shape: plain text vs one JSON object per line. Takes "
      "effect on the next line.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_logfmt_names), 0, 0,
      logfmt_get, logfmt_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .device_creation = 2,
      .help_arg = "json|plain", .help_order = 130,
      .help_text = "structured stderr (default plain)" },

    ENVROW_LATCHED("PSXTRACE_CLOCK_REPLAY", "Debug", "0",
        "Oracle clock replay: with --psxtrace-verify and the golden's .vblpos "
        "and .spuq sidecars, each of the reference's in-work VBLANKs is "
        "delivered at the function entry it recorded and the SPU envelope is "
        "charged to the reference's exact clock at every wait, poll, query "
        "and libspu call. Read once when the golden opens; "
        "tests/harness/check_psx_oracle.sh sets it under OPENPETE_ORACLE_CLOCK=1."),
    ENVROW_LATCHED("CRASH_TEST", "Debug", "",
        "Fault injection for the crash handler: segv (a null store) or "
        "overflow (unbounded recursion) on the game fiber before the game's "
        "entry runs. Read once at boot."),
    ENVROW_LIVE("WRITE_FAIL", "Debug", "",
        "Fault injection for persisted files: <substring>[:<bytes>] makes "
        "every atomic save (memory card, save state, config, mod config, "
        "shader cache) whose path contains <substring> fail after <bytes> "
        "bytes, with the previous file kept. Read at each save."),
    ENVROW_DEVICE("RHI_DEPTH_D24", "Debug", "0",
        "1 = take the D24_UNORM_S8 depth format even where D32_SFLOAT_S8 is "
        "available, running the fallback's depth convention. Read at device "
        "creation."),
    ENVROW_LATCHED_NOTE("PRECOMPILE_FORCE", "Debug",
        "unset: runs in a windowed run on a hardware device",
        "1 runs the pipeline precompile at bring-up in any run mode (headless, "
        "deterministic capture, a software device); 0 skips it in any mode. "
        "Unset, it runs in a windowed run on a hardware device and is skipped "
        "in headless and deterministic runs and on a software device. Read "
        "once at bring-up."),
    ENVROW_LATCHED("EAGER", "Debug", "1",
      "Eager production (default on): the span of presents is extracted "
      "AHEAD of each tick and presented from a dedicated thread's scene "
      "ring, so the tick's execution leaves no hole in the display. 0 = "
      "present from the sim thread (the first A/B for any pacing "
      "regression). Windowed native-vk only; a --screenshot-on-frame "
      "target disengages it unless OPENPETE_EAGER_SHOT=1. Latched at "
      "startup."),
    ENVROW_LATCHED("THREAD_SPLIT", "Debug", "1",
      "Run the sim on a worker thread while the main thread owns the window "
      "and the event pump, so dragging or resizing the window cannot stall "
      "the game. On by default; set 0 to put the loop back on the main "
      "thread. Windowed native-vk only. Latched at startup."),
    ENVROW_LATCHED("GLINT", "Debug", "2",
      "What the gem-glint feeder emits: 0 nothing, 1 the near four-point "
      "stars alone, 2 the near stars and the far crosses. The guest draw "
      "runs at every setting, so 0 and 1 price the feed's painted pixels "
      "against 2 from one binary. Latched at first use."),
    ENVROW_DEVICE("VK_VALIDATION", "Debug", "0",
        "1 = enable the Khronos Vulkan validation layer and log its warnings "
        "and errors as 'validation:' lines. Needs the Vulkan SDK (the layer "
        "is not part of a driver install); costs CPU time on every Vulkan "
        "call. Read at instance creation."),
    ENVROW_LATCHED("VK_FAULT", "Debug", "",
        "submit:N = the N-th fenced Vulkan queue submit (a frame or present "
        "submit) returns device-lost without submitting, which drives the "
        "device-lost shutdown on a working device. Read at the first "
        "submit."),
    ENVROW_DEVICE("GPU_TIMERS", "Debug", "0",
        "Per-pass GPU timestamp zones (query pools created at device init)."),
    ENVROW_LIVE("PHASE_TIMERS", "Debug", "0",
        "Once-per-second per-phase CPU us report (re-resolved per VBL)."),
    ENVROW_LIVE("MOD_TIMERS", "Debug", "0",
        "Per-mod tick/present ms report (re-read per report window)."),
    ENVROW_LIVE("GPU_TIMERS_VERBOSE", "Debug", "0",
        "GPU-timer stderr line includes nested sub-zones, not just depth-0 "
        "(re-read per report; needs GPU_TIMERS)."),
    ENVROW_LIVE("BACKDROP_DEFER", "Debug", "1",
        "Defer cyclorama/backdrop fills until after WORLD-opaque so early-Z "
        "rejects covered pixels (default 1; 0 = PSX draw-first order)."),
    ENVROW_LATCHED("MATERIAL_STATS", "Debug", "0",
        "Print new peaks of interned material slots."),
    ENVROW_LATCHED("TEXPACK_STATS", "Debug", "0",
        "HD-texpack sub-run disposition counters."),
    ENVROW_LIVE("MOBY_OCCLUSION", "Debug", "1",
        "Moby occlusion culling on/off (default 1; re-resolved per VBL)."),
    ENVROW_LATCHED("MOBY_OCCL_SLACK", "Debug", "1500",
        "Occlusion-grid depth slack before a rect counts hidden (default 1500)."),
    ENVROW_LATCHED("OCCL_CAM_MOVE", "Debug", "24",
        "Occlusion-grid camera bound: world units the querying camera may differ "
        "from the grid's before the depth verdict is refused (default 24)."),
    ENVROW_LATCHED("OCCL_CAM_TURN", "Debug", "0.35",
        "Occlusion-grid camera bound: degrees the querying camera may be rotated "
        "from the grid's before the depth verdict is refused (default 0.35)."),
    ENVROW_LATCHED("MOBY_RANK", "Debug", "1",
        "Same-moby painter select: between faces of one moby that intersect in 3D, "
        "the game's own paint order (its OT key, evaluated natively) overrides "
        "per-pixel depth (default 1; 0 = true depth only)."),
    ENVROW_LATCHED("MOBY_SHADOW_DECAL", "Debug", "1",
        "Depth-aware moby-shadow decal vs legacy subtract pipe (default 1)."),
    ENVROW_LATCHED("SHADOW_PLAYER", "Debug", "1",
        "Player ground-shadow leg feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("SHADOW_INTERP", "Debug", "1",
        "Moby drop-shadow own-motion offsets under an interpolated camera (default 1); "
        "0 leaves every shadow at the newest tick's world pose."),
    ENVROW_LATCHED("PORTAL3_INTERP", "Debug", "1",
        "Spyro 2 and Spyro 3 portal-preview sub-tick interpolation (default 1); "
        "0 replays every preview at the state its tick captured."),
    ENVROW_LATCHED("PARTICLES", "Debug", "1",
        "Particle feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("TRACERS", "Debug", "1",
        "Tracer ribbon and mesh feed enable (default 1); the guest draws run either way."),
    ENVROW_LATCHED("TRACERS_INTERP", "Debug", "1",
        "Spyro 2 and Spyro 3 tracer sub-tick rebase (default 1); "
        "0 replays every ribbon and mesh at the screen positions its tick drew."),
    ENVROW_LATCHED("GLOW", "Debug", "1",
        "Glow-ring feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("ZAP", "Debug", "1",
        "Zoe's zap feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("SHADOW_BAND", "Debug", "256",
        "Fallback view-Z band for shadow decal mode 0 (default 256)."),
    ENVROW_LATCHED("SHADOW_HW_BIAS", "Debug", "-64,-2,-0.002",
        "Hardware depth bias c,s,clamp on subtractive shadow pipes."),
    ENVROW_LIVE("SHADOW_DECAL_MODE", "Debug", "",
        "Force shadow-decal shader mode (calibration override)."),
    ENVROW_LATCHED("PORTAL_MASK_BIAS", "Debug", "0,0,0",
        "Portal-mask camera-ward depth pull (3 floats)."),
    ENVROW_LATCHED("HUD6_ZPIPE", "Debug", "0",
        "A/B: HUD glyph bucket via opaque depth-tested pipe."),
    ENVROW_LATCHED("HUD6_NOSORT", "Debug", "0",
        "A/B: keep bucket-6 submission order (skip z sort)."),
    ENVROW_LATCHED("NATIVE_SCENE_DELAY", "Debug", "0",
        "Compose from a past completed frame instead of the current one."),
    ENVROW_LATCHED("TAG_SPLIT", "Debug", "0",
        "Force batch split by mod scene-tag."),
    ENVROW_LATCHED("TAG_TINT", "Debug", "0",
        "Visual tint by scene tag."),
    ENVROW_LIVE("SHADOW_WALL_HEIGHT", "Debug", "400",
        "Decal wall-occlusion height threshold (default 400)."),
    ENVROW_LIVE("SHADOW_STEEP_NY", "Debug", "0.5",
        "Up-normal below which a surface counts as wall (default 0.5)."),
    ENVROW_LIVE("SHADOW_PLAT_EPS", "Debug", "150",
        "Raised-occluder clause epsilon (default 150; 1e5 disables)."),
    ENVROW_LIVE("SHADOW_PLAT_SLACK", "Debug", "48",
        "Platform-clause slack (default 48)."),
    ENVROW_LIVE("SHADOW_FOOTPRINT", "Debug", "1",
        "Player-shadow footprint term: the terrain under the ring below the caster "
        "(default 1); 0 leaves the fan's own surface under the depth rule."),
    ENVROW_LATCHED("SHADOW_BIAS_SPYRO", "Debug", "1.0",
        "Spyro-shadow depth-bias multiplier."),
    ENVROW_LATCHED("SHADOW_BIAS_MOBY", "Debug", "1.0",
        "Moby-shadow depth-bias multiplier."),
    ENVROW_LATCHED("PORTAL_SKY_EPS", "Debug", "24",
        "Portal-sky forward depth margin (default 24; two independent latches)."),
    ENVROW_LATCHED("PORTAL_SKY_DEPTH", "Debug", "plane",
        "Portal sky depth model: ot|plane|ring."),
    ENVROW_LIVE("PORTAL_SKY_OTPUSH", "Debug", "1",
        "Far-portal sky rides the .s +64-slot OT bump as an 8192-world depth push (0 = un-pushed plane)."),
    ENVROW_LATCHED("NOLOD_PORTAL_DOME", "Debug", "1",
        "Kill switch for extended-LOD always-render dome (default 1)."),
    ENVROW_LATCHED("PORTAL_GUARD", "Debug", "0",
        "Experiment: re-feed .s near-band tris as portal guard."),
    ENVROW_LATCHED("PORTAL_APERTURE", "Debug", "1",
        "Stencil the portal sky to the opening polygon (0 = old sky-footprint mask)."),
    ENVROW_LATCHED("PORTAL_APERTURE_MARGIN", "Debug", "40",
        "Outward margin on the portal aperture, in WORLD units (default 40; negative erodes)."),
    ENVROW_LATCHED("PORTAL_RIM_CULL", "Debug", "1",
        "Cull terrain lying in a portal's opening plane (the inner rim). 0 = keep."),
    ENVROW_LATCHED("PORTAL_TRANSP_COVER", "Debug", "1",
        "Drop blended terrain (waterfalls, both tiers) the portal preview covers "
        "(0 = let it blend through the preview)."),
    ENVROW_LATCHED("PORTAL_PLANE_MARGIN", "Debug", "128",
        "Plane-cover near margin M (world units): inside an opening the preview covers "
        "everything at or behind plane - M. Portal-name text floats ~192-256 in front; "
        "frame erosion begins near 512."),
    ENVROW_LATCHED("PORTAL_RIM_NEAR", "Debug", "128",
        "Rim predicate: how far the NEAREST vertex may sit from the opening plane, in "
        "world units - the rim must touch the plane, an arch's front wall must not."),
    ENVROW_LATCHED("PORTAL_RIM_EPS", "Debug", "384",
        "Rim predicate: how far the FARTHEST vertex may stray from the opening plane, "
        "in world units."),
    ENVROW_LATCHED("PORTAL_RIM_REACH", "Debug", "128",
        "Rim predicate: how far OUTSIDE the opening polygon a face may reach and still "
        "count as rim, measured in the plane, in world units."),
    ENVROW_LATCHED("PORTAL_RIM_ANGLE", "Debug", "30",
        "Rim predicate: max angle between a rim face and the opening plane, in degrees."),
    ENVROW_LATCHED("PORTAL_RIM_BROAD", "Debug", "1",
        "Portal-rim sector broadphase: skip rim classification for sectors provably "
        "clear of every opening (1 on, 0 off, 2 = classify live + audit the broadphase)."),
    ENVROW_LATCHED("PORTAL_NUDGE", "Debug", "",
        "Per-portal opening correction \"id:dh,dv[,sh[,sv]];...\" in world units."),
    ENVROW_LIVE("TERRAIN_CULL", "Debug", "3",
        "bit0 HP NCLIP cull, bit1 LP cull (default 3 = both; each mirrors its walk's exact rule)."),
    ENVROW_LIVE("TERRAIN_FLAT", "Debug", "0",
        "Draw terrain untextured."),
    ENVROW_LATCHED("TERRAIN_HQSCALE", "Debug", "4.0",
        "Near/HQ classification scale (default 4.0)."),
    ENVROW_LATCHED("TERRAIN_NEARZ", "Debug", "1.0",
        "Near clip plane in world/16 view-Z (default 1.0)."),
    ENVROW_LATCHED("TERRAIN_NEARHQ", "Debug", "1",
        "Near/HQ subdivide pass enable (default 1)."),
    ENVROW_LIVE("TERRAIN_TEXTBL", "Debug", "0",
        "0 auto; 1 force LQ; 2 force HQ tiledefs."),
    ENVROW_LATCHED("TERRAIN_HIDETAIL", "Debug", "-1",
        "Force hi-detail flag into sector decode cache."),
    ENVROW_LIVE("TERRAIN_OCCL", "Debug", "1",
        "Sector occlusion cull via occlusion grid (default 1)."),
    ENVROW_LIVE("TERRAIN_HOLEFILL", "Debug", "1",
        "Stencil-gated LP hole fill at LOD boundary (default 1)."),
    ENVROW_LATCHED("TERRAIN_LPKEEP", "Debug", "0",
        "Keep ALL near-LP faces (probe)."),
    ENVROW_LATCHED("TERRAIN_FILL_FOGSLACK", "Debug", "0",
        "ir0 slack admitting not-fully-fogged faces to fill."),
    ENVROW_LATCHED("TERRAIN_FILL_THINPX", "Debug", "16",
        "Screen-thin bbox cap for slack-admitted fill faces (default 16)."),
    ENVROW_LATCHED("TERRAIN_FILL_DILATE", "Debug", "0",
        "Fill-tri screen dilation px."),
    ENVROW_LATCHED("TERRAIN_FILL_UP", "Debug", "0.7",
        "Fill upward growth px (default 0.7)."),
    ENVROW_LATCHED("TERRAIN_FILL_QUANT", "Debug", "1",
        "Floor-quantize fill verts (default 1)."),
    ENVROW_LIVE("TERRAIN_LP_ADMIT", "Debug", "1",
        "Authored per-face LP admission (.s exact; 0 = legacy)."),
    ENVROW_LIVE("TERRAIN_LP_FARCULL", "Debug", "1",
        "LP far cull at m_CullingDistance (.s exact; original LOD only)."),
    ENVROW_LIVE("TERRAIN_HP_DROP", "Debug", "1",
        "HP LOD drop at m_LodDistance (.s exact; 0 = fog-corner legacy)."),
    ENVROW_LIVE("TERRAIN_OT_BIAS", "Debug", "1",
        "Authored .s OT-slot depth offsets, both tiers (0 = raw depth)."),
    ENVROW_LIVE("TERRAIN_GAPFILL", "Debug", "1",
        "Frozen fill-tag LP faces (default 1)."),
    ENVROW_LATCHED("TERRAIN_ARTFIX", "Debug", "1",
        "Recolour known unpainted level-data faces (0 = authentic)."),
    ENVROW_LATCHED("TERRAIN_LOD_FAITHFUL", "Debug", "1",
        "Faithful .s sector binning (original LOD; 0 = admission-only)."),
    ENVROW_LATCHED("TERRAIN_FOGSCALE", "Debug", "1.0",
        "Debug force-override of the fog curve (wins over --fog-scale)."),
    ENVROW_LATCHED("TERRAIN_FOGDPCS", "Debug", "1",
        "Exact per-corner DPCS vertex fog decode (default 1)."),
    ENVROW_LATCHED("TERRAIN_CLUTFOG", "Debug", "1",
        "CLUT-row texel fog ladder (default 1)."),
    ENVROW_LIVE("TERRAIN_LOD_HANDOFF", "Debug", "1",
        "Drop HP beyond fog saturation, substitute LP (default 1)."),
    ENVROW_LATCHED("TERRAIN_PORTAL_CONFORM", "Debug", "1",
        "Terrain-vs-portal-sky depth conformance clamp (default 1)."),
    ENVROW_LATCHED("TERRAIN_NEARVAR", "Debug", "1",
        "Retain near-plane sectors on the GPU clip-fan path (0 = CPU walk)."),
    ENVROW_LIVE("TERRAIN_VANIM", "Debug", "1",
        "Sub-tick lerp of moving-terrain verts (default 1)."),
    ENVROW_LIVE("TERRAIN_TANIM", "Debug", "1",
        "Sub-tick lerp of scrolling terrain textures (default 1)."),
    ENVROW_LIVE("TERRAIN_CANIM", "Debug", "1",
        "Sub-tick lerp of animated terrain vertex colours (0 = hold the previous tick's)."),
    ENVROW_LATCHED("TERRAIN_ALLVIS", "Debug", "0",
        "Walk ALL sectors, bypass visibility groups (probe)."),
    ENVROW_LATCHED("TERRAIN_NEARQ", "Debug", "0",
        "Read the .s deferred near queue as debug oracle route."),
    ENVROW_LIVE("TERRAIN_NOTEX", "Debug", "0",
        "Feed terrain untextured."),
    ENVROW_LIVE("TERRAIN_TIERCOLOR", "Debug", "0",
        "Colour terrain by LOD tier."),
    ENVROW_LIVE("TERRAIN_TIERONLY", "Debug", "0",
        "Draw only LP (1) or only HP (2) tier."),

    { "devui_open", "Open this overlay at boot", "Debug", "--devui-open", "OPENPETE_DEVUI_OPEN",
      "Open this panel as soon as it initializes instead of waiting for the "
      "M key, expanding the Mods list and every mod node once so an "
      "unattended windowed run reaches a mod UI section body. Read once by "
      "the overlay's init, so the row shows the boot value; M toggles the "
      "panel live.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "0|1", .help_order = 120,
      .help_text = "open the dev config overlay (M panel) as soon as it\n"
          "initializes instead of waiting for the M key, with the Mods\n"
          "list and every mod node expanded once, so an unattended run\n"
          "reaches a mod UI section body (on|off also accepted).\n"
          "Windowed native-vk runs only; inert elsewhere." },

    { "devui_inject_mouse", "Park the overlay mouse", "Debug",
      NULL, "OPENPETE_DEVUI_INJECT_MOUSE",
      "Post a fixed cursor position (window pixels, \"X,Y\") into this "
      "overlay's input every present, so an unattended run can hover a "
      "widget and read the result out of the log instead of needing a hand "
      "on the mouse. Parsed once by the overlay's init, so the row shows "
      "the boot value.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, injectmouse_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    ENVROW_LATCHED("MOUSE_INJECT", "Debug", "",
        "Path of a mouse-event script (\"<pump-iteration> move x y | down b | "
        "up b | click2 b | rel dx dy | wheel n | menu 0|1 | capture 0|1 | shot | "
        "keydown k | keyup k\") "
        "pushed through the event pump as SDL events, so an unattended run "
        "drives every mouse consumer. Read once at boot."),
    ENVROW_LATCHED("MOUSE_PROBE", "Debug", "0",
        "Log the mouse channel on change and every consumer gesture with its "
        "outcome. Read once at boot."),
    ENVROW_DEVICE("WINDOW_STATE", "Debug", "0",
        "Log the window's flags and input focus at bring-up and at exit for a "
        "window created without --background, which prints them anyway. The "
        "comparison of the two is what proves the flag changed the state. "
        "Read once per line."),

    { "fillpick", "Fill-tag inspector", "Debug", NULL, "OPENPETE_FILLPICK",
      "Pick the terrain face under the crosshair and re-judge its LP fill "
      "tag live (P key). Display-only audit tool for the frozen list in "
      "tests/freecam/fill-tags.verdicts; verdicts land in a session file.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_get, fillpick_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fillpick_preview", "Fill-tag preview", "Debug", NULL, "OPENPETE_FILLPICK_PREVIEW",
      "With the inspector engaged, draw every LP face - not just the tagged "
      "ones - tinting the untagged candidates magenta (H key). This is how a "
      "face that should be filling a hole is found before it is tagged.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_prev_get, fillpick_prev_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    ENVROW_LATCHED("GLITCH_MARK_N", "Debug", "600",
        "Depth of the glitch-mark rings, in presents and in terrain walks "
        "(600 is about 1.25 s of presents at 480 Hz). Read once at boot, "
        "when the rings are allocated."),
    ENVROW_LATCHED("GLITCH_MARK_AT", "Debug", "",
        "Raise the glitch mark automatically instead of on the glitch_mark "
        "binding: \"<n>\" or \"p<n>\" at present serial n, \"w<n>\" at "
        "terrain-walk ordinal n (the only spelling a headless run reaches, "
        "since headless composes no presents), \"<n>ms\" n milliseconds "
        "after boot. One shot. Read once at boot."),
    ENVROW_LATCHED("ACTOR_OWN_KEY", "Debug", "1",
        "Actor faces contest only other mobys' keys (0 = own keys too)."),
    ENVROW_LATCHED("MOBY_OTBIAS", "Debug", "1",
        "Master switch for the three ordering-bias mechanisms mirrored from "
        "the game's OT depth pull: 0 turns off MOBY_TIE_PULL, MOBY_FACE_KEY "
        "and PLAYER_SORT_PULL together."),
    ENVROW_LATCHED("MOBY_TIE_PULL", "Debug", "1",
        "Moby walk: feed each vertex 127 world units nearer than its true "
        "view-Z when the record centre is in front of the camera, so a "
        "same-slot moby-versus-terrain tie resolves to the moby (0 = off)."),
    ENVROW_LATCHED("MOBY_FACE_KEY", "Debug", "1",
        "Moby walk: write each opaque actor face's ordering key (mean "
        "perspective W less 128 x the authored depth offset) into the actor "
        "key image for the key contest (0 = off)."),
    ENVROW_LATCHED("PLAYER_SORT_PULL", "Debug", "1",
        "Spyro walk: apply the sortDepth pulls - the front sentinel's "
        "whole-model near pull, otherwise the 127-unit tie pull and the "
        "mirrored copy's negative depth-key bias (0 = off)."),
    ENVROW_LIVE("MOBY_TRUESCOPE", "Debug", "1",
        "Scope moby og-key yields by true depth (0 = og compound painter)."),
    ENVROW_LATCHED("SHADED_OTBIAS", "Debug", "1",
        "Same pull for the SHADED walk."),
    ENVROW_LATCHED("TRUE_W", "Debug", "7",
        "Perspective W from the true view-Z, not the OT-pulled depth "
        "(bitmask 1=player 2=moby 4=world; 0 = legacy derive-from-depth)."),
    ENVROW_LATCHED("MOBY_SNAP_STALE", "Debug", "1",
        "Snap instead of lerp across stale interp pairs (default 1)."),
    ENVROW_LATCHED("MOBY_JUMP_HOLD", "Debug", "1",
        "Hold a replayed moby at its previous tick across a one-tick position "
        "jump (Spyro 2/3; 0 interpolates across it)."),
    ENVROW_LATCHED("MOBY_MIRROR_HOLD", "Debug", "64",
        "Sub-tick hold threshold for a moby's one-tick turn, in byte angles "
        "(256 = one turn); 0 interpolates every interval."),
    ENVROW_LATCHED("MOBY_IDLE", "Debug", "0",
        "Display-only idle animation for frozen mobys."),
    ENVROW_LATCHED("MOBY_HOLD_PX", "Debug", "10",
        "Max screen motion for tier-2 moby hold replay (default 10)."),
    ENVROW_LATCHED("SHADED_NOHL", "Debug", "0",
        "Disable shaded-walk highlight."),
    ENVROW_LATCHED("ACTOR_FRONT", "Debug", "1",
        "Dragon-rescue actor front pass (default 1)."),
    ENVROW_LATCHED("ENV_C2", "Debug", "14.5",
        "Metal env-reflection calibration constant (default 14.5)."),
    ENVROW_LATCHED("ENV_AZSIGN", "Debug", "1.0",
        "Metal env-reflection azimuth sign."),
    ENVROW_LATCHED("SPYRO_SCRATCH_MAT", "Debug", "0",
        "Force old scratch-block matrix read."),
    ENVROW_LATCHED("SPYRO_UNTEX", "Debug", "0",
        "Draw Spyro feed untextured (bisect)."),
    ENVROW_LATCHED("GEM_FLYOUT_KEY", "Debug", "1",
        "Rotation-fingerprint keying of HUD gem-flyout interp (default 1)."),
    ENVROW_LATCHED("RA_REBASE", "Debug", "1",
        "Runahead camera rebase (default 1)."),
    ENVROW_LATCHED("RA_CAM_COMMIT", "Debug", "1",
        "Committed-timeline camera regions under runahead (default 1)."),
    ENVROW_LATCHED("INTERP_LINEAR", "Debug", "0",
        "Force linear lerp instead of pred1 Hermite."),
    ENVROW_LATCHED("INTERP_PERIOD_UNCAP", "Debug", "0",
        "Diagnostic: uncap interp period."),
    ENVROW_LATCHED("INTERP_PHASE_MS", "Debug", "0",
        "Capture-stamp phase bias vs present grid (0-16)."),
    ENVROW_LATCHED("BRACKET_CHECK", "Debug", "0",
        "Hash gameplay RAM around sub-tick brackets, log leaks."),
    ENVROW_LATCHED("FREECAM_SPIN", "Debug", "0",
        "Scripted continuous yaw sweep (units/tick)."),
    ENVROW_LATCHED("FREECAM_DRIFT", "Debug", "",
        "Scripted per-tick camera drift dx,dy,dz."),
    ENVROW_LATCHED("CAMLOCK_POSE", "Debug", "",
        "Engage the render camera lock at x,y,z,yaw,pitch (a camlock: "
        "log line pastes verbatim)."),
    ENVROW_LATCHED("CAMLOCK_FRAME", "Debug", "",
        "Engage the render camera lock (latch the live view) at VBL N."),
    ENVROW_LATCHED("OT_TRUNCATE", "Debug", "",
        "Submit only first N OT slots (paint-order bisection)."),
    ENVROW_LATCHED("TRANSITION_PROBE", "Debug", "0",
        "One line per (func,frame) during level transitions."),

    /* -------- Debug diagnostics (stderr censuses; flip on, watch, flip off) -------- */
    ENVROW_LATCHED("REEXTRACT_DBG", "Debug diagnostics", "0",
        "Per-present alpha/bracket summary of the re-extract spine."),
    ENVROW_LATCHED("REEXTRACT_FEEDDBG", "Debug diagnostics", "0",
        "Per-frame feeder freshness/counts (spyro + moby)."),
    ENVROW_LATCHED("CANON_CAPDBG", "Debug diagnostics", "0",
        "1/200-frame canonical-capture row prints."),
    ENVROW_LATCHED("CAP_ROLL_DBG", "Debug diagnostics", "0",
        "One line per interp ring roll."),
    ENVROW_LATCHED("ALPHA_MAP_DBG", "Debug diagnostics", "0",
        "Per-present present-time to interp-time mapping."),
    ENVROW_LATCHED("PART_INTERP_DBG", "Debug diagnostics", "0",
        "1/s particle pool-interp verdict census."),
    ENVROW_LATCHED("ANIMDBG", "Debug diagnostics", "0",
        "Spyro anim segment/blend + sub-tick vertex probe."),
    ENVROW_LATCHED("SPYRO_RECDBG", "Debug diagnostics", "0",
        "Per-epoch player-hook record census (calls per feeder, ring roll, hook-vs-ring bytes, pairing bound)."),
    ENVROW_LATCHED("MOBY_TIER_DBG", "Debug diagnostics", "0",
        "Extraction-tier census (proj/walk us, cache hits)."),
    ENVROW_LATCHED("MOBY_OCCL_DBG", "Debug diagnostics", "0",
        "1/s occlusion cull/seen breakdown."),
    ENVROW_LATCHED("MOBY_ANIMSW_DBG", "Debug diagnostics", "0",
        "Animation-switch cross/snap counters."),
    ENVROW_LATCHED("MOBY_GPF_DBG", "Debug diagnostics", "0",
        "One line per GPF re-extract."),
    ENVROW_LATCHED("MOBY_CLASSDBG", "Debug diagnostics", "0",
        "One line per drawn moby: class + vaddr + world position."),
    ENVROW_LATCHED("MOBY_CLERP_DBG", "Debug diagnostics", "0",
        "Face-0 colour-lerp ramp per present."),
    ENVROW_LATCHED("MOBY_INTERP_MISS", "Debug diagnostics", "0",
        "Record mobys with no interp snapshot."),
    ENVROW_LATCHED("SHADED_INTERP_MISS", "Debug diagnostics", "0",
        "Same for shaded mobys."),
    ENVROW_LATCHED("MOBY_TRIDBG", "Debug diagnostics", "",
        "Log moby walk tris in a screen box x0,y0,x1,y1,fA,fB."),
    ENVROW_LATCHED("SCENE_BOXDBG", "Debug diagnostics", "",
        "Log composed scene tris overlapping a PSX-screen box x0,y0,x1,y1 at VBLs fA..fB, "
        "with a per-present tested/covering count; retained terrain cache off while set."),
    ENVROW_LATCHED("SCENE_CAPDBG", "Debug diagnostics", "0",
        "Print new peaks of scene-tri demand vs caps."),
    ENVROW_LATCHED("TERRAIN_CACHE_DBG", "Debug diagnostics", "0",
        "~1/s sector-cache hit/miss census."),
    ENVROW_LATCHED("OCCL_CAMDBG", "Debug diagnostics", "0",
        "Trace the occlusion grid's camera: one line per grid queue and retrieval, "
        "and one per query the camera bound admits (distance and matrix trace)."),
    ENVROW_LATCHED("TERRAIN_OCCL_DBG", "Debug diagnostics", "0",
        "Sector occlusion-cull census."),
    ENVROW_LATCHED("TERRAIN_OCCL_AUDIT", "Debug diagnostics", "",
        "From this VBL on, check every sector the occlusion cull calls off-screen "
        "against the exact sphere-vs-frame test; prints each false off-screen sector "
        "and cumulative totals at exit."),
    ENVROW_LATCHED("TERRAIN_TIMERS", "Debug diagnostics", "0",
        "~1/s per-sector walk-cost census."),
    ENVROW_LATCHED("TERRAIN_VISDBG", "Debug diagnostics", "0",
        "Visible-sector list diagnostics."),
    ENVROW_LATCHED("TERRAIN_ANIMDBG", "Debug diagnostics", "0",
        "Per-present env-anim change census."),
    ENVROW_LATCHED("MOD_TABLE_CENSUS", "Debug diagnostics", "0",
        "Mod-table census at exit: one line per growable mod table with "
        "its element count, the fixed capacity it used to have, its "
        "storage size and how many times it grew. 1 writes the report to "
        "stderr; any other value names a file to write it to, which a "
        "reader needs because stderr rides a pipe whose pump can lose the "
        "tail of a burst at exit."),
    ENVROW_LATCHED("TEXPACK_CATCENSUS", "Debug diagnostics", "0",
        "HD-texpack category census (which categories resolve per level)."),
    ENVROW_LATCHED("TEXPACK_ANIMPROBE", "Debug diagnostics", "",
        "Raw Tiledef words of record N each present (env-anim rewrite shape)."),
    ENVROW_LATCHED("TEXSEL_DBG", "Debug diagnostics", "0",
        "Terrain texture-id selector match census."),
    ENVROW_LATCHED("TEXSEL_DBG2", "Debug diagnostics", "0",
        "Material texture-id selector census (compose side)."),
    ENVROW_LATCHED("TERRAIN_VANIMDBG", "Debug diagnostics", "0",
        "Vert-anim refresh census."),
    ENVROW_LIVE("TERRAIN_FOGDBG", "Debug diagnostics", "0",
        "Fog decode debug prints."),
    ENVROW_LATCHED("TERRAIN_CALDBG", "Debug diagnostics", "0",
        "Near-classification calibration stats vs .s queue."),
    ENVROW_LATCHED("TERRAIN_NEARDBG", "Debug diagnostics", "0",
        "Census of straddling foreground quads by route."),
    ENVROW_LIVE("TERRAIN_UVDBG", "Debug diagnostics", "0",
        "UV/tile A/B vs captured .s prims."),
    ENVROW_LIVE("TERRAIN_UVBOX", "Debug diagnostics", "",
        "Box filter x0,y0,x1,y1 for UVDBG."),
    ENVROW_LIVE("TERRAIN_UVALL", "Debug diagnostics", "0",
        "UVDBG print-all modifier."),
    ENVROW_LIVE("TERRAIN_POINT", "Debug diagnostics", "",
        "Frontmost native tri + .s prim under screen points x,y;x,y."),
    ENVROW_LIVE("PARTICLE_DEBUG", "Debug diagnostics", "0",
        "Native-vs-.s particle selector counts per present."),
    ENVROW_LATCHED("STREAK_DBG", "Debug diagnostics", "0",
        "Log emitted line/fan legs over 300 px."),
    ENVROW_LATCHED("PART_PHASE_DBG", "Debug diagnostics", "0",
        "Per-present phase-lerp trace of first particle."),
    ENVROW_LATCHED("GLOW_ANIM_DBG", "Debug diagnostics", "0",
        "Glow raw-change census + phase trace."),
    ENVROW_LATCHED("SPARKLE_INTERP_DBG", "Debug diagnostics", "0",
        "Per-present sparkle slot-0 phase trace."),
    ENVROW_LATCHED("PORTAL_MAT_DBG", "Debug diagnostics", "0",
        "Per-dome derived-vs-GTE matrix validation print."),
    ENVROW_LATCHED("PORTAL_REEXTRACT_DBG", "Debug diagnostics", "0",
        "1/20-frame portal replay-state print."),
    ENVROW_LIVE("POLYBUF_GUARD", "Debug diagnostics", "0",
        "polygon-buffer overflow detector: logs when the prim cursor "
        "(D_800757B0) passes the buffer end (D_800756FC) and starts "
        "overwriting g_WorldOT. 0=off (default), 1=on, "
        "verbose=per-frame headroom"),
    ENVROW_LATCHED("NOLOD_DOME_DBG", "Debug diagnostics", "0",
        "Per-event forced-dome feed print under extended LOD."),
};
#define KNOB_DEBUG_COUNT (sizeof(g_builtin_debug) / sizeof(g_builtin_debug[0]))
#else
#define KNOB_DEBUG_COUNT 0
#endif /* OPENPETE_HIDE_DEBUG_FLAGS */

/* Working table: the built-ins, copied once, plus room for module-contributed
 * rows. One flat array because the UI walks a single {ptr, count} and groups by
 * section; adds happen at feature apply-options time, long before the overlay
 * exists, so no row pointer the UI holds can be invalidated by an insertion. */
#define KNOB_RELEASE_COUNT (sizeof(g_builtin) / sizeof(g_builtin[0]))
#define KNOB_BUILTIN_COUNT (KNOB_RELEASE_COUNT + KNOB_DEBUG_COUNT)
#define KNOB_EXTRA_MAX     16

static knob_desc_t g_rows[KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX];
static size_t      g_row_count;

/** @brief Copy the built-in rows into the working table on first use: the release rows, then the debug rows marked debug_block.
 *
 * Later calls do nothing. The copy is unsynchronized: callers are expected to
 * run on one thread during boot. */
static void rows_init(void) {
    if (g_row_count) return;
    memcpy(g_rows, g_builtin, sizeof g_builtin);
#ifndef OPENPETE_HIDE_DEBUG_FLAGS
    memcpy(g_rows + KNOB_RELEASE_COUNT, g_builtin_debug, sizeof g_builtin_debug);
    for (size_t i = KNOB_RELEASE_COUNT; i < KNOB_BUILTIN_COUNT; i++)
        g_rows[i].debug_block = 1;
#endif
    g_row_count = KNOB_BUILTIN_COUNT;
}

knob_desc_t *knob_registry(size_t *count) {
    rows_init();
    if (count) *count = g_row_count;
    return g_rows;
}

int openpete_knob_add(const knob_desc_t *row) {
    if (!row) return -1;
    rows_init();
    if (g_row_count >= KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX) {
        OPL_WARN_F("knobs", "no room for row \"%s\" (extra capacity %d)",
                   row->id ? row->id : "?", KNOB_EXTRA_MAX);
        return -1;
    }

    /* Sections are rendered as contiguous runs, so a row joins its section by
     * landing right after that section's last row; an unknown section starts
     * its own run at the end. */
    size_t at = g_row_count;
    for (size_t i = 0; i < g_row_count; i++)
        if (row->section && !strcmp(g_rows[i].section, row->section))
            at = i + 1;

    memmove(&g_rows[at + 1], &g_rows[at],
            (g_row_count - at) * sizeof g_rows[0]);
    g_rows[at] = *row;
    g_row_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot resolution: value + source stamping                           */
/* ------------------------------------------------------------------ */

/** @brief The overlay source tag of a row, from the config resolver's winning layer.
 *
 * openpete_config_knob_layer holds the layer order; this maps its label to
 * the overlay's three tags: "default" (or NULL) is DEFAULT, "env" or a
 * label ending ".env" is ENV, and every other layer (the command line, the
 * bk2 sidecar, saved, an [args] or bare-key file layer) is CLI. The overlay
 * shows the precise layer and the pinned state separately, from
 * openpete_config_knob_layer and openpete_config_knob_pinned. */
static knob_src_t knob_src_from_config(const knob_desc_t *k) {
    const char *layer = openpete_config_knob_layer(k->id);
    if (!layer || !strcmp(layer, "default")) return KNOB_SRC_DEFAULT;
    if (!strcmp(layer, "env")) return KNOB_SRC_ENV;
    size_t n = strlen(layer);
    if (n >= 4 && !strcmp(layer + n - 4, ".env")) return KNOB_SRC_ENV;
    return KNOB_SRC_CLI;
}

void knob_registry_init(const struct openpete_options *opts) {
    g_opts = opts;

    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        k->boot_src = knob_src_from_config(k);

        /* Seed the row's displayed value from the parsed options. WHERE the
         * value came from is not decided here: config.c resolved the whole
         * layer stack at boot, and re-deriving it from each field's unset
         * sentinel would be a second implementation of the precedence rules,
         * free to drift from the first. The post-pass below asks the
         * resolver. */
        if (!strcmp(k->id, "fullscreen")) {
            k->stored_num = opts->fullscreen ? 1 : 0;
        } else if (!strcmp(k->id, "aspect")) {
            if (opts->aspect_w_over_h > 0.0f) {
                k->stored_num = nearest_enum_idx(k_aspect_vals + 1, 5,
                                                 opts->aspect_w_over_h) + 1;
            } else {
                k->stored_num = 0;   /* auto (also the default) */
            }
        } else if (!strcmp(k->id, "ssaa")) {
            k->stored_num = ssaa_idx_of(opts->ssaa > 0 ? opts->ssaa : 1);
        } else if (!strcmp(k->id, "vsync")) {
            if (opts->vsync != -1) {
                k->stored_num = opts->vsync ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_PRESENT_MODE");
                if (e && (!strcmp(e, "immediate") || !strcmp(e, "mailbox"))) {
                    k->stored_num = 0;
                } else {
                    k->stored_num = 1;   /* default FIFO */
                }
            }
        } else if (!strcmp(k->id, "perf_stats")) {
            k->stored_num = opts->perf_stats;
        } else if (!strcmp(k->id, "perf_position")) {
            k->stored_num = opts->perf_position;
        } else if (!strcmp(k->id, "osd")) {
            k->stored_num = opts->osd ? 1 : 0;
        } else if (!strcmp(k->id, "osd_position")) {
            k->stored_num = opts->osd_position;
        } else if (!strcmp(k->id, "osd_timeout")) {
            k->stored_num = (double)opts->osd_timeout_ms / 1000.0;
        } else if (!strcmp(k->id, "vertex_precision")) {
            k->stored_num = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER);
        } else if (!strcmp(k->id, "raster_quantize")) {
            /* mirror the renderer's auto-pair resolve (-1 = 1x iff integer snap) */
            k->stored_num = (opts->raster_quantize >= 0) ? opts->raster_quantize
                          : (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER ? 1 : 0);
        } else if (!strcmp(k->id, "dither")) {
            k->stored_num = opts->dither ? 1 : 0;
        } else if (!strcmp(k->id, "texture_mapping")) {
            k->stored_num = (opts->texture_mapping == OPENPETE_TEXTURE_MAPPING_AFFINE);
        } else if (!strcmp(k->id, "lod")) {
            k->stored_num = (opts->lod != OPENPETE_LOD_ORIGINAL);
        } else if (!strcmp(k->id, "terrain_visibility")) {
            k->stored_num = (opts->terrain_visibility != OPENPETE_TERRAIN_VIS_GAME);
        } else if (!strcmp(k->id, "fog")) {
            k->stored_num = openpete_fog_enabled() ? 1 : 0;
        } else if (!strcmp(k->id, "fog_scale")) {
            k->stored_num = openpete_fog_scale;
        } else if (!strcmp(k->id, "render_fps")) {
            k->stored_num = opts->render_fps;
        } else if (!strcmp(k->id, "rewind")) {
            k->stored_num = opts->rewind_window;
        } else if (!strcmp(k->id, "tick_scale")) {
            k->stored_num = opts->tick_scale;
        } else if (!strcmp(k->id, "runahead")) {
            k->stored_num = opts->runahead;
        } else if (!strcmp(k->id, "fov")) {
            k->stored_num = opts->fov_deg;
        } else if (!strcmp(k->id, "mute")) {
            k->stored_num = opts->mute ? 1 : 0;
        } else if (!strcmp(k->id, "sfx")) {
            k->stored_num = opts->sfx_volume;
        } else if (!strcmp(k->id, "music")) {
            k->stored_num = opts->music_volume;
        } else if (!strcmp(k->id, "vibration")) {
            k->stored_num = opts->vibration;
        } else if (!strcmp(k->id, "mouse")) {
            k->stored_num = opts->mouse != 0 ? 1 : 0;
        } else if (!strcmp(k->id, "input_lock")) {
            k->stored_num = opts->input_lock > 0 ? 1 : 0;
        } else if (!strcmp(k->id, "background")) {
            k->stored_num = opts->background > 0 ? 1 : 0;
        } else if (!strcmp(k->id, "mouse_sensitivity")) {
            k->stored_num = opts->mouse_sensitivity >= 0.0f ? opts->mouse_sensitivity : 1.0;
        } else if (!strcmp(k->id, "record_cfr")) {
            k->stored_num = opts->record_cfr;
        } else if (!strcmp(k->id, "record_frames")) {
            k->stored_num = opts->record_frames;
        } else if (!strcmp(k->id, "devui_open")) {
            if (opts->devui_open >= 0) {
                k->stored_num = opts->devui_open ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_DEVUI_OPEN");
                k->stored_num = (e && *e && *e != '0') ? 1 : 0;
            }
        } else if (!strcmp(k->id, "record_fps_overlay")) {
            if (opts->record_fps_overlay >= 0) {
                k->stored_num = opts->record_fps_overlay ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_FPS_IN_RECORD");
                k->stored_num = (e && *e && *e != '0') ? 1 : 0;
            }
        } else if (!strcmp(k->id, "renderer")) {
            k->stored_num = opts->renderer;
        } else if (!strcmp(k->id, "skip_to_level")) {
            k->stored_num = opts->skip_to_level;
        } else if (!strcmp(k->id, "skip_to_cutscene")) {
            k->stored_num = opts->skip_to_cutscene;
        } else if (!strcmp(k->id, "log_format")) {
            k->stored_num = opts->log_format;
        } else if (!strcmp(k->id, "freecam") || !strcmp(k->id, "camlock")) {
            /* Both start disengaged whatever armed them: the env var (or
             * OPENPETE_CAMLOCK_POSE) requests engagement later, at a point
             * the feature itself picks. */
            k->stored_num = 0;
        }

        /* Stamp the boot value for the "reset to boot" affordance. Actions
         * carry no value to reset to. */
        if (k->type == KNOB_ACTION) {
            continue;
        } else if (k->type == KNOB_STRING) {
            if (k->get_str) k->get_str(k, k->boot_str, sizeof k->boot_str);
        } else {
            k->boot_num = k->get_num ? k->get_num(k) : k->stored_num;
        }
    }
}

int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const knob_desc_t *k,
                                           char *buf, size_t cap),
                           int (*set_str)(knob_desc_t *k, const char *v)) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        if (get_str) ks[i].get_str = get_str;
        if (set_str) { ks[i].set_str = set_str; ks[i].mut = KNOB_LIVE; }
        return 0;
    }
    return -1;   /* row absent (compiled out) - nothing to bind */
}

int openpete_knob_bind_vocab(const char *id, const char *const *tokens,
                             int count) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        ks[i].enum_values = tokens;
        ks[i].enum_count  = count;
        return 0;
    }
    return -1;   /* row absent (compiled out) - nothing to bind */
}

int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (!k || k->type == KNOB_ACTION) return -1;
    if (cfg_schema_find_id(k->id) < 0) return -1;   /* not a config knob */

    /* The SSAA row is an enum of "off"/"2x"/"3x"/"4x" because that is what
     * reads well in a menu; the flag takes the factor (1 = off). Every other
     * enum row already displays the CLI's own words. */
    if (!strcmp(k->id, "ssaa")) {
        int idx = (int)(k->get_num ? k->get_num(k) : k->stored_num);
        if (idx < 0 || idx >= K_SSAA_N) idx = 0;
        snprintf(buf, cap, "%d", k_ssaa_vals[idx]);
        return 0;
    }

    knob_value_str(k, buf, cap);
    /* get_str placeholders ("(default)", "(none)", "(unset - 1)") describe
     * the absence of a value; writing one back as a literal would turn it
     * into a real setting on the next boot. */
    if (buf[0] == '(') return -1;
    return 0;
}

int knob_registry_save_edited(void) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    int saved = 0;
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        if (k->type == KNOB_ACTION) continue;

        /* Only what the user actually moved. Saving unchanged rows would
         * bake this launch's whole resolved state into the file, so a knob
         * they never touched would stop following its default. */
        char now[KNOB_STR_MAX];
        if (k->type == KNOB_STRING) {
            if (!k->get_str) continue;
            k->get_str(k, now, sizeof now);
            if (!strcmp(now, k->boot_str)) continue;
        } else {
            double v = k->get_num ? k->get_num(k) : k->stored_num;
            if (v == k->boot_num) continue;
        }
        if (knob_config_value_str(k, now, sizeof now) != 0) continue;
        if (openpete_config_save_knob(k->id, now) == 0) saved++;
    }
    if (openpete_config_save_flush() != 0) return -1;
    return saved;
}

void knob_registry_late_stamp(void) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(ks[i].id, "render_fps") ||
            !strcmp(ks[i].id, "tick_scale") ||
            !strcmp(ks[i].id, "runahead")) {
            /* The pacer seeds its live state (resolved render-fps target,
             * tick scale, gated runahead depth) at host-loop start, after
             * knob_registry_init ran; adopt the settled value as the boot
             * value so the row doesn't read as "edited" on first open. */
            ks[i].boot_num   = ks[i].get_num(&ks[i]);
            ks[i].stored_num = ks[i].boot_num;
        }
    }
}

void knob_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (k->type == KNOB_ACTION) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "%s", k->action_label ? k->action_label : "Apply");
        return;
    }
    if (k->type == KNOB_STRING) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "?");
        return;
    }
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    switch (k->type) {
    case KNOB_BOOL:  snprintf(buf, cap, "%s", v != 0.0 ? "on" : "off"); break;
    case KNOB_INT:   snprintf(buf, cap, "%d", (int)v); break;
    case KNOB_FLOAT: snprintf(buf, cap, "%.3g", v); break;
    case KNOB_ENUM: {
        int i = (int)v;
        if (k->enum_values && i >= 0 && i < k->enum_count)
            snprintf(buf, cap, "%s", k->enum_values[i]);
        else
            snprintf(buf, cap, "%d", i);
        break;
    }
    default: snprintf(buf, cap, "?"); break;
    }
}
