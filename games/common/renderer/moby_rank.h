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

/** @brief Whether the same-moby paint rank is on (OPENPETE_MOBY_RANK, default on; boot-latched).
 *
 * @return 0 when OPENPETE_MOBY_RANK is set to 0, so the faces of one moby
 *         order by true depth alone; 1 when it is unset, empty or any other value. */
int  openpete_mobyrank_on(void);

/* The parts of the full-key paint order OPENPETE_RANK_TIE selects. ORDER: a
 * moby's prims order by their full key, the bits the bucket shift drops
 * included, and a decal (a face stacked or coplanar on an earlier face of its
 * moby) in its base's bucket paints right after the base. CONTEST: a face
 * contests while its intersection-space box overlaps another face's that
 * shares no edge with it, and every triangle of a prim contests when one
 * does. */
#define MOBY_RANK_TIE_ORDER   1
#define MOBY_RANK_TIE_CONTEST 2

/** @brief The full-key paint order parts OPENPETE_RANK_TIE selects, latched on first use.
 *
 * Unset, empty or 0: none (the bucket order, ties in insertion order, and the
 * crossing contest). 1: both parts; "order" or "contest": that part alone.
 * @return A mask of MOBY_RANK_TIE_ORDER and MOBY_RANK_TIE_CONTEST. */
int  moby_rank_tie_mode(void);

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

/** @brief Whether a row's draw links every prim of a record into one world ordering-table bucket.
 *
 * Such a record paints whole against the terrain, its faces in the order of
 * its local bucket array (S1 func_80022A2C, S2 func_80046FD8 and S3
 * func_80033C5C, both face families); the other rows splice the array into
 * consecutive world buckets.
 * @param row The row.
 * @return 1 for MOBY_OTK_S1_SHADED, MOBY_OTK_S23_SIMPLE and
 *         MOBY_OTK_S23_SIMPLE_FLAT, else 0. */
int moby_otk_row_single_bucket(moby_otk_row_t row);

/* One record's key parameters: `base` the depth base the draw subtracts,
 * `shift` the bucket shift, `trz` the GTE TRZ it loaded, `aoff` the local
 * buckets the draw moves its bucket array base up by when the depth base came
 * out <= 0 (the base is then replaced by base_min, and a face's array entry is
 * its bucket plus `aoff`). */
typedef struct moby_otk_rec {
    int8_t  row;
    int8_t  shift;
    int32_t base;
    int32_t trz;
    int32_t aoff;
} moby_otk_rec_t;

/** @brief Fills one record's key parameters for a row of the paint-key table.
 *
 * An out-of-range row and MOBY_OTK_NONE yield a MOBY_OTK_NONE record, whose
 * faces all return -1 from moby_otk_face_bucket.
 * @param row The record's row.
 * @param a The record's animation-header byte the row scales its base and shift by (0 on a row that reads none).
 * @param trz The GTE TRZ the draw loaded.
 * @param out Receives the row, the depth base, the bucket shift and `trz`. */
void moby_otk_record(moby_otk_row_t row, int a, int trz, moby_otk_rec_t* out);

/* One vertex as the draw's RTPS leaves it: SZ3 and SXY2. */
typedef struct moby_otk_vert {
    int32_t sz;
    int16_t sx, sy;
} moby_otk_vert_t;

/** @brief RTPS (sf=1, lm=0) of the model-space vertex (vx, vy, vz) under rotation M and translation tr.
 *
 * The draw's screen offset is in PSX pixels and H is its projection distance. A
 * pose between two keyframes carries the INTPL result, whose sf=1 shift is the
 * floor of the float lerp.
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

/** @brief The GTE NCLIP of three RTPS outputs: twice the signed screen area of (a, b, c).
 *
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @return The cross product of the screen edges, positive for one winding and negative for the other. */
long moby_otk_nclip(const moby_otk_vert_t* a, const moby_otk_vert_t* b,
                    const moby_otk_vert_t* c);

/* One face as the draw's NCLIP and key read it: its vertices in face order
 * (v[3] NULL on a triangle), `wbit` the winding flag that lets a face pass
 * the facing test either way round, `bias` the decoded ordering-bias field,
 * `back_adds` 1 when the row's back-face offset applies to this face,
 * `near_limit` the TRZ below which the draw keeps a back face (INT32_MIN:
 * never), and `mirror` 1 when the face's record draws mirrored, which
 * negates both NCLIPs before any test (moby_game_t.record_face_sign). */
