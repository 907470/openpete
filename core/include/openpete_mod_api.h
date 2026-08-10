// openpete_mod_api.h — the MOD-FACING API surface (SDK header).
//
// This header is staged into <exe>/sdk/ and compiled into third-party mod
// shared objects. Everything a mod touches at runtime flows through the
// openpete_mod_api_t struct passed to its entry point — mods never link
// against engine symbols directly (portable across ELF/PE, no -rdynamic).
//
// A mod is a folder under mods/ with a mod.toml manifest and src/*.c,
// compiled on load by the engine toolchain into a per-mod shared object
// (see core/mods/mod_loader.c). The entry point:
//
//     int openpete_mod_entry(const openpete_mod_api_t* api,
//                            openpete_mod_t* self);
//
// returns 0 on success (non-zero disables the mod with a log line).
//
// Override semantics:
// pre-hook / post-hook / full replacement are ONE mechanism, distinguished
// by where the override calls api->base(cpu) — last / first / never.
// base() falls through the priority chain and bottoms out at the
// recompiled original. Event detection: post-hook + state-diff around
// base() (no separate event subsystem).
//
// v0 scope: raw cpu-context override signatures only (CPUState* — the
// MIPS O32 register file; args in cpu->a0..a3, return in cpu->v0).
// Prototype-driven natural-signature wrappers come with the marshalling
// generator (P1, later rung).

#ifndef OPENPETE_MOD_API_H
#define OPENPETE_MOD_API_H

#include <stdint.h>
#include <stddef.h>  /* offsetof — OP_STATE_FIELD */

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

/* One play of a game-named sound, seen host-side at voice key-on (see
 * sfx_replace/sfx_hook below). */
typedef struct openpete_sfx_event {
    const char* name;        /* game-registered sound name */
    uint16_t pitch;          /* SPU units, 0x1000 = 44.1 kHz; writable */
    uint8_t  suppress;       /* set nonzero to mute this play */
    uint32_t play_index;     /* nth play of this name this session */
    uint32_t ms_since_prev;  /* host ms since the previous play */
} openpete_sfx_event_t;

/* A level-ready callback (see on_level below). */
typedef void (*openpete_mod_level_fn)(CPUState* cpu, int level_id);

/* ── Per-mod UI (see register_ui below) ──────────────────────────────
 * Immediate-mode widget vtable the engine's dev overlay implements —
 * mods never see the underlying UI library. Widgets returning int
 * report 1 when the value changed this frame (checkbox/slider/combo
 * write through their pointer), buttons report 1 on click. tooltip()
 * annotates the PREVIOUS widget on hover. The vtable is engine-owned
 * and append-only, like the api struct. */
typedef struct openpete_mod_ui {
    void (*text)(const char* fmt, ...);          /* printf-style line   */
    void (*text_disabled)(const char* fmt, ...); /* dimmed line         */
    int  (*button)(const char* label);
    int  (*checkbox)(const char* label, int* v);
    int  (*slider_int)(const char* label, int* v, int min, int max);
    int  (*slider_float)(const char* label, float* v, float min,
                         float max);
    int  (*combo)(const char* label, int* idx, const char* const* items,
                  int count);
    int  (*input_text)(const char* label, char* buf, int cap);
    void (*separator)(void);
    void (*same_line)(void);
    void (*tooltip)(const char* text);
    void* _reserved[8]; /* future widgets; always NULL today */
} openpete_mod_ui_t;

/* The mod's section body. Runs only while the overlay is open AND the
 * mod is enabled; draw with `ui`, `user` is the register_ui cookie. */
typedef void (*openpete_mod_ui_fn)(const openpete_mod_ui_t* ui,
                                   void* user);

/* Live enable/disable notification (see register_toggle_hook). Runs at
 * the tick boundary, AFTER the mod's registration surfaces flipped —
 * tick context, so guest reads/writes are legal. on=0 is the place to
 * UNDO apply-once guest effects (repointed tables, patched colours);
 * on=1 rarely needs work — level-scoped apply re-runs automatically
 * (the level-ready hook re-fires for the level in progress). */
typedef void (*openpete_mod_toggle_fn)(CPUState* cpu, int on);

/* Savestate participation (see register_savestate_hooks): serialize the
 * mod's own non-canonical state alongside the engine snapshot. save
 * returns bytes written into buf (<= cap, cap is 64 KiB) or -1; load
 * receives the blob saved by the same mod id.
 *
 * REINIT SIGNAL: load may also be invoked as load(NULL, 0), meaning "the
 * engine restored a state that carries NO usable blob for you" (state
 * predates the mod, or your blob was dropped by the state-ABI verdict
 * below). Treat it as: the guest world just changed under you — reset
 * host statics to a from-scratch posture and re-derive from guest RAM. */
typedef int  (*openpete_mod_state_save_fn)(void* buf, int cap);
typedef void (*openpete_mod_state_load_fn)(const void* buf, int len);

