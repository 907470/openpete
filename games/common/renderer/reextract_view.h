/* games/common/renderer/reextract_view.h — installs a game's sub-tick
 * re-extract body behind the sub-tick view.
 *
 * Sub-tick presents show interpolated state. Producers still publish it by
 * writing gameplay-readable memory and restoring it afterwards; the view is the
 * destination those writes are migrating to, and while it is open it serves
 * staged bytes to every read the feeders make. Opening it is behaviour-neutral
 * until a producer stages something — an empty view resolves every address to
 * real memory. */
#ifndef OPENPETE_GAME_REEXTRACT_VIEW_H
#define OPENPETE_GAME_REEXTRACT_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register `body` as the game's re-extract, wrapped in a view open/close pair.
 * `body` receives the CPUState the renderer passes, as a void*. */
void game_reextract_view_install(void (*body)(void* cpu));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_REEXTRACT_VIEW_H */
