// games/common/renderer/native_gte.h — the ONE copy of the GTE
// float-mirror math shared by every native feeder of every game.
//
// Two projection families exist on purpose:
//
//   * ngte_project_raw — unclamped float H/z (moby, cyclorama). More
//     correct than the GTE for widescreen reveal; callers own their
//     near-plane policy on the returned view-Z.
//   * ngte_project_gte / ngte_screen_gte — the GTE-faithful pipeline
//     (IR16 saturation, SZ3 clamp, 0x1FFFF divide cap, optional ±1024
//     SXY bound) used where PSX screen positions must be matched to the
//     unit (spyro, flame, shadow, particle — particle drops the SXY
//     bound: widescreen reveal needs the true X).
//
// Near-plane z-floors deliberately stay in the CALLERS: they encode
// per-feeder policy (moby !=0→1.0, portal >1.0, terrain 1e-9, tracer
// reject ≤1.0) and unifying them is a behaviour change, not consolidation.

#ifndef OPENPETE_NATIVE_GTE_H
#define OPENPETE_NATIVE_GTE_H

#include <stdint.h>
#include "psx_runtime_internal.h"

/* GTE UNR divide cap (0x1FFFF in 16.16) and SXY output saturation. */
#define NGTE_DIV_CAP   (131071.0 / 65536.0)
#define NGTE_SXY_BOUND 1024.0

/* The game's camera matrix scales its screen-Y row by this (row norms
 * 4096/2560/4096): the vertical FOV of a clip_h-row band is
 * 2·atan((clip_h/2) / (NGTE_Y_ROW · H)). */
#define NGTE_Y_ROW 0.625

/* One projection view per producer pass: every input a feeder needs to
 * project into PSX screen space, read once from the CPU state the pass
 * runs under (the staged g_Camera, which a held producer camera swaps) and
 * from the renderer's display policy. Built inside the producer-camera
 * bracket where one exists, so the bracket's integer-camera rule
 * (openpete_native_interp_pos returns 0 for the camera) reaches cam[].
 *
 * H_canon is the GTE's H: every hardware-limit mirror (IR16 saturation,
 * divide cap, near-fixup regime, UNR overflow regime) reads it. H_eff =
 * H_canon · s is the projection distance the feeder draws with; s is the
 * --fov scale (1.0 exactly at the default, so H_eff == H_canon
 * bit-for-bit). F / xlo / xhi are the Hor+ factor and its screen-X range;
 * extended is 1 when the view shows more than the canonical 4:3 frame
 * (F > 1 or s < 1) and the feeders must admit what the game's verdicts
 * dropped. */
typedef struct {
    int    cam_M[3][3];   /* g_Camera projection matrix (+0x00), Q12 */
    double cam_ram[3];    /* camera position, integer staged RAM (+0x28..0x30) */
    double cam[3];        /* camera position, sub-tick interpolated when
                           * available, else cam_ram */
    int    cam_interp;    /* 1 = cam[] came from the interp spine */
    double ofx, ofy;      /* gte_ctrl[24], [25] / 65536 */
    double H_canon;       /* gte_ctrl[26] */
    double s;             /* --fov scale, latched per present */
    double H_eff;         /* H_canon · s */
    float  F, xlo, xhi;   /* Hor+ factor and screen-X bounds */
    int    extended;      /* view wider than the canonical 4:3 frame */
} ngte_view;

void ngte_view_build(CPUState* cpu, ngte_view* v);

/* The view's camera in the form the occlusion grid compares: position in
 * full-world units and the camera matrix with each row scaled to unit length.
 * The game's matrix is Q12 and not exactly orthonormal, so the rows are
 * normalized here; the grid's bound is then a plain nine-element dot product
 * against the grid camera's matrix. */
struct openpete_occl_camera;
void ngte_occl_camera(const ngte_view* v, struct openpete_occl_camera* c);

