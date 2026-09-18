/* games/spyro-2/renderer/reextract.c — Spyro 2's sub-tick re-extract
 * orchestrator: on every present between two canonical ticks it installs the
 * camera transform interpolated between the interp ring's prev and curr
 * snapshots (region 0) into the sub-tick view, and re-runs the sky and
 * terrain feeders against it. The staged bytes are what every feeder read resolves to for the
 * duration of the present; guest RAM is never written.
 *
 * Provides the renderer/orchestrator.h entries. The canonical-scratch captures
 * and the per-producer camera bracket report "nothing captured", which is what
 * the moby, player and shadow feeders this game does not yet reach would fill;
 * their call sites stay where Spyro 3 has them so a later feeder lands
 * without moving the surrounding code. */
#include "renderer/orchestrator.h"
#include "renderer/cyclorama.h"
#include "renderer/terrain.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "openpete_interp.h"
#include "renderer/interp_lerp.h"    /* lerp16/32, lerp_matrix, lerp_vec3 */
#include "renderer/interp_regions_gen.h"
#include "renderer/reextract_view.h"
#include "renderer/env_knob.h"
#include "openpete_interp_objs.h"      /* g_openpete_tick_period_ns */
#include "openpete_native_render.h"
#include "openpete_subtick_view.h"
#include "openpete_pace_log.h"
#include "openpete_fiber.h"   /* openpete_frame */
#include "psx_runtime_internal.h"
#include "game_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Offsets inside the camera transform block, all confirmed against the layout
 * descriptor's read sites: the sky matrix the cyclorama draw loads with ctc2
 * (cyclorama.s:32-40), the world matrix the terrain draw loads the same way
 * (environment.s:1572-1581) and the world position the sector cull reads
 * (environment.s:1585-1588). */
#define CAM_OFF_SKY   0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u

/* The un-rounded interpolated camera position of this present, for the
 * projection's floor fix; valid only while a bracket is installed. */
static int    s_fpos_active;
static double s_fpos[3];

int openpete_native_interp_pos(unsigned int key, double out[3]) {
    if (!s_fpos_active || key != game_layout.cam.base) return 0;
    out[0] = s_fpos[0]; out[1] = s_fpos[1]; out[2] = s_fpos[2];
    return 1;
}

/* The canonical-render epoch (renderer/orchestrator.h): every canonical
 * present closes one, and a producer that ran inside the one just closed is
 * fresh, so a sub-tick pass replays only what the canonical present drew. The
 * terrain feeder re-derives from the camera and stamps nothing, so no producer
 * reports fresh yet; the counter runs anyway so a feeder added later gets a
 * correct epoch from its first tick. */
static uint32_t s_epoch_now = 1;   /* the stamp captures take (present in flight) */
static uint32_t s_epoch_ok;        /* the stamp a capture must carry to be fresh */

/* OPENPETE_CAM_DBG=1: per canonical present and per sub-tick re-extract, the
 * camera transform the pass will draw with: the world matrix at +0x14 as nine
 * shorts and the position at +0x28 as three words. The canonical line reads
 * live guest memory, the re-extract line reads the ring's two endpoints and the
 * staged lerp, so a camera the two passes disagree on is a printed number.
 * Diagnostic only. */
static int cam_dbg_on(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_CAM_DBG");
    return v;
}
static void cam_dbg_line(const char* tag, const uint8_t* c) {
    if (!c) { fprintf(stderr, "[cam-dbg f=%u %s] (null)\n", openpete_frame, tag); return; }
    const int16_t* m = (const int16_t*)(c + CAM_OFF_VIEW);
    const int16_t* sk = (const int16_t*)(c + CAM_OFF_SKY);
    const int32_t* p = (const int32_t*)(c + CAM_OFF_POS);
    fprintf(stderr, "[cam-dbg f=%u %s] pos=(%d,%d,%d) view=[%d %d %d %d %d %d %d %d %d] sky0=%d\n",
            openpete_frame, tag, p[0], p[1], p[2],
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], sk[0]);
}

void spyro_native_canon_epoch_commit(void) {
    if (cam_dbg_on())
        cam_dbg_line("CANON", (const uint8_t*)openpete_psx_to_host_ptr(game_layout.cam.base));
    s_epoch_ok = s_epoch_now;
    s_epoch_now++;
}
uint32_t spyro_native_canon_epoch_stamp(void) { return s_epoch_now; }
int  spyro_native_canon_epoch_fresh(uint32_t stamp) { return stamp == s_epoch_ok; }
int  spyro_native_canon_mobyreg_fresh(void) { return 0; }   /* no moby feeder */
int  spyro_native_canon_shaded_fresh(void) { return 0; }    /* no second material pass */

