/* mod_material_link_test - material composition by source prefixing
 * (core/mods/mod_mat_link.c) on the census shaders and the fixtures.
 *
 * Census (--census <mods-dir>, repeatable): every material fragment under
 * <mods-dir>/<mod>/ (a .frag naming op_material, dot directories skipped)
 * is compiled per profile through the compile-on-boot front end (stock,
 * terrain, portal for the scene contract; stock for the decal contract),
 * prepared, and printed with its verdict and reason, identity and discard.
 * --expect-linkable fails the run on any census member that is not
 * linkable, except a shader named by --expect-unlinkable <label> (its
 * census label, <mod>/<path>), which fails the run when it is linkable
 * in any profile. Then per contract and profile every linkable non-identity
 * member enters one combined module, and the scene stock members also enter
 * the actor module (OPENPETE_MOD_LINK_ACTOR).
 *
 * Fixtures (--fixtures <dir>): <dir>/cases.tsv holds one case per line,
 * tab-separated, paths relative to <dir>:
 *   member   <path> <profile|all> <verdict> [identity] [kills]
 *   prebuilt <path> <profile>      (its module without source: no-source)
 *   refuse   <path> <pin> <0|1>                       (the discard refusal)
 *   combine  <profile> <ok|tables>=N|compile|discard> <path>... (one module)
 *   vkbad    <path>   (under --vk: its stock module draws a validation error)
 * Each case asserts its expected result.
 *
 * Every combined module is checked: it compiles, spirv-val accepts it
 * (--target-env vulkan1.0) when a validator is given, and every member order
 * tried (all orders up to five members, else rotations, the reversal and
 * eight shuffles) gives byte-identical text and SPIR-V and the same case
 * per member. With --vk the module's pipelines are created on a headless
 * device under the Khronos validation layer (mod_material_link_vk.c), and
 * any `Validation Error` fails the run.
 *
 * Usage: mod_material_link_test <sdk-dir> <spirv-val|""> [--vk]
 *          [--expect-linkable] [--expect-unlinkable <label>]... [--census <mods-dir>]...
 *          [--fixtures <dir>]
 * Exit 0 when every check passes and at least one combined module was
 * built and checked; 1 otherwise; 77 under --vk when no Vulkan device or
 * no validation layer is present. */

#include "mod_glsl.h"
#include "mod_mat_link.h"
#include "mod_material_link_vk.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#define op_getpid _getpid
#else
#include <unistd.h>
#define op_getpid getpid
#endif

#define MAX_ENTRIES 512
#define MAX_MEMBERS 64
#define PATH_MAX_T  (OPENPETE_MOD_GLSL_PATH_MAX * 2)

/* One material fragment shader and its prepared profiles. */
typedef struct {
    char path[PATH_MAX_T];
    char label[PATH_MAX_T];
    int  contract;
    int  profiles;       /* 3 scene, 1 decal */
    int  census;         /* from a census directory */
    uint32_t* spv[OPENPETE_MOD_LINK_PROFILES];
    size_t    words[OPENPETE_MOD_LINK_PROFILES];
    openpete_mod_link_member_t m[OPENPETE_MOD_LINK_PROFILES];
} entry_t;

static entry_t s_e[MAX_ENTRIES];
static int s_n;
static openpete_mod_link_ctx_t* s_ctx;
static const char* s_sdk;
static const char* s_spirv_val;
static int s_vk;
static int s_fail, s_modules, s_vk_pipes;
/* Scratch files in the working directory, named by process id: the
 * mod_material_link and mod_material_link_vk tests run this program at once
 * under ctest -j, and a shared name lets one remove or overwrite the
 * other's module before spirv-val reads it. Set by scratch_names(). */
static char s_log[64];
static char s_spv[64];
static const char* const k_prof[OPENPETE_MOD_LINK_PROFILES] = { "stock", "terrain", "portal" };

/** @brief Report a failed check and count it. */
static void fail(const char* what, const char* detail) {
    printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    s_fail++;
}

/** @brief Return whether a path is a directory. */
static int is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/** @brief Compile one profile of a shader standalone, as registration does.
 *
 * @return The word count, 0 on failure. */
