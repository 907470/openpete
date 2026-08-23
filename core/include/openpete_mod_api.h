// openpete_mod_api.h — the MOD-FACING API surface (SDK header, api v1).
//
// This header is staged into <exe>/sdk/ and compiled into third-party mod
// shared objects. Everything a mod touches at runtime flows through the
// openpete_mod_api_t struct passed to its entry point — mods never link
// against engine symbols directly (portable across ELF/PE, no -rdynamic).
//
// A mod is a folder under mods/ with a mod.toml manifest; most mods need
// no code at all (assets/ + manifest — see mods/README.md). Mods that DO
// ship src/*.c are compiled on load by the engine toolchain into a
// per-mod shared object (core/mods/mod_loader.c). The entry point:
//
//     int openpete_mod_entry(const openpete_mod_api_t* api,
//                            openpete_mod_t* self);
//
// returns 0 on success (non-zero disables the mod with a log line).
//
// ── STABILITY ───────────────────────────────────────────────────────
// This struct is api v1 and APPEND-ONLY: entries never move, change
// signature, or disappear; new capability appends at the end under a
// bumped OPENPETE_MOD_API_VERSION. The manifest's `api = N` pin is the
// MINIMUM version the mod needs, not an exact match. Two deliberate
// exceptions to "nothing here ever breaks":
//   - CPUState override signatures (below) gain typed siblings when the
//     marshalling generator lands; the raw form stays.
//   - The UI annex (openpete_mod_ui.h) is a SEPARATE header on its own
//     version line and carries no stability promise at all.
//
// Override semantics:
// pre-hook / post-hook / full replacement are ONE mechanism, distinguished
// by where the override calls api->base(cpu) — last / first / never.
// base() falls through the priority chain and bottoms out at the
// recompiled original. Event detection: post-hook + state-diff around
// base() (no separate event subsystem — diff the world, e.g. g_LevelId
// under g_Gamestate == GS_Playing for "level ready").
//
// Raw cpu-context override signatures only for now (CPUState* — the
// MIPS O32 register file; args in cpu->a0..a3, return in cpu->v0).
// Prototype-driven natural-signature wrappers come with the marshalling
// generator (a later, additive rung).

#ifndef OPENPETE_MOD_API_H
#define OPENPETE_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/* Opaque per-mod handle owned by the loader. */
typedef struct openpete_mod openpete_mod_t;

/* An override body. Read/write guest state through cpu; call
 * api->base(cpu) to fall through to the next link / the original. */
typedef void (*openpete_mod_override_fn)(CPUState* cpu);

/* Per-present context handed to present hooks (see
 * register_present_hook below). */
typedef struct openpete_present_ctx {
    uint32_t tick;   /* canonical sim tick counter (openpete_frame)   */
    float    alpha;  /* sub-tick phase [0,1) between tick and tick+1;
                      * 0 on canonical presents                       */
    float    dt;     /* wall seconds since the previous present       */
    float    aspect; /* overlay-space width: x spans [0, aspect]      */
} openpete_present_ctx_t;

typedef void (*openpete_mod_present_fn)(const openpete_present_ctx_t* ctx);

/* Live enable/disable notification (see register_toggle_hook). Runs at
 * the tick boundary, AFTER the mod's registration surfaces flipped —
 * tick context, so guest reads/writes are legal. on=0 is the place to
 * UNDO apply-once guest effects (repointed tables, patched colours);
 * on=1 rarely needs work — level-scoped apply re-runs automatically. */
typedef void (*openpete_mod_toggle_fn)(CPUState* cpu, int on);

enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

/* guest_alloc flags. */
enum {
    OP_GALLOC_LOW = 1u << 0, /* allocate from the LOW free window below
                              * the game's EXE image (canonical RAM:
                              * savestate-carried, visible to every
                              * access path) instead of the >2MB
                              * expansion arena. For payloads whose
                              * format addresses memory through narrow
                              * fields (Spyro's animated-model frames
                              * hold 21-bit absolute vertex-stream
                              * addresses). The window is small (tens of
                              * KB) — park compact payloads, not
                              * scratch. */
};

/* Post-process injection points (postfx_register). Points are defined
 * SEMANTICALLY, not by resolution. */
