// games/spyro-1/renderer/spyro_records.c — per-call records of the player
// render hooks: the model-record store, the shadow and flame records bound
// to it and the OPENPETE_SPYRO_RECDBG instrument. See spyro_records.h.
//
// Determinism: the hooks read guest RAM into host copies on the game fiber;
// the commit reads the interp ring; the present side hands the copies to the
// re-extract, which stages them through the sub-tick view. Nothing here
// writes guest RAM.

#include "renderer/spyro_records.h"
#include "psx_runtime_internal.h"   /* CPUState */
#include "openpete_psx.h"           /* openpete_psx_to_host_ptr */
#include "openpete_interp.h"        /* ring: curr_tick_ns, curr_snapshot, reset generation, deterministic */
#include "openpete_fiber.h"         /* openpete_frame, openpete_speculative */
#include "openpete.h"               /* openpete_runahead_get */
#include "renderer/reextract.h" /* canon epoch stamp / fresh */
#include "spyro1_symbols.gen.h"     /* G_SPYRO, G_SPYROFLAME, D_SPYROSHADOW, G_SCRATCH */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern op_time_t g_openpete_tick_stamp;      /* per-VBL stamp (PLL); openpete_interp_objs.c */
extern op_time_t g_openpete_tick_period_ns;  /* one wall-time tick (--tick-scale aware) */

#define SEG_SCRATCH  (G_SCRATCH + 0x1900u)
#define FK_SCRATCH   (G_SCRATCH + 0x1D00u)
#define SXY_BASE     0x1F800000u

/* ---- the epoch's list (game fiber) ----------------------------------- */

typedef struct {
    int      n;
    uint32_t stamp;     /* canon epoch of the first append */
    uint32_t frame;     /* openpete_frame of the first append */
    spyro_model_rec_t r[SPYRO_REC_MAX];
    int      nsh;       /* shadow records, each bound to a model record */
    spyro_shadow_rec_t sh[SPYRO_REC_MAX];
    int      nfl;       /* flame records, the same */
    spyro_flame_rec_t  fl[SPYRO_REC_MAX];
} rec_list_t;

static rec_list_t s_list;             /* accumulating: the committed tick's hook calls */
static rec_list_t s_led;              /* accumulating: the speculative fires of one led frame (runahead) */
static uint32_t   s_cap_refused_epoch, s_spec_refused_epoch, s_led_fires_epoch;

/* The list is keyed by (epoch stamp, VBL): the stamp advances only at a
 * commit, which the --skip-to-level fast-forward and CD stalls skip, so a
 * stamp-only key would accumulate a fast-forward's calls to the cap. */
static void list_open(void) {
    uint32_t stamp = spyro_native_canon_epoch_stamp();
    int empty = (s_list.n == 0 && s_list.nsh == 0 && s_list.nfl == 0);
    if (!empty && (s_list.stamp != stamp || s_list.frame != openpete_frame)) { s_list.n = 0; s_list.nsh = 0; s_list.nfl = 0; empty = 1; }
    if (empty) { s_list.stamp = stamp; s_list.frame = openpete_frame; }
}

/* The led list: one speculative frame's fires (runahead runs 2N speculative
 * VBLs after the committed tick; the last draw among them is the ring's
 * capture tick). A new frame starts a new list. */
static void led_open(void) {
    if (s_led.frame != openpete_frame) { s_led.n = 0; s_led.nsh = 0; s_led.nfl = 0; s_led.frame = openpete_frame; }
    s_led_fires_epoch++;
}


/* ---- the store (three deep, one clock) ------------------------------- */

typedef struct {
    rec_list_t list;
    op_time_t  ns;       /* the store clock stamp of this list */
    int        valid;
} store_slot_t;

static store_slot_t s_store[3];       /* [0] curr, [1] prev, [2] prev2 */
static uint32_t     s_store_epoch;    /* the canon epoch curr was confirmed at (roll or dedupe) */
static op_time_t    s_latched_ring_ns;
static uint32_t     s_reset_gen_seen;

/* ---- OPENPETE_SPYRO_RECDBG instrument ----------------------------------- */

