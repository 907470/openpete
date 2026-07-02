// tracer_native.c — native gamestate re-derivation of the additive "tracer/beam"
// renderer (func_800189F0 -> func_800189F8), e.g. the dream-weavers fireball's
// glowing streak. Re-derived in FLOAT from the tracer-point gamestate (no
// capture of the .s's already-projected int16 prims), so it has true per-vertex
// view-Z depth, high-FPS sub-tick support, and no quantization.
//
// Tracer data (drained + zeroed by the .s every frame, so snapshot at hook
// entry before the real renderer runs):
//   0x80075684            = active tracer-SET count (then zeroed)
//   0x800772C8 + set*4     = per-set point count
//   0x80078658 + set*4     = per-set points-base pointer
//   0x8007596C             = g_LevelId (==63 Gnasty Gnorc boss: width>>2 + R<->G
//                            colour swap, i.e. the white->red gradient -> green)
//   each point = 28-byte record: world XYZ @+0/+4/+8, fade @+24
//     (+12/+16/+20 are the .s's int16 SX/SY/viewZ — we re-project instead).
//
// Algorithm (mirrors func_800189F8, all float):
//   project each point via func_80017B48's permuted/negated axes
//     V = (camY-Y, camZ-Z, X-camX); MAC = camRot * V / 4096; viewZ = MAC3;
//     SX = OFX + MAC1*H/MAC3, SY = OFY + MAC2*H/MAC3   (pure float, no GTE clamp)
//   per point i (fade f, skipped when f>=81):
//     width   = (625*i)<<5   (>>2 if g_LevelId==63)
//     dir     = next point screen delta (dx,dy); len = hypot(dx,dy)
//     perp    = (dy,-dx) * width / (viewZ*len)        (perspective ribbon)
//     corners = P +/- perp ; colour = (150-2f, 150-4f, 150-10f) clamped,
//               R<->G swapped at g_LevelId==63 (Gnasty's green tracers)
//   emit a gouraud G4 (additive, ABR1) spanning prev corners -> curr corners,
//   once >=2 points emitted and viewZ < 256000.
//
// Display-only: the real renderer still runs (verdict/state), we read only the
// tracer-point gamestate and feed the native scene. Oracle inert.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "feature_registry.h"
#include "psx_runtime.h"
#include "openpete_native_render.h"
#include "render_paths.h"
#include "spyro_native_reextract.h"   /* sub-tick re-extract entry */
#include "spyro1_symbols.gen.h"

extern void     func_800189F0(CPUState* cpu);   /* the real tracer/beam renderer */
extern unsigned openpete_frame;


#define MAX_SETS 16
#define MAX_PTS  72

typedef struct { int x, y, z, f; } tpoint;
typedef struct { int count; tpoint pt[MAX_PTS]; } tset;
static tset s_sets[MAX_SETS];
static int  s_nsets;                 /* sets captured at the last canonical frame */

static int s16i(uint32_t v) { return (int)(int16_t)(uint16_t)v; }

/* Snapshot the tracer-point gamestate before the .s drains it. */
static void tracer_snapshot(CPUState* cpu) {
    s_nsets = 0;
    uint32_t nsets = cpu->read_word(TRACER_COUNT);
    for (uint32_t s = 0; s < nsets && s_nsets < MAX_SETS; s++) {
        int count = (int)cpu->read_word(TRACER_CNTTAB + s * 4u);
        uint32_t base = cpu->read_word(TRACER_BASETAB + s * 4u);
        if (count <= 0 || base < 0x80000000u) continue;
        if (count > MAX_PTS) count = MAX_PTS;
        tset* ts = &s_sets[s_nsets++];
        ts->count = count;
        for (int i = 0; i < count; i++) {
            uint32_t p = base + (uint32_t)i * 28u;
            ts->pt[i].x = (int)cpu->read_word(p + 0u);
            ts->pt[i].y = (int)cpu->read_word(p + 4u);
            ts->pt[i].z = (int)cpu->read_word(p + 8u);
            ts->pt[i].f = (int)cpu->read_word(p + 24u);
        }
    }
}

/* func_80017B48 in float: world point -> screen (sx,sy) + true full-scale
 * view-Z. Permuted/negated axes; pure float (no GTE int16 clamp / div cap). */
