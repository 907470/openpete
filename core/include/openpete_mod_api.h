// openpete_mod_api.h — the mod API.
//
// Staged into <exe>/sdk/ and compiled into every code mod. A mod reaches
// the engine only through the openpete_mod_api_t struct passed to its
// entry point; it never links against engine symbols.
//
//     int openpete_mod_entry(const openpete_mod_api_t* api,
//                            openpete_mod_t* self);
//
// Return 0 to load; any other value disables the mod and logs the value.
//
// Stability. The struct layout is frozen at api 10: from there on entries
// are appended, never moved, retyped or removed. The manifest `api = N`
// pin is the minimum version the mod needs; check `api_version < N`,
// never `!=`. The UI annex (openpete_mod_ui.h) is versioned separately
// and carries no stability promise.
//
// Version at which each entry appeared: `since api N` in its comment.
// api 1 is the base set; 7 removed the moby-type registry; 10 moved
// notify to the tail of the struct and added the ABI descriptor.
//
// Overrides. A pre-hook, a post-hook and a full replacement are one
// mechanism, distinguished by where the override calls api->base(cpu):
// last, first or never. base() runs the next override in the chain and
// bottoms out at the recompiled original. There is no event system:
// detect an event by diffing guest state around base().
//
// Override bodies take the raw MIPS register file (CPUState): arguments
// in cpu->a0..a3, return value in cpu->v0. openpete_sdk_wrappers.h
// provides natural-signature wrappers for every prototyped function.

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

/* An override body. Read and write guest state through cpu; call
 * api->base(cpu) to run the next override or the original. */
typedef void (*openpete_mod_override_fn)(CPUState* cpu);

/* Context handed to a present hook (register_present_hook). */
typedef struct openpete_present_ctx {
    uint32_t tick;   /* sim tick counter                               */
    float    alpha;  /* sub-tick phase [0,1) between tick and tick+1;
                      * 0 on a canonical present                       */
    float    dt;     /* seconds between this scene's animation moment
                      * and the previous scene's                       */
    float    aspect; /* overlay-space width: x spans [0, aspect]       */
} openpete_present_ctx_t;

typedef void (*openpete_mod_present_fn)(const openpete_present_ctx_t* ctx);

/* Live enable/disable callback (register_toggle_hook). Runs at the tick
 * boundary after the mod's registrations have flipped; guest reads and
 * writes are legal. on=0 is where a mod restores guest bytes its entry
 * or hooks changed. */
typedef void (*openpete_mod_toggle_fn)(CPUState* cpu, int on);

enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

/* guest_alloc flags. */
enum {
    OP_GALLOC_IMMUTABLE = 1u << 0, /* the mod promises that after the
                              * allocating tick neither it nor the game
                              * writes these bytes. The engine may leave
                              * them out of savestates and rewind history
                              * and may abort on a detected write. Use for
                              * parked model data, never for scratch. */
};

/* Post-process injection points (postfx_register). */
enum {
    OP_POSTFX_COMPOSITE = 0, /* after scene and HUD compose, before the
                              * present blit; the default point */
    OP_POSTFX_SCENE     = 1, /* after the 3D scene, before the HUD, so the
                              * HUD stays unaffected. Scene depth is
                              * available at op_scene_depth (terrain only,
                              * not mobys). */
    OP_POSTFX_PRESENT   = 2, /* reserved; registration is refused */
};

/* Sim tick rate: 1125000/37609 Hz (29.913...). The PSX outputs 263-line
 * non-interlaced video at 2250000/37609 Hz (59.826...) and the game runs
 * one tick per two VBLs. Not 29.97: that is the interlaced field rate,
 * which this game never uses, and a mod converting ticks to seconds with
 * it drifts 0.19 % against game time. --tick-scale stretches wall time
 * only; a tick stays one tick. */
#define OPENPETE_TICK_HZ  (1125000.0 / 37609.0)
#define OPENPETE_VBL_HZ   (2250000.0 / 37609.0)

/* Render channels: the renderer's feeder paths. A material selector
 * names one of these. docs/render-channel-census.md lists the keys each
 * channel can be narrowed on. */