static const struct { const char* name; uint32_t addr; uint32_t len; unsigned ring; } s_reg[SPYRO_REC_FEEDERS] = {
    { "model",  G_SPYRO,       0x2A4u, 1u },   /* ring region indices: interp_regions.c */
    { "shadow", D_SPYROSHADOW, 0x28u,  3u },
    { "flame",  G_SPYROFLAME,  0xD0u,  2u },
};
#define REC_OBS_MAX   16
#define REC_BYTES_MAX 0x2A4u

typedef struct {
    int      n, over;
    int32_t  pos[REC_OBS_MAX][3];
    uint8_t  cf[REC_OBS_MAX];
    uint8_t  last[REC_BYTES_MAX];
    int      last_ok;
} feeder_obs_t;

static feeder_obs_t s_obs[SPYRO_REC_FEEDERS];
static feeder_obs_t s_prev_model;

static unsigned s_epochs, s_spec_refused, s_cap_refused, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets;
static unsigned s_mismatch[SPYRO_REC_FEEDERS];
static unsigned s_no_ring, s_pairing_only, s_unpaired, s_pass2;
static unsigned s_led_fires, s_led_match, s_led_miss;   /* runahead: speculative fires; led lists applied / not */
static int      s_max_calls[SPYRO_REC_FEEDERS];
static int      s_max_epochs_vbl, s_epochs_this_vbl;
static uint32_t s_last_commit_frame = 0xFFFFFFFFu;
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
static void rec_pos(const spyro_model_rec_t* r, int32_t out[3]) { memcpy(out, r->spyro, 12); }

/* ---- hooks ------------------------------------------------------------- */

static void observe(int feeder) {
    feeder_obs_t* o = &s_obs[feeder];
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
    const uint8_t* rb = (const uint8_t*)openpete_psx_to_host_ptr(s_reg[feeder].addr);
    if (!sp || !rb) return;
    if (o->n < REC_OBS_MAX) { memcpy(o->pos[o->n], sp, 12); o->cf[o->n] = sp[0x2B]; o->n++; }
    else o->over++;
    memcpy(o->last, rb, s_reg[feeder].len);
    o->last_ok = 1;
}

int spyro_records_model_append(struct CPUState* cpu, uint32_t rp_start, uint32_t rp_end) {
    (void)cpu;
    /* Runahead lookahead: the hook fires on speculative ticks too. Those
     * fires are not the canonical epoch (scratch_row_capture refuses them
     * the same way) — they build the led list, the lerp source of the
     * committed records at commit. The singleton's seg pair rolled on
     * speculative fires (unguarded, unlike the registry rows), so its shape
     * blend was between LED poses, matching the ring's led anim clocks;
     * the led list's seg row keeps that. */
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (spyro_records_dbg()) observe(SPYRO_REC_MODEL); list_open(); }
    if (L->n >= SPYRO_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp  = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
    const uint8_t* seg = (const uint8_t*)openpete_psx_to_host_ptr(SEG_SCRATCH);
    const uint8_t* fk  = (const uint8_t*)openpete_psx_to_host_ptr(FK_SCRATCH);
    const uint8_t* sxy = (const uint8_t*)openpete_psx_to_host_ptr(SXY_BASE);
    if (!sp || !seg || !fk || !sxy) return -1;
    spyro_model_rec_t* r = &L->r[L->n];
    memcpy(r->spyro, sp,  SPYRO_REC_SPYRO_LEN);
    memcpy(r->src,   sp,  SPYRO_REC_SPYRO_LEN);
    memcpy(r->seg,   seg, SPYRO_REC_SEG_LEN);
    memcpy(r->fk,    fk,  SPYRO_REC_FK_LEN);
    memcpy(r->sxy,   sxy, SPYRO_REC_SXY_LEN);
    r->rp_start = rp_start; r->rp_end = rp_end;
    r->drawn = 1; r->pair = -1; r->pair2 = -1;
    return L->n++;
}

/* The model record drawn at the live g_Spyro position — the latest, the mod
 * calls model, shadow, flame per player (coop_draw.c) — or, when none was
 * (hidden player, respawn blink), a pairing-only model record appended at
 * this call's ordinal: the next drawn model pairs with it and the shadow or
 * flame renders under its g_Spyro fields; never rendered itself. -1 at the
 * cap. */
