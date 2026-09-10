/**
 * @file openpete_mod_api.h
 * @brief The mod API.
 *
 * Staged into `<exe>/sdk/` and compiled into every code mod. A mod reaches
 * the engine only through the ::openpete_mod_api_t struct passed to its
 * entry point; it never links against engine symbols.
 *
 * @code
 * int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self);
 * @endcode
 *
 * Return 0 to load; any other value disables the mod and logs the value.
 *
 * @par Stability
 * The struct layout is frozen at api 11: from there on entries are
 * appended, never moved, retyped or removed, and the loader refuses a
 * prebuilt object built against an earlier api. The manifest `api = N`
 * pin is the minimum version the mod needs; check `api_version < N`,
 * never `!=`. The two extension headers — the widget vtable (openpete_mod_ui.h) and
 * the ImGui escape hatch (openpete_imgui.h) — are versioned
 * separately and carry no stability promise. Each entry states the
 * version it appeared in with `@since`.
 *
 * @par Overrides
 * A pre-hook, a post-hook and a full replacement are one mechanism,
 * distinguished by where the override calls `api->base(cpu)`: last, first
 * or never. base() runs the next override in the chain and bottoms out at
 * the recompiled original. There is no event system: detect an event by
 * diffing guest state around base().
 *
 * Override bodies take the raw MIPS register file (::CPUState): arguments
 * in `cpu->a0..a3`, return value in `cpu->v0`. openpete_sdk_wrappers.h
 * provides natural-signature wrappers for every prototyped function.
 */

#ifndef OPENPETE_MOD_API_H
#define OPENPETE_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/** @brief Opaque per-mod handle owned by the loader. */
typedef struct openpete_mod openpete_mod_t;

/**
 * @brief An override body.
 *
 * Read and write guest state through @p cpu; call `api->base(cpu)` to run
 * the next override or the original.
 */
typedef void (*openpete_mod_override_fn)(CPUState* cpu);

/** @brief Context handed to a present hook (openpete_mod_api::register_present_hook). */
typedef struct openpete_present_ctx {
    uint32_t tick;   /**< Sim tick counter. */
    float    alpha;  /**< Sub-tick phase [0,1) between tick and tick+1; 0 on a canonical present. */
    float    dt;     /**< Seconds between this scene's animation moment and the previous scene's. */
    float    aspect; /**< Overlay-space width: x spans [0, aspect]. */
} openpete_present_ctx_t;

/** @brief A present hook body. */
typedef void (*openpete_mod_present_fn)(const openpete_present_ctx_t* ctx);

/**
 * @brief Live enable/disable callback (openpete_mod_api::register_toggle_hook).
 *
 * Runs at the tick boundary after the mod's registrations have flipped;
 * guest reads and writes are legal. @p on = 0 is where a mod restores
 * guest bytes its entry or hooks changed.
 */
typedef void (*openpete_mod_toggle_fn)(CPUState* cpu, int on);

/** @brief Log levels for openpete_mod_api::log. */
enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

/** @brief Flags for openpete_mod_api::guest_alloc. */
enum {
    /**
     * The mod promises that after the allocating tick neither it nor the
     * game writes these bytes. The engine may leave them out of savestates
     * and rewind history and may abort on a detected write. Use for parked
     * model data, never for scratch.
     */
    OP_GALLOC_IMMUTABLE = 1u << 0,
};

/** @brief Post-process injection points for openpete_mod_api::postfx_register. */
enum {
    OP_POSTFX_COMPOSITE = 0, /**< After scene and HUD compose, before the present blit; the default point. */
    OP_POSTFX_SCENE     = 1, /**< After the 3D scene, before the HUD, so the HUD stays unaffected.
                              *   Scene depth is available at `op_scene_depth` (terrain only, not mobys). */
    OP_POSTFX_PRESENT   = 2, /**< Reserved; registration is refused. */
};

/**
 * @brief Sim tick rate in Hz: 1125000/37609 (29.913...).
 *
 * The PSX outputs 263-line non-interlaced video at 2250000/37609 Hz
 * (59.826...) and the game runs one tick per two VBLs. Not 29.97: that is
 * the interlaced field rate, which this game never uses, and a mod
 * converting ticks to seconds with it drifts 0.19 % against game time.
 * `--tick-scale` stretches wall time only; a tick stays one tick.
 */
#define OPENPETE_TICK_HZ  (1125000.0 / 37609.0)
/** @brief VBL rate in Hz: 2250000/37609 (59.826...). */
#define OPENPETE_VBL_HZ   (2250000.0 / 37609.0)

