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

/* Compile-on-boot UX pass (plan openpete-mod-compile-on-boot): compile
 * any enabled source mod whose content-hash build key is stale, BEFORE
 * openpete_mods_init() — the game calls this while its splash is still
 * alive so the compiles render as a progress stage instead of a silent
 * stall. Pass 1 hashes every enabled src mod (milliseconds); if nothing
 * is stale it returns 0 without invoking either callback (no window
 * flash on warm boots). Otherwise the stale set compiles inside ONE
 * `run_task` callback (NULL = inline), reporting
 * progress("COMPILING MODS", done, total) from the worker.
 *
 * Purely an acceleration/UX layer: init behaves identically whether or
 * not prepare ran (it re-checks the cache and compiles anything still
 * stale, inline and silent). Compile failures are logged and left for
 * init to disable — never fatal. Returns the number of mods that were
 * stale. */
typedef void (*openpete_mods_progress_fn)(const char* stage, int done,
                                          int total);
typedef int (*openpete_mods_task_fn)(void* userdata);
typedef int (*openpete_mods_run_task_fn)(openpete_mods_task_fn fn,
                                         void* userdata);
int openpete_mods_prepare(const openpete_options_t* opts,
                          openpete_mods_progress_fn progress,
                          openpete_mods_run_task_fn run_task);

/* --compile-mods utility mode (build-time gate): compile EVERY mod with
 * src/ in the resolved mods dir — enabled.txt deliberately ignored, so a
 * disabled mod's breakage still surfaces — against <exe>/sdk, through
 * the same key/stamp/compile path boot uses (a warm cache stays warm; a
 * boot after a clean pass recompiles nothing). Returns the number of
 * failed compiles (0 = success), -1 on hard setup errors (no compiler /
 * no mods dir resolvable). The cmake `spyro1_mods` step runs this on
 * native presets; cross presets compile staged mods cmake-side instead
 * (games/spyro-1/CMakeLists.txt). */
int openpete_mods_compile_all(const openpete_options_t* opts);

/* Per-function-entry chain dispatch. Returns 1 = an override chain ran
 * (skip the recompiled body), 0 = no chain for addr (run the body).
 * Called from psx_override_dispatch_game behind the
 * openpete_mods_active fast gate. */
int openpete_mods_dispatch(CPUState* cpu, uint32_t addr);

/* 1 while at least one override chain is registered. Hot-path gate. */
extern int openpete_mods_active;

/* bk2 input-movie status (read-only v1 of the bk2 access service):
 * run.c publishes once per sim frame while a movie is loaded; mods
 * query via api->bk2_status to synchronize with replays. Record/replay
 * CONTROL stays engine-owned (mid-session anchoring is TAS infra). */
typedef struct {
    int active;      /* a movie is loaded this session               */
    int frame;       /* current movie frame (<0 before the offset)   */
    int frame_count; /* total frames in the movie                    */
    int overriding;  /* this tick's input actually came from it      */
} openpete_mods_bk2_status_t;
void openpete_mods_bk2_publish(const openpete_mods_bk2_status_t* st);
void openpete_mods_bk2_query(openpete_mods_bk2_status_t* out);

/* Guest scratch arena (core/mods/mod_arena.c): session-lifetime bump
 * allocator in the >2MB expansion window, registered as a psx_runtime
 * region — guest-addressable through the standard accessors, host-
 * addressable via *host_view. Returns the guest vaddr, 0 on failure.
 * NOT in the engine savestate snapshot; no free. */
uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   void** host_view, const char* owner);

/* Low park window (mod_arena.c): canonical-RAM bump allocator below the
 * game's EXE image, for payloads with narrow address fields (21-bit
 * Spyro anim vertex offsets). The GAME registers the window at boot;
 * allocation fails loudly before that. */
void     openpete_mods_arena_low_init(uint32_t base, uint32_t size);
uint32_t openpete_mods_guest_alloc_low(uint32_t size, uint32_t align,
                                       void** host_view,
                                       const char* owner);

/* One-shot mod sound (core/host/mod_sound.c): host-side additive OpenAL
 * playback of a PCM WAV at `path`, gain-clamped [0,4]. Sim-neutral by
 * construction; legal from tick and present hooks. 0 ok / -1 failed
 * (logged; never fatal). */
int openpete_mod_sound_play(const char* path, float gain);

