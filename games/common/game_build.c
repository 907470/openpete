// games/common/game_build.c - the running game's build record, its selection
// among the linked games, and the entries that forward to it. See
// game_build.h.

#include "game_build.h"

#include "game_host_desc.h"
#include "openpete_identify.h"
#include "openpete_psx.h"

#include <string.h>

static const openpete_game_build_t* s_build;

/** @brief Return the linked game with this id.
 *
 * @param id A game id, or NULL.
 * @return The game, or NULL when none is linked under that id. */
static const openpete_game_build_t* linked(const char* id) {
    if (!id) return NULL;
    for (size_t i = 0; i < openpete_game_build_count; i++)
        if (!strcmp(openpete_game_builds[i]->id, id)) return openpete_game_builds[i];
    return NULL;
}

/** @brief Return the value of the last `flag value` pair in argv.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @param flag The flag spelling ("--disc").
 * @return The value, or NULL when the flag is absent or has no value. */
static const char* flag_value(int argc, char** argv, const char* flag) {
    const char* v = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) v = argv[i + 1];
    return v;
}

/** @brief Return the linked game whose serial the disc image carries.
 *
 * @param path The image, or NULL.
 * @return The game, or NULL for no image, an unreadable one, or a game this executable does not link. */
static const openpete_game_build_t* disc_game(const char* path) {
    char serial[32];
    if (!path || openpete_psx_read_disc_serial(path, serial, sizeof serial) != 0) return NULL;
    return linked(openpete_game_for_serial(serial));
}

/** @brief Return the linked game whose exe_name is the invoked name, without directory or ".exe".
 *
 * @param argv0 The invoked path, or NULL.
 * @return The game, or NULL. */
static const openpete_game_build_t* named_game(const char* argv0) {
    if (!argv0) return NULL;
    const char* base = argv0;
    for (const char* p = argv0; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, ".exe")) n -= 4;
    for (size_t i = 0; i < openpete_game_build_count; i++) {
        const char* e = openpete_game_builds[i]->exe_name;
        if (strlen(e) == n && !strncmp(e, base, n)) return openpete_game_builds[i];
    }
    return NULL;
}

const openpete_game_build_t* openpete_game_build_named(int argc, char** argv) {
    if (openpete_game_build_count == 0) return NULL;
    if (openpete_game_build_count == 1) return openpete_game_builds[0];
    const openpete_game_build_t* b = disc_game(flag_value(argc, argv, "--disc"));
    if (!b) b = disc_game(flag_value(argc, argv, "--ingest"));
    if (!b) b = linked(flag_value(argc, argv, "--game"));
    if (!b) b = named_game(argc > 0 ? argv[0] : NULL);
    return b;
}

const openpete_game_build_t* openpete_game_build_select(int argc, char** argv) {
    const openpete_game_build_t* b = openpete_game_build_named(argc, argv);
    if (b || openpete_game_build_count == 0) return b;
    return openpete_game_builds[0];
}

const openpete_game_build_t* openpete_game_build_linked(const char* id) {
    return linked(id);
}

void openpete_game_build_install(const openpete_game_build_t* b) {
    s_build = b;
    game_runtime_install(b->runtime);
}

const openpete_game_build_t* openpete_game_build(void) { return s_build; }

void game_state_regions_register(void) {
    if (s_build && s_build->state_regions_register) s_build->state_regions_register();
}

void game_disc_boot_order_register(void) {
    if (s_build && s_build->disc_boot_order_register) s_build->disc_boot_order_register();
}

void game_save_icon_register(void) {
    if (s_build && s_build->save_icon_register) s_build->save_icon_register();
}
