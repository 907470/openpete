#pragma once
/* level_table.h - a game's table of selectable levels, and the lookups
 * --skip-to-level, the dev-overlay level warp and the terrain feeder's level
 * names make against it.
 *
 * Each game holds one table in level-number order; the per-game
 * differences are the table's rows, its alias rows and the named-target
 * resolver flags. Parse and lookup only: nothing here reads or writes PSX
 * RAM. */

#include <stddef.h>

struct openpete_surface_sink;

/* One selectable target. `name` is the command-line spelling, `display` the
 * dev-overlay dropdown label (NULL on an alias row), `number` the game's own
 * level number. */
typedef struct {
    const char* name;
    const char* display;
    int         number;
} game_level_t;

typedef struct {
    const game_level_t* levels;     /* the dropdown's rows, in table order */
    size_t              count;
    const game_level_t* aliases;    /* extra command-line spellings, or NULL */
    size_t              n_aliases;
    const char*         kind;       /* noun in printed lists ("level") */
    const char*         flag;       /* the flag the names are typed to */
    unsigned            resolve_flags; /* NAMED_TARGET_* bits */
} game_level_table_t;

/* Initialise a game_level_table_t's row and alias fields from two arrays. */
#define GAME_LEVEL_ROWS(arr) (arr), (sizeof(arr) / sizeof((arr)[0]))

/** @brief Resolve a command-line name or number against a level table.
 *
 * Searches the rows, then the aliases, through named_target_resolve with the
 * table's flags; a failure prints the rows' names to stderr.
 * @param t The table.
 * @param arg The command-line value, or NULL.
 * @return The level number, or -1. */
int game_level_resolve(const game_level_table_t* t, const char* arg);

/** @brief Report every row and alias name of a table to the surface dump as a vocabulary of the table's flag.
 *
 * @param t The table.
 * @param sink The dump's collector. */
void game_level_surface(const game_level_table_t* t,
                        struct openpete_surface_sink* sink);

/** @brief Write a level's command-line name, or "level<N>" when no row holds the number.
 *
 * @param t The table.
 * @param number The level number.
 * @param out Receives the NUL-terminated name, truncated to cap.
 * @param cap The size of out in bytes; at least 1. */
void game_level_name(const game_level_table_t* t, int number, char* out,
                     size_t cap);

/** @brief Find a level number's row index.
 *
 * @param t The table.
 * @param number The level number.
 * @return The index into t->levels, or -1 when no row holds the number. */
int game_level_index(const game_level_table_t* t, int number);