enum {
    OP_POSTFX_COMPOSITE = 0, /* after scene+HUD compose, before the present
                              * blit — the first and default point */
    OP_POSTFX_SCENE     = 1, /* post-3D, pre-HUD: effects tint the world
                              * while the HUD stays crisp on top. Also
                              * gets real scene depth at op_scene_depth
                              * (WORLD-opaque pre-pass; terrain, not
                              * mobys). See mods/scene-sepia. */
    OP_POSTFX_PRESENT   = 2, /* post-blit, swapchain — NOT IMPLEMENTED.
                              * Registering here is refused. It would be
                              * identical to COMPOSITE until the present
                              * blit does something COMPOSITE cannot see
                              * (an upscaler), which is why it has not
                              * been built. */
};

/* Canonical sim-tick rate in Hz — exactly 1125000/37609 = 29.9130527… .
 *
 * The PSX runs 240p non-interlaced, so a frame is a full 263 scanlines and
 * the vertical rate is 2250000/37609 = 59.8261054… Hz; the game calls
 * VSync(2), so gameplay advances at half that. This is NOT 29.97 — that
 * figure derives from the interlaced 262.5-line field rate, which nothing
 * this engine outputs uses. A mod that converts ticks to seconds with 29.97
 * runs 0.19 % fast against game time (~1.1 s over a ten-minute run).
 *
 * A compile-time constant rather than an api entry: it never varies at
 * runtime, and pinning it in the header keeps mods from pasting a literal.
 * (--tick-scale stretches WALL time only; a tick is still a tick.) */
#define OPENPETE_TICK_HZ  (1125000.0 / 37609.0)
#define OPENPETE_VBL_HZ   (2250000.0 / 37609.0)

/* Channels == the renderer's feeder paths, and the values ARE the tag's
 * path nibble so a selector maps to tags without a translation table.
 * `docs/render-channel-census.md` says what each feeder can be keyed on. */
enum {
    OP_CHAN_MOBY     = 1,
    OP_CHAN_TERRAIN  = 2,
    OP_CHAN_PLAYER   = 3,
    OP_CHAN_FLAME    = 4,
    OP_CHAN_PARTICLE = 5,
    OP_CHAN_SKY      = 6,
    OP_CHAN_SHADOW   = 7,
    OP_CHAN_TRACER   = 8,
    OP_CHAN_PORTAL   = 9,   /* destination-sky previews inside a portal */
};
/* Portal preview kinds — the `ident` key for OP_CHAN_PORTAL. The game
 * draws a portal three ways across the approach, so a material that takes
 * only one of them POPS as the player walks up; -1 (all three) is almost
 * always what a portal mod means. */
enum {
    OP_PORTAL_FLAT    = 0,  /* far colour chip / close backstop */
    OP_PORTAL_DOME    = 1,  /* full gouraud dome, close up      */
    OP_PORTAL_BLENDED = 2,  /* fogged dome, medium cross-dissolve */
};
/* Retained: `path` used 0-based values before channels existed. */
enum { OP_RP_MOBY = 0, OP_RP_TERRAIN = 1 };
/* Refinements. Each pair is a NARROWING: setting neither member selects
 * both, which is what an unqualified "all terrain" has to mean. (The
 * textured/untextured pair used to be a two-way filter where unflagged
 * meant textured-only — so the plainest possible terrain selector
 * silently skipped every gouraud face, which is the LP tier and the
 * beyond-LOD band. Matching NARROWER than the author asked is as much a
 * bug as matching wider.) */
enum {
    OP_SEL_ENV_ANIM   = 1u << 0,  /* env-animated faces only */
    OP_SEL_HP_TIER    = 1u << 1,  /* high-poly tier only  } neither = both */
    OP_SEL_LP_TIER    = 1u << 2,  /* low-poly tier only   }               */
    OP_SEL_UNTEXTURED = 1u << 3,  /* gouraud faces only   } neither = both */
    OP_SEL_TEXTURED   = 1u << 4,  /* textured faces only  }               */
};
/* Every flag the engine understands. A selector carrying a bit outside
 * this mask is REFUSED rather than silently matching wider than the mod
 * author asked for — `struct_size` alone cannot catch a new flag bit,
 * because adding one never changes sizeof. */
#define OP_SEL_FLAGS_KNOWN (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | \
                            OP_SEL_LP_TIER  | OP_SEL_UNTEXTURED | \
                            OP_SEL_TEXTURED)