static int bind_model(rec_list_t* L, const uint8_t* sp) {
    for (int k = L->n - 1; k >= 0; k--)
        if (memcmp(L->r[k].spyro, sp, 12) == 0) return k;
    if (L->n >= SPYRO_REC_MAX) { s_cap_refused_epoch++; return -1; }
    spyro_model_rec_t* r = &L->r[L->n];
    memset(r, 0, sizeof *r);
    memcpy(r->spyro, sp, SPYRO_REC_SPYRO_LEN);
    memcpy(r->src,   sp, SPYRO_REC_SPYRO_LEN);
    r->drawn = 0; r->pair = -1; r->pair2 = -1;
    if (L == &s_list) s_pairing_only++;
    return L->n++;
}

int spyro_records_shadow_append(struct CPUState* cpu) {
    (void)cpu;
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (spyro_records_dbg()) observe(SPYRO_REC_SHADOW); list_open(); }
    if (L->nsh >= SPYRO_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
    const uint8_t* sh = (const uint8_t*)openpete_psx_to_host_ptr(D_SPYROSHADOW);
    if (!sp || !sh) return -1;
    int m = bind_model(L, sp);
    if (m < 0) return -1;
    spyro_shadow_rec_t* d = &L->sh[L->nsh];
    memcpy(d->sh,  sh, SPYRO_REC_SHADOW_LEN);
    memcpy(d->src, sh, SPYRO_REC_SHADOW_LEN);
    d->model = (int8_t)m; d->drawn = 1;
    L->nsh++;
    return m;
}

int spyro_records_flame_append(struct CPUState* cpu) {
    (void)cpu;
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (spyro_records_dbg()) observe(SPYRO_REC_FLAME); list_open(); }
    if (L->nfl >= SPYRO_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYRO);
    const uint8_t* fl = (const uint8_t*)openpete_psx_to_host_ptr(G_SPYROFLAME);
    if (!sp || !fl) return -1;
    int m = bind_model(L, sp);
    if (m < 0) return -1;
    spyro_flame_rec_t* d = &L->fl[L->nfl];
    memcpy(d->fl,  fl, SPYRO_REC_FLAME_LEN);
    memcpy(d->src, fl, SPYRO_REC_FLAME_LEN);
    d->model = (int8_t)m; d->drawn = 1;
    L->nfl++;
    return m;
}

/* ---- commit ------------------------------------------------------------ */

static int lists_differ(const rec_list_t* a, const rec_list_t* b) {
    if (a->n != b->n) return 1;
    for (int k = 0; k < a->n; k++) {
        const spyro_model_rec_t *x = &a->r[k], *y = &b->r[k];
        if (x->drawn != y->drawn) return 1;
        if (memcmp(x->spyro, y->spyro, SPYRO_REC_SPYRO_LEN) != 0) return 1;
        if (x->drawn && (memcmp(x->seg, y->seg, SPYRO_REC_SEG_LEN) != 0 ||
                         memcmp(x->fk,  y->fk,  SPYRO_REC_FK_LEN)  != 0 ||
                         memcmp(x->sxy, y->sxy, SPYRO_REC_SXY_LEN) != 0)) return 1;
    }
    if (a->nsh != b->nsh) return 1;
    for (int j = 0; j < a->nsh; j++)
        if (a->sh[j].model != b->sh[j].model || a->sh[j].drawn != b->sh[j].drawn ||
            memcmp(a->sh[j].sh, b->sh[j].sh, SPYRO_REC_SHADOW_LEN) != 0) return 1;
    if (a->nfl != b->nfl) return 1;
    for (int j = 0; j < a->nfl; j++)
        if (a->fl[j].model != b->fl[j].model || a->fl[j].drawn != b->fl[j].drawn ||
            memcmp(a->fl[j].fl, b->fl[j].fl, SPYRO_REC_FLAME_LEN) != 0) return 1;
    return 0;
}

/* Two passes: index first, position-verified within B; then nearest
 * still-unpaired previous record within B, ties to the lower index. Runs on
 * the host loop's thread and reads no clock. */
