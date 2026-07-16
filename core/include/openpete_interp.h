/* core/include/openpete_interp.h — render-side state interpolation ring.
 *
 * Phase 2 scaffolding of notes/plans/openpete-fps-unlock.md. Captures
 * gameplay-transform state at the end of each PSX sim tick (29.97 Hz)
 * so the render path can later lerp between snapshots N-1 and N and
 * re-emit DrawOTag at host refresh cadence (60/144/unlimited Hz).
 *
 * Three-stage ring: the last three distinct ticks (prev2, prev, curr).
 * Alpha = (op_now() - curr.tick_ns) / sim_period_ns. Re-emission shows
 * the world at virtual time `prev.tick + alpha * sim_period`, accepting
 * one tick of latency in exchange for never extrapolating. prev2 is kept
 * so the render side can build a cubic (Hermite) interpolant with a
 * centered start-tangent — halving the velocity discontinuity at tick
 * boundaries vs the linear prev→curr chord (see openpete_interp_query3).
 *
 * Scope (this ring): the capture API, the dedupe predicate, and the
 * prev2/prev/curr query (openpete_interp_query3). The lerped re-emission
 * itself — the save-canonical → write-lerped → restore bracket around the
 * sub-tick present — is built GAME-SIDE (e.g. spyro_native_reextract.c), NOT
 * here: core stays address-free, and the game owns which regions/feeders it
 * brackets plus its own restore discipline (see the game's bracket-integrity
 * self-check, OPENPETE_BRACKET_CHECK). Capturing here is read-only against
 * PSX RAM — landing capture alone has zero gameplay impact and is unconditional.
 *
 * This DIRECT ring drives the camera/Spyro alpha. Per-OBJECT (indirect pool)
 * state uses a parallel store (openpete_interp_objs.h) with its OWN per-object
 * alpha — intentionally independent of this ring's alpha, so an object that
 * advanced on a tick the camera didn't (idle-homeworld dragon under a static
 * camera, and vice-versa) still interpolates. The two can differ by up to ~1
 * VBL on the same present; that is correct, not a bug.
 *
 * Region table is game-supplied via the weak `openpete_interp_regions`
 * symbol (parallel to `openpete_state_regions` in openpete_psx.h).
 * core/ holds zero game-specific addresses. */

#ifndef OPENPETE_INTERP_H
#define OPENPETE_INTERP_H

#include <stddef.h>
#include <stdint.h>

#include "openpete_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum total bytes captured per snapshot across all regions. Set
 * generously to cover Spyro (0x2A4) + Camera (0x110) + later mobies.
 * Two snapshots fit in ~64 KB total, which is comfortable for L1/L2. */
#define OPENPETE_INTERP_MAX_REGIONS         8
#define OPENPETE_INTERP_MAX_SNAPSHOT_BYTES  (32u * 1024u)

/* Region descriptor. Kept narrower than OpenpeteStateRegion: the interp
 * ring only handles DIRECT regions in v1 — moby pools (indirect arrays)
 * land in task #59 with their own descriptor variant. */
typedef struct {
    const char* name;       /* diagnostic */
    uint32_t    psx_addr;   /* KSEG0 PSX vaddr */
    uint32_t    length;     /* bytes to capture */
} openpete_interp_region_t;

/* Game registers its region table via this call before openpete_interp_init.
 * Explicit registration (instead of weak symbols) so we don't depend on
 * cross-toolchain weak-symbol semantics — MinGW's PE/COFF link rules don't
 * reliably let a strong override in an executable object override a weak
 * definition pulled from a static library archive. Call once at game
 * startup; pass NULL/0 to keep capture disabled.
 *
 * Safe to call multiple times — last call wins. Caller retains ownership
 * of the table; openpete_interp captures the pointer + count by value. */
void openpete_interp_register_regions(const openpete_interp_region_t* regions,
                                      size_t count);

/* One ring slot. `data[]` holds the concatenation of each region's bytes
 * in table order; `region_offsets[r]` gives the offset of region r within
 * `data[]`. `valid` flips to 1 the first time this slot is written. */
