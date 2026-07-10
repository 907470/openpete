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