/* Live modset toggle (P2): queue an enable/disable; it applies at the
 * NEXT tick boundary by flipping owner-keyed disabled flags across all
 * registration surfaces (nothing is torn down — re-enable restores the
 * mod exactly where it was). Asset overlays are boot-scoped ("restart"
 * tier). openpete_mods_tick_boundary is the apply point, called at the
 * canonical present commit with the running canonical-tick counter;
 * OPENPETE_MODS_TOGGLE="tick:id:0|1[,...]" schedules for CI/dev. */
/* Loaded-mod enumeration (the mod menu's data source). */
int         openpete_mods_count(void);
const char* openpete_mods_id(int i);
int         openpete_mods_is_enabled(const char* id);

int  openpete_mods_set_mod_enabled(const char* id, int on);
void openpete_mods_tick_boundary(unsigned canonical_tick);
void openpete_mods_site_set_enabled(const char* mod_id, int on);
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on);

/* Per-mod time instrument (mod_chain.c): SELF-time accumulation per
 * owner (tick hooks fed by the chain runner, present hooks by
 * mod_draw.c) + the periodic report that names any mod exceeding the
 * sim-tick budget. ticks_elapsed = sim ticks in the window. */
void openpete_mods_time_add(const char* owner, int64_t ns, int is_present);
void openpete_mods_time_report(unsigned ticks_elapsed);

/* Crash attribution: non-NULL = an override chain frame is live on this
 * thread; returns the innermost mod id (addr_out = the overridden
 * vaddr). Consumed by core/runtime/crash.c so a crash report names the
 * offending mod (PoC rung 6). */
const char* openpete_mods_crash_attribution(uint32_t* addr_out);

/* Per-modset save safety: canonical tag of the loaded gameplay-relevant
 * modset ("id@ver+id@ver", ids sorted; mods declaring sim_neutral=true
 * excluded). "" = vanilla. Stable after openpete_mods_init(); consumed
 * by core/host/memcard.c to tag cards + warn on mismatch. */
const char* openpete_mods_modset_tag(void);

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

/* Level-slot namespace (rung 12). A registered slot is a NEW level id
 * whose WAD TOC entry is cloned from a donor level; the game ops own
 * the id→behavior aliasing so the slot loads and plays like the donor
 * while keeping its own per-level state index. v1 capacity: the guest
 * tables have exactly ONE free in-bounds slot (index 35 / id 65 in
 * Spyro 1); lifts beyond that ride openpete-ram-virtualization. */
typedef struct {
    /* Claim the free slot for donor_level_id: seed its TOC entry and
     * install the alias hooks. Returns the claimed level id, 0 = not
     * ready yet (boot TOC not loaded) or no free slot. */
    int (*claim_slot)(CPUState* cpu, int donor_level_id);
    /* Trigger a transition into level_id from live gameplay (the
     * level-select cheat's writes). 0 = queued, -1 = not in gameplay. */
    int (*enter_level)(CPUState* cpu, int level_id);
} openpete_level_ops_t;

void openpete_mods_set_level_ops(const openpete_level_ops_t* ops);

int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag);
int openpete_mods_level_slot_id(const char* mod_id, const char* name);
int openpete_mods_level_enter(CPUState* cpu, int level_id);

/* Level-ready event (mod_registry.c): hooks fire once per level on the
 * first master-update tick with gameplay live; also drives
 * openpete_mods_sfx_defs_apply. Loader-facing registration. */
int openpete_mods_on_level(const char* mod_id, openpete_mod_level_fn fn,
                           const char* log_tag);
int openpete_mods_registry_ensure_driver(const char* log_tag);

/* Loader-facing (wired into the mod api). */
int openpete_mods_moby_register(const char* mod_id, const char* name,
                                int donor_class,
                                openpete_moby_update_fn update_fn,
                                const char* log_tag);
int openpete_mods_moby_type_id(const char* mod_id, const char* name);
uint32_t openpete_mods_moby_spawn(CPUState* cpu, const char* mod_id,
                                  const char* name);

/* ── SFX overlay service (core/mods/mod_sfx.c) ─────────────────────
 * Host-side sound substitution at voice key-on. The game's SPU thunk
 * layer registers a resolver (sample SPU addr -> stable sound name) and
 * an uploader (stage a replacement ADPCM body into free SPU tail space,
 * returns its SPU addr or 0), then consults on_key with each keyed
 * voice's stock addr+pitch and applies the returned substitution to a
 * HOST COPY of the voice attrs only — guest state stays byte-stock. */
typedef const char* (*openpete_mods_sfx_resolver_fn)(uint32_t spu_addr);
typedef uint32_t (*openpete_mods_sfx_upload_fn)(const void* body,
                                                uint32_t len);
void openpete_mods_sfx_set_fns(openpete_mods_sfx_resolver_fn resolve,
                               openpete_mods_sfx_upload_fn upload);