static void pair_lists(rec_list_t* cur, const rec_list_t* prv) {
    int taken[SPYRO_REC_MAX] = {0};
    for (int k = 0; k < cur->n; k++) { cur->r[k].pair = -1; cur->r[k].pair2 = -1; }
    if (!prv) { s_unpaired += (unsigned)cur->n; return; }
    /* One record each side: nothing to disambiguate, and the singleton it
     * replaces paired the ring's prev/curr unconditionally (the camera cut
     * guard, not B, decides a teleport there). Keeps the modless k=0 identical. */
    if (cur->n == 1 && prv->n == 1) { cur->r[0].pair = 0; cur->r[0].pair2 = prv->r[0].pair; return; }
    for (int k = 0; k < cur->n && k < prv->n; k++) {
        int32_t a[3], b[3]; rec_pos(&cur->r[k], a); rec_pos(&prv->r[k], b);
        if (dist3(a, b) <= SPYRO_REC_PAIR_B) { cur->r[k].pair = (int8_t)k; taken[k] = 1; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) continue;
        int32_t a[3]; rec_pos(&cur->r[k], a);
        int best = -1; double bd = 0.0;
        for (int j = 0; j < prv->n; j++) {
            if (taken[j]) continue;
            int32_t b[3]; rec_pos(&prv->r[j], b);
            double d = dist3(a, b);
            if (d <= SPYRO_REC_PAIR_B && (best < 0 || d < bd)) { best = j; bd = d; }
        }
        if (best >= 0) { cur->r[k].pair = (int8_t)best; taken[best] = 1; s_pass2++; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) cur->r[k].pair2 = prv->r[cur->r[k].pair].pair;
        else s_unpaired++;
    }
}

static void summary(void) {
    fprintf(stderr,
        "[recdbg-summary] epochs=%u no_ring=%u max_calls m=%d s=%d f=%d spec_refused=%u cap_refused=%u "
        "max_epochs_per_vbl=%d roll_ring=%u roll_records=%u roll_both=%u dedupe=%u resets=%u "
        "pairing_only=%u unpaired=%u pass2=%u led_fires=%u led_match=%u led_miss=%u "
        "mismatch m=%u s=%u f=%u max_model_pos_delta=%.0f "
        "maxB n=1:%.0f n>=2:%.0f minsep=%.0f\n",
        s_epochs, s_no_ring, s_max_calls[0], s_max_calls[1], s_max_calls[2], s_spec_refused, s_cap_refused,
        s_max_epochs_vbl, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets,
        s_pairing_only, s_unpaired, s_pass2, s_led_fires, s_led_match, s_led_miss,
        s_mismatch[0], s_mismatch[1], s_mismatch[2], s_max_model_delta,
        s_maxB_single, s_maxB_multi, s_minsep);
}

