/* games/spyro-2/renderer/reextract.c — Spyro 2's sub-tick re-extract
 * orchestrator: on every present between two canonical ticks it installs the
 * camera transform interpolated between the interp ring's prev and curr
 * snapshots (region 0) into the sub-tick view, and re-runs the sky, terrain,
 * moby and player feeders against it. The staged bytes are what every feeder
 * read resolves to for the duration of the present; guest RAM is never
 * written.
 *
 * Provides the renderer/orchestrator.h entries. The per-producer camera
 * bracket reports "nothing captured", which is what the feeders this game does
 * not yet reach would fill; their call sites stay where the other game has
 * them so a later feeder lands without moving the surrounding code. */
#include "renderer/orchestrator.h"
#include "renderer/cyclorama.h"
#include "renderer/moby.h"
#include "renderer/terrain.h"
#include "renderer/player.h"
#include "renderer/shadow.h"        /* shadow_reextract, shadow_player_reextract */
#include "renderer/particles.h"
#include "renderer/player_game.h"   /* PLAYER_PARTS_MAX */
#include "renderer/portal_spyro23.h" /* portal_s23_reextract */
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "openpete_interp.h"
#include "renderer/flame.h"          /* the shared fire-breath producer */
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
#include <string.h>

/* Offsets inside the camera transform block, all confirmed against the layout
 * descriptor's read sites: the sky matrix the cyclorama draw loads with ctc2
 * (cyclorama.s:32-40), the world matrix the terrain draw loads the same way
 * (environment.s:1572-1581) and the world position the sector cull reads
 * (environment.s:1585-1588). */
#define CAM_OFF_SKY   0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u
/* The world position inside the player-shadow state block
 * (game_layout.player.shadow_base, asm/mobyfunc.s:626-628). */
#define SHADOW_POS_OFF 0x04u

/* The un-rounded interpolated camera position of this present, for the
 * projection's floor fix; valid only while a bracket is installed. */
static int    s_fpos_active;
static double s_fpos[3];

/* The same for the player's world position, installed per record by the replay
 * loop below. The provider's part-0 transform asks for it by the state block's
 * address, so one slot serves every record in turn. */
static int    s_ppos_active;
static double s_ppos[3];

int openpete_native_interp_pos(unsigned int key, double out[3]) {
    const double* src = NULL;
    if (s_fpos_active && key == game_layout.cam.base)          src = s_fpos;
    else if (s_ppos_active && key == game_layout.player.base)  src = s_ppos;
    if (!src) return 0;
    out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
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
/* The stamp the moby cull's record capture took, so a tick that ran no moby
 * draw (a pause or a load screen) leaves the queue stale instead of replaying
 * the last level's records. */
static uint32_t s_mobyreg_epoch;

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
    player_records_epoch_commit();               /* close the hooks' record epoch */
}
uint32_t spyro_native_canon_epoch_stamp(void) { return s_epoch_now; }
int  spyro_native_canon_epoch_fresh(uint32_t stamp) { return stamp == s_epoch_ok; }
int  spyro_native_canon_mobyreg_fresh(void) { return s_mobyreg_epoch == s_epoch_ok; }
int  spyro_native_canon_shaded_fresh(void) { return 0; }    /* no second material pass */

/* The moby cull records live in the draw scratch this tick carved out of the
 * primitive buffer, and the draw passes that follow the moby draw reuse that
 * memory before any sub-tick present runs. So the queue is copied at the moby
 * hook and staged back over its own address for the duration of each
 * intermediate present, which is what makes the replay read the records the
 * tick actually drew rather than whatever overwrote them. */
static uint8_t  s_recs_copy[0x1000];
static uint32_t s_recs_base;
static uint32_t s_recs_len;

/* The player draw's part-transform records live in the same per-tick draw
 * scratch, at a fixed offset the player hook names, and the passes after it
 * reuse those bytes before any sub-tick present runs. Captured and staged the
 * same way as the moby queue, so the player replay decodes the records the
 * tick drew instead of whatever overwrote them.
 *
 * Two slots, not one: the hook captures the part records and the pre-blend
 * colour table in the same tick, and a single slot would leave one of them
 * unstaged. An unstaged span is not merely stale: the replay's two-pass decode
 * writes each rendered pose into the block it reads, and outside the sub-tick
 * view that host pointer is live PSX RAM, so the present would edit gameplay
 * memory. Slots are keyed by the scratch offset the hook named. */