static size_t compile_standalone(const char* path, const char* src, int profile, int contract,
                                 uint32_t** out) {
    openpete_mod_glsl_include_t inc;
    openpete_mod_glsl_include_init(&inc, path, s_sdk);
    return openpete_mod_glsl_compile(src, OPENPETE_MOD_GLSL_STAGE_FRAG, &inc,
                                     openpete_mod_link_profile_preamble(contract, profile), s_log,
                                     out);
}

/** @brief Add a shader to the entry table, compile its profiles and prepare each.
 *
 * @param label The name printed for it.
 * @param census Nonzero for a census shader.
 * @return The entry, or NULL when unreadable or the table is full. */
static entry_t* add_entry(const char* path, const char* label, int census) {
    char* src = s_n < MAX_ENTRIES ? openpete_mod_glsl_read_file(path, NULL) : NULL;
    if (!src) { fail("unreadable or table full", path); return NULL; }
    entry_t* e = &s_e[s_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->label, sizeof e->label, "%s", label);
    e->census = census;
    e->contract = strstr(src, "openpete_psx_decal.glsl") ? OPENPETE_MOD_LINK_CONTRACT_DECAL
                                                          : OPENPETE_MOD_LINK_CONTRACT_SCENE;
    e->profiles = e->contract == OPENPETE_MOD_LINK_CONTRACT_SCENE ? OPENPETE_MOD_LINK_PROFILES : 1;
    for (int p = 0; p < e->profiles; p++) {
        e->words[p] = compile_standalone(path, src, p, e->contract, &e->spv[p]);
        if (!e->words[p]) { fail("standalone compile", path); continue; }
        openpete_mod_link_inspect(e->contract, p, e->spv[p], e->words[p], &e->m[p]);
        if (e->m[p].verdict != OPENPETE_MOD_LINK_PENDING) fail("inspect not pending", path);
        openpete_mod_link_prepare(s_ctx, src, path, NULL, NULL, s_log, &e->m[p]);
    }
    free(src);
    return e;
}

/** @brief Print one entry's verdict per profile. */
static void print_entry(const entry_t* e) {
    for (int p = 0; p < e->profiles; p++) {
        const openpete_mod_link_member_t* m = &e->m[p];
        printf("%-9s %s [%s] block=%u%s%s%s%s\n", openpete_mod_link_verdict_name(m->verdict),
               e->label, e->contract ? "decal" : k_prof[p], m->block_fields >= 0 ? m->block_bytes : 0,
               m->identity ? " identity" : "", m->kills ? " kills" : "",
               m->reason[0] ? " - " : "", m->reason);
    }
}

/** @brief Collect every material fragment under a directory into the entry table, recursively, skipping dot entries.
 *
 * @param root The census directory, for labels relative to it. */
static void collect(const char* root, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    char names[256][256];
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && n < 256)
        if (de->d_name[0] != '.' && strlen(de->d_name) < 256) snprintf(names[n++], 256, "%s", de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], (int (*)(const void*, const void*))strcmp);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX_T];
        if (snprintf(p, sizeof p, "%s/%s", dir, names[i]) >= (int)sizeof p) {
            fail("path too long", names[i]);
            continue;
        }
        if (is_dir(p)) { collect(root, p); continue; }
        if (openpete_mod_glsl_stage_from_path(p) != OPENPETE_MOD_GLSL_STAGE_FRAG) continue;
        char* src = openpete_mod_glsl_read_file(p, NULL);
        const int material = src && strstr(src, "op_material") != NULL;
        free(src);
        if (material) add_entry(p, p + strlen(root) + 1, 1);
    }
}

/** @brief Write words to a temporary file and run spirv-val on them.
 *
 * @return 0 when valid or when no validator was given, 1 otherwise. */
static int spirv_val(const uint32_t* w, size_t n) {
    if (!s_spirv_val) return 0;
    const char* path = s_spv;
    FILE* f = fopen(path, "wb");
    if (!f) return 1;
    const size_t put = fwrite(w, sizeof *w, n, f);
    fclose(f);
    char cmd[PATH_MAX_T + 64];
    snprintf(cmd, sizeof cmd, "\"%s\" --target-env vulkan1.0 %s", s_spirv_val, path);
    const int rc = put == n ? system(cmd) : 1;
    remove(path);
    return rc != 0;
}