/* Engagement census of the --fov policy (OPENPETE_FOV_CENSUS=1 arms it and
 * prints at exit). Every GTE hardware-limit mirror keys on H_canon; each
 * site also evaluates the same test at H_eff and both verdicts are counted,
 * so one run shows per site that the regime engaged (hit > 0), that the
 * display H would have moved it (eff != hit when s != 1) and that it did
 * not (hit is the same code at every FOV; at s = 1 the two are equal).
 * sxy_bound counts the SXY pin on the scaled output, which moves with s by
 * design. bb_untagged counts shaded-walk billboard records with no HUD tag
 * (expected 0: every billboard anchor is a game-authored screen
 * coordinate). reveal_admitted counts mobys the reveal admitted host-side
 * (0 at the canonical view), and reveal_spec_solo the subset of those whose
 * renderer byte selects the specular pass, which is the subset the walk gives
 * the special per-face material to. */
enum { NGTE_SITE_RAW_SAT, NGTE_SITE_GTE_SAT, NGTE_SITE_PART_CAP,
       NGTE_SITE_MOBY_NEAR, NGTE_SITE_TERRAIN_UNR, NGTE_SITE_N };
typedef struct {
    unsigned long hit[NGTE_SITE_N];   /* the limit test at H_canon */
    unsigned long eff[NGTE_SITE_N];   /* the same test at H_eff */
    unsigned long sxy_bound;
    unsigned long bb_untagged;
    unsigned long reveal_admitted;
    unsigned long reveal_spec_solo;
} ngte_census_t;
extern ngte_census_t ngte_census;
extern int ngte_census_on;
#define NGTE_CENSUS_HIT(field) \
    ((void)(ngte_census_on ? __atomic_fetch_add(&ngte_census.field, 1ul, __ATOMIC_RELAXED) : 0ul))
/* One limit site: canon is the verdict the code acts on, eff the verdict the
 * display H would have given. */
#define NGTE_LIMIT(site, canon, eff_) \
    ((void)(ngte_census_on ? ((canon) ? __atomic_fetch_add(&ngte_census.hit[site], 1ul, __ATOMIC_RELAXED) : 0ul, \
                              (eff_)  ? __atomic_fetch_add(&ngte_census.eff[site], 1ul, __ATOMIC_RELAXED) : 0ul) : 0ul))
/* *c = *v with s = 1.0, H_eff = H_canon: the view for records whose screen
 * position the game authored (HUD, billboards) and for instruments that
 * compare against canonical prims. */
void ngte_view_canon(const ngte_view* v, ngte_view* c);

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from raw words —
 * for callers whose source is the GTE control regs, not RAM. */
void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]);

/* Unpack a GTE SHORTMATRIX (5 words = 9 packed int16) from PSX RAM. */
void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]);

/* MAC1/2/3 = (M · v) / 4096 — the rotate step alone, no TR, no divide. */
void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3);

/* IR1/2/3 as the hardware hands them back with lm = 0: MAC saturated to signed
 * 16 bits. A walk that mirrors a draw reading IR after MVMVA needs the
 * saturation, since the unsaturated MAC differs wherever the sector is far
 * enough for the product to overflow. */
static inline double ngte_ir_clamp(double mac) {
    return mac < -32768.0 ? -32768.0 : (mac > 32767.0 ? 32767.0 : mac);
}

/* Raw-float projection family. TR may be NULL (treated as zero). ofx/ofy
 * are per-call (the shaded walk's billboards project about their own
 * anchor); H comes from the view.
 * z-floor: mac3 != 0 ? mac3 : 1.0 (exactly-zero guard only — callers
 * cull/clip on the RETURNED mac3, which is unfloored). out_sat mirrors
 * the GTE's would-have-saturated flags. Returns mac3 (view-Z). */
double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, const ngte_view* v,
                        double* out_sx, double* out_sy, int* out_sat);

/* GTE-faithful projection family output. m1/m2/m3 are the UNsaturated
 * MACs (particle stores them); sx/sy are post-IR16-clamp screen coords. */
typedef struct {
    double sx, sy;
    double m1, m2, m3;
    int sat;              /* SZ3 <= 0 or H >= 2*SZ3 (divide capped) */
} ngte_proj;

