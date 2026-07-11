// openpete_mods.h — ENGINE-facing mod framework API (loader + chains).
//
// The mod-facing surface lives in openpete_mod_api.h; this header is what
// the runtime and per-game code call. See
// notes/plans/openpete-modding-framework.md (P1 vertical slice).
//
// Interposition rides the recompiler's universal function-entry
// chokepoint: every recompiled function's prologue calls
// psx_override_dispatch(cpu, addr) (code_generator.cpp), which fans into
// psx_override_dispatch_game(), which calls openpete_mods_dispatch()
// below. Direct jal calls, indirect calls, and dispatched calls all pass
// through the callee's own prologue, so a per-address chain here
// intercepts every call to a chosen function with zero generated-C edits.
// Cost when no mods are loaded: one global-flag branch
// (openpete_mods_active == 0).

#ifndef OPENPETE_MODS_H
#define OPENPETE_MODS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* FILE — savestate mods-section hooks */

#include "openpete.h" /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/* Name→vaddr symbol table (generated from manifest/symbols.toml by
 * tools/gen_mod_sdk.py into sdk_symbols_table.c, game-side). The game's
 * main registers it before openpete_run(). Entries sorted by name. */
typedef struct {
    const char* name;
    uint32_t    addr;
    uint8_t     legacy; /* 1 = historical raw alias (func_XXXXXXXX for a
                         * since-renamed function) — resolvable forever
                         * (decomp promotion contract), but the loader
                         * warns so mods migrate to the stable name. */
} openpete_sdk_symbol_t;

void openpete_mods_set_symbol_table(const openpete_sdk_symbol_t* table,
                                    size_t count);

/* Discover, build, load, and initialize mods. Called once at boot after
 * options parsing (core/runtime/run.c). Resolution order for the mods
 * directory: opts->mods_dir → env OPENPETE_MODS_DIR → <exe>/mods.
 * A missing/empty directory is not an error: zero mods loaded,
 * openpete_mods_active stays 0, and the sim path is byte-identical to a
 * modless build (inertness gate #1/#2). Returns the number of mods
 * loaded (>= 0), or -1 on a hard setup error (which disables mods and
 * continues; mods never brick a launch). */
int openpete_mods_init(const openpete_options_t* opts);

/* Per-function-entry chain dispatch. Returns 1 = an override chain ran
 * (skip the recompiled body), 0 = no chain for addr (run the body).
 * Called from psx_override_dispatch_game behind the
 * openpete_mods_active fast gate. */
int openpete_mods_dispatch(CPUState* cpu, uint32_t addr);

/* 1 while at least one override chain is registered. Hot-path gate. */
extern int openpete_mods_active;

/* Crash attribution: non-NULL = an override chain frame is live on this
 * thread; returns the innermost mod id (addr_out = the overridden
 * vaddr). Consumed by core/runtime/crash.c so a crash report names the
 * offending mod (PoC rung 6). */
const char* openpete_mods_crash_attribution(uint32_t* addr_out);

/* ── Draw service (core/mods/mod_draw.c) ───────────────────────────── */

#include "openpete_mod_api.h" /* openpete_mod_present_fn / ctx */

/* Per-present flush, called from run.c's two present legs (canonical +
 * sub-tick) just before openpete_native_render_frame(). canonical=1
 * commits the tick-scoped command buffer (the sim tick that just ran);
 * both legs run present hooks, emit queued actor draws into the scene
 * (cpu = the game CPU, for the game-side extractor's guest reads), and
 * push the composed overlay vertices to the native renderer. Near-zero
 * cost when no mod ever drew. */
void openpete_mods_present_flush(CPUState* cpu, int canonical);

/* Game-registered text rasterizer: lays out `str` at (x, y) with glyph
 * cap-height cap_h (overlay space) and tint rgb into out_verts (an
 * openpete_hud_vert array, cap entries), returning the vertex count.
 * Spyro registers a text_moby_native wrapper (main.cpp). */