/**
 * @brief Render channels: the renderer's feeder paths.
 *
 * A material selector names one of these. Each channel can be narrowed on
 * the keys listed in openpete_mod_material_selector.
 */
enum {
    OP_CHAN_MOBY     = 1,
    OP_CHAN_TERRAIN  = 2,
    OP_CHAN_PLAYER   = 3,
    OP_CHAN_FLAME    = 4,
    OP_CHAN_PARTICLE = 5,
    OP_CHAN_SKY      = 6,
    OP_CHAN_SHADOW   = 7,
    OP_CHAN_TRACER   = 8,
    OP_CHAN_PORTAL   = 9,   /**< Destination-sky preview inside a portal. */
};
/**
 * @brief Portal preview kinds: the `ident` key for #OP_CHAN_PORTAL.
 *
 * The game draws a portal three ways across the approach; a material
 * keyed on one kind pops as the player walks up, so -1 (all three) is the
 * usual choice.
 */
enum {
    OP_PORTAL_FLAT    = 0,  /**< Far colour chip / close backstop. */
    OP_PORTAL_DOME    = 1,  /**< Full gouraud dome, close up. */
    OP_PORTAL_BLENDED = 2,  /**< Fogged dome, medium cross-dissolve. */
};
/**
 * @brief Particle kinds: the high byte of the `ident` key for #OP_CHAN_PARTICLE.
 *
 * The particle feeder draws six effects. The ident of a draw is
 * `(kind << 8) | sel`, where `sel` is the game's own particle class for
 * the two kinds that carry one (quads and lines) and 0 otherwise.
 * #OP_PART_IDENT builds the key. The ident match is exact, so a kind
 * that carries a class is claimed one class per row. Glow rings and
 * sparkles are separate game renderers that share this channel; a
 * glow-only or sparkle-only material keys on the kind with `sel` 0.
 */
enum {
    OP_PART_QUAD  = 0,  /**< Textured billboard quads; `sel` = particle class. */
    OP_PART_LINE  = 1,  /**< Streak lines; `sel` = particle class. */
    OP_PART_GLOW  = 2,  /**< Additive glow rings around collectables. */
    OP_PART_FLARE = 3,  /**< Sparkle flares (the twinkle quads). */
    OP_PART_SPARK = 4,  /**< Sparkle lines. */
    OP_PART_STAR  = 5,  /**< Dragon-rescue star fan. */
};
/** @brief The `ident` key for one particle kind and class; `sel` is 0 for kinds without a class. */
#define OP_PART_IDENT(kind, sel) \
    ((int)(((unsigned)(kind) << 8) | ((unsigned)(sel) & 0xFFu)))
/** @brief Legacy 0-based path values; prefer `OP_CHAN_*`. */
enum { OP_RP_MOBY = 0, OP_RP_TERRAIN = 1 };
/**
 * @brief Selector refinements.
 *
 * Each pair narrows: setting neither member of a pair selects both.
 */
enum {
    OP_SEL_ENV_ANIM   = 1u << 0,  /**< Env-animated faces only. */
    OP_SEL_HP_TIER    = 1u << 1,  /**< High-poly tier only; neither tier bit = both. */
    OP_SEL_LP_TIER    = 1u << 2,  /**< Low-poly tier only. */
    OP_SEL_UNTEXTURED = 1u << 3,  /**< Gouraud faces only; neither texture bit = both. */
    OP_SEL_TEXTURED   = 1u << 4,  /**< Textured faces only. */
};
/**
 * @brief Every flag the engine understands.
 *
 * A selector carrying any other bit is refused: `struct_size` cannot
 * detect a new flag bit, since adding one does not change `sizeof`.
 */
#define OP_SEL_FLAGS_KNOWN (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | \
                            OP_SEL_LP_TIER  | OP_SEL_UNTEXTURED | \
                            OP_SEL_TEXTURED)

/** @brief Chooses the geometry a material may claim. */
typedef struct openpete_mod_material_selector {
    uint32_t struct_size; /**< `sizeof(*this)` at the mod's compile time; the engine refuses a size it does not know. */
    int      channel;     /**< `OP_CHAN_*`; -1 = any channel. */
    int      ident;       /**< Channel-local identity, meaning per channel: flame ribbon or cap,
                           *   particle `OP_PART_*` kind `(kind << 8 | sel)`, shadow caster kind,
                           *   portal preview kind. -1 = any. Player, sky and tracer emit only 0. */
    int      moby_class;  /**< Moby only; -1 = any. */
    int      texture;     /**< Terrain texture-record id 0..127; -1 = any. */
    int      level;       /**< `g_LevelId`; -1 = any. Sector and texture ids are per level, so a
                           *   key without this matches the same id in every level. */
    unsigned flags;       /**< `OP_SEL_*` refinements. */
} openpete_mod_material_selector_t;
/**
 * @brief A selector that matches everything; narrow only the fields you mean.
 *
 * A zero-initialised selector would mean channel 0 and class 0.
 */
