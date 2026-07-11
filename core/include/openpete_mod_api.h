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
// Override semantics (P1 of notes/plans/openpete-modding-framework.md):
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

/* Savestate participation (see register_savestate_hooks): serialize the
 * mod's own non-canonical state alongside the engine snapshot. save
 * returns bytes written into buf (<= cap, cap is 64 KiB) or -1; load
 * receives the blob saved by the same mod id. */
typedef int  (*openpete_mod_state_save_fn)(void* buf, int cap);
typedef void (*openpete_mod_state_load_fn)(const void* buf, int len);

enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

#define OPENPETE_MOD_API_VERSION 1u

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

    /* ── Instruction-site hook tier (PoC rung 14 — the escape hatch) ──
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

    /* ── Content registry: moby types (PoC rung 11) ──────────────────
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

    /* ── Content registry: level slots (PoC rung 12) ─────────────────
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

    /* ── One-shot sound (engine services) ────────────────────────────
     * Play `assets/sounds/<name>.wav` from this mod's directory as a
     * HOST-side additive one-shot (OpenAL; never the SPU — sim-neutral
     * by construction, legal from tick and present hooks). gain 0..4,
     * 1.0 = as-authored; WAVs are cached after the first play.
     * 0 ok / -1 failed (missing file, no audio — logged, never fatal). */
    int (*play_sound)(openpete_mod_t* self, const char* name, float gain);

    /* ── Declared input bindings (engine services) ───────────────────
     * Resolve a [[binding]] declared in this mod's mod.toml
     * ({name, key, purpose}) to the live host key state. Declaring
     * bindings (instead of raw host_key_down) surfaces conflicts across
     * the enabled set at load time and lets a future mod menu rebind
     * without touching the mod. Same tick-context divergence caveats as
     * host_key_down. Undeclared name = error log + 0. */
    int (*binding_down)(openpete_mod_t* self, const char* name);

    /* ── bk2 input-movie status (engine services, read-only v1) ──────
     * active: a movie is loaded this session; frame: current movie
     * frame (<0 before the offset); frame_count: movie length;
     * overriding: this tick's input actually came from the movie.
     * All zeros when no movie. Record/replay CONTROL stays engine-
     * owned (mid-session anchoring is TAS infra). out fields are
     * int-sized; pass a struct of 4 ints. */
    void (*bk2_status)(openpete_mod_t* self, int* active, int* frame,
                       int* frame_count, int* overriding);
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