typedef struct moby_otk_face {
    const moby_otk_vert_t* v[4];
    int32_t bias;
    int32_t near_limit;
    int8_t  wbit;
    int8_t  back_adds;
    int8_t  mirror;
} moby_otk_face_t;

/** @brief The bucket word of the prim the draw makes of one face of a record.
 *
 * The row's NCLIP rule picks the prim (the triangle (v0,v1,v2), the triangle
 * (v3,v1,v2) or the whole quad) and whether it is a back face; the row's key
 * arithmetic turns its vertices' SZ3 into a local bucket.
 * @param r The record's key parameters.
 * @param f The face.
 * @return The bucket word: the local bucket and the key's position inside it
 *         (MOBY_OTK_BUCKET, MOBY_OTK_SUB), with MOBY_OTK_QUAD set when the prim
 *         is the whole quad and MOBY_OTK_V3 when it is the triangle
 *         (v3,v1,v2); -1 when the
 *         draw culls the face or drops the prim from its table, or the record's
 *         row is MOBY_OTK_NONE. */
int moby_otk_face_bucket(const moby_otk_rec_t* r, const moby_otk_face_t* f);

/** @brief The world ordering-table bucket the draw splices one local bucket array entry into.
 *
 * The record's local bucket array holds 0x80 eight-byte entries from the
 * array base; `ltop` is the highest entry any face of the record filled. The
 * splice (S2 mobydraw.s 0x8004539C-0x80045488, S3 func_80031124's copy of
 * it) starts at world bucket wbase - ((8 * (0x80 - ltop)) << n >> 8) - 1,
 * floored at 0, and walks the array down from `ltop` in runs of 0x100 >> n
 * bytes, one world bucket nearer per run, staying at bucket 0 once it
 * reaches it.
 * @param wbase The record's world bucket base (cull record +0x18).
 * @param n The record's splice shift (cull record +0x1B), 0..5.
 * @param ltop The highest filled array entry.
 * @param l The entry, at most `ltop`.
 * @return The world bucket, >= 0. */
int moby_otk_splice_bucket(int wbase, int n, int ltop, int l);

/* The regular draw keeps its local bucket array 0x2000 bytes below its record
 * list (S2 mobydraw.s 0x8004453C-0x80044548: the list at GTE data 2 + 0x2000,
 * the array at GTE data 2), so an entry at 0x400 or beyond lands on the list:
 * no record the draw keeps fills one, and a record that would (one revealed
 * past the cull's range, whose depth base wraps) has no splice to mirror. */
#define MOBY_OTK_ARRAY_LIMIT 0x400

/** @brief The local bucket array entry a face's prim fills: its bucket plus the record's array offset.
 *
 * @param r The record's key parameters.
 * @param otk The face's bucket word.
 * @return The entry, or -1 when the word is negative (the draw keeps no prim of the face). */
int moby_otk_array_entry(const moby_otk_rec_t* r, int32_t otk);

/** @brief The record's highest filled local bucket array entry once one more face is keyed.
 *
 * The splice starts from that entry (moby_otk_splice_bucket).
 * @param r The record's key parameters.
 * @param otk The face's bucket word; a negative word fills nothing.
 * @param top The highest entry before this face, -1 for none.
 * @return The larger of `top` and the face's entry. */
int moby_otk_top_entry(const moby_otk_rec_t* r, int32_t otk, int top);

/** @brief The world ordering-table bucket of a record's depth key: the key, moved 0x20 buckets on at 0x110 and beyond.
 *
 * Both S2/S3 moby draws leave world buckets 0x110-0x12F unused that way: the
 * animated cull (mobydraw.s 0x80043FC4-0x80043FD0) and the simple models'
 * draw (0x800474FC-0x80047508).
 * @param key The record's depth key, >= 0.
 * @return The world bucket. */
int moby_otk_world_key(int key);

/** @brief A world bucket in the depth scale of the terrain's contest keys: moby_otk_world_key undone.
 *
 * The terrain's keys carry no such move, so a bucket at 0x130 or beyond is
 * taken 0x20 buckets back before it contests them.
 * @param w The world bucket.
 * @return The bucket on the depth scale. */
int moby_otk_depth_bucket(int w);

/* A walk triangle's bucket word: its face's bucket word, with MOBY_OTK_Q2 set
 * on the second triangle of a prim whose first triangle carries the same
 * word, and MOBY_OTK_FOREIGN set on a triangle that is not part of the prim
 * the key chose (the walk's float facing test split the face differently from
 * the draw's integer NCLIP). MOBY_OTK_BUCKET extracts the bucket (bits
 * 12-26, saturated at MOBY_OTK_BUCKET_MAX) and MOBY_OTK_SUB the key's
 * position inside it: the low bits the shift drops, in key units, the top
 * MOBY_OTK_SUB_BITS of them when the shift is wider. */
