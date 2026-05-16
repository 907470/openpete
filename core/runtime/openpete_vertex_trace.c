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

/* Per-vertex stability log — separate file. */
static FILE*   s_vstab_fp        = NULL;
static uint32_t s_vstab_frame_limit = 50;
static uint32_t s_vstab_skip_until  = 0;       /* skip until openpete_frame >= this */
static uint32_t s_vstab_last_frame   = 0xFFFFFFFFu;
static uint32_t s_vstab_frames_seen  = 0;
static uint32_t s_vstab_last_span_id = 0xFFFFFFFFu;
static uint32_t s_vstab_vert_idx     = 0;

extern unsigned g_openpete_render_cache_matrix_span_id;

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

static void vstab_init_from_env(void) {
    static int init_done = 0;
    if (init_done) return;
    init_done = 1;
    const char* path = getenv("OPENPETE_VERT_STABILITY_LOG");
    if (!path || !*path) return;
    const char* fl = getenv("OPENPETE_VERT_STABILITY_LOG_FRAMES");
    if (fl && *fl) s_vstab_frame_limit = (uint32_t)atoi(fl);
    const char* sk = getenv("OPENPETE_VERT_STABILITY_LOG_SKIP");
    if (sk && *sk) s_vstab_skip_until = (uint32_t)atoi(sk);
    s_vstab_fp = fopen(path, "wb");
    if (!s_vstab_fp) return;
    uint32_t header[8] = {0};
    header[0] = VSTAB_MAGIC;
    header[1] = VSTAB_VERSION;
    header[2] = (uint32_t)sizeof(openpete_vstab_record_t);
    fwrite(header, sizeof(header), 1, s_vstab_fp);
    fflush(s_vstab_fp);
}

int openpete_vstab_enabled(void) {
    vstab_init_from_env();
    return s_vstab_fp != NULL;
}

void openpete_vstab_record_vertex(int16_t v_local[3], const int32_t mac[3]) {
    vstab_init_from_env();
    if (!s_vstab_fp) return;
    if (openpete_frame < s_vstab_skip_until) return;

    /* Track frame boundaries — stop logging once we've seen the limit. */
    if (openpete_frame != s_vstab_last_frame) {
        if (s_vstab_frames_seen >= s_vstab_frame_limit) {
            /* Reached cap. Stop logging silently. */
            fflush(s_vstab_fp);
            fclose(s_vstab_fp);
            s_vstab_fp = NULL;
            return;
        }
        s_vstab_last_frame = openpete_frame;
        s_vstab_frames_seen++;
    }

    /* Reset vert_idx when matrix_span_id changes. */
    uint32_t span_id = g_openpete_render_cache_matrix_span_id;
    if (span_id != s_vstab_last_span_id) {
        s_vstab_last_span_id = span_id;
        s_vstab_vert_idx = 0;
    }

    openpete_vstab_record_t r;
    r.openpete_frame    = openpete_frame;
    r.matrix_span_id    = span_id;
    r.vert_idx_in_span  = s_vstab_vert_idx++;
    r.v_local[0]        = v_local[0];
    r.v_local[1]        = v_local[1];
    r.v_local[2]        = v_local[2];
    r._pad              = 0;
    r.mac[0]            = mac[0];
    r.mac[1]            = mac[1];
    r.mac[2]            = mac[2];
    fwrite(&r, sizeof(r), 1, s_vstab_fp);
}

/* ---- Per-span RT/TR log (G3) ---- */
static FILE*    s_span_fp        = NULL;
static int      s_span_init      = 0;
static uint32_t s_span_skip      = 0;
static uint32_t s_span_cap       = 250;
static uint32_t s_span_last_fr   = 0xFFFFFFFFu;
static uint32_t s_span_frames    = 0;

static void spanlog_init(void) {
    if (s_span_init) return;
    s_span_init = 1;
    const char* p = getenv("OPENPETE_SPAN_LOG");
    if (!p || !*p) return;
    const char* sk = getenv("OPENPETE_SPAN_LOG_SKIP");
    if (sk && *sk) s_span_skip = (uint32_t)atoi(sk);
    const char* fl = getenv("OPENPETE_SPAN_LOG_FRAMES");
    if (fl && *fl) s_span_cap = (uint32_t)atoi(fl);
    s_span_fp = fopen(p, "wb");
    if (!s_span_fp) return;
    uint32_t h[8] = { SPANLOG_MAGIC, SPANLOG_VERSION, 52u, 0,0,0,0,0 };
    fwrite(h, sizeof(h), 1, s_span_fp);
    fflush(s_span_fp);
}

int openpete_spanlog_enabled(void) {
    spanlog_init();
    return s_span_fp != NULL;
}

void openpete_spanlog_record(const int16_t rt[3][3], const int32_t tr[3],
                             const int16_t v0[3], uint32_t sxy_key,
                             uint32_t obj_tag) {
    spanlog_init();
    if (!s_span_fp) return;
    if (openpete_frame < s_span_skip) return;
    if (openpete_frame != s_span_last_fr) {
        if (s_span_frames >= s_span_cap) {
            fflush(s_span_fp); fclose(s_span_fp); s_span_fp = NULL; return;
        }
        s_span_last_fr = openpete_frame;
        s_span_frames++;
    }
    uint8_t buf[52];
    memcpy(buf +  0, &openpete_frame, 4);
    memcpy(buf +  4, &g_openpete_render_cache_matrix_span_id, 4);
    int16_t rt9[9] = { rt[0][0],rt[0][1],rt[0][2],
                       rt[1][0],rt[1][1],rt[1][2],
                       rt[2][0],rt[2][1],rt[2][2] };
    memcpy(buf +  8, rt9, 18);
    memcpy(buf + 26, tr,  12);
    memcpy(buf + 38, v0,   6);
    memcpy(buf + 44, &sxy_key, 4);
    memcpy(buf + 48, &obj_tag, 4);
    fwrite(buf, 1, 52, s_span_fp);
}

void openpete_vtrace_close(void) {
    if (s_fp) {
        fflush(s_fp);
        fclose(s_fp);
        s_fp = NULL;
    }
    if (s_span_fp) { fflush(s_span_fp); fclose(s_span_fp); s_span_fp = NULL; }
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
                                  const int32_t delta_sxy[2],
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
    r.delta_sxy[0]   = delta_sxy[0];
    r.delta_sxy[1]   = delta_sxy[1];
    r.vertex_count   = vertex_count;
    r.iter_kind      = s_iter_kind;
    r.is_ui_opt_out  = is_ui_opt_out;
    r.matched        = matched;
    r.velocity_capped = velocity_capped;
    fwrite(&r, sizeof(r), 1, s_fp);
}
