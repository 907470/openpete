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