/* The canonical-scratch captures: nothing this game's terrain feeder reads is
 * transient render-pass scratch, so there is nothing to copy out and stage
 * back. The moby queue and the player part records are what fill these on a
 * game whose feeders decode them. */
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len) {
    (void)cpu; (void)off; (void)len;
}
void spyro_native_canon_capture_recs(void* cpu) { (void)cpu; }
void spyro_native_canon_capture_sxy(void* cpu)  { (void)cpu; }
void spyro_native_canon_capture_camera(void* cpu, int producer) { (void)cpu; (void)producer; }
int  spyro_native_prod_camera_enter(int producer) { (void)producer; return 0; }
void spyro_native_prod_camera_leave(void) {}

/* A cut: the camera moved more than 5000 units or turned more than 90
 * degrees in one tick (a level entry, a teleport), where lerping the chord
 * sweeps the view through geometry it never showed. */
static int camera_cut(const uint8_t* p, const uint8_t* c) {
    const int32_t* pp = (const int32_t*)(p + CAM_OFF_POS);
    const int32_t* cp = (const int32_t*)(c + CAM_OFF_POS);
    double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
    if (dx*dx + dy*dy + dz*dz > 5000.0 * 5000.0) return 1;
    const int16_t* pm = (const int16_t*)(p + CAM_OFF_SKY);
    const int16_t* cm = (const int16_t*)(c + CAM_OFF_SKY);
    double frob = 0.0;
    for (int k = 0; k < 9; k++) frob += (double)pm[k] * (double)cm[k];
    return frob < 4096.0 * 4096.0;   /* row-dot sum under one unit: more than 90 degrees */
}

/* OPENPETE_RX_CENSUS=LO-HI (or a single frame): per sub-tick re-extract in that
 * frame range, the triangles each producer added to the scene, in call order.
 * A producer that returns early reads as zero, which is what separates a
 * missing producer from an empty one. Diagnostic only. */
static int rx_census_on(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    return env_frame_range_active(&s_range, "OPENPETE_RX_CENSUS", openpete_frame);
}
static uint32_t rx_scene_tris(void) {
    uint32_t n[16], t = 0;
    openpete_native_scene_tag_family_counts(n);
    for (int f = 0; f < 16; f++) t += n[f];
    return t;
}
/* The line is assembled in a buffer and written once at END: the producers
 * this census brackets have stderr traces of their own, and a line built by
 * one fprintf per step interleaves with them into an unreadable row. */
#define RX_CENSUS_BEGIN() \
    const int rx_cen = rx_census_on(); \
    uint32_t rx_prev = 0; \
    char rx_line[256]; int rx_len = 0; \
    if (rx_cen) { rx_prev = rx_scene_tris(); \
        rx_len = snprintf(rx_line, sizeof rx_line, "[rx-census f=%u REEXTRACT]", openpete_frame); \
        if (rx_len < 0) rx_len = 0; }
#define RX_CENSUS_STEP(nm) \
    if (rx_cen) { uint32_t rx_now = rx_scene_tris(); \
        if (rx_len < (int)sizeof rx_line) { \
            int rx_n = snprintf(rx_line + rx_len, sizeof rx_line - (size_t)rx_len, \
                                " %s=%u", (nm), rx_now - rx_prev); \
            if (rx_n > 0) rx_len += rx_n; } \
        rx_prev = rx_now; }
#define RX_CENSUS_END(br, al) \
    if (rx_cen) { \
        if (rx_len < (int)sizeof rx_line) \
            snprintf(rx_line + rx_len, sizeof rx_line - (size_t)rx_len, \
                     " | bracketed=%d alpha=%.3f", (br), (al)); \
        rx_line[sizeof rx_line - 1] = '\0'; \
        fprintf(stderr, "%s\n", rx_line); }

