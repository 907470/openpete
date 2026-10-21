#pragma once
/* areawarp.h - move between a Spyro 3 level's areas from the dev overlay (M
 * key): the "Area warp" dropdown and its Warp button, entering the chosen
 * area through the game's own EnterSubArea portal call. See areawarp.c. */

typedef struct CPUState CPUState;
struct openpete_surface_sink;

/** @brief Reset the area warp and add its two dev-overlay rows.
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
 * from area 0 queues EnterSubArea(target, the pair of the area-0 portal to
 * the target); from a sub-area it first queues EnterSubArea(0, the pair of
 * that area's exit portal), and on arrival in area 0 the leg to the target.
 * This function writes no guest word itself; the transition queue makes the
 * call at the next Draw entry.
 * @param cpu The guest CPU state. */
void spyro3_areawarp_on_tick(CPUState* cpu);
