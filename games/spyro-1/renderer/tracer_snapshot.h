/* games/spyro-1/renderer/tracer_snapshot.h — copy of the tracer/beam sets the
 * game renderer (func_800189F0) drains each tick.
 *
 * The set count, per-set point counts and per-set point bases are guest data;
 * the copy trusts none of them beyond the game's own table size. */
#ifndef SPYRO1_RENDERER_TRACER_SNAPSHOT_H
#define SPYRO1_RENDERER_TRACER_SNAPSHOT_H

#include <stdint.h>

#include "psx_runtime.h"

#define TRACER_MAX_SETS     16
#define TRACER_MAX_PTS      72
/* g_TracerPointCount[4] and g_TracerLists[4] (include/graphics.h): the game's
 * tables hold four sets, so a set index at or past 4 reads unrelated RAM. */
#define TRACER_TABLE_SLOTS  4u

typedef struct { int x, y, z, f; } tracer_point_t;
typedef struct { int count; tracer_point_t pt[TRACER_MAX_PTS]; } tracer_set_t;

/** @brief Copies the tracer sets' points (world XYZ at +0/+4/+8, fade at +24) into out.
 *
 * Reads the set count at count_va, clamped to TRACER_TABLE_SLOTS, and per
 * set the point count at cnttab_va + s*4 and the points base at
 * basetab_va + s*4. A set with no points or a base outside KSEG0 is
 * skipped; at most max sets of TRACER_MAX_PTS 28-byte points are kept.
 * @param cpu The guest CPU state, read only.
 * @param count_va Guest address of the active set count.
 * @param cnttab_va Guest address of the per-set point-count table.
 * @param basetab_va Guest address of the per-set points-base table.
 * @param out Receives the sets.
 * @param max Capacity of out.
 * @return The number of sets written. */
int tracer_snapshot_take(CPUState* cpu, uint32_t count_va, uint32_t cnttab_va,
                         uint32_t basetab_va, tracer_set_t* out, int max);

#endif