typedef struct openpete_mod_material_selector {
    /* sizeof(*this) at the mod's compile time; the engine refuses sizes it
     * does not know rather than reading past what the mod actually wrote. */
    uint32_t struct_size;
    int      channel;     /* OP_CHAN_* ; -1 = any channel */
    /* Channel-local identity — the tag's [15:0]. Meaning is per channel:
     * flame ribbon/caps, the particle (kind<<8 | sel) pair, shadow caster
     * kind. -1 = any. Channels whose feeder has no identity to give
     * (player, sky, tracer) only ever emit 0, so only -1 or 0 match. */
    int      ident;
    int      moby_class;  /* moby only; -1 = any */
    int      texture;     /* terrain texture-record id 0..127; -1 = any */
    int      level;       /* g_LevelId; -1 = any. Sector/texture ids are
                           * per-level, so a key without this matches the
                           * same id in EVERY level. */
    unsigned flags;       /* OP_SEL_* refinements */
} openpete_mod_material_selector_t;
/* Fill a selector with the match-everything defaults; a mod then narrows
 * only what it cares about. Zero-init would mean channel 0 / class 0. */
static inline openpete_mod_material_selector_t openpete_mod_material_sel_any(void) {
    openpete_mod_material_selector_t s;
    s.struct_size = (uint32_t)sizeof s;
    s.channel = -1; s.ident = -1; s.moby_class = -1;
    s.texture = -1; s.level = -1; s.flags = 0u;
    return s;
}

/* ── Refine: the middle layer of the selection seam ──────────────────
 * A selector decides WHICH geometry a material may claim; a refine
 * callback decides, per instance, whether it actually does — and with
 * what parameters. That is what makes "class 42, but only while it is
 * burning" expressible: the selector prunes to class 42 cheaply, the
 * callback reads that instance's state and declines the rest.
 *
 * The callback runs during EXTRACTION, in tick context, on the serial
 * emit path — guest RAM is readable and coherent through the ordinary
 * api->guest()/op_read_u32() accessors, which is why no CPUState
 * appears here. It is NOT a render hook and must not touch host
 * rendering state.
 *
 * SEMANTICS, NEVER CADENCE: the engine promises the verdict a material
 * carries is the one your callback most recently gave for that
 * instance. It does NOT promise how often you are called — results are
 * interned, and a channel that later banks geometry across presents
 * will re-resolve from identity rather than re-invoke you. So a
 * callback must be a pure function of the key plus tick-stable guest
 * state. Counting calls to infer "instances drawn" is measuring the
 * engine's memoization, not the scene. */

/* Per-channel key versions — bumped when a channel's key members change
 * meaning. The engine stamps the live one into `version`; a mod that
 * cares can refuse a version it was not written against. */
#define OPENPETE_MATERIAL_KEY_VER_MOBY    1u
#define OPENPETE_MATERIAL_KEY_VER_TERRAIN 1u

/* What the engine knows about one instance at refine time. The engine
 * fills every field; `channel` selects which union member is live. */
typedef struct openpete_mod_material_key {
    uint32_t struct_size;   /* sizeof at ENGINE compile time */
    uint32_t version;       /* OPENPETE_MATERIAL_KEY_VER_* for `channel` */
    int      channel;       /* OP_CHAN_* */
    union {
        struct {
            uint32_t vaddr;      /* guest address of the moby record */
            int      moby_class; /* m_Class */
        } moby;
        struct {
            int      texture;    /* Tiledef id, or -1 for the untextured band */
            unsigned flags;      /* the OP_SEL_* bits describing this face */
        } terrain;
    } u;
} openpete_mod_material_key_t;

/* Return the number of bytes written to `out` (0 = claim it, no
 * parameters), or -1 to DECLINE — that instance keeps stock shading.
 * `cap` is OPENPETE_MATERIAL_UNIFORMS_MAX; never write past it. */
typedef int (*openpete_mod_refine_fn)(const openpete_mod_material_key_t* key,
                                      void* out, uint32_t cap);

/* 5: the material shading contract inverted — the engine owns main() and a
 * material supplies op_material(vec4 base). Materials written against api 4
 * (their own main() + outColor) fail to COMPILE against the api-5 contract,
 * deliberately: silently keeping them would keep them shading without the
 * perspective divide and without og's per-pixel painter verdicts. */
#define OPENPETE_MOD_API_VERSION 5u

