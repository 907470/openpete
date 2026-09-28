/* Same-moby paint rank — see moby_rank.h. The key table, its evaluator and
 * the integer RTPS / NCLIP mirror the rows read. */

#include "renderer/moby_rank.h"
#include "renderer/native_gte.h"   /* ngte_rtps_int */

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* ---- gate ---------------------------------------------------------------- */

int openpete_mobyrank_on(void) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = getenv("OPENPETE_MOBY_RANK");
        /* Unset or 0: the faces of one moby order by true depth alone. */
        s_on = (e && *e) ? (*e != '0') : 0;
    }
    return s_on;
}

/* ---- paint-key table -------------------------------------------------------
 * A row's arithmetic, per prim of one record:
 *   k = sum over the prim's vertices of (w[i] * SZ3[i]) >> 1 - base
 * (weights in halves: the S1 regular triangle's SZ + SZ/2 is weight 3), plus
 * `back_pre` on a back face; the prim is dropped when k < 0 (k <= 0 under
 * OTK_DROP_NONPOS). The face bias field b then enters as b * bias_mul buckets,
 * either before the shift (k += (b * bias_mul) << shift, dropped when that
 * goes negative) or after it (bucket += b * bias_mul, dropped when negative);
 * bucket = k >> shift, plus `back_post` on a back face.
 *
 * The record's base and shift: base = 4 * (TRZ - c), or under
 * OTK_BASE_X8_I16HALF 8 * int16((TRZ - c) >> 1), where c = base_c, shifted
 * left by the record's header byte `a` on a row that scales by it; a base
 * <= 0 is replaced by base_min (the draw then moves its bucket array base
 * instead, which keeps the record's relative order). shift = shift_add, plus
 * `a` on a row that scales by it. */
typedef enum { OTK_BASE_X4, OTK_BASE_X8_I16HALF } otk_base_form_t;
typedef enum { OTK_DROP_NEG, OTK_DROP_NONPOS } otk_drop_t;
typedef enum { OTK_BIAS_NONE, OTK_BIAS_PRE, OTK_BIAS_POST } otk_bias_t;
/* Which prim the draw makes of a face, from NCLIP on its integer SXY:
 *  DUAL  a triangle draws on NCLIP(v0,v1,v2) > 0 (!= 0 under the winding
 *        flag); a quad draws whole when NCLIP(v0,v1,v2) < 0 and NCLIP(v3,v1,v2)
 *        > 0, as triangle (v0,v1,v2) when the first is negative and the second
 *        not positive, as triangle (v3,v1,v2) when the first is >= 0 and the
 *        second positive, and not at all otherwise; the winding flag negates a
 *        positive first and a negative second NCLIP. No back faces.
 *  DUAL_FLAG_WHOLE  DUAL, except that a quad with the winding flag draws whole
 *        with no NCLIP test. DUAL's negation makes such a quad whole too unless
 *        an NCLIP is zero, where DUAL keys a triangle instead.
 *  WHOLE the face draws whole: a triangle on NCLIP(v0,v1,v2) > 0, a quad on
 *        that or NCLIP(v3,v1,v2) < 0 (a back face); the test is skipped while
 *        TRZ < near_limit, and the face is a back face when NCLIP(v0,v1,v2) < 0.
 *  FLAT  a triangle draws on NCLIP(v0,v1,v2) != 0, a back face when negative;
 *        a quad takes DUAL's front prims, and when NCLIP(v0,v1,v2) >= 0 and
 *        NCLIP(v3,v1,v2) < 0 draws whole as a back face. A back face draws
 *        only while TRZ < near_limit. */
typedef enum {
    OTK_CHOOSE_DUAL, OTK_CHOOSE_DUAL_FLAG_WHOLE, OTK_CHOOSE_WHOLE, OTK_CHOOSE_FLAT,
    OTK_CHOOSE_MODES
} otk_choose_t;

