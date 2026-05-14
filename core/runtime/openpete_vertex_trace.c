/* openpete_vertex_trace.c — see header. Single-file impl, no external
 * dependencies beyond stdio + openpete_time. */

#include "openpete_vertex_trace.h"
#include "openpete_time.h"

#include <stdlib.h>
#include <string.h>

extern uint32_t openpete_frame;

static FILE*   s_fp          = NULL;
static uint8_t s_iter_kind   = 0;
static uint32_t s_render_idx = 0;

extern float g_openpete_substitute_alpha_snapshot;

int openpete_vtrace_open(const char* path) {
    if (!path || !*path) return 0;
    s_fp = fopen(path, "wb");
    if (!s_fp) return -1;
    openpete_vtrace_header_t h = {
        .magic     = VTRACE_MAGIC,
        .version   = VTRACE_VERSION,
        .record_sz = (uint32_t)sizeof(openpete_vtrace_batch_t),
        ._reserved = {0,0,0,0,0},
    };
    fwrite(&h, sizeof(h), 1, s_fp);
    fflush(s_fp);
    return 0;
}

void openpete_vtrace_close(void) {
    if (s_fp) {
        fflush(s_fp);
        fclose(s_fp);
        s_fp = NULL;
    }
}

int openpete_vtrace_enabled(void) {
    return s_fp != NULL;
}

void openpete_vtrace_set_iter_kind(int kind) {
    s_iter_kind = (uint8_t)kind;
}

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
                                  uint8_t velocity_capped) {
    if (!s_fp) return;

    /* Each prepare_substitute call is one OT walk. batch_idx==0 marks
     * the start; bump render_idx unconditionally so multiple
     * intermediates within the same openpete_frame are distinct. */
    if (batch_idx == 0u) {
        s_render_idx++;
    }

    openpete_vtrace_batch_t r;
    memset(&r, 0, sizeof(r));
    r.op_now_ns      = (int64_t)op_now();
    r.openpete_frame = openpete_frame;
    r.render_idx     = s_render_idx;
    r.alpha          = g_openpete_substitute_alpha_snapshot;
    r.batch_idx      = batch_idx;
    r.batch_count    = batch_count;
    r.matrix_span_id = matrix_span_id;
    r.object_id      = object_id;
    r.centroid[0]    = centroid[0];
    r.centroid[1]    = centroid[1];
    r.centroid[2]    = centroid[2];
    r.sxy_centroid[0] = sxy_centroid[0];
    r.sxy_centroid[1] = sxy_centroid[1];
    r.next_sxy_centroid[0] = next_sxy_centroid[0];
    r.next_sxy_centroid[1] = next_sxy_centroid[1];
    r.vertex_count   = vertex_count;
    r.iter_kind      = s_iter_kind;
    r.is_ui_opt_out  = is_ui_opt_out;
    r.matched        = matched;
    r.velocity_capped = velocity_capped;
    fwrite(&r, sizeof(r), 1, s_fp);
}