/* The divide half alone, for callers that already have view-space MACs. */
void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, const ngte_view* v,
                     int sxy_clamp, ngte_proj* o);

/* Full GTE-faithful projection. TR may be NULL (treated as zero). */
void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, const ngte_view* v,
                      int sxy_clamp, ngte_proj* o);

/* GTE DPCS single channel, bit-exact integer mirror: c8 = 8-bit colour,
 * fc12 = 12-bit-expanded far colour, ir0 = 1.3.12 blend factor. */
int ngte_dpcs_ch(int c8, int fc12, int ir0);

/* GTE DPCS in float: lerp every 8-bit channel of rgb toward the matching
 * channel of fc by ir0/4096, rounded and clamped to 0..255. Both are packed
 * 0x00BBGGRR. The sky and portal-preview draws run the real op (cmd 0x10,
 * sf=1) over a colour table with FC = the fog far colour's components SHL 4
 * and IR0 = the fog factor; FC SHL 12 then lines up with the colour's SHL 16,
 * so the op reduces to this per-channel lerp. The float form is what the rest
 * of the native path carries; ngte_dpcs_ch is the bit-exact integer mirror
 * for the walks that must match the GTE to the unit. */
uint32_t ngte_depth_cue(uint32_t rgb, uint32_t fc, int ir0);

/* The fog far colour (game_layout.fog.far_colour) as packed 0x00BBGGRR: one
 * word holding the three 8-bit components, or, when fog.far_colour_words is
 * set, three words R, G, B at the GTE's RFC/GFC/BFC scale (16 times the
 * 8-bit value). 0 when the game leaves the address unset. */
uint32_t ngte_fog_far_colour(CPUState* cpu);

/* O = (A · B) >> 12 on Q12 3x3 matrices, the column-rotate the game does
 * via MVMVA. Accumulates in 64 bits: the GTE's MAC is 44 bits wide, and
 * a scaled model matrix times a rotation exceeds int32 before the shift. */
void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]);

/* The game's in-RAM sin/cos table (game_layout.sin_table): 256 entries per
 * circle, int16 Q12, cos at +0x80 bytes. A byte angle indexes it at byte<<1.
 * A game whose table is unlocated (the field is 0) gets sin 0 / cos 4096 from
 * every helper below instead of a read through address 0. */

/* sin/cos at fractional byte-angle resolution: lerp adjacent table
 * entries. Exact at integer angles, smooth between, so a rotation renders
 * below the 1.4°/step byte grid. fb wraps into [0,256). */
void ngte_sincos_f(CPUState* cpu, double fb, double* s_out, double* c_out);

/* ngte_sincos_f rounded to the nearest int for Q12 matrix composition. */
void ngte_sincos_interp(CPUState* cpu, double fb, int* s_out, int* c_out);

/* Project a camera-relative offset the way every self-linking producer in
 * this engine does: the offset ordered (X - camX, camY - Y, camZ - Z) in the
 * pool's own world units, rotated by the camera matrix with a zero
 * translation, then divided. The SXY bound is dropped, as the particle feeder
 * drops it, because widescreen reveal needs the true X. */
void ngte_project_offset(const ngte_view* v, const double off[3], ngte_proj* o);

/* The rotation carrying a set of screen-space corner extents from the table
 * vector one angle names to the vector another names, as the cos/sin of the
 * change. Both angles are in table entries, 256 to the turn; a routine that
 * indexes the table at a fixed offset from its record's angle byte folds that
 * offset into both. The extents ride the table's own vector, so the change is
 * the rotation between the two table vectors: exact whatever the table's
 * phase, sense and amplitude, because the amplitude divides out. Writes the
 * identity and returns 0 when the two angles coincide or the table is
 * unlocated. */
int ngte_table_rot(CPUState* cpu, double from_ang, double to_ang,
                   double* rc, double* rs);

/* Byte-exact table lookups for a packed euler word (Z=byte0, X=byte1,
 * Y=byte2; m_Rotation@+0x44). rot==0 gives sin 0 / cos 4096 on every axis. */