/* ── Savestate state-ABI declaration (compile-time verified) ──────────
 * A mod that persists state via register_savestate_hooks SHOULD also
 * declare its state layout with OPENPETE_MOD_STATE below. The macro bakes
 * a {offsetof, sizeof} field table + version into an exported const
 * descriptor (`openpete_mod_stateabi`, resolved by the loader at dlopen);
 * the engine fingerprints it and records the fingerprint in every state
 * file. On load:
 *   fp match                     -> blob delivered to load()
 *   fp mismatch + DROP_OK        -> blob dropped, load(NULL,0) reinit
 *   fp mismatch, no DROP_OK      -> WHOLE state load refused (no corruption)
 *   no descriptor (opaque tier)  -> blob delivered only under an exact
 *                                   build-key match, else dropped + reinit
 * So: declaring buys your states longevity across recompiles that keep
 * the layout; not declaring pins them to the exact build. */
enum {
    OP_MOD_STATE_DROP_OK = 1u << 0, /* on fp mismatch: drop my blob and
                                     * reinit me instead of refusing the
                                     * whole load */
    OP_MOD_RA_PURE       = 1u << 1, /* tick hooks touch guest state only —
                                     * safe to re-run speculatively
                                     * (runahead) with no suppression */
    OP_MOD_RA_HOST_FX    = 1u << 2, /* tick hooks have host side effects
                                     * (sfx_play, draw, logs) that the
                                     * engine can suppress during
                                     * speculative ticks */
};

typedef struct openpete_mod_stateabi {
    uint32_t magic;              /* 'SABI' = 0x53414249 */
    uint32_t abi_version;        /* mod-declared state schema version */
    uint32_t abi_size;           /* sizeof(state struct) */
    uint32_t flags;              /* OP_MOD_STATE_* | OP_MOD_RA_* */
    uint32_t n_fields;
    const uint32_t (*fields)[2]; /* {offsetof, member sizeof} table */
} openpete_mod_stateabi_t;

#define OP_STATE_FIELD(S, f) { (uint32_t)offsetof(S, f), \
                               (uint32_t)sizeof(((S*)0)->f) }

/* One per mod, file scope. FIELDS... is a brace list of OP_STATE_FIELD.
 * Example:
 *   typedef struct { uint32_t ticks; uint32_t park_vaddr; } my_state_t;
 *   OPENPETE_MOD_STATE(my_state_t, 1, OP_MOD_RA_PURE,
 *       OP_STATE_FIELD(my_state_t, ticks),
 *       OP_STATE_FIELD(my_state_t, park_vaddr))
 * Tip: put host-cached guest vaddrs (arena parks) IN the struct — they
 * restore with the state, fixing the stale-park-pointer class outright. */
#define OPENPETE_MOD_STATE(S, ver, fl, ...)                               \
    _Static_assert(sizeof(S) <= 64 * 1024,                                \
                   "mod state blob exceeds the 64KiB cap");               \
    static const uint32_t op_stateabi_fields_[][2] = { __VA_ARGS__ };     \
    const openpete_mod_stateabi_t openpete_mod_stateabi = {               \
        0x53414249u, (ver), (uint32_t)sizeof(S), (fl),                    \
        (uint32_t)(sizeof(op_stateabi_fields_) / sizeof(uint32_t[2])),    \
        op_stateabi_fields_ };

/* Capability-only form: declare runahead safety (OP_MOD_RA_*) for a mod
 * with NO persisted state. Required for runahead to arm — the engine
 * refuses speculation unless every enabled mod declares RA_PURE or
 * RA_HOST_FX. */
#define OPENPETE_MOD_RA_CAPS(fl)                                          \
    const openpete_mod_stateabi_t openpete_mod_stateabi = {               \
        0x53414249u, 0u, 0u, (fl), 0u, (const uint32_t (*)[2])0 };

enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

/* ── Mod shader types (see the shader sections in the struct below) ──
 * Shaders are GPU-only: they can never touch guest RAM, so every shader
 * tier is sim-neutral by construction. The C side may READ gameplay
 * state to feed uniforms; nothing flows back. */

/* Post-process injection points (postfx_register). Points are defined
 * SEMANTICALLY, not by resolution. */
enum {
    OP_POSTFX_COMPOSITE = 0, /* after scene+HUD compose, before the present
                              * blit — the first and default point */
    OP_POSTFX_SCENE     = 1, /* post-3D, pre-HUD (not yet available) */
    OP_POSTFX_PRESENT   = 2, /* post-blit, swapchain (not yet available) */
};

/* Vertex formats for custom-pipeline draws (pipeline_register). */
enum {
    OP_VTX_POS_UV_RGBA = 0,  /* openpete_mod_vertex_t below — 24 bytes */
};

/* The OP_VTX_POS_UV_RGBA vertex. The MOD's vertex shader owns the
 * interpretation of pos (attributes arrive untransformed: location 0 =
 * vec3 pos, 1 = vec2 uv, 2 = vec4 rgba/255; the engine UBO — time,
 * tick, alpha, resolution — is at set 3 binding 0, see
 * sdk/shaders/openpete_shader_api.glsl). rgba byte order R,G,B,A. */
