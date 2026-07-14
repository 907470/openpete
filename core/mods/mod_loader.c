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
// mods/<id>/.build/<id>.so, rebuilt when the CONTENT-HASH build key
// (api version + compiler identity + SDK headers + sources) differs
// from the stamped one — mtime churn never forces a rebuild. Mods may
// also ship as <id>.zip (unpacked to .unpacked/<id>/, content-stamped;
// "folder or zip, identical"). The shared object is dlopen'd and its
// openpete_mod_entry(api, self) is called to register overrides.
//
// Failure posture: a mod that fails to parse/build/load is disabled with
// an error log (compile log at mods/<id>/.build/compile.log) — mods
// never brick a launch. Zero mods loaded leaves the engine byte-
// identical to a modless build (openpete_mods_active == 0).
//
// Platform (openpete-mod-compile-on-boot): every platform compiles
// src/ mods on load — Windows resolves the bundled llvm-mingw the
// distribution package ships (recomp/tools/llvm-mingw), falling back to
// a prebuilt mods/<id>/.build/<id>.dll only when no compiler exists. A
// mod with NO src/ but a prebuilt .build/<id>.{so,dll} is a binary-only
// mod: loaded as-is, never hashed or compiled. openpete_mods_prepare()
// lets the game compile stale mods behind its splash (progress UI)
// before openpete_mods_init() loads them from the warmed cache.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "../host/sha256.h"     /* content-hash build cache key */
#include "../host/host_spawn.h" /* silent compiler spawn + cc identity */
#include "miniz.h"              /* zip mods (folder or zip, identical) */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h> /* MinGW provides it too */
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MOD_DL_EXT ".dll"
#define MOD_Q "\""  /* cmd.exe argument quoting */
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
#define MOD_Q "'"   /* POSIX shell argument quoting */
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
#define MOD_BINDINGS_MAX  8

typedef struct {
    char name[24];    /* binding_down() key within the mod's namespace */
    char key[24];     /* host key name (host_key_down vocabulary) */
    char purpose[64]; /* human text for conflict messages / mod menu */
} mod_binding_t;
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
    mod_binding_t bindings[MOD_BINDINGS_MAX]; /* declared host keys */
    int   n_bindings;
    char  build_key[65]; /* content-hash build key (hot-reload watch) */
    char  cfg_key[65];   /* config.toml content hash (hot-reload watch;
                          * a config edit re-enters entry without a
                          * recompile — the cached object is reused) */
    char  shader_key[65];/* <mod>/shaders/ content hash (hot-reload watch;
                          * a shader-only edit reloads against the cached
                          * C object — the per-shader .spv stamps decide
                          * what actually recompiles) */
    int   has_src;       /* 0 = binary-only/assets-only: never compiled,
                          * skipped by the hot-reload watch */
    int   enabled;       /* live-toggle state (loaded mods start on) */
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
    if (off >= 2u * 1024u * 1024u) {
        /* Expansion window: registered regions (the mod guest arena,
         * relocated buffers) resolve through the runtime's cold path. */
        return openpete_psx_to_host_ptr(vaddr);
    }
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

static int api_on_level(openpete_mod_t* self, openpete_mod_level_fn fn) {
    if (!self || !fn) return -1;
    if (purity_violation("on_level")) return -1;
    return openpete_mods_on_level(self->id, fn, self->log_tag);
}

static int api_sfx_def_replace(openpete_mod_t* self, uint16_t model_class,
                               uint8_t sound_slot, const void* body,
                               uint32_t len) {
    if (!self) return -1;
    if (purity_violation("sfx_def_replace")) return -1;
    return openpete_mods_sfx_def_replace(self, model_class, sound_slot,
                                         body, len);
}

static uint32_t api_guest_alloc_low(openpete_mod_t* self, uint32_t size,
                                    uint32_t align, void** host_view) {
    if (!self) return 0;
    if (purity_violation("guest_alloc_low")) return 0;
    return openpete_mods_guest_alloc_low(size, align, host_view, self->id);
}

static char s_sdk_dir[MOD_PATH_MAX];

/* defined later in this file */
static int parse_manifest(const char* mod_dir, struct openpete_mod* mod);
static void exports_remove_owner(const char* id);

/* Hot reload (P4, dev mode): drop a mod's cross-mod exports; importers
 * hold raw pointers into the OLD object, which stays resident (never
 * dlclose mid-session), so stale imports keep working against stale
 * code until their owners also reload — logged so nobody is surprised. */

static int build_mod(struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len);
static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]);
static int config_compute_key(const char* mod_dir, char key_out[65]);
static int mod_compile(const char* mod_dir, const char* id,
                       const char* sdk_dir, const char* key,
                       const char* out_path, int stamp);
typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);
static const openpete_mod_api_t s_api;

/* Reload one mod in place at a tick boundary: capture its state (the
 * savestate-participation pair), sweep every registration it owns,
 * re-parse + rebuild + dlopen a FRESH object (unique .hotN name — the
 * old object stays resident by design), re-run entry, restore state. */
