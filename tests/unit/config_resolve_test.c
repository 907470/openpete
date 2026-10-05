/* tests/unit/config_resolve_test.c - openpete.toml precedence.
 *
 * The resolver's whole job is deciding which of several layers wins and
 * then materializing exactly ONE spelling of it. Prose can't hold that
 * ordering steady across edits; this can. Each case is a synthetic layer
 * stack plus the argv/env it must produce.
 *
 * The assertions are on the MATERIALIZED result - the spliced argv and the
 * process environment - not on the report, because that is what the rest
 * of the program actually consumes.
 */

#include "openpete_config.h"
#include "openpete_keymap.h"
#include "openpete_paths.h"
#include "../../core/config/config_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #define rmdir_quiet(p) ((void)_rmdir(p))
#else
  #include <unistd.h>
  #define rmdir_quiet(p) ((void)rmdir(p))
#endif


static int g_fail;
static const char *g_case = "";

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL [%s] ", g_case);                          \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n         at %s:%d\n", __FILE__, __LINE__);   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* ---- harness ------------------------------------------------------- */

static char g_toml_path[512];

/* Read the config file back. NUL-terminates at the byte count read: a
 * plain fread into a reused buffer leaves the tail of a LONGER previous
 * read in place, which reads back as content the file no longer has. */
static void read_toml(char *out, size_t cap) {
    memset(out, 0, cap);
    FILE *f = fopen(g_toml_path, "rb");
    if (!f) return;
    size_t got = fread(out, 1, cap - 1, f);
    out[got] = '\0';
    fclose(f);
}