/** @brief Fill an order permutation for trial t of n members: every order (n <= 5) or rotations, the reversal and LCG shuffles. */
static void trial_order(int t, int n, int* ord) {
    for (int i = 0; i < n; i++) ord[i] = i;
    if (n <= 5) {   /* the t-th permutation in factorial base */
        int pool[MAX_MEMBERS], left = n, rest = t;
        for (int i = 0; i < n; i++) pool[i] = i;
        for (int i = 0; i < n; i++) {
            int f = 1;
            for (int k = 2; k < left; k++) f *= k;
            const int pick = rest / f;
            rest %= f;
            ord[i] = pool[pick];
            for (int k = pick; k < left - 1; k++) pool[k] = pool[k + 1];
            left--;
        }
        return;
    }
    if (t < n) { for (int i = 0; i < n; i++) ord[i] = (i + t) % n; return; }
    if (t == n) { for (int i = 0; i < n; i++) ord[i] = n - 1 - i; return; }
    uint32_t s = 2654435761u * (uint32_t)t;
    for (int i = n - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        const int j = (int)(s % (uint32_t)(i + 1)), tmp = ord[i];
        ord[i] = ord[j];
        ord[j] = tmp;
    }
}

/** @brief Return the number of member orders tried for n members. */
static int trials_for(int n) {
    if (n <= 5) { int f = 1; for (int k = 2; k <= n; k++) f *= k; return f; }
    return n + 1 + 8;
}

/** @brief Build a module from the members in one order and compare it with the reference build.
 *
 * @param ref The first order's module.
 * @param ref_case The first order's case per member.
 * @return 0 when identical, 1 otherwise. */
static int same_in_order(const openpete_mod_link_member_t* const* mem, int n, int contract,
                         int profile, const int* ord, const openpete_mod_link_module_t* ref,
                         const int* ref_case) {
    const openpete_mod_link_member_t* in[MAX_MEMBERS];
    int cases[MAX_MEMBERS];
    for (int i = 0; i < n; i++) in[i] = mem[ord[i]];
    openpete_mod_link_module_t m;
    int bad = openpete_mod_link_build(s_ctx, in, n, contract, profile, NULL, NULL, s_log, cases, &m) != 0;
    bad = bad || m.words != ref->words || strcmp(m.text, ref->text) != 0 ||
          memcmp(m.spv, ref->spv, m.words * 4) != 0;
    for (int i = 0; i < n && !bad; i++) bad = cases[i] != ref_case[ord[i]];
    openpete_mod_link_module_free(&m);
    return bad;
}