static void mod_hot_reload(struct openpete_mod* mod) {
    OPL_INFO_F("mods", "hot reload: %s (sources changed)", mod->id);
    static unsigned char blob[64 * 1024];
    int blen = openpete_mods_state_capture(mod->id, blob, sizeof blob);

    openpete_mods_chain_remove_owner(mod->id);
    openpete_mods_draw_remove_owner(mod->id);
    openpete_mods_site_detach_all(mod->id);
    openpete_mods_registry_remove_owner(mod->id);
    openpete_mods_state_remove(mod->id);
    openpete_mods_shader_remove_owner(mod->id);
    exports_remove_owner(mod->id);

    mod->n_deps = mod->n_above = mod->n_below = mod->n_bindings = 0;
    char dir_copy[MOD_PATH_MAX];
    snprintf(dir_copy, sizeof dir_copy, "%s", mod->dir);
    if (parse_manifest(dir_copy, mod) != 0) {
        OPL_ERROR_F("mods", "%s: reload manifest parse failed — mod is "
                    "now INERT until the next successful reload", mod->id);
        return;
    }
    static unsigned s_hot_n = 0;
    char hot_path[MOD_PATH_MAX + 32];
    snprintf(hot_path, sizeof hot_path, "%s/.build/%s.hot%u" MOD_DL_EXT,
             mod->dir, mod->id, ++s_hot_n);
#ifdef _WIN32
    /* The boot-loaded base DLL is file-locked on Windows: compile
     * straight to the unique .hotN name and skip the stamp — the base
     * DLL goes stale, so the next boot recompiles it once and
     * re-stamps. */
    char key[65];
    if (mod_compute_key(mod->dir, s_sdk_dir, key) != 0 ||
        mod_compile(mod->dir, mod->id, s_sdk_dir, key, hot_path, 0) != 0) {
        OPL_ERROR_F("mods", "%s: reload compile failed — mod is now "
                    "INERT until the next successful reload", mod->id);
        return;
    }
    snprintf(mod->build_key, sizeof mod->build_key, "%s", key);
#else
    char so_path[MOD_PATH_MAX];
    if (build_mod(mod, s_sdk_dir, so_path, sizeof so_path) != 0) {
        OPL_ERROR_F("mods", "%s: reload compile failed — mod is now "
                    "INERT until the next successful reload", mod->id);
        return;
    }
    {   /* copy: a fresh inode guarantees dlopen loads a fresh object */
        FILE* in = fopen(so_path, "rb");
        FILE* outf = in ? fopen(hot_path, "wb") : NULL;
        if (!in || !outf) {
            if (in) fclose(in);
            if (outf) fclose(outf);
            OPL_ERROR_F("mods", "%s: reload stage copy failed", mod->id);
            return;
        }
        unsigned char cbuf[8192];
        size_t got;
        while ((got = fread(cbuf, 1, sizeof cbuf, in)) > 0)
            fwrite(cbuf, 1, got, outf);
        fclose(in);
        fclose(outf);
    }
#endif
    void* dl = mod_dlopen(hot_path);
    if (!dl) {
        OPL_ERROR_F("mods", "%s: reload dlopen failed: %s", mod->id,
                    mod_dlerror());
        return;
    }
    mod_entry_fn entry = (mod_entry_fn)mod_dlsym(dl, "openpete_mod_entry");
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
    if (!entry || entry(&s_api, mod) != 0) {
        OPL_ERROR_F("mods", "%s: reload entry failed — mod is now INERT",
                    mod->id);
        return;
    }
    mod->dl = dl; /* old object deliberately stays resident */
    if (blen >= 0)
        openpete_mods_state_restore(mod->id, blob, blen);
    OPL_INFO_F("mods", "hot reload: %s live (state %s)", mod->id,
               blen >= 0 ? "carried" : "fresh");
}

/* Watch: every 64 canonical ticks, recompute each loaded mod's content
 * build key; a change (source OR SDK) reloads it. OPENPETE_MOD_HOT_RELOAD=1
 * only — this is dev-mode machinery. */
static void hot_reload_poll(void) {
    static int s_hot = -1;
    if (s_hot < 0) {
        const char* e = getenv("OPENPETE_MOD_HOT_RELOAD");
        s_hot = (e && *e && *e != '0') ? 1 : 0;
        if (s_hot) OPL_INFO_F("mods", "hot reload armed (dev mode)");
    }
    if (!s_hot) return;
    static unsigned s_cnt = 0;
    if (++s_cnt % 64u) return;
    for (int i = 0; i < s_mod_count; i++) {
        struct openpete_mod* mod = &s_mods[i];
        if (!mod->dl || !mod->has_src) continue;
        /* Key-only recompute (pure hashing, ms); the compile happens
         * inside mod_hot_reload on a real change. config.toml is watched
         * too — a config-only edit reloads against the cached object, so
         * entry re-reads the fresh values. */
        char key[65], ckey[65], skey[65];
        if (mod_compute_key(mod->dir, s_sdk_dir, key) != 0)
            continue;
        (void)config_compute_key(mod->dir, ckey);
        (void)openpete_mods_shader_watch_key(mod->dir, skey);
        if (strcmp(key, mod->build_key) != 0 ||
            strcmp(ckey, mod->cfg_key) != 0 ||
            strcmp(skey, mod->shader_key) != 0)
            mod_hot_reload(mod);
    }
}

/* ── Live modset toggle (P2) ─────────────────────────────────────────
 * enable/disable apply at the NEXT TICK BOUNDARY (the canonical present
 * commit), never mid-tick: requests queue here and the apply flips
 * owner-keyed disabled flags across every registration surface — chain
 * links (skipped transparently, base() nesting preserved), present
 * hooks, baked instruction sites (guarded identity arm), and registry
 * rows. Nothing is torn down, so re-enable restores the mod exactly
 * where it was. Asset overlays stay boot-scoped ("restart tier" in the
 * plan's language — their consumers scan at init).
 *
 * The save modset tag deliberately does NOT rebuild on toggle: a
 * divergent mod that already ran this session has already diverged it;
 * the session tag is the loaded set, not the instantaneous one.
 *
 * Drivers: the future mod menu; and OPENPETE_MODS_TOGGLE=
 * "tick:id:0|1[,...]" (a canonical-tick schedule) for CI/dev. */

void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on);
void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on);

typedef struct { char id[MOD_ID_MAX]; int on; } toggle_req_t;
static toggle_req_t s_toggle_q[16];
static int          s_toggle_n = 0;

typedef struct { unsigned tick; char id[MOD_ID_MAX]; int on; } toggle_sched_t;
static toggle_sched_t s_toggle_sched[16];
static int            s_toggle_sched_n = 0;

int         openpete_mods_count(void) { return s_mod_count; }
const char* openpete_mods_id(int i) {
    return (i >= 0 && i < s_mod_count) ? s_mods[i].id : NULL;
}
int openpete_mods_is_enabled(const char* id) {
    for (int i = 0; i < s_mod_count; i++)
        if (strcmp(s_mods[i].id, id) == 0) return s_mods[i].enabled;
    return 0;
}

int openpete_mods_set_mod_enabled(const char* id, int on) {
    if (!id || !id[0]) return -1;
    if (s_toggle_n >= (int)(sizeof s_toggle_q / sizeof s_toggle_q[0]))
        return -1;
    snprintf(s_toggle_q[s_toggle_n].id, MOD_ID_MAX, "%s", id);
    s_toggle_q[s_toggle_n].on = on ? 1 : 0;
    s_toggle_n++;
    return 0;
}