typedef struct { int sy, cy, sx, cx, sz, cz; } ngte_euler_sc;
void ngte_euler_sincos(CPUState* cpu, uint32_t rot, ngte_euler_sc* e);

/* out = start·Ry·Rx·Rz in Q12, the order the game's MVMVA sequence applies
 * (func_80022A2C block_80022DBC..80022F34). */
void ngte_compose_euler(const int start[3][3], const ngte_euler_sc* e, int out[3][3]);

/* ---- texture page / CLUT bitfields --------------------------------------
 * One decode of the GPU's two texture words, shared by every feeder that
 * samples VRAM. `tp` is the texture-page bitfield (bits 0-3 page X in
 * 64-texel columns, bit 4 page Y in 256-line halves, bits 5-6 the
 * semi-transparency rate ABR, bits 7-8 the colour depth); `clut` is the
 * CLUT bitfield (bits 0-5 X in 16-entry columns, bits 6-14 Y in lines).
 * Callers own their ABE/STP predicate: this reports the raw ABR and leaves
 * the blend decision to ngte_abr_blend. */
typedef struct {
    int tpx, tpy;      /* texture page origin, VRAM texels / lines */
    int clx, cly;      /* CLUT origin, VRAM texels / lines */
    int bpp;           /* 4, 8 or 16; 32 for the replaced-texture escape */
    int abr;           /* raw semi-transparency rate, 0..3 */
    int mode;          /* raw colour-depth field, 0..3 */
    int replaced_id;   /* replaced-texture id, or -1 when there is none */
} ngte_tpage;

/* Colour depth 3 is reserved by the hardware, so an emitter can spend it on
 * naming a replaced texture whose id rides in the CLUT half. With this flag
 * mode 3 yields bpp 32, tpx = the signed id, replaced_id = the same id and a
 * zeroed CLUT. Without it mode 3 decodes as an ordinary 16bpp page, which is
 * what a feeder that never sees the escape expects. */
#define NGTE_TP_REPLACED_ID 0x1u

void ngte_decode_tpage(unsigned tp, unsigned clut, unsigned flags, ngte_tpage* out);

/* ABR to the renderer's blend code: 1 = (B+F)/2, 2 = B+F, 3 = B-F,
 * 4 = B+F/4. NGTE_ABR_EXACT is the hardware's own table. A feeder with no
 * B+F/4 pipeline passes NGTE_ABR_QUARTER_AS_ADD, which draws rate 3
 * additive. Blend code 0 (opaque) is the caller's, from its own ABE bit. */
typedef enum {
    NGTE_ABR_EXACT = 0,
    NGTE_ABR_QUARTER_AS_ADD = 1
} ngte_abr_policy;

int ngte_abr_blend(int abr, ngte_abr_policy policy);

/* ---- screen-space backface test ----------------------------------------
 * Signed 2*area of a screen triangle: the sign of the GTE's NCLIP MAC0,
 * which the game tests to drop a face. NGTE_NCLIP_SXY_GRID rounds each
 * coordinate to the GTE's int16 SXY grid first, so the verdict matches the
 * .s face for face; NGTE_NCLIP_FLOAT keeps float precision, which stops a
 * near-zero-area fan flipping sign at grazing angles and popping. */
typedef enum {
    NGTE_NCLIP_FLOAT = 0,
    NGTE_NCLIP_SXY_GRID = 1
} ngte_nclip_mode;

double ngte_nclip(double ax, double ay, double bx, double by,
                  double cx, double cy, ngte_nclip_mode mode);

/* Hex dump of `nwords` guest words from `base`, eight per line, under
 * `label`. Verify-mode diagnostic; writes to stderr. */
void ngte_dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords);

