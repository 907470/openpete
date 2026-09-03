// core/mods/mod_loader.c — discover, build, and load mods.
//
// P1 vertical slice of notes/plans/openpete-modding-framework.md:
//   mods/<id>/mod.toml     manifest ([mod] id/version/api)
//   mods/<id>/src/*.c      mod source, compiled on load
//   mods/enabled.txt       one mod id per line; order IS priority
//                          (later lines override earlier — S3AIR
//                          semantics). Missing file = all discovered
//                          mods, lexicographic order. --mods (env
//                          OPENPETE_MODS) overrides the file with a
//                          comma-separated id list, same semantics.
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
#include "openpete_osd.h"
#include "../host/texture_pack.h"   /* texpack_fog_shift */
#include "openpete_mod_api.h"
#include "openpete_mod_ui.h"
#include "openpete_log.h"
#include "openpete_host_input.h"
#include "openpete_keymap.h"     /* [keys.mod.<id>] binding overrides */
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

#include <SDL3/SDL.h> /* SDL_GetBasePath — shipped sdk/ + toolchain */
#include "openpete_paths.h" /* mods/ lives with the writable user data */

#include "openpete_psx.h"   /* openpete_psx_get_ram — api->guest() */
#include "openpete_state.h" /* savestate slots — engine services */

#include "../../third_party/tomlc99/toml.h"

/* mod_chain.c internals shared within core/mods. */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

#define MODS_MAX          64  /* keep <= mod_config.c CFG_SLOTS; the scan
                               * WARNS when a mods dir exceeds this rather
                               * than silently dropping readdir-order tail
                               * entries (which once ate a freshly added
                               * mod on a 33-mod install) */
#define MOD_ID_MAX        64
_Static_assert(MOD_ID_MAX == OPENPETE_ARENA_OWNER_MAX,
               "arena ledger owner field must hold a full mod id");
#define MOD_BINDINGS_MAX  8

typedef struct {
    char name[24];    /* binding_down() key within the mod's namespace */
    char key[24];     /* host key name (host_key_down vocabulary) */
    char purpose[64]; /* human text for conflict messages / mod menu */
} mod_binding_t;

/* One [[config]] schema row from mod.toml — what the engine renders in
 * the Mods panel for this mod. Values themselves live in config.toml. */
#define MOD_CFG_ROWS_MAX  32
#define MOD_CFG_ITEMS_MAX 8
typedef struct {
    char    key[96];   /* dotted config path */
    char    type;      /* 'b' bool, 'i' int, 'd' float, 's' string */
    char    label[64];
    char    group[32];
    char    help[128];
    double  min, max;  /* numeric range; min >= max = free input */
    char    items[MOD_CFG_ITEMS_MAX][24];
    const char* item_ptrs[MOD_CFG_ITEMS_MAX]; /* stable combo view */
    int     n_items;
    int64_t def_i;     /* default: bool/int */
    double  def_d;     /* default: float */
    char    def_s[64]; /* default: string */
} mod_cfg_row_t;
/* One [[material]] row from mod.toml — the zero-C material tier. Holds
 * exactly what `material_register` takes, in manifest spelling, so a mod
 * that only wants "reshade this channel with this frag" needs no C at
 * all. Rows register BEFORE openpete_mod_entry, so a mixed mod's C code
 * registers later and therefore wins (last match takes the pixels): the
 * manifest states the default, code refines it. */
#define MOD_MATERIAL_ROWS_MAX 16
#define MOD_MATERIAL_PARAMS_MAX 64   /* floats; capped again by the ABI's
                                      * OPENPETE_MATERIAL_UNIFORMS_MAX */
/* Manifest channel spelling -> OP_CHAN_*. "any" (or an absent key) is -1;
 * an unrecognised name returns -2 so the caller refuses the row instead
 * of silently widening it to every channel. */