static void game_reextract_body(CPUState* cpu) {
    double qa = 0.0;
    const openpete_interp_snapshot_t *prev2 = NULL, *prev = NULL, *curr = NULL;
    int paired = openpete_interp_query3(openpete_interp_present_now(), g_openpete_tick_period_ns,
                                        &qa, &prev2, &prev, &curr);
    if (!paired) openpete_pace_note_alpha(0.0, 0);

    const uint32_t cam_len = game_layout.cam.xform_end - game_layout.cam.base;
    uint8_t* cam = (uint8_t*)openpete_subtick_view_stage(game_layout.cam.base, cam_len);
    int bracketed = 0;
    double alpha = 0.0;
    if (paired && cam && prev && curr) {
        /* The camera's ring region, resolved by name from the registered table:
         * a restated index selects the wrong region once a row is inserted. */
        static int cam_region = -2;
        if (cam_region == -2) cam_region = game_interp_row_index(GAME_INTERP_CAM_ROW);
        const uint8_t* pcam = cam_region < 0 ? NULL : prev->data + prev->region_offsets[cam_region];
        const uint8_t* ccam = cam_region < 0 ? NULL : curr->data + curr->region_offsets[cam_region];
        if (pcam && ccam && !camera_cut(pcam, ccam)) {
            double period = (double)(curr->tick_ns - prev->tick_ns);
            if (period <= 0.0 || period > 4.0 * (double)g_openpete_tick_period_ns)
                period = (double)g_openpete_tick_period_ns;
            alpha = (double)(openpete_interp_present_now() - curr->tick_ns) / period;
            /* The registered regions stopped changing longer ago than the
             * pair's own cadence: prev to curr describes a motion that has
             * ended, and the head is the state this frame shows. Wall-clock
             * phase reaches the same answer by itself (now runs away from
             * curr->tick_ns and the clamp below pins alpha at 1); a present
             * phased onto curr->tick_ns does not, and would re-lerp the middle
             * of that last motion at any distance behind the frame it is
             * labelled with. */
            if (openpete_interp_curr_is_stale()) alpha = 1.0;
            if (alpha < -0.35) alpha = -0.35; else if (alpha > 1.0) alpha = 1.0;
            lerp_matrix(cam, pcam, ccam, CAM_OFF_SKY,  alpha);
            lerp_matrix(cam, pcam, ccam, CAM_OFF_VIEW, alpha);
            lerp_vec3  (cam, pcam, ccam, CAM_OFF_POS,  alpha);
            const int32_t* pp = (const int32_t*)(pcam + CAM_OFF_POS);
            const int32_t* cp = (const int32_t*)(ccam + CAM_OFF_POS);
            for (int i = 0; i < 3; i++) s_fpos[i] = (double)pp[i] + ((double)cp[i] - (double)pp[i]) * alpha;
            s_fpos_active = 1;
            bracketed = 1;
            openpete_pace_note_alpha(alpha, 1);
        }
    }
    if (cam_dbg_on()) {
        fprintf(stderr, "[cam-dbg f=%u RX] paired=%d bracketed=%d alpha=%.4f\n",
                openpete_frame, paired, bracketed, alpha);
        cam_dbg_line("RX-live", (const uint8_t*)openpete_psx_to_host_ptr(game_layout.cam.base));
        cam_dbg_line("RX-staged", cam);
        if (paired && prev && curr) {
            static int cdbg_region = -2;
            if (cdbg_region == -2) cdbg_region = game_interp_row_index(GAME_INTERP_CAM_ROW);
            if (cdbg_region >= 0) {
                cam_dbg_line("RX-prev", prev->data + prev->region_offsets[cdbg_region]);
                cam_dbg_line("RX-curr", curr->data + curr->region_offsets[cdbg_region]);
            }
        }
    }
    openpete_native_scene_begin();
    RX_CENSUS_BEGIN();
    cyclorama_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* sky dome + background */
    RX_CENSUS_STEP("cyclorama");
    terrain_reextract(cpu, bracketed ? alpha : 0.0, bracketed);
    RX_CENSUS_STEP("terrain");
    /* The HUD, dialog boxes, text and screen fades are produced during the
     * game's own render and cannot be re-derived here, so the canonical present
     * banks them and every re-extract re-applies that carry on top of the
     * re-derived 3D scene (fade interpolated by the alpha, held panels
     * re-emitted as captured). Nothing banks a panel until this game reaches
     * the 2D-UI feeder, and an empty carry re-applies nothing. */
    openpete_native_scene_apply_overlay(bracketed ? alpha : 0.0);
    RX_CENSUS_END(bracketed, alpha);
    s_fpos_active = 0;
}

static void game_reextract_body_v(void* cpu_v) {
    game_reextract_body((CPUState*)cpu_v);
}

void game_reextract_init(void) {
    game_reextract_view_install(game_reextract_body_v);
}