static int tracer_project(const double R[3][3], const double cam[3],
                         double ofx, double ofy, double H,
                         int X, int Y, int Z,
                         double* sx, double* sy, double* vz) {
    double V0 = cam[1] - Y;   /* camY - Y */
    double V1 = cam[2] - Z;   /* camZ - Z */
    double V2 = X - cam[0];   /* X - camX */
    double m1 = (R[0][0]*V0 + R[0][1]*V1 + R[0][2]*V2) / 4096.0;
    double m2 = (R[1][0]*V0 + R[1][1]*V1 + R[1][2]*V2) / 4096.0;
    double m3 = (R[2][0]*V0 + R[2][1]*V1 + R[2][2]*V2) / 4096.0;
    *vz = m3;
    if (m3 <= 1.0) { *sx = *sy = 0.0; return 0; }   /* at/behind camera */
    *sx = ofx + m1 * H / m3;
    *sy = ofy + m2 * H / m3;
    return 1;
}

static void emit_tri(double ax, double ay, double az, uint32_t acol,
                     double bx, double by, double bz, uint32_t bcol,
                     double cx, double cy, double cz, uint32_t ccol) {
    openpete_scene_tri st;
    memset(&st, 0, sizeof st);
    st.textured = 0;
    st.blend    = 2;                          /* ABR mode 1 (additive) */
    st.v[0].sx = (float)ax; st.v[0].sy = (float)ay; st.v[0].z = (float)az; st.v[0].rgba = acol | 0xFF000000u;
    st.v[1].sx = (float)bx; st.v[1].sy = (float)by; st.v[1].z = (float)bz; st.v[1].rgba = bcol | 0xFF000000u;
    st.v[2].sx = (float)cx; st.v[2].sy = (float)cy; st.v[2].z = (float)cz; st.v[2].rgba = ccol | 0xFF000000u;
    openpete_native_scene_add_tri(&st, OPENPETE_SCENE_ACTOR, 0);  /* category ignored for blend!=0 */
}

/* Project the snapshot points (against the CURRENT camera — interpolated on
 * sub-ticks) and feed the additive ribbon with true per-vertex depth. */
static void tracer_build_and_feed(CPUState* cpu) {
    if (s_nsets <= 0) return;
    double R[3][3], cam[3];
    uint32_t w0 = cpu->read_word(G_CAMERA + 0),  w1 = cpu->read_word(G_CAMERA + 4),
             w2 = cpu->read_word(G_CAMERA + 8),  w3 = cpu->read_word(G_CAMERA + 12),
             w4 = cpu->read_word(G_CAMERA + 16);
    R[0][0]=s16i(w0&0xFFFF); R[0][1]=s16i(w0>>16); R[0][2]=s16i(w1&0xFFFF);
    R[1][0]=s16i(w1>>16);    R[1][1]=s16i(w2&0xFFFF); R[1][2]=s16i(w2>>16);
    R[2][0]=s16i(w3&0xFFFF); R[2][1]=s16i(w3>>16); R[2][2]=s16i(w4&0xFFFF);
    cam[0]=(int32_t)cpu->read_word(G_CAMERA + 0x28);
    cam[1]=(int32_t)cpu->read_word(G_CAMERA + 0x2C);
    cam[2]=(int32_t)cpu->read_word(G_CAMERA + 0x30);
    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFFu);
    int is_gnasty = (cpu->read_word(G_LEVELID) == 63u);   /* level 63: thin + green */
    int wshift    = is_gnasty ? 2 : 0;

    openpete_native_scene_begin_frame(openpete_frame);

    for (int s = 0; s < s_nsets; s++) {
        tset* ts = &s_sets[s];
        int    havePrev = 0, emitted = 0;
        double pc0x=0,pc0y=0,pc1x=0,pc1y=0,pvz=0; uint32_t pcol = 0;
        /* The .s ribbon loop runs s6 = 0..count-2 (block_80018A9C requires
         * count>=2; block_80018EB4 continues while s6 < count-1). The last
         * point (count-1) is projected in the pre-pass but only consumed as the
         * direction target for point count-2's segment — never drawn. Point
         * count-2 takes width 0 (the tip pinch), so the final segment tapers to
         * a point. Iterating to count-1 here would emit an extra full-width
         * segment whose reversed neighbour direction crosses over past the tip. */
        for (int i = 0; i < ts->count - 1; i++) {
            int f = ts->pt[i].f;
            if (f >= 81) continue;            /* fully faded -> skip (prev unchanged) */
            double sx, sy, vz;
            if (!tracer_project(R, cam, ofx, ofy, H, ts->pt[i].x, ts->pt[i].y, ts->pt[i].z, &sx, &sy, &vz))
                continue;
            /* screen direction to the neighbouring point (next; backward at the end) */
            int j = (i + 1 < ts->count) ? i + 1 : i - 1;
            double dx = 0.0, dy = 0.0;
            if (j >= 0) {
                double nx, ny, nvz;
                if (tracer_project(R, cam, ofx, ofy, H, ts->pt[j].x, ts->pt[j].y, ts->pt[j].z, &nx, &ny, &nvz)) {
                    dx = nx - sx; dy = ny - sy;
                }
            }
            double len = sqrt(dx*dx + dy*dy);
            /* Ribbon half-width (func_800189F8 block_80018AC8/AFC): the first 3
             * points ramp via the per-point accumulator ((625*i)<<5 = 0/20000/
             * 40000); the bulk is a constant 0x11170 (70000); the second-to-last
             * point is 0 (the ribbon pinches at the tip). (>>2 when cfg==63.) */
            int wi = (i < 3) ? ((625 * i) << 5)
                             : ((i == ts->count - 2) ? 0 : 0x11170);
            if (wshift) wi >>= 2;
            double width = (double)wi;
            double px = 0.0, py = 0.0;
            if (len > 1e-6 && vz > 1.0) {
                double scale = width / (vz * len);       /* perspective ribbon half-width */
                px = dy * scale; py = -dx * scale;
            }
            int Rc = 150 - 2 * f, Gc = 150 - 4 * f, Bc = 150 - 10 * f;
            if (Rc < 0) Rc = 0; if (Gc < 0) Gc = 0; if (Bc < 0) Bc = 0;
            if (is_gnasty) { int t = Rc; Rc = Gc; Gc = t; }  /* func_800189F8: g_LevelId==63 swaps R<->G */
            uint32_t col = (uint32_t)(Rc | (Gc << 8) | (Bc << 16));
            double c0x = sx + px, c0y = sy + py;
            double c1x = sx - px, c1y = sy - py;
            emitted++;
            if (emitted >= 2 && havePrev && vz < 256000.0) {
                double dz = openpete_native_depth_from_viewz((float)vz);
                double pz = openpete_native_depth_from_viewz((float)pvz);
                /* G4: v0=c0(cur) v1=c1(cur) v2=pc0(prev) v3=pc1(prev) -> tris (0,1,2)(1,3,2) */
                emit_tri(c0x,c0y,dz,col,  c1x,c1y,dz,col,  pc0x,pc0y,pz,pcol);
                emit_tri(c1x,c1y,dz,col,  pc1x,pc1y,pz,pcol, pc0x,pc0y,pz,pcol);
            }
            pc0x=c0x; pc0y=c0y; pc1x=c1x; pc1y=c1y; pvz=vz; pcol=col; havePrev=1;
        }
    }
}