static void write_toml(const char *body) {
    FILE *f = fopen(g_toml_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", g_toml_path); exit(1); }
    fputs(body, f);
    fclose(f);
}

/* The resolved argv of one run, for the assertions below. */
typedef struct {
    int    argc;
    char **argv;
    int    rc;
} run_t;

static int fake_identity(const char *disc_path, char *serial, size_t sc,
                         char *sha, size_t shc) {
    (void)disc_path;
    snprintf(serial, sc, "SCUS-94228");
    snprintf(sha, shc, "95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f");
    return 0;
}

/* Run the resolver over `body` with `typed` as the typed command line.
 * `with_disc` registers the fake identity hook so the [disc.*] scopes
 * apply without needing a real image. */
static run_t run(const char *name, const char *body,
                 const char *const *typed, int with_disc) {
    g_case = name;
    openpete_config_reset();
    write_toml(body);

    static char *argv[32];
    int argc = 0;
    argv[argc++] = (char *)"openpete";
    argv[argc++] = (char *)"--config";
    argv[argc++] = g_toml_path;
    for (int i = 0; typed && typed[i]; i++) argv[argc++] = (char *)typed[i];

    openpete_config_set_game_id("spyro-1");
    if (with_disc) openpete_config_set_disc_identity_fn(fake_identity);

    run_t r = { argc, argv, 0 };
    int oc = 0;
    char **ov = NULL;
    r.rc = openpete_config_apply(argc, argv, argc, &oc, &ov);
    if (r.rc == 1) { r.argc = oc; r.argv = ov; }
    return r;
}

/* Index of `flag` in the resolved argv, or -1. */
static int arg_index(const run_t *r, const char *flag) {
    for (int i = 1; i < r->argc; i++)
        if (!strcmp(r->argv[i], flag)) return i;
    return -1;
}

static void expect_flag_value(const run_t *r, const char *flag, const char *val) {
    int i = arg_index(r, flag);
    CHECK(i >= 0, "expected %s in the resolved argv", flag);
    if (i < 0) return;
    CHECK(i + 1 < r->argc && !strcmp(r->argv[i + 1], val),
          "%s should be %s, got %s", flag, val,
          (i + 1 < r->argc) ? r->argv[i + 1] : "(nothing)");
}

static void expect_no_flag(const run_t *r, const char *flag) {
    CHECK(arg_index(r, flag) < 0,
          "%s should NOT have been emitted (a higher layer won)", flag);
}

static void expect_env(const char *var, const char *val) {
    const char *got = getenv(var);
    CHECK(got && !strcmp(got, val), "%s should be %s, got %s", var, val,
          got ? got : "(unset)");
}

static void expect_env_unset(const char *var) {
    const char *got = getenv(var);
    CHECK(!got, "%s should be unset, got %s", var, got ? got : "");
}

static void clear_env(const char *var) {
#ifdef _WIN32
    _putenv_s(var, "");
#else
    unsetenv(var);
#endif
}

/* ---- cases --------------------------------------------------------- */

/* Inside one scope, notation ranks as it does in the real world: an
 * [args] entry beats an [env] entry beats a bare key. Moving a line
 * between the file and the shell must not change who wins. */
static void case_spelling_mirror(void) {
    run_t r = run("spelling mirror",
        "[game.spyro-1]\n"
        "ssaa = 1\n"
        "[game.spyro-1.args]\n"
        "\"--ssaa\" = 4\n", NULL, 0);
    expect_flag_value(&r, "--ssaa", "4");

    r = run("spelling mirror: env over base",
        "[game.spyro-1]\n"
        "mods = \"a\"\n"
        "[game.spyro-1.env]\n"
        "OPENPETE_MODS = \"b\"\n", NULL, 0);
    expect_env("OPENPETE_MODS", "b");
    expect_no_flag(&r, "--mods");
    clear_env("OPENPETE_MODS");

    r = run("spelling mirror: args over env",
        "[game.spyro-1.env]\n"
        "OPENPETE_MODS = \"b\"\n"
        "[game.spyro-1.args]\n"
        "\"--mods\" = \"c\"\n", NULL, 0);
    expect_flag_value(&r, "--mods", "c");
    expect_env_unset("OPENPETE_MODS");
    clear_env("OPENPETE_MODS");
}

/* The rule that makes the hierarchy meaningful: a more specific SCOPE
 * beats a less specific one however either is spelled. Without this, a
 * user who writes global defaults in [args] notation would find them
 * silently beating per-disc tuning. */
static void case_specificity_beats_notation(void) {
    run_t r = run("scope beats notation",
        "[game.spyro-1.args]\n"
        "\"--ssaa\" = 2\n"
        "[disc.SCUS-94228]\n"
        "ssaa = 4\n", NULL, 1);
    expect_flag_value(&r, "--ssaa", "4");

    r = run("scope beats notation (sha over serial)",
        "[disc.SCUS-94228.args]\n"
        "\"--ssaa\" = 2\n"
        "[disc.95f03abf]\n"
        "ssaa = 8\n", NULL, 1);
    expect_flag_value(&r, "--ssaa", "8");
}

/* THE case the whole design exists for. Splicing [args] into argv and
 * setenv-ing [env] would hand this decision to --vsync's own
 * sentinel-unset consumer, which takes the flag whenever one is present -
 * so the LOW-specificity [args] would beat the HIGH-specificity [env].
 * Flattening must emit the env spelling and no flag at all. */
static void case_cross_spelling_inversion(void) {
    clear_env("OPENPETE_PRESENT_MODE");
    run_t r = run("cross-spelling inversion",
        "[game.spyro-1.args]\n"
        "\"--vsync\" = 1\n"
        "[disc.SCUS-94228.env]\n"
        "OPENPETE_PRESENT_MODE = \"immediate\"\n", NULL, 1);
    expect_env("OPENPETE_PRESENT_MODE", "immediate");
    expect_no_flag(&r, "--vsync");
    clear_env("OPENPETE_PRESENT_MODE");
}

/* A launch tier supplies the knob, so the file's copy must not be
 * emitted at all - otherwise the flag parser sees both and argv order,
 * not the hierarchy, decides. */
static void case_launch_tier_suppresses(void) {
    static const char *const typed[] = { "--lod", "extended", NULL };
    run_t r = run("typed CLI suppresses the file",
        "[game.spyro-1]\n"
        "lod = \"original\"\n", typed, 0);
    CHECK(arg_index(&r, "--lod") >= 0, "the typed --lod must survive");
    /* Exactly one --lod: the config's must not have been added. */
    int n = 0;
    for (int i = 1; i < r.argc; i++) if (!strcmp(r.argv[i], "--lod")) n++;
    CHECK(n == 1, "expected exactly one --lod in argv, found %d", n);
    CHECK(openpete_config_knob_pinned("lod"), "lod should read as pinned");
    const char *pin = openpete_config_knob_pin_spelling("lod");
    CHECK(pin && !strcmp(pin, "--lod"), "pin spelling should be --lod, got %s",
          pin ? pin : "(null)");
}

/* The real environment outranks every file tier, including [args]. */
static void case_real_env_beats_file(void) {
#ifdef _WIN32
    _putenv_s("OPENPETE_MODS", "from-shell");
#else
    setenv("OPENPETE_MODS", "from-shell", 1);
#endif
    run_t r = run("real env beats the file",
        "[game.spyro-1.args]\n"
        "\"--mods\" = \"from-file\"\n", NULL, 0);
    expect_no_flag(&r, "--mods");
    expect_env("OPENPETE_MODS", "from-shell");
    CHECK(openpete_config_knob_pinned("mods"), "mods should read as pinned");
    clear_env("OPENPETE_MODS");
}

/* [saved] is a tier above every hand-written scope (you clicked it, it
 * sticks) but still below anything typed this launch. */
static void case_saved_tier(void) {
    run_t r = run("saved beats hand-written scopes",
        "[disc.SCUS-94228.args]\n"
        "\"--ssaa\" = 2\n"
        "[saved]\n"
        "ssaa = 8\n", NULL, 1);
    expect_flag_value(&r, "--ssaa", "8");

    static const char *const typed[] = { "--ssaa", "16", NULL };
    r = run("but the command line beats saved",
        "[saved]\n"
        "ssaa = 8\n", typed, 0);
    int n = 0;
    for (int i = 1; i < r.argc; i++) if (!strcmp(r.argv[i], "--ssaa")) n++;
    CHECK(n == 1, "expected only the typed --ssaa, found %d", n);
}

/* A boolean false in [args] is how a higher tier un-sets a valueless
 * flag a lower tier asked for. */
static void case_false_suppresses(void) {
    run_t r = run("false suppresses an inherited flag",
        "[args]\n"
        "\"--screenshot-freeze-backdrop\" = true\n"
        "[game.spyro-1.args]\n"
        "\"--screenshot-freeze-backdrop\" = false\n", NULL, 0);
    expect_no_flag(&r, "--screenshot-freeze-backdrop");

    r = run("...and true emits it",
        "[args]\n"
        "\"--screenshot-freeze-backdrop\" = true\n", NULL, 0);
    CHECK(arg_index(&r, "--screenshot-freeze-backdrop") >= 0,
          "--screenshot-freeze-backdrop should be emitted");
}

/* --no-lod is --lod extended. If the resolver treated them as unrelated
 * flags, both would be emitted and argv order would decide. */
static void case_alias_collapses(void) {
    run_t r = run("alias collapses onto its knob",
        "[args]\n"
        "\"--no-lod\" = true\n"
        "[game.spyro-1]\n"
        "lod = \"original\"\n", NULL, 0);
    expect_flag_value(&r, "--lod", "original");
    expect_no_flag(&r, "--no-lod");

    r = run("alias wins when it is the higher tier",
        "[game.spyro-1]\n"
        "lod = \"original\"\n"
        "[disc.SCUS-94228.args]\n"
        "\"--no-lod\" = true\n", NULL, 1);
    expect_flag_value(&r, "--lod", "extended");
}

/* A key that was renamed keeps working, value and all.
 *
 * openpete writes these files itself (SAVE in the M overlay), so the previous
 * release's saved settings are exactly what a rename breaks: without the
 * mapping the old spelling is an "unknown key" and the boot fails outright.
 * The value has to travel too - `show_fps` was a bool and `perf_stats` is a
 * three-level enum, so true means the level the old key could select. */
static void case_renamed_key(void) {
    run_t r = run("renamed key: true maps to the level it meant",
        "[game.spyro-1]\n"
        "show_fps = true\n", NULL, 0);
    expect_flag_value(&r, "--perf-stats", "fps");
    expect_no_flag(&r, "--show-fps");

    r = run("renamed key: false maps to off",
        "[game.spyro-1]\n"
        "show_fps = false\n", NULL, 0);
    expect_flag_value(&r, "--perf-stats", "off");

    /* And it ranks as the key it became: the new spelling in a more specific
     * scope beats the old one in a less specific scope, rather than the two
     * being treated as unrelated knobs and both surviving. */
    r = run("renamed key: outranked by the new spelling",
        "[game.spyro-1]\n"
        "show_fps = true\n"
        "[disc.SCUS-94228]\n"
        "perf_stats = \"detailed\"\n", NULL, 1);
    expect_flag_value(&r, "--perf-stats", "detailed");
    /* exactly one --perf-stats survived */
    int n = 0;
    for (int i = 1; i < r.argc; i++)
        if (!strcmp(r.argv[i], "--perf-stats")) n++;
    CHECK(n == 1, "the old and new spellings should collapse to one flag, got %d", n);
}

/* An env-only knob spelled as a bare key goes out as its variable - and
 * a false one leaves the variable ABSENT, because several of these
 * consumers only test for presence. */
static void case_env_only_knob(void) {
    clear_env("OPENPETE_NOTEX");
    run_t r = run("bare key for an env-only knob",
        "notex = true\n", NULL, 0);
    (void)r;
    expect_env("OPENPETE_NOTEX", "1");
    clear_env("OPENPETE_NOTEX");

    r = run("false leaves it unset, not \"0\"",
        "notex = false\n", NULL, 0);
    expect_env_unset("OPENPETE_NOTEX");
}

/* Typos in keys are hard errors: a config line that silently does nothing
 * is the failure mode that costs an hour. */
static void case_validation(void) {
    run_t r = run("unknown key", "lodd = \"extended\"\n", NULL, 0);
    CHECK(r.rc < 0, "an unknown key must fail the run");

    /* A value of the wrong type is a stored setting gone stale, not a typo
     * in a key: a boot warns and keeps the default; --config-report, which
     * is how a file is checked, refuses it. */
    r = run("bad enum value", "lod = \"turbo\"\nvsync = \"maybe\"\n", NULL, 0);
    CHECK(r.rc >= 0, "a value outside the vocabulary must not fail a boot");
    expect_no_flag(&r, "--lod");
    expect_no_flag(&r, "--vsync");
    static const char *const report[] = { "--config-report", NULL };
    r = run("bad enum value, report", "lod = \"turbo\"\n", report, 0);
    CHECK(r.rc < 0, "--config-report must refuse a value outside the vocabulary");
    r = run("bad [args] enum value", "[args]\n\"--lod\" = \"turbo\"\n", NULL, 0);
    CHECK(r.rc >= 0, "an [args] value outside the vocabulary must not fail a boot");
    expect_no_flag(&r, "--lod");

    r = run("non-flag key in [args]",
        "[args]\nlod = \"extended\"\n", NULL, 0);
    CHECK(r.rc < 0, "an [args] key that isn't a flag must fail the run");

    r = run("ambiguous sha prefix",
        "[disc.95f03abf]\nssaa = 2\n"
        "[disc.95f03abf97]\nssaa = 4\n", NULL, 1);
    CHECK(r.rc < 0, "two matching sha prefixes must fail the run");
}

/* A user file overrides the shipped file at the SAME scope, but never
 * reaches above it: scope is still the major key. Exercised through the
 * rank arithmetic rather than two real files, which --config cannot
 * express - see the file-order assertion in the report test below. */
static void case_scope_beats_file(void) {
    run_t r = run("scope still outranks the file",
        "[game.spyro-1]\n"
        "ssaa = 2\n"
        "[disc.SCUS-94228]\n"
        "ssaa = 4\n", NULL, 1);
    expect_flag_value(&r, "--ssaa", "4");
}

/* Unregistered flags (no schema row) still layer, and still collapse to
 * one copy - the resolver dedupes rather than trusting argv order,
 * because some flags don't parse last-write-wins. */
static void case_unregistered_flags(void) {
    run_t r = run("unregistered flags dedupe by tier",
        "[args]\n"
        "\"--frame-limit\" = 60\n"
        "[game.spyro-1.args]\n"
        "\"--frame-limit\" = 120\n", NULL, 0);
    expect_flag_value(&r, "--frame-limit", "120");
    int n = 0;
    for (int i = 1; i < r.argc; i++) if (!strcmp(r.argv[i], "--frame-limit")) n++;
    CHECK(n == 1, "expected one --frame-limit, found %d", n);
}

/* Every knob-registry row with a flag needs a schema row, or it silently
 * loses its bare key (and, if it also has an env twin, its flattening).
 * The registry itself can't be linked here - its rows point into the
 * renderer - so knob_registry_init runs the reverse half of this check at
 * boot. What we can prove here is that the schema is self-consistent. */
static void case_schema_sanity(void) {
    g_case = "schema sanity";
    size_t n;
    const cfg_knob_t *s = cfg_schema(&n);
    CHECK(n > 20, "the schema looks truncated (%zu rows)", n);
    for (size_t i = 0; i < n; i++) {
        CHECK(s[i].id && s[i].id[0], "row %zu has no id", i);
        CHECK(s[i].cli || s[i].env, "%s has neither a flag nor a variable", s[i].id);
        if (s[i].type == CFG_ENUM)
            CHECK(s[i].vocab && s[i].vocab[0], "%s is an enum with no vocabulary",
                  s[i].id);
        if (s[i].alias)
            CHECK(s[i].alias_value, "%s's alias has no value", s[i].id);
        /* The bare key must be the flag with its dashes swapped, or the
         * file spelling and the CLI spelling drift apart. */
        if (s[i].cli) {
            char derived[96];
            snprintf(derived, sizeof derived, "%s", s[i].cli + 2);
            for (char *p = derived; *p; p++) if (*p == '-') *p = '_';
            CHECK(!strcmp(derived, s[i].id),
                  "%s should be named '%s' to match its flag", s[i].id, derived);
        }
        for (size_t j = i + 1; j < n; j++)
            CHECK(strcmp(s[i].id, s[j].id), "duplicate schema id %s", s[i].id);
    }
}

/* The writer's contract: everything above the managed marker survives byte
 * for byte (comments, ordering, tables the reader doesn't know), and the
 * zone below round-trips back through the reader as the [saved] tier. */
static void case_writer_round_trip(void) {
    static const char *hand =
        "# my notes, which must survive\n"
        "ssaa = 2\n"
        "\n"
        "[game.spyro-1]\n"
        "lod = \"original\"   # trailing comment\n";

    run_t r = run("writer: preserve the hand-written zone", hand, NULL, 0);
    CHECK(r.rc >= 0, "the fixture should load cleanly");

    CHECK(openpete_config_save_knob("ssaa", "8") == 0, "save_knob(ssaa) failed");
    CHECK(openpete_config_save_knob("lod", "extended") == 0, "save_knob(lod) failed");
    CHECK(openpete_config_save_knob("fullscreen", "1") == 0, "save_knob(fullscreen) failed");
    CHECK(openpete_config_save_knob("nonsense", "1") != 0,
          "save_knob should refuse a knob with no schema row");
    CHECK(openpete_config_save_flush() == 0, "save_flush failed");

    char body[4096];
    read_toml(body, sizeof body);
    CHECK(body[0] != '\0', "the saved file should exist and be non-empty");

    CHECK(strstr(body, "# my notes, which must survive") != NULL,
          "the hand-written comment was lost");
    CHECK(strstr(body, "# trailing comment") != NULL,
          "a trailing comment was lost");
    CHECK(strstr(body, "[saved]") != NULL, "no managed [saved] block written");
    /* Typed as the knob declares, so the reader takes it back unchanged. */
    CHECK(strstr(body, "fullscreen = true") != NULL,
          "a bool should round-trip as true, not \"1\"");
    CHECK(strstr(body, "ssaa = 8") != NULL, "an int should round-trip bare");
    CHECK(strstr(body, "lod = \"extended\"") != NULL, "an enum should be quoted");

    /* Read the written file back: [saved] must now outrank both scopes. */
    g_case = "writer: saved tier reads back";
    openpete_config_reset();
    static char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)"openpete";
    argv[argc++] = (char *)"--config";
    argv[argc++] = g_toml_path;
    openpete_config_set_game_id("spyro-1");
    int oc = 0; char **ov = NULL;
    run_t r2 = { argc, argv, 0 };
    r2.rc = openpete_config_apply(argc, argv, argc, &oc, &ov);
    if (r2.rc == 1) { r2.argc = oc; r2.argv = ov; }
    CHECK(r2.rc >= 0, "the written file should re-read cleanly");
    expect_flag_value(&r2, "--ssaa", "8");
    expect_flag_value(&r2, "--lod", "extended");

    /* Saving twice must not stack managed blocks. */
    g_case = "writer: idempotent";
    CHECK(openpete_config_save_flush() == 0, "second flush failed");
    read_toml(body, sizeof body);
    const char *first = strstr(body, "[saved]");
    CHECK(first && !strstr(first + 1, "[saved]"),
          "a second save appended another [saved] block");

    g_case = "writer: reset clears the block";
    CHECK(openpete_config_reset_saved() == 0, "reset_saved failed");
    read_toml(body, sizeof body);
    CHECK(strstr(body, "[saved]") == NULL, "reset left a [saved] block");
    CHECK(strstr(body, "# my notes, which must survive") != NULL,
          "reset ate the hand-written zone");
}

