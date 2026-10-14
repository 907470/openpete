/* Target captures of the host loop; see openpete_run_capture.h. */
#include "openpete_run_capture.h"

#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_native_render.h"
#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void openpete_psyx_take_screenshot(const char* path);  /* PNG or BMP by extension */
extern void openpete_psyx_take_screenshot_ext(const char* path,
                                              int* out_w, int* out_h);

/* Every leg's state. A target's fired flag makes it fire once; an armed flag
 * delays a PsyCross-sourced leg by one present (see the leg). */
static struct {
    bool og_fired[OPENPETE_MAX_SCREENSHOT_FRAMES];   /* -og leg */
    bool og_armed[OPENPETE_MAX_SCREENSHOT_FRAMES];
    bool vk_fired[OPENPETE_MAX_SCREENSHOT_FRAMES];   /* -vk leg */
    bool vk_armed[OPENPETE_MAX_SCREENSHOT_FRAMES];
    bool int_fired[OPENPETE_MAX_SCREENSHOT_FRAMES];  /* intermediate leg */
    double int_alpha;            /* OPENPETE_INTERMEDIATE_ALPHA, < 0 until read */
    /* Intermediate legs taken, and how many of them the game's re-extract
     * bracketed. A leg with no bracket rendered the canonical pose under a
     * sub-tick filename, so a harness that reads only the file reports a pass
     * on an image the sub-tick path never produced. */
    unsigned inter_shots, inter_bracketed;
    /* --screenshot-presents-start/count: presents captured since the feed key
     * reached the start, shared by the canonical and the intermediate legs so
     * both contribute to one sequence; and the bookkeeping that logs each
     * capture and warns once when the sequence mixes extents. */
    int presents_captured;
    int presents_written;        /* captures written, all kinds */
    int presents_native_kind;    /* native-offscreen intermediate captures */
    int shot_w, shot_h;          /* the sequence's reference extent */
    int mixed_warned;
    /* OPENPETE_VRAM_DUMP: the path, read once, and whether it was written. */
    int vd_read;
    const char *vd;
    bool vram_dumped;
} s_cap = { .int_alpha = -1.0 };

/** @brief Log one captured present and warn once when the sequence mixes pixel extents.
 *
 * Counts the capture. A capture with a nonpositive extent is not compared;
 * the first positive extent becomes the sequence's reference and a later
 * different one logs a single warning naming both. Images at two extents
 * cannot be differenced against each other, so the warning marks the run
 * for discarding.
 * @param path The written file.
 * @param kind The capture kind label.
 * @param present_index The present's index.
 * @param frame The feed key's value (openpete_feed_key_now) at the capture.
 * @param w Image width in pixels.
 * @param h Image height in pixels. */
static void presents_note_shot(const char *path, const char *kind,
                               int present_index, unsigned frame, int w, int h)
{
    OPL_INFO_F("screenshot",
               "wrote %s on frame %u (present=%d, kind=%s, extent=%dx%d)",
               path, frame, present_index, kind, w, h);
    s_cap.presents_written++;
    if (w <= 0 || h <= 0) return;
    if (s_cap.shot_w == 0) {
        s_cap.shot_w = w; s_cap.shot_h = h;
    } else if ((w != s_cap.shot_w || h != s_cap.shot_h) && !s_cap.mixed_warned) {
        s_cap.mixed_warned = 1;
        OPL_WARN_F("screenshot",
                   "present sequence mixes extents: %dx%d and %dx%d; "
                   "presents from this run are not comparable",
                   s_cap.shot_w, s_cap.shot_h, w, h);
    }
}

/** @brief Return whether the present sequence is open at the current feed key.
 * @param opts Session options.
 * @return true while --screenshot-presents-start is reached and the count is not. */
static bool presents_open(const openpete_options_t *opts)
{
    return opts->screenshot_presents_start >= 0
        && (int)openpete_feed_key_now() >= opts->screenshot_presents_start
        && s_cap.presents_captured < opts->screenshot_presents_count;
}

/** @brief Write the -og reference of one --screenshot-on-frame target.
 *
 * The file is the target's path with -og before the extension, so it sits
 * beside the native -vk dump.
 * @param opts Session options.
 * @param si Target index. */