#define S2_RANGE_SLOTS 2
static struct {
    uint8_t  copy[0x200];
    uint32_t off, base, len, epoch;
} s_range[S2_RANGE_SLOTS];

void spyro_native_canon_capture_range(void* cpu_v, unsigned off, unsigned len) {
    CPUState* cpu = (CPUState*)cpu_v;
    int slot = -1;
    for (int i = 0; i < S2_RANGE_SLOTS; i++)
        if (s_range[i].len && s_range[i].off == off) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < S2_RANGE_SLOTS; i++) if (!s_range[i].len) { slot = i; break; }
    if (slot < 0) return;
    s_range[slot].len = 0;
    if (!len || len > sizeof s_range[slot].copy || !game_layout.polybuf.end) return;
    uint32_t end = cpu->read_word(game_layout.polybuf.end);
    if (end < 0x80000000u) return;
    uint32_t base = end - game_layout.player.part_base_polybuf_off + off;
    const void* host = openpete_psx_to_host_ptr(base);
    if (!host) return;
    memcpy(s_range[slot].copy, host, len);
    s_range[slot].off   = off;
    s_range[slot].base  = base;
    s_range[slot].len   = len;
    s_range[slot].epoch = s_epoch_now;
}

/* Stage every captured range this present. Returns 0 unless all of them are
 * fresh and staged, so a partial stage never reaches the replay. */
static int stage_player_records(void) {
    int staged = 0;
    for (int i = 0; i < S2_RANGE_SLOTS; i++) {
        if (!s_range[i].len) continue;
        if (s_range[i].epoch != s_epoch_ok) return 0;
        void* stage = openpete_subtick_view_stage(s_range[i].base, s_range[i].len);
        if (!stage) return 0;
        memcpy(stage, s_range[i].copy, s_range[i].len);
        staged++;
    }
    return staged;
}
void spyro_native_canon_capture_recs(void* cpu) {
    (void)cpu;
    s_recs_len = 0;
    uint32_t base = moby_canon_rec_base();
    uint32_t len  = moby_canon_rec_bytes();
    if (!base || !len || len > sizeof s_recs_copy) return;
    const void* host = openpete_psx_to_host_ptr(base);
    if (!host) return;
    memcpy(s_recs_copy, host, len);
    s_recs_base = base;
    s_recs_len  = len;
    s_mobyreg_epoch = s_epoch_now;
}

/* Stage the captured queue for this present. Returns 0 when there is none, so
 * the caller skips the replay rather than decoding whatever now lives there. */
static int stage_moby_records(void) {
    if (!s_recs_len || s_mobyreg_epoch != s_epoch_ok) return 0;
    void* stage = openpete_subtick_view_stage(s_recs_base, s_recs_len);
    if (!stage) return 0;
    memcpy(stage, s_recs_copy, s_recs_len);
    return 1;
}

/* The player draw's projected-SXY block is not staged on this game: the player
 * walk decodes the captured part records instead of reading the block back. */
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

/* Replay every player call the canonical tick recorded, not just the avatar.
 *
 * The pass driver draws the avatar twice on a tick over a reflective surface:
 * a mirrored copy first, with the height above the surface in the state word,
 * the world Z moved to twice that height below it and its own ordering bias
 * in the sort byte, then the avatar with those three fields restored
 * (asm/nonmatchings/drawutil/func_8001EC5C.s:39-90). Live gamestate at present
 * time therefore describes neither call: the driver put it back. The record
 * store holds each call's own state block, so the loop stages that block into
 * the sub-tick view and runs the walk once per record, with the mirrored
 * verdict taken from the record rather than re-read. Reproducing the copy from
 * the capture is what makes it draw on an intermediate present; without it the
 * reflection appears only on canonical ticks and vanishes at any present rate
 * above the tick rate.
 *
 * Host-only: the staged span is a view copy, restored to the ambient bytes at
 * the end, and no guest word is written. */
/* The player's ground shadow, at its own place in the record order: the shadow
 * feeder reads one state block, the record store holds the block each call
 * drew with, so the loop stages that block over the ambient bytes and runs the
 * fan once per record. The anchor is bracketed against the previous tick's,
 * teleport-guarded like the camera, so the shadow slides with the avatar
 * between ticks instead of stepping with it.
 *
 * Host-only: the staged span is a view copy and no guest word is written. */