/** @brief Check that openpete_config_token_source matches spliced tokens by pointer only.
 *
 * The option parser keeps the default for a refused config value and fails
 * a refused typed one, so a typed token whose text equals a spliced one
 * (fog_scale = "abc" in the file, --fov abc typed) must not read as
 * spliced. */
static void case_token_source_by_pointer(void) {
    static const char *typed[] = { "--fov", "abc", NULL };
    run_t r = run("token source: pointer identity", "fog_scale = \"abc\"\n", typed, 0);
    CHECK(r.rc == 1, "the fixture should splice tokens");
    int cfg = arg_index(&r, "--fog-scale");
    int cli = arg_index(&r, "--fov");
    CHECK(cfg >= 0 && cfg + 1 < r.argc, "the spliced --fog-scale is missing");
    CHECK(cli >= 0 && cli + 1 < r.argc, "the typed --fov is missing");
    if (cfg < 0 || cfg + 1 >= r.argc || cli < 0 || cli + 1 >= r.argc) return;
    CHECK(openpete_config_token_source(r.argv[cfg + 1]) != NULL,
          "the spliced value should carry its origin label");
    CHECK(!strcmp(r.argv[cli + 1], r.argv[cfg + 1]), "the fixture needs equal texts");
    CHECK(openpete_config_token_source(r.argv[cli + 1]) == NULL,
          "a typed token equal in text to a spliced one read as spliced");
}