/* Dispatch hook: snapshot the tracer gamestate (before the .s drains it), run
 * the real renderer (verdict), then re-derive + feed natively. */
static int on_800189F0_tracer(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int feed = openpete_native_render_enabled();
    if (!feed && !render_paths_active()) return 0;        /* inert: normal dispatch */

    static int s_in = 0;
    if (s_in) return 0;                                   /* re-entrancy guard */
    s_in = 1;

    if (feed) tracer_snapshot(cpu);                        /* capture before the drain */
    uint32_t pb0 = cpu->read_word(POLYBUF_CURSOR);
    func_800189F0(cpu);                                   /* real renderer (verdict) */
    /* Record the tracer's emitted polybuf range as RP_TRACER: drives the OT-filter
     * isolation AND the canon-drawn liveness gate the sub-tick re-extract reads.
     * Empty when no tracer drew (render_path_record drops empty ranges), so a frame
     * with no tracer reads as "not drawn" and the spine won't replay a stale beam. */
    render_path_record(RP_TRACER, pb0, cpu->read_word(POLYBUF_CURSOR));
    if (feed && s_nsets > 0 && render_path_enabled(RP_TRACER))
        tracer_build_and_feed(cpu);
    s_in = 0;
    return 1;
}

/* Sub-tick re-extract: replay the snapshot against the interpolated camera. */
void tracer_native_reextract(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    if (!render_path_enabled(RP_TRACER)) return;
    /* Liveness gate (matches every other feeder): only replay if the most recent
     * canonical frame actually drew a tracer. Without this, when the canonical
     * tracer render pauses (sim stall, level transition) while the high-FPS spine
     * keeps presenting, the intermediate presents replayed the last beam as a ghost
     * over a scene whose mobys/particles had already stopped rendering. */
    if (!render_path_canon_drawn(RP_TRACER)) return;
    tracer_build_and_feed(cpu);
}

static const feature_dispatch_entry_t tracer_dispatch[] = {
    { 0x800189F0u, on_800189F0_tracer },
    { 0u, NULL },
};

const feature_module_t tracer_native_module = {
    .name           = "tracer_native",
    .apply_options  = NULL,
    .dispatch_table = tracer_dispatch,
};