static int mod_channel_from_name(const char* s) {
    static const struct { const char* name; int chan; } k[] = {
        { "any",      -1 },              { "moby",     OP_CHAN_MOBY },
        { "terrain",  OP_CHAN_TERRAIN }, { "player",   OP_CHAN_PLAYER },
        { "flame",    OP_CHAN_FLAME },   { "particle", OP_CHAN_PARTICLE },
        { "sky",      OP_CHAN_SKY },     { "shadow",   OP_CHAN_SHADOW },
        { "tracer",   OP_CHAN_TRACER },  { "portal",   OP_CHAN_PORTAL },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (strcmp(s, k[i].name) == 0) return k[i].chan;
    return -2;
}

typedef struct {
    char  frag[128];   /* "shaders/<name>.frag" — relative to the mod dir */
    char  vert[128];   /* optional "shaders/<name>.vert" */
    int   channel;     /* OP_CHAN_*; -1 = any */
    int   ident, moby_class, texture, level;   /* -1 = any */
    unsigned flags;    /* OP_SEL_* narrowing bits */
    float params[MOD_MATERIAL_PARAMS_MAX];
    int   n_params;
    int   enabled;     /* [[material]] enabled = false starts it off */
} mod_material_row_t;
#define MOD_PATH_MAX      1024
#define MOD_SRCS_MAX      64
#define MOD_DEPS_MAX      8

struct openpete_mod {
    char  id[MOD_ID_MAX];
    char  version[32];
    char  name[64];      /* manifest display name (fallback: id) */
    char  dir[MOD_PATH_MAX];
    char  data_dir[MOD_PATH_MAX + 8]; /* "<dir>/data", made on demand */
    char  log_tag[MOD_ID_MAX + 8]; /* "mod:<id>" */
    char  deps[MOD_DEPS_MAX][MOD_ID_MAX];
    int   n_deps;
    int   raw_warned;  /* raw-tier warning issued (once per mod) */
    char  state_decl;  /* manifest state: 0 undeclared, 'n' none,
                        * 'r' rebuildable (host state re-derives from
                        * guest RAM) — gates savestate/rewind arming */
    char  ra_decl;     /* manifest runahead: 0 undeclared, 'p' pure,
                        * 'h' host-fx — gates speculative re-execution */
    int   sim_mutative; /* [mod] sim_mutative: -1 undeclared (the engine
                        * derives it), 0 the mod never writes guest RAM,
                        * 1 it does. Drives the save modset tag. */
    mod_binding_t bindings[MOD_BINDINGS_MAX]; /* declared host keys */
    int   n_bindings;
    mod_cfg_row_t cfg_rows[MOD_CFG_ROWS_MAX]; /* [[config]] schema */
    int   n_cfg;
    mod_material_row_t mat_rows[MOD_MATERIAL_ROWS_MAX]; /* [[material]] */
    int   n_mat_rows;
    int   cfg_reload_pending; /* Apply wrote config.toml; reload at the
                               * next tick boundary re-reads it */
    /* [[level]] blocks: declarative custom levels (registered with the
     * content registry at load; the engine owns slot/TOC/warp). */
    struct { char name[64]; char files[64]; char donor[24]; int replace;
             char music[64]; char title[40]; }
          levels[4];
    int   n_levels;
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
    int   enabled;       /* live-toggle state */
    int   boot_on;       /* in the boot enabled set (vs a deferred-entry
                          * load-all row, which starts disabled) */
    int   entered;       /* openpete_mod_entry has run; load-all rows
                          * defer it to their first enable so a
                          * boot-disabled mod runs zero code */
    int   has_assets;    /* assets/ overlay root exists (boot-scoped) */
    openpete_mod_ui_body_fn ui_body; /* UI-annex descriptor body
                                      * (openpete_mod_ui_section symbol,
                                      * resolved at dlopen, ABI-checked;
                                      * NULL = none) */
    /* ui_status line block: accumulates within a tick, lazily reset on
     * the mod's first call in a NEW tick — a one-shot line printed at
     * entry or on a level event persists until repainted. */
    char     status[24][120];
    int      status_n;
    int      status_trunc;
    unsigned status_tick;
    openpete_mod_toggle_fn toggle_fn; /* live-toggle notification */
    void* dl;
};

static struct openpete_mod s_mods[MODS_MAX];
static int                 s_mod_count = 0;
static unsigned            s_toggle_epoch; /* defined with the queue below */

/* Where the enabled set came from, so "fix your load order" messages
 * name the list the user actually controls: the mods dir's enabled.txt,
 * or the --mods override that replaced it. Set by mods_discover. */
static const char* s_order_src = "enabled.txt";

/* Read the object's ABI descriptor (openpete_mod_api.h, "ABI
 * descriptor") before anything in it runs. Refused: no descriptor (an
 * object from before the layout freeze — its vtable shape is unknown),
 * a newer api than the engine's, or a larger api struct (calls would
 * index past the engine's vtable). Older-but-descriptored loads: the
 * struct is append-only from the freeze on. The check runs on BOTH the
 * engine-compiled path (always green: the SDK stub rides the compile
 * line) and the prebuilt-object path, which compile-on-load cannot
 * rebuild and which is therefore the path this exists for. */
static int mod_check_abi(void* dl, const char* id, const char* path) {
    const openpete_mod_abi_t* d =
        (const openpete_mod_abi_t*)mod_dlsym(dl, "openpete_mod_abi");
    if (!d || d->magic != OPENPETE_MOD_ABI_MAGIC) {
        OPL_ERROR_F("mods", "%s: %s carries no ABI descriptor — it was "
                    "built without the SDK's openpete_mod_abi.c (add "
                    "<sdk>/openpete_mod_abi.c to the compile line, or let "
                    "the engine compile the mod from src/) — disabled",
                    id, path);
        return -1;
    }
    if (d->api_version > OPENPETE_MOD_API_VERSION ||
        d->api_size > (uint32_t)sizeof(openpete_mod_api_t)) {
        OPL_ERROR_F("mods", "%s: %s was built for api %u (struct %u bytes) "
                    "but this engine provides api %u (%u bytes) — a newer "
                    "object cannot load into an older engine; update "
                    "openpete or rebuild the mod against this SDK — "
                    "disabled", id, path, d->api_version, d->api_size,
                    OPENPETE_MOD_API_VERSION,
                    (unsigned)sizeof(openpete_mod_api_t));
        return -1;
    }
    return 0;
}

/* Sweep every registration surface a mod owns. Runs before a reload
 * re-enters the mod, and after a FAILED entry: an entry that registered
 * chains, draws or materials and then returned nonzero must not leave
 * them live under an id the loader has just declared disabled. */
static void mod_strip_registrations(struct openpete_mod* mod) {
    openpete_mods_chain_remove_owner(mod->id);
    openpete_mods_draw_remove_owner(mod->id);
    openpete_mods_registry_remove_owner(mod->id);
    openpete_mods_postfx_remove_owner(mod->id);  /* before shaders: retires pipelines */
    openpete_mods_material_remove_owner(mod->id);
    openpete_mods_shader_remove_owner(mod->id);
    mod->ui_body   = NULL;
    mod->toggle_fn = NULL;
}

/* Resolve the UI annex's registration descriptor (see
 * openpete_mod_ui.h). The abi check runs at LOAD so the prebuilt-DLL
 * fallback path (which compile-on-load cannot rebuild) fails loudly
 * instead of calling through a stale vtable shape: on mismatch the
 * section is refused with a log line and the mod itself keeps running. */
static void mod_resolve_ui_section(struct openpete_mod* mod, void* dl) {
    mod->ui_body = NULL;
    const openpete_mod_ui_section_t* d = (const openpete_mod_ui_section_t*)
        mod_dlsym(dl, "openpete_mod_ui_section");
    if (!d) return;
    if (d->magic != 0x4F505549u || d->abi != OPENPETE_MOD_UI_ABI ||
        !d->body) {
        OPL_ERROR_F("mods", "%s: UI annex ABI mismatch (mod built for "
                    "abi %u, engine has %u) — section refused; the mod "
                    "itself still runs. Rebuild the mod against the "
                    "current SDK.", mod->id,
                    d->magic == 0x4F505549u ? d->abi : 0u,
                    (unsigned)OPENPETE_MOD_UI_ABI);
        return;
    }
    mod->ui_body = d->body;
}

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

/* ---- native-renderer visual-bypass advisory ------------------------------
 * The default --renderer vk draws the 3D world from gamestate and does NOT
 * rasterize the PSX ordering table, so overriding one of the original PSX
 * render / GPU-submission functions to change the picture has NO visible effect
 * under vk (it shows only under --renderer psycross). Warn so the author isn't
 * left wondering. True only when native is the sole display (pure vk, windowed);
 * under psycross / ab / diff / headless the PSX path still rasterizes.
 *
 * Scope: the *visual* no-op only. Freezing these can also drop the render→
 * gameplay channel writes (m_WasDrawn etc.) and desync — a separate, sharper
 * concern this advisory deliberately does not cover. */
static int s_native_hides_psx = 0;   /* set in openpete_mods_init from opts */

/* Original PSX render / GPU-submission functions whose rasterized output the
 * native renderer replaces. Matched by resolved address, so both override_addr
 * and override_name are covered. */
static const char* const k_psx_render_fns[] = {
    "GamestateDraw",                                   /* top-level world draw */
    "DrawOTag", "DrawSync",                            /* OT submit / GPU sync */
    "PutDrawEnv", "SetDefDrawEnv", "SetDrawMode",      /* PsyQ GPU draw state   */
    "TitlescreenDraw", "TitlescreenDrawSprite",        /* titlescreen render    */
};

/* If addr is one of the native-bypassed render functions (resolved against the
 * SDK symbol table), return its name; else NULL. */
static const char* psx_render_fn_name(uint32_t addr) {
    if (!addr || !s_symbols) return NULL;
    for (size_t i = 0; i < sizeof k_psx_render_fns / sizeof k_psx_render_fns[0]; i++) {
        const openpete_sdk_symbol_t* s = sym_find(k_psx_render_fns[i]);
        if (s && s->addr == addr) return k_psx_render_fns[i];
    }
    return NULL;
}

static void warn_render_bypassed(openpete_mod_t* self, uint32_t addr) {
    if (!s_native_hides_psx) return;
    const char* rn = psx_render_fn_name(addr);
    if (!rn) return;
    OPL_WARN_F(self->log_tag,
               "override targets PSX render function %s (0x%08X): the native "
               "renderer (--renderer vk) draws the world from gamestate and does "
               "not rasterize the PSX ordering table, so visual changes here "
               "won't appear under vk. Draw via the mod draw API for native "
               "visuals, or run --renderer psycross to see this path.", rn, addr);
}

static int api_override_addr(openpete_mod_t* self, uint32_t addr,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_addr")) return -1;
    int rc = openpete_mods_chain_add(addr, fn, self->id);
    if (rc == 0) {
        OPL_INFO_F(self->log_tag, "override registered at 0x%08X", addr);
        /* Raw addresses are region-pinned and drift across decomp
         * progress — warn from the actual use, once per mod (the old
         * manifest tier="raw" declaration said the same thing but could
         * lie in either direction). */
        if (!self->raw_warned) {
            self->raw_warned = 1;
            OPL_WARN_F("mods", "%s uses raw-tier addresses: unstable "
                       "across engine/decomp updates and region-pinned",
                       self->id);
        }
        warn_render_bypassed(self, addr);
    }
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
    if (rc == 0) {
        OPL_INFO_F(self->log_tag, "override registered: %s (0x%08X)",
                   name, sym->addr);
        warn_render_bypassed(self, sym->addr);
    }
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

/* Region-table inverse (psx_runtime.h; declared here for the same reason
 * call_by_address is — this file does not pull the runner header in). */
int psx_runtime_region_host_to_vaddr(const void* host, uint32_t* out_vaddr);

/* Guest vaddr -> host pointer. Delegates to the engine's own resolver so
 * the two never disagree: it strips the segment bits with the full
 * 0x1FFFFFFF mask (no 16 MB ceiling from a 24-bit offset), covers the
 * scratchpad, and consults the region table for anything above canonical
 * RAM — the arena included, at any size it grows to.
 *
 * Guest address 0 is PSX NULL and resolves to NULL here, NOT to the base
 * of RAM: a mod that missed a failed allocation and writes through the
 * returned pointer must fault in its own frame rather than silently
 * poisoning the kernel area (which is also where the low park lives). */
static void* api_guest(uint32_t vaddr) {
    if (purity_violation("guest")) return NULL;
    uint32_t seg = vaddr >> 28;
    if (seg != 0x0u && seg != 0x8u && seg != 0x9u && seg != 0xAu &&
        seg != 0xBu)
        return NULL;
    /* The kernel area (physical [0, 0x10000)) is the stream-handle range:
     * a frame word whose 21-bit field addresses it decodes as a handle,
     * never as bytes. No allocator lands there, so this raw translation
     * is the one route left for a mod to park data under the bound; it
     * is refused (NULL), loudly once. Not fatal: a mod chasing a null
     * model row lands here on a READ, which is harmless. */
    if (vaddr != 0u && (vaddr & 0x1FFFFFFFu) < 0x10000u) {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            OPL_WARN_F("mods", "guest(0x%08X): the kernel area is the "
                       "stream-handle range — no mod data may live below "
                       "0x10000; refused (NULL)", vaddr);
        }
        return NULL;
    }
    return openpete_psx_to_host_ptr(vaddr);
}

/* MIPS kseg0 base — architectural, not a game address (the purity check
 * permits #define'd constants). */
#define OP_KSEG0_BASE 0x80000000u
#define OP_KSEG0_SCRATCH 0x9F800000u /* scratchpad's KSEG0 spelling */

/* Host pointer -> guest vaddr, the exact inverse of api_guest for every
 * address that call can hand out: canonical RAM, the scratchpad, and any
 * registered region (guest_alloc's arena included — a mod that walks a
 * host_view and needs the vaddr of an interior byte relies on this). */
static uint32_t api_guest_addr(const void* host) {
    if (!host) return 0;
    const uint8_t* p = (const uint8_t*)host;
    uint8_t* ram = openpete_psx_get_ram();
    if (ram && p >= ram && p < ram + 2u * 1024u * 1024u)
        return OP_KSEG0_BASE | (uint32_t)(p - ram);
    uint8_t* scratch = openpete_psx_get_scratch();
    if (scratch && p >= scratch &&
        p < scratch + OPENPETE_PSX_SCRATCH_SIZE)
        return OP_KSEG0_SCRATCH | (uint32_t)(p - scratch);
    uint32_t va = 0;
    if (psx_runtime_region_host_to_vaddr(host, &va)) return va;
    return 0;
}

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

static void api_call_addr(CPUState* cpu, uint32_t addr) {
    if (purity_violation("call_addr")) return;
    call_by_address(cpu, addr);
}

static int api_register_present_hook(openpete_mod_t* self,
                                     openpete_mod_present_fn fn) {
    if (!self) return -1;
    if (purity_violation("register_present_hook")) return -1;
    int rc = openpete_mods_register_present_hook(fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "present hook registered");
    return rc;
}

/* Per-mod UI section (api v3): the callback is stored on the mod and
 * invoked by the dev overlay's Mods panel while the mod is enabled.
 * Registration is entry/tick context (the callback itself runs under
 * the render-purity guard — see openpete_mods_ui_draw). */

/* Canonical tick counter mirror for the ui_status lazy reset; advanced
 * by openpete_mods_tick_boundary. */
static unsigned s_current_tick;

static void api_ui_status(openpete_mod_t* self, const char* fmt, ...) {
    if (!self || !fmt) return;
    struct openpete_mod* m = self;
    if (m->status_tick != s_current_tick) {
        m->status_n = 0;
        m->status_trunc = 0;
        m->status_tick = s_current_tick;
    }
    if (m->status_n >= (int)(sizeof m->status / sizeof m->status[0])) {
        m->status_trunc = 1;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->status[m->status_n], sizeof m->status[0], fmt, ap);
    va_end(ap);
    m->status_n++;
}

/* Toasts are attributed to the mod on BOTH axes: the coalescing key is
 * namespaced with its id so two mods cannot repaint each other's toast, and
 * the rate-limit bucket is per mod so one mod in a tick loop cannot starve
 * another's notifications — or the engine's own. */
static void api_notify(openpete_mod_t* self, const char* key,
                       uint32_t timeout_ms, const char* fmt, ...) {
    if (!self || !fmt) return;
    struct openpete_mod* m = self;
    char text[OPENPETE_OSD_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char nskey[OPENPETE_OSD_KEY_MAX];
    if (key && *key) snprintf(nskey, sizeof nskey, "%s/%s", m->id, key);
    openpete_osd_notify(m->id, (key && *key) ? nskey : NULL,
                        timeout_ms, "%s", text);
}

static int api_register_toggle_hook(openpete_mod_t* self,
                                    openpete_mod_toggle_fn fn) {
    if (!self) return -1;
    if (purity_violation("register_toggle_hook")) return -1;
    self->toggle_fn = fn;
    return 0;
}

/* ---- engine services (P2 contract) ----------------------------------- */




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
    /* Snapshot, not live SDL: mods call this from their tick, which runs on
     * the game fiber — not necessarily the thread that pumps events and so
     * refreshes SDL's keyboard array. */
    return openpete_host_input_key_down((int)sc);
}

static char s_sdk_dir[MOD_PATH_MAX];

/* defined later in this file */
static int parse_manifest(const char* mod_dir, struct openpete_mod* mod);
static int mod_enter_deferred(struct openpete_mod* mod);

static int build_mod(struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len);
static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]);
static void sdk_prefix_drop(void);
static int config_compute_key(const char* mod_dir, char key_out[65]);
static int mod_compile(const char* mod_dir, const char* id,
                       const char* sdk_dir, const char* key,
                       const char* out_path, int stamp);
typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);
static const openpete_mod_api_t s_api;

/* Reload one mod in place at a tick boundary: sweep every registration
 * it owns, re-parse + rebuild + dlopen a FRESH object (unique .hotN
 * name — the old object stays resident by design), re-run entry. The
 * mod's own host state does NOT cross the reload: entry re-runs and
 * re-derives it. */
static void mod_hot_reload(struct openpete_mod* mod) {
    OPL_INFO_F("mods", "hot reload: %s (source or config change)", mod->id);

    /* Build and open the fresh object BEFORE touching anything live: a
     * reload that cannot produce a loadable object keeps the running one
     * (registrations and all) instead of leaving the mod inert. The
     * Mods-panel Apply reaches this path, so a stray syntax error must
     * cost a log line, not the session's mod. */
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
        OPL_ERROR_F("mods", "%s: reload compile failed — keeping the "
                    "running object", mod->id);
        return;
    }
    snprintf(mod->build_key, sizeof mod->build_key, "%s", key);
#else
    char so_path[MOD_PATH_MAX];
    if (build_mod(mod, s_sdk_dir, so_path, sizeof so_path) != 0) {
        OPL_ERROR_F("mods", "%s: reload compile failed — keeping the "
                    "running object", mod->id);
        return;
    }
    {   /* copy: a fresh inode guarantees dlopen loads a fresh object */
        FILE* in = fopen(so_path, "rb");
        FILE* outf = in ? fopen(hot_path, "wb") : NULL;
        if (!in || !outf) {
            if (in) fclose(in);
            if (outf) fclose(outf);
            OPL_ERROR_F("mods", "%s: reload stage copy failed — keeping "
                        "the running object", mod->id);
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
        OPL_ERROR_F("mods", "%s: reload dlopen failed: %s — keeping the "
                    "running object", mod->id, mod_dlerror());
        return;
    }
    if (mod_check_abi(dl, mod->id, hot_path) != 0) return;
    mod_entry_fn entry = (mod_entry_fn)mod_dlsym(dl, "openpete_mod_entry");
    if (!entry) {
        OPL_ERROR_F("mods", "%s: reload object has no openpete_mod_entry "
                    "— keeping the running object", mod->id);
        return;
    }

    /* A loadable object exists: now retire the running one. From here a
     * failure leaves the mod INERT — the old registrations are gone and
     * the new entry never completed — which the log says in as many
     * words. */
    mod_strip_registrations(mod);
    mod->n_deps = mod->n_bindings = 0;
    mod->n_cfg = 0;
    mod->n_mat_rows = 0;   /* re-parsed below; else a reload appends dupes */
    mod->state_decl = mod->ra_decl = 0;
    char dir_copy[MOD_PATH_MAX];
    snprintf(dir_copy, sizeof dir_copy, "%s", mod->dir);
    if (parse_manifest(dir_copy, mod) != 0) {
        OPL_ERROR_F("mods", "%s: reload manifest parse failed — mod is "
                    "now INERT until the next successful reload", mod->id);
        return;
    }
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
    openpete_mods_arena_entry_begin(mod->id); /* replay its allocations */
    if (entry(&s_api, mod) != 0) {
        OPL_ERROR_F("mods", "%s: reload entry failed — mod is now INERT",
                    mod->id);
        mod_strip_registrations(mod);
        return;
    }
    mod->dl = dl; /* old object deliberately stays resident */
    mod_resolve_ui_section(mod, dl);
    /* Re-entry registered every surface in its default (enabled) state;
     * a live-disabled mod must come back disabled, not resurrected. And
     * a reload changes what the active registration set means, so the
     * timeline-derived state (rewind ring, runahead snapshot) is stale
     * either way — bump the modset epoch. */
    openpete_mods_chain_set_owner_enabled(mod->id, mod->enabled);
    openpete_mods_draw_set_owner_enabled(mod->id, mod->enabled);
    openpete_mods_registry_set_owner_enabled(mod->id, mod->enabled);
    s_toggle_epoch++;
    OPL_INFO_F("mods", "hot reload: %s live%s", mod->id,
               mod->enabled ? "" : " (still disabled)");
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
    sdk_prefix_drop(); /* an SDK edit must show up in this round's keys */
    for (int i = 0; i < s_mod_count; i++) {
        struct openpete_mod* mod = &s_mods[i];
        /* Deferred-entry rows (never entered) reload nothing: no code
         * of theirs is live; their first enable uses the boot object. */
        if (!mod->dl || !mod->has_src || !mod->entered) continue;
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
 * hooks, and registry rows. Nothing is torn down, so re-enable restores the mod exactly
 * where it was. Asset overlays stay boot-scoped ("restart tier" in the
 * plan's language — their consumers scan at init).
 *
 * The save modset tag deliberately does NOT rebuild on toggle: a
 * divergent mod that already ran this session has already diverged it;
 * the session tag is the loaded set, not the instantaneous one.
 *
 * Drivers: the future mod menu; and OPENPETE_MODS_TOGGLE=
 * "tick:id:0|1[,...]" (a canonical-tick schedule) for CI/dev. */


typedef struct {
    char id[MOD_ID_MAX];
    int  on;
    int  defer_logged; /* one deferral notice per request, not per tick */
} toggle_req_t;
static toggle_req_t s_toggle_q[16];
static int          s_toggle_n = 0;
static unsigned     s_toggle_epoch = 0; /* bumped per applied change */

unsigned openpete_mods_toggle_epoch(void) { return s_toggle_epoch; }

typedef struct { unsigned tick; char id[MOD_ID_MAX]; int on; } toggle_sched_t;
static toggle_sched_t s_toggle_sched[16];
static int mod_is_mutative(const struct openpete_mod* m);

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

int openpete_mods_menu_info(int i, openpete_mods_menu_info_t* out) {
    if (i < 0 || i >= s_mod_count || !out) return 0;
    const struct openpete_mod* m = &s_mods[i];
    out->id          = m->id;
    out->name        = m->name[0] ? m->name : m->id;
    out->version     = m->version;
    out->enabled     = m->enabled;
    out->entered     = m->entered;
    out->sim_mutative = mod_is_mutative(m);
    out->has_ui      = m->ui_body != NULL || m->status_n > 0;
    out->has_assets  = m->has_assets;
    out->has_src     = m->has_src;
    out->has_cfg     = m->n_cfg;
    return 1;
}

/* ── Config-schema accessors (the Mods panel's data source) ────────── */

int openpete_mods_cfg_row_count(int mod_index) {
    if (mod_index < 0 || mod_index >= s_mod_count) return 0;
    return s_mods[mod_index].n_cfg;
}

int openpete_mods_cfg_row(int mod_index, int row,
                          openpete_mods_cfg_row_t* out) {
    if (mod_index < 0 || mod_index >= s_mod_count || !out) return 0;
    struct openpete_mod* m = &s_mods[mod_index];
    if (row < 0 || row >= m->n_cfg) return 0;
    const mod_cfg_row_t* r = &m->cfg_rows[row];
    out->key     = r->key;
    out->type    = r->type;
    out->label   = r->label[0] ? r->label : r->key;
    out->group   = r->group;
    out->help    = r->help;
    out->min     = r->min;
    out->max     = r->max;
    out->items   = r->n_items ? r->item_ptrs : NULL;
    out->n_items = r->n_items;
    out->ival    = 0;
    out->fval    = 0.0;
    out->sval    = "";
    static char sbuf[128];
    switch (r->type) {
    case 'b':
        out->ival = openpete_mods_config_bool(m, r->key, r->def_i != 0);
        break;
    case 'i':
        out->ival = openpete_mods_config_int(m, r->key, r->def_i);
        break;
    case 'd':
        out->fval = openpete_mods_config_float(m, r->key, r->def_d);
        break;
    case 's':
        (void)openpete_mods_config_str(m, r->key, r->def_s, sbuf,
                                       (int)sizeof sbuf);
        out->sval = sbuf;
        break;
    }
    return 1;
}

int openpete_mods_cfg_apply(int mod_index, int n,
                            const openpete_mods_cfg_edit_t* edits) {
    if (mod_index < 0 || mod_index >= s_mod_count || n <= 0 || !edits)
        return -1;
    struct openpete_mod* m = &s_mods[mod_index];
    if (openpete_mods_config_write(m->dir, m->log_tag, n, edits) != 0)
        return -1;
    m->cfg_reload_pending = 1;
    return 0;
}

/* Overlay-side invocation of a mod's UI section. The render-purity
 * guard makes guest access / game calls / registration refuse inside
 * the callback, exactly as in present hooks: widgets mutate host-side
 * mod state, tick hooks consume it. */
void openpete_mods_ui_draw(int i, const struct openpete_mod_ui* ui) {
    if (i < 0 || i >= s_mod_count || !ui) return;
    struct openpete_mod* m = &s_mods[i];
    if (!m->enabled) return;
    for (int l = 0; l < m->status_n; l++)
        ui->text("%s", m->status[l]);
    if (m->status_trunc)
        ui->text_disabled("... (ui_status block truncated)");
    if (!m->ui_body) return;
    openpete_mods_in_present = 1;
    m->ui_body(ui);
    openpete_mods_in_present = 0;
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

/* Scheduled config edits (harness): OPENPETE_MODS_CFG_APPLY=
 * "tick:id:key=int[,...]" commits an integer config edit through the
 * same path the Mods panel's Apply uses — config.toml rewritten, reload
 * queued, entry re-run at the next tick boundary — so a headless run can
 * exercise re-entry deterministically. */
typedef struct {
    unsigned tick;
    char     id[MOD_ID_MAX];
    char     key[64];
    int64_t  val;
} cfg_apply_sched_t;
static cfg_apply_sched_t s_cfg_sched[8];
static int               s_cfg_sched_n = 0;

static void cfg_apply_schedule_parse(void) {
    const char* e = getenv("OPENPETE_MODS_CFG_APPLY");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        cfg_apply_sched_t t;
        long long v;
        if (sscanf(tok, "%u:%63[^:]:%63[^=]=%lld", &t.tick, t.id, t.key,
                   &v) == 4 &&
            s_cfg_sched_n < (int)(sizeof s_cfg_sched / sizeof s_cfg_sched[0])) {
            t.val = (int64_t)v;
            s_cfg_sched[s_cfg_sched_n++] = t;
        } else {
            OPL_ERROR_F("mods", "OPENPETE_MODS_CFG_APPLY: bad token \"%s\" "
                        "(want tick:id:key=int)", tok);
        }
    }
    if (s_cfg_sched_n)
        OPL_INFO_F("mods", "config-apply schedule armed (%d entr%s)",
                   s_cfg_sched_n, s_cfg_sched_n == 1 ? "y" : "ies");
}

static void cfg_apply_schedule_run(unsigned canonical_tick) {
    for (int i = 0; i < s_cfg_sched_n; i++) {
        cfg_apply_sched_t* t = &s_cfg_sched[i];
        if (t->tick != canonical_tick) continue;
        int idx = -1;
        for (int m = 0; m < s_mod_count; m++)
            if (strcmp(s_mods[m].id, t->id) == 0) { idx = m; break; }
        if (idx < 0) {
            OPL_ERROR_F("mods", "config-apply schedule: no mod \"%s\"", t->id);
            continue;
        }
        openpete_mods_cfg_edit_t e;
        memset(&e, 0, sizeof e);
        e.key = t->key; e.type = 'i'; e.ival = t->val;
        OPL_INFO_F("mods", "config-apply schedule: %s %s=%lld at tick %u",
                   t->id, t->key, (long long)t->val, canonical_tick);
        (void)openpete_mods_cfg_apply(idx, 1, &e);
    }
}

void openpete_mods_tick_boundary(CPUState* cpu, unsigned canonical_tick) {
    s_current_tick = canonical_tick;
    for (int i = 0; i < s_toggle_sched_n; i++) {
        if (s_toggle_sched[i].tick == canonical_tick)
            (void)openpete_mods_set_mod_enabled(s_toggle_sched[i].id,
                                                s_toggle_sched[i].on);
    }
    cfg_apply_schedule_run(canonical_tick);
    hot_reload_poll();

    /* Both of these are things a UI section may have asked for from inside a
     * present, where they could not be honoured: a retuned selector would
     * have split the frame between two keys, and the class census reads
     * guest RAM. The tick boundary is where they become legal. */
    openpete_mods_material_apply_pending();
    if (s_mod_count) openpete_mods_moby_class_census(cpu);

    /* Apply-committed config changes: reload the mod so its entry
     * re-reads the fresh values (build key unchanged, so the compile is
     * a cache hit). A mod that never entered (deferred row / assets-
     * only) just re-parses the file — entry reads it on first enable. */
    for (int i = 0; i < s_mod_count; i++) {
        struct openpete_mod* mod = &s_mods[i];
        if (!mod->cfg_reload_pending) continue;
        mod->cfg_reload_pending = 0;
        if (mod->dl && mod->has_src && mod->entered)
            mod_hot_reload(mod);
        else
            openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    }

    int kept = 0;
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
        if (mod->enabled == on) continue; /* no-op: don't bump the epoch */
        /* A disable that would strand the player (the mod owns the level
         * they are inside) DEFERS: the request stays queued and lands
         * the moment they leave the slot. The epoch must not bump until
         * it actually applies. */
        if (!on && openpete_mods_registry_owner_busy(id)) {
            if (!s_toggle_q[q].defer_logged) {
                OPL_INFO_F("mods", "live toggle: \"%s\" deferred — can't "
                           "be turned off while inside its level", id);
                s_toggle_q[q].defer_logged = 1;
            }
            s_toggle_q[kept++] = s_toggle_q[q];
            continue;
        }
        if (on && !mod->entered && mod_enter_deferred(mod) != 0) {
            OPL_ERROR_F("mods", "live toggle: \"%s\" failed to start — "
                        "staying disabled", id);
            continue;
        }
        openpete_mods_chain_set_owner_enabled(id, on);
        openpete_mods_draw_set_owner_enabled(id, on);
        openpete_mods_registry_set_owner_enabled(id, on);
        mod->enabled = on;
        s_toggle_epoch++;
        OPL_INFO_F("mods", "live toggle: %s %s (tick %u)",
                   id, on ? "ENABLED" : "DISABLED", canonical_tick);
        /* Notify AFTER the flip: tick context, so the mod may undo (or
         * re-stage) its guest-side effects with the surfaces already in
         * the new state. */
        if (mod->toggle_fn) mod->toggle_fn(cpu, on);
    }
    s_toggle_n = kept;
}

const char* openpete_mods_build_key(const char* id) {
    if (!id) return NULL;
    for (int i = 0; i < s_mod_count; i++)
        if (strcmp(s_mods[i].id, id) == 0) return s_mods[i].build_key;
    return NULL;
}

/* 1 iff every ENABLED code mod declares a runahead posture (manifest
 * runahead = "pure" | "host-fx"). Assets-only mods run zero code and are
 * trivially capable. An undeclared code mod makes runahead unarmable;
 * *offender names the first one. "Code mod" = a loaded object, so a
 * binary-only mod (prebuilt object, no src/) is held to the same
 * declaration as a source one. */
int openpete_mods_runahead_capable(const char** offender) {
    for (int i = 0; i < s_mod_count; i++) {
        if (!s_mods[i].enabled || !s_mods[i].dl) continue;
        if (!s_mods[i].ra_decl) {
            if (offender) *offender = s_mods[i].id;
            return 0;
        }
    }
    if (offender) *offender = NULL;
    return 1;
}

/* 1 iff every ENABLED code mod declares a host-state posture (manifest
 * state = "none" | "rebuildable") — the gate for timeline jumps
 * (rewind): an undeclared mod's host state may silently desync when the
 * guest world moves under it. Assets-only mods hold no host state. */
int openpete_mods_state_safe(const char** offender) {
    for (int i = 0; i < s_mod_count; i++) {
        if (!s_mods[i].enabled || !s_mods[i].dl) continue;
        if (!s_mods[i].state_decl) {
            if (offender) *offender = s_mods[i].id;
            return 0;
        }
    }
    if (offender) *offender = NULL;
    return 1;
}

/* Does this mod write guest RAM? An explicit sim_mutative wins. With
 * none, the engine proves the answer where it can: a mod that loads no
 * code object has no path to guest RAM at all, and a mod that claims a
 * level slot changes what the player is progressing through. Everything
 * else is assumed to mutate, so a forgotten declaration produces a
 * spurious save warning rather than a silently mistagged card. */
static int mod_is_mutative(const struct openpete_mod* m) {
    if (m->sim_mutative >= 0) return m->sim_mutative;
    return (m->dl != NULL) || (m->n_levels > 0);
}

/* Save modset tag (per-modset save safety): the canonical identity of
 * the running gameplay-relevant modset — "id@ver+id@ver", ids sorted,
 * non-mutative mods excluded. Empty string = vanilla. Consumed by the
 * host memcard module to tag save cards and warn on mismatch. */
static char s_tag[1024];
static int  s_tag_built = 0;

const char* openpete_mods_modset_tag(void) {
    if (s_tag_built) return s_tag;
    s_tag_built = 1;
    s_tag[0] = '\0';
    /* Sorted id order → order-independent canonical form. Only mods
     * whose entry has RUN can have diverged the session — deferred-
     * entry load-all rows join the tag at their first enable (the
     * enable path clears s_tag_built; memcard reads the tag per
     * check/write, so later writes carry the grown tag). */
    const struct openpete_mod* picked[MODS_MAX];
    int n = 0;
    for (int i = 0; i < s_mod_count; i++)
        if (s_mods[i].entered && mod_is_mutative(&s_mods[i]))
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






/* Declared-binding resolve: name → [keys.mod.<id>] if the config rebound
 * it, else this mod's own [[binding]] key → host_key_down. Same
 * tick-context caveats as host_key_down itself.
 *
 * The config override is what lets a user resolve a clash between two
 * mods (or between a mod and a host hotkey) without editing either mod —
 * the registry is the thing you rebind, not the mod. It also reaches
 * gamepad sources, which the mod.toml `key` spelling cannot. */
static int api_binding_down(openpete_mod_t* self, const char* name) {
    if (!self || !name) return 0;

    km_source_t src[KM_MAX_SOURCES];
    int n = 0;
    if (openpete_keymap_mod_binding(self->id, name, src, KM_MAX_SOURCES, &n)) {
        for (int i = 0; i < n; i++)
            if (openpete_keymap_source_down(&src[i]))
                return 1;
        return 0;
    }

    for (int i = 0; i < self->n_bindings; i++)
        if (strcmp(self->bindings[i].name, name) == 0)
            return api_host_key_down(self->bindings[i].key);
    OPL_ERROR_F(self->log_tag, "binding \"%s\" not declared in mod.toml",
                name);
    return 0;
}

/* Guest scratch arena: sim-visible state — tick context only. */
static uint32_t api_guest_alloc(openpete_mod_t* self, uint32_t size,
                                uint32_t align, uint32_t flags,
                                void** host_view) {
    if (host_view) *host_view = NULL;
    if (!self) return 0;
    if (purity_violation("guest_alloc")) return 0;
    if (flags & ~(uint32_t)OP_GALLOC_IMMUTABLE) {
        OPL_ERROR_F("mods", "%s: guest_alloc: unknown flags %#x", self->id,
                    flags);
        return 0;
    }
    return openpete_mods_guest_alloc(size, align, flags, host_view,
                                     self->id);
}

/* Stream handle: sim-visible numbering — tick context only. */
static uint32_t api_vstream_handle(openpete_mod_t* self,
                                   uint32_t stream_vaddr) {
    if (!self) return 0;
    if (purity_violation("vstream_handle")) return 0;
    return openpete_mods_vstream_handle(stream_vaddr, self->id);
}


/* One-shot sound: host-side additive — no purity gate (sim-neutral by
 * construction; presentation, not render output). */
static int api_sfx_play(openpete_mod_t* self, const char* name,
                          float gain) {
    if (!self || !name || strstr(name, "..")) return -1;
    char path[MOD_PATH_MAX + 96];
    snprintf(path, sizeof path, "%s/assets/sfx/own/%s.wav", self->dir, name);
    return openpete_mod_sound_play(path, gain);
}

const char* openpete_mods_id_of(openpete_mod_t* self) {
    return self ? self->id : NULL;
}

const char* openpete_mods_dir_of(openpete_mod_t* self) {
    return self ? self->dir : NULL;
}

int openpete_mods_load_index_of(openpete_mod_t* self) {
    if (!self) return -1;
    ptrdiff_t i = self - s_mods;
    return (i >= 0 && i < s_mod_count) ? (int)i : -1;
}

const char* openpete_mods_sdk_dir(void) {
    return s_sdk_dir;
}

/* ---- pillar 9: game data + texture packs (api 2) ----------------------- */

static int api_read_level_data(openpete_mod_t* self, int level_id,
                               void* buf, uint32_t cap) {
    (void)self;
    return openpete_mods_read_level_data(level_id, buf, cap);
}

static int api_texpack_fog_shift(openpete_mod_t* self, const float scale_rgb[3],
                                 const float offset_rgb[3]) {
    (void)self;
    texture_pack_fog_shift_set(scale_rgb, offset_rgb);
    return 0;
}

/* pillar 10: materials. The mod-facing form takes a frag shader only —
 * vertex-stage overrides stay engine-internal, so a material can never
 * move geometry, only reshade it. */
static int api_material_register(openpete_mod_t* self,
                                 const openpete_mod_material_selector_t* sel,
                                 int shader) {
    if (!sel) return -1;
    return openpete_mods_material_register(self, sel, -1, shader, NULL);
}

static int api_moby_classes(openpete_mod_t* self, uint16_t* out, int cap) {
    (void)self;   /* the loaded-class set is a property of the level */
    return openpete_mods_moby_classes(out, cap);
}

static int api_material_register_refine(openpete_mod_t* self,
                                        const openpete_mod_material_selector_t* sel,
                                        int shader,
                                        openpete_mod_refine_fn refine) {
    if (!sel) return -1;
    return openpete_mods_material_register(self, sel, -1, shader, refine);
}

static const openpete_mod_api_t s_api = {
    .api_version   = OPENPETE_MOD_API_VERSION,
    /* pillar 1: the override chain */
    .override_addr = api_override_addr,
    .override_name = api_override_name,
    .base          = openpete_mods_base,
    .call          = api_call_addr,
    /* pillar 2: guest memory */
    .guest         = api_guest,
    .guest_addr    = api_guest_addr,
    .guest_alloc   = api_guest_alloc,
    /* pillar 3: lifecycle and identity */
    .log           = api_log,
    .data_dir      = api_data_dir,
    .register_toggle_hook = api_register_toggle_hook,
    /* pillar 4: settings */
    .config_bool   = openpete_mods_config_bool,
    .config_int    = openpete_mods_config_int,
    .config_float  = openpete_mods_config_float,
    .config_str    = openpete_mods_config_str,
    .ui_status     = api_ui_status,
    .notify        = api_notify,
    .vstream_handle = api_vstream_handle,
    /* pillar 5: present and screen-space draw */
    .register_present_hook = api_register_present_hook,
    .draw_text     = openpete_mods_draw_text,
    .draw_rect     = openpete_mods_draw_rect,
    /* pillar 6: post-process shaders */
    .shader_register     = openpete_mods_shader_register,
    .postfx_register     = openpete_mods_postfx_register,
    .postfx_enable       = openpete_mods_postfx_enable,
    .postfx_set_uniforms = openpete_mods_postfx_set_uniforms,
    /* pillar 8: audio */
    .sfx_replace   = openpete_mods_sfx_replace,
    .sfx_play      = api_sfx_play,
    /* declared input bindings */
    .binding_down  = api_binding_down,
    /* pillar 9: game data + texture packs (api 2) */
    .read_level_data   = api_read_level_data,
    .texpack_fog_shift = api_texpack_fog_shift,
    /* pillar 10: materials */
    .material_register   = api_material_register,
    .material_register_refine = api_material_register_refine,
    .material_enable     = openpete_mods_material_enable,
    .material_set_params = openpete_mods_material_set_params,
    .material_set_selector = openpete_mods_material_set_selector,
    .moby_classes        = api_moby_classes,
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
    /* The api struct is append-only, and source mods recompile against
     * the CURRENT staged SDK regardless — so the manifest pin is the
     * MINIMUM api the mod's calls need, not an exact build match. Only
     * a mod needing a newer engine refuses. */
    if (api.ok && api.u.i > (int64_t)OPENPETE_MOD_API_VERSION) {
        OPL_ERROR_F("mods", "%s: mod \"%s\" needs api %lld, engine has %u — "
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
    {   /* Display name for menus; ids stay the stable identity. */
        toml_datum_t nm = toml_string_in(t, "name");
        if (nm.ok) {
            snprintf(mod->name, sizeof mod->name, "%s", nm.u.s);
            free(nm.u.s);
        }
        if (!mod->name[0])
            snprintf(mod->name, sizeof mod->name, "%s", mod->id);
    }
    /* sim_mutative: does this mod write guest RAM? Undeclared leaves it
     * to mod_is_mutative(), which can prove the answer for a mod that
     * ships no code and claims no level. */
    toml_datum_t sm = toml_bool_in(t, "sim_mutative");
    mod->sim_mutative = sm.ok ? (sm.u.b ? 1 : 0) : -1;
    /* The key was once spelled sim_neutral, with the opposite sense.
     * Honour it with a warning rather than ignoring it: silently
     * dropping the old spelling would move a mod into the save modset
     * tag without anyone noticing. */
    toml_datum_t sn = toml_bool_in(t, "sim_neutral");
    if (sn.ok) {
        OPL_WARN_F("mods", "%s: sim_neutral is obsolete — write "
                   "sim_mutative = %s instead (the sense is inverted)",
                   mod->id, sn.u.b ? "false" : "true");
        if (!sm.ok) mod->sim_mutative = sn.u.b ? 0 : 1;
    }
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
    /* State + runahead declarations (savestate/rewind/runahead gates).
     * state: "none" (no host state derived from guest history) or
     * "rebuildable" (host state re-derives from guest RAM within a
     * tick). runahead: "pure" (tick hooks touch guest state only) or
     * "host-fx" (host side effects the engine suppresses during
     * speculative ticks). Unknown values are an error: a wrong claim
     * here corrupts timelines, so it must not parse quietly. */
    toml_datum_t st = toml_string_in(t, "state");
    if (st.ok) {
        if (strcmp(st.u.s, "none") == 0)             mod->state_decl = 'n';
        else if (strcmp(st.u.s, "rebuildable") == 0) mod->state_decl = 'r';
        else OPL_ERROR_F("mods", "%s: unknown state = \"%s\" (want "
                         "\"none\" or \"rebuildable\") — ignored",
                         mod->id, st.u.s);
        free(st.u.s);
    }
    toml_datum_t ra = toml_string_in(t, "runahead");
    if (ra.ok) {
        if (strcmp(ra.u.s, "pure") == 0)          mod->ra_decl = 'p';
        else if (strcmp(ra.u.s, "host-fx") == 0)  mod->ra_decl = 'h';
        else OPL_ERROR_F("mods", "%s: unknown runahead = \"%s\" (want "
                         "\"pure\" or \"host-fx\") — ignored",
                         mod->id, ra.u.s);
        free(ra.u.s);
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
    /* [[config]] schema rows: key + type (+ default/label/group/help,
     * numeric min/max, string items). The engine renders these in the
     * Mods panel; a row with a bad or missing type/key is skipped with
     * an error rather than guessing. */
    toml_array_t* cfgs = toml_array_in(root, "config");
    if (cfgs) {
        int n = toml_array_nelem(cfgs);
        for (int i = 0; i < n; i++) {
            if (mod->n_cfg >= MOD_CFG_ROWS_MAX) {
                OPL_ERROR_F("mods", "%s: more than %d [[config]] rows — "
                            "the rest are ignored", path, MOD_CFG_ROWS_MAX);
                break;
            }
            toml_table_t* c = toml_table_at(cfgs, i);
            if (!c) continue;
            toml_datum_t ck = toml_string_in(c, "key");
            toml_datum_t ct = toml_string_in(c, "type");
            if (!ck.ok || !ct.ok) {
                OPL_ERROR_F("mods", "%s: [[config]] needs key and type",
                            path);
                if (ck.ok) free(ck.u.s);
                if (ct.ok) free(ct.u.s);
                continue;
            }
            char type = 0;
            if (strcmp(ct.u.s, "bool") == 0)        type = 'b';
            else if (strcmp(ct.u.s, "int") == 0)    type = 'i';
            else if (strcmp(ct.u.s, "float") == 0)  type = 'd';
            else if (strcmp(ct.u.s, "string") == 0) type = 's';
            if (!type) {
                OPL_ERROR_F("mods", "%s: [[config]] \"%s\": unknown type "
                            "\"%s\"", path, ck.u.s, ct.u.s);
                free(ck.u.s);
                free(ct.u.s);
                continue;
            }
            mod_cfg_row_t* r = &mod->cfg_rows[mod->n_cfg];
            memset(r, 0, sizeof *r);
            snprintf(r->key, sizeof r->key, "%s", ck.u.s);
            r->type = type;
            free(ck.u.s);
            free(ct.u.s);
            toml_datum_t d;
            if ((d = toml_string_in(c, "label")).ok) {
                snprintf(r->label, sizeof r->label, "%s", d.u.s);
                free(d.u.s);
            }
            if ((d = toml_string_in(c, "group")).ok) {
                snprintf(r->group, sizeof r->group, "%s", d.u.s);
                free(d.u.s);
            }
            if ((d = toml_string_in(c, "help")).ok) {
                snprintf(r->help, sizeof r->help, "%s", d.u.s);
                free(d.u.s);
            }
            if ((d = toml_double_in(c, "min")).ok) r->min = d.u.d;
            if ((d = toml_double_in(c, "max")).ok) r->max = d.u.d;
            switch (type) {
            case 'b':
                if ((d = toml_bool_in(c, "default")).ok)
                    r->def_i = d.u.b ? 1 : 0;
                break;
            case 'i':
                if ((d = toml_int_in(c, "default")).ok) r->def_i = d.u.i;
                break;
            case 'd':
                if ((d = toml_double_in(c, "default")).ok) r->def_d = d.u.d;
                else if ((d = toml_int_in(c, "default")).ok)
                    r->def_d = (double)d.u.i;
                break;
            case 's':
                if ((d = toml_string_in(c, "default")).ok) {
                    snprintf(r->def_s, sizeof r->def_s, "%s", d.u.s);
                    free(d.u.s);
                }
                break;
            }
            toml_array_t* it = toml_array_in(c, "items");
            if (it && type == 's') {
                int ni = toml_array_nelem(it);
                for (int k = 0; k < ni && r->n_items < MOD_CFG_ITEMS_MAX;
                     k++) {
                    toml_datum_t iv = toml_string_at(it, k);
                    if (!iv.ok) continue;
                    snprintf(r->items[r->n_items], sizeof r->items[0],
                             "%s", iv.u.s);
                    free(iv.u.s);
                    r->n_items++;
                }
            }
            for (int k = 0; k < r->n_items; k++)
                r->item_ptrs[k] = r->items[k];
            mod->n_cfg++;
        }
        if (mod->n_cfg)
            OPL_INFO_F("mods", "%s: %d config schema row(s)", mod->id,
                       mod->n_cfg);
    }
    {   /* [[level]] — declarative custom levels */
        toml_array_t* la = toml_array_in(root, "level");
        int nl = la ? toml_array_nelem(la) : 0;
        for (int i = 0; i < nl && mod->n_levels < 4; i++) {
            toml_table_t* lt = toml_table_at(la, i);
            if (!lt) continue;
            toml_datum_t ln = toml_string_in(lt, "name");
            toml_datum_t lf = toml_string_in(lt, "files");
            toml_datum_t ld = toml_string_in(lt, "donor");
            toml_datum_t lr = toml_bool_in(lt, "replace");
            toml_datum_t lm = toml_string_in(lt, "music");
            toml_datum_t lti = toml_string_in(lt, "title");
            if (ln.ok && lf.ok && ld.ok) {
                snprintf(mod->levels[mod->n_levels].name,
                         sizeof mod->levels[0].name, "%s", ln.u.s);
                snprintf(mod->levels[mod->n_levels].files,
                         sizeof mod->levels[0].files, "%s", lf.u.s);
                snprintf(mod->levels[mod->n_levels].donor,
                         sizeof mod->levels[0].donor, "%s", ld.u.s);
                mod->levels[mod->n_levels].replace = lr.ok && lr.u.b;
                snprintf(mod->levels[mod->n_levels].music,
                         sizeof mod->levels[0].music, "%s",
                         lm.ok ? lm.u.s : "");
                snprintf(mod->levels[mod->n_levels].title,
                         sizeof mod->levels[0].title, "%s",
                         lti.ok ? lti.u.s : "");
                mod->n_levels++;
            } else {
                OPL_ERROR_F("mods", "%s: [[level]] needs name, files and "
                            "donor — skipped", path);
            }
            if (ln.ok) free(ln.u.s);
            if (lf.ok) free(lf.u.s);
            if (ld.ok) free(ld.u.s);
            if (lm.ok) free(lm.u.s);
            if (lti.ok) free(lti.u.s);
        }
    }
    {   /* [[material]] — declarative reshade rows (zero-C material tier).
         * Unknown channel/tier/textured spellings are REFUSED, never
         * defaulted: a typo'd key that silently widened the selection
         * would repaint geometry the author never asked for, and the
         * whole selector contract is "never match wider than asked". */
        toml_array_t* ma = toml_array_in(root, "material");
        int nm = ma ? toml_array_nelem(ma) : 0;
        for (int i = 0; i < nm; i++) {
            if (mod->n_mat_rows >= MOD_MATERIAL_ROWS_MAX) {
                OPL_ERROR_F("mods", "%s: more than %d [[material]] rows — "
                            "the rest are ignored", path,
                            MOD_MATERIAL_ROWS_MAX);
                break;
            }
            toml_table_t* mt = toml_table_at(ma, i);
            if (!mt) continue;
            mod_material_row_t r;
            memset(&r, 0, sizeof r);
            r.channel = r.ident = r.moby_class = r.texture = r.level = -1;
            r.enabled = 1;

            toml_datum_t d;
            int bad = 0;
            if ((d = toml_string_in(mt, "frag")).ok) {
                snprintf(r.frag, sizeof r.frag, "%s", d.u.s); free(d.u.s);
            }
            if ((d = toml_string_in(mt, "vert")).ok) {
                snprintf(r.vert, sizeof r.vert, "%s", d.u.s); free(d.u.s);
            }
            if (!r.frag[0] && !r.vert[0]) {
                OPL_ERROR_F("mods", "%s: [[material]] needs frag (or vert) "
                            "— skipped", path);
                continue;
            }
            if ((d = toml_string_in(mt, "channel")).ok) {
                r.channel = mod_channel_from_name(d.u.s);
                if (r.channel == -2) {
                    OPL_ERROR_F("mods", "%s: [[material]] unknown channel "
                                "\"%s\" — skipped", path, d.u.s);
                    bad = 1;
                }
                free(d.u.s);
            }
            if (bad) continue;
            if ((d = toml_int_in(mt, "ident")).ok)   r.ident      = (int)d.u.i;
            if ((d = toml_int_in(mt, "class")).ok)   r.moby_class = (int)d.u.i;
            if ((d = toml_int_in(mt, "texture")).ok) r.texture    = (int)d.u.i;
            if ((d = toml_int_in(mt, "level")).ok)   r.level      = (int)d.u.i;
            if ((d = toml_bool_in(mt, "enabled")).ok) r.enabled   = d.u.b ? 1 : 0;
            /* tier / textured are NARROWING: omit for both, name one to
             * restrict. They mirror the C flags exactly. */
            if ((d = toml_string_in(mt, "tier")).ok) {
                if (strcmp(d.u.s, "hp") == 0)      r.flags |= OP_SEL_HP_TIER;
                else if (strcmp(d.u.s, "lp") == 0) r.flags |= OP_SEL_LP_TIER;
                else {
                    OPL_ERROR_F("mods", "%s: [[material]] tier must be "
                                "\"hp\" or \"lp\" (got \"%s\") — skipped",
                                path, d.u.s);
                    bad = 1;
                }
                free(d.u.s);
            }
            if (bad) continue;
            if ((d = toml_bool_in(mt, "textured")).ok)
                r.flags |= d.u.b ? OP_SEL_TEXTURED : OP_SEL_UNTEXTURED;
            if ((d = toml_bool_in(mt, "env_anim")).ok && d.u.b)
                r.flags |= OP_SEL_ENV_ANIM;
            toml_array_t* pa = toml_array_in(mt, "params");
            if (pa) {
                int np = toml_array_nelem(pa);
                if (np > MOD_MATERIAL_PARAMS_MAX) {
                    OPL_ERROR_F("mods", "%s: [[material]] params has %d "
                                "floats, max %d — skipped", path, np,
                                MOD_MATERIAL_PARAMS_MAX);
                    continue;
                }
                for (int k = 0; k < np; k++) {
                    toml_datum_t pv = toml_double_at(pa, k);
                    if (!pv.ok) {   /* TOML keeps 1 and 1.0 distinct */
                        toml_datum_t iv = toml_int_at(pa, k);
                        if (!iv.ok) {
                            OPL_ERROR_F("mods", "%s: [[material]] params[%d] "
                                        "is not a number — skipped", path, k);
                            bad = 1;
                            break;
                        }
                        r.params[k] = (float)iv.u.i;
                    } else {
                        r.params[k] = (float)pv.u.d;
                    }
                }
                if (bad) continue;
                r.n_params = np;
            }
            mod->mat_rows[mod->n_mat_rows++] = r;
        }
        if (mod->n_mat_rows)
            OPL_INFO_F("mods", "%s: %d declarative material row(s)", mod->id,
                       mod->n_mat_rows);
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
/* The SDK half of the build key is the same for every mod and is the
 * large half — a few hundred KB of generated headers against a few KB of
 * mod source — so it is hashed once per round and the digest state is
 * cloned per mod. The byte stream fed to the hash is unchanged, so the
 * keys, and every stamp already on disk, are identical to hashing it
 * per mod. The hot-reload watch drops the prefix before each poll round
 * so an SDK edit is still noticed. */
static openpete_sha256_ctx s_sdk_prefix;
static char                s_sdk_prefix_dir[MOD_PATH_MAX];
static int                 s_sdk_prefix_ok = 0;

static void sdk_prefix_drop(void) { s_sdk_prefix_ok = 0; }

static const openpete_sha256_ctx* sdk_prefix(const char* sdk_dir) {
    if (s_sdk_prefix_ok && strcmp(s_sdk_prefix_dir, sdk_dir) == 0)
        return &s_sdk_prefix;
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    {
        const char* ident = mods_cc_identity();
        openpete_sha256_update(&hc, ident, strlen(ident));
    }
    static const char* const sdk_inputs[] = {
        "openpete_mod_api.h", "openpete_mod_ui.h", "openpete_sdk_symbols.h",
        "openpete_sdk_wrappers.h", "psx_runtime.h",
        "openpete_game_structs.h",
        "openpete_mod_abi.c",
    };
    for (size_t i = 0; i < sizeof sdk_inputs / sizeof sdk_inputs[0]; i++) {
        char p[MOD_PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", sdk_dir, sdk_inputs[i]);
        (void)hash_file(&hc, p); /* absent header = stable no-op */
    }
    s_sdk_prefix = hc;
    snprintf(s_sdk_prefix_dir, sizeof s_sdk_prefix_dir, "%s", sdk_dir);
    s_sdk_prefix_ok = 1;
    return &s_sdk_prefix;
}

static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]) {
    char srcs[MOD_SRCS_MAX][MOD_PATH_MAX];
    int n_srcs = mod_list_srcs(mod_dir, srcs);
    if (n_srcs <= 0) return -1;

    openpete_sha256_ctx hc = *sdk_prefix(sdk_dir);
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
    /* The ABI descriptor rides every compile line (openpete_mod_api.h,
     * "ABI descriptor"): compiled against the same SDK as the mod's own
     * sources, it records the api the object is really built for. */
    if (off < (int)sizeof cmd)
        off += snprintf(cmd + off, sizeof cmd - (size_t)off,
                        " " MOD_Q "%s/openpete_mod_abi.c" MOD_Q, sdk_dir);
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

/* Run openpete_mod_entry + resolve the state-ABI descriptor. Shared by
 * the boot load and the deferred first-enable path. */
static void mod_register_materials(struct openpete_mod* mod);

static int mod_run_entry(struct openpete_mod* mod) {
    mod_register_materials(mod);   /* manifest first; entry's C wins over it */
    mod_entry_fn entry =
        (mod_entry_fn)mod_dlsym(mod->dl, "openpete_mod_entry");
    if (!entry) {
        OPL_ERROR_F("mods", "%s: no openpete_mod_entry symbol — disabled",
                    mod->id);
        return -1; /* keep the .so resident; it is inert without chains */
    }
    openpete_mods_arena_entry_begin(mod->id);
    int rc = entry(&s_api, mod);
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s: openpete_mod_entry returned %d — disabled",
                    mod->id, rc);
        mod_strip_registrations(mod); /* whatever it registered before failing */
        return -1;
    }
    mod_resolve_ui_section(mod, mod->dl);
    mod->entered = 1;
    return 0;
}

/* Arm the mod's [[material]] rows — the zero-C material tier. Runs
 * BEFORE openpete_mod_entry so a mixed mod's C registrations land later
 * and therefore win the pixels (last match takes them): the manifest is
 * the declared default, C is the refinement.
 *
 * A row that cannot be honoured is dropped with an error and the rest
 * still arm — one bad row must not cost a mod its whole manifest — but
 * it is never silently downgraded to a wider selector. */
static void mod_register_materials(struct openpete_mod* mod) {
    for (int i = 0; i < mod->n_mat_rows; i++) {
        const mod_material_row_t* r = &mod->mat_rows[i];
        int frag = -1, vert = -1;
        if (r->frag[0]) {
            frag = openpete_mods_shader_register(mod, r->frag);
            if (frag < 0) {
                OPL_ERROR_F("mods", "%s: [[material]] frag \"%s\" failed to "
                            "build — row skipped", mod->id, r->frag);
                continue;
            }
        }
        if (r->vert[0]) {
            vert = openpete_mods_shader_register(mod, r->vert);
            if (vert < 0) {
                OPL_ERROR_F("mods", "%s: [[material]] vert \"%s\" failed to "
                            "build — row skipped", mod->id, r->vert);
                continue;
            }
        }
        openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
        sel.channel    = r->channel;
        sel.ident      = r->ident;
        sel.moby_class = r->moby_class;
        sel.texture    = r->texture;
        sel.level      = r->level;
        sel.flags      = r->flags;
        int h = openpete_mods_material_register(mod, &sel, vert, frag, NULL);
        if (h < 0) continue;          /* register already logged the reason */
        if (r->n_params)
            (void)openpete_mods_material_set_params(
                mod, h, r->params, (uint32_t)(r->n_params * sizeof(float)));
        if (!r->enabled)
            (void)openpete_mods_material_enable(mod, h, 0);
    }
}

/* Register the mod's assets/ overlay root (in enabled/entry order —
 * texture/music namespaces consume it). */
static void mod_register_assets(struct openpete_mod* mod) {
    if (!mod->has_assets) return;
    char assets[MOD_PATH_MAX + 8];
    snprintf(assets, sizeof assets, "%s/assets", mod->dir);
    (void)openpete_mods_register_asset_root(assets, mod->id);
}

/* run_entry=0 = deferred-entry load (the load-all path): compile,
 * dlopen and parse everything, but run NO mod code and register no
 * asset root — a boot-disabled mod is inert by construction until its
 * first enable runs mod_enter_deferred. */
static int load_mod(struct openpete_mod* mod, const char* sdk_dir,
                    int run_entry) {
    char so_path[MOD_PATH_MAX];
    {
        char src_dir[MOD_PATH_MAX + 8];
        snprintf(src_dir, sizeof src_dir, "%s/src", mod->dir);
        struct stat st;
        mod->has_src = (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode));
    }
    {
        char assets[MOD_PATH_MAX + 8];
        snprintf(assets, sizeof assets, "%s/assets", mod->dir);
        struct stat st;
        mod->has_assets = (stat(assets, &st) == 0 && S_ISDIR(st.st_mode));
        for (int i = 0; i < mod->n_levels; i++)
            (void)openpete_mods_custom_register(mod->id,
                                                mod->levels[i].name,
                                                mod->levels[i].files,
                                                assets,
                                                mod->levels[i].donor,
                                                mod->levels[i].replace,
                                                mod->levels[i].music,
                                                mod->levels[i].title,
                                                mod->log_tag);
    }
    if (!mod->has_src) {
        snprintf(so_path, sizeof so_path, "%s/.build/%s" MOD_DL_EXT,
                 mod->dir, mod->id);
        if (file_mtime(so_path) == (time_t)-1) {
            /* Declarative mods are legal: no src/ and no prebuilt
              * object means the manifest and its files are the whole
              * mod. */
            if (!run_entry) {
                OPL_INFO_F("mods", "loaded %s v%s (declarative; disabled)",
                           mod->id, mod->version);
                return 0;
            }
            OPL_INFO_F("mods", "loaded %s v%s (declarative)", mod->id,
                       mod->version);
            mod->entered = 1;
            mod_register_materials(mod);   /* a manifest-only mod IS its rows */
            mod_register_assets(mod);
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
    if (mod_check_abi(mod->dl, mod->id, so_path) != 0) {
        mod->dl = NULL; /* the object stays mapped; nothing in it runs */
        return -1;
    }
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
    if (!run_entry) {
        OPL_INFO_F("mods", "loaded %s v%s (%s; entry deferred until "
                   "enabled)", mod->id, mod->version, so_path);
        return 0;
    }
    if (mod_run_entry(mod) != 0) return -1;
    OPL_INFO_F("mods", "loaded %s v%s (%s)", mod->id, mod->version,
               so_path);
    mod_register_assets(mod);
    return 0;
}

/* First enable of a deferred-entry mod (tick boundary): enter deps
 * first (a library's entry must have run before its dependent's does;
 * deps stay disabled), then run entry and register the assets/ root.
 * Boot-scanned asset consumers (texture pack) only see the root after
 * a restart; per-play consumers (music) apply immediately. Late entry
 * registers at the chain tail = highest priority — restart to slot the
 * mod into enabled.txt order. */
static int mod_enter_deferred(struct openpete_mod* mod) {
    if (mod->entered) return 0;
    for (int d = 0; d < mod->n_deps; d++) {
        struct openpete_mod* dep = NULL;
        for (int i = 0; i < s_mod_count; i++)
            if (strcmp(s_mods[i].id, mod->deps[d]) == 0) {
                dep = &s_mods[i];
                break;
            }
        if (!dep) {
            OPL_ERROR_F("mods", "%s: dependency \"%s\" is not loaded",
                        mod->id, mod->deps[d]);
            return -1;
        }
        if (mod_enter_deferred(dep) != 0) return -1;
    }
    if (mod->dl) {
        if (mod_run_entry(mod) != 0) return -1;
    } else {
        mod->entered = 1; /* assets-only: the overlay root IS the mod */
        mod_register_materials(mod);
    }
    mod_register_assets(mod);
    s_tag_built = 0; /* the modset tag now includes this mod if divergent */
    return 0;
}

/* ---- enabled-list resolution ---------------------------------------- */

/* Dependency topo-sort (rung 5). DFS, stable in enabled order: a dep
 * loads before its dependents, so dependents keep HIGHER chain priority
 * than their libraries, and a library's entry has run by the time its
 * client's does. Missing dep / cycle = actionable error, dependent
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
            char where[MOD_PATH_MAX + 16];
            if (strcmp(s_order_src, "enabled.txt") == 0)
                snprintf(where, sizeof where, "%s/enabled.txt", c->mods_dir);
            else
                snprintf(where, sizeof where, "the --mods list");
            OPL_ERROR_F("mods", "\"%s\" requires \"%s\", which is not "
                        "enabled — add it to %s (ahead of "
                        "\"%s\") or install it; \"%s\" disabled",
                        c->pending[i].id, c->pending[i].deps[d],
                        where, c->pending[i].id, c->pending[i].id);
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
    int  boot_on;           /* in the enabled set (vs a deferred-entry
                             * load-all row) */
} mod_ref_t;

/* Resolve the mods dir (opts → env → <exe>/mods), unpack zips, and
 * produce the ordered enabled set (enabled.txt order = priority;
 * fallback: all discovered, lexicographic). Returns the count (0 = no
 * mods directory / nothing enabled: inert). openpete_mods_prepare and
 * openpete_mods_init both run this so they see the same set; everything
 * here is idempotent (zip unpack is content-stamped). */
/* Resolve the mods root: opts → env → the writable-data location.
 * Mods are user-authored and get compiled in place (.build/, .unpacked/),
 * so the root has to be writable: openpete_paths puts it next to the exe
 * in a portable install and under the per-user data directory otherwise,
 * which is what a read-only install medium leaves. Returns 0 with
 * mods_dir filled, -1 if no base path is resolvable. */
static int mods_root(const openpete_options_t* opts, char* mods_dir,
                     size_t mods_dir_cap) {
    const char* src = opts && opts->mods_dir ? opts->mods_dir
                                             : getenv("OPENPETE_MODS_DIR");
    if (src) {
        snprintf(mods_dir, mods_dir_cap, "%s", src);
        return 0;
    }
    return openpete_paths_data_dir("mods", mods_dir, mods_dir_cap, 0);
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

    int n_ids = 0, n_over = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t nl = strlen(e->d_name);
        if (nl > 4 && strcmp(e->d_name + nl - 4, ".zip") == 0) {
            char id[MOD_ID_MAX];
            snprintf(id, sizeof id, "%.*s", (int)(nl - 4), e->d_name);
            if (n_ids >= max) { n_over++; continue; }
            if (mod_unpack_zip(mods_dir, id) == 0)
                snprintf(ids[n_ids++], MOD_ID_MAX, "%s", id);
            continue;
        }
        char manifest[MOD_PATH_MAX];
        snprintf(manifest, sizeof manifest, "%s/%s/mod.toml", mods_dir,
                 e->d_name);
        if (file_mtime(manifest) == (time_t)-1) continue;
        if (n_ids >= max) { n_over++; continue; }
        snprintf(ids[n_ids++], MOD_ID_MAX, "%s", e->d_name);
    }
    closedir(d);
    /* Never truncate silently: which mods fall off a full scan depends on
     * raw readdir order, so the victim looks random to the user. */
    if (n_over)
        OPL_ERROR_F("mods", "mods dir has %d more mod(s) than the %d this "
                    "build can load — %d skipped in directory order",
                    n_over, max, n_over);
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

/* Trim leading/trailing blanks in place; ids compare exactly, so an
 * invisible "crt-beans " would skip as unknown. Returns the start. */
static char* trim_id(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* t = s + strlen(s);
    while (t > s && (t[-1] == ' ' || t[-1] == '\t')) *--t = 0;
    return s;
}

static int mods_discover(const openpete_options_t* opts,
                         char* mods_dir, size_t mods_dir_cap,
                         mod_ref_t* out, int max) {
    if (mods_root(opts, mods_dir, mods_dir_cap) != 0) return 0;

    /* --mods (env OPENPETE_MODS) replaces enabled.txt wholesale: a
     * comma-separated id list, same order-IS-priority semantics. An
     * empty list is meaningful — "enable nothing" — so it is checked
     * for presence, not truthiness. */
    const char* enabled_override =
        opts && opts->mods_enabled ? opts->mods_enabled
                                   : getenv("OPENPETE_MODS");
    s_order_src = enabled_override ? "--mods" : "enabled.txt";

    char ids[MODS_MAX][MOD_ID_MAX];
    int n_ids = mods_scan(mods_dir, ids, MODS_MAX);
    if (n_ids == 0) return 0;

    /* enabled.txt (one id per line, '#' comments) fixes the order; the
     * order IS the priority (later = higher). Fallback: lexicographic. */
    char order[MODS_MAX][MOD_ID_MAX];
    int n_order = 0;
    char en_path[MOD_PATH_MAX];
    snprintf(en_path, sizeof en_path, "%s/enabled.txt", mods_dir);
    FILE* en = enabled_override ? NULL : fopen(en_path, "r");
    if (enabled_override) {
        const char* p = enabled_override;
        while (n_order < MODS_MAX && *p) {
            const char* comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            char item[MOD_ID_MAX * 2];
            snprintf(item, sizeof item, "%.*s", (int)len, p);
            char* s = trim_id(item);
            if (strlen(s) >= MOD_ID_MAX)
                OPL_WARN_F("mods", "--mods: id \"%.32s...\" exceeds %d "
                           "characters — skipped", s, MOD_ID_MAX - 1);
            else if (*s)
                snprintf(order[n_order++], MOD_ID_MAX, "%s", s);
            if (!comma) break;
            p = comma + 1;
        }
        if (n_order == 0) {
            OPL_INFO_F("mods", "--mods lists no mods — none enabled "
                       "(mods dir: %s)", mods_dir);
            return 0;
        }
    } else if (en) {
        /* One WHOLE line per iteration: a comment (or any text) longer
         * than the buffer must not come back as fragments, or each tail
         * reads as a bogus id and warns at every boot.
         *
         * fgets drops the newline in two very different cases: the line
         * outran the buffer, or the file simply ended without a trailing
         * newline. Only the first is truncation — telling them apart by
         * "did fgets fill the buffer" rather than by the missing newline,
         * because a final line with no newline is the norm for editors
         * that don't add one, and treating it as truncated silently drops
         * whatever mod the user appended last. */
        char line[512];
        while (n_order < MODS_MAX && fgets(line, sizeof line, en)) {
            size_t got = strlen(line);
            int truncated = (got == sizeof line - 1) && line[got - 1] != '\n';
            if (truncated) { /* drain the rest of the over-long line */
                int c;
                while ((c = fgetc(en)) != EOF && c != '\n') { }
            }
            line[strcspn(line, "\r\n")] = 0;
            char* s = trim_id(line);
            if (!*s || *s == '#') continue;
            if (truncated) {
                OPL_WARN_F("mods", "enabled.txt: line starting \"%.32s\" is "
                           "longer than %d characters — skipped",
                           s, (int)(sizeof line - 1));
                continue;
            }
            if (strlen(s) >= MOD_ID_MAX) {
                OPL_WARN_F("mods", "enabled.txt: id \"%.32s...\" exceeds "
                           "%d characters — skipped", s, MOD_ID_MAX - 1);
                continue;
            }
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
            /* Say which case it is: a folder missing its manifest, or no
             * folder at all (typical after enabling a mod the deployed
             * mods dir doesn't have yet — rebuild/stage or copy it in). */
            char d[MOD_PATH_MAX];
            snprintf(d, sizeof d, "%s/%s", mods_dir, order[i]);
            if (file_mtime(d) == (time_t)-1)
                OPL_WARN_F("mods", "%s names \"%s\" but there is "
                           "no mods/%s/ folder (or <id>.zip) here — skipped "
                           "(mods dir: %s)", s_order_src, order[i], order[i],
                           mods_dir);
            else
                OPL_WARN_F("mods", "%s names \"%s\" but mods/%s/ has "
                           "no mod.toml — skipped", s_order_src, order[i],
                           order[i]);
            continue;
        }
        mod_ref_resolve(mods_dir, order[i], &out[n_out]);
        out[n_out].boot_on = 1;
        n_out++;
    }

    /* Deferred-entry load-all (interactive path): every other
     * discovered mod loads too — compiled and dlopen'd, entry deferred
     * to its first enable — so the overlay's Mods panel can list and
     * enable the whole directory. Scope guard: --mods/OPENPETE_MODS
     * (every harness/CI case) and headless runs keep exact-set
     * semantics; the no-enabled.txt fallback already enables all. */
    if (!enabled_override && en && opts && !opts->headless) {
        qsort(ids, (size_t)n_ids, sizeof ids[0], cmp_id_row);
        for (int i = 0; i < n_ids && n_out < max; i++) {
            int listed = 0;
            for (int j = 0; j < n_out && !listed; j++)
                if (strcmp(out[j].id, ids[i]) == 0) listed = 1;
            if (listed) continue;
            mod_ref_resolve(mods_dir, ids[i], &out[n_out]);
            out[n_out].boot_on = 0;
            n_out++;
        }
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
    /* Pure vk windowed = native is the sole display and the PSX ordering table
     * is not rasterized, so render-function overrides are visual no-ops. Under
     * psycross / ab / diff / headless the PSX path still rasterizes. */
    s_native_hides_psx = opts && opts->renderer == OPENPETE_RENDERER_VK
                              && !opts->headless;

    char mods_dir[MOD_PATH_MAX];
    mod_ref_t refs[MODS_MAX];
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, refs,
                               MODS_MAX);
    if (n_refs <= 0) return 0;

    char sdk_dir[MOD_PATH_MAX];
    mods_sdk_dir(sdk_dir, sizeof sdk_dir);
    snprintf(s_sdk_dir, sizeof s_sdk_dir, "%s", sdk_dir);

    /* Phase A: parse all discovered manifests (no loading yet).
     *
     * STATIC, not a local: struct openpete_mod is ~7 KB (it carries several
     * PATH_MAX buffers plus the [[material]] rows), so MODS_MAX of them is
     * over half a megabyte in one frame. As a local that silently outgrew
     * the Windows main-thread stack the moment the material rows were added
     * — boot died with EXCEPTION_STACK_OVERFLOW right after the PSX EXE
     * loaded, while Linux's 8 MB stack absorbed it and showed nothing.
     * Single-threaded boot-path code, so a static is the cheap fix and it
     * matches s_mods above. */
    static struct openpete_mod pending[MODS_MAX];
    int n_pending = 0;
    for (int i = 0; i < n_refs && n_pending < MODS_MAX; i++) {
        struct openpete_mod* mod = &pending[n_pending];
        memset(mod, 0, sizeof(*mod));
        if (parse_manifest(refs[i].dir, mod) != 0) continue;
        mod->boot_on = refs[i].boot_on;
        n_pending++;
    }

    /* Phase B: topo-sort by hard deps (see topo_visit above). */
    topo_ctx_t tc;
    memset(&tc, 0, sizeof tc);
    tc.pending   = pending;
    tc.n_pending = n_pending;
    tc.mods_dir  = mods_dir;
    for (int i = 0; i < n_pending; i++) (void)topo_visit(&tc, i);

    /* Entry policy: boot-enabled mods enter at load, and so do their
     * deps (a library's entry must run before its client's — the dep
     * enters but stays disabled if it wasn't in the enabled set
     * itself). Every other load-all row defers entry to its first
     * enable. */
    int must_enter[MODS_MAX] = {0};
    {
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int i = 0; i < n_pending; i++) {
                if (!pending[i].boot_on && !must_enter[i]) continue;
                for (int d = 0; d < pending[i].n_deps; d++) {
                    int j = topo_find(&tc, pending[i].deps[d]);
                    if (j >= 0 && !pending[j].boot_on && !must_enter[j]) {
                        must_enter[j] = 1;
                        changed = 1;
                    }
                }
            }
        }
    }

    /* Phase C: build + load in topo order; a load failure cascades to
     * dependents (their imports would dangle). */
    int loaded = 0, enabled = 0;
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
        if (load_mod(mod, sdk_dir,
                     pending[i].boot_on || must_enter[i]) != 0)
            continue;
        mod->enabled = mod->boot_on;
        load_ok[i] = 1;
        s_mod_count++;
        loaded++;
        if (mod->enabled) enabled++;
        /* Boot-disabled rows: make every registration surface agree.
         * Matters for entered-but-disabled deps: their entry registered
         * chains/hooks, which default to enabled. */
        if (!mod->enabled) {
            openpete_mods_chain_set_owner_enabled(mod->id, 0);
            openpete_mods_draw_set_owner_enabled(mod->id, 0);
            openpete_mods_registry_set_owner_enabled(mod->id, 0);
        }
    }

    /* Declared-bindings conflict scan: two enabled mods claiming the
     * same host key is legal (reads are non-exclusive) but surprising —
     * name both, with purposes, so the user can re-order or rebind.
     *
     * Compares the EFFECTIVE binding, not the declared one: rebinding a
     * mod's key in [keys.mod.<id>] is the documented way to resolve one
     * of these, so a scan blind to the override would keep warning about
     * a clash the user has already fixed. */
    for (int i = 0; i < s_mod_count; i++)
        for (int bi = 0; bi < s_mods[i].n_bindings; bi++) {
            char key_i[80];
            snprintf(key_i, sizeof key_i, "%s",
                     openpete_keymap_mod_effective_key(
                         s_mods[i].id, s_mods[i].bindings[bi].name,
                         s_mods[i].bindings[bi].key));
            if (strcmp(key_i, "(unbound)") == 0) continue;
            for (int j = i + 1; j < s_mod_count; j++)
                for (int bj = 0; bj < s_mods[j].n_bindings; bj++) {
                    const char* key_j = openpete_keymap_mod_effective_key(
                        s_mods[j].id, s_mods[j].bindings[bj].name,
                        s_mods[j].bindings[bj].key);
                    if (strcmp(key_i, key_j) == 0)
                        OPL_WARN_F("mods", "key \"%s\" bound by both %s "
                                   "(%s) and %s (%s)",
                                   key_i,
                                   s_mods[i].id,
                                   s_mods[i].bindings[bi].purpose,
                                   s_mods[j].id,
                                   s_mods[j].bindings[bj].purpose);
                }
        }

    if (enabled > 0)
        OPL_INFO_F("mods", "%d mod%s active (dir: %s)", enabled,
                   enabled == 1 ? "" : "s", mods_dir);
    if (loaded > enabled)
        OPL_INFO_F("mods", "%d more mod%s loaded disabled (entry "
                   "deferred) — enable via the M-overlay Mods panel or "
                   "the O-menu", loaded - enabled,
                   loaded - enabled == 1 ? "" : "s");
    toggle_parse_env();
    cfg_apply_schedule_parse();
    return loaded;
}