/** @brief Check that a saved fov of 0 is written bare and spliced back as --fov 0.
 *
 * 0 is the game's own projection; the option parser takes it unclamped. */
static void case_fov_zero_round_trip(void) {
    run_t r = run("fov 0: save", "fov = 70\n", NULL, 0);
    CHECK(r.rc >= 0, "the fixture should load cleanly");
    CHECK(openpete_config_save_knob("fov", "0") == 0, "save_knob(fov) failed");
    CHECK(openpete_config_save_flush() == 0, "save_flush failed");
    char body[4096];
    read_toml(body, sizeof body);
    CHECK(strstr(body, "fov = 0\n") != NULL, "fov 0 should be written bare");

    g_case = "fov 0: read back";
    openpete_config_reset();
    static char *argv[4];
    argv[0] = (char *)"openpete";
    argv[1] = (char *)"--config";
    argv[2] = g_toml_path;
    openpete_config_set_game_id("spyro-1");
    run_t r2 = { 3, argv, 0 };
    int oc = 0; char **ov = NULL;
    r2.rc = openpete_config_apply(3, argv, 3, &oc, &ov);
    if (r2.rc == 1) { r2.argc = oc; r2.argv = ov; }
    expect_flag_value(&r2, "--fov", "0");
}

/* ---- [keys] --------------------------------------------------------- */

