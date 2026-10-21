#pragma once
/* areawarp.h - warp to any Spyro 3 sub-area from the dev overlay (M key):
 * the "Area warp" dropdown and its Warp button, loading the area's level
 * through the level warp when needed and entering the area through the
 * game's own EnterSubArea portal call. See areawarp.c. */

typedef struct CPUState CPUState;
struct openpete_surface_sink;

/** @brief Build the area warp's entries, reset it and add its two dev-overlay rows.
 *
 * Called once from the level warp's apply_options, so the rows follow the
 * level warp's. Also arms the scripted driver OPENPETE_AREAWARP_TEST. */
void spyro3_areawarp_register(void);

/** @brief Report the area warp's two row templates to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro3_areawarp_surface(struct openpete_surface_sink* sink);

/** @brief Drive the area warp once per tick: drain a queued request, then follow the entry to arrival.
 *
 * Runs every tick from game_on_frame_pre, after the level warp. A request
 * for a sub-area of another level first queues the level warp's
 * WarpToLevel(0, level) with the area word zeroed; a request from a sub-area
 * of the target level first queues EnterSubArea(0, the pair of that area's
 * exit portal); on arrival in the target level's area 0, or for a request
 * made there, it queues EnterSubArea(area, the pair of the area-0 portal to
 * the area). At the title screen a request arms --skip-to-area. This
 * function writes no guest word itself; the transition queue makes each
 * call at the next Draw entry.
 * @param cpu The guest CPU state. */
void spyro3_areawarp_on_tick(CPUState* cpu);
