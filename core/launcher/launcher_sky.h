/* launcher_sky.h - the launcher's background over the library's level domes:
 * the flat clear colour until a library game's domes are in its launcher
 * cache, then the domes of every level of the library's games in random
 * order without repeats, 8 s holds and 2.5 s crossfades (a launch's first
 * dome fades in from the clear colour), panning 2 deg/s at 90 deg horizontal
 * FOV pitched 8 deg up. Domes that arrive while none shows
 * (the first disc) reveal the game's first level through a radial
 * noise-edged wipe from the title over 3.5 s; a game whose domes arrive while
 * one shows has its levels inserted at random positions of the current
 * no-repeat order. Motion follows the present clock the launcher passes each
 * present. Drawn by the sky pass (openpete_launcher_sky.h); every call is on
 * the launcher's thread and does nothing while screens cannot show. */
#ifndef OPENPETE_LAUNCHER_SKY_STATE_H
#define OPENPETE_LAUNCHER_SKY_STATE_H

#include "openpete_time.h"

/** @brief Load a library game's domes from its launcher cache, as the cache holds them, joining the rotation without the reveal.
 *
 * A file already loaded at the same size and modification time is not read
 * again; a game with no domes file adds nothing.
 * @param game The game id. */
void launcher_sky_seed(const char* game);

/** @brief The moment a game's launcher assets are ready: load its domes from the cache and join them to the rotation, starting the reveal when no dome shows.
 *
 * Logs one line naming the game, the domes joined and whether the reveal
 * started.
 * @param game The game id. */
void launcher_sky_assets_ready(const char* game);

/** @brief Record the title's centre in the window, the reveal's centre.
 *
 * @param u Across the window, 0..1.
 * @param v Down the window, 0..1. */
void launcher_sky_title_at(float u, float v);

/** @brief Advance the rotation to a present's instant and hand that present the sky's view (openpete_native_launcher_sky_next).
 *
 * The launcher's first drawn present gets no view: it shows the clear
 * colour, and the sky pipeline's creation and the first dome's upload fall
 * on a later present.
 * @param now The present's instant on the present clock.
 * @param after_first true once a launcher present has been drawn. */
void launcher_sky_present(op_time_t now, bool after_first);

/** @brief Report whether the view last handed to a present is one opaque dome, no crossfade or reveal running.
 *
 * @return true for one opaque dome. */
bool launcher_sky_opaque(void);

/** @brief Name the level whose dome is on screen: the incoming one past a crossfade's midpoint, else the current one.
 *
 * @param game Receives the game id; may be NULL.
 * @param level Receives the level id; may be NULL.
 * @return true when a dome shows, false for the flat background. */
bool launcher_sky_shown(const char** game, const char** level);

#endif /* OPENPETE_LAUNCHER_SKY_STATE_H */