#define MOBY_OTK_Q2      0x40000000
#define MOBY_OTK_QUAD    0x20000000
#define MOBY_OTK_V3      0x10000000
#define MOBY_OTK_FOREIGN 0x08000000
#define MOBY_OTK_SUB_BITS  12
#define MOBY_OTK_BUCKET_MAX 0x7FFF
#define MOBY_OTK_BUCKET(w) (((w) & 0x07FFFFFF) >> MOBY_OTK_SUB_BITS)
#define MOBY_OTK_SUB(w)    ((w) & ((1 << MOBY_OTK_SUB_BITS) - 1))

/* The triangles the walk emits of a face: (v0,v1,v2), (v3,v1,v2), and the
 * quad's second half (v1,v3,v2). */
typedef enum { MOBY_OTK_TRI_A, MOBY_OTK_TRI_V3, MOBY_OTK_TRI_QUAD_B } moby_otk_tri_t;

/** @brief The bucket word of one emitted triangle of a face, marked foreign when the key's prim does not contain it.
 *
 * A negative word and a whole-quad word pass unchanged.
 * @param otk The face's bucket word.
 * @param tri Which of the face's triangles.
 * @return `otk`, with MOBY_OTK_FOREIGN added when the prim the key chose does not contain `tri`. */
static inline int32_t moby_otk_tri(int32_t otk, moby_otk_tri_t tri)
{
    if (otk < 0 || (otk & MOBY_OTK_QUAD)) return otk;
    int v3 = (otk & MOBY_OTK_V3) != 0;
    int in = tri == MOBY_OTK_TRI_A ? !v3 : tri == MOBY_OTK_TRI_V3 && v3;
    return in ? otk : (otk | MOBY_OTK_FOREIGN);
}

/** @brief The bucket word of a prim's second emitted triangle, given whether its first triangle was emitted.
 *
 * @param otk The prim's bucket word.
 * @param first_emitted 1 when the prim's first triangle was emitted.
 * @return `otk` with MOBY_OTK_Q2 set when it is non-negative and the first triangle was emitted, else `otk`. */
static inline int32_t moby_otk_second(int32_t otk, int first_emitted)
{
    return (otk >= 0 && first_emitted) ? (otk | MOBY_OTK_Q2) : otk;
}

/* One prim of a moby in the paint-order sort: its sort key and its index. */
typedef struct moby_otk_ord {
    int32_t k;
    int     i;
} moby_otk_ord_t;

/* Engagement counts of the full-key order, over contested prims: `tie_reorder`
 * same-bucket pairs the order paints against their insertion order,
 * `decal_kept` decals in their base's bucket whose own full key lies deeper
 * than the base's, which the order still paints right after the base. */
typedef struct moby_otk_tie_count {
    long tie_reorder;
    long decal_kept;
} moby_otk_tie_count_t;

/** @brief The paint order of one moby's prims, farthest first.
 *
 * A triangle with a negative bucket word or MOBY_OTK_Q2 (a prim's second
 * triangle) is left out. With `full` 0 the prims order by bucket, a tie in
 * insertion order: the game's own ordering table. With `full` 1 they order by
 * the full key (MOBY_OTK_BUCKET, then MOBY_OTK_SUB), a tie in insertion
 * order, and a prim with a base (`under`) in its own bucket takes the nearer
 * of its own key and its base's, so it paints after the base as the game's
 * insertion order does; a decal in another bucket keeps its key, so every
 * decal pair paints in the game's order. Bases resolve in walk order, a
 * decal's base before the decal.
 * @param otk The triangles' bucket words.
 * @param under Per triangle, the index of the earlier prim it lies on as a
 *        decal, or -1; read only with `full` set.
 * @param cont Per triangle, its contest flag; read only for the counts.
 * @param n The triangle count.
 * @param full 1 for the full-key order, 0 for the bucket order.
 * @param scratch n entries of sort space.
 * @param order Receives the ranked triangles' indices in paint order.
 * @param c Accumulates the engagement counts when not NULL.
 * @return The number of ranked triangles written to `order`. */
int moby_otk_paint_order(const int32_t* otk, const int* under, const uint8_t* cont, int n,
                         int full, moby_otk_ord_t* scratch, int* order,
                         moby_otk_tie_count_t* c);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOBY_RANK_H */
