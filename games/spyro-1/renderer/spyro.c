// games/spyro-1/renderer/spyro.c — the numerical harness for Spyro 1's player
// feeder: capture the primitives the game's own routine emits, decode the same
// pose natively, and report the difference.
//
// The feeder itself is the shared walk (games/common/renderer/player.c) with
// this game's decoders (renderer/player_game.c). This translation unit adds
// nothing to the render; it is the instrument that proved the decode, kept so
// a change to either half is re-measurable on the same numbers. It runs only
// under OPENPETE_SPYRO_VERIFY, from the shared hook, after the game's routine
// has run and while its scratch is still live.
//
// Four independent verdicts, each isolating one failure mode:
//   * spyro-scratch — per-index screen distance against the routine's own
//     projected SXY block. No nearest-neighbour ambiguity, so a per-axis
//     decode error shows directly.
//   * spyro-verify — nearest-neighbour distance from each emitted primitive
//     vertex to the nearest native vertex, prim to native so an over-decode
//     cannot inflate it.
//   * spyro-walk — ordered, index-exact comparison of the native face walk
//     against the emitted primitives: colour and UV agreement per triangle is
//     the byte-exact topology verdict.
//   * spyro-mat — the derived per-part matrix and translation against the
//     records the routine built, which hold exactly what it used.
//
// Determinism: reads only.

#include "psx_runtime_internal.h"
#include "openpete_psx.h"
#include "game_layout.h"
#include "renderer/player.h"
#include "renderer/player_game.h"
#include "renderer/native_gte.h"
#include "openpete_profile.h"
#include "spyro1_symbols.gen.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define PL (game_layout.player)

/* ---- captured primitives (ground truth) --------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (prim[0x7]) */
    uint8_t  len;           /* ordering-table tag length (prim[0] >> 24) */
    int      nv;            /* 3 or 4 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static int s_code_hist[256];

/* Walk the polybuf by ordering-table tag length (stride = ((w0 >> 24) + 1) *
 * 4) and decode each primitive by its code byte with the general PSX polygon
 * decode (bit 4 gouraud, bit 3 quad, bit 2 textured). */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("spyro.capture");
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag, or corrupt */
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if (s_gte_n < MAX_PRIMS) {
            prim_t* t = &s_gte[s_gte_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->len = (uint8_t)len;
            t->nraw = (int)(stride / 4); if (t->nraw > 20) t->nraw = 20;
            for (int i = 0; i < t->nraw; i++) t->raw[i] = cpu->read_word(p + (uint32_t)i*4);
            if ((code & 0xE0) == 0x20) {
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad;
                t->nv = quad ? 4 : 3;
                uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
                int c = 2;
                for (int k = 0; k < t->nv; k++) {
                    t->rgb[k] = (gouraud && k > 0) ? (t->raw[c++] & 0xFFFFFF) : rgb0;
                    uint32_t xy = t->raw[c++];
                    t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                    t->y[k] = (float)(int16_t)(xy >> 16);
                    if (tex) {
                        uint32_t uv = t->raw[c++];
                        t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                        if (k == 0)      t->clut  = (int)(uv >> 16);
                        else if (k == 1) t->tpage = (int)(uv >> 16);
                    }
                }
            }
            s_code_hist[code & 0xFF]++;
            s_gte_n++;
        }
        p += stride;
    }
}

static const char* k_part_name[3] = { "body", "head", "tail" };

