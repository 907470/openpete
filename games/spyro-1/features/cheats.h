#pragma once
/* cheats.h - Spyro-1 half of --cheats (auto-applied built-in cheats).
 *
 * The CLI/registry half lives in core (options.c parses --cheats and hands
 * the spec to the feature module's apply_options); this half owns the cheat
 * name table, the gameplay globals each cheat writes, and the "apply once a
 * level is live" timing. Called from game_extras_impl.c (game_on_frame_pre).
 * See cheats.c for the mechanism. */

#include <stdint.h>

typedef struct CPUState CPUState;

/** @brief Applies the --cheats selection and the dev overlay's queued cheats once gameplay is live.
 *
 * The --cheats selection is applied on the first live tick of each session
 * and re-armed whenever the title screen shows, so New Game after a quit
 * gets it again. Live means game state play, pause or inventory, a level
 * loaded, and no attract demo. Also refreshes the live state the overlay's
 * buttons gate on, so it runs every tick even without --cheats; called once
 * per tick from game_on_frame_pre. Does nothing on a runahead speculative
 * tick.
 * @param cpu The guest CPU state. */
void spyro1_cheats_on_tick(CPUState* cpu);

struct openpete_surface_sink;
/** @brief Report each cheat's dev-overlay row and its --cheats name to the surface dump.
 *
 * @param sink The dump's collector. */
void spyro1_cheats_surface(struct openpete_surface_sink* sink);
