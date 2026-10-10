// games/common/recomp_attach.h - attach the game's recomp module before boot.
//
// Two implementations, selected by OPENPETE_RECOMP_LINKAGE in
// cmake/openpete_game.cmake:
//   recomp_attach_static.c - registers the descriptor linked into the exe.
//   recomp_attach_shared.c - loads <tag>-recomp.{so,dll}, binds its
//                            descriptor and the host->module forwarders the
//                            game's module_imports.h lists.
#ifndef OPENPETE_GAME_RECOMP_ATTACH_H
#define OPENPETE_GAME_RECOMP_ATTACH_H

#include <stddef.h>

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
 *     platform library name next to the exe).
 * @param disc_path The disc a missing or unusable module is rebuilt from.
 * @param allow_build Nonzero to permit that rebuild.
 * @return 0 on success, -1 when no usable module could be registered. */
int openpete_game_recomp_attach(const char* module_path_override,
                                const char* disc_path,
                                int allow_build);

/** @brief Writes a library entry's module path, modules/<codegen stamp>/<tag>-recomp.{so,dll} under the entry, creating its directory.
 *
 * Defined by the first-boot pipeline in a build that has one: the codegen
 * stamp hashes PSXRecomp, the staged manifest and source kit, and the
 * compiler, so a module built by another version is never loaded. Every
 * other build defines it to fail, and such a build loads no module from
 * the disc library.
 * @param entry_dir The library entry's directory.
 * @param out Receives the path.
 * @param cap Capacity of out.
 * @return 0 on success, -1 when this build has no first-boot pipeline or
 *         PSXRecomp is not found. */
int openpete_game_library_module_path(const char* entry_dir, char* out, size_t cap);

/** @brief Builds the game's recomp module from the disc on the player's machine.
 *
 * Defined by the game's first-boot pipeline; called by the shared attach
 * only in a build configured with OPENPETE_RECOMP_FIRST_BOOT.
 * @param disc_path The disc image (a .cue or .bin).
 * @param out_module_path Where the linked module is written.
 * @return 0 on success, -1 on any failure (reported on stderr). */
int openpete_game_recomp_build_module(const char* disc_path,
                                      const char* out_module_path);

/** @brief Writes the executables the first-boot module build recompiles, carved from the disc, to a directory.
 *
 * Defined by the game's first-boot pipeline: PSX.EXE and one <name>.ovl
 * per overlay of the staged converged manifest.
 * @param cue_path The disc image (a .cue or .bin).
 * @param out_dir The output directory, created when absent.
 * @param manifest_path The staged manifest.toml naming the overlays.
 * @return 0 when every file was written, else -1. */
int openpete_game_extract_recomp_inputs(const char* cue_path,
                                        const char* out_dir,
                                        const char* manifest_path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_RECOMP_ATTACH_H */