/* `keys` is a scope-local sub-table like `env` and `args`, harvested by
 * harvest_scope for EVERY scope. These cases are the reason that is one
 * call site: the failure they exist to catch is a [keys] that quietly
 * works at global scope only - which is exactly how the scoped spelling
 * of [args]/[env] came to be believed missing.
 *
 * Assertions read the keymap store rather than the argv splice: keys
 * never materialize as flags, so the resolved binding IS the result. */

/* keymap.c reaches SDL only through a lookup seam, and this test links no
 * SDL - so stand in a table with just the names these cases use, plus
 * every name the registry's own defaults are spelled with (they are
 * parsed at init, and an unresolvable default would read as a spurious
 * "unbound"). Codes are arbitrary but stable. */
static const struct { int kind; const char *name; int code; } g_fake_names[] = {
    { KM_SRC_KEY, "A", 1 },  { KM_SRC_KEY, "B", 2 },  { KM_SRC_KEY, "C", 3 },
    { KM_SRC_KEY, "D", 4 },  { KM_SRC_KEY, "F", 5 },  { KM_SRC_KEY, "G", 6 },
    { KM_SRC_KEY, "H", 7 },  { KM_SRC_KEY, "I", 8 },  { KM_SRC_KEY, "L", 9 },
    { KM_SRC_KEY, "M", 10 }, { KM_SRC_KEY, "N", 11 }, { KM_SRC_KEY, "O", 12 },
    { KM_SRC_KEY, "P", 13 }, { KM_SRC_KEY, "S", 14 }, { KM_SRC_KEY, "T", 15 },
    { KM_SRC_KEY, "U", 16 }, { KM_SRC_KEY, "V", 17 }, { KM_SRC_KEY, "W", 18 },
    { KM_SRC_KEY, "X", 19 }, { KM_SRC_KEY, "Y", 20 }, { KM_SRC_KEY, "Z", 21 },
    { KM_SRC_KEY, "1", 22 }, { KM_SRC_KEY, "2", 23 }, { KM_SRC_KEY, "3", 24 },
    { KM_SRC_KEY, "F5", 25 }, { KM_SRC_KEY, "F6", 26 }, { KM_SRC_KEY, "F7", 27 },
    { KM_SRC_KEY, "F8", 28 }, { KM_SRC_KEY, "F9", 29 },
    { KM_SRC_KEY, "Up", 30 }, { KM_SRC_KEY, "Down", 31 },
    { KM_SRC_KEY, "Left", 32 }, { KM_SRC_KEY, "Right", 33 },
    { KM_SRC_KEY, "Space", 34 }, { KM_SRC_KEY, "Return", 35 },
    { KM_SRC_KEY, "Tab", 36 },   { KM_SRC_KEY, "Backspace", 37 },
    { KM_SRC_KEY, "Left Shift", 38 },  { KM_SRC_KEY, "Right Shift", 39 },
    { KM_SRC_KEY, "Left Ctrl", 40 },   { KM_SRC_KEY, "Right Ctrl", 41 },
    { KM_SRC_KEY, "[", 42 },  { KM_SRC_KEY, "]", 43 },

    { KM_SRC_PADBTN, "south", 100 }, { KM_SRC_PADBTN, "east", 101 },
    { KM_SRC_PADBTN, "west", 102 },  { KM_SRC_PADBTN, "north", 103 },
    { KM_SRC_PADBTN, "start", 104 }, { KM_SRC_PADBTN, "back", 105 },
    { KM_SRC_PADBTN, "leftshoulder", 106 },
    { KM_SRC_PADBTN, "rightshoulder", 107 },
    { KM_SRC_PADBTN, "leftstick", 108 },
    { KM_SRC_PADBTN, "rightstick", 109 },
    { KM_SRC_PADBTN, "dpup", 110 },   { KM_SRC_PADBTN, "dpdown", 111 },
    { KM_SRC_PADBTN, "dpleft", 112 }, { KM_SRC_PADBTN, "dpright", 113 },

    { KM_SRC_PADAXIS, "leftx", 200 },  { KM_SRC_PADAXIS, "lefty", 201 },
    { KM_SRC_PADAXIS, "rightx", 202 }, { KM_SRC_PADAXIS, "righty", 203 },
    { KM_SRC_PADAXIS, "lefttrigger", 204 },
    { KM_SRC_PADAXIS, "righttrigger", 205 },
};