static void og_write(const openpete_options_t *opts, int si)
{
    char base[1024], path[1100];
    openpete_screenshot_format_path(opts->screenshot_output,
                                    opts->screenshot_frames[si],
                                    /*present=*/0,
                                    base, sizeof(base));
    const char *odot = strrchr(base, '.');
    if (odot) snprintf(path, sizeof(path), "%.*s-og%s",
                       (int)(odot - base), base, odot);
    else      snprintf(path, sizeof(path), "%s-og", base);
    openpete_psyx_take_screenshot(path);
    OPL_INFO_F("screenshot",
               "wrote %s on frame %u tick %u (target=%d, %s-keyed)",
               path, openpete_frame, openpete_canon_tick,
               opts->screenshot_frames[si],
               opts->screenshot_ticks_keyed ? "tick" : "frame");
}

/** @brief Write the -og reference of every --screenshot-on-frame target the key reached.
 *
 * --screenshot-native-only skips the leg: PsyCross isn't rasterizing
 * (openpete_psycross_renders == 0), so the present FBO it reads is stale.
 * PsyCross rasterizes an OT on the present AFTER the one that submitted it,
 * so the FBO on the keyed present holds the previous tick; the leg arms on
 * the keyed present and fires one present later, so the -og and -vk files
 * show the same tick.
 * @param opts Session options.
 * @param shot_key The present's capture key. */
static void og_leg(const openpete_options_t *opts, int shot_key)
{
    for (int si = 0; !opts->screenshot_native_only &&
                     si < opts->screenshot_frame_count; ++si) {
        if (s_cap.og_fired[si]) continue;
        if (shot_key < opts->screenshot_frames[si]) continue;
        if (!s_cap.og_armed[si]) { s_cap.og_armed[si] = true; continue; }
        s_cap.og_fired[si] = true;
        og_write(opts, si);
    }
}

/** @brief Write PsyCross's 1024x512 16-bit VRAM mirror once under OPENPETE_VRAM_DUMP=<path>.
 *
 * Written on the frame or tick named by OPENPETE_VRAM_DUMP_FRAME, default the
 * first keyed screenshot. The mirror is CPU-side, so this is independent of
 * GL state and works headless. Raw little-endian u16, row-major.
 * @param opts Session options.
 * @param shot_key The present's capture key. */
static void vram_dump_leg(const openpete_options_t *opts, int shot_key)
{
    if (!s_cap.vd_read) { s_cap.vd = getenv("OPENPETE_VRAM_DUMP"); s_cap.vd_read = 1; }
    if (!s_cap.vd || !*s_cap.vd || s_cap.vram_dumped) return;
    const char *vf = getenv("OPENPETE_VRAM_DUMP_FRAME");
    int want = vf && *vf ? atoi(vf)
             : (opts->screenshot_frame_count > 0 ? opts->screenshot_frames[0] : -1);
    if (want < 0 || shot_key < want) return;
    extern void GR_ReadVRAM(unsigned short *dst, int x, int y, int dst_w, int dst_h);
    static unsigned short vram[1024 * 512];
    GR_ReadVRAM(vram, 0, 0, 1024, 512);
    FILE *vfh = fopen(s_cap.vd, "wb");
    if (!vfh) { OPL_WARN_F("vram-dump", "cannot open %s", s_cap.vd); return; }
    size_t n = fwrite(vram, sizeof vram, 1, vfh);
    fclose(vfh);
    s_cap.vram_dumped = true;
    OPL_INFO_F("vram-dump", "wrote %s (1024x512 u16) on frame %u tick %u%s",
               s_cap.vd, openpete_frame, openpete_canon_tick,
               n == 1 ? "" : " (SHORT WRITE)");
}

/** @brief Write one present of the sequence from a capture source and count it.
 *
 * The psycross-canonical kind reads PsyCross's present FBO; the native kinds
 * read the native offscreen.
 * @param opts Session options.
 * @param kind The capture kind label: psycross-canonical, native-canonical or native-intermediate. */
static void presents_write(const openpete_options_t *opts, const char *kind)
{
    char path[1024];
    int sw = 0, sh = 0;
    openpete_screenshot_format_path(opts->screenshot_output,
                                    (int)openpete_feed_key_now(),
                                    s_cap.presents_captured,
                                    path, sizeof(path));
    if (strcmp(kind, "psycross-canonical") == 0) {
        openpete_psyx_take_screenshot_ext(path, &sw, &sh);
    } else {
        /* The device is the present thread's while eager is engaged; take it
         * back for the readback and return it. */
        const int pt_was = openpete_present_thread_active();
        if (pt_was) openpete_present_thread_set_active(0);
        openpete_native_render_screenshot_ext(path, kind, &sw, &sh);
        if (pt_was) openpete_present_thread_set_active(1);
    }
    presents_note_shot(path, kind, s_cap.presents_captured,
                       openpete_feed_key_now(), sw, sh);
    s_cap.presents_captured++;
}

