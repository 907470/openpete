// games/common/game_build.c - the running game's build record, its selection
// among the linked games, and the entries that forward to it. See
// game_build.h.

#include "game_build.h"

#include "game_host_desc.h"
#include "openpete_identify.h"
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "renderer/orchestrator.h"
#include "renderer/text_moby.h"

#include <stdlib.h>

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

const openpete_game_build_t* openpete_game_build_named(int argc, char** argv) {
    if (openpete_game_build_count == 0) return NULL;
    return disc_game(flag_value(argc, argv, "--disc"));
}

const openpete_game_build_t* openpete_game_build_select(int argc, char** argv) {
    const openpete_game_build_t* b = openpete_game_build_named(argc, argv);
    if (b || openpete_game_build_count == 0) return b;
    return openpete_game_builds[0];
}

const openpete_game_build_t* openpete_game_build_linked(const char* id) {
    return linked(id);
}

static int s_install_refusals;

void openpete_game_build_install(const openpete_game_build_t* b) {
    if (openpete_launcher_jobs_in_flight() > 0) {
        s_install_refusals++;
        OPL_ERROR_F("game", "install of %s refused: a launcher job is in flight (%d refused)",
                    b ? b->id : "(none)", s_install_refusals);
#ifndef NDEBUG
        abort();
#endif
        return;
    }
    s_build = b;
    game_runtime_install(b->runtime);
}

int openpete_game_build_install_refusals(void) { return s_install_refusals; }

void openpete_game_report_resolution(const openpete_options_t* opts) {
    const char* path = getenv("OPENPETE_CONFIG_REPORT_OUT");
    if (!path || !*path) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    openpete_config_report(f);
    fprintf(f, "disc_path %s\n", opts->disc_path ? opts->disc_path : "-");
    fclose(f);
}

void openpete_game_install_dump(FILE* out) {
    fprintf(out, "build %p %s\n", (const void*)s_build, s_build ? s_build->id : "-");
    fprintf(out, "install_refusals %d\n", s_install_refusals);
    game_host_tables_dump(out);
    game_orchestrator_dump(out);
    openpete_game_runtime_dump(out);
    text_moby_native_dump(out);
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
