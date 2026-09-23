// games/spyro-1/renderer/shadow_spyro.c — the SPYRO SILHOUETTE SHADOW
// renderer (func_80059A48 / r_shadows.s) re-derived from gamestate in float,
// plus its A/B harness. It is a separate renderer from the moby drop-shadows
// (renderer/shadow.c, shared between games): an untextured gouraud G3 fan
// (GPU code 0x32) projecting Spyro's silhouette onto the floor, drawn with
// PSX semi-transparent ABR mode 2 (B-F, SUBTRACTIVE — the shadow grey darkens
// the ground; the bundled draw-mode word 0xE1000640 carries ABR bits 0b10).
//
// STATIC ASM FINDINGS:
//   * the draw bundles an E1 draw-mode mini-packet (DMA tag len 2) ahead of
//     each G3 (DMA tag len 6); the G3 fan is (anchor, strip[i], strip[i+1])
//     with color0=0x608080 and color1=color2=0.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to normal dispatch) then only READS state afterwards.
// Gated by OPENPETE_SHADOW_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "openpete_vstream.h"
#include "renderer/reextract.h"  /* sub-tick re-extract entry */
#include "renderer/player.h"           /* per-call records of the player hooks */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "renderer/shadow.h"        /* shared prim capture, projection, decal params */
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern void     func_80059A48(CPUState* cpu);  /* the real SPYRO-shadow renderer */
extern unsigned openpete_frame;

/* ---- spyro-shadow dump ----------------------------------------------------
 * Reproduces the model-silhouette navigation read-only (keyframe A/B vertex
 * data via g_Models[0] + g_Spyro selectors) and dumps the post-call scratchpad
 * (0x1F800000..0xA0) — the .s's working buffer: lerped silhouette (0x00..),
 * ring radii/Y (0x80/0x90), projected strip SXY/SZ (0x08..0x78). The scratch is
 * the ORACLE for deriving the native interp + ring math. */
static void shadow_spyro_dump(CPUState* cpu) {
    fprintf(stderr, "\n=== [shadow-spyro-dump f=%u] ===\n", openpete_frame);
    int overwater = (int)cpu->read_word(D_SPYROSHADOW + 0x24);
    int sx = (int)cpu->read_word(D_SPYROSHADOW + 0x10);
    int sy = (int)cpu->read_word(D_SPYROSHADOW + 0x14);
    int sz = (int)cpu->read_word(D_SPYROSHADOW + 0x18);
    fprintf(stderr, "  SpyroShadow: overwater=%d pos=(%d,%d,%d)  +0x08=%08X +0x0C=%08X +0x1C=%08X\n",
            overwater, sx, sy, sz,
            cpu->read_word(D_SPYROSHADOW+8), cpu->read_word(D_SPYROSHADOW+0xC),
            cpu->read_word(D_SPYROSHADOW+0x1C));
    int heading = (int)cpu->read_byte(G_SPYRO+0x0E);
    uint32_t selX = cpu->read_half(G_SPYRO+0x18), selY = cpu->read_half(G_SPYRO+0x1E);
    int factor = (int)cpu->read_byte(G_SPYRO+0x24);
    uint32_t models0 = cpu->read_word(G_MODELS);
    fprintf(stderr, "  g_Spyro: heading(+0x0E)=%d selX(+0x18)=%04X selY(+0x1E)=%04X factor(+0x24)=%d\n",
            heading, selX, selY, factor);
    fprintf(stderr, "  g_Models[0]=%08X  camera pos=(%d,%d,%d)\n", models0,
            (int)cpu->read_word(G_CAMERA+0x28), (int)cpu->read_word(G_CAMERA+0x2C),
            (int)cpu->read_word(G_CAMERA+0x30));

    /* model navigation: keyframe A = (selX.lo submodel, selY.lo sub); B = (.hi,.hi) */
    if (models0 >= 0x80000000u && models0 < 0x80800000u) {
        uint32_t subA = cpu->read_word(models0 + ((selX & 0xFF) << 2) + 0x38);
        uint32_t subB = cpu->read_word(models0 + (((selX >> 8) & 0xFF) << 2) + 0x38);
        uint32_t vdA = cpu->read_word(subA + ((selY & 0xFF) << 2) + 0x24);
        uint32_t vdB = cpu->read_word(subB + (((selY >> 8) & 0xFF) << 2) + 0x24);
        uint32_t vmA = openpete_vstream_addr(vdA & 0x1FFFFFu, 1), vmB = openpete_vstream_addr(vdB & 0x1FFFFFu, 1);
        fprintf(stderr, "  nav: subA=%08X subB=%08X vdA=%08X->%08X vdB=%08X->%08X\n",
                subA, subB, vdA, vmA, vdB, vmB);
        fprintf(stderr, "  keyframe A words: %08X %08X   B words: %08X %08X\n",
                cpu->read_word(vmA), cpu->read_word(vmA+4), cpu->read_word(vmB), cpu->read_word(vmB+4));
    }

    fprintf(stderr, "  ring D_8006E268[0..15]:\n   ");
    for (int i = 0; i < 16; i++) { fprintf(stderr, " %08X", cpu->read_word(D_RING + (uint32_t)i*4)); if (i==7) fprintf(stderr, "\n   "); }
    /* post-call scratchpad: the .s's working buffer */
    fprintf(stderr, "\n  scratch 0x1F800000..0xA0 (post-call):\n");
    for (int r = 0; r < 0xA0; r += 16) {
        fprintf(stderr, "    +%02X:", r);
        for (int b = 0; b < 16; b += 4) fprintf(stderr, " %08X", cpu->read_word(0x1F800000u + (uint32_t)(r+b)));
        fprintf(stderr, "\n");
    }
    shadow_dump_prims("shadow-spyro");
    fprintf(stderr, "=== end shadow-spyro-dump ===\n\n");
}