bool openpete_capture_canon_reference(const openpete_options_t *opts, int shot_key)
{
    og_leg(opts, shot_key);
    vram_dump_leg(opts, shot_key);
    /* Canonical leg of --screenshot-presents-start/count. The intermediate leg
     * reads the native offscreen at the window extent, so reading PsyCross's
     * present FBO here would write the sequence through two renderers at two
     * extents (the guest 4:3 crop against the Hor+-widened native frame) and
     * no adjacent pair would be comparable. With a native scene the capture
     * is therefore deferred to openpete_capture_canon_native, where the
     * offscreen holds this canonical present through the same compose the
     * intermediate leg reads. The PsyCross read stays the capture only when
     * there is no native scene, where it is the only image. */
    if (!presents_open(opts)) return false;
    if (openpete_native_scene_enabled()) return true;
    presents_write(opts, "psycross-canonical");
    return false;
}

/** @brief Write the -vk dump of every --screenshot-on-frame target the key reached.
 *
 * The native leg never lags: it composes the newest tick at the keyed
 * present, and every PsyCross-sourced leg arms itself to catch up. In vk mode
 * this leg is that native compose, so it takes no arm. In PsyCross display
 * mode it is a readback of PsyCross's present FBO and carries the same
 * one-present OT lag the -og arm cancels, so it takes the same arm. Both legs
 * of a target then name one tick, and a target names the same tick in either
 * mode.
 * @param opts Session options.
 * @param shot_key The present's capture key. */
static void vk_leg(const openpete_options_t *opts, int shot_key)
{
    for (int si = 0; si < opts->screenshot_frame_count; ++si) {
        if (s_cap.vk_fired[si]) continue;
        if (shot_key < opts->screenshot_frames[si]) continue;
        if (!openpete_native_scene_enabled() && !s_cap.vk_armed[si]) {
            s_cap.vk_armed[si] = true;
            continue;
        }
        s_cap.vk_fired[si] = true;
        char base[1024];
        openpete_screenshot_format_path(opts->screenshot_output,
                                        opts->screenshot_frames[si],
                                        0, base, sizeof(base));
        /* The device is the present thread's while eager is engaged: take it
         * back for the dump, then return it. */
        const int pt_was = openpete_present_thread_active();
        if (pt_was) openpete_present_thread_set_active(0);
        openpete_native_render_screenshot_all(base);
        if (pt_was) openpete_present_thread_set_active(1);
    }
}

void openpete_capture_canon_native(const openpete_options_t *opts, int shot_key,
                                   bool presents_owed)
{
    if (presents_owed && openpete_native_scene_enabled())
        presents_write(opts, "native-canonical");
    /* Maintain the capture freeze backdrop: render this present's world frame
     * into the capture freeze texture so a later headless dump of a freeze
     * frame (pause / flight-results / level-exit) shows the green frozen-world
     * effect. The live path snapshots frozen_tex only with a window, so a
     * headless capture must keep it itself. A no-op on freeze/empty frames,
     * so once the game pauses it holds the last world frame. Off by default
     * (an extra render per present); enabled only for bk2s/poses that reach a
     * freeze state. */
    if (opts->screenshot_freeze_backdrop && openpete_native_scene_enabled()
        && (opts->screenshot_frame_count > 0 || opts->screenshot_intermediate_count > 0))
        openpete_native_render_snapshot_frozen(openpete_native_widescreen_factor());
    if (openpete_native_render_enabled() && opts->screenshot_frame_count > 0)
        vk_leg(opts, shot_key);
}

/** @brief Return the sub-tick phase of the intermediate leg, reading OPENPETE_INTERMEDIATE_ALPHA once.
 *
 * The phase is a fraction of the tick period: 0 sits on the ring's curr
 * stamp and 1 on the identity instant, so the re-extract can be diffed
 * against the canonical dump of the same tick as an identity check on the
 * spine. Default 0.5; clamped to 0..1.
 * @return The phase. */
static double intermediate_alpha(void)
{
    if (s_cap.int_alpha < 0.0) {
        const char* e = getenv("OPENPETE_INTERMEDIATE_ALPHA");
        s_cap.int_alpha = (e && *e) ? atof(e) : 0.5;
        if (s_cap.int_alpha < 0.0) s_cap.int_alpha = 0.0; else if (s_cap.int_alpha > 1.0) s_cap.int_alpha = 1.0;
    }
    return s_cap.int_alpha;
}

