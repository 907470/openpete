/* Paint-rank calibration instrument: compares the native same-moby paint rank
 * (moby_rank.h) against the game's own ordering table for the same pose.
 * Reads guest OT output, so it runs only under OPENPETE_MOBY_RANK_CAL and on
 * no present path's output: it prints and changes nothing. */
#ifndef OPENPETE_MOBY_RANK_CAL_H
#define OPENPETE_MOBY_RANK_CAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The calibration instrument's level, from OPENPETE_MOBY_RANK_CAL (boot-latched).
 *
 * @return 0 off; 1 prints one summary line per feed pass; 2 also prints one
 *         line per disagreeing pair. */
int moby_rank_cal_on(void);

/* One feed pass's walk triangles, in walk order (a moby's triangles are
 * contiguous): owning moby, the draw's integer SXY of the three vertices,
 * native rank (0 = none), contest flag, rank eligibility, bucket word,
 * face-topology key, and the intersecting pairs (walk indices). `pass` names
 * the feed pass. */
typedef struct {
    int n;
    const uint32_t* moby;
    const int16_t (*isxy)[6];
    const int* rank;
    const uint8_t* contest;
    const uint8_t* eligible;
    const int32_t* otk;      /* the triangle's bucket word (moby_rank.h) */
    const uint32_t* fkey;    /* the triangle's face-topology key */
    const int* ip_a;
    const int* ip_b;
    int ip_n;
    const char* pass;
    long tie_reorder;        /* the pass's full-key order counts (moby_otk_tie_count_t) */
    long decal_kept;
} moby_rank_cal_in_t;

/** @brief Compares one feed pass's native same-moby paint rank against the game's ordering table for the same pose.
 *
 * Walks the world ordering table's bucket array as the game's linker will
 * (game_layout.moby.cal_world_ot), gives every ranked native prim whose
 * integer screen points are exactly the vertices of one game prim that prim's
 * paint position, and compares the native order with it over same-moby pairs
 * of such triangles: the intersecting pairs, the pairs of two contested
 * triangles, and all pairs. Prints one `[rank-cal]` line to stderr, and at
 * level 2 a `[rank-cal-dis]` line per disagreeing intersecting or contested
 * pair (a disagreeing pair outside those two sets is only counted).
 * @param in The feed pass's walk triangles. */
void moby_rank_cal_run(const moby_rank_cal_in_t* in);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOBY_RANK_CAL_H */
