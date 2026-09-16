/* games/spyro-3/renderer/reextract.c — Spyro 3's sub-tick re-extract
 * orchestrator: on every present between two canonical ticks it installs the
 * camera transform interpolated between the interp ring's prev and curr
 * snapshots (region 0) into the sub-tick view, and re-runs the terrain
 * feeder against it. The staged bytes are what every feeder read resolves to
 * for the duration of the present; guest RAM is never written.
 *
 * Provides the renderer/orchestrator.h entries: the fractional camera
 * position for the projection's floor fix, and the two per-present commits
 * (no canonical-render epoch or clear-colour sync exists on this game yet). */
#include "renderer/orchestrator.h"
#include "renderer/terrain.h"
#include "renderer/cyclorama.h"
#include "renderer/portal.h"
#include "renderer/shadow.h"
#include "renderer/moby.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "openpete_interp.h"
#include "openpete_interp_objs.h"      /* g_openpete_tick_period_ns */
#include "openpete_native_render.h"
#include "openpete_subtick_view.h"
#include "openpete_pace_log.h"
#include "psx_runtime_internal.h"
#include "game_layout.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#define CAM_OFF_SKY   0x00u
#define CAM_OFF_VIEW  0x14u
#define CAM_OFF_POS   0x28u

static int16_t lerp16(int16_t a, int16_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}
static int32_t lerp32(int32_t a, int32_t b, double t) {
    double v = (double)a + ((double)b - (double)a) * t;
    return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}
/* A 3x3 int16 rotation matrix (9 shorts; the 10th is pad and stays curr).
 * One tick apart the endpoints differ by a tiny rotation, so a componentwise
 * lerp is sub-pixel. */
static void lerp_matrix(uint8_t* live, const uint8_t* prev, const uint8_t* curr, uint32_t off, double t) {
    const int16_t* pp = (const int16_t*)(prev + off);
    const int16_t* pc = (const int16_t*)(curr + off);
    int16_t*       pl = (int16_t*)(live + off);
    for (int i = 0; i < 9; ++i) pl[i] = lerp16(pp[i], pc[i], t);
}
static void lerp_vec3(uint8_t* live, const uint8_t* prev, const uint8_t* curr, uint32_t off, double t) {
    const int32_t* pp = (const int32_t*)(prev + off);
    const int32_t* pc = (const int32_t*)(curr + off);
    int32_t*       pl = (int32_t*)(live + off);
    for (int i = 0; i < 3; ++i) pl[i] = lerp32(pp[i], pc[i], t);
}

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
 * fresh, so a sub-tick pass replays only what the canonical present drew. A
 * pause or load screen that runs no moby draw leaves that producer stale and
 * its replay is skipped, instead of painting the last gameplay tick's set over
 * a frozen backdrop. */
static uint32_t s_epoch_now = 1;   /* the stamp captures take (present in flight) */
static uint32_t s_epoch_ok;        /* the stamp a capture must carry to be fresh */
static uint32_t s_prod_epoch[3];

void spyro_native_canon_epoch_commit(void) {
    s_epoch_ok = s_epoch_now;
    s_epoch_now++;
}
uint32_t spyro_native_canon_epoch_stamp(void) { return s_epoch_now; }
int  spyro_native_canon_epoch_fresh(uint32_t stamp) { return stamp == s_epoch_ok; }
int  spyro_native_canon_mobyreg_fresh(void) { return s_prod_epoch[SPYRO_PROD_MOBYREG] == s_epoch_ok; }
int  spyro_native_canon_shaded_fresh(void) { return 0; }   /* no second material pass */
/* The moby cull records live in the draw scratch this tick carved out of the
 * polybuf, and the draw passes that follow the moby draw reuse that memory
 * before any sub-tick present runs — measured: the queue's leading records read
 * back as unrelated words one VBL later. So the queue is copied at the moby
 * hook and staged back over its own address for the duration of each
 * intermediate present, which is what makes the replay read the records the
 * tick actually drew. Nothing else this game's feeders read is transient, so
 * capture_range has nothing to do. */
static uint8_t  s_recs_copy[0x1000];
static uint32_t s_recs_base;
static uint32_t s_recs_len;

void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len) {
    (void)cpu; (void)off; (void)len;
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
}

/* Stage the captured queue for this present. Returns 0 when there is none, so
 * the caller skips the replay rather than reading whatever now lives there. */
static int stage_moby_records(void) {
    if (!s_recs_len) return 0;
    void* stage = openpete_subtick_view_stage(s_recs_base, s_recs_len);
    if (!stage) return 0;
    memcpy(stage, s_recs_copy, s_recs_len);
    return 1;
}
void spyro_native_canon_capture_camera(void* cpu, int producer) {
    (void)cpu;
    if ((unsigned)producer < sizeof s_prod_epoch / sizeof s_prod_epoch[0])
        s_prod_epoch[producer] = s_epoch_now;
}
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

static void game_reextract_body(CPUState* cpu) {
    (void)cpu;
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
        const uint8_t* pcam = prev->data + prev->region_offsets[0];
        const uint8_t* ccam = curr->data + curr->region_offsets[0];
        if (!camera_cut(pcam, ccam)) {
            double period = (double)(curr->tick_ns - prev->tick_ns);
            if (period <= 0.0 || period > 4.0 * (double)g_openpete_tick_period_ns)
                period = (double)g_openpete_tick_period_ns;
            alpha = (double)(openpete_interp_present_now() - curr->tick_ns) / period;
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
    openpete_native_scene_begin();
    cyclorama_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* sky dome + background */
    terrain_reextract(cpu, bracketed ? alpha : 0.0, bracketed);
    portal3_reextract(cpu, bracketed ? alpha : 0.0, bracketed);   /* homeworld portal previews */
    if (stage_moby_records())
        moby_reextract(cpu);        /* the 3D actors, after the terrain they stand on */
    shadow_reextract(cpu);          /* drop-shadows under mobys */
    s_fpos_active = 0;
}

static void game_reextract(void* cpu_v) {
    CPUState* cpu = (CPUState*)cpu_v;
    openpete_subtick_view_begin(cpu);
    game_reextract_body(cpu);
    openpete_subtick_view_end(cpu);
}

void game_reextract_init(void) {
    openpete_native_register_reextract(game_reextract);
}