/* Returns 1 if this play is substituted (out_addr/out_pitch filled;
 * out_suppress=1 => mute the play), 0 = leave stock. */
int openpete_mods_sfx_on_key(uint32_t spu_addr, uint16_t stock_pitch,
                             uint32_t* out_addr, uint16_t* out_pitch,
                             int* out_suppress);

/* New SPU bank uploaded (level load): staged replacement addresses are
 * invalid — forget them; they re-stage lazily at next key-on. */
void openpete_mods_sfx_bank_reset(void);

/* Mod identity (mod_loader.c): stable id string, NULL-safe. */
const char* openpete_mods_id_of(openpete_mod_t* self);
/* Mod directory + the staged SDK directory (mod_loader.c), NULL-safe. */
const char* openpete_mods_dir_of(openpete_mod_t* self);
const char* openpete_mods_sdk_dir(void);

/* ── Mod shaders (mod_shader.c) ───────────────────────────────────────
 * GLSL compile service (tier S1 of plans/openpete-mod-shaders.md) +
 * refusing stubs for the later tiers (slots exist from api v2). */
int openpete_mods_shader_register(openpete_mod_t* self, const char* path);
/* SPIR-V words for a live handle (NULL = bad/swept handle). stage is a
 * glslang_stage_t value (vertex/fragment). */
const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage);
void openpete_mods_shader_remove_owner(const char* mod_id);
/* Hot-reload watch: content hash of <mod>/shaders/ ("-" if absent). */
int openpete_mods_shader_watch_key(const char* mod_dir, char key_out[65]);
int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order);
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled);
int openpete_mods_postfx_set_uniforms(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size);

/* Renderer-facing postfx chain (native_render.c executor). Uniform
 * blocks cap at 1 KiB (fits every portable UBO limit with room for the
 * frame ring). pipeline_slot caches the executor's rhi_pipeline* inside
 * the registration; a hot-reload sweep moves orphaned pipelines to the
 * retirement list for the renderer to destroy safely. */
#define OPENPETE_POSTFX_UNIFORMS_MAX 1024u
/* pipeline_slot sentinel: build failed, don't retry or destroy. */
#define OPENPETE_POSTFX_PIPE_FAILED ((void*)(uintptr_t)1)
typedef struct {
    const uint32_t* frag_spv;
    size_t          frag_words;
    const void*     uniforms;
    uint32_t        uniforms_len;
    void**          pipeline_slot;
    const char*     owner;
} openpete_postfx_pass_t;
int  openpete_mods_postfx_collect(int point, openpete_postfx_pass_t* out,
                                  int cap);
int  openpete_mods_postfx_take_retired(void** out, int cap);
void openpete_mods_postfx_remove_owner(const char* mod_id);
void openpete_mods_postfx_retire_all(void);   /* shutdown: cache every pipeline */

/* Custom-pipeline draw queue (S7) — renderer-facing drain. */
typedef struct {
    const uint32_t* vert_spv; size_t vert_words;
    const uint32_t* frag_spv; size_t frag_words;
    int blend, depth_test, depth_write;   /* OP_BLEND_* + flags */
    void** pipeline_slot;
    const char* owner;
} openpete_mod_pipe_gpu_t;
int  openpete_mods_drawmesh_count(void);
int  openpete_mods_drawmesh_get(int i, openpete_mod_pipe_gpu_t* pd,
                                const openpete_mod_vertex_t** verts, int* n,
                                uint16_t* tex);
void openpete_mods_drawmesh_clear(void);
void openpete_mods_pipeline_remove_owner(const char* mod_id);
void openpete_mods_pipeline_retire_all(void);

/* Material selectors (S8a/S8b) — draw-time resolve for the batcher.
 * NULL spv side = keep the stock stage (executor falls back to the
 * embedded psx_scene shaders). uniform_slot > 0 = a per-instance
 * override (S8b): bind material-UBO block (1 + uniform_slot) as the mod
 * block; 0 = declarative whole-class (engine block only). */
typedef struct {
    const uint32_t* vert_spv; size_t vert_words;
    const uint32_t* frag_spv; size_t frag_words;
    void** pipeline_slot;
    const char* owner;
    int   uniform_slot;
} openpete_mod_material_gpu_t;
int  openpete_mods_material_resolve(uint32_t tag,
                                    openpete_mod_material_gpu_t* out);
void openpete_mods_material_remove_owner(const char* mod_id);
void openpete_mods_material_retire_all(void);

/* Per-instance material callback (S8b). Cap on a per-draw uniform
 * block (fits the material UBO block; matches the postfx cap). */
