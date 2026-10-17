/* core/config/identify.c - the game a session runs, and the one full
 * configuration parse after it (contract in openpete_identify.h). */

#include "openpete_identify.h"

#include "openpete.h"
#include "openpete_boot_timing.h"
#include "openpete_config.h"
#include "openpete_disc_trace.h"
#include "openpete_error_ids.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "openpete_time.h"

#include <stdio.h>
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

const char* openpete_ident_source_name(openpete_ident_source_t s) {
    switch (s) {
    case OPENPETE_IDENT_DISC:       return "disc";
    case OPENPETE_IDENT_LIBRARY:    return "library";
    case OPENPETE_IDENT_SELECTOR:   return "selector";
    case OPENPETE_IDENT_EXECUTABLE: return "executable";
    default:                        return "none";
    }
}

/** @brief The game the disc names: the library entry's recorded game, else its serial's game.
 *
 * Reads the serial into out->serial when the disc is readable.
 * @param in The sources.
 * @param out Receives the serial.
 * @return The game id, or NULL when the disc names none. */
static const char* disc_game(const openpete_ident_in_t* in, openpete_ident_t* out) {
    out->serial[0] = '\0';
    if (!in->disc_path || !*in->disc_path) return NULL;
    (void)openpete_psx_read_disc_serial(in->disc_path, out->serial, sizeof out->serial);
    if (in->recorded_game && openpete_game_registered(in->recorded_game))
        return in->recorded_game;
    return openpete_game_for_serial(out->serial);
}

int openpete_identify(const openpete_ident_in_t* in, openpete_ident_t* out) {
    memset(out, 0, sizeof *out);
    out->disc_path = (in->disc_path && *in->disc_path) ? in->disc_path : NULL;

    if (in->selector && !openpete_game_registered(in->selector)) {
        fprintf(stderr, "openpete: --game %s: no such game (spyro-1, spyro-2, spyro-3)\n",
                in->selector);
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    const char* from_disc = disc_game(in, out);
    if (from_disc && in->selector && strcmp(from_disc, in->selector) != 0) {
        fprintf(stderr, "openpete: --game %s, but the disc %s is %s (%s) %s\n",
                in->selector, out->disc_path, from_disc,
                out->serial[0] ? out->serial : "recorded in the library",
                openpete_error_ref(OP_E_WRONG_DISC));
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    if (from_disc) {
        out->game = from_disc;
        out->source = in->from_library ? OPENPETE_IDENT_LIBRARY : OPENPETE_IDENT_DISC;
    } else if (in->selector) {
        out->game = in->selector;
        out->source = OPENPETE_IDENT_SELECTOR;
    } else if (in->linked_game) {
        out->game = in->linked_game;
        out->source = OPENPETE_IDENT_EXECUTABLE;
    }
    return OPENPETE_EXIT_OK;
}

int openpete_identify_require_linked(const openpete_ident_t* id, const char* linked_game) {
    if (!id->game || !linked_game || !strcmp(id->game, linked_game))
        return OPENPETE_EXIT_OK;
    fprintf(stderr, "openpete: the %s names %s, but this executable runs %s %s\n",
            openpete_ident_source_name(id->source), id->game, linked_game,
            openpete_error_ref(OP_E_WRONG_DISC));
    return OPENPETE_EXIT_SETUP_ERROR;
}

int openpete_options_parse_identified(struct openpete_options* opts, int argc,
                                      char** argv, const openpete_ident_t* id,
                                      const char* display_name) {
    openpete_config_set_game_id(id->game);
    openpete_config_set_identified_disc(id->disc_path);
    openpete_config_set_identity_source(openpete_ident_source_name(id->source));
    openpete_boot_mark(OP_BOOT_IDENTIFIED);
    OPL_INFO_F("boot", "identified %s by %s (disc %s, serial %s)",
               id->game ? id->game : "no game", openpete_ident_source_name(id->source),
               id->disc_path ? id->disc_path : "-", id->serial[0] ? id->serial : "-");

    openpete_options_init(opts);
    if (display_name) opts->display_name = display_name;
    const int rc = openpete_options_parse(opts, argc, argv);
    openpete_boot_mark(OP_BOOT_MATERIALIZED);
    if (rc != OPENPETE_EXIT_OK) return rc;
    /* The clock latched its env knobs at its first call, and the disc
     * trace at the serial read above, before any tier was materialized. */
    op_time_reload_env();
    openpete_disc_trace_reload();
    if (id->source == OPENPETE_IDENT_LIBRARY) opts->ingest_path = NULL;
    if (!opts->disc_path && !opts->exe_path) opts->disc_path = id->disc_path;
    return OPENPETE_EXIT_OK;
}

int openpete_disc_identity_serial(const char* disc_path, char* serial,
                                  size_t serial_cap, char* sha, size_t sha_cap) {
    if (serial && serial_cap) serial[0] = '\0';
    if (sha && sha_cap) sha[0] = '\0';
    if (!disc_path || !*disc_path || !serial || serial_cap < 12) return -1;
    return openpete_psx_read_disc_serial(disc_path, serial, serial_cap) == 0 ? 0 : -1;
}