typedef struct {
    uint8_t tri_w[3], quad_w[4];     /* vertex weights, halves */
    uint8_t base_form;               /* otk_base_form_t */
    uint8_t base_by_a, shift_by_a;   /* c and the shift scale by the header byte */
    uint8_t drop;                    /* otk_drop_t */
    uint8_t bias_tri, bias_quad;     /* otk_bias_t per prim kind */
    uint8_t choose;                  /* otk_choose_t */
    int8_t  bias_mul;
    int8_t  shift_add;
    int8_t  base_min;
    int8_t  back_post;
    int16_t back_pre;
    int16_t base_c;
} otk_spec_t;

static const otk_spec_t k_otk_rows[MOBY_OTK_ROWS] = {
    /* func_8001F798 builds a local bucket array per record (D_8006FCF4) and
     * splices it into consecutive world buckets highest first (r_moby.s
     * 1528-1602): quad k = SZa+SZb+SZc+SZd - s6, k += (bias*2) << s7,
     * bucket k >> s7 (r_moby.s 1127-1152, 1173-1189); triangle
     * k = SZa + SZa/2 + SZb + SZb/2 + SZc - s6, bucket (k >> s7) + bias*2
     * (r_moby.s 1310-1330, 1355-1374); s7 = a1 + 4, s6 = 4*(TRZ - (0x200 << a1)),
     * s6 <= 0 replaced by 1 (r_moby.s 506-517, 986-1011). The winding flag
     * (face word 0 bit 0) negates the NCLIPs (r_moby.s 1068-1110). */
    [MOBY_OTK_S1_REGULAR] = {
        .tri_w = { 3, 3, 2 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X4, .base_c = 0x200, .base_by_a = 1, .base_min = 1,
        .shift_by_a = 1, .shift_add = 4, .drop = OTK_DROP_NEG,
        .bias_tri = OTK_BIAS_POST, .bias_quad = OTK_BIAS_PRE, .bias_mul = 2,
        .choose = OTK_CHOOSE_DUAL },
    /* func_80020F34 keys as func_8001F798: s6/s7 from the same record fields
     * (r_moby.s 2111-2155, 2630-2654), quad k = SZa+SZb+SZc+SZd - s6 plus
     * (bias*2) << s7 (r_moby.s 2759-2801, 2824-2838), triangle
     * SZa + SZa/2 + SZb + SZb/2 + SZc - s6, then >> s7, + bias*2 (r_moby.s
     * 2965-2990, 3012-3026, 3348-3368), from the SZ3 halfwords its vertex
     * loops store (r_moby.s 2316-2335, 2388-2407). A fresh face with word 0
     * bit 0 set skips the second NCLIP and draws the quad whole (r_moby.s
     * 2716-2718, 2749-2759); a strip face with it draws on NCLIP != 0 (r_moby.s
     * 2926-2941). The record branch on the top byte of record +0x30 (r_moby.s
     * 2640-2642) loads the env-map light matrix and texture offset (r_moby.s
     * 3723-3834) and returns before the face loop with the key registers
     * untouched. */
    [MOBY_OTK_S1_SPECULAR] = {
        .tri_w = { 3, 3, 2 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X4, .base_c = 0x200, .base_by_a = 1, .base_min = 1,
        .shift_by_a = 1, .shift_add = 4, .drop = OTK_DROP_NEG,
        .bias_tri = OTK_BIAS_POST, .bias_quad = OTK_BIAS_PRE, .bias_mul = 2,
        .choose = OTK_CHOOSE_DUAL_FLAG_WHOLE },
    /* func_80022A2C keys every face of one moby into D_8006FCF4 at
     * (sum of four SZ3 slots - t8) >> 5, a triangle's third vertex filling
     * the fourth slot twice, dropped when <= 0 [r_moby.s 4605-4620,
     * 4662-4680, 4753-4763, 4876-4882, 4950-4972]; t8 = max(0, TRZ - 0x100)
     * << 2 (r_moby.s 4189-4197). Gouraud faces add the low two bits of word
     * 1 after the shift (r_moby.s 4619-4620, 4679-4680); a metal back face
     * adds 0x200 before it (r_moby.s 4894-4899). The array links highest
     * bucket first into one world bucket (r_moby.s 5006-5025). The facing
     * test: r_moby.s 4560-4590, 4705-4721, metal 4841-4847. */
    [MOBY_OTK_S1_SHADED] = {
        .tri_w = { 2, 2, 4 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X4, .base_c = 0x100, .base_min = 0,
        .shift_add = 5, .drop = OTK_DROP_NONPOS, .back_pre = 0x200,
        .bias_tri = OTK_BIAS_POST, .bias_quad = OTK_BIAS_POST, .bias_mul = 1,
        .choose = OTK_CHOOSE_WHOLE },
    /* S3 func_80031124 (mobydraw.s; S2's func_80044504 is the same
     * instruction sequence): quad K = SZ0+SZ1+SZ2+SZ3 - s6,
     * triangle K = SZ0 + 2*SZ1 + SZ2 - s6 (mobydraw.s 1592-1650, 1753-1774;
     * the special-material modes key the same, 2341-2356), dropped when
     * K < 0; bucket = (K - ((bias*4) << s7)) >> s7, dropped when negative;
     * s7 = A + 4 (1465-1471); s6 = 8*int16((TRZ - (0x200 << A)) >> 1), <= 0
     * replaced by 1 (557-571, 909-910, 1472-1487); splice highest bucket
     * first (1886-1956). */
    [MOBY_OTK_S23_REGULAR] = {
        .tri_w = { 2, 4, 2 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X8_I16HALF, .base_c = 0x200, .base_by_a = 1, .base_min = 1,
        .shift_by_a = 1, .shift_add = 4, .drop = OTK_DROP_NEG,
        .bias_tri = OTK_BIAS_PRE, .bias_quad = OTK_BIAS_PRE, .bias_mul = -4,
        .choose = OTK_CHOOSE_DUAL },
    /* S3 func_80033C5C (mobydraw.s; S2's func_80046FD8 is the same code 19
     * lines earlier): per vertex RTPS, SZ3 kept at scratch +0x1E0 (4236-4251);
     * s6 = 4*(TRZ - 0x400), <= 0 replaced by 1 (4201, 4368-4375); quad
     * k = SZ0+SZ1+SZ2+SZ3 - s6 (4476-4479), triangle k = SZ0 + 2*SZ1 + SZ2 - s6
     * (4622-4626), dropped when k < 0; the signed top nibble of face word 0
     * adds that many buckets before the shift by 5, dropped when negative
     * (4436, 4493-4495, 4584, 4636-4638). The facing test: 4420-4470,
     * 4584-4605; the array links highest bucket first into the record's world
     * bucket (4956-4974). */
    [MOBY_OTK_S23_SIMPLE] = {
        .tri_w = { 2, 4, 2 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X4, .base_c = 0x400, .base_min = 1,
        .shift_add = 5, .drop = OTK_DROP_NEG,
        .bias_tri = OTK_BIAS_PRE, .bias_quad = OTK_BIAS_PRE, .bias_mul = 1,
        .choose = OTK_CHOOSE_DUAL },
    /* The same walk's flat-lit faces (face word 1 bit 1): four SZ3 slots, a
     * triangle's third vertex twice, minus s6, dropped when < 0, >> 5, plus 16
     * buckets on a back face (mobydraw.s 4720-4801); a back face draws only
     * while TRZ, less 0x800 when word 1 bit 0 is set, is negative
     * (4748-4760). */
    [MOBY_OTK_S23_SIMPLE_FLAT] = {
        .tri_w = { 2, 2, 4 }, .quad_w = { 2, 2, 2, 2 },
        .base_form = OTK_BASE_X4, .base_c = 0x400, .base_min = 1,
        .shift_add = 5, .drop = OTK_DROP_NEG, .back_post = 16,
        .bias_tri = OTK_BIAS_NONE, .bias_quad = OTK_BIAS_NONE,
        .choose = OTK_CHOOSE_FLAT },
};

void moby_otk_record(moby_otk_row_t row, int a, int trz, moby_otk_rec_t* out)
{
    out->row = (int8_t)row;
    out->trz = trz;
    out->base = 0;
    out->shift = 0;
    if (row <= MOBY_OTK_NONE || row >= MOBY_OTK_ROWS) { out->row = MOBY_OTK_NONE; return; }
    const otk_spec_t* s = &k_otk_rows[row];
    int sa = s->base_by_a ? (a & 31) : 0;
    int64_t d = (int64_t)trz - ((int64_t)s->base_c << sa);
    int64_t b = s->base_form == OTK_BASE_X4 ? 4 * d
                                            : 8 * (int64_t)(int16_t)(int32_t)(d >> 1);
    if (b <= 0) b = s->base_min;
    out->base = b > INT32_MAX ? INT32_MAX : (int32_t)b;
    out->shift = (int8_t)(((s->shift_by_a ? a : 0) + s->shift_add) & 31);
}

/* ---- integer RTPS / NCLIP mirror -------------------------------------------- */

/* The vertex packs into VXY0 as VX + (VY << 16), so a negative VX borrows one
 * from VY, and VZ0 takes the low half; the GTE step itself is ngte_rtps_int. */
void moby_otk_rtps(const int tr[3], const int M[3][3], double vx, double vy, double vz,
                   double ofx, double ofy, int H, moby_otk_vert_t* out)
{
    int32_t vxi = (int32_t)floor(vx), vyi = (int32_t)floor(vy), vzi = (int32_t)floor(vz);
    uint32_t packed = ((uint32_t)vyi << 16) + (uint32_t)vxi;
    ngte_rtps_out o;
    ngte_rtps_int(M, tr, (int16_t)(packed & 0xFFFFu), (int16_t)(packed >> 16), (int16_t)vzi,
                  ofx, ofy, H, &o);
    out->sz = o.sz3;
    out->sx = o.sx;
    out->sy = o.sy;
}

long moby_otk_nclip(const moby_otk_vert_t* a, const moby_otk_vert_t* b, const moby_otk_vert_t* c)
{
    return (long)a->sx * (b->sy - c->sy) + (long)b->sx * (c->sy - a->sy)
         + (long)c->sx * (a->sy - b->sy);
}

/* ---- prim choice and key ------------------------------------------------------ */

typedef struct {
    const moby_otk_vert_t* v[4];
    int quad;
    int v3;
    int back;
} otk_prim_t;

/** @brief Fills a prim descriptor from a face.
 *
 * `v3` selects the triangle (v3,v1,v2), whose first vertex is the face's
 * fourth; otherwise the prim starts at v0. A quad prim carries all four
 * vertices.
 * @param p Receives the prim.
 * @param f The face.
 * @param quad 1 for the whole quad, 0 for a triangle.
 * @param v3 1 for the triangle (v3,v1,v2).
 * @param back 1 for a back face. */
static void otk_prim(otk_prim_t* p, const moby_otk_face_t* f, int quad, int v3, int back)
{
    p->v3 = v3;
    p->v[0] = v3 ? f->v[3] : f->v[0];
    p->v[1] = f->v[1];
    p->v[2] = f->v[2];
    p->v[3] = quad ? f->v[3] : NULL;
    p->quad = quad;
    p->back = back;
}

/** @brief The front prim of a quad under the dual NCLIP test.
 *
 * A negative first NCLIP keeps (v0,v1,v2), widened to the whole quad when the
 * second NCLIP is positive; otherwise a positive second NCLIP keeps
 * (v3,v1,v2).
 * @param f The face.
 * @param n1 The NCLIP of (v0,v1,v2).
 * @param n2 The NCLIP of (v3,v1,v2).
 * @param p Receives the prim, as a front face.
 * @return 1 when a prim was chosen, 0 when neither test keeps one. */
static int otk_dual_front(const moby_otk_face_t* f, long n1, long n2, otk_prim_t* p)
{
    if (n1 < 0) { otk_prim(p, f, n2 > 0, 0, 0); return 1; }
    if (n2 > 0) { otk_prim(p, f, 0, 1, 0); return 1; }
    return 0;
}

/** @brief The DUAL rule: the prim a face draws as, never a back face.
 *
 * A triangle draws on n1 > 0, or n1 != 0 under the winding flag. A quad takes
 * NCLIP(v3,v1,v2) as the second value; under the winding flag a positive n1
 * and a negative second value are negated. otk_dual_front then picks the prim.
 * @param r Unused.
 * @param f The face.
 * @param n1 The NCLIP of (v0,v1,v2).
 * @param p Receives the prim.
 * @return 1 when a prim is chosen, 0 when the draw drops the face. */
static int otk_choose_dual(const moby_otk_rec_t* r, const moby_otk_face_t* f, long n1,
                           otk_prim_t* p)
{
    (void)r;
    if (!f->v[3]) {
        if (f->wbit ? n1 == 0 : n1 <= 0) return 0;
        otk_prim(p, f, 0, 0, 0);
        return 1;
    }
    long n2 = moby_otk_nclip(f->v[3], f->v[1], f->v[2]);
    if (f->wbit) {
        if (n1 > 0) n1 = -n1;
        if (n2 < 0) n2 = -n2;
    }
    return otk_dual_front(f, n1, n2, p);
}

/** @brief The DUAL_FLAG_WHOLE rule: DUAL, except a quad with the winding flag draws whole.
 *
 * The whole quad is chosen with no NCLIP test.
 * @param r The record, passed to otk_choose_dual.
 * @param f The face.
 * @param n1 The NCLIP of (v0,v1,v2).
 * @param p Receives the prim.
 * @return 1 when a prim is chosen, 0 when the draw drops the face. */
static int otk_choose_dual_flag_whole(const moby_otk_rec_t* r, const moby_otk_face_t* f,
                                      long n1, otk_prim_t* p)
{
    if (!f->wbit || !f->v[3]) return otk_choose_dual(r, f, n1, p);
    otk_prim(p, f, 1, 0, 0);
    return 1;
}

/** @brief The WHOLE rule: the face draws whole, as a back face when its first NCLIP is negative.
 *
 * While the record's TRZ is below the face's near limit the face always draws.
 * Past it a triangle draws on n1 > 0, and a quad on n1 > 0 or, as a back face,
 * on a negative NCLIP(v3,v1,v2).
 * @param r The record; supplies TRZ.
 * @param f The face; supplies the near limit.
 * @param n1 The NCLIP of (v0,v1,v2).
 * @param p Receives the prim.
 * @return 1 when a prim is chosen, 0 when the draw drops the face. */
static int otk_choose_whole(const moby_otk_rec_t* r, const moby_otk_face_t* f, long n1,
                            otk_prim_t* p)
{
    int back = n1 < 0;
    if (!(r->trz < f->near_limit) && n1 <= 0) {
        if (!f->v[3] || moby_otk_nclip(f->v[3], f->v[1], f->v[2]) >= 0) return 0;
        back = 1;
    }
    otk_prim(p, f, f->v[3] != NULL, 0, back);
    return 1;
}

/** @brief The FLAT rule: two-sided faces whose back side draws only near the camera.
 *
 * A triangle draws on n1 != 0, a back face when n1 is negative. A quad takes
 * DUAL's front prims and otherwise, on a negative second NCLIP, draws whole as
 * a back face. A back face is kept only while the record's TRZ is below the
 * face's near limit.
 * @param r The record; supplies TRZ.
 * @param f The face; supplies the near limit.
 * @param n1 The NCLIP of (v0,v1,v2).
 * @param p Receives the prim.
 * @return 1 when a prim is chosen, 0 when the draw drops the face. */
static int otk_choose_flat(const moby_otk_rec_t* r, const moby_otk_face_t* f, long n1,
                           otk_prim_t* p)
{
    if (!f->v[3]) {
        if (n1 == 0) return 0;
        otk_prim(p, f, 0, 0, n1 < 0);
    } else {
        long n2 = moby_otk_nclip(f->v[3], f->v[1], f->v[2]);
        if (!otk_dual_front(f, n1, n2, p)) {
            if (n2 == 0) return 0;
            otk_prim(p, f, 1, 0, 1);
        }
    }
    return !p->back || r->trz < f->near_limit;
}

/** @brief The bucket word of a chosen prim under a row's key arithmetic.
 *
 * k is the sum over the prim's vertices of (weight * SZ) >> 1, less the
 * record's base, plus the row's back_pre on a back face the face marks with
 * back_adds; the prim is dropped when k < 0 (k <= 0 under OTK_DROP_NONPOS).
 * The face bias times bias_mul enters before the shift (OTK_BIAS_PRE, dropped
 * when k goes negative) or after it (OTK_BIAS_POST); the bucket is k shifted
 * right by the record's shift, dropped when negative, plus back_post on a back
 * face. MOBY_OTK_QUAD and MOBY_OTK_V3 are set from the prim.
 * @param s The row.
 * @param r The record's base and shift.
 * @param f The face; supplies the bias and back_adds.
 * @param p The prim chosen for the face.
 * @return The bucket word, or -1 when the draw drops the prim. */
static int otk_key(const otk_spec_t* s, const moby_otk_rec_t* r, const moby_otk_face_t* f,
                   const otk_prim_t* p)
{
    const uint8_t* w = p->quad ? s->quad_w : s->tri_w;
    int n = p->quad ? 4 : 3;
    int64_t k = -(int64_t)r->base;
    for (int i = 0; i < n; i++) k += ((int64_t)w[i] * p->v[i]->sz) >> 1;
    int back = p->back && f->back_adds;
    if (back) k += s->back_pre;
    if (s->drop == OTK_DROP_NONPOS ? k <= 0 : k < 0) return -1;
    int mode = p->quad ? s->bias_quad : s->bias_tri;
    int64_t add = (int64_t)f->bias * s->bias_mul;
    if (mode == OTK_BIAS_PRE) {
        k += add * ((int64_t)1 << r->shift);
        if (k < 0) return -1;
    }
    int64_t b = k >> r->shift;
    if (mode == OTK_BIAS_POST) b += add;
    if (b < 0) return -1;
    if (back) b += s->back_post;
    return (int)b | (p->quad ? MOBY_OTK_QUAD : 0) | (p->v3 ? MOBY_OTK_V3 : 0);
}

int moby_otk_face_bucket(const moby_otk_rec_t* r, const moby_otk_face_t* f)
{
    if (r->row <= MOBY_OTK_NONE || r->row >= MOBY_OTK_ROWS) return -1;
    const otk_spec_t* s = &k_otk_rows[r->row];
    typedef int (*otk_choose_fn)(const moby_otk_rec_t*, const moby_otk_face_t*, long,
                                 otk_prim_t*);
    static const otk_choose_fn k_choose[OTK_CHOOSE_MODES] = {
        [OTK_CHOOSE_DUAL] = otk_choose_dual,
        [OTK_CHOOSE_DUAL_FLAG_WHOLE] = otk_choose_dual_flag_whole,
        [OTK_CHOOSE_WHOLE] = otk_choose_whole,
        [OTK_CHOOSE_FLAT] = otk_choose_flat,
    };
    long n1 = moby_otk_nclip(f->v[0], f->v[1], f->v[2]);
    otk_prim_t p;
    return k_choose[s->choose](r, f, n1, &p) ? otk_key(s, r, f, &p) : -1;
}