typedef struct openpete_mod_api {
    uint32_t api_version;   /* == OPENPETE_MOD_API_VERSION (the struct is
                             * append-only: a mod's manifest `api` pin is the
                             * MINIMUM version its calls need; check
                             * `api_version < N`, never `!=`) */

    /* ── Pillar 1: the override chain ────────────────────────────────
     * Attach an override to a guest function by symbol name (named tier
     * — resolved through the SDK symbol table, e.g. "CameraUpdate") or
     * by PSX vaddr (raw tier — region/binary-pinned; declare
     * tier = "raw" in the manifest). Returns 0 on success, non-zero if
     * the name is unknown / args invalid.
     * Priority: the enabled-list order IS the priority — overrides from
     * later-listed mods run first; base() walks toward earlier mods and
     * bottoms out at the recompiled original. */
    int (*override_addr)(openpete_mod_t* self, uint32_t addr,
                         openpete_mod_override_fn fn);
    int (*override_name)(openpete_mod_t* self, const char* name,
                         openpete_mod_override_fn fn);

    /* Fall through to the next override in the chain (or the recompiled
     * original). Only valid while an override registered by this mod is
     * on the call stack. May be called 0, 1, or N times; each call runs
     * the continuation once. */
    void (*base)(CPUState* cpu);

    /* Invoke a guest function by vaddr through the dispatch layer (the
     * callee's own override chain runs — calls see the same modded
     * world as the game). Argument/return marshalling is the caller's
     * job: write cpu->a0..a3 first, read cpu->v0 after. Only valid on
     * the sim thread with a live cpu (i.e. inside an override). */
    void (*call)(CPUState* cpu, uint32_t addr);

    /* ── Pillar 2: guest memory ──────────────────────────────────────
     * Translate a guest vaddr (kuseg/kseg0/kseg1 RAM) to a host pointer
     * into the 2MB guest-RAM mirror, or NULL if out of range. Guest RAM
     * is little-endian and so is every supported host, so typed reads/
     * writes through the pointer see what the game sees. Global
     * addresses: OP_GADDR_* in openpete_sdk_symbols.h. Reminder: values
     * INSIDE guest structs that are pointers hold guest vaddrs —
     * re-translate before chasing. Sim-thread use only. */
    void* (*guest)(uint32_t vaddr);

    /* Inverse of guest(): host pointer into the guest-RAM mirror →
     * kseg0 vaddr, or 0 if the pointer is not inside guest RAM. */
    uint32_t (*guest_addr)(const void* host);

    /* Allocate `size` bytes at a GUEST-addressable vaddr with a typed
     * HOST view of the same bytes. Default arena: the >2MB expansion
     * window (NOT savestate-carried — re-derive persistent contents
     * after a state load; recompiled sites that MASK addresses to 2MB
     * cannot see it). OP_GALLOC_LOW allocates from the low free window
     * instead (canonical, savestate-carried, but tens of KB). Session-
     * lifetime bump, no free; align must be a power of two (0 = arena
     * default). Returns the guest vaddr, 0 = exhausted/unavailable.
     * TICK context only. */
    uint32_t (*guest_alloc)(openpete_mod_t* self, uint32_t size,
                            uint32_t align, uint32_t flags,
                            void** host_view);

    /* ── Pillar 3: lifecycle and identity ────────────────────────────
     * printf-style logging, tagged "mod:<id>". level = OP_MOD_LOG_*. */
    void (*log)(openpete_mod_t* self, int level, const char* fmt, ...);

    /* Per-mod persistent data directory (mods/<id>/data, created on
     * first call). Returns an engine-owned absolute path string. Mods
     * are native code — plain stdio inside it is the intended use. */
    const char* (*data_dir)(openpete_mod_t* self);

    /* Live-toggle notification: called at the tick boundary each time
     * THIS mod's live toggle actually flips, after the flip. The engine
     * freezes a disabled mod's registrations but never rewinds guest
     * state — a mod whose entry or hooks WROTE guest state restores the
     * stock bytes in on=0. This is the mod's unload story. One
     * registration per mod (re-register replaces). Returns 0. */
    int (*register_toggle_hook)(openpete_mod_t* self,
                                openpete_mod_toggle_fn fn);

    /* ── Pillar 4: settings (config.toml + [[config]] schema) ────────
     * Read a value from mods/<id>/config.toml. Keys are dotted paths:
     * "chests.collision" reads key `collision` in table [chests]. `def`
     * is returned when the file, the key, or a type-compatible value is
     * absent. The file is parsed once, right before entry runs — read
     * config at entry and branch there; when the user commits a change
     * in the Mods panel (rendered from the manifest's [[config]] schema
     * rows) the engine rewrites config.toml and RELOADS the mod, so
     * entry re-reads fresh values. Numeric coercion: bools read as 0/1
     * through config_int/config_float; ints read through config_float
     * (and floats truncate through config_int); strings never coerce.
     * config_str copies at most cap-1 chars + NUL into buf and returns
     * 1 if the key was present (0 = default copied). */
    int     (*config_bool)(openpete_mod_t* self, const char* key, int def);
    int64_t (*config_int)(openpete_mod_t* self, const char* key,
                          int64_t def);
    double  (*config_float)(openpete_mod_t* self, const char* key,
                            double def);
    int     (*config_str)(openpete_mod_t* self, const char* key,
                          const char* def, char* buf, int cap);

    /* Print a status line into this mod's section of the Mods panel —
     * the kernel's answer to "my UI is just text". Legal from any
     * context (host-side only). Lines ACCUMULATE within a tick and
     * persist until the next tick in which the mod calls ui_status
     * again — a one-shot line printed at entry or on a level event
     * stays visible; a per-tick printer repaints its whole block each
     * tick. Past ~24 lines the block truncates with an ellipsis marker
     * (genuinely custom panels belong to the UI annex,
     * openpete_mod_ui.h — separately versioned, no stability promise). */
    void (*ui_status)(openpete_mod_t* self, const char* fmt, ...);

    /* ── Pillar 5: present and screen-space draw ─────────────────────
     * Register a per-present hook: runs once per presented frame (30 Hz
     * canonical + every sub-tick present at high --render-fps) with the
     * present context. RENDER-PURE by contract AND enforcement: while a
     * present hook runs, guest access / game calls / base() / override
     * registration are refused with an error — presents are
     * non-deterministic in count and timing, so anything sim-affecting
     * belongs in tick hooks. Publish state from tick hooks into your
     * own variables; evaluate your own curves at ctx->alpha here (the
     * engine never interpolates mod visuals). Never runs headless. */
    int (*register_present_hook)(openpete_mod_t* self,
                                 openpete_mod_present_fn fn);

    /* Screen-space draws (native vk renderer). Coordinates are OVERLAY
     * SPACE: window-height fractions, origin top-left, +y down; x spans
     * [0, aspect], y spans [0, 1]. Host-side only (zero guest-RAM
     * writes — a drawing observer stays sim-neutral by construction).
     * Immediate mode, two contexts: called from a TICK context the
     * command shows for all of that tick's presents (redraw every
     * tick); from a PRESENT hook it lives one present. draw_text
     * glyphs: digits, A-Z (lowercase folds), % / ? + ^ . ' and ':' —
     * Spyro's own 3D gold text mobys; rgb is the glyph tint (e.g.
     * 0x605010 for HUD gold); text appears once the game has loaded the
     * glyph font. draw_rect rgba: R,G,B,A bytes (alpha currently
     * ignored: opaque). Headless and PsyCross-only runs consume
     * nothing. */
    void (*draw_text)(float x, float y, float cap_h, uint32_t rgb,
                      const char* str);
    void (*draw_rect)(float x, float y, float w, float h, uint32_t rgba);

    /* ── Pillar 6: post-process shaders ──────────────────────────────
     * Register a GLSL shader by path relative to the mod directory
     * (e.g. "shaders/crt.frag"); stage from the extension (.vert /
     * .frag — postfx uses .frag). The ENGINE compiles it to SPIR-V on
     * load (in-process glslang, vulkan1.0 dialect), content-hash cached
     * under .build/shaders/. A prebuilt "<path>.spv" next to a MISSING
     * source loads instead but requires a "<path>.spv.ihash" sidecar
     * matching the current shader-interface hash. `#include
     * "openpete_shader_api.glsl"` resolves to the SDK interface header.
     * Returns a shader handle >= 0, or -1 (missing file / compile error
     * — see .build/shaders/<name>.log; the mod keeps loading, the
     * effect is simply unavailable). Entry/tick context only. Sources
     * join the dev hot-reload watch. */
    int (*shader_register)(openpete_mod_t* self, const char* path);

    /* Fullscreen fragment passes at a named injection point
     * (OP_POSTFX_*). Pass order across mods = enabled-list priority;
     * `order` breaks ties within a mod (lower runs first). The pass
     * samples the previous pass (set 0) and receives the engine UBO
     * (time, tick, interp alpha, resolution — see
     * sdk/shaders/openpete_shader_api.glsl) plus the mod's uniform
     * block. postfx_set_uniforms COPIES data on call (<= 1024 bytes;
     * push from tick or present hooks). postfx_register returns an fx
     * handle >= 0 (registered DISABLED; call postfx_enable) or -1.
     * Entry/tick context for register; enable/uniforms any context. */
    int (*postfx_register)(openpete_mod_t* self, int frag_shader,
                           int point, int order);
    int (*postfx_enable)(openpete_mod_t* self, int fx, int enabled);
    int (*postfx_set_uniforms)(openpete_mod_t* self, int fx,
                               const void* data, uint32_t size);

    /* ── Pillar 7: content — new moby types ──────────────────────────
     * Register a NEW moby type: the engine claims a class id that is
     * FREE in the current level, seeds the model row from donor_class
     * (the new type renders/collides with the donor's model), and calls
     * update_fn once per live instance per sim tick, AFTER the game's
     * own update. Ids are LEVEL-SCOPED — invalidated on level change
     * and re-claimed when gameplay resumes; write level-independent
     * behavior against the moby pointer, not the id.
     *
     * moby_type_id returns the currently claimed class id, -1 if the
     * type isn't live in this level (yet) — a changed id is also the
     * mod's level-change epoch signal.
     *
     * spawn_moby instantiates one through the game's own spawner (full
     * donor-class init; placed at Spyro when no parent), rebranded to
     * the registered type. Returns the guest moby vaddr (position at
     * +0x0C, 3×i32) or 0 while the type isn't live / the pool is full.
     * TICK context only. */
    int (*register_moby_type)(openpete_mod_t* self, const char* name,
                              int donor_class,
                              void (*update_fn)(CPUState* cpu,
                                                uint32_t moby_vaddr));
    int (*moby_type_id)(openpete_mod_t* self, const char* name);
    uint32_t (*spawn_moby)(openpete_mod_t* self, CPUState* cpu,
                           const char* name);

    /* ── Pillar 8: audio ─────────────────────────────────────────────
     * The `name` domain differs per verb: sfx_play resolves in THIS
     * MOD's assets/sounds/; sfx_replace resolves in the GAME's
     * registered sound names.
     *
     * sfx_replace: substitute the sample behind a named game sound.
     * body = raw SPU-ADPCM frames (no VAG header, final block end-flag
     * 0x01), copied by the engine. pitch = SPU units (0x1000 =
     * 44.1 kHz) used instead of the game's pitch computation; 0 = keep
     * the game's pitch. Voice-layer names ("gem_pickup", ...) are
     * replaced host-side at key-on — guest RAM, SPU banks and the
     * gameplay rand() stream stay byte-stock, so the substitution is
     * sim-neutral by construction. Names the game layer marks as
     * def-shaped (per-level SoundDefinition sounds, e.g.
     * "chest_break_wooden") route through a guest def repoint instead
     * and are sim-DIVERGENT — the log line says which route was taken.
     * Unknown name => -1.
     *
     * sfx_play: play `assets/sounds/<name>.wav` from this mod's
     * directory as a HOST-side additive one-shot (never the SPU —
     * sim-neutral by construction, legal from tick and present hooks).
     * gain 0..4, 1.0 = as-authored; WAVs are cached after the first
     * play. 0 ok / -1 failed (missing file, no audio — logged, never
     * fatal). */
    int (*sfx_replace)(openpete_mod_t* self, const char* name,
                       const void* adpcm_body, uint32_t len,
                       uint16_t pitch);
    int (*sfx_play)(openpete_mod_t* self, const char* name, float gain);

    /* ── Declared input bindings ─────────────────────────────────────
     * Resolve a [[binding]] declared in this mod's mod.toml
     * ({name, key, purpose}) to the live host key state (1 = held).
     * Declaring bindings surfaces conflicts across the enabled set at
     * load time and lets the user rebind without touching the mod (the
     * openpete.toml [keys.mod.<id>] table overrides the declared key,
     * reaches gamepad sources like "pad:north", and takes "none" to
     * disable). Reading host input in a TICK hook intentionally
     * diverges gameplay (the practice-ROM pattern); display-only reads
     * belong in present hooks. Undeclared name = error log + 0. */
    int (*binding_down)(openpete_mod_t* self, const char* name);

    /* ---- pillar 9: game data + texture packs (api 2) ------------------- */

    /* Read one level's raw Data blob from the USER'S OWN mounted disc into
     * buf (cap bytes); buf NULL queries the length. Returns the blob length,
     * or <0 when no game layer provides level data / the id is unknown. Lets
     * a mod borrow authored game content (another level's sky, palette,
     * geometry) without shipping derived data. */
    int (*read_level_data)(openpete_mod_t* self, int level_id,
                           void* buf, uint32_t cap);

    /* Retarget the texture-pack fog ambience: fogged replacement texels
     * blend toward scale*stock+offset (offsets in 0..255 units) instead of
     * the stock ladder colours — for packs whose art re-grades the level.
     * NULL/NULL restores stock. Per-texture grade fits still take
     * precedence where they have signal; this is the fallback ambience.
     * Display-only. */
    int (*texpack_fog_shift)(openpete_mod_t* self, const float scale_rgb[3],
                             const float offset_rgb[3]);

    /* ---- pillar 10: materials (api 3) --------------------------------- */

    /* Attach a fragment shader to a SET of rendered geometry, chosen by the
     * data that defines it: a render channel (moby, terrain, player, flame,
     * particle, sky, shadow, tracer), optionally narrowed by that channel's
     * own keys — moby class, terrain texture/tier, a channel-local identity,
     * a level. Build the selector with openpete_mod_material_sel_any() and
     * narrow only what you mean; the engine REFUSES a selector it cannot
     * fully honour rather than matching wider than you asked.
     *
     * `shader` is a frag handle from shader_register. The shader reshades
     * within whatever draw group the geometry already belongs to: it cannot
     * change blending, depth policy or draw order, so a material can never
     * reorder the scene. Unmatched or failed builds keep stock shading —
     * a material is never a way to make something invisible.
     *
     * Returns a material handle, or -1. Registration starts ENABLED. */
    int (*material_register)(openpete_mod_t* self,
                             const openpete_mod_material_selector_t* sel,
                             int shader);

    /* Turn one registered material on or off. Cheap and callable from any
     * context — this is what a [[config]] toggle drives. */
    int (*material_enable)(openpete_mod_t* self, int material, int on);

    /* Set the uniform block the material's shader reads (std140, up to
     * OPENPETE_MATERIAL_UNIFORMS_MAX bytes). May be called per tick; for
     * smoothly time-varying effects prefer driving them from the engine
     * UBO's time in the shader over re-pushing a block every present. */
    int (*material_set_params)(openpete_mod_t* self, int material,
                               const void* block, uint32_t len);

    /* Register a material whose per-instance verdict comes from a refine
     * callback (api 4). Identical to material_register except the engine
     * calls `refine` once per candidate instance during extraction: it
     * returns a uniform block for that instance, or -1 to leave it stock.
     *
     * Instances whose blocks are byte-identical share one slot and stay in
     * ONE batch, so a callback that returns few distinct outcomes is cheap
     * and one that returns a distinct block per instance costs a draw
     * apiece. Scope the SELECTOR as tightly as you can and let the callback
     * decide the rare cases — that ordering is the whole point of the
     * three-layer seam.
     *
     * Refine is available on the channels that have a stable per-instance
     * identity (moby today; terrain is gated on the retained-cache work).
     * On a channel without one, register declaratively and drive the
     * material with material_set_params instead. */
    int (*material_register_refine)(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int shader, openpete_mod_refine_fn refine);
} openpete_mod_api_t;

/* Convenience typed guest accessors. */
static inline uint32_t op_read_u32(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint32_t*)p : 0u;
}
static inline void op_write_u32(const openpete_mod_api_t* api, uint32_t va,
                                uint32_t v) {
    void* p = api->guest(va);
    if (p) *(uint32_t*)p = v;
}
static inline int32_t op_read_s32(const openpete_mod_api_t* api, uint32_t va) {
    return (int32_t)op_read_u32(api, va);
}
static inline uint8_t op_read_u8(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint8_t*)p : 0u;
}
static inline void op_write_u8(const openpete_mod_api_t* api, uint32_t va,
                               uint8_t v) {
    void* p = api->guest(va);
    if (p) *(uint8_t*)p = v;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_API_H */