static inline openpete_mod_material_selector_t openpete_mod_material_sel_any(void) {
    openpete_mod_material_selector_t s;
    s.struct_size = (uint32_t)sizeof s;
    s.channel = -1; s.ident = -1; s.moby_class = -1;
    s.texture = -1; s.level = -1; s.flags = 0u;
    return s;
}

/**
 * @brief Key version per channel, stamped into openpete_mod_material_key::version.
 *
 * Bumped when a channel's key members change meaning.
 */
#define OPENPETE_MATERIAL_KEY_VER_MOBY    1u
/** @copydoc OPENPETE_MATERIAL_KEY_VER_MOBY */
#define OPENPETE_MATERIAL_KEY_VER_TERRAIN 1u

/**
 * @brief One instance at refine time.
 *
 * Every field is filled by the engine; @c channel selects the live union
 * member.
 */
typedef struct openpete_mod_material_key {
    uint32_t struct_size;   /**< `sizeof` at engine compile time. */
    uint32_t version;       /**< `OPENPETE_MATERIAL_KEY_VER_*` for @c channel. */
    int      channel;       /**< `OP_CHAN_*`. */
    union {
        struct {
            uint32_t vaddr;      /**< Guest address of the moby record. */
            int      moby_class; /**< `m_Class`. */
        } moby;
        struct {
            int      texture;    /**< Tiledef id, or -1 for the untextured band. */
            unsigned flags;      /**< The `OP_SEL_*` bits describing this face. */
        } terrain;
    } u;
} openpete_mod_material_key_t;

/** @brief Size cap of a material's per-instance uniform block. */
#define OPENPETE_MATERIAL_UNIFORMS_MAX 256u

/**
 * @brief Refine callback (openpete_mod_api::material_register_refine).
 *
 * The selector decides which geometry a material may claim; the refine
 * callback decides per instance whether it does, and with what uniform
 * block. Example: the selector prunes to class 42, the callback claims
 * only instances that are burning.
 *
 * The callback runs during extraction in tick context: guest RAM is
 * readable through `api->guest()` and `op_read_*`; host render state is
 * off limits. Results are memoised per instance, so the callback must be
 * a pure function of the key and tick-stable guest state; the call count
 * is not part of the contract.
 *
 * @param key  The instance.
 * @param out  Uniform block to fill, at most @p cap bytes.
 * @param cap  #OPENPETE_MATERIAL_UNIFORMS_MAX.
 * @return Bytes written to @p out (0 = claim with no parameters), or -1 to
 *         decline: the instance keeps stock shading.
 */
typedef int (*openpete_mod_refine_fn)(const openpete_mod_material_key_t* key,
                                      void* out, uint32_t cap);

/** @brief The api version this header describes. */
#define OPENPETE_MOD_API_VERSION 11u

