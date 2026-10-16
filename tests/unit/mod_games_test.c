// tests/unit/mod_games_test.c - the games a mod names besides the one it
// runs on (core/mods/mod_games.c): list merging and limits, the route of a
// disc call for every role and install state, the first missing required
// game, the installed query, and the refused-mod rows.

#include "mod_games.h"
#include "openpete_mods.h"

#include <stdio.h>
#include <string.h>

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

/* The fake library: spyro-2 has a disc, spyro-3 has none. */
static int s_lookups;

static int fake_lookup(const char* game, char* cue, size_t cue_cap, char* bin,
                       size_t bin_cap) {
    s_lookups++;
    if (strcmp(game, "spyro-2") != 0) return -1;
    snprintf(cue, cue_cap, "/lib/s2/disc.cue");
    snprintf(bin, bin_cap, "/lib/s2/disc.bin");
    return 0;
}

static void test_list(void) {
    mod_game_list_t l;
    memset(&l, 0, sizeof l);
    CHECK(mod_game_list_add(&l, "spyro-2", 0) == 0);
    CHECK(mod_game_list_role(&l, "spyro-2") == MOD_GAME_OPTIONAL);
    /* Named again as required: one row, now required. */
    CHECK(mod_game_list_add(&l, "spyro-2", 1) == 1);
    CHECK(l.n == 1);
    CHECK(mod_game_list_role(&l, "spyro-2") == MOD_GAME_REQUIRED);
    /* Required first, optional second: stays required. */
    CHECK(mod_game_list_add(&l, "spyro-3", 1) == 0);
    CHECK(mod_game_list_add(&l, "spyro-3", 0) == 1);
    CHECK(mod_game_list_role(&l, "spyro-3") == MOD_GAME_REQUIRED);
    CHECK(mod_game_list_role(&l, "spyro-1") == MOD_GAME_UNLISTED);
    CHECK(mod_game_list_add(&l, "", 1) == -1);
    CHECK(mod_game_list_add(&l, NULL, 1) == -1);
    CHECK(mod_game_list_add(&l, "an-id-longer-than-the-row-holds", 1) == -1);
    char id[8];
    for (int i = l.n; i < MOD_GAMES_MAX; i++) {
        snprintf(id, sizeof id, "g%d", i);
        CHECK(mod_game_list_add(&l, id, 0) == 0);
    }
    CHECK(mod_game_list_add(&l, "one-more", 0) == -1);
}

static void test_routes(void) {
    const char* ids[] = {"spyro-1", "spyro-2", "spyro-3"};
    s_lookups = 0;
    mod_games_snapshot("spyro-1", ids, 3, fake_lookup);
    CHECK(s_lookups == 2); /* the running game is never looked up */
    CHECK(mod_games_slot("spyro-2") >= 0);
    CHECK(mod_games_slot("spyro-3") == -1);
    CHECK(mod_games_slot("spyro-1") == -1);
    CHECK(strcmp(mod_games_slot_path(mod_games_slot("spyro-2"), 0), "/lib/s2/disc.cue") == 0);
    CHECK(strcmp(mod_games_slot_path(mod_games_slot("spyro-2"), 1), "/lib/s2/disc.bin") == 0);
    CHECK(mod_games_slot_path(-1, 0) == NULL);

    CHECK(mod_games_installed("spyro-1", "spyro-1") == 1);
    CHECK(mod_games_installed("spyro-1", "spyro-2") == 1);
    CHECK(mod_games_installed("spyro-1", "spyro-3") == 0);
    CHECK(mod_games_installed("spyro-1", "spyro-9") == 0);
    CHECK(mod_games_installed("spyro-1", NULL) == 0);

    mod_game_list_t l;
    memset(&l, 0, sizeof l);
    (void)mod_game_list_add(&l, "spyro-2", 0);
    (void)mod_game_list_add(&l, "spyro-3", 0);
    CHECK(mod_games_route(&l, "spyro-1", "spyro-1") == MOD_DISC_BOOTED);
    CHECK(mod_games_route(&l, "spyro-1", "spyro-2") == MOD_DISC_OTHER);
    CHECK(mod_games_route(&l, "spyro-1", "spyro-3") == MOD_DISC_ABSENT);
    CHECK(mod_games_route(&l, "spyro-1", NULL) == MOD_DISC_INVALID);
    CHECK(mod_games_route(&l, "spyro-1", "") == MOD_DISC_INVALID);
    mod_game_list_t none;
    memset(&none, 0, sizeof none);
    /* Installed but unlisted: refused. */
    CHECK(mod_games_route(&none, "spyro-1", "spyro-2") == MOD_DISC_UNLISTED);
    CHECK(mod_games_route(&none, "spyro-1", "spyro-1") == MOD_DISC_BOOTED);

    /* Required games: the running game is implied; spyro-3 is missing. */
    mod_game_list_t req;
    memset(&req, 0, sizeof req);
    (void)mod_game_list_add(&req, "spyro-1", 1);
    (void)mod_game_list_add(&req, "spyro-2", 1);
    CHECK(mod_games_first_missing(&req, "spyro-1") == NULL);
    (void)mod_game_list_add(&req, "spyro-3", 0);
    CHECK(mod_games_first_missing(&req, "spyro-1") == NULL);
    (void)mod_game_list_add(&req, "spyro-3", 1);
    CHECK(mod_games_first_missing(&req, "spyro-1") != NULL &&
          strcmp(mod_games_first_missing(&req, "spyro-1"), "spyro-3") == 0);
    /* The same list on Spyro 3: spyro-3 is the running game, spyro-1 is missing. */
    mod_games_snapshot("spyro-3", ids, 3, fake_lookup);
    CHECK(strcmp(mod_games_first_missing(&req, "spyro-3"), "spyro-1") == 0);
}

static void test_refused(void) {
    CHECK(openpete_mods_refused_count() == 0);
    mod_games_refused_add("a", "requires spyro-3");
    mod_games_refused_add("b", "requires spyro-9");
    mod_games_refused_add("a", "requires spyro-2");
    CHECK(openpete_mods_refused_count() == 2);
    const char *id = NULL, *why = NULL;
    CHECK(openpete_mods_refused_at(0, &id, &why) == 1);
    CHECK(strcmp(id, "a") == 0 && strcmp(why, "requires spyro-2") == 0);
    CHECK(openpete_mods_refused_at(2, &id, &why) == 0);
}

int main(void) {
    test_list();
    test_routes();
    test_refused();
    if (s_fail) {
        printf("mod_games_test: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("mod_games_test: PASS\n");
    return 0;
}