typedef int (*openpete_mods_text_emit_fn)(const char* str, float x, float y,
                                          float cap_h, unsigned rgb,
                                          void* out_verts, int cap);
void openpete_mods_draw_set_text_fn(openpete_mods_text_emit_fn fn);

/* Game-registered actor pose capture + emit (the actor render service —
 * rung 16). capture snapshots the live pose of actor `kind` into an
 * opaque blob (returns bytes or -1); it reads the game's own
 * hook-consistent snapshot, never mid-tick state, and works headless.
 * emit draws the actor at a recorded pose into the CURRENT frame's
 * native scene (returns triangles emitted, 0 = skipped safely). Spyro
 * registers spyro1_actor_* wrappers (main.cpp). */
typedef int (*openpete_mods_actor_capture_fn)(int kind, void* buf, int cap);
typedef int (*openpete_mods_actor_emit_fn)(CPUState* cpu, int kind,
                                           const void* pose, int len,
                                           uint32_t tint_rgba);
void openpete_mods_draw_set_actor_fns(openpete_mods_actor_capture_fn cap,
                                      openpete_mods_actor_emit_fn emit);

/* Loader-facing submission entry points (wired into the mod api). */
void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str);
void openpete_mods_draw_rect(float x, float y, float w, float h,
                             uint32_t rgba);
int  openpete_mods_capture_actor_pose(int kind, void* buf, int cap);
int  openpete_mods_draw_actor(int kind, const void* pose, int len,
                              uint32_t tint_rgba);
void openpete_mods_suppress_player_draw(void);
int  openpete_mods_register_present_hook(openpete_mod_present_fn fn,
                                         const char* owner_id);

/* 1 while a present hook is executing on this thread — the loader's
 * guest/call/override entry points refuse during it (render-purity). */
extern int openpete_mods_in_present;

/* ── Instruction-site hook tier (core/mods/mod_sites.c, rung 14) ───────
 *
 * Sites are declared in mod.toml [[site]] blocks and BAKED at recomp-
 * regen time: tools/gen_mod_sites.py collects enabled mods' sites into
 * generated/mod_sites.txt (folded into --channel-sites) and emits
 * generated/mod_sites_registry.c mapping (mod_id, name) → the baked
 * index the guarded emission reads. The generated C tests
 * openpete_modsite_active[idx] and calls openpete_modsite_call() — both
 * defined in mod_sites.c, declared for the generated code in
 * psx_runtime.h. Identity while inactive; behaviorally inert by
 * construction (oracle-provable), byte-identical to stock when the site
 * set is empty (regen-without-sites). */

/* Hard cap on baked sites — the generated C indexes a fixed-size flag
 * array; tools/gen_mod_sites.py enforces the same bound at collection. */
#define OPENPETE_MODSITE_MAX 256

typedef struct {
    const char* mod_id; /* owning mod */
    const char* name;   /* site name within the mod's namespace */
    uint32_t    addr;   /* anchored guest instruction vaddr (diagnostics) */
    int         idx;    /* baked index into openpete_modsite_active[] */
} openpete_modsite_desc_t;

/* Registered by the game's main() from the generated registry (same
 * pattern as the SDK symbol table). Count 0 = no sites baked. */
void openpete_mods_set_site_registry(const openpete_modsite_desc_t* table,
                                     int count);

/* Loader-facing attach (wired into the mod api's site_handler). Returns
 * 0 = handler live, -1 = (mod_id, name) not baked into this binary (the
 * loader logs the regen recipe; the site stays inert). */
int openpete_mods_site_attach(const char* mod_id, const char* name,
                              void (*fn)(CPUState* cpu, uint32_t site_addr),
                              const char* log_tag);

/* Detach every handler owned by mod_id (unload/reload path). */
void openpete_mods_site_detach_all(const char* mod_id);