enum {
    OP_CHAN_MOBY     = 1,
    OP_CHAN_TERRAIN  = 2,
    OP_CHAN_PLAYER   = 3,
    OP_CHAN_FLAME    = 4,
    OP_CHAN_PARTICLE = 5,
    OP_CHAN_SKY      = 6,
    OP_CHAN_SHADOW   = 7,
    OP_CHAN_TRACER   = 8,
    OP_CHAN_PORTAL   = 9,   /* destination-sky preview inside a portal */
};
/* Portal preview kinds: the `ident` key for OP_CHAN_PORTAL. The game
 * draws a portal three ways across the approach; a material keyed on one
 * kind pops as the player walks up, so -1 (all three) is the usual
 * choice. */
enum {
    OP_PORTAL_FLAT    = 0,  /* far colour chip / close backstop */
    OP_PORTAL_DOME    = 1,  /* full gouraud dome, close up      */
    OP_PORTAL_BLENDED = 2,  /* fogged dome, medium cross-dissolve */
};
/* Legacy 0-based path values; prefer OP_CHAN_*. */
enum { OP_RP_MOBY = 0, OP_RP_TERRAIN = 1 };
/* Selector refinements. Each pair narrows: setting neither member of a
 * pair selects both. */
enum {
    OP_SEL_ENV_ANIM   = 1u << 0,  /* env-animated faces only */
    OP_SEL_HP_TIER    = 1u << 1,  /* high-poly tier only  } neither = both */
    OP_SEL_LP_TIER    = 1u << 2,  /* low-poly tier only   }               */
    OP_SEL_UNTEXTURED = 1u << 3,  /* gouraud faces only   } neither = both */
    OP_SEL_TEXTURED   = 1u << 4,  /* textured faces only  }               */
};
/* Every flag the engine understands. A selector carrying any other bit
 * is refused: struct_size cannot detect a new flag bit, since adding one
 * does not change sizeof. */
#define OP_SEL_FLAGS_KNOWN (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | \
                            OP_SEL_LP_TIER  | OP_SEL_UNTEXTURED | \
                            OP_SEL_TEXTURED)

typedef struct openpete_mod_material_selector {
    /* sizeof(*this) at the mod's compile time; the engine refuses a size
     * it does not know. */
    uint32_t struct_size;
    int      channel;     /* OP_CHAN_*; -1 = any channel */
    /* Channel-local identity, meaning per channel: flame ribbon or cap,
     * particle (kind << 8 | sel), shadow caster kind, portal preview
     * kind. -1 = any. Player, sky and tracer emit only 0. */
    int      ident;
    int      moby_class;  /* moby only; -1 = any */
    int      texture;     /* terrain texture-record id 0..127; -1 = any */
    int      level;       /* g_LevelId; -1 = any. Sector and texture ids
                           * are per level, so a key without this matches
                           * the same id in every level. */
    unsigned flags;       /* OP_SEL_* refinements */
} openpete_mod_material_selector_t;
/* A selector that matches everything; narrow only the fields you mean.
 * A zero-initialised selector would mean channel 0 and class 0. */
static inline openpete_mod_material_selector_t openpete_mod_material_sel_any(void) {
    openpete_mod_material_selector_t s;
    s.struct_size = (uint32_t)sizeof s;
    s.channel = -1; s.ident = -1; s.moby_class = -1;
    s.texture = -1; s.level = -1; s.flags = 0u;
    return s;
}

/* Refine callbacks (material_register_refine). The selector decides
 * which geometry a material may claim; the refine callback decides per
 * instance whether it does, and with what uniform block. Example: the
 * selector prunes to class 42, the callback claims only instances that
 * are burning.
 *
 * The callback runs during extraction in tick context: guest RAM is
 * readable through api->guest() and op_read_*; host render state is off
 * limits. Results are memoised per instance, so the callback must be a
 * pure function of the key and tick-stable guest state; the call count
 * is not part of the contract. */

/* Key version per channel, stamped into `version`; bumped when a
 * channel's key members change meaning. */
#define OPENPETE_MATERIAL_KEY_VER_MOBY    1u
#define OPENPETE_MATERIAL_KEY_VER_TERRAIN 1u

/* One instance at refine time. Every field is filled by the engine;
 * `channel` selects the live union member. */