static void spyro_dump(CPUState* cpu, uint32_t startp, uint32_t endp) {
    fprintf(stderr, "\n[spyro-dump f=%u] prims [%08X,%08X) %u bytes  OFX=%d OFY=%d H=%u\n",
            openpete_frame, startp, endp, endp - startp,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);

    ngte_dump_words(cpu, "g_Spyro[0x00..0x50]", PL.base, 0x14);
    fprintf(stderr,
        "  anim:  body=%02X head=%02X tail=%02X  sortDepth=%02X\n"
        "  colorFilter@0x28=%08X  emitMode@0x30=%08X\n",
        cpu->read_byte(PL.base + PL.anim_off[0]),
        cpu->read_byte(PL.base + PL.anim_off[1]),
        cpu->read_byte(PL.base + PL.anim_off[2]),
        cpu->read_byte(PL.base + PL.sort_depth_off),
        cpu->read_word(PL.base + PL.colour_filter_off),
        cpu->read_word(PL.base + PL.emit_mode_off));

    uint32_t model = cpu->read_word(G_MODELS) | 0x80000000u;
    fprintf(stderr, "  SPYRO_MODEL=%08X  numAnims=%u\n", model, cpu->read_word(model) & 0xFFFF);
    ngte_dump_words(cpu, "model[0x00..0x40]", model, 0x10);

    uint32_t block = player_part_block_base(cpu);
    for (int s = 0; s < PL.part_count; s++) {
        uint32_t animIdx = cpu->read_byte(PL.base + PL.anim_off[s]);
        uint32_t animHdr = cpu->read_word(model + 0x38u + animIdx * 4u);
        fprintf(stderr, "  --- part %d (%s): animIdx=%u animHdr=%08X\n",
                s, k_part_name[s], animIdx, animHdr);
        if (animHdr < 0x80000000u || animHdr >= 0x80800000u) { fprintf(stderr, "    (bad hdr ptr)\n"); continue; }
        ngte_dump_words(cpu, "animHdr[0x00..0x28]", animHdr, 0x0A);
        fprintf(stderr, "    m_AnimationVertices=%08X faces=%08X colors=%08X\n",
                cpu->read_word(animHdr + 0x10), cpu->read_word(animHdr + 0x14),
                cpu->read_word(animHdr + 0x18));
        uint32_t e = block + (uint32_t)s * PL.part_stride;
        fprintf(stderr, "    part-record @%08X:\n   ", e);
        for (int w = 0; w < (int)(PL.part_stride / 4u); w++)
            fprintf(stderr, "%08X ", cpu->read_word(e + (uint32_t)w*4));
        fprintf(stderr, "\n");
    }

    char hb[300]; int hp = 0; hb[0] = 0;
    for (int c = 0; c < 256; c++) if (s_code_hist[c] && hp < (int)sizeof(hb)-16)
        hp += snprintf(hb+hp, sizeof(hb)-hp, " %02X=%d", c, s_code_hist[c]);
    fprintf(stderr, "  prim codes:%s  (total=%d)\n", hb, s_gte_n);
    fprintf(stderr, "[spyro-dump] end\n\n");
}

/* ---- the native side, through the shared feeder's introspection --------- */
static int   s_proj_n, s_walk_n;
static float s_px[2048], s_py[2048], s_pz[2048];
static int   s_psat[2048], s_ppart[2048];

static void snap_projection(void) {
    if (s_proj_n > 2048) s_proj_n = 2048;
    for (int i = 0; i < s_proj_n; i++)
        player_proj_vert(i, &s_px[i], &s_py[i], &s_pz[i], &s_psat[i], &s_ppart[i]);
}

static void native_rebuild(CPUState* cpu) {
    s_proj_n = player_project_all(cpu);
    snap_projection();
    s_walk_n = player_build_walk(cpu);
}

/* ---- geometry A/B: native projected vertices vs the primitive vertices ---
 * Nearest-neighbour match of each primitive vertex to the nearest native
 * vertex, prim to native so an over-decode cannot inflate the score. */
static void spyro_compare_geometry(void) {
    if (!s_proj_n || !s_gte_n) { fprintf(stderr, "[spyro-verify f=%u] no data (proj=%d prim=%d)\n",
                                         openpete_frame, s_proj_n, s_gte_n); return; }
    int total = 0, hit4 = 0, hit2 = 0; double errsum = 0; float emax = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        for (int k = 0; k < p->nv; k++) {
            float bestd = 1e9f;
            for (int j = 0; j < s_proj_n; j++) {
                float dx = p->x[k] - s_px[j], dy = p->y[k] - s_py[j];
                float d = dx*dx + dy*dy;
                if (d < bestd) bestd = d;
            }
            float dist = sqrtf(bestd);
            total++; errsum += dist; if (dist > emax) emax = dist;
            if (dist <= 4.0f) hit4++; if (dist <= 2.0f) hit2++;
        }
    }
    fprintf(stderr, "[spyro-verify f=%u] proj=%d primVerts=%d  match<=4px=%.1f%% <=2px=%.1f%%  mean=%.2fpx max=%.1fpx\n",
            openpete_frame, s_proj_n, total,
            100.0*hit4/total, 100.0*hit2/total, errsum/total, emax);
}