/** @brief Write a module's text to <dir>/<name>.frag when MOD_MATERIAL_LINK_DUMP names a directory, for reading it. */
static void dump_text(const char* name, const char* text) {
    const char* dir = getenv("MOD_MATERIAL_LINK_DUMP");
    if (!dir || !*dir) return;
    char p[PATH_MAX_T];
    int k = snprintf(p, sizeof p, "%s/", dir);
    for (const char* c = name; *c && k + 6 < (int)sizeof p; c++)
        p[k++] = (*c == '/' || *c == ' ' || *c == '[' || *c == ']') ? '_' : *c;
    snprintf(p + k, sizeof p - (size_t)k, ".frag");
    FILE* f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/** @brief Check a built module: spirv-val, member-order identity and, under --vk, the validation layer.
 *
 * @return 0 when every check passes. */
static int check_module(const char* name, const openpete_mod_link_member_t* const* mem, int n,
                        int contract, int profile, const openpete_mod_link_module_t* mod,
                        const int* cases) {
    int bad = 0;
    dump_text(name, mod->text);
    if (spirv_val(mod->spv, mod->words)) { fail("spirv-val", name); bad = 1; }
    if (strstr(mod->text, "[[m")) { fail("an attribute name was prefixed", name); bad = 1; }
    int orders = 0;
    for (int t = 1; t < trials_for(n); t++, orders++) {
        int ord[MAX_MEMBERS];
        trial_order(t, n, ord);
        if (same_in_order(mem, n, contract, profile, ord, mod, cases)) { fail("member order changes the module", name); bad = 1; break; }
    }
    int vk_errors = 0, vk_made = 0;
    if (s_vk && link_vk_pipelines(mod->spv, mod->words, contract, profile, &vk_made, &vk_errors) != 0) {
        fail("pipeline create", name);
        bad = 1;
    }
    if (vk_errors) { fail("validation errors", name); bad = 1; }
    s_vk_pipes += vk_made;
    printf("module    %s: %d member(s), %d case(s), %zu words, %d table(s) %zu B moved, "
           "%d other order(s) identical%s, spirv-val %s", name, n, mod->cases, mod->words,
           mod->cdata.tables, mod->cdata.len, bad ? 0 : orders, bad ? " (FAILED)" : "",
           s_spirv_val ? "ok" : "skipped");
    if (s_vk) printf(", %d pipeline(s) %d validation error(s)", vk_made, vk_errors);
    printf("\n");
    s_modules++;
    return bad;
}

/** @brief Build one module from members and run check_module on it.
 *
 * @param expect 0 to expect a module, 1 a compile failure, 2 a discard refusal.
 * @param min_tables The constant tables the module must move.
 * @return 0 when the outcome matches. */
static int build_and_check(const char* name, const openpete_mod_link_member_t* const* mem, int n,
                           int contract, int profile, int expect, int min_tables) {
    openpete_mod_link_module_t mod;
    int cases[MAX_MEMBERS];
    const int rc = openpete_mod_link_build(s_ctx, mem, n, contract, profile, NULL, NULL, s_log, cases, &mod);
    int bad = 0;
    if (expect == 0 && rc != 0) { fail(name, mod.log); bad = 1; }
    else if (expect != 0 && (rc == 0 || (expect == 2) != (mod.kills != 0))) { fail("expected refusal", name); bad = 1; }
    else if (rc == 0 && mod.cdata.tables < min_tables) { fail("too few tables moved", name); bad = 1; }
    else if (rc == 0) bad = check_module(name, mem, n, contract, profile, &mod, cases);
    else printf("refused   %s: %s\n", name, mod.log);
    openpete_mod_link_module_free(&mod);
    return bad;
}

/** @brief Build one census module from every linkable non-identity census member of a contract and member profile.
 *
 * @param c The contract.
 * @param mp The member profile.
 * @param bp The build profile: @p mp, or OPENPETE_MOD_LINK_ACTOR over the scene stock members.
 * @param label The build profile's name in the module line. */
static void census_module(int c, int mp, int bp, const char* label) {
    const openpete_mod_link_member_t* mem[MAX_MEMBERS];
    int n = 0;
    for (int i = 0; i < s_n && n < MAX_MEMBERS; i++) {
        const entry_t* e = &s_e[i];
        if (!e->census || e->contract != c || mp >= e->profiles) continue;
        if (e->m[mp].verdict == OPENPETE_MOD_LINK_OK && !e->m[mp].identity) mem[n++] = &e->m[mp];
    }
    if (!n) return;
    char name[64];
    snprintf(name, sizeof name, "census %s/%s", c ? "decal" : "scene", label);
    build_and_check(name, mem, n, c, bp, 0, 0);
}

/** @brief Build the census module of each contract and profile, and the scene actor module over the stock members. */
static void census_modules(void) {
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < OPENPETE_MOD_LINK_PROFILES; p++) census_module(c, p, p, k_prof[p]);
    census_module(OPENPETE_MOD_LINK_CONTRACT_SCENE, OPENPETE_MOD_LINK_STOCK, OPENPETE_MOD_LINK_ACTOR, "actor");
}

/** @brief Find or add the fixture entry for a path relative to the fixture directory. */
static entry_t* fixture(const char* dir, const char* rel) {
    char p[PATH_MAX_T];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_e[i].path, p) == 0) return &s_e[i];
    entry_t* e = add_entry(p, rel, 0);
    if (e) print_entry(e);
    return e;
}