/** @brief Re-extract the scene at the intermediate phase and write one target's -vk dump.
 * @param opts Session options.
 * @param ire The game's re-extract.
 * @param cpu The guest CPU it reads.
 * @param si Target index. */
static void intermediate_shot(const openpete_options_t *opts,
                              openpete_native_reextract_fn ire, void *cpu, int si)
{
    const int target = opts->screenshot_intermediate_frames[si];
    int paired = openpete_interp_paired();
    if (!paired)
        OPL_WARN_F("screenshot",
                   "intermediate frame %d: no interp bracket "
                   "(spawn/load/static) - capturing canonical pose", target);
    op_time_t curr_ns = openpete_interp_curr_tick_ns();
    const double alpha = intermediate_alpha();
    /* alpha 1 is the identity leg: every consumer must land on its own curr
     * state, and a consumer whose distinct-state gap exceeds one tick period
     * is still mid-blend one nominal period past the ring's curr stamp. The
     * identity instant saturates the slowest live gap; every faster consumer
     * clamps its own phase at 1. */
    openpete_interp_set_present_target(
        alpha >= 1.0
            ? openpete_interp_identity_present_ns()
            : curr_ns + (op_time_t)((double)g_openpete_tick_period_ns * alpha));
    ire(cpu);                                  /* s_scene @ sub-tick phase */
    openpete_interp_set_present_target(0);     /* restore op_now() fallback */
    /* The bracket verdict of the pass just run: the game-side re-extract
     * publishes it with the alpha it drew at. */
    const int bracketed = openpete_pace_last_alpha_paired();
    s_cap.inter_shots++;
    s_cap.inter_bracketed += bracketed ? 1u : 0u;
    char base[1024];
    openpete_screenshot_format_path(opts->screenshot_output, target, 0, base, sizeof(base));
    /* The re-extract built the interpolated scene into the CURRENT scene
     * (never pushed to frame history), so compose depth 0 - the normal
     * shot-compose delay reads an empty lagged slot. */
    openpete_native_render_set_capture_nodelay(1);
    openpete_native_render_screenshot_all(base);
    openpete_native_render_set_capture_nodelay(0);
    OPL_INFO_F("screenshot",
               "wrote intermediate alpha=%.3f native -vk on frame %u (target=%d, paired=%d, bracketed=%d)",
               alpha, openpete_frame, target, paired, bracketed);
}

void openpete_capture_canon_intermediate(const openpete_options_t *opts, void *cpu)
{
    /* Headless-capable: this drives an explicit re-extract; the live sub-tick
     * present path is headless-blocked. Deterministic capture pins the ring
     * stamps to a fixed grid so alpha lands exactly on the phase. */
    if (!openpete_native_scene_enabled() || opts->screenshot_intermediate_count <= 0) return;
    openpete_native_reextract_fn ire = openpete_native_get_reextract();
    for (int si = 0; ire && si < opts->screenshot_intermediate_count; ++si) {
        if (s_cap.int_fired[si]) continue;
        if ((int)openpete_feed_key_now() < opts->screenshot_intermediate_frames[si]) continue;
        s_cap.int_fired[si] = true;
        intermediate_shot(opts, ire, cpu, si);
    }
}

void openpete_capture_subtick_present(const openpete_options_t *opts)
{
    if (!presents_open(opts)) return;
    s_cap.presents_native_kind++;
    presents_write(opts, "native-intermediate");
}

void openpete_capture_report_presents(const openpete_options_t *opts)
{
    if (opts->screenshot_presents_start < 0) return;
    OPL_INFO_F("screenshot",
               "present sequence: %d captured, %d native-intermediate, "
               "extent %dx%d%s",
               s_cap.presents_written,
               s_cap.presents_native_kind, s_cap.shot_w, s_cap.shot_h,
               s_cap.mixed_warned ? " (MIXED EXTENTS)"
               : s_cap.presents_native_kind ? "" : " (VACUOUS: no intermediate)");
}

void openpete_capture_report_intermediate(void)
{
    if (s_cap.inter_shots)
        OPL_INFO_F("sub-tick", "%u intermediate screenshot leg(s): %u bracketed, %u unbracketed",
                   s_cap.inter_shots, s_cap.inter_bracketed,
                   s_cap.inter_shots - s_cap.inter_bracketed);
}