void spyro_records_epoch_commit(void) {
    const int dbg = spyro_records_dbg();
    static int armed = 0;
    if (dbg && !armed) { armed = 1; atexit(summary); }
    s_epochs++;

    /* Reset wherever the ring reset (savestate load, rewind, the runahead
     * depth switch, adoption): the store's history describes the pre-jump
     * timeline. */
    uint32_t gen = openpete_interp_reset_generation();
    if (gen != s_reset_gen_seen) {
        s_reset_gen_seen = gen;
        for (int i = 0; i < 3; i++) s_store[i].valid = 0;
        s_latched_ring_ns = 0;
        s_resets++;
    }

    if (openpete_frame == s_last_commit_frame) s_epochs_this_vbl++;
    else { s_epochs_this_vbl = 1; s_last_commit_frame = openpete_frame; }
    if (s_epochs_this_vbl > s_max_epochs_vbl) s_max_epochs_vbl = s_epochs_this_vbl;

    /* This epoch's list: the accumulating list if it was appended under the
     * epoch that just committed, else empty. */
    static rec_list_t epoch;
    if ((s_list.n || s_list.nsh || s_list.nfl) && spyro_native_canon_epoch_fresh(s_list.stamp)) epoch = s_list;
    else { epoch.n = 0; epoch.nsh = 0; epoch.nfl = 0; }
    s_list.n = 0; s_list.nsh = 0; s_list.nfl = 0;

    /* The ring's tick-end dragon and whether it rolled since the last commit. */
    const openpete_interp_snapshot_t* curr = openpete_interp_curr_snapshot();
    op_time_t ns = openpete_interp_curr_tick_ns();
    int ring_rolled = (ns != 0 && ns != s_latched_ring_ns);
    s_latched_ring_ns = ns;
    if (!curr) s_no_ring++;

    /* Pairing-only record: no hook observed the ring's tick-end g_Spyro
     * position — the dragon was not drawn this tick (hidden, blink) or the
     * epoch had no call at all. Appended last, never rendered; shadow and
     * flame records bind to it and the next drawn record pairs with it. */
    if (curr) {
        const uint8_t* ring = curr->data + curr->region_offsets[s_reg[SPYRO_REC_MODEL].ring];
        int seen = 0;
        for (int k = 0; k < epoch.n; k++)
            if (memcmp(epoch.r[k].spyro, ring, 12) == 0) { seen = 1; break; }
        /* --runahead: the hooks fire on the committed tick, the ring holds
         * the led one. The singleton lerped the ring's led position,
         * rotation and clocks over the committed hook's bytes and scratch
         * rows; every record does the same from the led list — the
         * speculative fires of the ring's capture tick, record k's led
         * bytes and seg row as record k's lerp SOURCE and shape, index for
         * index (the same hook sequence N ticks on), the committed bytes
         * staying the staged base (the shadow's silhouette selectors read
         * the base; the golden's camera-rotate-taps encodes that). A led
         * list from another frame or of another shape (a blink, a reseed)
         * leaves the records committed for that tick and is counted. */
        if (openpete_runahead_get() > 0 && epoch.n > 0 && s_led_fires_epoch > 0) {
            /* The led list of this epoch's speculation: fired after the
             * committed tick, no later than the ring's capture (the ring
             * captures at the VBL after the led draw: led=F+1 for the
             * capture at F+2 under --runahead 1). */
            int led_ok = (s_led.frame > epoch.frame &&
                          s_led.frame <= curr->tick && s_led.n == epoch.n);
            for (int k = 0; led_ok && k < epoch.n; k++)
                if (s_led.r[k].drawn != epoch.r[k].drawn) led_ok = 0;
            if (led_ok) {
                for (int k = 0; k < epoch.n; k++) {
                    memcpy(epoch.r[k].src, s_led.r[k].spyro, SPYRO_REC_SPYRO_LEN);
                    if (epoch.r[k].drawn) memcpy(epoch.r[k].seg, s_led.r[k].seg, SPYRO_REC_SEG_LEN);
                }
                if (s_led.nsh == epoch.nsh)
                    for (int j = 0; j < epoch.nsh; j++) memcpy(epoch.sh[j].src, s_led.sh[j].sh, SPYRO_REC_SHADOW_LEN);
                if (s_led.nfl == epoch.nfl)
                    for (int j = 0; j < epoch.nfl; j++) memcpy(epoch.fl[j].src, s_led.fl[j].fl, SPYRO_REC_FLAME_LEN);
                s_led_match++;
            } else s_led_miss++;
            /* The ring's dragon is one of these records, N ticks on: no
             * pairing-only record for it. */
            seen = 1;
        }
        if (!seen && epoch.n < SPYRO_REC_MAX) {
            spyro_model_rec_t* r = &epoch.r[epoch.n++];
            memset(r, 0, sizeof *r);
            memcpy(r->spyro, ring, SPYRO_REC_SPYRO_LEN);
            memcpy(r->src,   ring, SPYRO_REC_SPYRO_LEN);
            r->drawn = 0; r->pair = -1; r->pair2 = -1;
            s_pairing_only++;
        }
        /* The ring's dragon's shadow and flame records when their hooks did
         * not fire this tick: pairing-only, the ring's tick-end bytes (the
         * anchor and the ribbon state are written every tick, drawn or
         * not), so a shadow or flame that appears next tick lerps from
         * them the way the singleton lerped the ring's prev region. Never
         * rendered. */
        int rk = -1;
        for (int k = epoch.n - 1; k >= 0; k--)
            if (memcmp(epoch.r[k].src, ring, 12) == 0) { rk = k; break; }
        if (rk >= 0) {
            int has_sh = 0, has_fl = 0;
            for (int j = 0; j < epoch.nsh; j++) if (epoch.sh[j].model == rk) { has_sh = 1; break; }
            for (int j = 0; j < epoch.nfl; j++) if (epoch.fl[j].model == rk) { has_fl = 1; break; }
            if (!has_sh && epoch.nsh < SPYRO_REC_MAX) {
                spyro_shadow_rec_t* d = &epoch.sh[epoch.nsh++];
                const uint8_t* rb = curr->data + curr->region_offsets[s_reg[SPYRO_REC_SHADOW].ring];
                memcpy(d->sh, rb, SPYRO_REC_SHADOW_LEN); memcpy(d->src, rb, SPYRO_REC_SHADOW_LEN);
                d->model = (int8_t)rk; d->drawn = 0;
            }
            if (!has_fl && epoch.nfl < SPYRO_REC_MAX) {
                spyro_flame_rec_t* d = &epoch.fl[epoch.nfl++];
                const uint8_t* rb = curr->data + curr->region_offsets[s_reg[SPYRO_REC_FLAME].ring];
                memcpy(d->fl, rb, SPYRO_REC_FLAME_LEN); memcpy(d->src, rb, SPYRO_REC_FLAME_LEN);
                d->model = (int8_t)rk; d->drawn = 0;
            }
        }
    }

    /* Roll or dedupe. */
    int rec_changed = !s_store[0].valid || lists_differ(&epoch, &s_store[0].list);
    if (ring_rolled && rec_changed) s_roll_both++;
    else if (ring_rolled)           s_roll_ring++;
    else if (rec_changed)           s_roll_records++;
    else                            s_dedupe++;
    if (ring_rolled || rec_changed) {
        s_store[2] = s_store[1];
        s_store[1] = s_store[0];
        s_store[0].list = epoch;
        s_store[0].valid = 1;
        /* One clock per store: the ring's stamp when it rolled (the store
         * carries the ring's grid exactly, PLL and deterministic alike); a
         * record-only roll stamps the VBL under the PLL and the store's own
         * grid, prev + P, under deterministic capture. */
        if (ring_rolled) s_store[0].ns = ns;
        else if (openpete_interp_deterministic() && s_store[1].valid)
            s_store[0].ns = s_store[1].ns + openpete_interp_det_period();
        else s_store[0].ns = g_openpete_tick_stamp ? g_openpete_tick_stamp : ns;
        pair_lists(&s_store[0].list, s_store[1].valid ? &s_store[1].list : NULL);
    }
    s_store_epoch = spyro_native_canon_epoch_stamp() - 1u;   /* the epoch that just committed */

    /* ---- instrument ---- */
    s_led_fires += s_led_fires_epoch;
    s_spec_refused += s_spec_refused_epoch;
    s_cap_refused  += s_cap_refused_epoch;
    if (dbg) {
        feeder_obs_t* m = &s_obs[SPYRO_REC_MODEL];
        int mm[SPYRO_REC_FEEDERS] = { 0, 0, 0 };
        double model_delta = 0.0;
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
        double B = 0.0, sep = -1.0;
        if (m->n > 0 && m->n == s_prev_model.n) {
            for (int k = 0; k < m->n; k++) { double d = dist3(m->pos[k], s_prev_model.pos[k]); if (d > B) B = d; }
            if (m->n == 1) { if (B > s_maxB_single) s_maxB_single = B; }
            else           { if (B > s_maxB_multi)  s_maxB_multi  = B; }
        }
        for (int i = 0; i < m->n; i++)
            for (int j = i + 1; j < m->n; j++) { double d = dist3(m->pos[i], m->pos[j]); if (sep < 0.0 || d < sep) sep = d; }
        if (sep >= 0.0 && (s_minsep < 0.0 || sep < s_minsep)) s_minsep = sep;
        for (int f = 0; f < SPYRO_REC_FEEDERS; f++)
            if (s_obs[f].n + s_obs[f].over > s_max_calls[f]) s_max_calls[f] = s_obs[f].n + s_obs[f].over;

        fprintf(stderr, "[recdbg f=%u e=%u] calls m=%d s=%d f=%d spec=%u led=%d@%u cap=%u epochs_vbl=%d ring=%d recchg=%d "
                "mm=%d/%d/%d mdelta=%.0f B=%.0f sep=%.0f store=%d pos=",
                openpete_frame, s_epochs, m->n + m->over, s_obs[1].n + s_obs[1].over, s_obs[2].n + s_obs[2].over,
                s_spec_refused_epoch, s_led_fires_epoch ? s_led.n : 0, s_led_fires_epoch ? s_led.frame : 0u,
                s_cap_refused_epoch, s_epochs_this_vbl, ring_rolled, rec_changed,
                mm[0], mm[1], mm[2], model_delta, B, sep, s_store[0].list.n);
        for (int k = 0; k < m->n; k++)
            fprintf(stderr, "%s(%d,%d,%d)c%u", k ? ";" : "", m->pos[k][0], m->pos[k][1], m->pos[k][2], m->cf[k]);
        fprintf(stderr, " pairs=");
        for (int k = 0; k < s_store[0].list.n; k++)
            fprintf(stderr, "%s%d%s", k ? "," : "", s_store[0].list.r[k].pair, s_store[0].list.r[k].drawn ? "" : "p");
        fprintf(stderr, " shadows=");
        for (int j = 0; j < s_store[0].list.nsh; j++)
            fprintf(stderr, "%s%d%s", j ? "," : "", s_store[0].list.sh[j].model, s_store[0].list.sh[j].drawn ? "" : "p");
        fprintf(stderr, " flames=");
        for (int j = 0; j < s_store[0].list.nfl; j++)
            fprintf(stderr, "%s%d%s", j ? "," : "", s_store[0].list.fl[j].model, s_store[0].list.fl[j].drawn ? "" : "p");
        fputc('\n', stderr);
        s_prev_model = *m;
    }
    memset(s_obs, 0, sizeof s_obs);
    s_spec_refused_epoch = 0;
    s_led_fires_epoch = 0;
    s_cap_refused_epoch = 0;
}