/** @brief Return a profile index by name, -1 for "all", -2 when unknown. */
static int profile_of(const char* s) {
    for (int p = 0; p < OPENPETE_MOD_LINK_PROFILES; p++)
        if (strcmp(s, k_prof[p]) == 0) return p;
    return strcmp(s, "all") == 0 ? -1 : -2;
}

/** @brief Run a `member` case: the verdict per profile, and the identity and kills flags. */
static void case_member(const char* dir, char** f, int nf) {
    entry_t* e = nf >= 4 ? fixture(dir, f[1]) : NULL;
    const int prof = nf >= 4 ? profile_of(f[2]) : -2;
    if (!e || prof < -1) { fail("bad member case", nf > 1 ? f[1] : "?"); return; }
    int want_id = 0, want_kills = 0;
    for (int i = 4; i < nf; i++) { want_id |= strcmp(f[i], "identity") == 0; want_kills |= strcmp(f[i], "kills") == 0; }
    for (int p = 0; p < e->profiles; p++) {
        if (prof >= 0 && p != prof) continue;
        const openpete_mod_link_member_t* m = &e->m[p];
        char what[PATH_MAX_T + 64];
        snprintf(what, sizeof what, "%s [%s] expected %s identity=%d kills=%d", f[1], k_prof[p], f[3],
                 want_id, want_kills);
        if (strcmp(openpete_mod_link_verdict_name(m->verdict), f[3]) != 0 ||
            m->identity != want_id || m->kills != want_kills)
            fail(what, m->reason);
        else
            printf("ok        member %s\n", what);
    }
}

/** @brief Run a `prebuilt` case: the module compiled from a file the census does not read as source, handed over without its source, is unlinkable with verdict no-source. */
static void case_prebuilt(const char* dir, char** f, int nf) {
    char p[PATH_MAX_T];
    snprintf(p, sizeof p, "%s/%s", dir, nf > 1 ? f[1] : "");
    char* src = nf >= 3 ? openpete_mod_glsl_read_file(p, NULL) : NULL;
    const int prof = nf >= 3 && profile_of(f[2]) >= 0 ? profile_of(f[2]) : 0;
    uint32_t* w = NULL;
    const size_t n = src ? compile_standalone(p, src, prof, OPENPETE_MOD_LINK_CONTRACT_SCENE, &w) : 0;
    free(src);
    if (!n) { fail("prebuilt module does not compile", p); return; }
    openpete_mod_link_member_t m;
    openpete_mod_link_prebuilt(OPENPETE_MOD_LINK_CONTRACT_SCENE, prof, w, n, &m);
    if (m.verdict != OPENPETE_MOD_LINK_NO_SOURCE) fail("prebuilt not no-source", f[1]);
    else printf("ok        prebuilt %s no-source (%s)\n", f[1], m.reason);
    openpete_mod_link_member_free(&m);
    free(w);
}

/** @brief Run a `refuse` case: the discard refusal of the stock module at an api pin. */
static void case_refuse(const char* dir, char** f, int nf) {
    entry_t* e = nf >= 4 ? fixture(dir, f[1]) : NULL;
    if (!e || !e->spv[0]) { fail("bad refuse case", nf > 1 ? f[1] : "?"); return; }
    const int pin = atoi(f[2]), want = atoi(f[3]);
    const int got = openpete_mod_link_discard_refused(pin, e->spv[0], e->words[0]);
    char what[PATH_MAX_T + 32];
    snprintf(what, sizeof what, "%s at api %d refused=%d", f[1], pin, want);
    if (got != want) fail(what, NULL);
    else printf("ok        refuse %s\n", what);
}

/** @brief Run a `vkbad` case under --vk: the standalone stock module must draw a validation error, which proves the layer reports to the count (the negative control). */
static void case_vkbad(const char* dir, char** f, int nf) {
    if (!s_vk) return;
    entry_t* e = nf >= 2 ? fixture(dir, f[1]) : NULL;
    if (!e || !e->spv[0]) { fail("bad vkbad case", nf > 1 ? f[1] : "?"); return; }
    int made = 0, errors = 0;
    link_vk_pipelines(e->spv[0], e->words[0], e->contract, 0, &made, &errors);
    if (errors == 0) fail("negative control drew no validation error", f[1]);
    else printf("ok        vkbad %s: %d validation error(s) on the standalone module\n", f[1], errors);
}