/* ===========================================================================
 *  SPYRO SILHOUETTE SHADOW — native float walk (3b.2). Re-derived entirely
 *  from gamestate (no scratchpad coupling): the silhouette ring is the model's
 *  shadow keyframes A/B (g_Models[0] + g_Spyro selectors) linearly interpolated
 *  by g_Spyro+0x24, doubled to 16 ring points (point + midpoint); each point's
 *  XZ direction is D_8006E268[k] scaled by the interpolated radius, its Y from
 *  the SpyroShadow struct words; projected through the heading matrix (camera
 *  cols 0,2 rotated by g_Spyro+0x0E) + the ground-anchor TR. Emits a G3 fan
 *  (anchor, strip[k], strip[k+1]), subtractive (ABR mode 2). Lerp + ring
 *  doubling validated bit-exact against the .s scratchpad (ring[8..15], Y[0..3]).
 * =========================================================================== */

/* heading matrix: rotate the camera matrix's columns 0,2 by `heading` (yaw
 * around Y) using sin D_8006CBF8 / cos D_8006CC78. Two MVMVA passes (same idiom
 * as the moby idxA block, but cols 0,2). */
static void build_heading_matrix(CPUState* cpu, const int C[3][3], int heading, int R[3][3]) {
    int sinv = shadow_rd_s16(cpu, D_SIN_TABLE + (uint32_t)heading*2);
    int cosv = shadow_rd_s16(cpu, D_COS_TABLE + (uint32_t)heading*2);
    int P[3], Q[3];
    shadow_mvmva_ir(C,  cosv, 0, sinv, P);   /* P = C·(cos,0,sin) -> col0 */
    shadow_mvmva_ir(C, -sinv, 0, cosv, Q);   /* Q = C·(-sin,0,cos) -> col2 */
    R[0][0]=P[0]; R[0][1]=C[0][1]; R[0][2]=Q[0];
    R[1][0]=P[1]; R[1][1]=C[1][1]; R[1][2]=Q[1];
    R[2][0]=P[2]; R[2][1]=C[2][1]; R[2][2]=Q[2];
}

/* native Spyro-shadow strip tris (G3 fan, untextured) */
typedef struct { float sx[3], sy[3], sz[3]; uint32_t rgb[3]; } sptri;
#define MAX_SPTRI 64
static sptri s_sptri[MAX_SPTRI]; static int s_sptri_n;