static void toggle_parse_env(void) {
    const char* e = getenv("OPENPETE_MODS_TOGGLE");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        unsigned tick;
        char id[MOD_ID_MAX];
        int on;
        if (sscanf(tok, "%u:%63[^:]:%d", &tick, id, &on) == 3 &&
            s_toggle_sched_n < (int)(sizeof s_toggle_sched /
                                     sizeof s_toggle_sched[0])) {
            toggle_sched_t* t = &s_toggle_sched[s_toggle_sched_n++];
            t->tick = tick;
            snprintf(t->id, MOD_ID_MAX, "%s", id);
            t->on = on ? 1 : 0;
        } else {
            OPL_ERROR_F("mods", "OPENPETE_MODS_TOGGLE: bad token \"%s\" "
                        "(want tick:id:0|1)", tok);
        }
    }
    if (s_toggle_sched_n)
        OPL_INFO_F("mods", "toggle schedule armed (%d entr%s)",
                   s_toggle_sched_n, s_toggle_sched_n == 1 ? "y" : "ies");
}

void openpete_mods_tick_boundary(unsigned canonical_tick) {
    for (int i = 0; i < s_toggle_sched_n; i++) {
        if (s_toggle_sched[i].tick == canonical_tick)
            (void)openpete_mods_set_mod_enabled(s_toggle_sched[i].id,
                                                s_toggle_sched[i].on);
    }
    hot_reload_poll();

    for (int q = 0; q < s_toggle_n; q++) {
        const char* id = s_toggle_q[q].id;
        int on = s_toggle_q[q].on;
        struct openpete_mod* mod = NULL;
        for (int i = 0; i < s_mod_count; i++)
            if (strcmp(s_mods[i].id, id) == 0) { mod = &s_mods[i]; break; }
        if (!mod) {
            OPL_ERROR_F("mods", "live toggle: no loaded mod \"%s\"", id);
            continue;
        }
        openpete_mods_chain_set_owner_enabled(id, on);
        openpete_mods_draw_set_owner_enabled(id, on);
        openpete_mods_site_set_enabled(id, on);
        openpete_mods_registry_set_owner_enabled(id, on);
        mod->enabled = on;
        OPL_INFO_F("mods", "live toggle: %s %s (tick %u)",
                   id, on ? "ENABLED" : "DISABLED", canonical_tick);
    }
    s_toggle_n = 0;
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

/* Declared-binding resolve: name → this mod's [[binding]] key →
 * host_key_down. Same tick-context caveats as host_key_down itself. */
static int api_binding_down(openpete_mod_t* self, const char* name) {
    if (!self || !name) return 0;
    for (int i = 0; i < self->n_bindings; i++)
        if (strcmp(self->bindings[i].name, name) == 0)
            return api_host_key_down(self->bindings[i].key);
    OPL_ERROR_F(self->log_tag, "binding \"%s\" not declared in mod.toml",
                name);
    return 0;
}

/* Guest scratch arena: sim-visible state — tick context only. */
static uint32_t api_guest_alloc(openpete_mod_t* self, uint32_t size,
                                uint32_t align, void** host_view) {
    if (host_view) *host_view = NULL;
    if (!self) return 0;
    if (purity_violation("guest_alloc")) return 0;
    return openpete_mods_guest_alloc(size, align, host_view, self->id);
}

/* bk2 status: read-only mirror published by run.c per sim frame. */
static void api_bk2_status(openpete_mod_t* self, int* active, int* frame,
                           int* frame_count, int* overriding) {
    (void)self;
    openpete_mods_bk2_status_t st;
    openpete_mods_bk2_query(&st);
    if (active)      *active      = st.active;
    if (frame)       *frame       = st.frame;
    if (frame_count) *frame_count = st.frame_count;
    if (overriding)  *overriding  = st.overriding;
}

/* One-shot sound: host-side additive — no purity gate (sim-neutral by
 * construction; presentation, not render output). */
static int api_sfx_play(openpete_mod_t* self, const char* name,
                          float gain) {
    if (!self || !name || strstr(name, "..")) return -1;
    char path[MOD_PATH_MAX + 96];
    snprintf(path, sizeof path, "%s/assets/sounds/%s.wav", self->dir, name);
    return openpete_mod_sound_play(path, gain);
}

/* Cross-mod export registry: "modid:name" → pointer (rung 5). */
typedef struct {
    char  name[MOD_ID_MAX + 64];
    void* ptr;
} mod_export_t;

static mod_export_t s_exports[MOD_EXPORTS_MAX];
static int          s_export_count = 0;

static void exports_remove_owner(const char* id) {
    char prefix[MOD_ID_MAX + 2];
    snprintf(prefix, sizeof prefix, "%s:", id);
    int w = 0, removed = 0;
    for (int i = 0; i < s_export_count; i++) {
        if (strncmp(s_exports[i].name, prefix, strlen(prefix)) == 0) {
            removed++;
            continue;
        }
        s_exports[w++] = s_exports[i];
    }
    s_export_count = w;
    if (removed)
        OPL_WARN_F("mods", "%s: %d export(s) dropped on reload — other "
                   "mods' existing imports now reference the OLD object",
                   id, removed);
}


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

const char* openpete_mods_id_of(openpete_mod_t* self) {
    return self ? self->id : NULL;
}

const char* openpete_mods_dir_of(openpete_mod_t* self) {
    return self ? self->dir : NULL;
}

const char* openpete_mods_sdk_dir(void) {
    return s_sdk_dir;
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
    .sfx_play            = api_sfx_play,
    .binding_down        = api_binding_down,
    .guest_alloc         = api_guest_alloc,
    .bk2_status          = api_bk2_status,
    .sfx_replace   = openpete_mods_sfx_replace,
    .sfx_hook      = openpete_mods_sfx_hook,
    .texture_register = openpete_mods_texture_register,
    .sfx_stage     = openpete_mods_sfx_stage,
    .config_bool   = openpete_mods_config_bool,
    .config_int    = openpete_mods_config_int,
    .config_float  = openpete_mods_config_float,
    .config_str    = openpete_mods_config_str,
    .on_level        = api_on_level,
    .sfx_def_replace = api_sfx_def_replace,
    .guest_alloc_low = api_guest_alloc_low,
    .shader_register     = openpete_mods_shader_register,
    .postfx_register     = openpete_mods_postfx_register,
    .postfx_enable       = openpete_mods_postfx_enable,
    .postfx_set_uniforms = openpete_mods_postfx_set_uniforms,
    .pipeline_register   = openpete_mods_pipeline_register,
    .draw_mesh           = openpete_mods_draw_mesh,
    .material_register   = openpete_mods_material_register,
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
    /* Declared input bindings ([[binding]] name/key/purpose): the
     * registry that makes host-key use visible — conflicts across the
     * enabled set are detected after loading (advisory warn; reads are
     * non-exclusive), and a future mod menu rebinds by editing the
     * registry, not the mod. */
    toml_array_t* binds = toml_array_in(root, "binding");
    if (binds) {
        int n = toml_array_nelem(binds);
        for (int i = 0; i < n && mod->n_bindings < MOD_BINDINGS_MAX; i++) {
            toml_table_t* b = toml_table_at(binds, i);
            if (!b) continue;
            toml_datum_t bn = toml_string_in(b, "name");
            toml_datum_t bk = toml_string_in(b, "key");
            toml_datum_t bp = toml_string_in(b, "purpose");
            if (bn.ok && bk.ok) {
                mod_binding_t* mb = &mod->bindings[mod->n_bindings++];
                snprintf(mb->name, sizeof mb->name, "%s", bn.u.s);
                snprintf(mb->key, sizeof mb->key, "%s", bk.u.s);
                snprintf(mb->purpose, sizeof mb->purpose, "%s",
                         bp.ok ? bp.u.s : "");
            } else {
                OPL_ERROR_F("mods", "%s: [[binding]] needs name and key",
                            path);
            }
            if (bn.ok) free(bn.u.s);
            if (bk.ok) free(bk.u.s);
            if (bp.ok) free(bp.u.s);
        }
    }
    rc = 0;
out:
    toml_free(root);
    return rc;
}

/* Hash a file's contents into `c`; returns 0 ok, -1 missing. */
static int hash_file(openpete_sha256_ctx* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        openpete_sha256_update(c, buf, got);
    fclose(f);
    return 0;
}

/* insertion-sort src paths so the build key is order-independent
 * (readdir order is filesystem-dependent). */
static void sort_srcs(char srcs[][MOD_PATH_MAX], int n) {
    for (int i = 1; i < n; i++) {
        char tmp[MOD_PATH_MAX];
        memcpy(tmp, srcs[i], MOD_PATH_MAX);
        int j = i - 1;
        while (j >= 0 && strcmp(srcs[j], tmp) > 0) {
            memcpy(srcs[j + 1], srcs[j], MOD_PATH_MAX);
            j--;
        }
        memcpy(srcs[j + 1], tmp, MOD_PATH_MAX);
    }
}

/* The compiler for mod builds. OPENPETE_MOD_CC is the explicit dev
 * override; under it, the bundled toolchain the distribution package
 * stages next to the exe (same paths as the first-boot module build in
 * recomp_build.cpp) is the zero-config default; then the module build's
 * env override; then the system compiler (POSIX only — Windows has
 * none, "" = fall back to a prebuilt DLL). */
static void mods_resolve_cc(char* cc, size_t cap) {
    const char* env = getenv("OPENPETE_MOD_CC");
    if (env && env[0]) {
        snprintf(cc, cap, "%s", env);
        return;
    }
    {
        const char* base = SDL_GetBasePath(); /* trailing sep, SDL-owned */
#ifdef _WIN32
        static const char* const rel = "recomp/tools/llvm-mingw/bin/"
                                       "x86_64-w64-mingw32-clang.exe";
#else
        static const char* const rel = "recomp/tools/clang/bin/clang";
#endif
        char bundled[MOD_PATH_MAX];
        snprintf(bundled, sizeof bundled, "%s%s", base ? base : "./", rel);
        if (file_mtime(bundled) != (time_t)-1) {
            snprintf(cc, cap, "%s", bundled);
            return;
        }
    }
    env = getenv("OPENPETE_CC");
    if (env && env[0]) {
        snprintf(cc, cap, "%s", env);
        return;
    }
#ifdef _WIN32
    cc[0] = '\0';
#else
    env = getenv("CC");
    snprintf(cc, cap, "%s", env && env[0] ? env : "cc");
#endif
}

/* First line of `<cc> --version`, cached per process — folded into the
 * build key so a toolchain bump (or an author's stamp from a different
 * box) invalidates the cache instead of silently reusing an object
 * built by a different compiler. "" if the probe fails (degrades to
 * source+SDK keying, the prior behavior). */
static const char* mods_cc_identity(void) {
    static int  s_probed = 0;
    static char s_identity[256];
    if (s_probed) return s_identity;
    s_probed = 1;
    char cc[MOD_PATH_MAX];
    mods_resolve_cc(cc, sizeof cc);
    if (!cc[0]) return s_identity;
    char cmd[MOD_PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, MOD_Q "%s" MOD_Q " --version 2>&1", cc);
    char out[1024];
    if (openpete_host_spawn_capture(cmd, out, sizeof out) < 0) return s_identity;
    out[strcspn(out, "\r\n")] = '\0';
    snprintf(s_identity, sizeof s_identity, "%s", out);
    return s_identity;
}

/* Enumerate + sort src/*.c. Returns the count, 0 if src/ has no .c
 * files, or -1 if there is no src/ directory at all (quiet — callers
 * decide whether that's binary-only, assets-only, or an error). */
static int mod_list_srcs(const char* mod_dir,
                         char srcs[][MOD_PATH_MAX]) {
    char src_dir[MOD_PATH_MAX];
    snprintf(src_dir, sizeof src_dir, "%s/src", mod_dir);
    DIR* d = opendir(src_dir);
    if (!d) return -1;
    int n_srcs = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n_srcs < MOD_SRCS_MAX) {
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 2, ".c") != 0) continue;
        snprintf(srcs[n_srcs], MOD_PATH_MAX, "%s/%s", src_dir, e->d_name);
        n_srcs++;
    }
    closedir(d);
    sort_srcs(srcs, n_srcs);
    return n_srcs;
}

