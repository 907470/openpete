/* core/mods/mod_games.c - the games a mod names besides the one it runs on
 * (contract in mod_games.h). */
#include "mod_games.h"

#include "openpete_mods.h" /* openpete_mods_refused_* */

#include <stdio.h>
#include <string.h>

/* One game of the installed-disc table. */
typedef struct {
    char id[MOD_GAME_ID_MAX];
    char cue[MOD_GAMES_PATH_MAX];
    char bin[MOD_GAMES_PATH_MAX];
    int  installed;
} mod_games_entry_t;

static mod_games_entry_t s_table[MOD_GAMES_TABLE_MAX];
static int               s_table_n;

/* Mods refused at load for a game, one row per mod id. */
#define MOD_REFUSED_MAX 64
static struct {
    char id[64];
    char reason[192];
} s_refused[MOD_REFUSED_MAX];
static int s_refused_n;

int mod_game_list_add(mod_game_list_t* l, const char* id, int required) {
    if (!l || !id || !*id || strlen(id) >= MOD_GAME_ID_MAX) return -1;
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->rows[i].id, id) != 0) continue;
        if (required) l->rows[i].required = 1;
        return 1;
    }
    if (l->n >= MOD_GAMES_MAX) return -1;
    snprintf(l->rows[l->n].id, MOD_GAME_ID_MAX, "%s", id);
    l->rows[l->n].required = required ? 1 : 0;
    l->n++;
    return 0;
}

int mod_game_list_role(const mod_game_list_t* l, const char* id) {
    if (!l || !id) return MOD_GAME_UNLISTED;
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->rows[i].id, id) == 0)
            return l->rows[i].required ? MOD_GAME_REQUIRED : MOD_GAME_OPTIONAL;
    return MOD_GAME_UNLISTED;
}

void mod_games_snapshot(const char* running, const char* const* ids, int n,
                        mod_games_lookup_fn lookup) {
    memset(s_table, 0, sizeof s_table);
    s_table_n = 0;
    for (int i = 0; i < n && s_table_n < MOD_GAMES_TABLE_MAX; i++) {
        if (!ids[i] || strlen(ids[i]) >= MOD_GAME_ID_MAX) continue;
        if (running && strcmp(ids[i], running) == 0) continue;
        mod_games_entry_t* e = &s_table[s_table_n++];
        snprintf(e->id, sizeof e->id, "%s", ids[i]);
        e->installed = lookup && lookup(e->id, e->cue, sizeof e->cue,
                                        e->bin, sizeof e->bin) == 0;
    }
}

int mod_games_slot(const char* game) {
    if (!game) return -1;
    for (int i = 0; i < s_table_n; i++)
        if (strcmp(s_table[i].id, game) == 0)
            return s_table[i].installed ? i : -1;
    return -1;
}

const char* mod_games_slot_path(int slot, int want_bin) {
    if (slot < 0 || slot >= s_table_n || !s_table[slot].installed) return NULL;
    return want_bin ? s_table[slot].bin : s_table[slot].cue;
}

int mod_games_installed(const char* running, const char* game) {
    if (!game) return 0;
    if (running && strcmp(game, running) == 0) return 1;
    return mod_games_slot(game) >= 0;
}

mod_disc_route_t mod_games_route(const mod_game_list_t* l, const char* running,
                                 const char* game) {
    if (!game || !*game) return MOD_DISC_INVALID;
    if (running && strcmp(game, running) == 0) return MOD_DISC_BOOTED;
    if (mod_game_list_role(l, game) == MOD_GAME_UNLISTED) return MOD_DISC_UNLISTED;
    return mod_games_slot(game) >= 0 ? MOD_DISC_OTHER : MOD_DISC_ABSENT;
}

const char* mod_games_first_missing(const mod_game_list_t* l, const char* running) {
    if (!l) return NULL;
    for (int i = 0; i < l->n; i++) {
        if (!l->rows[i].required) continue;
        if (!mod_games_installed(running, l->rows[i].id)) return l->rows[i].id;
    }
    return NULL;
}

void mod_games_refused_add(const char* id, const char* reason) {
    if (!id) return;
    int i = 0;
    while (i < s_refused_n && strcmp(s_refused[i].id, id) != 0) i++;
    if (i == s_refused_n) {
        if (s_refused_n >= MOD_REFUSED_MAX) return;
        s_refused_n++;
    }
    snprintf(s_refused[i].id, sizeof s_refused[i].id, "%s", id);
    snprintf(s_refused[i].reason, sizeof s_refused[i].reason, "%s",
             reason ? reason : "");
}

int openpete_mods_refused_count(void) { return s_refused_n; }

int openpete_mods_refused_at(int i, const char** id, const char** reason) {
    if (i < 0 || i >= s_refused_n) return 0;
    if (id) *id = s_refused[i].id;
    if (reason) *reason = s_refused[i].reason;
    return 1;
}
