#ifndef OPENPETE_IDENTIFY_H
#define OPENPETE_IDENTIFY_H

/*
 * Game identification: which game a session runs, and the configuration
 * order around it.
 *
 * Sources, in order: the disc named on the command line (--disc, --ingest)
 * or by a game-neutral config tier, read for the serial in its SYSTEM.CNF;
 * the --game selector; the library entry the launcher or the run selected
 * (its recorded game); and, in a per-game executable, the game it links.
 * A disc and a selector that name different games refuse the launch.
 *
 * Order: openpete_options_parse_neutral resolves the tiers without a game or
 * disc scope and writes nothing; the consumers that run before the game is
 * known read that result. openpete_identify names the game;
 * openpete_options_parse_identified then sets the configuration layer's
 * game and disc and runs the one full parse, which writes the environment
 * and the spliced argv once.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_options;

typedef enum {
    OPENPETE_IDENT_NONE = 0,
    OPENPETE_IDENT_DISC,        /* the serial of a disc named on the command line or a neutral tier */
    OPENPETE_IDENT_LIBRARY,     /* the game recorded on the selected library entry */
    OPENPETE_IDENT_SELECTOR,    /* --game */
    OPENPETE_IDENT_EXECUTABLE   /* the game a per-game executable links */
} openpete_ident_source_t;

typedef struct {
    const char*             game;       /* registered game id, or NULL */
    openpete_ident_source_t source;
    char                    serial[32]; /* the disc's serial, "" when unread */
    const char*             disc_path;  /* the identified disc, or NULL */
} openpete_ident_t;

typedef struct {
    const char* selector;       /* --game, or NULL */
    const char* disc_path;      /* the disc this run boots, or NULL */
    int         from_library;   /* 1 when disc_path is a library entry */
    const char* recorded_game;  /* the library entry's recorded game, or NULL */
    const char* linked_game;    /* the game this executable links, or NULL */
} openpete_ident_in_t;

/** @brief The registered game whose disc carries `serial` in SYSTEM.CNF.
 *
 * @param serial A serial as openpete_psx_read_disc_serial writes it ("SCUS-94228").
 * @return The game id ("spyro-1"), or NULL for an unregistered serial or NULL. */
const char* openpete_game_for_serial(const char* serial);

/** @brief Whether `id` is a registered game id.
 * @param id A game id; NULL returns 0.
 * @return 1 when registered, else 0. */
int openpete_game_registered(const char* id);

/** @brief The label of an identification source ("disc", "library", "selector", "executable", "none").
 * @param s The source.
 * @return The label, never NULL. */
const char* openpete_ident_source_name(openpete_ident_source_t s);

/** @brief Name the game a session runs from its sources.
 *
 * The disc's game is the library entry's recorded game, else the game whose
 * serial the disc's SYSTEM.CNF names; an unreadable disc or an unregistered
 * serial names none. The result is the disc's game, else the selector, else
 * the linked game. Prints the refusal and returns OPENPETE_EXIT_SETUP_ERROR
 * for an unregistered selector or a selector the disc's game contradicts.
 * @param in The sources.
 * @param out Receives the result.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
int openpete_identify(const openpete_ident_in_t* in, openpete_ident_t* out);

/** @brief Refuse a game this executable does not link.
 *
 * A per-game executable runs only its own game's host code; the message
 * names the identified game, its source and the executable's game.
 * @param id The identification result.
 * @param linked_game The game this executable links.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR after the message. */
int openpete_identify_require_linked(const openpete_ident_t* id, const char* linked_game);

/** @brief Hand the identification to the configuration layer and run the one full parse into opts.
 *
 * Sets the configuration layer's game id, identified disc and source label,
 * records the boot timing line's identified mark, then initializes opts,
 * restores display_name and the identified disc, and runs
 * openpete_options_parse, which materializes the environment and argv once;
 * records the materialized mark, and re-reads the env knobs latched before
 * it (op_time_reload_env, openpete_disc_trace_reload). When the full parse
 * names no disc and no
 * EXE, opts->disc_path is the identified disc; --ingest is cleared once the
 * library has consumed it (from_library).
 * @param opts Receives the parse; any previous content is discarded.
 * @param argc Argument count.
 * @param argv Arguments as typed.
 * @param id The identification result.
 * @param display_name The game's display name, or NULL for the default.
 * @return openpete_options_parse's result. */
int openpete_options_parse_identified(struct openpete_options* opts, int argc,
                                      char** argv, const openpete_ident_t* id,
                                      const char* display_name);

/** @brief Disc-identity hook for the [disc.*] scopes from the image alone: its serial, no hash.
 *
 * The form of openpete_disc_identity_fn for a game with no library: the
 * serial is read from SYSTEM.CNF; a hash is never computed.
 * @param disc_path The image, or NULL.
 * @param serial Receives the serial, "" when unread.
 * @param serial_cap Its capacity.
 * @param sha Receives "".
 * @param sha_cap Its capacity.
 * @return 0 when a serial was read, else -1. */
int openpete_disc_identity_serial(const char* disc_path, char* serial,
                                  size_t serial_cap, char* sha, size_t sha_cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_IDENTIFY_H */