typedef struct openpete_mod_vertex {
    float    x, y, z;
    float    u, v;
    uint32_t rgba;
} openpete_mod_vertex_t;

/* Blend modes — the engine's PSX-derived set. */
enum {
    OP_BLEND_OPAQUE = 0,
    OP_BLEND_AVG    = 1,     /* (back+front)/2 */
    OP_BLEND_ADD    = 2,
    OP_BLEND_SUB    = 3,
    OP_BLEND_ADD4   = 4,     /* back + front/4 */
};

/* Custom-pipeline description (pipeline_register). Append-only, like
 * the api struct itself; _reserved must be zero. */
typedef struct openpete_mod_pipeline_desc {
    int      vert_shader;    /* shader_register handle of a .vert */
    int      frag_shader;    /* shader_register handle of a .frag */
    int      vertex_format;  /* OP_VTX_* */
    int      blend;          /* OP_BLEND_* */
    int      depth_test;     /* nonzero = test against scene depth */
    int      depth_write;    /* nonzero = write scene depth */
    uint32_t _reserved[4];   /* zero */
} openpete_mod_pipeline_desc_t;

/* Material-selector target paths + flags (material_register). */
enum {
    OP_RP_MOBY    = 0,
    OP_RP_TERRAIN = 1,
};
enum {
    OP_SEL_ENV_ANIM = 1u << 0,  /* env-animated tile set (water/lava/portals) */
    OP_SEL_HP_TIER  = 1u << 1,  /* high-poly terrain tier only */
    OP_SEL_LP_TIER  = 1u << 2,  /* low-poly terrain tier only */
    OP_SEL_UNTEXTURED = 1u << 3, /* terrain: the untextured (beyond-LOD
                                  * gouraud) band only; a per-instance
                                  * callback here receives moby_vaddr 0 —
                                  * decline (-1) for stock, or return the
                                  * band's uniform block */
};

/* Declarative subset selector: which of the engine's own draws a
 * registered material overrides. -1 = wildcard. */
typedef struct openpete_mod_material_selector {
    int      path;        /* OP_RP_* ; -1 = any path */
    int      moby_class;  /* Moby m_Class; -1 = any (terrain: ignored) */
    int      texture;     /* terrain Tiledef/texture-record id 0..127 (a face's
                           * word2 & 0x7F — the record index the texture-dump
                           * tooling reports); -1 = any. Terrain textured faces
                           * only; a per-instance callback here receives the
                           * texture id in the moby_vaddr argument. */
    unsigned flags;       /* OP_SEL_* refinements, 0 = none */
} openpete_mod_material_selector_t;

/* Per-instance material callback: runs host-side at extraction for each
 * matching instance. Fill uniform_out (<= cap bytes) and return the
 * byte count to override this instance, or return -1 to decline (stock
 * shading). READ-ONLY guest access; render-pure context rules apply. */
typedef int (*openpete_mod_material_fn)(CPUState* cpu, uint32_t moby_vaddr,
                                        void* uniform_out, uint32_t cap);

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

#define OPENPETE_MOD_API_VERSION 4u

