// games/common/recomp_attach.h - attach the game's recomp module before boot.
//
// Two implementations, selected by OPENPETE_RECOMP_LINKAGE in
// cmake/openpete_game.cmake:
//   recomp_attach_static.c - registers the descriptor linked into the exe.
//   recomp_attach_shared.c - loads the running game's <tag>-recomp.{so,dll}
//                            and binds its descriptor and the host->module
//                            forwarders (game_build.h).
#ifndef OPENPETE_GAME_RECOMP_ATTACH_H
#define OPENPETE_GAME_RECOMP_ATTACH_H

#include <stddef.h>

struct openpete_game_build;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Attaches the game's recomp module and registers it as the active module.
 *
 * Static linkage registers the descriptor linked into the exe and ignores
 * every argument. Shared linkage loads the module library and binds its
 * descriptor and forwarders. When the library is absent or unusable, a
 * build configured with OPENPETE_RECOMP_FIRST_BOOT and allowed to build
 * rebuilds it from the disc on a launcher task and binds again; any other
 * build refuses, so a run never compiles a module its build tree did not
 * produce.
 * @param module_path_override The --recomp-module path or a library
 *     entry's module, or NULL for the default (shared linkage: the
 *     platform library name next to the exe when present, else, for a
 *     game with the first-boot pipeline, the stamped module under the
 *     "modules" data directory, openpete_paths_data_dir).
 * @param disc_path The disc a missing or unusable module is rebuilt from.
 * @param allow_build Nonzero to permit that rebuild.
 * @return 0 on success, -1 when no usable module could be registered. */
int openpete_game_recomp_attach(const char* module_path_override,
                                const char* disc_path,
                                int allow_build);

/** @brief Writes a game's module path in a library entry, modules/<codegen stamp>/<tag>-recomp.{so,dll} under the entry, creating its directory.
 *
 * Forwards to the game's first-boot pipeline (game_build.h): the codegen
 * stamp hashes PSXRecomp, the game's staged manifest, the source kit, and
 * the compiler, so a module built by another version is never loaded. The
 * game is passed, never read from the installed build, so the launcher
 * names any game's module whichever game is installed. A game or build
 * without the pipeline fails, and such a build loads no module from the
 * disc library.
 * @param b The game's build record.
 * @param entry_dir The library entry's directory.
 * @param out Receives the path.
 * @param cap Capacity of out.
 * @return 0 on success, -1 when the game has no first-boot pipeline or
 *         PSXRecomp is not found. */
int openpete_game_library_module_path(const struct openpete_game_build* b, const char* entry_dir,
                                      char* out, size_t cap);

/** @brief Tell the player on stderr why the recompiled module is being built: absent ("is not there"), it is built from the disc on first boot and after an update that changes the generated code; any other reason names the failure and the rebuild.
 *
 * The one message the bind's rebuild and the launcher's module build both
 * print; scripts/check_dist_first_boot.sh greps both forms.
 * @param why The reason the module cannot be used, as the attach words it. */
void openpete_recomp_announce_build(const char* why);

/** @brief Builds the running game's recomp module from the disc on the player's machine.
 *
 * The first-boot pipeline (games/common/recomp_build.cpp), reached through
 * the running game's build record; the shared attach calls it only for a
 * game whose record names it.
 * @param disc_path The disc image (a .cue or .bin).
 * @param out_module_path Where the linked module is written.
 * @return 0 on success, -2 when the launcher job running it was asked to
 *         stop (openpete_launcher_job_cancelled; each compiler command is
 *         killed on it, the build directory is removed and no partial
 *         module is written), -1 on any other failure (reported on stderr). */
int openpete_recomp_first_boot_build(const char* disc_path,
                                     const char* out_module_path);

/** @brief Writes a game's library-entry module path for its first-boot pipeline, creating its directory.
 *
 * The pipeline half of openpete_game_library_module_path. The codegen
 * stamp is computed once per game per process; its compiler probes once
 * per process.
 * @param b The game's build record.
 * @param entry_dir The library entry's directory.
 * @param out Receives the path.
 * @param cap Capacity of out.
 * @return 0 on success, -1 when PSXRecomp is not found. */
int openpete_recomp_first_boot_module_path(const struct openpete_game_build* b,
                                           const char* entry_dir, char* out, size_t cap);

/** @brief Writes a game's first-boot inputs to a directory: PSX.EXE and one <name>.ovl per overlay of the staged converged manifest, each the bytes of the WAD.WAD entry the manifest names.
 *
 * games/common/recomp_extract_wad_entry.cpp; the extract_recomp_inputs of
 * a game whose manifest names each overlay's WAD entry. The manifest's
 * `decrypt` and `restore` keys, when present, are applied to the carved
 * bytes before each file is written.
 * @param cue_path The disc image (a .cue or .bin).
 * @param out_dir The output directory, created when absent.
 * @param manifest_path The staged manifest.toml naming the overlays.
 * @return 0 when every file was written, else -1. */
int openpete_extract_wad_entry_inputs(const char* cue_path,
                                      const char* out_dir,
                                      const char* manifest_path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_RECOMP_ATTACH_H */
