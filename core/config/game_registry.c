/* core/config/game_registry.c - the registered games and their discs' serials
 * (contract in openpete_identify.h). */

#include "openpete_identify.h"

#include <stddef.h>
#include <string.h>

/* Every registered game and the serial its SYSTEM.CNF BOOT line names (the
 * EXE_FILENAME of games/<id>/recomp.conf in openpete_psx_read_disc_serial's
 * spelling; tests/unit/identify_test.c checks the two agree). A disc with
 * another serial identifies no game. */
static const struct { const char* id; const char* serial; } k_games[] = {
    { "spyro-1", "SCUS-94228" },
    { "spyro-2", "SCUS-94425" },
    { "spyro-3", "SCUS-94467" },
};
#define GAME_COUNT (sizeof k_games / sizeof k_games[0])

const char* openpete_game_for_serial(const char* serial) {
    if (!serial || !*serial) return NULL;
    for (size_t i = 0; i < GAME_COUNT; i++)
        if (!strcmp(k_games[i].serial, serial)) return k_games[i].id;
    return NULL;
}

int openpete_game_registered(const char* id) {
    if (!id) return 0;
    for (size_t i = 0; i < GAME_COUNT; i++)
        if (!strcmp(k_games[i].id, id)) return 1;
    return 0;
}

const char* openpete_game_id_at(int i) {
    return i >= 0 && (size_t)i < GAME_COUNT ? k_games[i].id : NULL;
}

const char* openpete_serial_of_game(const char* id) {
    if (!id) return NULL;
    for (size_t i = 0; i < GAME_COUNT; i++)
        if (!strcmp(k_games[i].id, id)) return k_games[i].serial;
    return NULL;
}