static int fake_name_to_code(int kind, const char *name) {
    for (size_t i = 0; i < sizeof g_fake_names / sizeof g_fake_names[0]; i++)
        if (g_fake_names[i].kind == kind && !strcmp(g_fake_names[i].name, name))
            return g_fake_names[i].code;
    return -1;
}

static const char *fake_code_to_name(int kind, int code) {
    for (size_t i = 0; i < sizeof g_fake_names / sizeof g_fake_names[0]; i++)
        if (g_fake_names[i].kind == kind && g_fake_names[i].code == code)
            return g_fake_names[i].name;
    return NULL;
}

static void install_fake_key_names(void) {
    openpete_keymap_set_lookup(fake_name_to_code, fake_code_to_name, NULL);
}

static int key_bound_to(openpete_action_t a, const char *name) {
    km_source_t s[KM_MAX_SOURCES];
    int n = openpete_keymap_sources(a, s, KM_MAX_SOURCES);
    for (int i = 0; i < n; i++) {
        if (s[i].kind != KM_SRC_KEY) continue;
        const char *nm = fake_code_to_name(KM_SRC_KEY, s[i].code);
        if (nm && !strcmp(nm, name)) return 1;
    }
    return 0;
}

static int key_source_count(openpete_action_t a) {
    km_source_t s[KM_MAX_SOURCES];
    return openpete_keymap_sources(a, s, KM_MAX_SOURCES);
}

