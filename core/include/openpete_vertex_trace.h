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
#define VTRACE_VERSION  1u

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
    int32_t  sxy_centroid[2];      /* screen pixels                    */
    int32_t  next_sxy_centroid[2]; /* predicted next-frame centroid    */
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
                                  const int32_t next_sxy_centroid[2],
                                  uint16_t vertex_count,
                                  uint8_t is_ui_opt_out,
                                  uint8_t matched,
                                  uint8_t velocity_capped);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_VERTEX_TRACE_H */