/** @brief The engine's function table, passed to the mod entry point. */
typedef struct openpete_mod_api {
    /** @brief The engine's #OPENPETE_MOD_API_VERSION; compare with `<`, never `!=`.
     * @since api 1 */
    uint32_t api_version;

    /** @name Overrides
     * Attach an override to a guest function by symbol name (resolved
     * through openpete_sdk_symbols.h, e.g. "CameraUpdate") or by PSX vaddr
     * (pinned to this binary; the loader warns once per mod on the first
     * raw registration). Priority is the enabled-list order: overrides
     * from later-listed mods run first, and base() walks toward earlier
     * mods and then the recompiled original.
     * @{ */
    /**
     * @brief Attach an override by PSX vaddr.
     * @return 0 on success, non-zero for invalid arguments.
     * @since api 1
     */
    int (*override_addr)(openpete_mod_t* self, uint32_t addr,
                         openpete_mod_override_fn fn);
    /**
     * @brief Attach an override by symbol name.
     * @return 0 on success, non-zero for an unknown name.
     * @since api 1
     */
    int (*override_name)(openpete_mod_t* self, const char* name,
                         openpete_mod_override_fn fn);

    /**
     * @brief Run the next override in the chain, or the original.
     *
     * Valid only while an override registered by this mod is on the call
     * stack. May be called any number of times; each call runs the
     * continuation once.
     * @since api 1
     */
    void (*base)(CPUState* cpu);

    /**
     * @brief Call a guest function by vaddr through the dispatch layer, so
     *        the callee's own override chain runs.
     *
     * Write `cpu->a0..a3` before, read `cpu->v0` after. Valid only inside
     * an override.
     * @since api 1
     */
    void (*call)(CPUState* cpu, uint32_t addr);
    /** @} */

    /** @name Guest memory
     * @{ */
    /**
     * @brief Translate a guest vaddr (kuseg, kseg0 or kseg1) to a host pointer.
     *
     * The 2 MB RAM, the scratchpad and guest_alloc's arena all resolve.
     * Guest RAM and every supported host are little-endian, so typed reads
     * and writes through the pointer see what the game sees. Pointer
     * fields inside guest structs hold guest vaddrs; translate them again
     * before dereferencing. Global addresses are `OP_GADDR_*` in
     * openpete_sdk_symbols.h. Sim thread only.
     * @return The host pointer, or NULL for an address that names nothing,
     *         including guest address 0.
     * @since api 1
     */
    void* (*guest)(uint32_t vaddr);

    /**
     * @brief Inverse of guest().
     *
     * Interior pointers map to the corresponding interior vaddr.
     * @return The guest vaddr of a host pointer into RAM, the scratchpad or
     *         an arena allocation; 0 if the pointer names no guest address.
     * @since api 1
     */
    uint32_t (*guest_addr)(const void* host);

    /**
     * @brief Allocate guest-addressable memory.
     *
     * The arena is a bump allocator with no free: allocate at entry and
     * reuse. Tick context only: call it, and write through @p host_view,
     * only from entry, tick hooks and toggle hooks.
     *
     * Contents are zero on first allocation. On reload the allocation
     * sequence is replayed: the i-th call with the same size, alignment
     * and flags returns the same vaddr and view, holding the bytes as
     * last written. Initialise what needs initialising.
     *
     * The vaddr and host view are stable for the process lifetime, and
     * the bytes are reachable through every engine guest access path
     * (`cpu->read_*`, `cpu->write_*`, guest()).
     *
     * Unless flagged #OP_GALLOC_IMMUTABLE the bytes are timeline state:
     * savestates, rewind, runahead and process handoff carry and restore
     * them like guest RAM. An allocation made after a snapshot keeps its
     * contents when that snapshot is restored; the cursor never rewinds.
     * Keep mutable per-session state here rather than in host statics,
     * and store guest vaddrs rather than host pointers, since carried
     * bytes cross process boundaries at handoff.
     *
     * Not promised: the arena's base or layout (never hardcode a vaddr; a
     * savestate whose allocation sequence does not prefix-match the live
     * session is refused, naming the mod and entry), survival of
     * @p host_view across handoff, or any particular snapshot mechanism.
     *
     * Model vertex streams referenced from animation frame words are
     * parked here too; the frame word carries a handle from
     * vstream_handle(), not an address.
     *
     * @param size       Bytes to allocate.
     * @param align      Power of two; 0 = default.
     * @param flags      `OP_GALLOC_*`. An unknown flag fails the call and
     *                   logs the flag.
     * @param host_view  Receives a host alias of the same bytes.
     * @return The guest vaddr; 0 = exhausted, unavailable or invalid.
     * @since api 1
     */
    uint32_t (*guest_alloc)(openpete_mod_t* self, uint32_t size,
                            uint32_t align, uint32_t flags,
                            void** host_view);
    /** @} */

    /** @name Lifecycle
     * @{ */
    /**
     * @brief printf-style logging, tagged `mod:<id>`.
     * @param level `OP_MOD_LOG_*`.
     * @since api 1
     */
    void (*log)(openpete_mod_t* self, int level, const char* fmt, ...);

    /**
     * @brief Per-mod persistent data directory (`mods/<id>/data`, created on first call).
     * @return An engine-owned absolute path; use plain stdio inside it.
     * @since api 1
     */
    const char* (*data_dir)(openpete_mod_t* self);

    /**
     * @brief Register the live-toggle callback.
     *
     * Runs at the tick boundary after each flip of this mod's enable
     * state. The engine freezes a disabled mod's registrations but never
     * rewinds guest state, so a mod that wrote guest state restores the
     * stock bytes on `on = 0`. One registration per mod; re-registering
     * replaces.
     * @return 0.
     * @since api 1
     */
    int (*register_toggle_hook)(openpete_mod_t* self,
                                openpete_mod_toggle_fn fn);
    /** @} */

    /** @name Settings
     * Read a value from `mods/<id>/config.toml`. Keys are dotted paths:
     * "chests.collision" is key `collision` in table `[chests]`. @p def is
     * returned when the file, the key or a type-compatible value is
     * absent. The file is parsed once, before entry runs; when the user
     * commits a change in the Mods panel (rendered from the manifest's
     * `[[config]]` rows) the engine rewrites config.toml and reloads the
     * mod, so read config at entry. Coercion: bools read as 0/1 through
     * config_int and config_float; ints read through config_float; floats
     * truncate through config_int; strings never coerce.
     * @{ */
    /** @brief Read a bool. @since api 1 */
    int     (*config_bool)(openpete_mod_t* self, const char* key, int def);
    /** @brief Read an integer. @since api 1 */
    int64_t (*config_int)(openpete_mod_t* self, const char* key,
                          int64_t def);
    /** @brief Read a float. @since api 1 */
    double  (*config_float)(openpete_mod_t* self, const char* key,
                            double def);
    /**
     * @brief Read a string.
     *
     * Copies at most `cap-1` chars plus NUL into @p buf.
     * @return 1 if the key was present, 0 if the default was copied.
     * @since api 1
     */
    int     (*config_str)(openpete_mod_t* self, const char* key,
                          const char* def, char* buf, int cap);

    /**
     * @brief Print a status line into this mod's section of the Mods panel.
     *
     * Legal from any context; host state only. Lines accumulate within a
     * tick and persist until the next tick in which the mod calls
     * ui_status again, so a line printed once stays visible and a per-tick
     * printer repaints its block each tick. The block truncates past about
     * 24 lines. Custom panels use openpete_mod_ui.h, or openpete_imgui.h
     * for the whole ImGui C API.
     * @since api 1
     */
    void (*ui_status)(openpete_mod_t* self, const char* fmt, ...);
    /** @} */

    /** @name Present hooks and screen-space draws
     * @{ */
    /**
     * @brief Register a per-present hook.
     *
     * Runs once per scene the engine extracts (the canonical one each tick
     * plus every sub-tick scene at a higher `--render-fps`). The hook
     * fires when its scene is extracted on the sim thread, ahead of
     * display, and the engine may produce fewer scenes than display
     * slots; call count and timing are not part of the contract.
     * `ctx->alpha` is the animation moment the scene shows and `ctx->dt`
     * the spacing to the previous scene, so a curve evaluated at
     * `ctx->alpha` is correct under any cadence. Publish state from tick
     * hooks into your own variables and evaluate it here; the engine never
     * interpolates mod visuals.
     *
     * Render-pure, enforced: guest access, game calls, base() and override
     * registration are refused inside a present hook, since presents are
     * non-deterministic in count and timing.
     *
     * Runs only when something is presented: a plain headless run never
     * calls it, a headless run that takes a screenshot does.
     * @since api 1
     */
    int (*register_present_hook)(openpete_mod_t* self,
                                 openpete_mod_present_fn fn);

    /**
     * @brief Draw text in overlay space on the native renderer.
     *
     * Overlay space: units of window height, origin top-left, +y down, x
     * in [0, aspect], y in [0, 1]. Host state only. Immediate mode: a call
     * from tick context shows for all of that tick's presents (redraw
     * every tick); a call from a present hook shows for that present.
     * Glyph set: digits, A-Z (lowercase folds up), space, and
     * `% / ? + ^ . ' :`, drawn with the game's own gold text glyphs; text
     * appears once the game has loaded the glyph font. Headless and
     * PsyCross-only runs consume nothing.
     * @param cap_h  Glyph cap height in overlay units.
     * @param rgb    Glyph tint (0x605010 = HUD gold).
     * @since api 1
     */
    void (*draw_text)(float x, float y, float cap_h, uint32_t rgb,
                      const char* str);
    /** @} */

    /** @name Post-process shaders
     * @{ */
    /**
     * @brief Register a GLSL shader by path relative to the mod directory.
     *
     * The extension selects the stage (e.g. "shaders/crt.frag"). The
     * engine compiles it to SPIR-V on load (in-process glslang, Vulkan
     * 1.0 dialect), cached by content hash under `.build/shaders/`. A
     * prebuilt `<path>.spv` next to a missing source loads instead if a
     * `<path>.spv.ihash` sidecar matches the current shader-interface
     * hash. `#include "openpete_shader_api.glsl"` resolves to the SDK
     * interface header. Entry or tick context. Sources join the
     * hot-reload watch.
     * @return A shader handle >= 0, or -1 for a missing file or compile
     *         error (see `.build/shaders/<name>.log`; the mod still loads).
     * @since api 1
     */
    int (*shader_register)(openpete_mod_t* self, const char* path);

    /**
     * @brief Register a fullscreen fragment pass at an injection point.
     *
     * Pass order across mods is enabled-list priority; @p order breaks
     * ties within a mod (lower runs first). The pass samples the previous
     * pass (set 0) and receives the engine UBO (time, tick, alpha,
     * resolution; see `shaders/openpete_shader_api.glsl`) plus the mod's
     * uniform block. Entry or tick context.
     * @param frag_shader  Handle from shader_register().
     * @param point        `OP_POSTFX_*`.
     * @return An fx handle >= 0, registered disabled, or -1.
     * @since api 1
     */
    int (*postfx_register)(openpete_mod_t* self, int frag_shader,
                           int point, int order);
    /** @brief Enable or disable a pass. Any context. @since api 1 */
    int (*postfx_enable)(openpete_mod_t* self, int fx, int enabled);
    /**
     * @brief Set a pass's uniform block.
     *
     * Copies @p data on call. Any context; push from tick or present hooks.
     * @param size At most 1024 bytes.
     * @since api 1
     */
    int (*postfx_set_uniforms)(openpete_mod_t* self, int fx,
                               const void* data, uint32_t size);
    /** @} */

    /** @name Audio
     * @{ */
    /**
     * @brief Substitute the sample behind a named game sound.
     *
     * The code form of dropping `<name>.wav` into this mod's `assets/sfx`.
     * The swap happens at the voice's PCM upload, below the SPU RAM image
     * and voice mirror, so guest RAM, the gameplay rand() stream and the
     * tick on which the game sees the sound end (it polls the stock
     * sample's duration) stay stock; a longer replacement rings out past
     * that tick. Sounds without a SoundTable name (a moby's own m_Sounds
     * def) are unreachable here; a hash-named file in `assets/sfx/`
     * covers every sample and needs no code.
     * @param name        A game sound name.
     * @param adpcm_body  Raw SPU-ADPCM frames (no VAG header, final block
     *                    end-flag 0x01); decoded and copied by the engine.
     * @param pitch       SPU units (0x1000 = 44.1 kHz), overriding the
     *                    game's pitch writes; 0 keeps the game's own pitch.
     * @return 0, or -1 for an unknown name.
     * @since api 1
     */
    int (*sfx_replace)(openpete_mod_t* self, const char* name,
                       const void* adpcm_body, uint32_t len,
                       uint16_t pitch);
    /**
     * @brief Play `assets/sfx/own/<name>.wav` from this mod's directory as
     *        a host-side one-shot.
     *
     * Never routed through the SPU, so it is legal from tick and present
     * hooks. WAVs are cached after the first play.
     * @param gain 0..4; 1.0 = as authored.
     * @return 0, or -1 for a missing file or no audio device (logged).
     * @since api 1
     */
    int (*sfx_play)(openpete_mod_t* self, const char* name, float gain);
    /** @} */

    /** @name Input bindings
     * @{ */
    /**
     * @brief Read a `[[binding]]` declared in this mod's mod.toml.
     *
     * Declared bindings (`{name, key, purpose}`) are conflict-checked
     * across the enabled set at load, and the user rebinds through
     * openpete.toml `[keys.mod.<id>]` (keyboard names, gamepad names such
     * as "pad:north", or "none"). Reading host input in a tick hook
     * diverges gameplay from a replay by design; display-only reads
     * belong in present hooks.
     * @return 1 while the bound host key is held; an undeclared name logs
     *         an error and returns 0.
     * @since api 1
     */
    int (*binding_down)(openpete_mod_t* self, const char* name);
    /** @} */

    /** @name Game data and texture packs
     * @{ */
    /**
     * @brief Read one level's raw Data blob from the mounted disc.
     *
     * Lets a mod use another level's sky, palette or geometry without
     * shipping derived data.
     * @param buf  Destination of @p cap bytes; NULL queries the length.
     * @return The blob length, or < 0 when no game layer provides level
     *         data or the id is unknown.
     * @since api 2
     */
    int (*read_level_data)(openpete_mod_t* self, int level_id,
                           void* buf, uint32_t cap);

    /**
     * @brief Retarget the texture-pack fog ambience.
     *
     * Fogged replacement texels blend toward `scale*stock+offset`
     * (offsets in 0..255 units) instead of the stock ladder colours, for
     * packs whose art re-grades the level. NULL/NULL restores stock.
     * Per-texture grade fits take precedence where they have signal.
     * Display only.
     * @since api 2
     */
    int (*texpack_fog_shift)(openpete_mod_t* self, const float scale_rgb[3],
                             const float offset_rgb[3]);
    /** @} */

    /** @name Materials
     * @{ */
    /**
     * @brief Attach a fragment shader to a set of geometry.
     *
     * The set is chosen by a render channel and that channel's keys (moby
     * class, terrain texture or tier, channel-local identity, level).
     * Build the selector with openpete_mod_material_sel_any() and narrow
     * only what you mean; the engine refuses a selector it cannot fully
     * honour rather than matching wider than asked.
     *
     * The shader reshades within the draw group the geometry already
     * belongs to: it cannot change blending, depth policy or draw order.
     * Unmatched or failed builds keep stock shading; a material cannot
     * make geometry invisible.
     * @param shader A frag handle from shader_register().
     * @return A material handle, registered enabled, or -1.
     * @since api 3
     */
    int (*material_register)(openpete_mod_t* self,
                             const openpete_mod_material_selector_t* sel,
                             int shader);

    /**
     * @brief Turn one registered material on or off.
     *
     * Callable from any context; the natural target of a `[[config]]`
     * toggle.
     * @since api 3
     */
    int (*material_enable)(openpete_mod_t* self, int material, int on);

    /**
     * @brief Set the uniform block the material's shader reads.
     *
     * May be called per tick; for time-varying effects prefer the engine
     * UBO's time in the shader.
     * @param block std140, at most #OPENPETE_MATERIAL_UNIFORMS_MAX bytes.
     * @since api 3
     */
    int (*material_set_params)(openpete_mod_t* self, int material,
                               const void* block, uint32_t len);

    /**
     * @brief material_register() with a per-instance refine callback.
     *
     * The engine calls @p refine once per candidate instance during
     * extraction and it returns that instance's uniform block, or -1 to
     * leave it stock. Instances with byte-identical blocks share one slot
     * and one batch, so a callback with few distinct outcomes is cheap and
     * one with a distinct block per instance costs a draw each. Scope the
     * selector tightly and let the callback decide the rare cases.
     *
     * Available on the moby channel (keyed per instance) and on terrain
     * (keyed per texture id or the untextured band, asked once per
     * present); on other channels register declaratively and drive the
     * material with material_set_params().
     * @since api 4
     */
    int (*material_register_refine)(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int shader, openpete_mod_refine_fn refine);

    /**
     * @brief Point a live material at a different selector.
     *
     * Keeps its handle, shader, refine callback and enabled state.
     * Selection is constant across a present, so a call from a present
     * hook or UI section takes effect at the next tick; from entry or a
     * tick hook it applies at once. The material keeps shading under its
     * old key until the new one applies. The selector is validated as
     * material_register() validates it.
     * @since api 6
     */
    int (*material_set_selector)(openpete_mod_t* self, int material,
                                 const openpete_mod_material_selector_t* sel);

    /**
     * @brief The moby classes the current level has loaded, ascending.
     *
     * Level-scoped, not visibility-scoped: a superset of the classes with
     * a live instance at this instant, and the set worth offering a user
     * to choose from. Reads an engine-side snapshot refreshed once per
     * tick, so it is safe from a UI section. Empty before the first level
     * finishes loading.
     * @param out  Destination of @p cap entries; `out = NULL, cap = 0`
     *             sizes the buffer.
     * @return The count.
     * @since api 6
     */
    int (*moby_classes)(openpete_mod_t* self, uint16_t* out, int cap);
    /** @} */

    /** @name Vertex streams
     * @{ */
    /**
     * @brief A stream handle for a vertex stream parked in the arena.
     *
     * The handle is the value to write into an animation frame word's
     * 21-bit vertex-stream field (bits 0-20) in place of an address that
     * does not fit. The game's readers resolve it to the full vaddr, so a
     * stream may live anywhere guest_alloc() puts it. The same vaddr
     * always yields the same handle; handles are below 0x8000 and never
     * 0. The game stores frame-word addresses halved; a handle is written
     * as-is in both encodings.
     *
     * Ask at entry, in a fixed order, once per stream: every call is
     * ledgered like an allocation, replayed on reload, and verified when
     * a savestate loads, so a sequence that depends on the order levels
     * were visited makes a savestate from another route refuse to load.
     * Tick context only.
     * @return The handle, or 0 for vaddr 0, a kernel-area vaddr, or an
     *         exhausted table.
     * @since api 9
     */
    uint32_t (*vstream_handle)(openpete_mod_t* self, uint32_t stream_vaddr);
    /** @} */

    /** @name Notifications
     * @{ */
    /**
     * @brief Post a transient on-screen notification.
     *
     * Drawn in the game's gold-bordered chrome at the anchor the player
     * chose. ui_status() reports state; notify reports an event.
     *
     * Legal from entry, tick hooks and present hooks; host state only.
     * Rate-limited per mod (a small burst, then a slow refill). A no-op
     * on runahead and rewind-replay ticks, whose effects are rolled back
     * or re-executed; a mod therefore sees fewer toasts than calls.
     *
     * Glyph set: 0-9, A-Z (lowercase folds up), space, and
     * `% / ? + ^ . ' :`. Any other character renders as a space.
     *
     * @param key         May be NULL. Coalesces: a live toast with the
     *                    same key is repainted with the new text and a
     *                    restarted timer instead of a second toast
     *                    stacking. The engine prefixes the mod id, so keys
     *                    never collide across mods.
     * @param timeout_ms  0 takes the player's `--osd-timeout` default;
     *                    values above 30000 clamp to it. A repaint takes
     *                    the repainting call's timeout.
     * @since api 8
     */
    void (*notify)(openpete_mod_t* self, const char* key,
                   uint32_t timeout_ms, const char* fmt, ...);
    /** @} */
} openpete_mod_api_t;

