// spyro_records.c — per-call records of the player render hooks (P0: the
// OPENPETE_SPYRO_RECDBG instrument). See spyro_records.h and the plan.
//
// Determinism: reads guest RAM at the hooks and the interp ring at commit;
// never writes either. Inert (two branches per hook) unless
// OPENPETE_SPYRO_RECDBG is set.

#include "spyro_records.h"
#include "psx_runtime_internal.h"   /* CPUState */
#include "openpete_psx.h"           /* openpete_psx_to_host_ptr */
#include "openpete_interp.h"        /* ring: curr_tick_ns, curr_snapshot */
#include "openpete_fiber.h"         /* openpete_frame, openpete_speculative */
#include "spyro1_symbols.gen.h"     /* G_SPYRO, G_SPYROFLAME, D_SPYROSHADOW */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Region bytes each feeder's record carries, and the DIRECT ring region that
 * holds the same bytes at tick end (index in s_spyro_interp_regions,
 * spyro_interp_regions.c: g_Camera 0, g_Spyro 1, g_SpyroFlame 2,
 * g_SpyroShadow 3). */
static const struct { const char* name; uint32_t addr; uint32_t len; unsigned ring; } s_reg[SPYRO_REC_FEEDERS] = {
    { "model",  G_SPYRO,       0x2A4u, 1u },
    { "shadow", D_SPYROSHADOW, 0x28u,  3u },
    { "flame",  G_SPYROFLAME,  0xD0u,  2u },
};
#define REC_OBS_MAX   16
#define REC_BYTES_MAX 0x2A4u

typedef struct {
    int      n, over;                  /* calls observed; calls past REC_OBS_MAX */
    int32_t  pos[REC_OBS_MAX][3];      /* g_Spyro m_Position at each call (the player identity) */
    uint8_t  cf[REC_OBS_MAX];          /* g_Spyro+0x2B: m_colorFilter.interp (Fairy-Kiss pre-blend) */
    uint8_t  last[REC_BYTES_MAX];      /* the region bytes at the LAST call */
    int      last_ok;
} feeder_obs_t;

static feeder_obs_t s_obs[SPYRO_REC_FEEDERS];   /* the epoch being observed */
static feeder_obs_t s_prev_model;               /* previous epoch's model set */
static uint32_t     s_spec_refused_epoch;

/* Run totals. */
static unsigned s_epochs, s_spec_refused, s_roll_ring, s_roll_records, s_roll_both, s_dedupe;
static unsigned s_mismatch[SPYRO_REC_FEEDERS];
static unsigned s_no_ring;                      /* commits before the ring held a slot */
static int      s_max_calls[SPYRO_REC_FEEDERS];
static int      s_max_epochs_vbl, s_epochs_this_vbl;
static uint32_t s_last_commit_frame = 0xFFFFFFFFu;
static op_time_t s_latched_ns;
static double   s_maxB_single = 0.0, s_maxB_multi = 0.0, s_minsep = -1.0, s_max_model_delta = 0.0;

int spyro_records_dbg(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_SPYRO_RECDBG"); on = (e && *e && *e != '0'); }
    return on;
}