/* CONTENT-HASH build key: api version + compiler identity + SDK header
 * bytes + sorted source bytes. mtime churn (git checkout, cp, zip
 * re-extract) never changes it; an SDK regen, api bump, toolchain swap,
 * or real source edit always does. Returns 0 with key_out filled, -1 if
 * the mod has no compilable sources. */
static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]) {
    char srcs[MOD_SRCS_MAX][MOD_PATH_MAX];
    int n_srcs = mod_list_srcs(mod_dir, srcs);
    if (n_srcs <= 0) return -1;

    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    {
        const char* ident = mods_cc_identity();
        openpete_sha256_update(&hc, ident, strlen(ident));
    }
    static const char* const sdk_inputs[] = {
        "openpete_mod_api.h", "openpete_sdk_symbols.h",
        "openpete_sdk_wrappers.h", "psx_runtime.h",
        "openpete_game_structs.h", "spyro1_actor_pose.h",
    };
    for (size_t i = 0; i < sizeof sdk_inputs / sizeof sdk_inputs[0]; i++) {
        char p[MOD_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", sdk_dir, sdk_inputs[i]);
        (void)hash_file(&hc, p); /* absent header = stable no-op */
    }
    for (int i = 0; i < n_srcs; i++)
        (void)hash_file(&hc, srcs[i]);
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

/* Content hash of config.toml (hot-reload watch companion to
 * mod_compute_key — config is not a compile input, so its key is
 * tracked separately and a config-only change reloads the mod against
 * the CACHED object). Absent file hashes to a stable sentinel. */
static int config_compute_key(const char* mod_dir, char key_out[65]) {
    char p[MOD_PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/config.toml", mod_dir);
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    if (hash_file(&hc, p) != 0) {
        snprintf(key_out, 65, "-");   /* no config file */
        return 0;
    }
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

static void mod_stamp_path(const char* mod_dir, const char* id,
                           char* out, size_t cap) {
    snprintf(out, cap, "%s/.build/%s.hash", mod_dir, id);
}

static void mod_read_stamp(const char* mod_dir, const char* id,
                           char stamped[65]) {
    stamped[0] = '\0';
    char stamp_path[MOD_PATH_MAX + 16];
    mod_stamp_path(mod_dir, id, stamp_path, sizeof stamp_path);
    FILE* f = fopen(stamp_path, "rb");
    if (!f) return;
    size_t got = fread(stamped, 1, 64, f);
    stamped[got] = '\0';
    if (got && stamped[got - 1] == '\n') stamped[got - 1] = '\0';
    fclose(f);
}

/* 1 = .build/<id>.{so,dll} exists and its stamp matches `key` — the
 * cached object is current, nothing to compile. */
static int mod_key_cached(const char* mod_dir, const char* id,
                          const char* key) {
    char so_path[MOD_PATH_MAX];
    snprintf(so_path, sizeof so_path, "%s/.build/%s" MOD_DL_EXT, mod_dir,
             id);
    if (file_mtime(so_path) == (time_t)-1) return 0;
    char stamped[65];
    mod_read_stamp(mod_dir, id, stamped);
    return strcmp(stamped, key) == 0;
}

/* Compile src/*.c → out_path with the resolved compiler. `stamp` = 1
 * records the build key on success so an unchanged mod never
 * recompiles (0 = hot-reload .hotN objects on Windows, where the base
 * DLL is file-locked and must go stale until the next boot). */
static int mod_compile(const char* mod_dir, const char* id,
                       const char* sdk_dir, const char* key,
                       const char* out_path, int stamp) {
    char srcs[MOD_SRCS_MAX][MOD_PATH_MAX];
    int n_srcs = mod_list_srcs(mod_dir, srcs);
    if (n_srcs <= 0) {
        OPL_ERROR_F("mods", "%s: no .c sources in src/", id);
        return -1;
    }

    char cc[MOD_PATH_MAX];
    mods_resolve_cc(cc, sizeof cc);
    if (!cc[0]) {
        OPL_ERROR_F("mods", "%s: no compiler — expected the bundled "
                    "toolchain at recomp/tools/ next to the exe, or set "
                    "OPENPETE_MOD_CC", id);
        return -1;
    }

    char build_dir[MOD_PATH_MAX];
    snprintf(build_dir, sizeof build_dir, "%s/.build", mod_dir);
    mod_mkdir(build_dir); /* EEXIST is fine */

    char stamp_path[MOD_PATH_MAX + 16];
    mod_stamp_path(mod_dir, id, stamp_path, sizeof stamp_path);

#ifdef _WIN32
    static const char* const flags =
        "-shared -O2 -g -fno-strict-aliasing -Wall";
#else
    static const char* const flags =
        "-shared -fPIC -O2 -g -fno-strict-aliasing -Wall";
#endif
    char cmd[8192];
    int off = snprintf(cmd, sizeof cmd,
                       MOD_Q "%s" MOD_Q " %s -I" MOD_Q "%s" MOD_Q
                       " -o " MOD_Q "%s" MOD_Q,
                       cc, flags, sdk_dir, out_path);
    for (int i = 0; i < n_srcs && off < (int)sizeof cmd; i++)
        off += snprintf(cmd + off, sizeof cmd - (size_t)off,
                        " " MOD_Q "%s" MOD_Q, srcs[i]);
    snprintf(cmd + off, sizeof cmd - (size_t)off,
             " >" MOD_Q "%s/compile.log" MOD_Q " 2>&1", build_dir);

    OPL_INFO_F("mods", "%s: compiling %d source file%s", id, n_srcs,
               n_srcs == 1 ? "" : "s");
    int rc = openpete_host_spawn(cmd);
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s: compile failed (see %s/compile.log) — "
                    "disabled", id, build_dir);
        /* Remove the STAMP only: nothing may cache-hit a failed build,
         * but the object file survives — it may be an author-shipped
         * prebuilt (cross-build handoff) that a no-compiler boot can
         * still fall back to. Deleting it here destroyed exactly that
         * artifact once; don't repeat it. */
        remove(stamp_path);
        return -1;
    }
    if (stamp) {
        FILE* f = fopen(stamp_path, "wb");
        if (f) { fprintf(f, "%s\n", key); fclose(f); }
    }
    return 0;
}

/* Build src/*.c → .build/<id>.{so,dll} when the build key differs from
 * the stamped one. Returns 0 with so_path filled (cache hit, fresh
 * compile, or — Windows without a compiler — a prebuilt DLL). */
static int build_mod(struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len) {
    char key[65];
    if (mod_compute_key(mod->dir, sdk_dir, key) != 0) {
        OPL_ERROR_F("mods", "%s: no .c sources in src/", mod->id);
        return -1;
    }
    snprintf(mod->build_key, sizeof mod->build_key, "%s", key);

    snprintf(so_path, so_path_len, "%s/.build/%s" MOD_DL_EXT, mod->dir,
             mod->id);
    if (mod_key_cached(mod->dir, mod->id, key)) {
        OPL_DEBUG_F("mods", "%s: cached %s up to date (key %.12s)",
                    mod->id, so_path, key);
        return 0;
    }

    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) {
            /* No compiler on this box (Windows without the bundled
             * toolchain): fall back to a prebuilt DLL. Verified-stale (a
             * stamp exists and mismatches) loads loudly; an unstamped
             * DLL (typical for cross-compiled staging) loads silently —
             * there is nothing trustworthy to compare against. */
            if (file_mtime(so_path) != (time_t)-1) {
                char stamped[65];
                mod_read_stamp(mod->dir, mod->id, stamped);
                if (stamped[0])
                    OPL_WARN_F("mods", "%s: %s was built from DIFFERENT "
                               "sources/SDK than what is on disk — "
                               "loading it anyway (rebuild the DLL to "
                               "update)", mod->id, so_path);
                return 0;
            }
            OPL_ERROR_F("mods", "%s: no compiler and no prebuilt %s — "
                        "install the packaged toolchain (recomp/tools/), "
                        "set OPENPETE_MOD_CC, or build the DLL with e.g. "
                        "x86_64-w64-mingw32-gcc -shared -O2 -I<sdk> — "
                        "disabled", mod->id, so_path);
            return -1;
        }
    }
    return mod_compile(mod->dir, mod->id, sdk_dir, key, so_path, 1);
}

typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);

static int load_mod(struct openpete_mod* mod, const char* sdk_dir) {
    char so_path[MOD_PATH_MAX];
    {
        char src_dir[MOD_PATH_MAX + 8];
        snprintf(src_dir, sizeof src_dir, "%s/src", mod->dir);
        struct stat st;
        mod->has_src = (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode));
    }
    if (!mod->has_src) {
        snprintf(so_path, sizeof so_path, "%s/.build/%s" MOD_DL_EXT,
                 mod->dir, mod->id);
        if (file_mtime(so_path) == (time_t)-1) {
            /* Pure-asset mods (textures/music only) are legal: no src/
             * and no prebuilt object means the manifest + assets/ root
             * are the whole mod. */
            OPL_INFO_F("mods", "loaded %s v%s (assets only)", mod->id,
                       mod->version);
            char assets[MOD_PATH_MAX + 8];
            snprintf(assets, sizeof assets, "%s/assets", mod->dir);
            struct stat st;
            if (stat(assets, &st) == 0 && S_ISDIR(st.st_mode))
                (void)openpete_mods_register_asset_root(assets, mod->id);
            return 0;
        }
        /* Binary-only mod: the prebuilt object IS the mod — loaded
         * as-is, never hashed or compiled, skipped by hot reload. */
        OPL_INFO_F("mods", "%s: binary-only mod (no src/), loading %s "
                   "as-is", mod->id, so_path);
    } else {
        if (build_mod(mod, sdk_dir, so_path, sizeof so_path) != 0)
            return -1;
    }

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
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
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

