/* tests/unit/identify_test.c - game identification (core/config/identify.c).
 *
 * The serial table against every game's recomp.conf EXE_FILENAME, and the
 * source order and refusals of openpete_identify: a disc beats --game and
 * the executable's game, a library entry's recorded game beats its serial,
 * a disc and a selector naming different games refuse, an unregistered
 * selector refuses, and a game the executable does not link refuses.
 * The disc read is faked: an image path names its serial ("x/SCUS-94425.cue"
 * reads as SCUS-94425; "unreadable" fails). identify.c's calls into the
 * option parser, the configuration layer, the boot timing line, the clock
 * and the disc trace are stubbed; config_resolve_test covers the
 * configuration side. */

#include "openpete_identify.h"
#include "openpete.h"
#include "openpete_boot_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stubs ---------------------------------------------------------- */

int openpete_psx_read_disc_serial(const char* image_path, char* out, size_t cap) {
    const char* slash = strrchr(image_path, '/');
    const char* base = slash ? slash + 1 : image_path;
    if (!strncmp(base, "unreadable", 10)) { out[0] = '\0'; return -1; }
    size_t n = strcspn(base, ".");
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    return 0;
}
void openpete_boot_mark(op_boot_mark mark) { (void)mark; }
void openpete_config_set_game_id(const char* id) { (void)id; }
void openpete_config_set_identified_disc(const char* p) { (void)p; }
void openpete_config_set_identity_source(const char* s) { (void)s; }
void openpete_options_init(openpete_options_t* o) { memset(o, 0, sizeof *o); }
int  openpete_options_parse(openpete_options_t* o, int c, char** v) { (void)o; (void)c; (void)v; return 0; }
void op_time_reload_env(void) {}
void openpete_disc_trace_reload(void) {}

/* ---- harness -------------------------------------------------------- */

static int g_fail;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL ");                                      \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "  (%s:%d)\n", __FILE__, __LINE__);            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/** @brief Identify from the given sources and check the result. */
static void expect(const char* name, const char* selector, const char* disc,
                   int lib, const char* recorded, const char* linked,
                   int want_rc, const char* want_game, openpete_ident_source_t want_src) {
    openpete_ident_in_t in = { selector, disc, lib, recorded, linked };
    openpete_ident_t id;
    int rc = openpete_identify(&in, &id);
    CHECK(rc == want_rc, "[%s] rc %d, want %d", name, rc, want_rc);
    if (rc != 0 || want_rc != 0) return;
    CHECK((id.game && want_game && !strcmp(id.game, want_game)) || (!id.game && !want_game),
          "[%s] game %s, want %s", name, id.game ? id.game : "-", want_game ? want_game : "-");
    CHECK(id.source == want_src, "[%s] source %s, want %s", name,
          openpete_ident_source_name(id.source), openpete_ident_source_name(want_src));
}

/** @brief Check the serial table against games/<id>/recomp.conf's EXE_FILENAME, in the serial reader's spelling. */
static void serials_match_recomp_conf(void) {
    static const char* const games[] = { "spyro-1", "spyro-2", "spyro-3" };
    for (int g = 0; g < 3; g++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/games/%s/recomp.conf", OPENPETE_SOURCE_DIR, games[g]);
        FILE* f = fopen(path, "r");
        CHECK(f != NULL, "cannot read %s", path);
        if (!f) continue;
        char line[512], serial[32] = "";
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "EXE_FILENAME=\"", 14) != 0) continue;
            /* SCUS_942.28 -> SCUS-94228 */
            size_t o = 0;
            for (const char* p = line + 14; *p && *p != '"' && o + 1 < sizeof serial; p++) {
                if (*p == '_') serial[o++] = '-';
                else if (*p != '.') serial[o++] = *p;
            }
            serial[o] = '\0';
        }
        fclose(f);
        const char* id = openpete_game_for_serial(serial);
        CHECK(id && !strcmp(id, games[g]), "serial %s of %s maps to %s", serial,
              games[g], id ? id : "nothing");
        CHECK(openpete_game_registered(games[g]), "%s not registered", games[g]);
    }
}

int main(void) {
    serials_match_recomp_conf();
    CHECK(!openpete_game_for_serial("SLUS-00000"), "an unknown serial names no game");

    expect("disc names the game", NULL, "d/SCUS-94425.cue", 0, NULL, "spyro-1",
           0, "spyro-2", OPENPETE_IDENT_DISC);
    expect("selector without a disc", "spyro-3", NULL, 0, NULL, "spyro-1",
           0, "spyro-3", OPENPETE_IDENT_SELECTOR);
    expect("selector agreeing with the disc", "spyro-1", "d/SCUS-94228.cue", 0, NULL, "spyro-1",
           0, "spyro-1", OPENPETE_IDENT_DISC);
    expect("selector contradicting the disc", "spyro-1", "d/SCUS-94467.cue", 0, NULL, "spyro-1",
           OPENPETE_EXIT_SETUP_ERROR, NULL, OPENPETE_IDENT_NONE);
    expect("unregistered selector", "spyro-9", NULL, 0, NULL, "spyro-1",
           OPENPETE_EXIT_SETUP_ERROR, NULL, OPENPETE_IDENT_NONE);
    expect("library entry's recorded game", NULL, "lib/SCUS-94228.cue", 1, "spyro-2", "spyro-1",
           0, "spyro-2", OPENPETE_IDENT_LIBRARY);
    expect("unidentified library entry by serial", NULL, "lib/SCUS-94228.cue", 1, NULL, "spyro-1",
           0, "spyro-1", OPENPETE_IDENT_LIBRARY);
    expect("unreadable disc falls to the executable", NULL, "d/unreadable.cue", 0, NULL, "spyro-1",
           0, "spyro-1", OPENPETE_IDENT_EXECUTABLE);
    expect("unknown serial falls to the selector", "spyro-2", "d/SLUS-00000.cue", 0, NULL, "spyro-1",
           0, "spyro-2", OPENPETE_IDENT_SELECTOR);
    expect("nothing names a game", NULL, NULL, 0, NULL, NULL, 0, NULL, OPENPETE_IDENT_NONE);

    openpete_ident_t id = { "spyro-2", OPENPETE_IDENT_DISC, "SCUS-94425", "d" };
    CHECK(openpete_identify_require_linked(&id, "spyro-1") == OPENPETE_EXIT_SETUP_ERROR,
          "a game the executable does not link refuses");
    CHECK(openpete_identify_require_linked(&id, "spyro-2") == 0, "the linked game passes");

    if (g_fail) { fprintf(stderr, "identify_test: %d failure(s)\n", g_fail); return 1; }
    printf("identify_test: all cases passed (3 serials, 10 identifications, 2 link checks)\n");
    return 0;
}