/**
 * @brief ABI descriptor every code object exports as `openpete_mod_abi`.
 *
 * Records the api version and api-struct size the object was compiled
 * against. The loader reads it before the entry point runs and refuses an
 * object built for a newer api or a larger struct than the engine has,
 * and an object with no descriptor. An older object loads, since the
 * struct is append-only from api 10.
 *
 * The engine adds the SDK's openpete_mod_abi.c to every compile line, so
 * a mod's own sources need not define the descriptor. A hand-built object
 * compiles that file in, or puts #OPENPETE_MOD_ABI_DEFINE in exactly one
 * of its sources.
 */
typedef struct openpete_mod_abi {
    uint32_t magic;       /**< #OPENPETE_MOD_ABI_MAGIC. */
    uint32_t api_version; /**< #OPENPETE_MOD_API_VERSION at the mod's compile time. */
    uint32_t api_size;    /**< `sizeof(openpete_mod_api_t)` at the mod's compile time. */
} openpete_mod_abi_t;
/** @brief 'OPAB'. */
#define OPENPETE_MOD_ABI_MAGIC 0x4F504142u
/** @brief Defines the ABI descriptor; use in exactly one source of a hand-built object. */
#define OPENPETE_MOD_ABI_DEFINE()                                          \
    const openpete_mod_abi_t openpete_mod_abi = {                          \
        OPENPETE_MOD_ABI_MAGIC, OPENPETE_MOD_API_VERSION,                  \
        (uint32_t)sizeof(openpete_mod_api_t) }

