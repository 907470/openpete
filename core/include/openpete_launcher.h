/* openpete_launcher.h - the launcher common to every game: the disc library,
 * the launcher window and its Vulkan device, and the screens drawn on it.
 *
 * Before the game runs, the launcher resolves which disc boots (an --ingest
 * copy into the library, an explicit --disc, the library's one entry of a
 * named game, the selector, or the drop screen when the library is empty),
 * shows the waits of the ingest copy, the first-boot module build
 * and the mod compile, and hands the game the window, the Vulkan device and
 * the imgui context it created (openpete_launcher_handoff). The screens draw
 * through imgui on that device (openpete_native_launcher_present); there is
 * no second renderer.
 *
 * The window and device open lazily: when a screen has to show, or at the
 * hand-off of a windowed run. A run whose identification needs no screen
 * creates them only at the hand-off, after the game is identified. Headless
 * runs open nothing.
 *
 * Library layout, wherever openpete_paths_data_dir puts writable data:
 *
 *   library/<first 16 hex digits of the disc's SHA-256>/
 *     disc.bin, disc.cue     ingested copy (cue synthesized)
 *     meta.toml              label / source / sha256 / last_played / game
 *     modules/...            the game's recompiled module, when it has one
 *
 * One library serves every game; an executable lists the entries recorded
 * for its own game and the unidentified ones.
 */
#ifndef OPENPETE_LAUNCHER_H
#define OPENPETE_LAUNCHER_H

#include <stddef.h>

#include "openpete.h" /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* A disc image the game knows by content hash, labelled in the library. */
typedef struct openpete_launcher_known_disc {
    const char* sha256;   /* 64 lowercase hex digits */
    const char* label;
} openpete_launcher_known_disc;

/* What the launcher needs from the game that calls it. */
typedef struct {
    /* Registered game id: the entries it lists. NULL before a game is
     * installed: every entry is listed, and the screens carry no game name. */
    const char* game_id;
    const char* display_name;   /* the screens' subtitle; NULL for none */
    const openpete_launcher_known_disc* known_discs;   /* NULL for none */
    int known_disc_count;
    /* The recompiled module path of a library entry, written into out;
     * returns 0, or -1 when the game loads no module from the library (a
     * static build). NULL for a game with none. */
    int (*module_path)(const char* entry_dir, char* out, size_t cap);
    /* The display name of a registered game id, prefixed to the selector's
     * label of an entry recorded for it; NULL for no prefix. */
    const char* (*game_name)(const char* game_id);
} openpete_launcher_game;

struct rhi_device;

typedef int (*openpete_launcher_task_fn)(void* userdata);

/** @brief Start the launcher for one game and set whether its screens may show.
 *
 * Call before any other launcher call. An executable linking several games
 * calls it first with a descriptor naming no game, before any game is
 * installed, to pick the disc; the installed game's entry then calls it
 * again with its own descriptor, and the window and device opened so far
 * stay. The window opens at the game window's size
 * (openpete_game_window_size), the 640x448 layout space before a game is
 * installed, from the options it is opened with: the game-neutral ones for
 * a screen before identification, the full resolution at the hand-off.
 * @param game The calling game; must outlive the launcher.
 * @param neutral The game-neutral options; must outlive the launcher.
 * @param ui Non-zero when screens may show (a windowed run); 0 for a headless
 *           run or a batch mode, which then open no window. */
void openpete_launcher_begin(const openpete_launcher_game* game,
                             const openpete_options_t* neutral, int ui);

/** @brief Resolve the disc this boot runs through the library, showing the drop screen or the version selector when one is needed.
 *
 * --ingest copies the image into the library and boots it; an explicit
 * --disc or --exe-path leaves the library untouched; otherwise the selector
 * picks among several entries, and among any number while the launcher
 * names no game (a disc dropped on it is ingested and boots instead), one
 * entry of a named game boots without it, the most recently played boots
 * without screens, and an empty library opens the drop screen. The harness
 * variable OPENPETE_LAUNCHER_DROP acts as a drop on whichever of these
 * applies, and without screens names the disc itself. A second call after an entry booted (the pick made before a game
 * was installed) boots that entry again without a scan or a screen, with
 * the module path the now-installed game names for it. When the launcher's device cannot be created the player is told
 * (openpete_launcher_vk_failure) and the selector falls back to the most
 * recent entry; the drop screen then has no fallback and the run ends.
 * @param opts The options; receives disc_path, and ingest_path from a drop.
 * @param module_path Receives the booted entry's module path, "" for none.
 * @param module_cap Its capacity.
 * @param exit_now Set to 1 when the player quit the drop screen or the
 *                 version selector.
 * @return OPENPETE_EXIT_OK; 1 after telling the player the library could
 *         not be read or the image could not be added; or
 *         OPENPETE_EXIT_SETUP_ERROR when the drop screen is needed and the
 *         device cannot be created. */
int  openpete_launcher_resolve(openpete_options_t* opts, char* module_path,
                               size_t module_cap, int* exit_now);