/** @brief Run a `combine` case: one module from the listed members, expecting a module, a compile failure or a discard refusal. */
static void case_combine(const char* dir, char** f, int nf) {
    const int prof = nf >= 4 ? profile_of(f[1]) : -2;
    const int tables = nf >= 4 && strncmp(f[2], "tables>=", 8) == 0 ? atoi(f[2] + 8) : 0;
    const int expect = nf < 4 ? -1 : (tables || strcmp(f[2], "ok") == 0) ? 0
                     : strcmp(f[2], "discard") == 0 ? 2 : 1;
    if (prof < 0 || expect < 0) { fail("bad combine case", NULL); return; }
    const openpete_mod_link_member_t* mem[MAX_MEMBERS];
    int n = 0, contract = -1;
    char name[PATH_MAX_T] = "combine";
    for (int i = 3; i < nf && n < MAX_MEMBERS; i++) {
        entry_t* e = fixture(dir, f[i]);
        if (!e || prof >= e->profiles) { fail("combine member missing", f[i]); return; }
        mem[n++] = &e->m[prof];
        contract = e->contract;
        strncat(name, " ", sizeof name - strlen(name) - 1);
        strncat(name, f[i], sizeof name - strlen(name) - 1);
    }
    strncat(name, " [", sizeof name - strlen(name) - 1);
    strncat(name, k_prof[prof], sizeof name - strlen(name) - 1);
    strncat(name, "]", sizeof name - strlen(name) - 1);
    if (!build_and_check(name, mem, n, contract, prof, expect, tables) && expect) printf("ok        %s refused as expected\n", name);
}

/** @brief Split a line at tabs, in place; returns the field count. */
static int split_tabs(char* line, char** f, int cap) {
    int n = 0;
    for (char* p = line; p && *p && n < cap;) {
        f[n++] = p;
        char* t = strchr(p, '\t');
        if (t) *t = '\0';
        p = t ? t + 1 : NULL;
    }
    return n;
}

/** @brief Run every case of a fixture directory's cases.tsv. */
static void run_fixtures(const char* dir) {
    char path[PATH_MAX_T];
    snprintf(path, sizeof path, "%s/cases.tsv", dir);
    char* txt = openpete_mod_glsl_read_file(path, NULL);
    if (!txt) { fail("no cases.tsv", path); return; }
    for (char* line = txt; line && *line;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* f[MAX_MEMBERS + 4];
        const int nf = line[0] && line[0] != '#' ? split_tabs(line, f, MAX_MEMBERS + 4) : 0;
        if (nf && strcmp(f[0], "member") == 0) case_member(dir, f, nf);
        else if (nf && strcmp(f[0], "prebuilt") == 0) case_prebuilt(dir, f, nf);
        else if (nf && strcmp(f[0], "refuse") == 0) case_refuse(dir, f, nf);
        else if (nf && strcmp(f[0], "combine") == 0) case_combine(dir, f, nf);
        else if (nf && strcmp(f[0], "vkbad") == 0) case_vkbad(dir, f, nf);
        else if (nf) fail("unknown case", f[0]);
        line = nl ? nl + 1 : NULL;
    }
    free(txt);
}

/* Census labels --expect-unlinkable names: shaders built to be unlinkable. */
static const char* s_unlinkable[8];
static int s_unlinkable_n;

/** @brief Return whether --expect-unlinkable names a census label. */
static int expected_unlinkable(const char* label) {
    for (int i = 0; i < s_unlinkable_n; i++)
        if (strcmp(s_unlinkable[i], label) == 0) return 1;
    return 0;
}