#define OPENPETE_MATERIAL_UNIFORMS_MAX 256u
/* True when any registered selector carries a per-instance callback —
 * the moby walk skips the resolve entirely otherwise (single bool). */
int  openpete_mods_material_has_instance(void);
/* Resolve ONE moby instance at extraction (moby walk, cpu available):
 * returns the batch-split slot stamped into the provenance tag — 0 =
 * defer to draw-time class resolve (declarative / no per-instance
 * winner), OPENPETE_MATERIAL_SLOT_STOCK = the callback declined (force
 * stock), else an interned per-instance slot (1..). */
#define OPENPETE_MATERIAL_SLOT_STOCK 0xFFF
int  openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                      uint32_t moby_vaddr);
/* Per-present interned-slot table lifecycle + readout for the material
 * UBO (native_render): reset at scene-begin, then the extractor interns
 * during the walk, then native_render writes each slot's bytes into the
 * UBO before the scene draw. */
void openpete_mods_material_frame_reset(void);
int  openpete_mods_material_slot_count(void);   /* 1 + interned slots */
const void* openpete_mods_material_slot_uniforms(int slot, uint32_t* len);
/* Enabled-list position of a mod (0-based load order; -1 unknown). */
int  openpete_mods_load_index_of(openpete_mod_t* self);
/* Last present-flush ctx (mod_draw.c) — the postfx engine UBO's
 * tick/alpha/canonical source, identical to the present-hook ctx. */
void openpete_mods_present_ctx(uint32_t* tick, float* alpha, int* canonical);
int openpete_mods_pipeline_register(openpete_mod_t* self,
                                    const openpete_mod_pipeline_desc_t* desc);
int openpete_mods_draw_mesh(openpete_mod_t* self, int pipeline,
                            const void* verts, int vert_count,
                            uint16_t texture_id);
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance);

/* Loader-facing (wired into the mod api). */
int openpete_mods_sfx_replace(openpete_mod_t* self, const char* name,
                              const void* body, uint32_t len,
                              uint16_t pitch);
int openpete_mods_sfx_hook(openpete_mod_t* self, const char* name,
                           void (*fn)(openpete_sfx_event_t* ev));
uint32_t openpete_mods_sfx_stage(openpete_mod_t* self, const void* body,
                                 uint32_t len);

/* SoundDefinition replacement (mod_sfx.c): the game registers a
 * def-repoint op (resolve (model class, m_Sounds slot) to the live
 * per-level def and rewrite its sample address; return 0 ok, nonzero =
 * class not present). Rows apply once per level from the registry's
 * level-ready point. */
typedef int (*openpete_mods_sfx_def_repoint_fn)(CPUState* cpu,
                                                uint16_t model_class,
                                                uint8_t sound_slot,
                                                uint32_t spu_addr);
void openpete_mods_sfx_set_def_repoint(openpete_mods_sfx_def_repoint_fn fn);
int  openpete_mods_sfx_def_replace(openpete_mod_t* self,
                                   uint16_t model_class,
                                   uint8_t sound_slot,
                                   const void* body, uint32_t len);
void openpete_mods_sfx_defs_apply(CPUState* cpu, int level_id);

/* Per-mod user configuration (mod_config.c): mods/<id>/config.toml,
 * parsed by the loader right before the mod's entry runs (boot and hot
 * reload). Keys are dotted paths ("chests.collision"); getters return
 * `def` for an absent file/key/type-mismatch. The loader watches the
 * file's content hash in dev hot-reload mode, so a config edit re-enters
 * the mod with fresh values. */
void openpete_mods_config_load(openpete_mod_t* mod, const char* mod_dir,
                               const char* log_tag);
int     openpete_mods_config_bool(openpete_mod_t* self, const char* key,
                                  int def);
int64_t openpete_mods_config_int(openpete_mod_t* self, const char* key,
                                 int64_t def);
double  openpete_mods_config_float(openpete_mod_t* self, const char* key,
                                   double def);
int     openpete_mods_config_str(openpete_mod_t* self, const char* key,
                                 const char* def, char* buf, int cap);

/* Mod-owned RGBA8 textures (mod_texture.c): registered under a
 * mod-chosen 16-bit id; model faces select them with the PSX-reserved
 * Tiledef tpage MODE 3 encoding + the id in the CLUT u16; the native
 * renderer resolves via _get and binds through the HD pipe. Host-side
 * only (guest RAM / VRAM / oracle untouched). */
int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h);
const unsigned char* openpete_mods_texture_get(uint16_t id, int* w, int* h);

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