static void shadow_spyro_build(CPUState* cpu) {
    s_sptri_n = 0;
    if ((int)cpu->read_word(D_SPYROSHADOW + 0x24) > 0) return;     /* over water: no shadow */

    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per walk */
    const ngte_view* v = &view;
    int C[3][3]; memcpy(C, v->cam_M, sizeof C);
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;

    /* ground anchor: (shadowPos - camPos) swizzled, RTPS through camera (TR=0) */
    int sposx = (int)cpu->read_word(D_SPYROSHADOW + 0x10);
    int sposy = (int)cpu->read_word(D_SPYROSHADOW + 0x14);
    int sposz = (int)cpu->read_word(D_SPYROSHADOW + 0x18);
    int camx = (int)v->cam_ram[0];   /* integer camera, as the .s loads it */
    int camy = (int)v->cam_ram[1];
    int camz = (int)v->cam_ram[2];
    int avx = shadow_s16(camy - sposy);     /* VX0 */
    int avy = shadow_s16(camz - sposz);     /* VY0 */
    int avz = shadow_s16(sposx - camx);     /* VZ0 */
    double TR[3]; shadow_ground_tr(C, avx, avy, avz, TR);   /* avx/avy/avz stay s16 (faithful, 1-unit) */
    double ZERO[3] = {0,0,0};
    shadow_pt anchor; shadow_project(C, ZERO, ofx, ofy, v, avx, avy, avz, &anchor);

    /* Hand the depth-aware shadow decal the camera reconstruction params (so the
     * shader can recover each terrain pixel's world-Y from the depth pre-pass and
     * discard wall pixels by HEIGHT above the shadow's ground plane — keeping the
     * flat fan's look while fixing wall punch-through). Display-only (never
     * written back to PSX RAM); refreshed every extract incl. the sub-tick
     * re-extract, so it tracks the interpolated camera. cam_col0 = camera matrix
     * column 0; worldY = cam_pos_y - (Cᵀ·MAC)[0]/4096 (validated: recovers
     * SpyroShadow.Y to ~0.04%). */
    {
        openpete_shadow_decal_params p;
        shadow_push_decal_params(cpu, v, C, ofx, ofy, H, &p);
        /* OPENPETE_SHADOW_DECAL_DEBUG: validate the FULL world reconstruction
         * (all three camera columns + swizzle inverse) CPU-side against the
         * anchor's known world position — the shader runs the same formulas. */
        if (getenv("OPENPETE_SHADOW_DECAL_DEBUG")) {
            /* Validate with the PUSHED inverse — the same math the shader runs. */
            shadow_pt a2; shadow_project(C, ZERO, ofx, ofy, v, avx, avy, avz, &a2);
            double vz = a2.sz;
            double M1 = (a2.sx - ofx) * vz / H, M2 = (a2.sy - ofy) * vz / H, M3 = vz;
            double V0 = (p.cam_col0[0]*M1 + p.cam_col0[1]*M2 + p.cam_col0[2]*M3) / 4096.0;
            double V1 = (p.cam_col1[0]*M1 + p.cam_col1[1]*M2 + p.cam_col1[2]*M3) / 4096.0;
            double V2 = (p.cam_col2[0]*M1 + p.cam_col2[1]*M2 + p.cam_col2[2]*M3) / 4096.0;
            static int n = 0;
            if ((n++ % 64) == 0) {
                fprintf(stderr, "[shdecal-dbg] recon=(%.0f %.0f %.0f) truth=(%d %d %d) sxy=(%.0f %.0f) vz=%.0f\n",
                        camx + V2, camy - V0, camz - V1, sposx, sposy, sposz, a2.sx, a2.sy, vz);
                fprintf(stderr, "[shdecal-dbg] C=[%d %d %d; %d %d %d; %d %d %d] of=(%.0f %.0f) h=%.0f camy=%d gy=%d\n",
                        C[0][0], C[0][1], C[0][2], C[1][0], C[1][1], C[1][2],
                        C[2][0], C[2][1], C[2][2], ofx, ofy, H, camy, sposy);
            }
        }
    }

    /* heading matrix */
    int heading = (int)cpu->read_byte(G_SPYRO + 0x0E);
    int Rh[3][3]; build_heading_matrix(cpu, C, heading, Rh);

    /* silhouette source: model keyframes A/B (hi-detail; g_Spyro+0x24 != 0) */
    uint32_t models0 = cpu->read_word(G_MODELS);
    if (models0 < 0x80000000u || models0 >= 0x80800000u) return;
    uint32_t selX = cpu->read_half(G_SPYRO + 0x18), selY = cpu->read_half(G_SPYRO + 0x1E);
    int factor = (int)cpu->read_byte(G_SPYRO + 0x24);
    uint8_t sil[9]; int8_t yv[9];
    if (factor != 0) {                  /* hi-detail: lerp two keyframes */
        uint32_t subA = cpu->read_word(models0 + ((selX & 0xFF) << 2) + 0x38);
        uint32_t subB = cpu->read_word(models0 + (((selX >> 8) & 0xFF) << 2) + 0x38);
        /* Spyro's frame words carry the stream address halved (or a handle); the resolver undoes both. */
        uint32_t vmA = openpete_vstream_addr(cpu->read_word(subA + ((selY & 0xFF) << 2) + 0x24) & 0x1FFFFFu, 1);
        uint32_t vmB = openpete_vstream_addr(cpu->read_word(subB + (((selY >> 8) & 0xFF) << 2) + 0x24) & 0x1FFFFFu, 1);
        uint32_t aw[2] = { cpu->read_word(vmA), cpu->read_word(vmA+4) };
        uint32_t bw[2] = { cpu->read_word(vmB), cpu->read_word(vmB+4) };
        for (int i = 0; i < 8; i++) {
            int a = (int)((aw[i>>2] >> ((i&3)*8)) & 0xFF);
            int b = (int)((bw[i>>2] >> ((i&3)*8)) & 0xFF);
            sil[i] = (uint8_t)(((a * (16 - factor)) + (b * factor)) >> 4);
        }
    } else {                            /* lo-detail: single keyframe (selX.lo) */
        uint32_t sub = cpu->read_word(models0 + ((selX & 0xFF) << 2) + 0x38);
        uint32_t vm = openpete_vstream_addr(cpu->read_word(sub + ((selY & 0xFF) << 2) + 0x24) & 0x1FFFFFu, 1);
        uint32_t w[2] = { cpu->read_word(vm), cpu->read_word(vm+4) };
        for (int i = 0; i < 8; i++) sil[i] = (uint8_t)((w[i>>2] >> ((i&3)*8)) & 0xFF);
    }
    sil[8] = sil[0];                    /* doubling-loop wrap (the .s reads scratch[0x08]=w0.b0) */
    /* Y source = SpyroShadow struct words 0,1 (signed bytes) */
    uint32_t yw[2] = { cpu->read_word(D_SPYROSHADOW + 0), cpu->read_word(D_SPYROSHADOW + 4) };
    for (int i = 0; i < 8; i++) yv[i] = (int8_t)((yw[i>>2] >> ((i&3)*8)) & 0xFF);
    yv[8] = yv[0];
    /* over-shadow flag: scale source points by 3/4 before doubling */
    int oflag = (cpu->read_word(D_SPYROSHADOW + 0x08) | cpu->read_word(D_SPYROSHADOW + 0x0C)) != 0;

    /* double 8 source points -> 16 ring points (point + midpoint) */
    int ring_r[16], ring_y[16];
    for (int i = 0; i < 8; i++) {
        int r0 = sil[i], r1 = sil[i+1];
        int y0 = yv[i],  y1 = yv[i+1];
        if (oflag) { r0=(r0*3)>>2; r1=(r1*3)>>2; y0=(y0*3)>>2; y1=(y1*3)>>2; }
        ring_r[2*i]   = r0 & 0xFF;          ring_y[2*i]   = (int8_t)y0;
        ring_r[2*i+1] = ((r0 + r1) >> 1) & 0xFF;  ring_y[2*i+1] = (int8_t)((y0 + y1) >> 1);
    }

    /* project the 16 ring points through heading + anchor TR */
    shadow_pt strip[16];
    for (int k = 0; k < 16; k++) {
        uint32_t rw = cpu->read_word(D_RING + (uint32_t)k*4);
        int rlo = shadow_s16((int)(rw & 0xFFFF)), rhi = shadow_s16((int)(rw >> 16));
        /* float (not the .s >>10 int floor): sub-pixel ring-corner local offset.
         * ring_r/ring_y stay byte-domain (genuine silhouette source); only the
         * direction scale is floated, like the dragon-star spoke. */
        double VX = (double)ring_r[k] * rhi / 1024.0;
        double VY = (double)ring_y[k] * 4.0;
        double VZ = (double)ring_r[k] * rlo / 1024.0;
        shadow_project(Rh, TR, ofx, ofy, v, VX, VY, VZ, &strip[k]);
    }

    /* G3 fan: (anchor, strip[k], strip[k+1]); color0=0x608080, c1=c2=0 */
    for (int k = 0; k < 16; k++) {
        if (s_sptri_n >= MAX_SPTRI) break;
        sptri* t = &s_sptri[s_sptri_n++];
        shadow_pt* p1 = &strip[k]; shadow_pt* p2 = &strip[(k+1) & 15];
        t->sx[0]=anchor.sx; t->sy[0]=anchor.sy; t->sz[0]=anchor.sz; t->rgb[0]=0x608080u;
        t->sx[1]=p1->sx;    t->sy[1]=p1->sy;    t->sz[1]=p1->sz;    t->rgb[1]=0;
        t->sx[2]=p2->sx;    t->sy[2]=p2->sy;    t->sz[2]=p2->sz;    t->rgb[2]=0;
    }
}

