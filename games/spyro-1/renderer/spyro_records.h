/* spyro_records.h — per-call records of the player render hooks.
 *
 * notes/plans/openpete-native-multi-spyro-subtick.md. The three player
 * feeders (model func_80023AC4, drop shadow func_80059A48, flame
 * func_80058D64) fire once per game call; a mod that draws extra dragons
 * calls them once per player with the player's state swapped into g_Spyro.
 * The sub-tick spine used to be a singleton over those calls (the last
 * call's bytes won); this TU is the record store that replaces it.
 *
 * Model records (P1): every canonical model-hook call appends a record — the
 * g_Spyro bytes the extractor reads, the seg scratch row, the Fairy-Kiss
 * colour row, the SXY row and the polybuf range — to the epoch's list. The
 * canonical-epoch commit closes the list into a three-deep store
 * (curr/prev/prev2) with one clock per store, pairs each record with a
 * record of the previous list (index first, position-verified within B; a
 * second pass by nearest position), and appends a pairing-only record when
 * no hook observed the ring's tick-end dragon. The intermediate re-extract
 * runs the Spyro feeder once per record, staging that record's bytes and
 * brackets (spyro_native_reextract.c). Modless the list holds the composer's
 * one call and the k=0 iteration is the old singleton path exactly.
 *
 * Shadow records (P2): every canonical shadow-hook call appends the
 * D_SPYROSHADOW bytes the walk reads, bound to the model record of the same
 * epoch whose g_Spyro position equals the live one at the hook (the mod calls
 * model, shadow, flame per player with the same state swapped in); a shadow
 * with no such model record (hidden player, blink) appends a pairing-only
 * model record at its call ordinal and binds to that. A shadow record takes
 * its model record's pairing and clock; only its own anchor bytes are
 * bracketed, between it and the previous list's shadow of the paired model.
 *
 * Flame records (P3): the same shape for the flame hook — the g_SpyroFlame
 * bytes the ribbon walk reads (it reads no g_Spyro field), bound to the
 * model record at the live g_Spyro position, bracketed (origin, attach
 * matrix, strand counters) against the previous list's flame of the paired
 * model.
 *
 * Runahead (P7): the hooks fire on the speculative ticks too, and those
 * fires build a LED list keyed by openpete_frame. At commit the led list
 * whose frame is the ring's capture tick supplies every record's lerp source
 * (position, rotation, clocks) and seg row, index for index, so N dragons
 * lead the way the singleton's one did; the committed bytes stay the staged
 * base. A led list that does not match (count, frame) leaves the records
 * committed and is counted.
 *
 * OPENPETE_SPYRO_RECDBG=1 prints the epoch census at commit, a per-present
 * line per record on the intermediates and a run summary at exit.
 */

#ifndef SPYRO_RECORDS_H
#define SPYRO_RECORDS_H

#include <stdint.h>
#include "openpete_time.h"   /* op_time_t */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

enum { SPYRO_REC_MODEL = 0, SPYRO_REC_SHADOW = 1, SPYRO_REC_FLAME = 2, SPYRO_REC_FEEDERS = 3 };

#define SPYRO_REC_MAX        16       /* per feeder per epoch (P0 measured 8) */
#define SPYRO_REC_SPYRO_LEN  0x2A4u   /* g_Spyro region (spyro_interp_regions.c) */
#define SPYRO_REC_SEG_LEN    0x100u   /* seg scratch row G_SCRATCH+0x1900 */
#define SPYRO_REC_FK_LEN     0x300u   /* Fairy-Kiss colour row G_SCRATCH+0x1D00 */
#define SPYRO_REC_SXY_LEN    0x400u   /* projected SXY scratchpad 0x1F800000 */
#define SPYRO_REC_SHADOW_LEN 0x28u    /* D_SPYROSHADOW region (spyro_interp_regions.c) */
#define SPYRO_REC_FLAME_LEN  0xD0u    /* g_SpyroFlame region (spyro_interp_regions.c) */
#define SPYRO_REC_PAIR_B     400.0    /* pairing bound B, units per tick (P0) */

typedef struct {
    uint8_t  spyro[SPYRO_REC_SPYRO_LEN];  /* the hook's g_Spyro: the staged base */
    uint8_t  src[SPYRO_REC_SPYRO_LEN];    /* the lerp source (position, rotation, clocks): the same bytes,
                                           * except under --runahead where the led list's record goes here
                                           * (the singleton lerped the ring's led bytes over the committed
                                           * base, which the shadow's silhouette selectors read) */
    uint8_t  seg[SPYRO_REC_SEG_LEN];
    uint8_t  fk[SPYRO_REC_FK_LEN];
    uint8_t  sxy[SPYRO_REC_SXY_LEN];
    uint32_t rp_start, rp_end;   /* polybuf range of the call (0,0 = pairing-only) */
    uint8_t  drawn;              /* 1 = a hook call; 0 = pairing-only (never rendered) */
    int8_t   pair;               /* index in the previous list, -1 unpaired */
    int8_t   pair2;              /* index in the list before that (via pair), -1 */
} spyro_model_rec_t;

