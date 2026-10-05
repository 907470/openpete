/* openpete_surface.h - the user-facing surface of a game binary as data.
 *
 * OPENPETE_SURFACE_DUMP=<path> makes the binary write its flags, knob
 * registry rows, settings-menu rows, key actions, config directives and
 * vocabularies as JSON to <path>, each with its declared default, run the
 * defaults selfcheck, and exit before option parsing, mods, disc lookup or
 * the recomp module. Free text (tooltips, display names) is left out, so a
 * wording edit does not change the dump.
 *
 * A game contributes the rows and vocabularies its features register at
 * apply-options time through game_surface (game_extras.h), which reads the
 * same static tables the features do and runs no feature code. */
#ifndef OPENPETE_SURFACE_H
#define OPENPETE_SURFACE_H

#include "openpete_knobs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The collector game_surface writes into; opaque to the game. */
typedef struct openpete_surface_sink openpete_surface_sink_t;

/** @brief Add one vocabulary entry: a word a flag accepts from a game table.
 *
 * @param sink The collector.
 * @param table Vocabulary table ("level", "cutscene", "area", "cheat").
 * @param flag The flag that takes the word ("--skip-to-level").
 * @param slug The word as the resolver matches it. */
void openpete_surface_vocab(openpete_surface_sink_t* sink, const char* table,
                            const char* flag, const char* slug);

/** @brief Add one registry row a game feature registers at apply-options time.
 *
 * The row is copied; its strings must outlive the dump.
 * @param sink The collector.
 * @param row The row template. */
void openpete_surface_row(openpete_surface_sink_t* sink, const knob_desc_t* row);

/** @brief Write the surface dump and exit when OPENPETE_SURFACE_DUMP names a path; return otherwise.
 *
 * Call from main before openpete_options_parse, so no env or config layer
 * has run. Prints one stderr line when it fires. The process exits 0 when
 * the dump is written and the selfcheck passes, 1 otherwise.
 * @param argv0 The program path, argv[0], for the selfcheck's parses. */
void openpete_surface_dump_run_if_requested(const char* argv0);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SURFACE_H */