/* feed the native Spyro shadow (untextured G3 fan, ABR mode 2 -> blend=3).
 * `slot`: the model record the shadow renders under (spyro_records.h), the
 * PLAYER slot of its tag. */
static void shadow_spyro_feed_native(CPUState* cpu, int slot) {
    if (!shadow_player_feed_enabled()) return;
    shadow_spyro_build(cpu);
    if (s_sptri_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_SHADOW(OPENPETE_TAG_SHADOW_PLAYER, (uint32_t)(slot < 0 ? 0 : slot)));
    for (int i = 0; i < s_sptri_n; i++) {
        sptri* c = &s_sptri[i];
        openpete_scene_tri st = {0};
        float bias = shadow_bias_spyro();
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = c->sx[k]; st.v[k].sy = c->sy[k];
            st.v[k].z  = shadow_depth(c->sz[k], 1.0f, bias);  /* already full-world MAC3 */
            st.v[k].pvz = shadow_depth(c->sz[k], 1.0f, 1.0f); /* true (unbiased) view-Z */
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (c->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0; st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
    openpete_native_scene_set_tag(0);
}

/* A/B: native Spyro-shadow fan vs the captured G3 prims (.s int16 SXY oracle).
 * Same fan order (anchor, strip[k], strip[k+1]); compare element-wise.
 * Reports VACUOUS when the routine emitted no untextured triangle. */
static void shadow_spyro_compare(CPUState* cpu) {
    shadow_spyro_build(cpu);
    int np = shadow_prim_count();
    int g3 = 0;
    for (int p = 0; p < np; p++) {
        const shadow_prim* pr = shadow_prim_at(p);
        if (!pr->textured && pr->nv == 3) g3++;
    }
    if (g3 == 0 || s_sptri_n == 0) {
        fprintf(stderr, "[shadow-spyro f=%u] VACUOUS native=%d G3=%d (nothing compared)\n",
                openpete_frame, s_sptri_n, g3);
        return;
    }
    int n = s_sptri_n < g3 ? s_sptri_n : g3;
    double worst = 0, sum = 0; int over2 = 0, nv = 0, qi = 0, cbad = 0;
    for (int p = 0; p < np && qi < n; p++) {
        const shadow_prim* pr = shadow_prim_at(p);
        if (!(!pr->textured && pr->nv == 3)) continue;
        sptri* c = &s_sptri[qi];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = c->sx[k]-pr->x[k], dy = c->sy[k]-pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > tw) tw = d;
            if (c->rgb[k] != pr->rgb[k]) cbad++;
        }
        if (tw > worst) worst = tw;
        if (tw > 2.0) over2++;
        qi++;
    }
    fprintf(stderr, "[shadow-spyro f=%u] native=%d G3=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx "
                    "over2px=%d/%d rgbMismatch=%d\n",
            openpete_frame, s_sptri_n, g3, n, nv?sum/nv:0, worst, over2, n, cbad);
}

