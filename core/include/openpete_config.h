#ifndef OPENPETE_CONFIG_H
#define OPENPETE_CONFIG_H

/*
 * openpete.toml — the single layered config file, and the only place a
 * persistent setting is written.
 *
 * PRECEDENCE (ascending — later wins):
 *
 *    0  built-in defaults
 *    1  openpete.toml   global scope
 *    2  openpete.toml   [game.<id>]
 *    3  openpete.toml   [disc.<SERIAL>]
 *    4  openpete.toml   [disc.<sha256>]
 *    5  openpete.toml   [saved]           (written by the in-game menus)
 *    6  the real process environment
 *    7  <movie>.bk2.args sidecar
 *    8  the command line as typed
 *    9  an in-game edit (a live mutation, not a layer)
 *
 * Tiers 1-4 subdivide by FILE (exe-dir < user-dir) and then by SPELLING
 * (bare keys < [env] < [args]) — scope is the major sort key, notation
 * the minor one, and in-file spellings rank as their real-world
 * counterparts do so that moving a line between the file and the shell
 * never changes which of two settings wins.
 *
 * The mechanism that makes the ordering real is CANONICAL FLATTENING:
 * a knob's winning layer is decided here, and only that one spelling is
 * materialized (an argv token or a setenv). Splicing and setenv-ing every
 * layer would hand the args-vs-env tiebreak back to each knob's own
 * sentinel-unset consumer, which is blind to scope — a low-specificity
 * [args] would beat a high-specificity [env].
 */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- boot ---------------------------------------------------------- */

/** @brief Load the config files, resolve every layer, and materialize each knob's winning spelling.
 *
 * Seeds the keymap, scans argv for --no-config, --config-report,
 * --keys-report and --config (OPENPETE_CONFIG when --config is absent;
 * the value "none" disables the files), reads the exe-dir and per-user
 * openpete.toml (or only the --config file), harvests the global, game,
 * disc-serial, disc-sha and [saved] scopes, resolves the keymap, and
 * materializes the winners: `[env]` winners are set in the process
 * environment (a variable the real environment already carries still
 * wins), and flag winners are spliced into a new argv after argv[0], left
 * of the caller's arguments, so a typed flag still overrides them.
 *
 * Called once from openpete_options_parse, before any getenv consumer
 * runs, because features cache their env reads on first use.
 * @param argc Argument count, including a bk2 sidecar splice.
 * @param argv Arguments; may carry the bk2 sidecar tokens left of the typed ones.
 * @param typed_argc Count of the arguments as typed, so the config report can label a sidecar token apart from a typed one; both outrank every file tier.
 * @param out_argc Receives the spliced argument count when the return is 1.
 * @param out_argv Receives the spliced argv when the return is 1; heap-owned and retained for the process lifetime.
 * @return 1 when tokens were spliced, 0 when none were, -1 on a config error that aborts startup (already printed to stderr). */
int openpete_config_apply(int argc, char** argv, int typed_argc,
                          int* out_argc, char*** out_argv);

/* The game id selecting the [game.<id>] scope ("spyro-1"). Set before
 * openpete_config_apply; unset means that scope never matches. */
void openpete_config_set_game_id(const char* id);

/* How the [disc.*] scopes get their keys. openpete_config_apply calls this
 * hook once it knows which image will boot, to learn that image's serial
 * and/or content hash. Cheap by contract: an ISO serial read, or a library
 * entry's already-recorded hash — never an image hash, which would mean
 * digesting ~500 MB on every launch. Fills `serial` / `sha` with "" for
 * whatever it cannot resolve; returns 0 if it resolved anything.
 *
 * Registered by the game before openpete_config_apply. A function pointer
 * rather than a weak symbol: MinGW's PE/COFF linker mis-resolves weak
 * overrides across static libraries (the same reason main.cpp registers
 * its other game hooks explicitly). Unregistered = no [disc.*] scopes. */
typedef int (*openpete_disc_identity_fn)(const char* disc_path,
                                         char* serial, size_t serial_cap,
                                         char* sha, size_t sha_cap);
void openpete_config_set_disc_identity_fn(openpete_disc_identity_fn fn);

/* ---- diagnostics --------------------------------------------------- */

/** @brief Print the config files, scopes, write target, directory layout and knob resolution table.
 *
 * The table lists every knob any layer touched with its value, winning
 * layer and the layers it shadowed, then the unshadowed contributions
 * with no schema row (raw flags and debug env vars) below the real
 * environment tier. The game mains call it for --config-report, printing
 * to stdout and exiting 0.
 * @param out Destination stream; NULL prints to stderr. */
void openpete_config_report(FILE* out);

/** @brief Return 1 when openpete_config_apply saw --config-report in argv, else 0.
 *
 * No caller reads it; the game mains test openpete_options_t.config_report
 * instead.
 * @return 1 when --config-report was passed, else 0. */
int openpete_config_report_requested(void);

/* Source label for a token the config spliced into argv ("game.spyro-1.args"),
 * or NULL when the token came from the command line. Lets the flag parser
 * point at the config file instead of at a flag the user never typed. */
const char* openpete_config_token_source(const char* token);

/* ---- provenance (the M overlay / O menu) ---------------------------- */

/** @brief The layer that won a knob, as a display string.
 *
 * The winning contribution's origin: "cli", "bk2 sidecar", "env" (the real
 * environment), "saved", or a file scope with its spelling ("global",
 * "game.spyro-1.env", "disc.SCUS-94228.args"). "default" when no layer set
 * the knob or the id is unknown.
 * @param knob_id Schema knob id.
 * @return The label, never NULL. */
const char* openpete_config_knob_layer(const char* knob_id);

/** @brief Whether a knob's winner is a launch tier: the real environment, the bk2 sidecar or the command line.
 *
 * A saved value does not take effect on the next launch while that
 * override is present. The overlay shows the same condition as a "pinned"
 * badge through openpete_config_knob_pin_spelling.
 * @param knob_id Schema knob id.
 * @return 1 when pinned, 0 otherwise or for an unknown id. */
int openpete_config_knob_pinned(const char* knob_id);

/** @brief The launch-tier spelling that pinned a knob ("--lod", "$OPENPETE_MODS").
 *
 * @param knob_id Schema knob id.
 * @return The spelling, or NULL when the knob is not pinned. */
const char* openpete_config_knob_pin_spelling(const char* knob_id);

/* ---- persistence (the in-game menus) -------------------------------- */

/* Write `value` (already in the knob's CLI vocabulary) into the managed
 * [saved] zone, in memory. Call openpete_config_save_flush to commit. */
int openpete_config_save_knob(const char* knob_id, const char* value);

/** @brief Write the in-memory [saved] zone to the save target.
 *
 * Everything above the managed marker is copied through byte for byte, so
 * hand-written config and its comments survive; a file without the marker
 * is copied whole. The marker, a note and a [saved] table of every saved
 * knob with a schema row, each written as its knob's type, follow. The file
 * is written to `<path>.tmp`, the target removed, and the temporary renamed
 * over it, so a failure between the remove and the rename leaves only the
 * temporary.
 * @return 0 on success, -1 when there is no save target or a write, remove
 *         or rename fails. */
int openpete_config_save_flush(void);

/** @brief Drop every saved knob and rewrite the save target with an empty managed zone.
 *
 * @return openpete_config_save_flush's result. */
int openpete_config_reset_saved(void);

/* Path the managed zone is written to, for UI display. Never NULL. */
const char* openpete_config_save_path(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CONFIG_H */