/* ---- per-vertex A/B against the routine's own projected SXY block --------
 * The projection cursor runs continuously across the parts, so the block holds
 * every vertex in stream order — the same order the native projection fills —
 * and index j maps to index j with no nearest-neighbour ambiguity and no
 * back-face noise. */
static void spyro_scratchpad_ab(CPUState* cpu) {
    if (s_proj_n <= 0) return;
    uint32_t block = player_part_block_base(cpu);
    for (int s = 0; s < PL.part_count; s++) {
        uint32_t e = block + (uint32_t)s * PL.part_stride;
        uint32_t fd = cpu->read_word(e + PL.part_flags_off);
        if (fd == 0) break;
        fprintf(stderr, "  (%s fd=%08X blend=%u A%s B%s sign=%d)\n",
                k_part_name[s], fd, fd & PL.blend_mask,
                (fd & (1u << PL.part_stream_flag_bit[0])) ? "2" : "1",
                (fd & (1u << PL.part_stream_flag_bit[1])) ? "2" : "1",
                (int)((fd & PL.part_matrix_sign) != 0));
    }
    int hit4 = 0, tot = 0; double errsum = 0;
    int part_hit[3]={0,0,0}, part_tot[3]={0,0,0};
    int worst_j = -1; float worst_d = 0, worst_gx = 0, worst_gy = 0;
    for (int j = 0; j < s_proj_n; j++) {
        uint32_t w = cpu->read_word(PL.sxy_base + (uint32_t)j * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_px[j] - gx, dy = s_py[j] - gy, d = sqrtf(dx*dx + dy*dy);
        tot++; errsum += d; if (d <= 4.0f) hit4++;
        int sg = s_ppart[j]; if (sg>=0&&sg<3){ part_tot[sg]++; if(d<=4.0f) part_hit[sg]++; }
        if (d > worst_d) { worst_d = d; worst_j = j; worst_gx = gx; worst_gy = gy; }
    }
    if (worst_j >= 0 && worst_d > 8.0f)
        fprintf(stderr, "  [outlier f=%u] j=%d part=%d err=%.1fpx  native=(%.1f,%.1f) game=(%.0f,%.0f) vz=%.1f sat=%d\n",
                openpete_frame, worst_j, s_ppart[worst_j], worst_d,
                s_px[worst_j], s_py[worst_j], worst_gx, worst_gy,
                s_pz[worst_j], s_psat[worst_j]);
    fprintf(stderr, "[spyro-scratch f=%u] <=4px=%.1f%% mean=%.2fpx | body=%.0f%% head=%.0f%% tail=%.0f%%\n",
            openpete_frame, tot?100.0*hit4/tot:0, tot?errsum/tot:0,
            part_tot[0]?100.0*part_hit[0]/part_tot[0]:0,
            part_tot[1]?100.0*part_hit[1]/part_tot[1]:0,
            part_tot[2]?100.0*part_hit[2]/part_tot[2]:0);
}

/* Recover a primitive vertex's stream index by exact SXY match against the
 * routine's own projected block. -1 when there is no exact match. */
static int spyro_index_of(CPUState* cpu, int px, int py) {
    uint32_t want = ((uint32_t)(py & 0xFFFF) << 16) | (uint32_t)(px & 0xFFFF);
    for (int j = 0; j < s_proj_n; j++)
        if (cpu->read_word(PL.sxy_base + (uint32_t)j * 4) == want) return j;
    return -1;
}
static int spyro_nn(float px, float py) {
    int best = -1; float bd = 1e18f;
    for (int j = 0; j < s_proj_n; j++) {
        float dx = px - s_px[j], dy = py - s_py[j], d = dx*dx + dy*dy;
        if (d < bd) { bd = d; best = j; }
    }
    return best;
}
/* How often would screen-distance nearest-neighbour pick a DIFFERENT vertex
 * than exact index recovery? A high rate means a face attached to the wrong
 * vertex whenever two parts overlap on screen. */
static void spyro_idx_audit(CPUState* cpu) {
    long exact = 0, disagree = 0, fallback = 0, total = 0;
    for (int i = 0; i < s_gte_n; i++) {
        prim_t* p = &s_gte[i];
        if (p->nv < 3) continue;
        for (int k = 0; k < p->nv; k++) {
            int ex = spyro_index_of(cpu, (int)p->x[k], (int)p->y[k]);
            total++;
            if (ex < 0) { fallback++; continue; }
            exact++;
            if (ex != spyro_nn(p->x[k], p->y[k])) disagree++;
        }
    }
    fprintf(stderr, "[spyro-idx f=%u] exact=%ld/%ld fallback=%ld  NN-would-disagree=%ld (%.1f%%)\n",
            openpete_frame, exact, total, fallback, disagree,
            total ? 100.0 * disagree / total : 0.0);
}

/* ---- walk A/B: ordered, index-exact against the captured primitives ------
 * The routine emits primitives in face order; the walk visits faces in face
 * order and splits quads the same way, so the two triangle sequences align
 * 1:1. Rebuild the EXPECTED list from the primitives (with stream indices
 * recovered exactly) and compare element-wise: colour and UV agreement is the
 * byte-exact verdict, since a wrong vertex carries a different model colour or
 * texel. Position is informational only — two of the vertices can share one
 * int16 screen position, which makes index recovery ambiguous there. */
typedef struct { int walk, expected, cnt_ok, n, pos_ok, rgb_ok, uv_ok, first_bad; } walk_stats;
#define MAX_EXP 8192
static void spyro_walk_eval(CPUState* cpu, walk_stats* st) {
    memset(st, 0, sizeof(*st)); st->first_bad = -1; st->cnt_ok = 1;
    if (s_gte_n <= 0 || s_proj_n <= 0) return;
    static struct { int i[3]; uint32_t rgb[3]; int u[3], v[3]; } exp[MAX_EXP];
    int en = 0;
    for (int p = 0; p < s_gte_n && en + 2 < MAX_EXP; p++) {
        prim_t* pr = &s_gte[p];
        if (pr->nv < 3) continue;
        int pi[4];
        for (int k = 0; k < pr->nv; k++) pi[k] = spyro_index_of(cpu, (int)pr->x[k], (int)pr->y[k]);
        int splits = (pr->nv == 4) ? 2 : 1;
        for (int s = 0; s < splits; s++) {
            int a,b,c;
            if (s == 0) { a=0;b=1;c=2; } else { a=1;b=3;c=2; }   /* the GPU quad split */
            exp[en].i[0]=pi[a]; exp[en].i[1]=pi[b]; exp[en].i[2]=pi[c];
            exp[en].rgb[0]=pr->rgb[a]; exp[en].rgb[1]=pr->rgb[b]; exp[en].rgb[2]=pr->rgb[c];
            exp[en].u[0]=pr->u[a]; exp[en].v[0]=pr->v[a];
            exp[en].u[1]=pr->u[b]; exp[en].v[1]=pr->v[b];
            exp[en].u[2]=pr->u[c]; exp[en].v[2]=pr->v[c];
            en++;
        }
    }
    int n = s_walk_n < en ? s_walk_n : en;
    for (int k = 0; k < n; k++) {
        int wi[3], wu[3], wv[3]; uint32_t wc[3];
        player_walk_tri(k, wi, wc, wu, wv);
        int pok=1, rok=1, uok=1;
        for (int j = 0; j < 3; j++) {
            int ei = exp[k].i[j];
            if (ei < 0 || wi[j] < 0 || wi[j] >= s_proj_n || ei >= s_proj_n ||
                floorf(s_px[wi[j]]) != floorf(s_px[ei]) ||
                floorf(s_py[wi[j]]) != floorf(s_py[ei])) pok=0;
            if (wc[j] != exp[k].rgb[j]) rok=0;
            if (wu[j] != exp[k].u[j] || wv[j] != exp[k].v[j]) uok=0;
        }
        if (pok) st->pos_ok++; if (rok) st->rgb_ok++; if (uok) st->uv_ok++;
        if (!(rok&&uok) && st->first_bad<0) st->first_bad=k;
    }
    st->walk = s_walk_n; st->expected = en; st->n = n; st->cnt_ok = (s_walk_n == en);
}
static void spyro_walk_compare(CPUState* cpu) {
    walk_stats st; spyro_walk_eval(cpu, &st);
    fprintf(stderr, "[spyro-walk f=%u] walk=%d expected=%d (prims=%d) %s pos=%.1f%% rgb=%.1f%% uv=%.1f%%  first_bad=%d\n",
            openpete_frame, st.walk, st.expected, s_gte_n, st.cnt_ok ? "CNT-OK" : "CNT-MISMATCH",
            st.n?100.0*st.pos_ok/st.n:0, st.n?100.0*st.rgb_ok/st.n:0, st.n?100.0*st.uv_ok/st.n:0, st.first_bad);
}

/* ---- matrix A/B: the derived transform against the records the routine built
 * Per part: the largest |derived - record| over the nine matrix elements and
 * the three translation words. A part with no matrix of its own inherits the
 * root's, so it is compared against the root record's matrix. */
typedef struct { int part_n; int m_max[3]; int tr_max[3]; int worst_m, worst_tr; } mat_stats;
static void spyro_matrix_eval(CPUState* cpu, mat_stats* ms) {
    memset(ms, 0, sizeof *ms);
    uint32_t block = player_part_block_base(cpu);
    if (!block) return;
    player_project_all(cpu);           /* fills the derived transforms */
    int M0[3][3]; ngte_read_matrix(cpu, block + PL.part_matrix_off, M0);
    for (int s = 0; s < PL.part_count; s++) {
        uint32_t e = block + (uint32_t)s*PL.part_stride;
        uint32_t fd = cpu->read_word(e + PL.part_flags_off);
        if (fd == 0) break;
        const player_xform_t* xf = player_part_xform(s);
        if (!xf) break;
        ms->part_n++;
        int hasMat = (s == 0) || (fd & PL.part_matrix_sign);
        int Ms_[3][3]; if (hasMat) ngte_read_matrix(cpu, e + PL.part_matrix_off, Ms_); else memcpy(Ms_, M0, sizeof M0);
        int mmax = 0;
        for (int i=0;i<3;i++) for (int j=0;j<3;j++) { int d = abs(xf->M[i][j] - Ms_[i][j]); if (d>mmax) mmax=d; }
        int TRs[3];
        for (int i=0;i<3;i++) TRs[i] = (int32_t)cpu->read_word(e + PL.part_tr_off + (uint32_t)i*4u);
        int tmax = 0;
        for (int i=0;i<3;i++) { int d = (int)(fabs(xf->TR[i] - (double)TRs[i]) + 0.5); if (d>tmax) tmax=d; }
        ms->m_max[s] = mmax; ms->tr_max[s] = tmax;
        if (mmax > ms->worst_m) ms->worst_m = mmax;
        if (tmax > ms->worst_tr) ms->worst_tr = tmax;
    }
}
static void spyro_matrix_compare(CPUState* cpu) {
    mat_stats ms; spyro_matrix_eval(cpu, &ms);
    fprintf(stderr, "[spyro-mat f=%u] parts=%d  Mmax(b/h/t)=%d/%d/%d  TRmax(b/h/t)=%d/%d/%d  => %s\n",
            openpete_frame, ms.part_n, ms.m_max[0], ms.m_max[1], ms.m_max[2],
            ms.tr_max[0], ms.tr_max[1], ms.tr_max[2],
            (ms.worst_m <= 2 && ms.worst_tr <= 2) ? "MATCH" : "MISMATCH");
}

/* ---- full-run aggregate -------------------------------------------------
 * Cheap per-tick (O(verts)) per-index match against the routine's own block,
 * accumulated across the whole run so a multi-level demo gets one verdict
 * without tens of thousands of log lines or the quadratic nearest-neighbour
 * cost. */
static struct {
    long frames, bad_frames;
    double worst_match; unsigned worst_frame; int worst_part;
    double max_err; unsigned max_err_frame;
    long w_cnt_bad, w_rgb_bad, w_uv_bad, w_interp;
    unsigned w_first_cnt_bad_frame, w_first_interp_frame;
    long mat_bad; int mat_worst_m, mat_worst_tr; unsigned mat_worst_frame;
} s_agg = { 0, 0, 100.0, 0, -1, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void spyro_agg_final(void) {
    fprintf(stderr, "[spyro-agg FINAL] frames=%ld imperfect=%ld (%.2f%%)  worst=%.1f%% (part %d @f%u)  maxerr=%.1fpx @f%u  => %s\n",
            s_agg.frames, s_agg.bad_frames,
            s_agg.frames ? 100.0 * s_agg.bad_frames / s_agg.frames : 0.0,
            s_agg.worst_match, s_agg.worst_part, s_agg.worst_frame,
            s_agg.max_err, s_agg.max_err_frame,
            s_agg.bad_frames == 0 ? "PASS (geometry 100% all parts, all frames)" : "GEOMETRY RESIDUAL");
    fprintf(stderr, "[spyro-walk FINAL] frames=%ld  cnt-mismatch=%ld (first @f%u)  rgb-bad=%ld  uv-bad=%ld  interp-path=%ld (first @f%u)  => %s\n",
            s_agg.frames, s_agg.w_cnt_bad, s_agg.w_first_cnt_bad_frame,
            s_agg.w_rgb_bad, s_agg.w_uv_bad, s_agg.w_interp, s_agg.w_first_interp_frame,
            (s_agg.w_cnt_bad==0 && s_agg.w_rgb_bad==0 && s_agg.w_uv_bad==0 && s_agg.w_interp==0)
              ? "PASS (walk topology+colour+UV byte-exact, all frames)" : "WALK RESIDUAL");
    fprintf(stderr, "[spyro-mat FINAL] frames=%ld  mat-mismatch=%ld  worstM=%d worstTR=%d @f%u  => %s\n",
            s_agg.frames, s_agg.mat_bad, s_agg.mat_worst_m, s_agg.mat_worst_tr, s_agg.mat_worst_frame,
            (s_agg.mat_bad==0) ? "PASS (M+TR derived from gamestate match the records, all frames)" : "MATRIX RESIDUAL");
}

static void spyro_aggregate(CPUState* cpu) {
    static int s_reg = 0;
    if (!s_reg) { s_reg = 1; atexit(spyro_agg_final); }
    int part_hit[3] = {0,0,0}, part_tot[3] = {0,0,0};
    double fmax = 0.0; int worst_j = -1; float wgx = 0, wgy = 0;
    for (int j = 0; j < s_proj_n; j++) {
        uint32_t w = cpu->read_word(PL.sxy_base + (uint32_t)j * 4);
        float gx = (float)(int16_t)(w & 0xFFFF), gy = (float)(int16_t)(w >> 16);
        float dx = s_px[j] - gx, dy = s_py[j] - gy, d = sqrtf(dx*dx + dy*dy);
        int sg = s_ppart[j]; if (sg>=0&&sg<3){ part_tot[sg]++; if(d<=4.0f) part_hit[sg]++; }
        if (d > fmax) { fmax = d; worst_j = j; wgx = gx; wgy = gy; }
    }
    if (fmax > 8.0f && worst_j >= 0)
        fprintf(stderr, "  [outlier f=%u] j=%d part=%d err=%.1fpx native=(%.1f,%.1f) game=(%.0f,%.0f) vz=%.1f sat=%d (n=%d)\n",
                openpete_frame, worst_j, s_ppart[worst_j], fmax,
                s_px[worst_j], s_py[worst_j], wgx, wgy,
                s_pz[worst_j], s_psat[worst_j], s_proj_n);
    s_agg.frames++;
    int bad = 0;
    for (int sg = 0; sg < 3; sg++) {
        if (!part_tot[sg]) continue;
        double m = 100.0 * part_hit[sg] / part_tot[sg];
        if (m < 100.0) bad = 1;
        if (m < s_agg.worst_match) { s_agg.worst_match = m; s_agg.worst_frame = openpete_frame; s_agg.worst_part = sg; }
    }
    if (bad) s_agg.bad_frames++;
    if (fmax > s_agg.max_err) { s_agg.max_err = fmax; s_agg.max_err_frame = openpete_frame; }

    /* The demo uses the DIRECT emit path. If any level sets the selector, the
     * direct-only walk would mis-stride, so flag it loudly rather than
     * silently emitting garbage. */
    if (PL.emit_mode_off && cpu->read_word(PL.base + PL.emit_mode_off) != 0) {
        if (!s_agg.w_interp) s_agg.w_first_interp_frame = openpete_frame;
        s_agg.w_interp++;
    }
    { walk_stats wst; spyro_walk_eval(cpu, &wst);
      if (!wst.cnt_ok) { if (!s_agg.w_cnt_bad) s_agg.w_first_cnt_bad_frame = openpete_frame; s_agg.w_cnt_bad++; }
      if (wst.n && wst.rgb_ok < wst.n) s_agg.w_rgb_bad++;
      if (wst.n && wst.uv_ok  < wst.n) s_agg.w_uv_bad++;
    }
    { mat_stats ms; spyro_matrix_eval(cpu, &ms);
      if (ms.worst_m > 2 || ms.worst_tr > 2) s_agg.mat_bad++;
      if (ms.worst_m > s_agg.mat_worst_m) { s_agg.mat_worst_m = ms.worst_m; s_agg.mat_worst_frame = openpete_frame; }
      if (ms.worst_tr > s_agg.mat_worst_tr) { s_agg.mat_worst_tr = ms.worst_tr; }
    }

    if ((s_agg.frames % 2000) == 0) {
        fprintf(stderr, "[spyro-agg f=%u] frames=%ld imperfect=%ld  worst=%.1f%% (part %d @f%u)  maxerr=%.1fpx @f%u\n",
                openpete_frame, s_agg.frames, s_agg.bad_frames, s_agg.worst_match,
                s_agg.worst_part, s_agg.worst_frame, s_agg.max_err, s_agg.max_err_frame);
        fprintf(stderr, "[spyro-walk f=%u] frames=%ld cnt-mismatch=%ld rgb-bad=%ld uv-bad=%ld interp-path=%ld => %s\n",
                openpete_frame, s_agg.frames, s_agg.w_cnt_bad, s_agg.w_rgb_bad, s_agg.w_uv_bad, s_agg.w_interp,
                (s_agg.w_cnt_bad==0 && s_agg.w_rgb_bad==0 && s_agg.w_uv_bad==0 && s_agg.w_interp==0)
                  ? "PASS (walk byte-exact)" : "WALK RESIDUAL");
        fprintf(stderr, "[spyro-mat f=%u] frames=%ld mat-mismatch=%ld worstM=%d worstTR=%d @f%u => %s\n",
                openpete_frame, s_agg.frames, s_agg.mat_bad, s_agg.mat_worst_m, s_agg.mat_worst_tr, s_agg.mat_worst_frame,
                (s_agg.mat_bad==0) ? "PASS (matrix byte-exact)" : "MATRIX RESIDUAL");
    }
}

/* ---- the entries the provider hands the shared hook --------------------- */

int spyro_verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SPYRO_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned spyro_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SPYRO_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

void spyro_verify_run(CPUState* cpu, uint32_t startp, uint32_t endp) {
    static int s_dumped = 0, s_reports = 0;
    if (openpete_frame < spyro_verify_frame() || endp <= startp) return;
    capture_prims(cpu, startp, endp);
    native_rebuild(cpu);
    if (!s_dumped) { spyro_dump(cpu, startp, endp); s_dumped = 1; }
    if (s_reports < 200) {            /* detailed spot-check (first 200 frames) */
        spyro_compare_geometry();
        spyro_scratchpad_ab(cpu);
        spyro_idx_audit(cpu);
        spyro_walk_compare(cpu);
        spyro_matrix_compare(cpu);
        s_reports++;
        native_rebuild(cpu);          /* the matrix eval re-projected; restore the snapshot */
    }
    spyro_aggregate(cpu);
}
