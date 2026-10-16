/* core/mods/mod_games.h - the games a mod names besides the one it runs on.
 *
 * A manifest's [mod] requires_games and optional_games list games whose
 * discs the mod reads. This unit holds the per-mod list, the decision of
 * where a disc call for a game goes, the installed-disc table the loader
 * fills once at mods init, and the rows of mods refused at load for a
 * missing game. It reads no file and opens no disc: the loader supplies
 * the library lookup and reads the discs, so the decisions are testable
 * without either. */
#ifndef OPENPETE_MOD_GAMES_H
#define OPENPETE_MOD_GAMES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ids one mod may name across both keys, and the bytes of one id. */
#define MOD_GAMES_MAX   8
#define MOD_GAME_ID_MAX 24
/* Games the installed-disc table holds, and the bytes of one path. */
#define MOD_GAMES_TABLE_MAX 8
#define MOD_GAMES_PATH_MAX  1024

/** @brief One game a manifest names, with the key that named it. */
typedef struct {
    char id[MOD_GAME_ID_MAX]; /**< The game id ("spyro-2"). */
    int  required;            /**< 1 from requires_games, 0 from optional_games. */
} mod_game_row_t;

/** @brief The games one mod names in requires_games and optional_games. */
typedef struct {
    mod_game_row_t rows[MOD_GAMES_MAX]; /**< The ids, in manifest order. */
    int            n;                   /**< Rows in use. */
} mod_game_list_t;

/* The role a list gives a game. */
enum {
    MOD_GAME_UNLISTED = 0,
    MOD_GAME_OPTIONAL = 1,
    MOD_GAME_REQUIRED = 2
};

/* Where a disc call for a game goes. */
typedef enum {
    MOD_DISC_INVALID = 0, /* NULL or empty game id */
    MOD_DISC_BOOTED,      /* the running game: the disc this session booted */
    MOD_DISC_OTHER,       /* a listed game with a disc in the library */
    MOD_DISC_ABSENT,      /* a listed game with no disc in the library */
    MOD_DISC_UNLISTED     /* a game named in neither list: refused */
} mod_disc_route_t;

/** @brief Add a game id to a mod's list.
 *
 * A game named twice keeps one row; a game in both lists is required.
 * @param l The list.
 * @param id The game id; NULL, empty or longer than MOD_GAME_ID_MAX - 1
 *        bytes is refused.
 * @param required 1 for requires_games, 0 for optional_games.
 * @return 0 when added, 1 when already listed, -1 when refused or the list
 *         is full. */
int mod_game_list_add(mod_game_list_t* l, const char* id, int required);

/** @brief Return the role a mod's list gives a game.
 *
 * @param l The list.
 * @param id The game id.
 * @return MOD_GAME_REQUIRED, MOD_GAME_OPTIONAL or MOD_GAME_UNLISTED. */
int mod_game_list_role(const mod_game_list_t* l, const char* id);

/** @brief Library lookup the table is filled from.
 *
 * @param game A game id.
 * @param cue Receives the path of the game's disc (a cue or an image).
 * @param cue_cap Capacity of @p cue.
 * @param bin Receives the sector image behind @p cue.
 * @param bin_cap Capacity of @p bin.
 * @return 0 when the library holds a disc of @p game, -1 otherwise. */
typedef int (*mod_games_lookup_fn)(const char* game, char* cue, size_t cue_cap,
                                   char* bin, size_t bin_cap);

/** @brief Fill the installed-disc table from a library lookup, once per session.
 *
 * Each id other than @p running is looked up; the table keeps every id,
 * with its paths when the lookup found a disc. Called on one thread before
 * any mod code runs; every reader after it only reads.
 * @param running The running game's id, which is never looked up.
 * @param ids The game ids to look up.
 * @param n Count of @p ids; at most MOD_GAMES_TABLE_MAX are kept.
 * @param lookup The library lookup. */
void mod_games_snapshot(const char* running, const char* const* ids, int n,
                        mod_games_lookup_fn lookup);

/** @brief Return the table slot of a game with a disc in the library.
 *
 * @param game The game id.
 * @return The slot (0 .. MOD_GAMES_TABLE_MAX - 1), or -1 when the game
 *         has no disc in the library or is not in the table. */
int mod_games_slot(const char* game);

/** @brief Return a table slot's disc path.
 *
 * @param slot A slot mod_games_slot returned.
 * @param want_bin Nonzero for the sector image, zero for the path the
 *        library names (the entry's cue).
 * @return The path, or NULL for a slot out of range or with no disc. */
const char* mod_games_slot_path(int slot, int want_bin);

/** @brief Report whether a game is installed: the running game, or a game with a disc in the library.
 *
 * @param running The running game's id.
 * @param game The game id.
 * @return 1 when installed, 0 otherwise or for a NULL id. */
int mod_games_installed(const char* running, const char* game);

/** @brief Decide where a mod's disc call for a game goes.
 *
 * @param l The mod's list.
 * @param running The running game's id.
 * @param game The game id the call names.
 * @return The route. */
mod_disc_route_t mod_games_route(const mod_game_list_t* l, const char* running,
                                 const char* game);

/** @brief Return the first required game, other than the running one, with no disc in the library.
 *
 * @param l The mod's list.
 * @param running The running game's id.
 * @return The id (owned by @p l), or NULL when every required game is
 *         installed. */
const char* mod_games_first_missing(const mod_game_list_t* l, const char* running);

/** @brief Record a mod refused at load, for the Mods panel.
 *
 * Rows live for the session; a second row for the same id replaces the
 * reason.
 * @param id The mod id.
 * @param reason One line naming why. */
void mod_games_refused_add(const char* id, const char* reason);

#ifdef __cplusplus
}
#endif
#endif