/* ---- ordering-table polybuf walk ----------------------------------------
 * A polybuf is a run of primitives laid out back to front. Each primitive
 * opens with its ordering-table tag: the tag's high byte is the primitive's
 * length in words, so the next primitive starts ((w0 >> 24) + 1) * 4 bytes
 * on, and the low 24 bits are the OT link. Word 1's high byte is the GPU
 * command code; codes 0x20..0x3F are polygons, with bit 4 gouraud, bit 3
 * quad, bit 2 textured and bit 1 semi-transparent, and the vertex words
 * follow in that order. A code outside that range (tiles, lines, draw-mode
 * mini-packets) decodes to nv = 0 and carries only its raw words.
 *
 * ngte_walk_prims is the one walk. Every bound a feeder disagrees on rides
 * in the policy; the decoded primitive goes to the feeder's sink, which
 * stores it in the feeder's own array, so no feeder's stored form changes.
 */
#define NGTE_PRIM_RAW_MAX 20

typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1] >> 24) */
    uint8_t  taglen;        /* OT tag length (word[0] >> 24) */
    int      nv;            /* 3 or 4 for a polygon, 0 otherwise */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad, stp;
    uint32_t raw[NGTE_PRIM_RAW_MAX];  /* words past nraw read zero */
    int      nraw;          /* raw words carried, capped by raw_words_max */
} ngte_prim;

/* Stores one primitive. Returns nonzero when it was stored: a sink whose
 * array is full returns 0, and the walk then leaves it out of the counts it
 * keeps per stored primitive. */
typedef int (*ngte_prim_sink)(void* ctx, const ngte_prim* prim);

typedef struct {
    uint32_t  tag_len_max;   /* a longer tag ends the walk as corrupt */
    int       raw_words_max; /* raw words the sink receives, 0 for none */
    int       guard;         /* primitives walked before the walk gives up */
    int       polygons_only; /* 1 = the sink sees polygons only */
    uint32_t* code_hist;     /* [256] by code byte, or NULL */
    uint32_t* tag_hist;      /* [256] by tag length, or NULL */
    uint32_t* total;         /* primitive counter, or NULL */
    int       count_all;     /* 1 = count every primitive walked, 0 = only
                              * the ones the sink stored */
} ngte_walk_policy;

void ngte_walk_prims(CPUState* cpu, uint32_t start, uint32_t end,
                     const ngte_walk_policy* pol, ngte_prim_sink sink, void* ctx);

/* ---- near-plane clip corner --------------------------------------------- */
/*
 * A corner in camera space, the form a Sutherland-Hodgman cut against a
 * constant-view-Z plane interpolates: position (m1, m2, m3), colour, UV.
 *
 * osx/osy/oflag carry an optional SCREEN-space override that a producer's
 * push step uses instead of projecting m1/m2/m3. A producer that projects
 * every corner leaves oflag zero. The terrain's near/HQ subdivide sets it on
 * its boundary midpoints under the integer vertex snap: a midpoint projected
 * from camera space lands sub-pixel off the parent's projected edge
 * (perspective), and the snap amplifies that into a visible T-junction crack
 * against the unsubdivided neighbour sharing that edge. The override places
 * the midpoint at the exact screen midpoint of the snapped parent corners
 * (integer or exact-.5, which the renderer's snap preserves), collinear with
 * the neighbour's snapped edge.
 *
 * fx/fy: a terrain producer's face-local coordinate of the corner (the
 * near drain's grid in quarter-edge units, psx_key_contest.glsl), carried
 * through a cut as the UVs are. Zero where no producer sets it.
 */
typedef struct { double m1, m2, m3; uint32_t rgb; float u, v;
                 float osx, osy; uint8_t oflag;
                 float fx, fy; } ngte_cvert;

/* The point where the edge a->b crosses the plane m3 == zc. Position, UV
 * and the face-local coordinate interpolate linearly in camera space; each colour channel rounds to nearest.
 * The parameter is clamped to [0,1], so an edge that does not cross returns
 * whichever endpoint the caller's inside test already picked. The result
 * projects normally: a crossing point is not one of the parent corners, so it
 * carries no screen override. */
ngte_cvert ngte_clip_lerp(ngte_cvert a, ngte_cvert b, double zc);

#endif /* OPENPETE_NATIVE_GTE_H */