/** @name Typed guest accessors
 * One read and one write per width and signedness: `op_read_{u,s}{8,16,32}`
 * and `op_write_{u,s}{8,16,32}`. A read of an unmapped address returns 0;
 * a write to one is dropped. Alignment is the guest's: a 16-bit access on
 * an odd address or a 32-bit access off a 4-byte boundary is a bus error
 * on the PSX and is not something the game's data does.
 * @{ */
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
static inline void op_write_s32(const openpete_mod_api_t* api, uint32_t va,
                                int32_t v) {
    op_write_u32(api, va, (uint32_t)v);
}
static inline uint16_t op_read_u16(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint16_t*)p : 0u;
}
static inline void op_write_u16(const openpete_mod_api_t* api, uint32_t va,
                                uint16_t v) {
    void* p = api->guest(va);
    if (p) *(uint16_t*)p = v;
}
static inline int16_t op_read_s16(const openpete_mod_api_t* api, uint32_t va) {
    return (int16_t)op_read_u16(api, va);
}
static inline void op_write_s16(const openpete_mod_api_t* api, uint32_t va,
                                int16_t v) {
    op_write_u16(api, va, (uint16_t)v);
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
static inline int8_t op_read_s8(const openpete_mod_api_t* api, uint32_t va) {
    return (int8_t)op_read_u8(api, va);
}
static inline void op_write_s8(const openpete_mod_api_t* api, uint32_t va,
                               int8_t v) {
    op_write_u8(api, va, (uint8_t)v);
}
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_API_H */