/** @brief Print the census verdicts and fail on an unlinkable census member when asked, or on a linkable one --expect-unlinkable names. */
static void report_census(int first, int expect_linkable) {
    int linkable = 0, total = 0, ident = 0;
    for (int i = first; i < s_n; i++) {
        const entry_t* e = &s_e[i];
        const int want_ok = !expected_unlinkable(e->label);
        print_entry(e);
        for (int p = 0; p < e->profiles; p++) {
            const int ok = e->m[p].verdict == OPENPETE_MOD_LINK_OK;
            total++;
            linkable += ok;
            ident += e->m[p].identity;
            if (expect_linkable && ok != want_ok)
                fail(want_ok ? "census member unlinkable" : "census member linkable", e->label);
        }
    }
    printf("census    %d shader(s), %d member(s), %d linkable, %d identity\n", s_n - first, total,
           linkable, ident);
}

/** @brief Name the glslang log and the spirv-val input after this process, so concurrent runs share no file. */
static void scratch_names(void) {
    const long pid = (long)op_getpid();
    snprintf(s_log, sizeof s_log, "mod_material_link_test.%ld.log", pid);
    snprintf(s_spv, sizeof s_spv, "mod_material_link_test.%ld.spv", pid);
}

/** @brief Fail VACUOUS on every engagement counter of the run still at zero, then print the summary line.
 *
 * One row per counter: the combined modules built, and under --vk the
 * pipelines created under the validation layer and the cell-record terrain
 * pipelines among them. A row that does not apply to the run is skipped. */
static void judge_engagement(void) {
    const int cells = link_vk_cell_pipelines();
    const struct { int applies, n; const char* none; } need[] = {
        { 1, s_modules, "no combined module was built" },
        { s_vk, s_vk_pipes, "no pipeline was created under the validation layer" },
        { s_vk, cells, "no cell-record terrain pipeline was created under the validation layer" },
    };
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++)
        if (need[i].applies && !need[i].n) fail("VACUOUS", need[i].none);
    printf("mod_material_link: %d entr%s, %d module(s), %d pipeline(s) (%d cell-record), "
           "%d failure(s)\n", s_n, s_n == 1 ? "y" : "ies", s_modules, s_vk_pipes, cells, s_fail);
}

/** @brief Run the census and fixture checks the command line names and report the failures.
 *
 * @param argc Argument count.
 * @param argv The SDK directory, the spirv-val path or "", then the options in the usage line.
 * @return 0 when every check passed, 1 on a failure, 77 under --vk with no device or layer. */
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <sdk-dir> <spirv-val|\"\"> [--vk] [--expect-linkable] "
                        "[--expect-unlinkable <label>]... [--census <mods-dir>]... [--fixtures <dir>]\n",
                argv[0]);
        return 1;
    }
    scratch_names();
    s_sdk = argv[1];
    s_spirv_val = argv[2][0] ? argv[2] : NULL;
    int expect_linkable = 0;
    for (int i = 3; i < argc; i++) {
        s_vk |= strcmp(argv[i], "--vk") == 0;
        expect_linkable |= strcmp(argv[i], "--expect-linkable") == 0;
        if (i + 1 < argc && strcmp(argv[i], "--expect-unlinkable") == 0 && s_unlinkable_n < 8)
            s_unlinkable[s_unlinkable_n++] = argv[i + 1];
    }
    if (s_vk) {
        const int rc = link_vk_open();
        if (rc) return rc;
    }
    s_ctx = openpete_mod_link_ctx_new(s_sdk);
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--census") != 0) continue;
        const int first = s_n;
        collect(argv[i + 1], argv[i + 1]);
        report_census(first, expect_linkable);
    }
    census_modules();
    for (int i = 3; i + 1 < argc; i++)
        if (strcmp(argv[i], "--fixtures") == 0) run_fixtures(argv[i + 1]);
    for (int i = 0; i < s_n; i++)
        for (int p = 0; p < OPENPETE_MOD_LINK_PROFILES; p++) {
            openpete_mod_link_member_free(&s_e[i].m[p]);
            free(s_e[i].spv[p]);
        }
    openpete_mod_link_ctx_free(s_ctx);
    if (s_vk) link_vk_close();
    remove(s_log);
    judge_engagement();
    return s_fail ? 1 : 0;
}