/* ---- present side -------------------------------------------------------- */

static int    s_pres_n = 0;
static int    s_pres_lerp = 0;
static double s_pres_alpha = 0.0;

int spyro_records_present_begin(int ring_paired, int ring_cut, double ring_alpha, double* out_alpha) {
    s_pres_n = 0; s_pres_lerp = 0; s_pres_alpha = 0.0;
    if (out_alpha) *out_alpha = 0.0;
    if (!s_store[0].valid) return 0;
    if (!spyro_native_canon_epoch_fresh(s_store_epoch)) return 0;
    s_pres_n = s_store[0].list.n;
    if (ring_paired && !ring_cut && s_store[1].valid) {
        s_pres_lerp = 1;
        if (s_store[0].ns == openpete_interp_curr_tick_ns()) {
            /* The store rolled with the ring: one clock, the ring's alpha. */
            s_pres_alpha = ring_alpha;
        } else {
            op_time_t p = s_store[0].ns - s_store[1].ns;
            if (p > g_openpete_tick_period_ns) p = g_openpete_tick_period_ns;
            double a = (p > 0) ? (double)((int64_t)openpete_interp_present_now() - (int64_t)s_store[0].ns) / (double)p : 1.0;
            if (a < -0.35) a = -0.35; else if (a > 1.0) a = 1.0;
            s_pres_alpha = a;
        }
    }
    if (out_alpha) *out_alpha = s_pres_alpha;
    return s_pres_n;
}