static double dist3(const int32_t a[3], const int32_t b[3]) {
    double dx = (double)a[0] - b[0], dy = (double)a[1] - b[1], dz = (double)a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

void spyro_records_hook_observe(int feeder, struct CPUState* cpu) {
    (void)cpu;
    if (!spyro_records_dbg()) return;
    if (feeder < 0 || feeder >= SPYRO_REC_FEEDERS) return;
    /* Runahead lookahead: the hook fires on speculative ticks too; those
     * fires are not the canonical epoch (scratch_row_capture refuses them
     * the same way). */
    if (openpete_speculative) { s_spec_refused_epoch++; return; }
    feeder_obs_t* o = &s_obs[feeder];
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
    const uint8_t* rb = (const uint8_t*)openpete_psx_to_host_ptr(s_reg[feeder].addr);
    if (!sp || !rb) return;
    if (o->n < REC_OBS_MAX) { memcpy(o->pos[o->n], sp, 12); o->cf[o->n] = sp[0x2B]; o->n++; }
    else o->over++;
    memcpy(o->last, rb, s_reg[feeder].len);
    o->last_ok = 1;
}

static void summary(void) {
    fprintf(stderr,
        "[recdbg-summary] epochs=%u no_ring=%u max_calls m=%d s=%d f=%d spec_refused=%u "
        "max_epochs_per_vbl=%d roll_ring=%u roll_records=%u roll_both=%u dedupe=%u "
        "mismatch m=%u s=%u f=%u max_model_pos_delta=%.0f "
        "maxB n=1:%.0f n>=2:%.0f minsep=%.0f\n",
        s_epochs, s_no_ring, s_max_calls[0], s_max_calls[1], s_max_calls[2], s_spec_refused,
        s_max_epochs_vbl, s_roll_ring, s_roll_records, s_roll_both, s_dedupe,
        s_mismatch[0], s_mismatch[1], s_mismatch[2], s_max_model_delta,
        s_maxB_single, s_maxB_multi, s_minsep);
}

void spyro_records_epoch_commit(void) {
    if (!spyro_records_dbg()) return;
    static int armed = 0;
    if (!armed) { armed = 1; atexit(summary); }
    s_epochs++;

    /* Epochs per VBL: the commit runs once per canonical present. */
    if (openpete_frame == s_last_commit_frame) s_epochs_this_vbl++;
    else { s_epochs_this_vbl = 1; s_last_commit_frame = openpete_frame; }
    if (s_epochs_this_vbl > s_max_epochs_vbl) s_max_epochs_vbl = s_epochs_this_vbl;

    /* Did the DIRECT ring roll since the previous commit? Its newest stamp
     * advances on every stored (post-dedupe) capture. */
    op_time_t ns = openpete_interp_curr_tick_ns();
    int ring_rolled = (ns != 0 && ns != s_latched_ns);
    s_latched_ns = ns;

    /* Did the model record set change (count, order or bytes)? */
    feeder_obs_t* m = &s_obs[SPYRO_REC_MODEL];
    int rec_changed = (m->n != s_prev_model.n) || (m->last_ok != s_prev_model.last_ok) ||
                      memcmp(m->pos, s_prev_model.pos, sizeof m->pos) != 0 ||
                      (m->last_ok && memcmp(m->last, s_prev_model.last, s_reg[SPYRO_REC_MODEL].len) != 0);
    if (ring_rolled && rec_changed) s_roll_both++;
    else if (ring_rolled)           s_roll_ring++;
    else if (rec_changed)           s_roll_records++;
    else                            s_dedupe++;

    /* Hook bytes vs the ring's tick-end bytes, per region. */
    const openpete_interp_snapshot_t* curr = openpete_interp_curr_snapshot();
    int mm[SPYRO_REC_FEEDERS] = { 0, 0, 0 };
    double model_delta = 0.0;
    if (!curr) s_no_ring++;
    for (int f = 0; f < SPYRO_REC_FEEDERS; f++) {
        if (!curr || !s_obs[f].last_ok) continue;
        const uint8_t* ring = curr->data + curr->region_offsets[s_reg[f].ring];
        mm[f] = memcmp(s_obs[f].last, ring, s_reg[f].len) != 0;
        if (mm[f]) s_mismatch[f]++;
        if (f == SPYRO_REC_MODEL && mm[f]) {
            int32_t rp[3]; memcpy(rp, ring, 12);
            model_delta = dist3(s_obs[f].pos[s_obs[f].n - 1], rp);
            if (model_delta > s_max_model_delta) s_max_model_delta = model_delta;
        }
    }

    /* Pairing bound B: per-tick displacement of each index-matched model
     * call; formation spacing: the closest two calls of this epoch. */
    double B = 0.0, sep = -1.0;
    if (m->n > 0 && m->n == s_prev_model.n) {
        for (int k = 0; k < m->n; k++) {
            double d = dist3(m->pos[k], s_prev_model.pos[k]);
            if (d > B) B = d;
        }
        if (m->n == 1) { if (B > s_maxB_single) s_maxB_single = B; }
        else           { if (B > s_maxB_multi)  s_maxB_multi  = B; }
    }
    for (int i = 0; i < m->n; i++)
        for (int j = i + 1; j < m->n; j++) {
            double d = dist3(m->pos[i], m->pos[j]);
            if (sep < 0.0 || d < sep) sep = d;
        }
    if (sep >= 0.0 && (s_minsep < 0.0 || sep < s_minsep)) s_minsep = sep;

    for (int f = 0; f < SPYRO_REC_FEEDERS; f++)
        if (s_obs[f].n + s_obs[f].over > s_max_calls[f]) s_max_calls[f] = s_obs[f].n + s_obs[f].over;
    s_spec_refused += s_spec_refused_epoch;

    fprintf(stderr, "[recdbg f=%u e=%u] calls m=%d s=%d f=%d spec=%u epochs_vbl=%d ring=%d recchg=%d "
            "mm=%d/%d/%d mdelta=%.0f B=%.0f sep=%.0f pos=",
            openpete_frame, s_epochs, m->n + m->over, s_obs[1].n + s_obs[1].over, s_obs[2].n + s_obs[2].over,
            s_spec_refused_epoch, s_epochs_this_vbl, ring_rolled, rec_changed,
            mm[0], mm[1], mm[2], model_delta, B, sep);
    for (int k = 0; k < m->n; k++)
        fprintf(stderr, "%s(%d,%d,%d)c%u", k ? ";" : "", m->pos[k][0], m->pos[k][1], m->pos[k][2], m->cf[k]);
    fputc('\n', stderr);

    /* Close the epoch. */
    s_prev_model = *m;
    memset(s_obs, 0, sizeof s_obs);
    s_spec_refused_epoch = 0;
}
