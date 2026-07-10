// core/mods/mod_loader.c — discover, build, and load mods.
//
// P1 vertical slice of notes/plans/openpete-modding-framework.md:
//   mods/<id>/mod.toml     manifest ([mod] id/version/api)
//   mods/<id>/src/*.c      mod source, compiled on load
//   mods/enabled.txt       one mod id per line; order IS priority
//                          (later lines override earlier — S3AIR
//                          semantics). Missing file = all discovered
//                          mods, lexicographic order.
//
// Mods compile against <exe>/sdk/ (openpete_mod_api.h, psx_runtime.h,
// openpete_sdk_symbols.h — staged there by the build) into
// mods/<id>/.build/<id>.so, rebuilt when sources or the SDK are newer
// (mtime cache; content-hash keying arrives with the distribution
// toolchain). The shared object is dlopen'd and its
// openpete_mod_entry(api, self) is called to register overrides.
//
// Failure posture: a mod that fails to parse/build/load is disabled with
// an error log (compile log at mods/<id>/.build/compile.log) — mods
// never brick a launch. Zero mods loaded leaves the engine byte-
// identical to a modless build (openpete_mods_active == 0).
//
// Platform: POSIX for the vertical slice. Windows needs LoadLibrary +
// the bundled-toolchain story from openpete-distribution-local-recomp;
// stubbed until then.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <SDL3/SDL.h> /* SDL_GetBasePath — default mods/ + sdk/ location */

#include "openpete_psx.h" /* openpete_psx_get_ram — api->guest() */

#include "../../third_party/tomlc99/toml.h"

/* mod_chain.c internals shared within core/mods. */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

#define MODS_MAX          32
#define MOD_ID_MAX        64
#define MOD_PATH_MAX      1024
#define MOD_SRCS_MAX      64

struct openpete_mod {
    char  id[MOD_ID_MAX];
    char  version[32];
    char  dir[MOD_PATH_MAX];
    char  log_tag[MOD_ID_MAX + 8]; /* "mod:<id>" */
    void* dl;
};

static struct openpete_mod s_mods[MODS_MAX];
static int                 s_mod_count = 0;

static const openpete_sdk_symbol_t* s_symbols = NULL;
static size_t                       s_symbol_count = 0;

void openpete_mods_set_symbol_table(const openpete_sdk_symbol_t* table,
                                    size_t count) {
    s_symbols = table;
    s_symbol_count = count;
}

/* ---- mod-facing API impl ------------------------------------------- */

static uint32_t api_resolve(const char* name) {
    if (!name || !s_symbols) return 0;
    size_t lo = 0, hi = s_symbol_count;
    while (lo < hi) { /* table is sorted by strcmp at generation time */
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s_symbols[mid].name, name);
        if (c == 0) return s_symbols[mid].addr;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int api_override_addr(openpete_mod_t* self, uint32_t addr,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    int rc = openpete_mods_chain_add(addr, fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "override registered at 0x%08X", addr);
    return rc;
}

static int api_override_name(openpete_mod_t* self, const char* name,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    uint32_t addr = api_resolve(name);
    if (addr == 0) {
        OPL_ERROR_F(self->log_tag,
                    "override target \"%s\" not in the SDK symbol table",
                    name ? name : "(null)");
        return -1;
    }
    int rc = openpete_mods_chain_add(addr, fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "override registered: %s (0x%08X)",
                   name, addr);
    return rc;
}

static void api_log(openpete_mod_t* self, int level, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
    va_end(ap);
    if (level < OPL_TRACE || level > OPL_ERROR) level = OPL_INFO;
    openpete_logf((openpete_log_level_t)level,
                  self ? self->log_tag : "mod:?", "%s", msg);
}

static void* api_guest(uint32_t vaddr) {
    uint32_t seg = vaddr >> 24;
    if (seg != 0x00u && seg != 0x80u && seg != 0xA0u) return NULL;
    uint32_t off = vaddr & 0x00FFFFFFu;
    if (off >= 2u * 1024u * 1024u) return NULL;
    uint8_t* ram = openpete_psx_get_ram();
    return ram ? ram + off : NULL;
}

static const openpete_mod_api_t s_api = {
    .api_version   = OPENPETE_MOD_API_VERSION,
    .override_addr = api_override_addr,
    .override_name = api_override_name,
    .base          = openpete_mods_base,
    .resolve       = api_resolve,
    .log           = api_log,
    .guest         = api_guest,
};

/* ---- discovery / manifest / build / load --------------------------- */

static time_t file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return (time_t)-1;
    return st.st_mtime;
}

