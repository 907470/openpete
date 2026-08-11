/* tests/unit/config_resolve_test.c — openpete.toml precedence.
 *
 * The resolver's whole job is deciding which of several layers wins and
 * then materializing exactly ONE spelling of it. Prose can't hold that
 * ordering steady across edits; this can. Each case is a synthetic layer
 * stack plus the argv/env it must produce.
 *
 * The assertions are on the MATERIALIZED result — the spliced argv and the
 * process environment — not on the report, because that is what the rest
 * of the program actually consumes.
 */

#include "openpete_config.h"
#include "../../core/runtime/config_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void openpete_config_reset_for_test(void);

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
    openpete_config_reset_for_test();
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
 * sentinel-unset consumer, which takes the flag whenever one is present —
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
 * emitted at all — otherwise the flag parser sees both and argv order,
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
        "\"--render-stats\" = true\n"
        "[game.spyro-1.args]\n"
        "\"--render-stats\" = false\n", NULL, 0);
    expect_no_flag(&r, "--render-stats");

    r = run("...and true emits it",
        "[args]\n"
        "\"--render-stats\" = true\n", NULL, 0);
    CHECK(arg_index(&r, "--render-stats") >= 0, "--render-stats should be emitted");
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

/* An env-only knob spelled as a bare key goes out as its variable — and
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

/* Typos and bad values are hard errors: a config line that silently does
 * nothing is the failure mode that costs an hour. */
static void case_validation(void) {
    run_t r = run("unknown key", "lodd = \"extended\"\n", NULL, 0);
    CHECK(r.rc < 0, "an unknown key must fail the run");

    r = run("bad enum value", "lod = \"turbo\"\n", NULL, 0);
    CHECK(r.rc < 0, "a value outside the vocabulary must fail the run");

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
 * express — see the file-order assertion in the report test below. */
static void case_scope_beats_file(void) {
    run_t r = run("scope still outranks the file",
        "[game.spyro-1]\n"
        "ssaa = 2\n"
        "[disc.SCUS-94228]\n"
        "ssaa = 4\n", NULL, 1);
    expect_flag_value(&r, "--ssaa", "4");
}

/* Unregistered flags (no schema row) still layer, and still collapse to
 * one copy — the resolver dedupes rather than trusting argv order,
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
 * The registry itself can't be linked here — its rows point into the
 * renderer — so knob_registry_init runs the reverse half of this check at
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
    openpete_config_reset_for_test();
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

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_toml_path, sizeof g_toml_path, "%s/openpete-config-test.toml",
             (tmp && *tmp) ? tmp : "/tmp");

    case_spelling_mirror();
    case_specificity_beats_notation();
    case_cross_spelling_inversion();
    case_launch_tier_suppresses();
    case_real_env_beats_file();
    case_saved_tier();
    case_false_suppresses();
    case_alias_collapses();
    case_env_only_knob();
    case_validation();
    case_scope_beats_file();
    case_unregistered_flags();
    case_writer_round_trip();
    case_schema_sanity();

    remove(g_toml_path);
    if (g_fail) {
        fprintf(stderr, "config_resolve_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("config_resolve_test: all cases passed\n");
    return 0;
}