/* Unpack <mods_dir>/<id>.zip to <mods_dir>/.unpacked/<id>/ ("folder or
 * zip, identical" — P1 build+load). The zip's content hash is stamped
 * next to the unpack dir, so an unchanged zip is never re-extracted;
 * mod.toml must sit at the zip ROOT. Entry paths are sanitized (no
 * absolute paths, no ".."). Returns 0 = unpacked/current. */
static int mod_unpack_zip(const char* mods_dir, const char* id) {
    char zip_path[MOD_PATH_MAX];
    snprintf(zip_path, sizeof zip_path, "%s/%s.zip", mods_dir, id);

    /* Content key. */
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    if (hash_file(&hc, zip_path) != 0) return -1;
    uint8_t digest[32];
    char key[65];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key);

    char root[MOD_PATH_MAX], dest[MOD_PATH_MAX], stamp[MOD_PATH_MAX + 8];
    snprintf(root, sizeof root, "%s/.unpacked", mods_dir);
    snprintf(dest, sizeof dest, "%s/.unpacked/%s", mods_dir, id);
    snprintf(stamp, sizeof stamp, "%s.hash", dest);

    char stamped[65] = "";
    FILE* sf = fopen(stamp, "rb");
    if (sf) {
        size_t got = fread(stamped, 1, sizeof stamped - 1, sf);
        stamped[got] = '\0';
        if (got && stamped[got - 1] == '\n') stamped[got - 1] = '\0';
        fclose(sf);
    }
    char probe[MOD_PATH_MAX + 12];
    snprintf(probe, sizeof probe, "%s/mod.toml", dest);
    if (strcmp(stamped, key) == 0 && file_mtime(probe) != (time_t)-1)
        return 0; /* current */

    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        OPL_ERROR_F("mods", "%s.zip: not a readable zip — skipped", id);
        return -1;
    }
    mod_mkdir(root);
    mod_mkdir(dest);
    int files = 0, ok = 1;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ok = 0; break; }
        if (st.m_filename[0] == '/' || strstr(st.m_filename, "..")) {
            OPL_ERROR_F("mods", "%s.zip: unsafe entry path \"%s\" — "
                        "skipped archive", id, st.m_filename);
            ok = 0;
            break;
        }
        char out[MOD_PATH_MAX];
        snprintf(out, sizeof out, "%s/%s", dest, st.m_filename);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            mod_mkdir(out);
            continue;
        }
        /* Ensure intermediate dirs exist (zips may omit dir entries). */
        for (char* p = out + strlen(dest) + 1; *p; p++) {
            if (*p != '/') continue;
            *p = '\0';
            mod_mkdir(out);
            *p = '/';
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, out, 0)) {
            OPL_ERROR_F("mods", "%s.zip: extract failed for \"%s\"", id,
                        st.m_filename);
            ok = 0;
            break;
        }
        files++;
    }
    mz_zip_reader_end(&zip);
    if (!ok) return -1;
    if (file_mtime(probe) == (time_t)-1) {
        OPL_ERROR_F("mods", "%s.zip: no mod.toml at the archive root — "
                    "skipped", id);
        return -1;
    }
    sf = fopen(stamp, "wb");
    if (sf) { fprintf(sf, "%s\n", key); fclose(sf); }
    OPL_INFO_F("mods", "%s: unpacked %d file%s from %s.zip", id, files,
               files == 1 ? "" : "s", id);
    return 0;
}

