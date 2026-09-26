/* Same-moby paint rank: the game's own paint order between the faces of one
 * moby, evaluated natively for the view being rendered.
 *
 * The PSX draws order one record's faces by an integer key built from the
 * GTE's per-vertex SZ3 and paint each face whole; the native renderer's rank
 * image and fragment select (native_render) replay that order between faces
 * of one moby where true depth would show them cutting through each other.
 * The key is computed from the native walk's own geometry through a mirror of
 * the draw's integer RTPS, NCLIP and key arithmetic, so nothing here reads
 * the game's ordering table (moby_rank_cal.h compares the two as an
 * instrument).
 *
 * Display-only: reads game state, writes nothing gameplay-readable. */
#ifndef OPENPETE_MOBY_RANK_H
#define OPENPETE_MOBY_RANK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master gate (OPENPETE_MOBY_RANK, default off; boot-latched). */
int  openpete_mobyrank_on(void);

/* ---- the game's paint key, evaluated natively ------------------------------
 * Every moby draw orders one record's faces the same way: it keys each prim
 * into a local bucket array by a weighted sum of its vertices' GTE SZ3 minus a
 * per-record depth base, shifted, biased per face, drops a prim whose key goes
 * negative, and splices the array into the world ordering table highest
 * bucket first, prims of one bucket in insertion order. The draws differ only
 * in the constants and in which prim the NCLIP test makes of a face; one row
 * of moby_rank.c's table holds them per (game, walk). Whether the splice fills
 * consecutive world buckets or one world bucket does not change the order
 * inside the record, which is all the rank reads. */
typedef enum moby_otk_row {
    MOBY_OTK_NONE = 0,         /* the walk's key is not mirrored: rank 0 */
    MOBY_OTK_S1_REGULAR,       /* S1 func_8001F798 */
    MOBY_OTK_S1_SPECULAR,      /* S1 func_80020F34 */
    MOBY_OTK_S1_SHADED,        /* S1 func_80022A2C */
    MOBY_OTK_S23_REGULAR,      /* S2 func_80044504, S3 func_80031124 */
    MOBY_OTK_S23_SIMPLE,       /* S2 func_80046FD8, S3 func_80033C5C: standard faces */
    MOBY_OTK_S23_SIMPLE_FLAT,  /* the same walks: flat-lit faces */
    MOBY_OTK_ROWS
} moby_otk_row_t;

/* One record's key parameters: `base` the depth base the draw subtracts,
 * `shift` the bucket shift, `trz` the GTE TRZ it loaded. */
typedef struct moby_otk_rec {
    int8_t  row;
    int8_t  shift;
    int32_t base;
    int32_t trz;
} moby_otk_rec_t;

/* Fills `out` for one record of `row`. `a` is the record's animation-header
 * byte the row scales its base and shift by (0 on a row that reads none);
 * `trz` the GTE TRZ the draw loaded. MOBY_OTK_NONE yields a record whose
 * faces all return -1. */
void moby_otk_record(moby_otk_row_t row, int a, int trz, moby_otk_rec_t* out);

/* One vertex as the draw's RTPS leaves it: SZ3 and SXY2. */
typedef struct moby_otk_vert {
    int32_t sz;
    int16_t sx, sy;
} moby_otk_vert_t;

/** RTPS (sf=1, lm=0) of the model-space vertex (vx, vy, vz) under rotation M
 * and translation tr, with the draw's screen offset (PSX pixels) and
 * projection distance H. A pose between two keyframes carries the INTPL
 * result, whose sf=1 shift is the floor of the float lerp.
 * @param tr  translation (TRX, TRY, TRZ)
 * @param M   rotation, Q12
 * @param vx  model-space x
 * @param vy  model-space y
 * @param vz  model-space z
 * @param ofx screen offset x, pixels
 * @param ofy screen offset y, pixels
 * @param H   projection distance
 * @param out SZ3 and SXY2 */
void moby_otk_rtps(const int tr[3], const int M[3][3], double vx, double vy, double vz,
                   double ofx, double ofy, int H, moby_otk_vert_t* out);

/* NCLIP of three RTPS outputs. */
long moby_otk_nclip(const moby_otk_vert_t* a, const moby_otk_vert_t* b,
                    const moby_otk_vert_t* c);

/* One face as the draw's NCLIP and key read it: its vertices in face order
 * (v[3] NULL on a triangle), `wbit` the winding flag that lets a face pass
 * the facing test either way round, `bias` the decoded ordering-bias field,
 * `back_adds` 1 when the row's back-face offset applies to this face, and
 * `near_limit` the TRZ below which the draw keeps a back face (INT32_MIN:
 * never). */
typedef struct moby_otk_face {
    const moby_otk_vert_t* v[4];
    int32_t bias;
    int32_t near_limit;
    int8_t  wbit;
    int8_t  back_adds;
} moby_otk_face_t;

/* The bucket word of the prim the draw makes of face `f` in record `r`: its
 * local bucket, with MOBY_OTK_QUAD set when that prim is the whole quad and
 * MOBY_OTK_V3 when it is the triangle (v3,v1,v2); -1 when the draw culls the
 * face or drops the prim from its table. */
int moby_otk_face_bucket(const moby_otk_rec_t* r, const moby_otk_face_t* f);

/* A walk triangle's bucket word: its face's bucket word, with MOBY_OTK_Q2 set
 * on the second triangle of a prim whose first triangle carries the same
 * word, and MOBY_OTK_FOREIGN set on a triangle that is not part of the prim
 * the key chose (the walk's float facing test split the face differently from
 * the draw's integer NCLIP). MOBY_OTK_BUCKET extracts the bucket. */
#define MOBY_OTK_Q2      0x40000000
#define MOBY_OTK_QUAD    0x20000000
#define MOBY_OTK_V3      0x10000000
#define MOBY_OTK_FOREIGN 0x08000000
#define MOBY_OTK_BUCKET(w) ((w) & 0x07FFFFFF)

/* The triangles the walk emits of a face: (v0,v1,v2), (v3,v1,v2), and the
 * quad's second half (v1,v3,v2). */
typedef enum { MOBY_OTK_TRI_A, MOBY_OTK_TRI_V3, MOBY_OTK_TRI_QUAD_B } moby_otk_tri_t;

/* The bucket word of the emitted triangle `tri` of a face whose bucket word is
 * `otk`: MOBY_OTK_FOREIGN added when the key's prim does not contain it. */
static inline int32_t moby_otk_tri(int32_t otk, moby_otk_tri_t tri)
{
    if (otk < 0 || (otk & MOBY_OTK_QUAD)) return otk;
    int v3 = (otk & MOBY_OTK_V3) != 0;
    int in = tri == MOBY_OTK_TRI_A ? !v3 : tri == MOBY_OTK_TRI_V3 && v3;
    return in ? otk : (otk | MOBY_OTK_FOREIGN);
}

/* The bucket word of a prim's second emitted triangle, given whether its
 * first triangle was emitted. */
static inline int32_t moby_otk_second(int32_t otk, int first_emitted)
{
    return (otk >= 0 && first_emitted) ? (otk | MOBY_OTK_Q2) : otk;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOBY_RANK_H */