/* ── Content registries (core/mods/mod_registry.c, rungs 11-12) ────────
 *
 * Namespaced additive content (`modid:entry`). v1 namespace: MOBY TYPES.
 * The capacity model is BattleShip's fighter_registry: rows seeded from
 * vanilla at level load — a registered type claims a class id whose
 * model row is FREE in the current level (a NULL g_Models row means the
 * level never uses that class, so the overlay update switch has no case
 * for it and the id falls through to `default` by construction) and
 * seeds the row from a donor class. Ids are LEVEL-SCOPED: invalidated
 * and re-allocated on level change (the loader repopulates the model
 * table). Lifts beyond the vanilla table size (>=512) ride
 * openpete-ram-virtualization — acknowledged, not hidden.
 *
 * Behavior is CODE, not a table: the game dispatches per-type behavior
 * through compiled switches, so registered types get a mod-provided
 * update_fn the registry dispatches after the game's own update tick
 * (a chain link on the game's master-update function, added lazily on
 * first registration — no types registered = no hook = inert). */

typedef void (*openpete_moby_update_fn)(CPUState* cpu, uint32_t moby_vaddr);

/* Game ops the registry delegates to (game addresses stay game-side;
 * Spyro registers spyro1_moby_ops from main.cpp). */
typedef struct {
    uint32_t update_hook_addr; /* master per-tick update fn (chain anchor) */
    /* Current level id, 0 = none/not in gameplay yet. */
    int (*level_id)(CPUState* cpu);
    /* Claim + seed a free model row from donor_class; 0 = not possible
     * yet (level loading / no free row / donor absent). Idempotent per
     * level: each call claims a FRESH row (seeded rows read non-free). */
    int (*alloc_seed_class)(CPUState* cpu, int donor_class);
    /* Spawn through the game's own spawner AS donor_class (full class-
     * correct init: model, renderer bits, collision), then rebrand the
     * instance to class_id. Returns guest moby vaddr, 0 on failure. */
    uint32_t (*spawn)(CPUState* cpu, int class_id, int donor_class);
    /* Call fn for every live moby of class_id. */
    void (*walk_class)(CPUState* cpu, int class_id,
                       openpete_moby_update_fn fn);
} openpete_moby_ops_t;

void openpete_mods_set_moby_ops(const openpete_moby_ops_t* ops);

/* Loader-facing (wired into the mod api). */
int openpete_mods_moby_register(const char* mod_id, const char* name,
                                int donor_class,
                                openpete_moby_update_fn update_fn,
                                const char* log_tag);
int openpete_mods_moby_type_id(const char* mod_id, const char* name);
uint32_t openpete_mods_moby_spawn(CPUState* cpu, const char* mod_id,
                                  const char* name);

/* ── Engine services (core/mods/mod_services.c) ────────────────────── */

/* Queue a mod-requested savestate op (1=save, 2=load) for execution at
 * the host loop's quiescent point. One pending op at a time. */
int openpete_mods_savestate_request(int op, unsigned slot,
                                    const char* owner);

/* run.c: consume the pending op (0 = none; clears). Execute it exactly
 * like the user hotkeys (game fiber yielded, before pad refresh). */
int openpete_mods_pending_state_op(unsigned* out_slot);

/* Loader-facing registration for per-mod savestate participation. */
int openpete_mods_state_register(openpete_mod_state_save_fn save,
                                 openpete_mod_state_load_fn load,
                                 const char* owner);

/* Asset-overlay roots: one per enabled mod with an assets/ dir, in
 * enabled order (= priority; later wins). Consumers (texture pack,
 * music override) layer these over their base dirs. */
int         openpete_mods_register_asset_root(const char* dir,
                                              const char* owner);
int         openpete_mods_asset_root_count(void);
const char* openpete_mods_asset_root(int i, const char** owner);

/* savestate.c: append/read the trailing "mods" section (per-mod blobs).
 * Read buffers while the file is open and tolerates pre-mods files
 * (EOF ⇒ nothing to restore); dispatch runs after the engine committed
 * guest state so mod load_fns see the restored world. */
int  openpete_mods_state_write_section(FILE* fp);
int  openpete_mods_state_read_section(FILE* fp);
void openpete_mods_state_dispatch(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MODS_H */