const spyro_model_rec_t* spyro_records_curr(int k) {
    return (k >= 0 && k < s_pres_n) ? &s_store[0].list.r[k] : NULL;
}
const spyro_model_rec_t* spyro_records_prev(int k) {
    const spyro_model_rec_t* c = spyro_records_curr(k);
    if (!c || c->pair < 0 || !s_store[1].valid || c->pair >= s_store[1].list.n) return NULL;
    return &s_store[1].list.r[c->pair];
}
const spyro_model_rec_t* spyro_records_prev2(int k) {
    const spyro_model_rec_t* c = spyro_records_curr(k);
    if (!c || c->pair2 < 0 || !s_store[2].valid || c->pair2 >= s_store[2].list.n) return NULL;
    return &s_store[2].list.r[c->pair2];
}
int spyro_records_lerps(int k) {
    const spyro_model_rec_t* c = spyro_records_curr(k);
    return c && s_pres_lerp && c->pair >= 0;
}
int spyro_records_shadow_count(void) { return s_pres_n ? s_store[0].list.nsh : 0; }
const spyro_shadow_rec_t* spyro_records_shadow_curr(int j) {
    return (s_pres_n && j >= 0 && j < s_store[0].list.nsh) ? &s_store[0].list.sh[j] : NULL;
}
const spyro_shadow_rec_t* spyro_records_shadow_prev(int j) {
    const spyro_shadow_rec_t* d = spyro_records_shadow_curr(j);
    const spyro_model_rec_t* m = d ? spyro_records_curr(d->model) : NULL;
    if (!m || m->pair < 0 || !s_store[1].valid) return NULL;
    const rec_list_t* prv = &s_store[1].list;
    for (int i = 0; i < prv->nsh; i++)
        if (prv->sh[i].model == m->pair) return &prv->sh[i];
    return NULL;
}
void spyro_records_shadow_note(int j, const int32_t* staged_anchor) {
    if (!spyro_records_dbg()) return;
    const spyro_shadow_rec_t* d = spyro_records_shadow_curr(j);
    if (!d) return;
    const spyro_shadow_rec_t* p = spyro_records_shadow_prev(j);
    fprintf(stderr, "[recdbg shadow f=%u j=%d] model=%d drawn=%d lerp=%d anchor=(%d,%d,%d)\n",
            openpete_frame, j, d->model, d->drawn, spyro_records_lerps(d->model) && p != NULL,
            staged_anchor ? staged_anchor[0] : 0, staged_anchor ? staged_anchor[1] : 0, staged_anchor ? staged_anchor[2] : 0);
}
int spyro_records_flame_count(void) { return s_pres_n ? s_store[0].list.nfl : 0; }
const spyro_flame_rec_t* spyro_records_flame_curr(int j) {
    return (s_pres_n && j >= 0 && j < s_store[0].list.nfl) ? &s_store[0].list.fl[j] : NULL;
}
const spyro_flame_rec_t* spyro_records_flame_prev(int j) {
    const spyro_flame_rec_t* d = spyro_records_flame_curr(j);
    const spyro_model_rec_t* m = d ? spyro_records_curr(d->model) : NULL;
    if (!m || m->pair < 0 || !s_store[1].valid) return NULL;
    const rec_list_t* prv = &s_store[1].list;
    for (int i = 0; i < prv->nfl; i++)
        if (prv->fl[i].model == m->pair) return &prv->fl[i];
    return NULL;
}
void spyro_records_flame_note(int j, const int32_t* staged_origin) {
    if (!spyro_records_dbg()) return;
    const spyro_flame_rec_t* d = spyro_records_flame_curr(j);
    if (!d) return;
    const spyro_flame_rec_t* p = spyro_records_flame_prev(j);
    fprintf(stderr, "[recdbg flame f=%u j=%d] model=%d drawn=%d lerp=%d origin=(%d,%d,%d)\n",
            openpete_frame, j, d->model, d->drawn, spyro_records_lerps(d->model) && p != NULL,
            staged_origin ? staged_origin[0] : 0, staged_origin ? staged_origin[1] : 0, staged_origin ? staged_origin[2] : 0);
}
void spyro_records_present_note(int k, const int32_t* staged_pos) {
    if (!spyro_records_dbg()) return;
    const spyro_model_rec_t* c = spyro_records_curr(k);
    if (!c) return;
    fprintf(stderr, "[recdbg present f=%u k=%d] drawn=%d pair=%d pair2=%d lerp=%d alpha=%.3f pos=(%d,%d,%d)\n",
            openpete_frame, k, c->drawn, c->pair, c->pair2, spyro_records_lerps(k), s_pres_alpha,
            staged_pos ? staged_pos[0] : 0, staged_pos ? staged_pos[1] : 0, staged_pos ? staged_pos[2] : 0);
}
void spyro_records_present_end(void) { s_pres_n = 0; s_pres_lerp = 0; }

op_time_t spyro_records_curr_ns(void) { return s_store[0].valid ? s_store[0].ns : 0; }