static void player_record_shadow(CPUState* cpu, int k, uint8_t* shb, double rec_alpha) {
    if (!shb) return;
    const uint32_t len = game_layout.player.shadow_len;
    int nsh = player_records_shadow_count();
    for (int j = 0; j < nsh; j++) {
        const player_shadow_rec_t* d = player_records_shadow_curr(j);
        if (!d || d->model != k || !d->drawn) continue;
        const player_shadow_rec_t* pd = player_records_shadow_prev(j);
        memcpy(shb, d->sh, len);
        if (player_records_lerps(k) && pd) {
            const int32_t* pp = (const int32_t*)(pd->src + SHADOW_POS_OFF);
            const int32_t* cp = (const int32_t*)(d->src + SHADOW_POS_OFF);
            double dx = (double)cp[0] - pp[0], dy = (double)cp[1] - pp[1], dz = (double)cp[2] - pp[2];
            if (dx*dx + dy*dy + dz*dz <= 5000.0 * 5000.0)
                lerp_vec3(shb, pd->src, d->src, SHADOW_POS_OFF, rec_alpha);
        }
        shadow_player_reextract(cpu, k);
        player_records_shadow_note(j, (const int32_t*)(shb + SHADOW_POS_OFF));
    }
}

static void player_records_replay(CPUState* cpu, int paired, double alpha) {
    const uint32_t len = game_layout.player.rec_len;
    double rec_alpha = 0.0;
    int n = player_records_present_begin(paired, 0, alpha, &rec_alpha);
    if (n <= 0 || !len || len > PLAYER_REC_STATE_MAX) {
        player_records_present_end(); return;
    }
    uint8_t* sb = (uint8_t*)openpete_subtick_view_stage(game_layout.player.base, len);
    if (!sb) { player_records_present_end(); return; }

    static uint8_t ambient[PLAYER_REC_STATE_MAX];
    memcpy(ambient, sb, len);

    /* The shadow feeder's own state block, staged beside the player's for the
     * same reason: one block serves every record in turn. */
    const uint32_t shlen = game_layout.player.shadow_len;
    uint8_t* shb = NULL;
    static uint8_t sh_ambient[PLAYER_REC_AUX_MAX];
    if (game_layout.player.shadow_base && shlen && shlen <= PLAYER_REC_AUX_MAX &&
        player_records_shadow_count() > 0) {
        shb = (uint8_t*)openpete_subtick_view_stage(game_layout.player.shadow_base, shlen);
        if (shb) memcpy(sh_ambient, shb, shlen);
    }
    const uint32_t pos_off = game_layout.player.pos_off;
    for (int k = 0; k < n; k++) {
        const player_model_rec_t* rec = player_records_curr(k);
        if (!rec) break;
        if (!rec->drawn) { player_records_present_note(k, (const int32_t*)rec->spyro); continue; }
        memcpy(sb, rec->spyro, len);

        /* Position and rotation between the two ticks this record pairs.
         * Without it the avatar holds its tick pose while the camera glides,
         * so every present above the tick rate shows the same 30 Hz step the
         * camera bracket exists to remove. The record store pairs each call
         * with its own previous one, so a mirrored copy brackets against the
         * previous tick's mirrored copy rather than against the avatar.
         *
         * The position goes in twice: as bytes, for the provider reads that
         * take the state block (the water plane, the mirrored copy's height),
         * and as the un-rounded double the part-0 transform prefers, which is
         * what keeps the avatar off the one-world-unit grid.
         *
         * Rotation is handed over as float byte-angles rather than written
         * back into the packed euler, because the euler is 8 bits per axis:
         * rounding the interpolated angle into it reproduces the 1.4-degree
         * step it is meant to subdivide. The shortest arc is taken per axis,
         * so a wrap through 0 does not spin the model the long way. */
        const player_model_rec_t* prv = player_records_prev(k);
        const int lerp = player_records_lerps(k) && prv != NULL;
        int    rot_active[PLAYER_PARTS_MAX] = {0};
        double rot_ang[PLAYER_PARTS_MAX][3] = {{0}};
        s_ppos_active = 0;
        if (lerp) {
            const uint8_t* psp = prv->src;
            const uint8_t* csp = rec->src;
            lerp_vec3(sb, psp, csp, pos_off, rec_alpha);
            const int32_t* pp = (const int32_t*)(psp + pos_off);
            const int32_t* cp = (const int32_t*)(csp + pos_off);
            for (int i = 0; i < 3; i++)
                s_ppos[i] = (double)pp[i] + ((double)cp[i] - (double)pp[i]) * rec_alpha;
            s_ppos_active = 1;
            for (int s = 0; s < PLAYER_PARTS_MAX; s++) {
                uint32_t off = game_layout.player.rot_off[s];
                if (!off) continue;
                uint32_t pr, cr;
                memcpy(&pr, psp + off, 4); memcpy(&cr, csp + off, 4);
                /* A zero word is the provider's "this part does not rotate
                 * this call" sentinel; interpolating through it would swing
                 * the part to the identity and back. */
                if (s != 0 && (pr == 0 || cr == 0)) continue;
                for (int a = 0; a < 3; a++) {
                    int pbk = (int)((pr >> (a * 8)) & 0xFFu);
                    int cbk = (int)((cr >> (a * 8)) & 0xFFu);
                    int d = cbk - pbk;
                    if (d > 128) d -= 256; else if (d < -128) d += 256;
                    rot_ang[s][a] = (double)pbk + (double)d * rec_alpha;
                }
                rot_active[s] = 1;
            }
        }
        player_native_set_rot_interp(lerp ? rot_active : NULL, &rot_ang[0][0]);
        /* The keyframe pose, on the same bracket as the position and rotation
         * above. The part records are composed from gamestate, and the record
         * store holds each call's composed block, so the walk decodes the two
         * poses the two ticks really rendered and lerps the projected
         * vertices. Without it the skeleton translates and turns smoothly
         * while its vertices hold the tick's keyframe blend, so the outline
         * steps at the tick rate under a smooth centroid. An unbracketed
         * present or an unpaired record leaves it off, which draws the tick's
         * own pose. */
        player_native_set_anim_interp(rec_alpha, lerp && paired);
        player_native_set_anim_pair((lerp && prv->drawn) ? prv->seg : NULL, rec->seg);
        player_native_set_player_slot(k);
        player_reextract_record(cpu, !rec->is_player);
        player_record_shadow(cpu, k, shb, rec_alpha);
        player_records_present_note(k, (const int32_t*)sb);
    }
    if (shb) memcpy(shb, sh_ambient, shlen);
    s_ppos_active = 0;
    player_native_set_rot_interp(NULL, NULL);
    player_native_set_anim_pair(NULL, NULL);
    player_native_set_anim_interp(0.0, 0);
    memcpy(sb, ambient, len);
    player_records_present_end();
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
    /* The tick side of the bracket, kept for the moby replay: the cull composed
     * every animated record against this camera, so the replay needs it to turn
     * those records onto the interpolated one. */
    const uint8_t* tickcam = NULL;
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
            tickcam = ccam;
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
    portal_s23_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* homeworld portal previews */
    RX_CENSUS_STEP("portal");
    if (stage_moby_records()) {
        /* The cull stores each animated record already composed against the
         * tick's camera, in view space; hand the replay that camera so it can
         * re-base onto the staged one. Cleared after the pass, so the canonical
         * inline render replays the records untouched. */
        moby_native_set_tick_camera(tickcam ? (const int16_t*)(tickcam + CAM_OFF_SKY) : NULL,
                                    tickcam ? (const int32_t*)(tickcam + CAM_OFF_POS) : NULL);
        /* Arm the per-object sub-tick clock for this feeder pass: it is the
         * timebase the keyframe crossfade reads, so without it every replayed
         * record holds the tick's mesh while its translation sweeps. Reset
         * after the pass so the canonical inline render stays byte-exact. */
        moby_native_set_rot_interp(1);
        moby_reextract(cpu);        /* the 3D actors, after the terrain they stand on */
        moby_native_set_rot_interp(0);
        moby_native_set_tick_camera(NULL, NULL);
    }
    RX_CENSUS_STEP("moby");
    if (stage_player_records())
        player_records_replay(cpu, bracketed, alpha);   /* every player call the tick drew */
    RX_CENSUS_STEP("player");
    shadow_reextract(cpu);          /* drop-shadows under mobys */
    RX_CENSUS_STEP("shadow");
    particles_reextract(cpu);       /* the particle primitives the tick captured */
    RX_CENSUS_STEP("particles");
    flame_records_present(cpu, bracketed, 0, bracketed ? alpha : 0.0, NULL);
    RX_CENSUS_STEP("flame");
    /* The HUD, dialog boxes, text and screen fades are produced during the
     * game's own render and cannot be re-derived here, so the canonical present
     * banks them and every re-extract re-applies that carry on top of the
     * re-derived 3D scene (fade interpolated by the alpha, held panels
     * re-emitted as captured). Without it a sub-tick present draws the world
     * with no 2D layer at all. */
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