typedef struct {
    uint32_t  tick;                 /* openpete_frame at capture time */
    op_time_t tick_ns;              /* op_now() at capture time */
    uint32_t  total_bytes;
    uint8_t   valid;
    uint8_t   pad[3];
    uint32_t  region_offsets[OPENPETE_INTERP_MAX_REGIONS];
    uint8_t   data[OPENPETE_INTERP_MAX_SNAPSHOT_BYTES];
} openpete_interp_snapshot_t;

/* Lifecycle. init computes the per-region offsets from the table; fails
 * (returns non-zero) if the total exceeds OPENPETE_INTERP_MAX_SNAPSHOT_BYTES
 * — bumping the cap is a deliberate decision, not a silent runtime error. */
int  openpete_interp_init(void);
void openpete_interp_shutdown(void);

/* Invalidate the snapshot ring (keeps the region config). Guest-state
 * discontinuity (savestate load, rewind restore): the ring's snapshots
 * describe the pre-jump timeline; dropping them makes the next present
 * snap to the restored state instead of sweeping a full-scene lerp across
 * the jump. init() self-registers this (plus the objs-table reset) with
 * the openpete_discontinuity broadcast. */
void openpete_interp_reset(void);

/* Active iff init() succeeded AND the game supplied a non-empty region
 * table AND `--render-fps` is set above the sim rate (caller's choice).
 * Defaults to "off"; flip on via openpete_interp_set_enabled. */
void openpete_interp_set_enabled(int on);
int  openpete_interp_enabled(void);

/* Capture the canonical state of every region into the ring's next slot.
 * Called from run.c after each successful sim tick (ticked_this_iter).
 * No-op when disabled or when the region table is empty. */
void openpete_interp_capture(uint32_t tick, op_time_t tick_ns);

/* Virtual present clock: the run loop sets, before each present's re-extract,
 * the EVENLY-SPACED wall time this present will effectively be DISPLAYED at
 * (a PLL over the present stream). Sub-tick phase (alpha) computations must
 * use openpete_interp_present_now() instead of op_now(): presents are
 * GENERATED at uneven wall offsets (the canonical present fires off the
 * render pacer's grid), but FIFO displays them at even refresh slots — phase
 * from raw generation time re-creates 3:1 velocity judder on the display no
 * matter how linear the generation-time mapping is. Falls back to op_now()
 * when unset (canonical-only paths, harnesses). */
void      openpete_interp_set_present_target(op_time_t t);
op_time_t openpete_interp_present_now(void);

/* Returns the most recent two snapshots (prev = tick N-1, curr = tick N)
 * via out params. Returns 1 if both are valid AND their data buffers
 * differ (byte-identical → state is stuck → caller should re-present
 * the last framebuffer instead of re-emitting). Returns 0 if either
 * snapshot isn't valid yet, or the two are byte-identical, or
 * `--render-fps` is at the sim rate.
 *
 * `*out_alpha` is clamped to [0, 1] when returning 1; undefined otherwise. */
int openpete_interp_query(op_time_t now_ns,
                          op_time_t sim_period_ns,
                          double*   out_alpha,
                          const openpete_interp_snapshot_t** out_prev,
                          const openpete_interp_snapshot_t** out_curr);

/* Like openpete_interp_query, but also returns prev2 = the distinct tick BEFORE
 * prev (two back from curr), for cubic/Hermite interpolation with a centered
 * start-tangent. `*out_prev2` is NULL until the ring holds three distinct ticks
 * or when the pairing fails — callers fall back to the linear prev→curr path.
 * Return value + alpha + prev/curr semantics are identical to openpete_interp_query. */
int openpete_interp_query3(op_time_t now_ns,
                           op_time_t sim_period_ns,
                           double*   out_alpha,
                           const openpete_interp_snapshot_t** out_prev2,
                           const openpete_interp_snapshot_t** out_prev,
                           const openpete_interp_snapshot_t** out_curr);

/* Diagnostics. */
uint64_t openpete_interp_total_captures(void);
uint64_t openpete_interp_total_dedupe_skips(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INTERP_H */
