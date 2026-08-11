#ifndef OPENPETE_CONFIG_H
#define OPENPETE_CONFIG_H

/*
 * openpete.toml — the single layered config file.
 *
 * Replaces the two sidecar files this project grew organically (an
 * `openpete.opts` whose tokens were spliced into argv, and an
 * `openpete.env` whose KEY=VALUE lines were setenv'd). Both are still
 * read, at the bottom of the stack, until they are retired.
 *
 * PRECEDENCE (ascending — later wins). See
 * notes/plans/openpete-config-consolidation.md for the derivation:
 *
 *    0  built-in defaults
 *    1  legacy openpete.env
 *    2  legacy openpete.opts
 *    3  openpete.toml   global scope
 *    4  openpete.toml   [game.<id>]
 *    5  openpete.toml   [disc.<SERIAL>]
 *    6  openpete.toml   [disc.<sha256>]
 *    7  openpete.toml   [saved]           (written by the in-game menus)
 *    8  the real process environment
 *    9  <movie>.bk2.args sidecar
 *   10  the command line as typed
 *   11  an in-game edit (a live mutation, not a layer)
 *
 * Tiers 3-6 subdivide by FILE (exe-dir < user-dir) and then by SPELLING
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

/* Load the config file(s), resolve every layer, and materialize the
 * result: `[env]` winners are applied to the process environment (the
 * real environment still wins), and flag winners are returned as a new
 * argv with the tokens spliced in after argv[0] — left of the caller's
 * own args, so anything typed still overrides.
 *
 * Called once from openpete_options_parse, before ANY getenv consumer:
 * features lazy-cache their env reads on first use.
 *
 * `argv` may already carry the bk2 sidecar splice; `typed_argc` is the
 * original count so the report can tell a sidecar flag from a typed one
 * (both outrank every file tier, so it changes no decision).
 *
 * Returns 1 with *out_argc / *out_argv set (heap-owned, retained for the
 * process lifetime), 0 when nothing was spliced, or -1 on a config error
 * that should abort startup (already reported to stderr). */
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

/* The resolution table: every knob any layer touched, its winning layer,
 * and what that layer shadowed. Printed by --config-report (which exits
 * straight afterwards) and at debug level on every boot. */
void openpete_config_report(FILE* out);

/* True when --config-report was passed (the caller prints and exits). */
int openpete_config_report_requested(void);

/* Source label for a token the config spliced into argv ("game.spyro-1.args"),
 * or NULL when the token came from the command line. Lets the flag parser
 * point at the config file instead of at a flag the user never typed. */
const char* openpete_config_token_source(const char* token);

/* ---- provenance (the M overlay / O menu) ---------------------------- */

/* Winning layer for a knob id, as a display string: "default", "cli",
 * "env", "saved", "game.spyro-1.args", ... Never NULL. */
const char* openpete_config_knob_layer(const char* knob_id);

/* True when the knob's winner is a launch tier (real env / sidecar / CLI),
 * i.e. a saved value would NOT take effect next launch while that override
 * is present. The UI renders this as a "pinned by --flag" badge. */
int openpete_config_knob_pinned(const char* knob_id);

/* The launch-tier spelling that pinned it ("--lod", "$OPENPETE_MODS"), or
 * NULL when the knob is not pinned. */
const char* openpete_config_knob_pin_spelling(const char* knob_id);

/* ---- persistence (the in-game menus) -------------------------------- */

/* Write `value` (already in the knob's CLI vocabulary) into the managed
 * [saved] zone, in memory. Call openpete_config_save_flush to commit. */
int openpete_config_save_knob(const char* knob_id, const char* value);

/* Commit the managed zone to disk. Everything above the managed marker is
 * copied through byte for byte, so hand-written config and its comments
 * survive; the zone below is regenerated. Atomic (temp + rename).
 * Returns 0 on success. */
int openpete_config_save_flush(void);

/* Drop every saved knob and rewrite (an empty managed zone). */
int openpete_config_reset_saved(void);

/* Path the managed zone is written to, for UI display. Never NULL. */
const char* openpete_config_save_path(void);

/* ---- migration ------------------------------------------------------ */

/* --convert-config: write an openpete.toml equivalent to whatever legacy
 * openpete.opts / openpete.env exist, rename the originals to .bak, and
 * report. Returns an OPENPETE_EXIT_* code; the caller exits with it. */
int openpete_config_convert_legacy(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CONFIG_H */