static void case_keys_every_scope(void) {
    /* Each scope in turn must be able to set a binding, and the more
     * specific one must win - the same ladder the knobs obey. */
    run("keys: global scope",
        "[keys.host]\n"
        "teleport = \"F5\"\n", NULL, 0);
    CHECK(key_bound_to(OPK_TELEPORT, "F5"), "global [keys.host] ignored");

    run("keys: game scope beats global",
        "[keys.host]\n"
        "teleport = \"F5\"\n"
        "[game.spyro-1.keys.host]\n"
        "teleport = \"F6\"\n", NULL, 0);
    CHECK(key_bound_to(OPK_TELEPORT, "F6"),
          "[game.spyro-1.keys.host] must beat the global [keys.host]");

    run("keys: disc serial beats game",
        "[game.spyro-1.keys.host]\n"
        "teleport = \"F6\"\n"
        "[disc.SCUS-94228.keys.host]\n"
        "teleport = \"F7\"\n", NULL, 1);
    CHECK(key_bound_to(OPK_TELEPORT, "F7"),
          "[disc.SERIAL.keys.host] must beat [game.*]");

    run("keys: disc sha beats serial",
        "[disc.SCUS-94228.keys.host]\n"
        "teleport = \"F7\"\n"
        "[disc.95f03abf.keys.host]\n"
        "teleport = \"F8\"\n", NULL, 1);
    CHECK(key_bound_to(OPK_TELEPORT, "F8"),
          "[disc.SHA.keys.host] must beat [disc.SERIAL]");

    run("keys: saved beats every file scope",
        "[disc.95f03abf.keys.host]\n"
        "teleport = \"F8\"\n"
        "[saved.keys.host]\n"
        "teleport = \"F9\"\n", NULL, 1);
    CHECK(key_bound_to(OPK_TELEPORT, "F9"),
          "[saved.keys.host] must beat every file scope");

    /* The pad namespace scopes identically - it is the same walk. */
    run("keys: pad scopes too",
        "[game.spyro-1.keys.pad]\n"
        "cross = \"Space\"\n", NULL, 0);
    CHECK(key_bound_to(OPK_PAD_CROSS, "Space"),
          "[game.spyro-1.keys.pad] ignored");
}

/* Merge is per ACTION. A scope that names one binding must leave every
 * other one alone - the regression this guards is a scope being made to
 * replace a whole table, which would silently reset 40 bindings. */
static void case_keys_per_action_merge(void) {
    run("keys: per-action merge",
        "[keys.host]\n"
        "teleport = \"F5\"\n"
        "pause    = \"F6\"\n"
        "[game.spyro-1.keys.host]\n"
        "teleport = \"F7\"\n", NULL, 0);
    CHECK(key_bound_to(OPK_TELEPORT, "F7"), "the named action must change");
    CHECK(key_bound_to(OPK_PAUSE, "F6"),
          "an action the higher scope did not name must survive");
}

/* Unbinding: "none" and [] both clear, and a higher scope can unbind
 * something a lower scope bound. This is the "don't ship the P shortcut"
 * path end to end. */
