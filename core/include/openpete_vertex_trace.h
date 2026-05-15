/* openpete_vertex_trace.h — per-batch binary trace of the substitute
 * pipeline. Off by default; enabled with --vertex-trace-log PATH.
 *
 * One fixed-size record per matcher batch per OT walk (sim-due AND each
 * intermediate render). Analyzed by scripts/analyze-vtrace.py to turn
 * "looks bad" into concrete numbers: per-batch trajectory continuity,
 * alpha-vs-delta correlation, match stability, velocity-cap fire rate.
 *
 * Record layout is stable and versioned (see VTRACE_MAGIC + VTRACE_VERSION).
 * Records are emitted in render order, then batch order within each render.
 */

#ifndef OPENPETE_VERTEX_TRACE_H
#define OPENPETE_VERTEX_TRACE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VTRACE_MAGIC    0x56545243u  /* "VTRC" */
/* v2: interpolation model. next_sxy_centroid (predicted future endpoint
 * under the old extrapolation model) replaced by delta_sxy (the known
 * [from→to] interpolation span). analyze-vtrace.py reconstructs applied
 * position as sx + delta_sxy*alpha (was sx + (nsx-sx)*alpha). */
#define VTRACE_VERSION  2u

/* File header — 32 bytes, one per file. */
typedef struct {
    uint32_t magic;      /* VTRACE_MAGIC */
    uint32_t version;    /* VTRACE_VERSION */
    uint32_t record_sz;  /* sizeof(openpete_vtrace_batch_t) */
    uint32_t _reserved[5];
} openpete_vtrace_header_t;

/* Per-batch record — fixed 72 bytes. */
typedef struct {
    int64_t  op_now_ns;            /* clock at record emission         */
    uint32_t openpete_frame;       /* yield counter                    */
    uint32_t render_idx;           /* monotonic, +1 per OT walk        */
    float    alpha;                /* substitute-time alpha            */
    uint32_t batch_idx;            /* 0..batch_count-1                 */
    uint32_t batch_count;          /* batches in this render           */
    uint32_t matrix_span_id;       /* RT/TR span identity              */
    uint32_t object_id;            /* matcher's persistent object id   */
    int32_t  centroid[3];          /* world q12                        */
    int32_t  sxy_centroid[2];      /* screen pixels (= "from" centroid) */
    int32_t  delta_sxy[2];         /* interpolation span (to - from)   */
    uint16_t vertex_count;
    uint8_t  iter_kind;            /* 0=sim-due, 1=intermediate        */
    uint8_t  is_ui_opt_out;
    uint8_t  matched;              /* 1 if paired with prev            */
    uint8_t  velocity_capped;      /* 1 if cap engaged this match attempt */
    uint16_t _pad;
} openpete_vtrace_batch_t;

/* Lifecycle. */
int  openpete_vtrace_open(const char* path);
void openpete_vtrace_close(void);
int  openpete_vtrace_enabled(void);

/* Iter-kind context. psyq_DrawOTag calls set_iter_kind(0) before
 * prepare_substitute; re_emit_last_ot calls set_iter_kind(1). */
void openpete_vtrace_set_iter_kind(int kind);

/* Emit one batch record. Called from matcher.c at the end of
 * prepare_substitute, after pair_count and velocity_capped are known. */
void openpete_vtrace_record_batch(uint32_t batch_idx,
                                  uint32_t batch_count,
                                  uint32_t matrix_span_id,
                                  uint32_t object_id,
                                  const int32_t centroid[3],
                                  const int32_t sxy_centroid[2],
                                  const int32_t delta_sxy[2],
                                  uint16_t vertex_count,
                                  uint8_t is_ui_opt_out,
                                  uint8_t matched,
                                  uint8_t velocity_capped);

/* ---- Per-vertex stability log (separate file, complementary harness) ----
 *
 * Records one fixed-size entry per cache emit. Used to answer:
 *   - Does GTE emit vertices in stable order per moby across frames?
 *   - Are vertex counts per moby stable (no LOD-switch chatter)?
 *   - How often does the same moby (= same v_local set) appear?
 *
 * Enabled via env: OPENPETE_VERT_STABILITY_LOG=path/to/file.bin
 *                  OPENPETE_VERT_STABILITY_LOG_FRAMES=N (default 50)
 *
 * Capped at N frames because per-emit logging is high-frequency
 * (~1500 emits/frame). N=50 frames ≈ 1.5 MB, plenty for the question.
 *
 * Format: 32B header (magic 'VSTB', version 1, record_sz 32) then
 * fixed records.
 */

#define VSTAB_MAGIC    0x56535442u  /* "VSTB" */
#define VSTAB_VERSION  1u

typedef struct {
    uint32_t openpete_frame;
    uint32_t matrix_span_id;        /* batch id within this frame */
    uint32_t vert_idx_in_span;      /* emit order, 0..N-1 within batch */
    int16_t  v_local[3];            /* model-local pre-transform — stable per logical vertex */
    int16_t  _pad;
    int32_t  mac[3];                /* post-transform, for cross-check */
} openpete_vstab_record_t;

void openpete_vstab_record_vertex(int16_t v_local[3], const int32_t mac[3]);
int  openpete_vstab_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_VERTEX_TRACE_H */
