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
// Platform: POSIX compiles mods on load; Windows loads PREBUILT DLLs
// (mods/<id>/.build/<id>.dll, e.g. mingw cross-builds) until the
// bundled-toolchain story from openpete-distribution-local-recomp
// brings compile-on-load there too.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h> /* MinGW provides it too */
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
/* Windows: mods LOAD from prebuilt DLLs (mods/<id>/.build/<id>.dll —
 * cross-compiled with mingw or built on a dev box); compile-on-load
 * arrives with the bundled toolchain (openpete-distribution-local-
 * recomp). Everything else (manifest, deps, chains, services) is
 * identical to POSIX. */
#include <windows.h>
#include <direct.h>
#define MOD_DL_EXT ".dll"
#define mod_mkdir(p) _mkdir(p)
static void* mod_dlopen(const char* path) {
    return (void*)LoadLibraryA(path);
}
static void* mod_dlsym(void* h, const char* name) {
    union { FARPROC fp; void* p; } u;
    u.fp = GetProcAddress((HMODULE)h, name);
    return u.p;
}
static const char* mod_dlerror(void) {
    static char buf[64];
    snprintf(buf, sizeof buf, "LoadLibrary/GetProcAddress error %lu",
             (unsigned long)GetLastError());
    return buf;
}
#else
#include <dlfcn.h>
#define MOD_DL_EXT ".so"
#define mod_mkdir(p) mkdir((p), 0755)
static void* mod_dlopen(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void* mod_dlsym(void* h, const char* name) { return dlsym(h, name); }
static const char* mod_dlerror(void) { return dlerror(); }
#endif

#include <SDL3/SDL.h> /* SDL_GetBasePath — default mods/ + sdk/ location */

#include "openpete_psx.h"   /* openpete_psx_get_ram — api->guest() */
#include "openpete_state.h" /* savestate slots — engine services */

#include "../../third_party/tomlc99/toml.h"

/* mod_chain.c internals shared within core/mods. */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

#define MODS_MAX          32
#define MOD_ID_MAX        64
#define MOD_PATH_MAX      1024
#define MOD_SRCS_MAX      64
#define MOD_DEPS_MAX      8
#define MOD_EXPORTS_MAX   128

struct openpete_mod {
    char  id[MOD_ID_MAX];
    char  version[32];
    char  dir[MOD_PATH_MAX];
    char  data_dir[MOD_PATH_MAX + 8]; /* "<dir>/data", made on demand */
    char  log_tag[MOD_ID_MAX + 8]; /* "mod:<id>" */
    char  deps[MOD_DEPS_MAX][MOD_ID_MAX];
    int   n_deps;
    char  above[MOD_DEPS_MAX][MOD_ID_MAX]; /* advisory: must be HIGHER
                                            * priority (later in
                                            * enabled.txt) than these */
    int   n_above;
    char  below[MOD_DEPS_MAX][MOD_ID_MAX]; /* advisory: lower priority */
    int   n_below;
    int   sim_neutral; /* [mod] sim_neutral=true: author declares the mod
                        * never diverges gameplay state (observers/HUD/
                        * presentation) — excluded from the save modset
                        * tag. Advisory, like tier. */
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

static const openpete_sdk_symbol_t* sym_find(const char* name) {
    if (!name || !s_symbols) return NULL;
    size_t lo = 0, hi = s_symbol_count;
    while (lo < hi) { /* table is sorted by strcmp at generation time */
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s_symbols[mid].name, name);
        if (c == 0) return &s_symbols[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static uint32_t api_resolve(const char* name) {
    const openpete_sdk_symbol_t* s = sym_find(name);
    return s ? s->addr : 0;
}

/* Render-purity guard: present hooks may only draw. Presents are
 * non-deterministic in count/timing, so guest access / game calls /
 * registration from one would desync replay (plan: render-pure by API
 * shape AND enforcement). */
static int purity_violation(const char* what) {
    if (!openpete_mods_in_present) return 0;
    OPL_ERROR_F("mods", "%s refused inside a present hook (render-pure "
                "context) — move sim-affecting work to a tick hook", what);
    return 1;
}

static int api_override_addr(openpete_mod_t* self, uint32_t addr,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_addr")) return -1;
    int rc = openpete_mods_chain_add(addr, fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "override registered at 0x%08X", addr);
    return rc;
}

static int api_override_name(openpete_mod_t* self, const char* name,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_name")) return -1;
    const openpete_sdk_symbol_t* sym = sym_find(name);
    if (!sym) {
        OPL_ERROR_F(self->log_tag,
                    "override target \"%s\" not in the SDK symbol table",
                    name ? name : "(null)");
        return -1;
    }
    if (sym->legacy) {
        /* Decomp promotion contract: historical raw names resolve
         * forever, but say so — the stable name is the durable one. */
        OPL_WARN_F(self->log_tag,
                   "\"%s\" is a legacy raw alias (function has been "
                   "renamed) — resolved to 0x%08X; update the mod to the "
                   "stable name", name, sym->addr);
    }
    int rc = openpete_mods_chain_add(sym->addr, fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "override registered: %s (0x%08X)",
                   name, sym->addr);
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
    if (purity_violation("guest")) return NULL;
    uint32_t seg = vaddr >> 24;
    if (seg != 0x00u && seg != 0x80u && seg != 0xA0u) return NULL;
    uint32_t off = vaddr & 0x00FFFFFFu;
    if (off >= 2u * 1024u * 1024u) return NULL;
    uint8_t* ram = openpete_psx_get_ram();
    return ram ? ram + off : NULL;
}

/* MIPS kseg0 base — architectural, not a game address (the purity check
 * permits #define'd constants). */
#define OP_KSEG0_BASE 0x80000000u

static uint32_t api_guest_addr(const void* host) {
    if (!host) return 0;
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram) return 0;
    const uint8_t* p = (const uint8_t*)host;
    if (p < ram || p >= ram + 2u * 1024u * 1024u) return 0;
    return OP_KSEG0_BASE | (uint32_t)(p - ram);
}

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

static void api_call_addr(CPUState* cpu, uint32_t addr) {
    if (purity_violation("call_addr")) return;
    call_by_address(cpu, addr);
}

static int api_register_present_hook(openpete_mod_t* self,
                                     openpete_mod_present_fn fn) {
    if (!self) return -1;
    int rc = openpete_mods_register_present_hook(fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "present hook registered");
    return rc;
}

/* ---- engine services (P2 contract) ----------------------------------- */

static int api_savestate_request(openpete_mod_t* self, int op,
                                 unsigned slot) {
    if (!self) return -1;
    if (purity_violation("savestate_request")) return -1;
    return openpete_mods_savestate_request(op, slot, self->id);
}

static int api_savestate_slot_present(unsigned slot) {
    return openpete_state_slot_present(slot);
}

static int api_register_savestate_hooks(openpete_mod_t* self,
                                        openpete_mod_state_save_fn save,
                                        openpete_mod_state_load_fn load) {
    if (!self) return -1;
    int rc = openpete_mods_state_register(save, load, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "savestate participation registered");
    return rc;
}

static const char* api_data_dir(openpete_mod_t* self) {
    if (!self) return NULL;
    if (!self->data_dir[0]) {
        snprintf(self->data_dir, sizeof self->data_dir, "%s/data",
                 self->dir);
        mod_mkdir(self->data_dir); /* EEXIST fine */
    }
    return self->data_dir;
}

static int api_host_key_down(const char* key_name) {
    if (!key_name) return 0;
    SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    int nkeys = 0;
    const bool* state = SDL_GetKeyboardState(&nkeys);
    return (state && (int)sc < nkeys) ? (state[sc] ? 1 : 0) : 0;
}

/* Actor render service (rung 16). Capture reads sim state — tick
 * context only; draw is pure render output, legal in both contexts. */
static int api_capture_actor_pose(int kind, void* buf, int cap) {
    if (purity_violation("capture_actor_pose")) return -1;
    return openpete_mods_capture_actor_pose(kind, buf, cap);
}

static int api_draw_actor(int kind, const void* pose, int len,
                          uint32_t tint_rgba) {
    return openpete_mods_draw_actor(kind, pose, len, tint_rgba);
}

static void api_suppress_player_draw(void) {
    openpete_mods_suppress_player_draw();
}

/* Instruction-site hook tier (rung 14). Handlers run mid-sim (tick
 * semantics); attaching is entry-time only — present hooks are render-
 * pure and may not mutate the dispatch surface. */
static int api_site_handler(openpete_mod_t* self, const char* name,
                            void (*fn)(CPUState* cpu, uint32_t site_addr)) {
    if (!self || !name || !fn) return -1;
    if (purity_violation("site_handler")) return -1;
    return openpete_mods_site_attach(self->id, name, fn, self->log_tag);
}

/* Content registry: moby types (rung 11). Registration is entry-time;
 * spawning mutates sim state — tick context only. */
static int api_register_moby_type(openpete_mod_t* self, const char* name,
                                  int donor_class,
                                  openpete_moby_update_fn update_fn) {
    if (!self || !name) return -1;
    if (purity_violation("register_moby_type")) return -1;
    return openpete_mods_moby_register(self->id, name, donor_class,
                                       update_fn, self->log_tag);
}

static int api_moby_type_id(openpete_mod_t* self, const char* name) {
    if (!self || !name) return -1;
    return openpete_mods_moby_type_id(self->id, name);
}

static uint32_t api_spawn_moby(openpete_mod_t* self, CPUState* cpu,
                               const char* name) {
    if (!self || !cpu || !name) return 0;
    if (purity_violation("spawn_moby")) return 0;
    return openpete_mods_moby_spawn(cpu, self->id, name);
}

/* Save modset tag (per-modset save safety): the canonical identity of
 * the running gameplay-relevant modset — "id@ver+id@ver", ids sorted,
 * sim_neutral mods excluded. Empty string = vanilla. Consumed by the
 * host memcard module to tag save cards and warn on mismatch. */
const char* openpete_mods_modset_tag(void) {
    static char s_tag[1024];
    static int  s_built = 0;
    if (s_built) return s_tag;
    s_built = 1;
    s_tag[0] = '\0';
    /* Sorted id order → order-independent canonical form. */
    const struct openpete_mod* picked[MODS_MAX];
    int n = 0;
    for (int i = 0; i < s_mod_count; i++)
        if (s_mods[i].dl && !s_mods[i].sim_neutral)
            picked[n++] = &s_mods[i];
    for (int i = 1; i < n; i++) {
        const struct openpete_mod* m = picked[i];
        int j = i - 1;
        while (j >= 0 && strcmp(picked[j]->id, m->id) > 0) {
            picked[j + 1] = picked[j];
            j--;
        }
        picked[j + 1] = m;
    }
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(s_tag + off, sizeof s_tag - off, "%s%s@%s",
                         i ? "+" : "", picked[i]->id, picked[i]->version);
        if (w < 0 || (size_t)w >= sizeof s_tag - off) break;
        off += (size_t)w;
    }
    return s_tag;
}

/* Content registry: level slots (rung 12). */
static int api_register_level_slot(openpete_mod_t* self, const char* name,
                                   int donor_level_id) {
    if (!self || !name) return -1;
    if (purity_violation("register_level_slot")) return -1;
    return openpete_mods_level_register(self->id, name, donor_level_id,
                                        self->log_tag);
}

static int api_level_slot_id(openpete_mod_t* self, const char* name) {
    if (!self || !name) return -1;
    return openpete_mods_level_slot_id(self->id, name);
}

static int api_enter_level(openpete_mod_t* self, CPUState* cpu,
                           int level_id) {
    if (!self || !cpu) return -1;
    if (purity_violation("enter_level")) return -1;
    return openpete_mods_level_enter(cpu, level_id);
}

/* Cross-mod export registry: "modid:name" → pointer (rung 5). */
typedef struct {
    char  name[MOD_ID_MAX + 64];
    void* ptr;
} mod_export_t;

static mod_export_t s_exports[MOD_EXPORTS_MAX];
static int          s_export_count = 0;

static int api_export_sym(openpete_mod_t* self, const char* name, void* ptr) {
    if (!self || !name || !ptr) return -1;
    char q[sizeof s_exports[0].name];
    snprintf(q, sizeof q, "%s:%s", self->id, name);
    for (int i = 0; i < s_export_count; i++) {
        if (strcmp(s_exports[i].name, q) == 0) {
            OPL_ERROR_F(self->log_tag, "duplicate export \"%s\"", q);
            return -1;
        }
    }
    if (s_export_count >= MOD_EXPORTS_MAX) {
        OPL_ERROR_F(self->log_tag, "export table full (%d)", MOD_EXPORTS_MAX);
        return -1;
    }
    snprintf(s_exports[s_export_count].name,
             sizeof s_exports[s_export_count].name, "%s", q);
    s_exports[s_export_count].ptr = ptr;
    s_export_count++;
    OPL_INFO_F(self->log_tag, "exported \"%s\"", q);
    return 0;
}

static void* api_import_sym(openpete_mod_t* self, const char* qualified) {
    if (!qualified) return NULL;
    for (int i = 0; i < s_export_count; i++) {
        if (strcmp(s_exports[i].name, qualified) == 0)
            return s_exports[i].ptr;
    }
    OPL_DEBUG_F(self ? self->log_tag : "mod:?",
                "import \"%s\": not found", qualified);
    return NULL;
}

static const openpete_mod_api_t s_api = {
    .api_version   = OPENPETE_MOD_API_VERSION,
    .override_addr = api_override_addr,
    .override_name = api_override_name,
    .base          = openpete_mods_base,
    .resolve       = api_resolve,
    .log           = api_log,
    .guest         = api_guest,
    .export_sym    = api_export_sym,
    .import_sym    = api_import_sym,
    .guest_addr    = api_guest_addr,
    .call_addr     = api_call_addr,
    .draw_text     = openpete_mods_draw_text,
    .draw_rect     = openpete_mods_draw_rect,
    .register_present_hook = api_register_present_hook,
    .savestate_request        = api_savestate_request,
    .savestate_slot_present   = api_savestate_slot_present,
    .register_savestate_hooks = api_register_savestate_hooks,
    .data_dir      = api_data_dir,
    .host_key_down = api_host_key_down,
    .capture_actor_pose = api_capture_actor_pose,
    .draw_actor         = api_draw_actor,
    .suppress_player_draw = api_suppress_player_draw,
    .site_handler       = api_site_handler,
    .register_moby_type = api_register_moby_type,
    .moby_type_id       = api_moby_type_id,
    .spawn_moby         = api_spawn_moby,
    .register_level_slot = api_register_level_slot,
    .level_slot_id       = api_level_slot_id,
    .enter_level         = api_enter_level,
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
    /* sim_neutral = true: author-declared observer/presentation-only —
     * excluded from the save modset tag (per-modset save safety). */
    toml_datum_t sn = toml_bool_in(t, "sim_neutral");
    mod->sim_neutral = (sn.ok && sn.u.b) ? 1 : 0;
    /* Hard dependencies: deps = ["other-mod", ...]. Topo-sorted before
     * loading (a dep's entry runs before its dependents'); a missing or
     * cyclic dep disables the mod with an actionable error. */
    toml_array_t* deps = toml_array_in(t, "deps");
    if (deps) {
        int n = toml_array_nelem(deps);
        for (int i = 0; i < n && mod->n_deps < MOD_DEPS_MAX; i++) {
            toml_datum_t d = toml_string_at(deps, i);
            if (!d.ok) continue;
            snprintf(mod->deps[mod->n_deps++], MOD_ID_MAX, "%s", d.u.s);
            free(d.u.s);
        }
    }
    /* Advisory ordering relations (validated against the enabled-list
     * order, never silently reordered — the user's list is the truth;
     * violations produce an actionable "move this mod" error). */
    toml_array_t* above = toml_array_in(t, "above");
    if (above) {
        int n = toml_array_nelem(above);
        for (int i = 0; i < n && mod->n_above < MOD_DEPS_MAX; i++) {
            toml_datum_t d = toml_string_at(above, i);
            if (!d.ok) continue;
            snprintf(mod->above[mod->n_above++], MOD_ID_MAX, "%s", d.u.s);
            free(d.u.s);
        }
    }
    toml_array_t* below = toml_array_in(t, "below");
    if (below) {
        int n = toml_array_nelem(below);
        for (int i = 0; i < n && mod->n_below < MOD_DEPS_MAX; i++) {
            toml_datum_t d = toml_string_at(below, i);
            if (!d.ok) continue;
            snprintf(mod->below[mod->n_below++], MOD_ID_MAX, "%s", d.u.s);
            free(d.u.s);
        }
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
        "openpete_mod_api.h", "openpete_sdk_symbols.h",
        "openpete_sdk_wrappers.h", "psx_runtime.h",
    };
    for (size_t i = 0; i < sizeof sdk_inputs / sizeof sdk_inputs[0]; i++) {
        char p[MOD_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", sdk_dir, sdk_inputs[i]);
        time_t m = file_mtime(p);
        if (m > newest_input) newest_input = m;
    }

    char build_dir[MOD_PATH_MAX];
    snprintf(build_dir, sizeof build_dir, "%s/.build", mod->dir);
    mod_mkdir(build_dir); /* EEXIST is fine */

    snprintf(so_path, so_path_len, "%s/%s" MOD_DL_EXT, build_dir, mod->id);
    time_t so_m = file_mtime(so_path);
    if (so_m != (time_t)-1 && so_m >= newest_input) {
        OPL_DEBUG_F("mods", "%s: cached %s up to date", mod->id, so_path);
        return 0;
    }

#ifdef _WIN32
    /* No on-box compiler yet: a prebuilt DLL is required. A stale one
     * (older than the sources) still loads, loudly. */
    if (so_m != (time_t)-1) {
        OPL_WARN_F("mods", "%s: %s is OLDER than the mod sources — "
                   "loading it anyway (rebuild the DLL to update)",
                   mod->id, so_path);
        return 0;
    }
    OPL_ERROR_F("mods", "%s: no prebuilt %s — Windows loads prebuilt mod "
                "DLLs (compile-on-load arrives with the bundled "
                "toolchain); build one with e.g. x86_64-w64-mingw32-gcc "
                "-shared -O2 -I<sdk> — disabled", mod->id, so_path);
    return -1;
#else
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
#endif /* !_WIN32 */
}

typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);

static int load_mod(struct openpete_mod* mod, const char* sdk_dir) {
    /* Pure-asset mods (textures/music only) are legal: no src/ dir means
     * nothing to compile or dlopen — the manifest + assets/ root are the
     * whole mod. */
    {
        char src_dir[MOD_PATH_MAX + 8];
        snprintf(src_dir, sizeof src_dir, "%s/src", mod->dir);
        struct stat st;
        if (stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            OPL_INFO_F("mods", "loaded %s v%s (assets only)", mod->id,
                       mod->version);
            char assets[MOD_PATH_MAX + 8];
            snprintf(assets, sizeof assets, "%s/assets", mod->dir);
            if (stat(assets, &st) == 0 && S_ISDIR(st.st_mode))
                (void)openpete_mods_register_asset_root(assets, mod->id);
            return 0;
        }
    }
    char so_path[MOD_PATH_MAX];
    if (build_mod(mod, sdk_dir, so_path, sizeof so_path) != 0) return -1;

    mod->dl = mod_dlopen(so_path);
    if (!mod->dl) {
        OPL_ERROR_F("mods", "%s: dlopen failed: %s — disabled", mod->id,
                    mod_dlerror());
        return -1;
    }
    mod_entry_fn entry =
        (mod_entry_fn)mod_dlsym(mod->dl, "openpete_mod_entry");
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
    /* Asset overlay: a mod's assets/ dir joins the overlay stack in
     * enabled order (texture/music namespaces consume it). */
    {
        char assets[MOD_PATH_MAX + 8];
        snprintf(assets, sizeof assets, "%s/assets", mod->dir);
        struct stat st;
        if (stat(assets, &st) == 0 && S_ISDIR(st.st_mode))
            (void)openpete_mods_register_asset_root(assets, mod->id);
    }
    return 0;
}

/* ---- enabled-list resolution ---------------------------------------- */

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Dependency topo-sort (rung 5). DFS, stable in enabled order: a dep
 * loads before its dependents, so dependents keep HIGHER chain priority
 * than their libraries and a lib's exports exist when the client's
 * entry runs. Missing dep / cycle = actionable error, dependent
 * disabled, run continues. */
enum { TS_WHITE = 0, TS_GRAY, TS_BLACK, TS_FAILED };

typedef struct {
    struct openpete_mod* pending;
    int                  n_pending;
    int                  state[MODS_MAX];
    int                  topo[MODS_MAX];
    int                  n_topo;
    const char*          mods_dir;
} topo_ctx_t;

static int topo_find(const topo_ctx_t* c, const char* id) {
    for (int j = 0; j < c->n_pending; j++)
        if (strcmp(c->pending[j].id, id) == 0) return j;
    return -1;
}

static int topo_visit(topo_ctx_t* c, int i) { /* 0 = ok, -1 = failed */
    if (c->state[i] == TS_BLACK)  return 0;
    if (c->state[i] == TS_FAILED) return -1;
    if (c->state[i] == TS_GRAY) {
        OPL_ERROR_F("mods", "dependency cycle through \"%s\" — disabled "
                    "(break the cycle in the mods' mod.toml deps)",
                    c->pending[i].id);
        c->state[i] = TS_FAILED;
        return -1;
    }
    c->state[i] = TS_GRAY;
    for (int d = 0; d < c->pending[i].n_deps; d++) {
        int j = topo_find(c, c->pending[i].deps[d]);
        if (j < 0) {
            OPL_ERROR_F("mods", "\"%s\" requires \"%s\", which is not "
                        "enabled — add it to %s/enabled.txt (above "
                        "\"%s\") or install it; \"%s\" disabled",
                        c->pending[i].id, c->pending[i].deps[d],
                        c->mods_dir, c->pending[i].id, c->pending[i].id);
            c->state[i] = TS_FAILED;
            return -1;
        }
        if (topo_visit(c, j) != 0) {
            if (c->state[i] != TS_FAILED) {
                OPL_ERROR_F("mods", "\"%s\" disabled: its dependency "
                            "\"%s\" failed", c->pending[i].id,
                            c->pending[i].deps[d]);
                c->state[i] = TS_FAILED;
            }
            return -1;
        }
    }
    if (c->state[i] == TS_FAILED) return -1; /* cycle marked us */
    c->state[i] = TS_BLACK;
    c->topo[c->n_topo++] = i;
    return 0;
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

    /* Phase A: parse all enabled manifests (no loading yet). */
    struct openpete_mod pending[MODS_MAX];
    int n_pending = 0;
    for (int i = 0; i < n_order && n_pending < MODS_MAX; i++) {
        int known = 0;
        for (int j = 0; j < n_ids; j++)
            if (strcmp(order[i], ids[j]) == 0) known = 1;
        if (!known) {
            OPL_WARN_F("mods", "enabled.txt names \"%s\" but mods/%s/ has "
                       "no mod.toml — skipped", order[i], order[i]);
            continue;
        }
        struct openpete_mod* mod = &pending[n_pending];
        memset(mod, 0, sizeof(*mod));
        char dir[MOD_PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s", mods_dir, order[i]);
        if (parse_manifest(dir, mod) != 0) continue;
        n_pending++;
    }
    for (int i = 0; i < n_ids; i++) free(ids[i]);

    /* Phase A.5: validate advisory above/below relations against the
     * enabled order. Priority = position (later = higher), so
     * above=["x"] means "must be listed AFTER x". Violations disable the
     * DECLARING mod with a move-this-mod error; relations naming mods
     * that aren't enabled are vacuously satisfied (advisory, not dep). */
    {
        int keep = 0;
        for (int i = 0; i < n_pending; i++) {
            int ok_adv = 1;
            for (int a = 0; a < pending[i].n_above && ok_adv; a++) {
                for (int j = 0; j < n_pending; j++) {
                    if (strcmp(pending[j].id, pending[i].above[a]) != 0)
                        continue;
                    if (j > i) {
                        OPL_ERROR_F("mods", "\"%s\" declares "
                                    "above=[\"%s\"] (must run first): "
                                    "move \"%s\" AFTER \"%s\" in "
                                    "enabled.txt — disabled",
                                    pending[i].id, pending[j].id,
                                    pending[i].id, pending[j].id);
                        ok_adv = 0;
                    }
                    break;
                }
            }
            for (int b = 0; b < pending[i].n_below && ok_adv; b++) {
                for (int j = 0; j < n_pending; j++) {
                    if (strcmp(pending[j].id, pending[i].below[b]) != 0)
                        continue;
                    if (j < i) {
                        OPL_ERROR_F("mods", "\"%s\" declares "
                                    "below=[\"%s\"] (must run last): "
                                    "move \"%s\" BEFORE \"%s\" in "
                                    "enabled.txt — disabled",
                                    pending[i].id, pending[j].id,
                                    pending[i].id, pending[j].id);
                        ok_adv = 0;
                    }
                    break;
                }
            }
            if (ok_adv) {
                if (keep != i) pending[keep] = pending[i];
                keep++;
            }
        }
        n_pending = keep;
    }

    /* Phase B: topo-sort by hard deps (see topo_visit above). */
    topo_ctx_t tc;
    memset(&tc, 0, sizeof tc);
    tc.pending   = pending;
    tc.n_pending = n_pending;
    tc.mods_dir  = mods_dir;
    for (int i = 0; i < n_pending; i++) (void)topo_visit(&tc, i);

    /* Phase C: build + load in topo order; a load failure cascades to
     * dependents (their imports would dangle). */
    int loaded = 0;
    int load_ok[MODS_MAX] = {0};
    for (int t = 0; t < tc.n_topo; t++) {
        int i = tc.topo[t];
        int deps_ok = 1;
        for (int d = 0; d < pending[i].n_deps; d++) {
            int j = topo_find(&tc, pending[i].deps[d]);
            if (j < 0 || !load_ok[j]) deps_ok = 0;
        }
        if (!deps_ok) {
            OPL_ERROR_F("mods", "\"%s\" disabled: a dependency failed to "
                        "load", pending[i].id);
            continue;
        }
        if (s_mod_count >= MODS_MAX) break;
        struct openpete_mod* mod = &s_mods[s_mod_count];
        *mod = pending[i];
        if (load_mod(mod, sdk_dir) != 0) continue;
        load_ok[i] = 1;
        s_mod_count++;
        loaded++;
    }

    if (loaded > 0)
        OPL_INFO_F("mods", "%d mod%s active (dir: %s)", loaded,
                   loaded == 1 ? "" : "s", mods_dir);
    return loaded;
}