static void case_keys_unbind(void) {
    run("keys: unbind with none",
        "[keys.host]\n"
        "rewind = \"none\"\n", NULL, 0);
    CHECK(key_source_count(OPK_REWIND) == 0, "\"none\" must unbind");

    run("keys: unbind with an empty array",
        "[keys.host]\n"
        "rewind = []\n", NULL, 0);
    CHECK(key_source_count(OPK_REWIND) == 0, "[] must unbind");

    run("keys: a higher scope unbinds a lower one's binding",
        "[keys.host]\n"
        "teleport = \"F5\"\n"
        "[game.spyro-1.keys.host]\n"
        "teleport = \"none\"\n", NULL, 0);
    CHECK(key_source_count(OPK_TELEPORT) == 0,
          "a higher scope's unbind must win");

    /* KM_DEV rows are unbound with no config at all, and bindable back. */
    run("keys: dev actions are off by default", "", NULL, 0);
    CHECK(key_source_count(OPK_FILLPICK) == 0,
          "a dev instrument must not be live in a stock config");
    run("keys: dev actions can be bound back",
        "[keys.host]\n"
        "fillpick = \"P\"\n", NULL, 0);
    CHECK(key_bound_to(OPK_FILLPICK, "P"), "a dev key must be bindable");
}

/* Arrays and comma-separated scalars are one spelling of one binding,
 * and a bad row is an error rather than a silently dropped line. */
static void case_keys_values(void) {
    run("keys: array of sources",
        "[keys.host]\n"
        "teleport = [\"F5\", \"pad:north\"]\n", NULL, 0);
    CHECK(key_source_count(OPK_TELEPORT) == 2, "an array must bind both");

    run("keys: comma scalar equals an array",
        "[keys.host]\n"
        "teleport = \"F5,pad:north\"\n", NULL, 0);
    CHECK(key_source_count(OPK_TELEPORT) == 2,
          "a comma scalar must bind both");

    run_t r;
    r = run("keys: unknown action is an error",
            "[keys.host]\n"
            "telepart = \"F5\"\n", NULL, 0);
    CHECK(r.rc < 0, "an unknown action must abort startup, not be ignored");

    r = run("keys: unknown namespace is an error",
            "[keys.padd]\n"
            "cross = \"C\"\n", NULL, 0);
    CHECK(r.rc < 0, "an unknown [keys.*] namespace must be named");

    r = run("keys: unknown key name is an error",
            "[keys.host]\n"
            "teleport = \"Nonexistent\"\n", NULL, 0);
    CHECK(r.rc < 0, "an unknown key name must abort startup");
}

/* The `portable` directive decides where openpete writes: the game folder
 * or the platform's per-user directories. It is the one top-level word
 * that is a directive rather than a knob, so it appears in no decision
 * table - these assertions are the only thing pinning it down. */
static void case_portable_layout(void) {
    char exedir[512], dir[512];
    CHECK(openpete_exe_dir(exedir, sizeof exedir) == 0,
          "the test binary's directory must resolve");

#ifdef _WIN32
    const int platform_default = 1;
#else
    const int platform_default = 0;
#endif

    run("portable: platform default with no directive", "", NULL, 0);
    CHECK(openpete_paths_portable() == platform_default,
          "no directive must leave the platform default in force");

    run("portable: true", "portable = true\n", NULL, 0);
    CHECK(openpete_paths_portable() == 1, "portable = true must select portable");
    CHECK(openpete_paths_state_dir("cards", dir, sizeof dir, 1) == 0, "cards must resolve");
    CHECK(strstr(dir, exedir) == dir, "a portable install keeps cards next to the exe");
    rmdir_quiet(dir);

    run("portable: false", "portable = false\n", NULL, 0);
    CHECK(openpete_paths_portable() == 0, "portable = false must select per-user");
    CHECK(openpete_paths_state_dir("states", dir, sizeof dir, 0) == 0, "states must resolve");
    CHECK(strstr(dir, exedir) != dir,
          "the per-user layout must not land in the game folder");

    /* A directive must not outlive the config that carried it, or a
     * second resolve in one process inherits the first one's layout. */
    run("portable: directive does not persist", "", NULL, 0);
    CHECK(openpete_paths_portable() == platform_default,
          "dropping the directive must restore the platform default");
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_toml_path, sizeof g_toml_path, "%s/openpete-config-test.toml",
             (tmp && *tmp) ? tmp : "/tmp");

    /* The keymap's name lookups are an SDL seam; this test links no SDL,
     * so install the same minimal fake table the binding assertions read
     * back through. Only the names these cases actually use. */
    install_fake_key_names();

    case_spelling_mirror();
    case_specificity_beats_notation();
    case_cross_spelling_inversion();
    case_launch_tier_suppresses();
    case_real_env_beats_file();
    case_saved_tier();
    case_false_suppresses();
    case_alias_collapses();
    case_renamed_key();
    case_env_only_knob();
    case_validation();
    case_scope_beats_file();
    case_unregistered_flags();
    case_writer_round_trip();
    case_token_source_by_pointer();
    case_fov_zero_round_trip();
    case_schema_sanity();
    case_keys_every_scope();
    case_keys_per_action_merge();
    case_keys_unbind();
    case_keys_values();
    case_portable_layout();

    remove(g_toml_path);
    if (g_fail) {
        fprintf(stderr, "config_resolve_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("config_resolve_test: all cases passed\n");
    return 0;
}