static int parse_manifest(const char* mod_dir, struct openpete_mod* mod) {
    char path[MOD_PATH_MAX];
    snprintf(path, sizeof path, "%s/mod.toml", mod_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;

    char errbuf[256];
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof errbuf);
    fclose(fp);
    if (!root) {
        OPL_ERROR_F("mods", "%s: TOML parse error: %s", path, errbuf);
        return -1;
    }

    int rc = -1;
    toml_table_t* t = toml_table_in(root, "mod");
    if (!t) {
        OPL_ERROR_F("mods", "%s: missing [mod] table", path);
        goto out;
    }
    toml_datum_t id = toml_string_in(t, "id");
    toml_datum_t ver = toml_string_in(t, "version");
    toml_datum_t api = toml_int_in(t, "api");
    if (!id.ok || !ver.ok) {
        OPL_ERROR_F("mods", "%s: [mod] requires id and version", path);
        if (id.ok) free(id.u.s);
        if (ver.ok) free(ver.u.s);
        goto out;
    }
    if (api.ok && api.u.i != (int64_t)OPENPETE_MOD_API_VERSION) {
        OPL_ERROR_F("mods", "%s: mod \"%s\" pins api %lld, engine has %u — "
                    "disabled", path, id.u.s, (long long)api.u.i,
                    OPENPETE_MOD_API_VERSION);
        free(id.u.s);
        free(ver.u.s);
        goto out;
    }
    snprintf(mod->id, sizeof mod->id, "%s", id.u.s);
    snprintf(mod->version, sizeof mod->version, "%s", ver.u.s);
    snprintf(mod->dir, sizeof mod->dir, "%s", mod_dir);
    snprintf(mod->log_tag, sizeof mod->log_tag, "mod:%s", mod->id);
    free(id.u.s);
    free(ver.u.s);
    /* Symbol-tier declaration: "named" (default) or "raw". Raw-tier
     * addresses (func_XXXXXXXX / bare vaddrs) are unstable across
     * decomp/recomp progress and pinned to this binary/region — warn so
     * breakage is a known trade, not a mystery (plan: two-tier symbol
     * stability). */
    toml_datum_t tier = toml_string_in(t, "tier");
    if (tier.ok) {
        if (strcmp(tier.u.s, "raw") == 0)
            OPL_WARN_F("mods", "%s declares raw-tier symbols: unstable "
                       "across engine/decomp updates and region-pinned",
                       mod->id);
        free(tier.u.s);
    }
    rc = 0;
out:
    toml_free(root);
    return rc;
}

/* Compile src/*.c → .build/<id>.so if sources or the SDK are newer.
 * Returns 0 with so_path filled on success. */
static int build_mod(const struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len) {
    char src_dir[MOD_PATH_MAX];
    snprintf(src_dir, sizeof src_dir, "%s/src", mod->dir);

    char srcs[MOD_SRCS_MAX][MOD_PATH_MAX];
    int n_srcs = 0;
    time_t newest_input = 0;

    DIR* d = opendir(src_dir);
    if (!d) {
        OPL_ERROR_F("mods", "%s: no src/ directory", mod->id);
        return -1;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n_srcs < MOD_SRCS_MAX) {
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 2, ".c") != 0) continue;
        snprintf(srcs[n_srcs], MOD_PATH_MAX, "%s/%s", src_dir, e->d_name);
        time_t m = file_mtime(srcs[n_srcs]);
        if (m > newest_input) newest_input = m;
        n_srcs++;
    }
    closedir(d);
    if (n_srcs == 0) {
        OPL_ERROR_F("mods", "%s: no .c sources in src/", mod->id);
        return -1;
    }

    /* SDK headers count as inputs: an SDK regen rebuilds every mod. */
    static const char* const sdk_inputs[] = {
        "openpete_mod_api.h", "openpete_sdk_symbols.h", "psx_runtime.h",
    };
    for (size_t i = 0; i < sizeof sdk_inputs / sizeof sdk_inputs[0]; i++) {
        char p[MOD_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", sdk_dir, sdk_inputs[i]);
        time_t m = file_mtime(p);
        if (m > newest_input) newest_input = m;
    }

    char build_dir[MOD_PATH_MAX];
    snprintf(build_dir, sizeof build_dir, "%s/.build", mod->dir);
    mkdir(build_dir, 0755); /* EEXIST is fine */

    snprintf(so_path, so_path_len, "%s/%s.so", build_dir, mod->id);
    time_t so_m = file_mtime(so_path);
    if (so_m != (time_t)-1 && so_m >= newest_input) {
        OPL_DEBUG_F("mods", "%s: cached %s up to date", mod->id, so_path);
        return 0;
    }

    const char* cc = getenv("OPENPETE_MOD_CC");
    if (!cc || !cc[0]) cc = getenv("CC");
    if (!cc || !cc[0]) cc = "cc";

    char cmd[8192];
    int off = snprintf(cmd, sizeof cmd,
                       "'%s' -shared -fPIC -O2 -g -fno-strict-aliasing "
                       "-Wall -I'%s' -o '%s'",
                       cc, sdk_dir, so_path);
    for (int i = 0; i < n_srcs && off < (int)sizeof cmd; i++)
        off += snprintf(cmd + off, sizeof cmd - (size_t)off, " '%s'",
                        srcs[i]);
    snprintf(cmd + off, sizeof cmd - (size_t)off,
             " >'%s/compile.log' 2>&1", build_dir);

    OPL_INFO_F("mods", "%s: compiling %d source file%s", mod->id, n_srcs,
               n_srcs == 1 ? "" : "s");
    int rc = system(cmd);
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s: compile failed (see %s/compile.log) — "
                    "disabled", mod->id, build_dir);
        remove(so_path);
        return -1;
    }
    return 0;
}

typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);

static int load_mod(struct openpete_mod* mod, const char* sdk_dir) {
    char so_path[MOD_PATH_MAX];
    if (build_mod(mod, sdk_dir, so_path, sizeof so_path) != 0) return -1;

    mod->dl = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!mod->dl) {
        OPL_ERROR_F("mods", "%s: dlopen failed: %s — disabled", mod->id,
                    dlerror());
        return -1;
    }
    mod_entry_fn entry =
        (mod_entry_fn)dlsym(mod->dl, "openpete_mod_entry");
    if (!entry) {
        OPL_ERROR_F("mods", "%s: no openpete_mod_entry symbol — disabled",
                    mod->id);
        return -1; /* keep the .so resident; it is inert without chains */
    }
    int rc = entry(&s_api, mod);
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s: openpete_mod_entry returned %d — disabled",
                    mod->id, rc);
        return -1;
    }
    OPL_INFO_F("mods", "loaded %s v%s (%s)", mod->id, mod->version,
               so_path);
    return 0;
}

/* ---- enabled-list resolution ---------------------------------------- */

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int openpete_mods_init(const openpete_options_t* opts) {
    char mods_dir[MOD_PATH_MAX];
    const char* src = opts && opts->mods_dir ? opts->mods_dir
                                             : getenv("OPENPETE_MODS_DIR");
    if (src) {
        snprintf(mods_dir, sizeof mods_dir, "%s", src);
    } else {
        const char* base = SDL_GetBasePath(); /* trailing sep, SDL-owned */
        if (!base) return 0;
        snprintf(mods_dir, sizeof mods_dir, "%smods", base);
    }

    DIR* d = opendir(mods_dir);
    if (!d) return 0; /* no mods directory: inert */

    /* Discover candidate ids (subdirs containing mod.toml). */
    char* ids[MODS_MAX];
    int n_ids = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n_ids < MODS_MAX) {
        if (e->d_name[0] == '.') continue;
        char manifest[MOD_PATH_MAX];
        snprintf(manifest, sizeof manifest, "%s/%s/mod.toml", mods_dir,
                 e->d_name);
        if (file_mtime(manifest) == (time_t)-1) continue;
        ids[n_ids++] = strdup(e->d_name);
    }
    closedir(d);
    if (n_ids == 0) return 0;

    /* enabled.txt (one id per line, '#' comments) fixes the order; the
     * order IS the priority (later = higher). Fallback: lexicographic. */
    char order[MODS_MAX][MOD_ID_MAX];
    int n_order = 0;
    char en_path[MOD_PATH_MAX];
    snprintf(en_path, sizeof en_path, "%s/enabled.txt", mods_dir);
    FILE* en = fopen(en_path, "r");
    if (en) {
        char line[MOD_ID_MAX];
        while (n_order < MODS_MAX && fgets(line, sizeof line, en)) {
            line[strcspn(line, "\r\n")] = 0;
            char* s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            snprintf(order[n_order++], MOD_ID_MAX, "%s", s);
        }
        fclose(en);
    } else {
        qsort(ids, (size_t)n_ids, sizeof ids[0], cmp_str);
        for (int i = 0; i < n_ids; i++)
            snprintf(order[n_order++], MOD_ID_MAX, "%s", ids[i]);
    }

    char sdk_dir[MOD_PATH_MAX];
    {
        const char* base = SDL_GetBasePath();
        snprintf(sdk_dir, sizeof sdk_dir, "%ssdk", base ? base : "./");
    }

    int loaded = 0;
    for (int i = 0; i < n_order; i++) {
        int known = 0;
        for (int j = 0; j < n_ids; j++)
            if (strcmp(order[i], ids[j]) == 0) known = 1;
        if (!known) {
            OPL_WARN_F("mods", "enabled.txt names \"%s\" but mods/%s/ has "
                       "no mod.toml — skipped", order[i], order[i]);
            continue;
        }
        if (s_mod_count >= MODS_MAX) break;
        struct openpete_mod* mod = &s_mods[s_mod_count];
        memset(mod, 0, sizeof(*mod));
        char dir[MOD_PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s", mods_dir, order[i]);
        if (parse_manifest(dir, mod) != 0) continue;
        if (load_mod(mod, sdk_dir) != 0) continue;
        s_mod_count++;
        loaded++;
    }

    for (int i = 0; i < n_ids; i++) free(ids[i]);
    if (loaded > 0)
        OPL_INFO_F("mods", "%d mod%s active (dir: %s)", loaded,
                   loaded == 1 ? "" : "s", mods_dir);
    return loaded;
}

#else /* _WIN32 */

void openpete_mods_set_symbol_table(const openpete_sdk_symbol_t* table,
                                    size_t count) {
    (void)table;
    (void)count;
}

int openpete_mods_init(const openpete_options_t* opts) {
    (void)opts;
    /* Windows mod loading arrives with the bundled-toolchain work in
     * openpete-distribution-local-recomp (LoadLibrary + clang). */
    return 0;
}

#endif /* _WIN32 */