typedef struct openpete_mod_api {
    uint32_t api_version;   /* == OPENPETE_MOD_API_VERSION */

    /* Attach an override to a guest function by PSX vaddr (raw tier —
     * region/binary-pinned) or by symbol name (named tier — resolved
     * through the SDK symbol table, e.g. "CameraUpdate"). Returns 0 on
     * success, non-zero if the name is unknown / args invalid.
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

    /* Resolve a named symbol to its PSX vaddr (0 = unknown). */
    uint32_t (*resolve)(const char* name);

    /* printf-style logging, tagged "mod:<id>". level = OP_MOD_LOG_*. */
    void (*log)(openpete_mod_t* self, int level, const char* fmt, ...);

    /* Translate a guest vaddr (kuseg/kseg0/kseg1 RAM) to a host pointer
     * into the 2MB guest-RAM mirror, or NULL if out of range. Guest RAM
     * is little-endian and so is every supported host, so typed reads/
     * writes through the pointer see what the game sees. Global
     * addresses: OP_GADDR_* in openpete_sdk_symbols.h (from
     * manifest/globals.toml). Reminder: values INSIDE guest structs that
     * are pointers hold guest vaddrs — re-translate before chasing.
     * Sim-thread use only (inside overrides / mod entry). */
    void* (*guest)(uint32_t vaddr);

    /* Cross-mod services (namespaced "modid:name", collision-free by
     * construction). A library mod publishes a pointer during its entry:
     *     api->export_sym(self, "magic", (void*)&my_fn);
     * A client that lists the lib in its mod.toml `deps` imports it —
     * deps are topo-sorted, so the lib's entry has already run:
     *     int (*magic)(void) = api->import_sym(self, "demo-lib:magic");
     * import returns NULL if absent (optional layering: check and
     * degrade gracefully). export returns 0 on success, non-zero on
     * duplicate/overflow. */
    int   (*export_sym)(openpete_mod_t* self, const char* name, void* ptr);
    void* (*import_sym)(openpete_mod_t* self, const char* qualified);

    /* Inverse of guest(): host pointer into the guest-RAM mirror → kseg0
     * vaddr, or 0 if the pointer is not inside guest RAM. Used by the
     * generated marshalling wrappers to pass host-visible structs back
     * to guest code. */
    uint32_t (*guest_addr)(const void* host);

    /* Invoke a guest function by vaddr through the dispatch layer (the
     * callee's own override chain runs — calls see the same modded world
     * as the game). Argument/return marshalling is the caller's job:
     * write cpu->a0..a3 first, read cpu->v0 after. Prefer the generated
     * op_call_<Name>() wrappers (openpete_sdk_wrappers.h), which also
     * save/restore the arg registers. Only valid on the sim thread with
     * a live cpu (i.e. inside an override). */
    void (*call_addr)(CPUState* cpu, uint32_t addr);

    /* ── Screen-space draw service (native vk renderer) ─────────────
     * Coordinates are OVERLAY SPACE: window-height fractions, origin
     * top-left, +y down; x spans [0, aspect], y spans [0, 1]. Draws are
     * host-side only (zero guest-RAM writes — a drawing observer mod
     * stays sim-neutral by construction) and render on the native vk
     * display: live window and --record videos. (--screenshot PNGs read
     * the scene-only reference target and do not include overlay layers
     * — same as the FPS overlay.) Headless and PsyCross-only runs
     * consume nothing.
     *
     * Immediate mode, two contexts:
     *   - called from a TICK context (inside an override): the command
     *     shows for all of that tick's presents; redraw every tick.
     *   - called from a PRESENT hook: the command lives one present.
     *
     * draw_text glyphs: digits, A-Z (lowercase folds), % / ? + ^ . '
     * and ':' — Spyro's own 3D gold text mobys; rgb is the glyph tint
     * (e.g. 0x605010 for HUD gold). Text appears once the game has
     * loaded the glyph font (early boot frames may draw nothing).
     * draw_rect rgba: R,G,B,A bytes (alpha currently ignored: opaque). */
    void (*draw_text)(float x, float y, float cap_h, uint32_t rgb,
                      const char* str);
    void (*draw_rect)(float x, float y, float w, float h, uint32_t rgba);

    /* Register a per-present hook: runs once per presented frame (30 Hz
     * canonical + every sub-tick present at high --render-fps) with the
     * present context. RENDER-PURE by contract AND enforcement: while a
     * present hook runs, guest access / game calls / base() / override
     * registration are refused with an error — presents are
     * non-deterministic in count and timing, so anything sim-affecting
     * belongs in tick hooks. Publish state from tick hooks into your own
     * variables; evaluate your own curves at ctx->alpha here (the engine
     * never interpolates mod visuals). Never runs headless. */
    int (*register_present_hook)(openpete_mod_t* self,
                                 openpete_mod_present_fn fn);

    /* ── Engine services (P2 contract) ──────────────────────────────
     * Savestates are ENGINE-NATIVE (never a mod feature): mods get a
     * trigger API + participation callbacks.
     *
     * savestate_request queues op (1=save, 2=load) on `slot` (shared
     * with the user's hotkey slots, 0..9); it executes at the next safe
     * boundary — the host loop, game fiber quiescent — NOT immediately.
     * One pending op at a time (a second request before execution fails).
     * Returns 0 = queued. On load the engine restores guest RAM/CPU/VRAM
     * and re-anchors the frame counter; register_savestate_hooks lets
     * this mod's own host-side state (tick counters, recording cursors)
     * travel inside the state file so a load doesn't desync it. */
    int (*savestate_request)(openpete_mod_t* self, int op, unsigned slot);
    int (*savestate_slot_present)(unsigned slot);
    int (*register_savestate_hooks)(openpete_mod_t* self,
                                    openpete_mod_state_save_fn save,
                                    openpete_mod_state_load_fn load);

    /* Per-mod persistent data directory (mods/<id>/data, created on
     * first call). Returns an engine-owned absolute path string. Mods
     * are native code — plain stdio inside it is the intended use. */
    const char* (*data_dir)(openpete_mod_t* self);

    /* Host keyboard query by SDL key name ("F5", "K", "Space", ...).
     * 1 = held. Reading host input in a TICK hook intentionally diverges
     * gameplay (hotkey trainers — the practice-ROM pattern); reading it
     * in a present hook for display-only purposes is fine. */
    int (*host_key_down)(const char* key_name);

    /* ── Actor render service (native vk renderer) ──────────────────
     * Draw a game actor at a RECORDED pose into this frame's 3D scene —
     * depth-tested against the world, no game code runs for it, zero
     * guest-RAM writes (the ghost-replay path; plan P2 draw service).
     *
     * capture_actor_pose(kind, buf, cap): snapshot the live pose of an
     * actor into an opaque versioned blob — exactly the field set the
     * native extractor reads, so a recorded blob replays completely by
     * construction. kind 0 = the player (Spyro; blob layout published in
     * spyro1_actor_pose.h so mods can evaluate position curves). Returns
     * blob bytes written (<= cap) or -1 (unknown kind / no snapshot yet
     * — the first snapshot lands once the game renders the actor).
     * TICK context only (it reads sim state; refused in present hooks).
     * Capture works headless; drawing needs the native display.
     *
     * draw_actor(kind, pose, len, tint_rgba): draw the actor at `pose`.
     * tint_rgba = 0xAARRGGBB; RGB multiplies the model's vertex colours
     * (0xFF = neutral), A < 0xFF renders PSX-average translucent (the
     * classic ghost look), A == 0xFF opaque. Same two submission
     * contexts as draw_text: tick context persists for that tick's
     * presents; a present hook's command lives one present — evaluate
     * your pose curve at ctx->alpha there for sub-tick smoothness.
     * Returns 0 = queued, -1 = invalid/full. A queued pose whose model
     * has been unloaded (level change) is skipped safely at render. */
    int (*capture_actor_pose)(int kind, void* buf, int cap);
    int (*draw_actor)(int kind, const void* pose, int len,
                      uint32_t tint_rgba);

    /* Hide the game's own player render while a draw_actor stand-in is
     * on screen (e.g. the hover cels replacing the jump pose). Same two
     * submission contexts as the draw calls: tick context hides the
     * player for all of that tick's presents, a present hook's call for
     * that present only. SAFE BY CONSTRUCTION: suppression only takes
     * effect on presents where a draw_actor emit actually produced
     * triangles — a missing/failed stand-in never leaves an invisible
     * player. Display-only — gameplay state is untouched. */
    void (*suppress_player_draw)(void);

    /* ── Instruction-site hook tier (the escape hatch) ────────────────
     * Attach a handler to a [[site]] this mod declared in its mod.toml.
     * A site anchors ONE guest instruction (by vaddr): kind "replace"
     * runs the handler INSTEAD of that instruction; kind "append" runs
     * it after. The handler gets the raw cpu context plus the site's
     * guest address (shared handlers dispatch on it) and reads/writes
     * cpu registers directly — raw-tier semantics at instruction
     * granularity.
     *
     * Sites are BAKED at recomp-regen time: tools/gen_mod_sites.py
     * collects enabled mods' [[site]] blocks and the code generator
     * emits a guarded substitution (identity while no handler is
     * registered). Returns 0 = attached (site live), -1 = this site is
     * not baked into the running binary — the loader logs the
     * regen-and-rebuild recipe (scripts/regen-mod-sites.sh); the mod
     * still loads, the site just stays inert. TICK-context rules apply
     * to the handler (it runs mid-sim); registration is entry-time
     * only (refused inside present hooks). */
    int (*site_handler)(openpete_mod_t* self, const char* name,
                        void (*fn)(CPUState* cpu, uint32_t site_addr));

    /* ── Content registry: moby types ────────────────────────────────
     * Register a NEW moby type: the engine claims a class id that is
     * FREE in the current level (its model row is empty, so the level's
     * update switch has no case for it), seeds the model row from
     * donor_class (the new type renders/collides with the donor's
     * model), and calls update_fn once per live instance per sim tick,
     * AFTER the game's own update. Ids are LEVEL-SCOPED — invalidated
     * on level change and re-claimed when gameplay resumes; write
     * level-independent behavior against the moby pointer, not the id.
     *
     * spawn_moby instantiates one through the game's own spawner (full
     * donor-class init: model, renderer, collision; placed at Spyro
     * when no parent), rebranded to the registered type. Returns the
     * guest moby vaddr (position at +0x0C, 3×i32) or 0 while the type
     * isn't live / the pool is full. TICK context only.
     *
     * moby_type_id returns the currently claimed class id, -1 if the
     * type isn't live in this level (yet). */
    int (*register_moby_type)(openpete_mod_t* self, const char* name,
                              int donor_class,
                              void (*update_fn)(CPUState* cpu,
                                                uint32_t moby_vaddr));
    int (*moby_type_id)(openpete_mod_t* self, const char* name);
    uint32_t (*spawn_moby)(openpete_mod_t* self, CPUState* cpu,
                           const char* name);

    /* ── Content registry: level slots ───────────────────────────────
     * Register a NEW level slot: the engine claims a free level id
     * (Spyro 1 has exactly ONE free in-bounds slot — id 65 / index 35;
     * more slots ride ram-virtualization), clones the donor level's
     * WAD TOC entry so the stock loader streams the donor's real disc
     * sectors, and aliases the id-keyed load functions to the donor so
     * the slot loads and plays donor-faithful while keeping its OWN
     * per-level state index (visited/gem/kill masks start fresh).
     *
     * level_slot_id returns the claimed id (-1 until claimed — the
     * claim lands on the first tick after the boot TOC read).
     * enter_level triggers a transition into any level id from live
     * gameplay (the level-select cheat's path; works for retail ids
     * too). TICK context only. */
    int (*register_level_slot)(openpete_mod_t* self, const char* name,
                               int donor_level_id);
    int (*level_slot_id)(openpete_mod_t* self, const char* name);
    int (*enter_level)(openpete_mod_t* self, CPUState* cpu, int level_id);

    /* ── SFX: one-shot (engine services) ─────────────────────────────
     * The sfx_* family covers all mod audio; the `name` domain differs
     * per verb — sfx_play resolves in THIS MOD's assets/sounds/;
     * sfx_replace/sfx_hook (below) resolve in the GAME's registered
     * sound names.
     *
     * sfx_play: play `assets/sounds/<name>.wav` from this mod's
     * directory as a HOST-side additive one-shot (OpenAL; never the
     * SPU — sim-neutral by construction, legal from tick and present
     * hooks). gain 0..4, 1.0 = as-authored; WAVs are cached after the
     * first play. 0 ok / -1 failed (missing file, no audio — logged,
     * never fatal). */
    int (*sfx_play)(openpete_mod_t* self, const char* name, float gain);

    /* ── Declared input bindings (engine services) ───────────────────
     * Resolve a [[binding]] declared in this mod's mod.toml
     * ({name, key, purpose}) to the live host key state. Declaring
     * bindings (instead of raw host_key_down) surfaces conflicts across
     * the enabled set at load time and lets a future mod menu rebind
     * without touching the mod. Same tick-context divergence caveats as
     * host_key_down. Undeclared name = error log + 0. */
    int (*binding_down)(openpete_mod_t* self, const char* name);

    /* ── Guest scratch arena (op_guest_alloc, P3) ────────────────────
     * Allocate `size` bytes at a GUEST-addressable vaddr (the >2MB
     * expansion window) with a typed HOST view of the same bytes —
     * for marshalled calls whose parameters are guest pointers.
     * Session-lifetime bump (no free); align must be a power of two
     * (0 = 8). Returns the guest vaddr, 0 = exhausted/unavailable.
     * NOT savestate-carried: rehydrate persistent contents in your
     * savestate-participation callbacks. Recompiled sites that MASK
     * addresses to 2MB cannot see arena data (standard accessor paths
     * all can). TICK context only. */
    uint32_t (*guest_alloc)(openpete_mod_t* self, uint32_t size,
                            uint32_t align, void** host_view);

    /* ── bk2 input-movie status (engine services, read-only v1) ──────
     * active: a movie is loaded this session; frame: current movie
     * frame (<0 before the offset); frame_count: movie length;
     * overriding: this tick's input actually came from the movie.
     * All zeros when no movie. Record/replay CONTROL stays engine-
     * owned (mid-session anchoring is TAS infra). out fields are
     * int-sized; pass a struct of 4 ints. */
    void (*bk2_status)(openpete_mod_t* self, int* active, int* frame,
                       int* frame_count, int* overriding);

    /* ── SFX overlay service ─────────────────────────────────────────
     * Host-side sound replacement at the voice layer: guest RAM, SPU
     * sample banks, sound tables and the gameplay rand() stream all
     * stay byte-stock — the substitution happens on a host copy of the
     * voice attributes at key-on, so an SFX mod is sim-neutral by
     * construction (the sim never reads SPU state back).
     *
     * Names are GAME-registered (Spyro 1: the SoundTable slot names —
     * "gem_pickup", "spyro_charge", ...) — unlike sfx_play's mod-local
     * WAV names. Unknown name => -1.
     *
     * sfx_replace: substitute the sample behind a named sound. body =
     * raw SPU-ADPCM frames (no VAG header, final block end-flag 0x01),
     * copied by the engine and staged into free SPU tail space per
     * level. pitch = SPU units (0x1000 = 44.1 kHz) used instead of the
     * game's pitch computation; 0 = keep the game's pitch. */
    int (*sfx_replace)(openpete_mod_t* self, const char* name,
                       const void* adpcm_body, uint32_t len,
                       uint16_t pitch);

    /* sfx_hook: per-play interception at key-on, host-side. ev->pitch
     * arrives prefilled (replacement pitch, else the game's); the hook
     * may rewrite it or set ev->suppress. ev->play_index counts plays
     * of this name; ev->ms_since_prev is the host-clock gap to the
     * previous play (~0 on the first). Keep hooks pure — no guest
     * writes; they run whenever the game keys the sound. */
    int (*sfx_hook)(openpete_mod_t* self, const char* name,
                    void (*fn)(openpete_sfx_event_t* ev));

    /* texture_register: mod-owned RGBA8 texture (w*h*4 bytes, row-major)
     * under a MOD-CHOSEN 16-bit id, so converted model faceblocks can
     * bake the id at pack time. Faces select it with the PSX-RESERVED
     * Tiledef encoding: tpage attr MODE bits ((attr>>7)&3) == 3, id in
     * the CLUT u16; UVs are raw texel coords into the registered image.
     * Sampled by the NATIVE renderer only (the PSX path shows garbage
     * for such faces — mode 3 does not exist on hardware). Re-register
     * an id to replace the pixels (hot reload). Returns 0, or -1 (bad
     * args / id space full). Host-side only: guest RAM, VRAM and the
     * oracle are untouched. */
    int (*texture_register)(openpete_mod_t* self, uint16_t id,
                            const void* rgba, int w, int h);

    /* sfx_stage: copy raw SPU-ADPCM into the free SPU tail through the
     * ENGINE'S shared allocator and return the SPU byte address (0 on
     * failure). Mods that place samples themselves (e.g. repointing a
     * SoundDefinition) MUST allocate here — a private tail pointer
     * collides with the overlay service's lazy staging (sfx_replace).
     * Addresses are valid for the CURRENT level bank only (the level
     * bank blast frees the tail) — re-stage per level. */
    uint32_t (*sfx_stage)(openpete_mod_t* self, const void* body,
                          uint32_t len);

    /* ── Per-mod user configuration (config.toml) ────────────────────
     * Read a value from mods/<id>/config.toml (optional, user-editable
     * — the manifest describes what the mod IS, config.toml what the
     * user WANTS from it). Keys are dotted paths: "chests.collision"
     * reads key `collision` in table [chests]; bare "verbose" reads a
     * root-level key. `def` is returned when the file, the key, or a
     * type-compatible value is absent — ship a config.toml documenting
     * the defaults, but make the CODE defaults authoritative.
     *
     * The file is parsed once, right before entry runs — read config at
     * entry and branch there. In dev hot-reload mode the loader watches
     * config.toml alongside the sources: an edit reloads the mod, so
     * entry re-runs with fresh values. Numeric coercion: bools read as
     * 0/1 through config_int/config_float; ints read through
     * config_float (and floats truncate through config_int); strings
     * never coerce. config_str copies at most cap-1 chars + NUL into
     * buf and returns 1 if the key was present (0 = default copied). */
    int     (*config_bool)(openpete_mod_t* self, const char* key, int def);
    int64_t (*config_int)(openpete_mod_t* self, const char* key,
                          int64_t def);
    double  (*config_float)(openpete_mod_t* self, const char* key,
                            double def);
    int     (*config_str)(openpete_mod_t* self, const char* key,
                          const char* def, char* buf, int cap);

    /* ── Level-ready event ───────────────────────────────────────────
     * fn(cpu, level_id) fires ONCE PER LEVEL, on the first master-
     * update tick with gameplay live: the model table is installed,
     * the level's CD stream is idle, and guest reads/writes/call_addr
     * are all legal inside (tick context). Registered mid-level (hot
     * reload), it fires for the level in progress on the next tick.
     *
     * This is THE place for per-level work — reading per-level tables,
     * repointing data, re-staging uploads. Do not hand-roll level
     * detection from install hooks (they run mid-CD-load, several
     * times) or camera hooks (frozen under the debug freecam; a level
     * loaded under an active pose never ticks them). Registration is
     * entry-time/tick-context. Returns 0, or -1 (table full / no game
     * ops). */
    int (*on_level)(openpete_mod_t* self, openpete_mod_level_fn fn);

    /* ── SFX: SoundDefinition replacement ────────────────────────────
     * For sounds sfx_replace can NOT name: plays that reference a
     * PER-LEVEL SoundDefinition index straight from a model's
     * m_Sounds[] (Spyro's chest breaks). Register the (model class,
     * m_Sounds slot, ADPCM body) once at entry; the ENGINE re-stages
     * the body into the SPU tail and repoints the live def every
     * level (level-ready driven — no polling, no camera hooks). body
     * is raw SPU-ADPCM frames (no VAG header), copied by the engine.
     * A class absent from a level is skipped silently for that level.
     * Rewrites guest def words: registering makes the mod
     * sim-divergent by design. Returns 0, or -1 (bad args / table
     * full / game op missing). */
    int (*sfx_def_replace)(openpete_mod_t* self, uint16_t model_class,
                           uint8_t sound_slot, const void* adpcm_body,
                           uint32_t len);

    /* ── Low guest park (guest_alloc_low) ────────────────────────────
     * Like guest_alloc, but from the LOW free window below the game's
     * EXE image (canonical RAM: savestate-carried, visible to every
     * access path). For payloads whose format addresses memory through
     * narrow fields — Spyro's animated-model frames hold 21-bit
     * absolute vertex-stream addresses, so parked Models MUST live in
     * the low 2MB (the guest_alloc arena is >2MB and unreachable).
     * Session-lifetime bump, no free; align 0 = 16. Returns the guest
     * vaddr (0 = exhausted/unregistered), host view via *host_view.
     * The window is small (tens of KB) — park compact payloads, not
     * scratch. TICK context only. */
    uint32_t (*guest_alloc_low)(openpete_mod_t* self, uint32_t size,
                                uint32_t align, void** host_view);

    /* ── Mod shaders: GLSL compile service ───────────────────────────
     * Register a GLSL shader by path relative to the mod directory
     * (e.g. "shaders/crt.frag"); stage from the extension (.vert /
     * .frag). The ENGINE compiles it to SPIR-V on load (in-process
     * glslang, vulkan1.0 dialect — the same dialect as the engine's
     * own shaders), content-hash cached under .build/shaders/. A
     * prebuilt "<path>.spv" next to a MISSING source loads instead
     * (binary-mod parity) but requires a "<path>.spv.ihash" sidecar
     * matching the current shader-interface hash — a stale prebuilt
     * refuses cleanly rather than misrendering. `#include
     * "openpete_shader_api.glsl"` resolves to the SDK interface
     * header; includes also resolve relative to the shader's own
     * directory. Returns a shader handle >= 0, or -1 (missing file /
     * compile error — see .build/shaders/<name>.log; the mod keeps
     * loading, the effect is simply unavailable). Entry/tick context
     * only. Sources join the dev hot-reload watch. */
    int (*shader_register)(openpete_mod_t* self, const char* path);

    /* ── Mod shaders: post-process chain ─────────────────────────────
     * Fullscreen fragment passes at a named injection point
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

    /* ── Mod shaders: custom-pipeline draws ──────────────────────────
     * Draw mod geometry with mod shaders through the native renderer.
     * pipeline_register builds a GPU pipeline from a desc (returns
     * handle >= 0 / -1; entry/tick context). draw_mesh submits
     * `vert_count` vertices of the pipeline's vertex format from a
     * PRESENT HOOK (canonical + intermediate); texture_id is a
     * texture_register id, 0 = untextured. Mods never see Vulkan —
     * RHI-shaped descriptions only. */
    int (*pipeline_register)(openpete_mod_t* self,
                             const openpete_mod_pipeline_desc_t* desc);
    int (*draw_mesh)(openpete_mod_t* self, int pipeline,
                     const void* verts, int vert_count,
                     uint16_t texture_id);

    /* ── Mod shaders: material selectors ─────────────────────────────
     * Override the SHADING of a subset of the engine's own draws —
     * a moby class, a terrain texture, the env-animated tile set —
     * without touching scheduling (pass position, depth, stencil and
     * category stay engine-owned). shader_pair = vert+frag handles;
     * either may be -1 to keep the stock stage. per_instance (may be
     * NULL = always override, no uniforms) runs at extraction per
     * matching instance: return a uniform block or -1 to decline that
     * instance. Uniforms are computed on CANONICAL presents and
     * replayed by identity on sub-tick presents (tick-stable by
     * default). Conflicts: enabled-list priority, first match wins.
     * Returns 0 or -1. Entry/tick context. */
    int (*material_register)(openpete_mod_t* self,
                             const openpete_mod_material_selector_t* sel,
                             int vert_shader, int frag_shader,
                             openpete_mod_material_fn per_instance);

    /* ── Read another level's raw Data WAD entry (host-side) ──────────
     * Copies the level's Data blob from the user's own disc image into
     * buf (buf=NULL just probes the length). Returns the entry byte
     * length, or -1 if unavailable / cap too small. Lets a mod borrow
     * another level's authored data (e.g. its cyclorama) at runtime
     * without shipping derived game data. Entry/tick context. */
    int (*read_level_data)(openpete_mod_t* self, int level_id,
                           void* buf, uint32_t cap);

    /* ── Pack fog shift ───────────────────────────────────────────────
     * A full-coverage texture pack that re-grades the scene (per-channel
     * v' = scale*v + offset, 0..255 units) leaves the terrain fog ladder
     * blending replaced texels toward the STOCK ambience. Registering the
     * pack's own transform retargets every fog affine at the pack's
     * ambience instead (offset corrected, slope untouched). scale NULL
     * clears. One global slot (last caller wins) — set it on level entry,
     * clear it on exit. Display-only. Entry/tick context. */
    int (*texpack_fog_shift)(openpete_mod_t* self,
                             const float scale_rgb[3],
                             const float offset_rgb[3]);

    /* ── Per-mod UI section (api v3) ─────────────────────────────────
     * Register this mod's section body for the dev overlay's Mods
     * panel (M key, windowed runs). The overlay lists every mod with
     * its live enable/disable toggle regardless; register_ui adds the
     * mod's own widgets under its entry. fn runs at overlay render
     * with the sim quiescent, RENDER-PURE by contract AND enforcement
     * (guest access / game calls / base() / registration are refused
     * inside — the present-hook rules): mutate host-side mod state
     * from widgets and let tick hooks consume it. Never runs while
     * the mod is disabled, and never headless. One registration per
     * mod (re-register replaces). Entry/tick context. Returns 0. */
    int (*register_ui)(openpete_mod_t* self, openpete_mod_ui_fn fn,
                       void* user);

    /* ── Live-toggle notification (api v4) ───────────────────────────
     * Called at the tick boundary each time THIS mod's live toggle
     * actually flips (not for no-op requests), after the flip: chains/
     * hooks/registry rows already reflect the new state. The engine
     * freezes a disabled mod's registrations but never rewinds guest
     * state — a mod whose entry or level hook WROTE guest state (table
     * repoints, patched colours) restores the stock bytes in on=0.
     * Enable-side reapply is usually free: level-ready hooks re-fire
     * for the level in progress on re-enable. Entry/tick context;
     * one registration per mod (re-register replaces). Returns 0. */
    int (*register_toggle_hook)(openpete_mod_t* self,
                                openpete_mod_toggle_fn fn);
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