/** @brief The game recorded on the library entry this boot resolved.
 *
 * @return The game id, "" for an unidentified entry, or NULL when the boot
 *         named its disc itself (--disc, --exe-path) or booted no entry. */
const char* openpete_launcher_selected_game(void);

/** @brief Show a progress stage on the launcher, opening its window when a screen may show.
 *
 * Wired as the progress sink of the ingest copy, the first-boot module
 * build and the mod compile. A no-op without screens and after the
 * hand-off. Called from inside openpete_launcher_run_task's worker it only
 * stores the update, which the runner draws on the launcher's thread.
 * @param stage The stage text.
 * @param done Completed units.
 * @param total Total units, or 0 for no bar. */
void openpete_launcher_progress(const char* stage, int done, int total);

/** @brief Run a blocking stage on a worker thread while the launcher's thread keeps its window drawn and responsive.
 *
 * The stage must not run on the window's thread: a Windows title-bar drag
 * traps that thread in the OS modal move loop. Blocks until fn returns;
 * meanwhile the launcher drains events and redraws the last progress every
 * 16 ms and from an event watch on window expose and move. The window opens
 * at the first progress report. fn runs inline without screens, after the
 * hand-off, or when the worker cannot start. Not reentrant.
 * @param fn The stage.
 * @param userdata Passed to fn.
 * @return fn's return value. */
int  openpete_launcher_run_task(openpete_launcher_task_fn fn, void* userdata);

/** @brief Hand the window, the device and the imgui context to the game, creating them now when no screen did.
 *
 * Windowed runs only; a headless run hands over nothing. After the hand-off
 * no launcher screen shows. When the device cannot be created the player
 * is told and opts->native_unavailable is set: the disc is named by now, so
 * the run continues on PsyCross's GL window.
 * @param opts The options from the full resolution.
 * @return OPENPETE_EXIT_OK. */
int  openpete_launcher_handoff(openpete_options_t* opts);

/** @brief Present one frame of the launcher's progress screen on a device: the title, the game's subtitle, the stage's banner, the stage line, the bar and the footer.
 *
 * The launcher's own waits and the renderer's pipeline precompile (after
 * the hand-off, on the device it adopted) draw through this one screen. It
 * reads no launcher state besides the OPENPETE_LAUNCHER_SHOT readback
 * marks, so it draws on a start that never showed a launcher screen. The
 * stage's banner and footer follow its text ("COMPILING MODS",
 * "COMPILING SHADERS", any other stage the first-time setup); a counted
 * stage shows done/total, or done alone while total is 0. The first frame
 * drawn this way is read back as the screen "precompile" when
 * OPENPETE_LAUNCHER_SHOT is set. Display owner only.
 * @param dev The device presented on; NULL for the launcher's own.
 * @param name The game's display name for the subtitle; NULL or "" for none.
 * @param stage The stage text.
 * @param done Completed units.
 * @param total Total units, or 0 for an empty bar.
 * @return 1 when a frame was drawn, 0 when the frame begin was refused or
 *         the device has no imgui context. */
int  openpete_launcher_draw_progress(struct rhi_device* dev, const char* name,
                                     const char* stage, int done, int total);

/** @brief Destroy whatever the launcher opened, for a run that ends before the hand-off. */
void openpete_launcher_close(void);

/** @brief Tell the player the Vulkan device could not be created: a message box naming the reason, and the log.
 *
 * The reason is always logged (and so written to the log file). The box
 * shows in a windowed run, except under gamescope (GAMESCOPE_WAYLAND_DISPLAY
 * set), where SDL's zenity box is not displayed and would block until the
 * compositor exits, and except when the harness variable
 * OPENPETE_MSGBOX_SKIP is set, so an unattended run does not block. The run
 * then takes the same branch as after a dismissed box.
 * @param why The reason, a sentence.
 * @param continues Non-zero when the run continues on PsyCross's GL window,
 *                  0 when it ends. */
void openpete_launcher_vk_failure(const char* why, int continues);

/** @brief Resolve the serial and library hash of a disc, the keys of openpete.toml's [disc.*] scopes.
 *
 * The serial comes from the image's SYSTEM.CNF (a few sectors). The hash is
 * read back from the library entry whose disc.cue or disc.bin is the same
 * file, recorded at ingest; a disc outside the library resolves a serial and
 * no hash, so a [disc.SHA] scope does not apply to it. With no disc nothing
 * is resolved.
 * @param disc_path The identified disc, or NULL or empty for none.
 * @param serial Receives the serial, or NULL.
 * @param serial_cap Its capacity.
 * @param sha Receives the library entry's hash, or NULL.
 * @param sha_cap Its capacity.
 * @return 0 when a serial or a hash was resolved, else -1. */
int  openpete_library_disc_identity(const char* disc_path, char* serial, size_t serial_cap,
                                    char* sha, size_t sha_cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_LAUNCHER_H */