/* ---- discovery (shared by prepare + init) ---------------------------- */

typedef struct {
    char id[MOD_ID_MAX];
    char dir[MOD_PATH_MAX]; /* folder, or .unpacked/<id> for zip mods */
} mod_ref_t;

/* Resolve the mods dir (opts → env → <exe>/mods), unpack zips, and
 * produce the ordered enabled set (enabled.txt order = priority;
 * fallback: all discovered, lexicographic). Returns the count (0 = no
 * mods directory / nothing enabled: inert). openpete_mods_prepare and
 * openpete_mods_init both run this so they see the same set; everything
 * here is idempotent (zip unpack is content-stamped). */
/* Resolve the mods root: opts → env → <exe>/mods. Returns 0 with
 * mods_dir filled, -1 if no base path is resolvable. */
static int mods_root(const openpete_options_t* opts, char* mods_dir,
                     size_t mods_dir_cap) {
    const char* src = opts && opts->mods_dir ? opts->mods_dir
                                             : getenv("OPENPETE_MODS_DIR");
    if (src) {
        snprintf(mods_dir, mods_dir_cap, "%s", src);
        return 0;
    }
    const char* base = SDL_GetBasePath(); /* trailing sep, SDL-owned */
    if (!base) return -1;
    snprintf(mods_dir, mods_dir_cap, "%smods", base);
    return 0;
}

/* Scan the mods root for candidate ids: subdirs containing mod.toml,
 * plus <id>.zip archives (unpacked to .unpacked/<id>/, content-hash
 * stamped so re-extraction only happens when the zip changes). A
 * folder and a zip with the same id: the folder wins (consumers probe
 * the folder first when resolving the dir). Returns the count. */
static int mods_scan(const char* mods_dir, char ids[][MOD_ID_MAX],
                     int max) {
    DIR* d = opendir(mods_dir);
    if (!d) return 0; /* no mods directory: inert */

    int n_ids = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && n_ids < max) {
        if (e->d_name[0] == '.') continue;
        size_t nl = strlen(e->d_name);
        if (nl > 4 && strcmp(e->d_name + nl - 4, ".zip") == 0) {
            char id[MOD_ID_MAX];
            snprintf(id, sizeof id, "%.*s", (int)(nl - 4), e->d_name);
            if (mod_unpack_zip(mods_dir, id) == 0)
                snprintf(ids[n_ids++], MOD_ID_MAX, "%s", id);
            continue;
        }
        char manifest[MOD_PATH_MAX];
        snprintf(manifest, sizeof manifest, "%s/%s/mod.toml", mods_dir,
                 e->d_name);
        if (file_mtime(manifest) == (time_t)-1) continue;
        snprintf(ids[n_ids++], MOD_ID_MAX, "%s", e->d_name);
    }
    closedir(d);
    return n_ids;
}

/* Resolve a candidate id to its mod dir: the folder, or the zip's
 * .unpacked/<id>/ when no folder carries a mod.toml. */
static void mod_ref_resolve(const char* mods_dir, const char* id,
                            mod_ref_t* r) {
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->dir, sizeof r->dir, "%s/%s", mods_dir, id);
    char probe[MOD_PATH_MAX + 12];
    snprintf(probe, sizeof probe, "%s/mod.toml", r->dir);
    if (file_mtime(probe) == (time_t)-1) /* zip-unpacked mod */
        snprintf(r->dir, sizeof r->dir, "%s/.unpacked/%s", mods_dir, id);
}

static int cmp_id_row(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static int mods_discover(const openpete_options_t* opts,
                         char* mods_dir, size_t mods_dir_cap,
                         mod_ref_t* out, int max) {
    if (mods_root(opts, mods_dir, mods_dir_cap) != 0) return 0;

    char ids[MODS_MAX][MOD_ID_MAX];
    int n_ids = mods_scan(mods_dir, ids, MODS_MAX);
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
        qsort(ids, (size_t)n_ids, sizeof ids[0], cmp_id_row);
        for (int i = 0; i < n_ids; i++)
            snprintf(order[n_order++], MOD_ID_MAX, "%s", ids[i]);
    }

    int n_out = 0;
    for (int i = 0; i < n_order && n_out < max; i++) {
        int known = 0;
        for (int j = 0; j < n_ids; j++)
            if (strcmp(order[i], ids[j]) == 0) known = 1;
        if (!known) {
            OPL_WARN_F("mods", "enabled.txt names \"%s\" but mods/%s/ has "
                       "no mod.toml — skipped", order[i], order[i]);
            continue;
        }
        mod_ref_resolve(mods_dir, order[i], &out[n_out]);
        n_out++;
    }
    return n_out;
}

static void mods_sdk_dir(char* out, size_t cap) {
    const char* base = SDL_GetBasePath();
    snprintf(out, cap, "%ssdk", base ? base : "./");
}

/* ---- compile-on-boot prepare (openpete-mod-compile-on-boot) ---------- */

struct mods_prepare_ctx {
    mod_ref_t                 refs[MODS_MAX];
    char                      keys[MODS_MAX][65];
    int                       stale[MODS_MAX];
    int                       n_stale;
    char                      sdk_dir[MOD_PATH_MAX];
    openpete_mods_progress_fn progress;
};

static int mods_prepare_task(void* ud) {
    struct mods_prepare_ctx* c = (struct mods_prepare_ctx*)ud;
    for (int i = 0; i < c->n_stale; i++) {
        const mod_ref_t* r = &c->refs[c->stale[i]];
        if (c->progress) c->progress("COMPILING MODS", i, c->n_stale);
        char so_path[MOD_PATH_MAX];
        snprintf(so_path, sizeof so_path, "%s/.build/%s" MOD_DL_EXT,
                 r->dir, r->id);
        /* A failure is logged (+ compile.log) and left for init to
         * report as disabled — prepare never aborts the boot. */
        (void)mod_compile(r->dir, r->id, c->sdk_dir,
                          c->keys[c->stale[i]], so_path, 1);
    }
    if (c->progress) c->progress("COMPILING MODS", c->n_stale, c->n_stale);
    return 0;
}