typedef struct {
    uint8_t  sh[SPYRO_REC_SHADOW_LEN];   /* D_SPYROSHADOW at the hook: anchor, Y words, flags, over-water */
    uint8_t  src[SPYRO_REC_SHADOW_LEN];  /* the anchor lerp source (the ring's led bytes under --runahead, else sh) */
    int8_t   model;                      /* index of the model record it renders under */
    uint8_t  drawn;                      /* 1 = a hook call; 0 = pairing-only: the ring's tick-end bytes for the
                                          * ring's dragon when its shadow hook did not fire (the anchor is written
                                          * every tick), so the shadow's first tick lerps like the singleton did */
} spyro_shadow_rec_t;

typedef struct {
    uint8_t  fl[SPYRO_REC_FLAME_LEN];    /* g_SpyroFlame at the hook: origin, strand counters, attach matrix, active */
    uint8_t  src[SPYRO_REC_FLAME_LEN];   /* the bracket's lerp source (as above) */
    int8_t   model;                      /* index of the model record it renders under */
    uint8_t  drawn;                      /* as above */
} spyro_flame_rec_t;

/* ---- canonical side (game fiber) ------------------------------------- */

/* Model hook: append this call's record (after the renderer ran, endp >
 * startp, g_Spyro and the scratch still holding what it read). Returns the
 * record's index in the epoch's list — the PLAYER tag slot for the tris the
 * hook emits — or -1 when refused (speculative fire, cap). */
int  spyro_records_model_append(struct CPUState* cpu, uint32_t rp_start, uint32_t rp_end);

/* Shadow hook: append this call's record (after the renderer ran, endp >
 * startp). Binds to the epoch's model record at the live g_Spyro position,
 * appending a pairing-only one when none was drawn. Returns the index of
 * the model record it bound to — the PLAYER slot of the shadow's tag — or
 * -1 when refused (speculative fire, cap). */
int  spyro_records_shadow_append(struct CPUState* cpu);

/* Flame hook: the same, for the flame record. */
int  spyro_records_flame_append(struct CPUState* cpu);

/* Once per canonical present from spyro_native_canon_epoch_commit, after the
 * DIRECT ring captured the tick: closes the epoch's list into the store. */
void spyro_records_epoch_commit(void);

/* ---- present side (host loop, inside one intermediate present) -------- */

/* Open the store for this present. Returns the record count to render (0
 * when the store is stale for the committed epoch or holds no list) and the
 * store's own alpha (its prev→curr clock against the present clock, clamped
 * like the ring's). `ring_paired` / `ring_cut` are the orchestrator's ring
 * verdicts: with either false every record renders from its current bytes. */
int  spyro_records_present_begin(int ring_paired, int ring_cut, double ring_alpha, double* out_alpha);
/* Record k of the current list, and its paired predecessors (NULL when
 * unpaired / no history). Valid between present_begin and present_end. */
const spyro_model_rec_t* spyro_records_curr(int k);
const spyro_model_rec_t* spyro_records_prev(int k);
const spyro_model_rec_t* spyro_records_prev2(int k);
/* Whether record k interpolates this present (paired and the ring bracketed). */
int  spyro_records_lerps(int k);
/* Shadow records of the current list: count, record j, and the previous
 * list's shadow bound to the model record j's model paired with (NULL when
 * the model is unpaired or that model drew no shadow). */
int  spyro_records_shadow_count(void);
const spyro_shadow_rec_t* spyro_records_shadow_curr(int j);
const spyro_shadow_rec_t* spyro_records_shadow_prev(int j);
/* RECDBG per-present line for shadow record j (staged anchor). */
void spyro_records_shadow_note(int j, const int32_t* staged_anchor);
/* Flame records, the same way. */
int  spyro_records_flame_count(void);
const spyro_flame_rec_t* spyro_records_flame_curr(int j);
const spyro_flame_rec_t* spyro_records_flame_prev(int j);
void spyro_records_flame_note(int j, const int32_t* staged_origin);
/* RECDBG per-present line for record k (staged position, pairing, alpha). */
void spyro_records_present_note(int k, const int32_t* staged_pos);
void spyro_records_present_end(void);

/* The store's newest clock stamp (0 = none): run.c's deterministic present
 * target takes the later of the ring's and the store's. */
op_time_t spyro_records_curr_ns(void);

/* 1 while OPENPETE_SPYRO_RECDBG is set (latched on first query). */
int spyro_records_dbg(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_RECORDS_H */