static void shadow_spyro_run_draw(CPUState* cpu, uint32_t addr) { (void)addr; func_80059A48(cpu); }

static hook_bracket_state s_shadow_spyro_hb;
static const hook_bracket_desc k_shadow_spyro_bracket = {
    .path = RP_SHADOW, .prof_tag = "shadow.srend",
    .run = shadow_spyro_run_draw, .verify_enabled = shadow_verify_enabled,
    .st = &s_shadow_spyro_hb,
};

static int on_80059A48_shadow_spyro(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_shadow_spyro_bracket, &startp, &endp, &feed, &verify))
        return 0;
    int slot = 0;
    if (endp > startp) slot = player_records_shadow_append(cpu);   /* the shadow record, bound to its model record */

    if ((feed || verify) && endp > startp) shadow_capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_SHADOW)) {
        shadow_spyro_feed_native(cpu, slot);         /* native gamestate walk */
    }

    if (verify && endp > startp && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { shadow_spyro_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) { shadow_spyro_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_shadow_spyro_bracket);
}

/* The Spyro drop-shadow of one shadow record: the record loop in
 * reextract.c calls this once per record with that record's
 * g_Spyro fields and anchor staged. Same gates as the moby shadows. */
void shadow_spyro_reextract(CPUState* cpu, int slot) {
    if (!hook_reextract_ready(RP_SHADOW)) return;
    shadow_spyro_feed_native(cpu, slot);
}

static const feature_dispatch_entry_t shadow_spyro_dispatch[] = {
    { 0x80059A48u, on_80059A48_shadow_spyro },
    { 0u, NULL },
};

const feature_module_t shadow_spyro_module = {
    .name           = "shadow_spyro_native",
    .apply_options  = NULL,
    .dispatch_table = shadow_spyro_dispatch,
};