typedef struct openpete_mod_material_key {
    uint32_t struct_size;   /* sizeof at engine compile time */
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

/* Size cap of a material's per-instance uniform block. */
#define OPENPETE_MATERIAL_UNIFORMS_MAX 256u

/* Return the number of bytes written to `out` (0 = claim with no
 * parameters), or -1 to decline: the instance keeps stock shading. `cap`
 * is OPENPETE_MATERIAL_UNIFORMS_MAX. */
typedef int (*openpete_mod_refine_fn)(const openpete_mod_material_key_t* key,
                                      void* out, uint32_t cap);

#define OPENPETE_MOD_API_VERSION 10u

typedef struct openpete_mod_api {
    uint32_t api_version;   /* == OPENPETE_MOD_API_VERSION of the engine;
                             * compare with `<`, never `!=` */

    /* ── Overrides ──────────────────────────────────────────────────
     * Attach an override to a guest function by symbol name (resolved
     * through openpete_sdk_symbols.h, e.g. "CameraUpdate") or by PSX
     * vaddr (the manifest must declare tier = "raw"). Returns 0 on
     * success, non-zero for an unknown name or invalid arguments.
     * Priority is the enabled-list order: overrides from later-listed
     * mods run first, and base() walks toward earlier mods and then the
     * recompiled original. Since api 1. */
    int (*override_addr)(openpete_mod_t* self, uint32_t addr,
                         openpete_mod_override_fn fn);
    int (*override_name)(openpete_mod_t* self, const char* name,
                         openpete_mod_override_fn fn);

    /* Run the next override in the chain, or the original. Valid only
     * while an override registered by this mod is on the call stack. May
     * be called any number of times; each call runs the continuation
     * once. Since api 1. */
    void (*base)(CPUState* cpu);

    /* Call a guest function by vaddr through the dispatch layer, so the
     * callee's own override chain runs. Write cpu->a0..a3 before, read
     * cpu->v0 after. Valid only inside an override. Since api 1. */
    void (*call)(CPUState* cpu, uint32_t addr);

    /* ── Guest memory ───────────────────────────────────────────────
     * Translate a guest vaddr (kuseg, kseg0 or kseg1) to a host pointer:
     * the 2 MB RAM, the scratchpad and guest_alloc's arena all resolve.
     * NULL for an address that names nothing, including guest address 0.
     * Guest RAM and every supported host are little-endian, so typed
     * reads and writes through the pointer see what the game sees.
     * Pointer fields inside guest structs hold guest vaddrs; translate
     * them again before dereferencing. Global addresses are OP_GADDR_* in
     * openpete_sdk_symbols.h. Sim thread only. Since api 1. */
    void* (*guest)(uint32_t vaddr);

    /* Inverse of guest(): a host pointer into RAM, the scratchpad or an
     * arena allocation to its guest vaddr, 0 if the pointer names no
     * guest address. Interior pointers map to the corresponding interior
     * vaddr. Since api 1. */
    uint32_t (*guest_addr)(const void* host);

    /* Allocate `size` bytes of guest-addressable memory. Returns the
     * guest vaddr (0 = exhausted, unavailable or invalid) and, through
     * `host_view`, a host alias of the same bytes. `align` is a power of
     * two (0 = default). The arena is a bump allocator with no free:
     * allocate at entry and reuse. Tick context only: call it, and write
     * through `host_view`, only from entry, tick hooks and toggle hooks.
     *
     * Contents are zero on first allocation. On reload the allocation
     * sequence is replayed: the i-th call with the same size, alignment
     * and flags returns the same vaddr and view, holding the bytes as
     * last written. Initialise what needs initialising.
     *
     * The vaddr and host view are stable for the process lifetime, and
     * the bytes are reachable through every engine guest access path
     * (cpu->read_*, cpu->write_*, api->guest).
     *
     * Unless flagged OP_GALLOC_IMMUTABLE the bytes are timeline state:
     * savestates, rewind, runahead and process handoff carry and restore
     * them like guest RAM. An allocation made after a snapshot keeps its
     * contents when that snapshot is restored; the cursor never rewinds.
     * Keep mutable per-session state here rather than in host statics,
     * and store guest vaddrs rather than host pointers, since carried
     * bytes cross process boundaries at handoff.
     *
     * An unknown flag fails the call (returns 0, logs the flag).
     *
     * Not promised: the arena's base or layout (never hardcode a vaddr;
     * a savestate whose allocation sequence does not prefix-match the
     * live session is refused, naming the mod and entry), survival of
     * `host_view` across handoff, or any particular snapshot mechanism.
     *
     * Model vertex streams referenced from animation frame words are
     * parked here too; the frame word carries a handle from
     * vstream_handle, not an address. Since api 1. */
    uint32_t (*guest_alloc)(openpete_mod_t* self, uint32_t size,
                            uint32_t align, uint32_t flags,
                            void** host_view);

    /* ── Lifecycle ──────────────────────────────────────────────────
     * printf-style logging, tagged "mod:<id>". level = OP_MOD_LOG_*.
     * Since api 1. */
    void (*log)(openpete_mod_t* self, int level, const char* fmt, ...);

    /* Per-mod persistent data directory (mods/<id>/data, created on
     * first call). Returns an engine-owned absolute path; use plain stdio
     * inside it. Since api 1. */
    const char* (*data_dir)(openpete_mod_t* self);

    /* Register the live-toggle callback: runs at the tick boundary after
     * each flip of this mod's enable state. The engine freezes a disabled
     * mod's registrations but never rewinds guest state, so a mod that
     * wrote guest state restores the stock bytes on on=0. One
     * registration per mod; re-registering replaces. Returns 0.
     * Since api 1. */
    int (*register_toggle_hook)(openpete_mod_t* self,
                                openpete_mod_toggle_fn fn);

    /* ── Settings ───────────────────────────────────────────────────
     * Read a value from mods/<id>/config.toml. Keys are dotted paths:
     * "chests.collision" is key `collision` in table [chests]. `def` is
     * returned when the file, the key or a type-compatible value is
     * absent. The file is parsed once, before entry runs; when the user
     * commits a change in the Mods panel (rendered from the manifest's
     * [[config]] rows) the engine rewrites config.toml and reloads the
     * mod, so read config at entry. Coercion: bools read as 0/1 through
     * config_int and config_float; ints read through config_float;
     * floats truncate through config_int; strings never coerce.
     * config_str copies at most cap-1 chars plus NUL into buf and
     * returns 1 if the key was present, 0 if the default was copied.
     * Since api 1. */
    int     (*config_bool)(openpete_mod_t* self, const char* key, int def);
    int64_t (*config_int)(openpete_mod_t* self, const char* key,
                          int64_t def);
    double  (*config_float)(openpete_mod_t* self, const char* key,
                            double def);
    int     (*config_str)(openpete_mod_t* self, const char* key,
                          const char* def, char* buf, int cap);

    /* Print a status line into this mod's section of the Mods panel.
     * Legal from any context; host state only. Lines accumulate within
     * a tick and persist until the next tick in which the mod calls
     * ui_status again, so a line printed once stays visible and a
     * per-tick printer repaints its block each tick. The block truncates
     * past about 24 lines. Custom panels use openpete_mod_ui.h.
     * Since api 1. */
    void (*ui_status)(openpete_mod_t* self, const char* fmt, ...);

    /* ── Present hooks and screen-space draws ───────────────────────
     * Register a per-present hook: runs once per scene the engine
     * extracts (the canonical one each tick plus every sub-tick scene at
     * a higher --render-fps). The hook fires when its scene is extracted
     * on the sim thread, ahead of display, and the engine may produce
     * fewer scenes than display slots; call count and timing are not
     * part of the contract. ctx->alpha is the animation moment the scene
     * shows and ctx->dt the spacing to the previous scene, so a curve
     * evaluated at ctx->alpha is correct under any cadence. Publish
     * state from tick hooks into your own variables and evaluate it here;
     * the engine never interpolates mod visuals.
     *
     * Render-pure, enforced: guest access, game calls, base() and
     * override registration are refused inside a present hook, since
     * presents are non-deterministic in count and timing.
     *
     * Runs only when something is presented: a plain headless run never
     * calls it, a headless run that takes a screenshot does. Since api 1. */
    int (*register_present_hook)(openpete_mod_t* self,
                                 openpete_mod_present_fn fn);

    /* Screen-space draws on the native renderer. Overlay space: units of
     * window height, origin top-left, +y down, x in [0, aspect], y in
     * [0, 1]. Host state only. Immediate mode: a call from tick context
     * shows for all of that tick's presents (redraw every tick); a call
     * from a present hook shows for that present. draw_text glyph set:
     * digits, A-Z (lowercase folds up), space, and % / ? + ^ . ' :, drawn
     * with the game's own gold text glyphs; rgb tints them (0x605010 =
     * HUD gold); text appears once the game has loaded the glyph font.
     * draw_rect rgba is R,G,B,A bytes; alpha is currently ignored.
     * Headless and PsyCross-only runs consume nothing. Since api 1. */
    void (*draw_text)(float x, float y, float cap_h, uint32_t rgb,
                      const char* str);
    void (*draw_rect)(float x, float y, float w, float h, uint32_t rgba);

    /* ── Post-process shaders ───────────────────────────────────────
     * Register a GLSL shader by path relative to the mod directory
     * (e.g. "shaders/crt.frag"); the extension selects the stage. The
     * engine compiles it to SPIR-V on load (in-process glslang, Vulkan
     * 1.0 dialect), cached by content hash under .build/shaders/. A
     * prebuilt "<path>.spv" next to a missing source loads instead if a
     * "<path>.spv.ihash" sidecar matches the current shader-interface
     * hash. `#include "openpete_shader_api.glsl"` resolves to the SDK
     * interface header. Returns a shader handle >= 0, or -1 for a missing
     * file or compile error (see .build/shaders/<name>.log; the mod still
     * loads). Entry or tick context. Sources join the hot-reload watch.
     * Since api 1. */
    int (*shader_register)(openpete_mod_t* self, const char* path);

    /* Fullscreen fragment pass at an injection point (OP_POSTFX_*).
     * Pass order across mods is enabled-list priority; `order` breaks
     * ties within a mod (lower runs first). The pass samples the previous
     * pass (set 0) and receives the engine UBO (time, tick, alpha,
     * resolution; see shaders/openpete_shader_api.glsl) plus the mod's
     * uniform block. postfx_set_uniforms copies `data` on call (at most
     * 1024 bytes; push from tick or present hooks). postfx_register
     * returns an fx handle >= 0, registered disabled, or -1. Register
     * from entry or tick context; enable and set_uniforms from any.
     * Since api 1. */
    int (*postfx_register)(openpete_mod_t* self, int frag_shader,
                           int point, int order);
    int (*postfx_enable)(openpete_mod_t* self, int fx, int enabled);
    int (*postfx_set_uniforms)(openpete_mod_t* self, int fx,
                               const void* data, uint32_t size);

    /* ── Audio ──────────────────────────────────────────────────────
     * sfx_replace: substitute the sample behind a named game sound; the
     * code form of dropping <name>.wav into this mod's assets/sfx. body
     * is raw SPU-ADPCM frames (no VAG header, final block end-flag 0x01),
     * decoded and copied by the engine. pitch is in SPU units (0x1000 =
     * 44.1 kHz) and overrides the game's pitch writes; 0 keeps the game's
     * own pitch. The swap happens at the voice's PCM upload, below the
     * SPU RAM image and voice mirror, so guest RAM, the gameplay rand()
     * stream and the tick on which the game sees the sound end (it polls
     * the stock sample's duration) stay stock; a longer replacement
     * rings out past that tick. Sounds without a SoundTable name (a
     * moby's own m_Sounds def) are unreachable here; a hash-named file in
     * assets/sfx/ covers every sample and needs no code. Unknown name
     * returns -1. Since api 1.
     *
     * sfx_play: play assets/sfx/own/<name>.wav from this mod's directory
     * as a host-side one-shot, never through the SPU, so it is legal from
     * tick and present hooks. gain 0..4, 1.0 = as authored; WAVs are
     * cached after the first play. Returns 0, or -1 for a missing file or
     * no audio device (logged). Since api 1. */
    int (*sfx_replace)(openpete_mod_t* self, const char* name,
                       const void* adpcm_body, uint32_t len,
                       uint16_t pitch);
    int (*sfx_play)(openpete_mod_t* self, const char* name, float gain);

    /* ── Input bindings ─────────────────────────────────────────────
     * Read a [[binding]] declared in this mod's mod.toml ({name, key,
     * purpose}): 1 while the bound host key is held. Declared bindings
     * are conflict-checked across the enabled set at load, and the user
     * rebinds through openpete.toml [keys.mod.<id>] (keyboard names,
     * gamepad names such as "pad:north", or "none"). Reading host input
     * in a tick hook diverges gameplay from a replay by design; display-
     * only reads belong in present hooks. Undeclared name logs an error
     * and returns 0. Since api 1. */
    int (*binding_down)(openpete_mod_t* self, const char* name);

    /* ── Game data and texture packs ────────────────────────────────
     * Read one level's raw Data blob from the mounted disc into buf (cap
     * bytes); buf NULL queries the length. Returns the blob length, or
     * < 0 when no game layer provides level data or the id is unknown.
     * Lets a mod use another level's sky, palette or geometry without
     * shipping derived data. Since api 2. */
    int (*read_level_data)(openpete_mod_t* self, int level_id,
                           void* buf, uint32_t cap);

    /* Retarget the texture-pack fog ambience: fogged replacement texels
     * blend toward scale*stock+offset (offsets in 0..255 units) instead
     * of the stock ladder colours, for packs whose art re-grades the
     * level. NULL/NULL restores stock. Per-texture grade fits take
     * precedence where they have signal. Display only. Since api 2. */
    int (*texpack_fog_shift)(openpete_mod_t* self, const float scale_rgb[3],
                             const float offset_rgb[3]);

    /* ── Materials ──────────────────────────────────────────────────
     * Attach a fragment shader to a set of geometry chosen by a render
     * channel and that channel's keys (moby class, terrain texture or
     * tier, channel-local identity, level). Build the selector with
     * openpete_mod_material_sel_any() and narrow only what you mean; the
     * engine refuses a selector it cannot fully honour rather than
     * matching wider than asked.
     *
     * `shader` is a frag handle from shader_register. The shader reshades
     * within the draw group the geometry already belongs to: it cannot
     * change blending, depth policy or draw order. Unmatched or failed
     * builds keep stock shading; a material cannot make geometry
     * invisible.
     *
     * Returns a material handle, or -1. Registration starts enabled.
     * Since api 3. */
    int (*material_register)(openpete_mod_t* self,
                             const openpete_mod_material_selector_t* sel,
                             int shader);

    /* Turn one registered material on or off. Callable from any context;
     * the natural target of a [[config]] toggle. Since api 3. */
    int (*material_enable)(openpete_mod_t* self, int material, int on);

    /* Set the uniform block the material's shader reads (std140, at most
     * OPENPETE_MATERIAL_UNIFORMS_MAX bytes). May be called per tick; for
     * time-varying effects prefer the engine UBO's time in the shader.
     * Since api 3. */
    int (*material_set_params)(openpete_mod_t* self, int material,
                               const void* block, uint32_t len);

    /* material_register with a per-instance refine callback: the engine
     * calls `refine` once per candidate instance during extraction and
     * it returns that instance's uniform block, or -1 to leave it stock.
     * Instances with byte-identical blocks share one slot and one batch,
     * so a callback with few distinct outcomes is cheap and one with a
     * distinct block per instance costs a draw each. Scope the selector
     * tightly and let the callback decide the rare cases.
     *
     * Available on channels with a stable per-instance identity (moby);
     * on other channels register declaratively and drive the material
     * with material_set_params. Since api 4. */
    int (*material_register_refine)(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int shader, openpete_mod_refine_fn refine);

    /* Point a live material at a different selector, keeping its handle,
     * shader, refine callback and enabled state. Selection is constant
     * across a present, so a call from a present hook or UI section
     * takes effect at the next tick; from entry or a tick hook it applies
     * at once. The material keeps shading under its old key until the new
     * one applies. The selector is validated as material_register
     * validates it. Since api 6. */
    int (*material_set_selector)(openpete_mod_t* self, int material,
                                 const openpete_mod_material_selector_t* sel);

    /* The moby classes the current level has loaded, ascending, into
     * `out`; returns the count (out=NULL, cap=0 sizes the buffer). Level-
     * scoped, not visibility-scoped: a superset of the classes with a
     * live instance at this instant, and the set worth offering a user
     * to choose from. Reads an engine-side snapshot refreshed once per
     * tick, so it is safe from a UI section. Empty before the first
     * level finishes loading. Since api 6. */
    int (*moby_classes)(openpete_mod_t* self, uint16_t* out, int cap);

    /* ── Vertex streams ─────────────────────────────────────────────
     * A stream handle for `stream_vaddr`: the value to write into an
     * animation frame word's 21-bit vertex-stream field (bits 0-20) in
     * place of an address that does not fit. The game's readers resolve
     * it to the full vaddr, so a stream may live anywhere guest_alloc
     * puts it. The same vaddr always yields the same handle; handles are
     * below 0x8000 and never 0, which is the failure return (vaddr 0, a
     * kernel-area vaddr, or the table exhausted).
     *
     * Ask at entry, in a fixed order, once per stream: every call is
     * ledgered like an allocation, replayed on reload, and verified when
     * a savestate loads, so a sequence that depends on the order levels
     * were visited makes a savestate from another route refuse to load.
     * The game stores frame-word addresses halved; a handle is written
     * as-is in both encodings. Tick context only. Since api 9. */
    uint32_t (*vstream_handle)(openpete_mod_t* self, uint32_t stream_vaddr);

    /* ── Notifications ──────────────────────────────────────────────
     * Post a transient on-screen notification in the game's gold-
     * bordered chrome at the anchor the player chose. ui_status reports
     * state; notify reports an event.
     *
     * `key` (may be NULL) coalesces: a live toast with the same key is
     * repainted with the new text and a restarted timer instead of a
     * second toast stacking. The engine prefixes the mod id, so keys
     * never collide across mods.
     *
     * `timeout_ms`: 0 takes the player's --osd-timeout default; values
     * above 30000 clamp to it. A repaint takes the repainting call's
     * timeout.
     *
     * Legal from entry, tick hooks and present hooks; host state only.
     * Rate-limited per mod (a small burst, then a slow refill). A no-op
     * on runahead and rewind-replay ticks, whose effects are rolled back
     * or re-executed; a mod therefore sees fewer toasts than calls.
     *
     * Glyph set: 0-9, A-Z (lowercase folds up), space, and % / ? + ^ . '
     * :. Any other character renders as a space. Since api 8. */
    void (*notify)(openpete_mod_t* self, const char* key,
                   uint32_t timeout_ms, const char* fmt, ...);
} openpete_mod_api_t;

/* ── ABI descriptor ──────────────────────────────────────────────────
 * Every code object exports one `openpete_mod_abi` recording the api
 * version and api-struct size it was compiled against. The loader reads
 * it before the entry point runs and refuses an object built for a newer
 * api or a larger struct than the engine has, and an object with no
 * descriptor. An older object loads, since the struct is append-only
 * from api 10.
 *
 * The engine adds the SDK's openpete_mod_abi.c to every compile line, so
 * a mod's own sources need not define the descriptor. A hand-built object
 * compiles that file in, or puts OPENPETE_MOD_ABI_DEFINE() in exactly one
 * of its sources. */
typedef struct openpete_mod_abi {
    uint32_t magic;       /* OPENPETE_MOD_ABI_MAGIC */
    uint32_t api_version; /* OPENPETE_MOD_API_VERSION at the mod's compile time */
    uint32_t api_size;    /* sizeof(openpete_mod_api_t) at the mod's compile time */
} openpete_mod_abi_t;
#define OPENPETE_MOD_ABI_MAGIC 0x4F504142u /* 'OPAB' */
#define OPENPETE_MOD_ABI_DEFINE()                                          \
    const openpete_mod_abi_t openpete_mod_abi = {                          \
        OPENPETE_MOD_ABI_MAGIC, OPENPETE_MOD_API_VERSION,                  \
        (uint32_t)sizeof(openpete_mod_api_t) }

/* Typed guest accessors. A read of an unmapped address returns 0; a
 * write to one is dropped. */
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