int openpete_mods_prepare(const openpete_options_t* opts,
                          openpete_mods_progress_fn progress,
                          openpete_mods_run_task_fn run_task) {
    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) return 0; /* no compiler on this box: init's
                               * prebuilt-DLL fallback owns messaging */
    }
    /* Boot-once, main-thread; static keeps ~37 KB off the stack. */
    static struct mods_prepare_ctx c;
    memset(&c, 0, sizeof c);
    char mods_dir[MOD_PATH_MAX];
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, c.refs,
                               MODS_MAX);
    if (n_refs <= 0) return 0;
    mods_sdk_dir(c.sdk_dir, sizeof c.sdk_dir);
    c.progress = progress;

    /* Pass 1: pure hashing — milliseconds; no window, no compile. */
    for (int i = 0; i < n_refs; i++) {
        char key[65];
        if (mod_compute_key(c.refs[i].dir, c.sdk_dir, key) != 0)
            continue; /* assets-only or binary-only mod */
        if (mod_key_cached(c.refs[i].dir, c.refs[i].id, key))
            continue;
        memcpy(c.keys[i], key, sizeof key);
        c.stale[c.n_stale++] = i;
    }
    if (c.n_stale == 0) return 0;

    /* Pass 2: compile the stale set as ONE worker task (the splash
     * runner is not reentrant); progress from inside the worker
     * lazy-opens the UI. */
    OPL_INFO_F("mods", "%d mod%s stale — compiling before boot",
               c.n_stale, c.n_stale == 1 ? "" : "s");
    if (run_task)
        (void)run_task(mods_prepare_task, &c);
    else
        (void)mods_prepare_task(&c);
    return c.n_stale;
}

int openpete_mods_compile_all(const openpete_options_t* opts) {
    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) {
            OPL_ERROR_F("mods", "--compile-mods: no compiler — expected "
                        "the bundled toolchain at recomp/tools/ next to "
                        "the exe, or set OPENPETE_MOD_CC");
            return -1;
        }
    }
    char mods_dir[MOD_PATH_MAX];
    if (mods_root(opts, mods_dir, sizeof mods_dir) != 0) {
        OPL_ERROR_F("mods", "--compile-mods: no mods dir resolvable");
        return -1;
    }

    /* EVERY candidate in the dir — enabled.txt is a runtime concern;
     * the build gate wants a disabled mod's breakage surfaced too. */
    char ids[MODS_MAX][MOD_ID_MAX];
    int n_ids = mods_scan(mods_dir, ids, MODS_MAX);
    if (n_ids == 0) {
        OPL_INFO_F("mods", "--compile-mods: nothing to compile in %s",
                   mods_dir);
        return 0;
    }
    qsort(ids, (size_t)n_ids, sizeof ids[0], cmp_id_row);

    char sdk_dir[MOD_PATH_MAX];
    mods_sdk_dir(sdk_dir, sizeof sdk_dir);

    int n_compiled = 0, n_cached = 0, n_failed = 0;
    for (int i = 0; i < n_ids; i++) {
        mod_ref_t r;
        mod_ref_resolve(mods_dir, ids[i], &r);
        char key[65];
        if (mod_compute_key(r.dir, sdk_dir, key) != 0)
            continue; /* assets-only or binary-only mod: nothing to do */
        if (mod_key_cached(r.dir, r.id, key)) {
            n_cached++;
            continue;
        }
        char so_path[MOD_PATH_MAX];
        snprintf(so_path, sizeof so_path, "%s/.build/%s" MOD_DL_EXT,
                 r.dir, r.id);
        if (mod_compile(r.dir, r.id, sdk_dir, key, so_path, 1) == 0)
            n_compiled++;
        else
            n_failed++; /* mod_compile logged + pointed at compile.log */
    }
    OPL_INFO_F("mods", "--compile-mods: %d compiled, %d up to date, "
               "%d failed (%s)", n_compiled, n_cached, n_failed, mods_dir);
    return n_failed;
}

int openpete_mods_init(const openpete_options_t* opts) {
    char mods_dir[MOD_PATH_MAX];
    mod_ref_t refs[MODS_MAX];
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, refs,
                               MODS_MAX);
    if (n_refs <= 0) return 0;

    char sdk_dir[MOD_PATH_MAX];
    mods_sdk_dir(sdk_dir, sizeof sdk_dir);
    snprintf(s_sdk_dir, sizeof s_sdk_dir, "%s", sdk_dir);

    /* Phase A: parse all enabled manifests (no loading yet). */
    struct openpete_mod pending[MODS_MAX];
    int n_pending = 0;
    for (int i = 0; i < n_refs && n_pending < MODS_MAX; i++) {
        struct openpete_mod* mod = &pending[n_pending];
        memset(mod, 0, sizeof(*mod));
        if (parse_manifest(refs[i].dir, mod) != 0) continue;
        n_pending++;
    }

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
        mod->enabled = 1;
        load_ok[i] = 1;
        s_mod_count++;
        loaded++;
    }

    /* Declared-bindings conflict scan: two enabled mods claiming the
     * same host key is legal (reads are non-exclusive) but surprising —
     * name both, with purposes, so the user can re-order or rebind. */
    for (int i = 0; i < s_mod_count; i++)
        for (int bi = 0; bi < s_mods[i].n_bindings; bi++)
            for (int j = i + 1; j < s_mod_count; j++)
                for (int bj = 0; bj < s_mods[j].n_bindings; bj++)
                    if (strcmp(s_mods[i].bindings[bi].key,
                               s_mods[j].bindings[bj].key) == 0)
                        OPL_WARN_F("mods", "key \"%s\" bound by both %s "
                                   "(%s) and %s (%s)",
                                   s_mods[i].bindings[bi].key,
                                   s_mods[i].id,
                                   s_mods[i].bindings[bi].purpose,
                                   s_mods[j].id,
                                   s_mods[j].bindings[bj].purpose);

    if (loaded > 0)
        OPL_INFO_F("mods", "%d mod%s active (dir: %s)", loaded,
                   loaded == 1 ? "" : "s", mods_dir);
    toggle_parse_env();
    return loaded;
}

