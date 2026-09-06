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

/* ── Parked former-ABI types ─────────────────────────────────────────
 * These left the mod-facing header at the api-v1 cut but their ENGINE
 * machinery survives (actor render, custom pipelines, materials,
 * SoundDefinition plumbing, level-ready driving, state blobs). They
 * live here, engine-internal, until their surfaces re-land as
 * append-only pillars. */

typedef void (*openpete_mod_level_fn)(struct CPUState* cpu, int level_id);

/* Engine-side spelling of openpete_mod_refine_fn (openpete_mod_api.h,
 * included further down — the struct is forward-declared so the two
 * typedefs stay compatible without reordering this header). No
 * CPUState: refine runs in tick context and reads guest RAM through the
 * mod api's own accessors, which keeps an engine-internal type out of
 * the public ABI. */
struct openpete_mod_material_key;
typedef int (*openpete_mod_material_fn)(
        const struct openpete_mod_material_key* key,
        void* uniform_out, uint32_t cap);
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
 * Resolution order for the enabled SET within that directory:
 * opts->mods_enabled (--mods) → env OPENPETE_MODS → enabled.txt → all
 * discovered mods lexicographically; in every case the order IS the
 * priority (later = higher).
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
 * addressable via *host_view. flags = OP_GALLOC_*. Every allocation is ledgered by
 * (owner, per-owner call index); the ledger is what the carry gate,
 * re-entry replay and the savestate verdict all consume. Returns the
 * guest vaddr, 0 on failure (exhausted, ledger full, bad args). */
#define OPENPETE_ARENA_OWNER_MAX 64 /* == the loader's MOD_ID_MAX */
uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   uint32_t flags, void** host_view,
                                   const char* owner);

/* Reset `owner`'s allocation call cursor: the loader calls this
 * immediately before running (or re-running) openpete_mod_entry, so the
 * mod's allocation sequence replays from its first ledger row. */
void openpete_mods_arena_entry_begin(const char* owner);

/* Snapshot support: the arena's used prefix is timeline state. save
 * copies [0, used); restore is a RE-ANCHOR — bytes allocated after the
 * snapshot keep their contents and the cursor never moves backwards.
 * immutable_check verifies every IMMUTABLE row's checksum (captured on
 * first check) and warns once per broken promise; call it once per
 * keyframe / file save, never per speculative tick. */
uint32_t       openpete_mods_arena_used(void);
const uint8_t* openpete_mods_arena_bytes(void);
void           openpete_mods_arena_mem_restore(const void* bytes,
                                               uint32_t used);
void           openpete_mods_arena_immutable_check(const char* when);

/* Stream handle (mod_arena.c): the index a mod writes into a frame
 * word's 21-bit field in place of a stream vaddr that will not fit. Same
 * vaddr -> same handle; every call is a ledger row (replayed on re-entry,
 * verified on load). 0 = refused (vaddr 0, kernel area, table or ledger
 * full). */
uint32_t openpete_mods_vstream_handle(uint32_t vaddr, const char* owner);

/* One-shot mod sound (core/host/mod_sound.c): host-side additive OpenAL
 * playback of a PCM WAV at `path`, gain-clamped [0,4]. Sim-neutral by
 * construction; legal from tick and present hooks. 0 ok / -1 failed
 * (logged; never fatal). */
int openpete_mod_sound_play(const char* path, float gain);

/* Same, for audio a caller already holds in memory — interleaved s16 PCM,
 * 1 or 2 channels. `key` names the sound for the buffer cache (a stable
 * label, not a path); the first call for a key uploads, later calls reuse
 * the AL buffer and re-read nothing. Lets a caller synthesize or decode a
 * sound without staging a WAV on disk. 0 ok / -1 failed. */
int openpete_mod_sound_play_pcm(const char* key, const int16_t* pcm,
                                uint32_t frame_count, int channels,
                                uint32_t sample_rate, float gain);

/* Engine mute, pushed down by openpete_set_mute(). These sources live on
 * PsyCross's AL context but outside its SPU mixer, so the SPU's own mute
 * flag does not reach them — without this they keep playing under --mute.
 * Muting also stops whatever is mid-flight, matching the music override. */
void openpete_mod_sound_set_mute(int on);

/* Live modset toggle (P2): queue an enable/disable; it applies at the
 * NEXT tick boundary by flipping owner-keyed disabled flags across all
 * registration surfaces (nothing is torn down — re-enable restores the
 * mod exactly where it was). Asset overlays follow the toggle at their
 * consumers' own boundaries: textures at the next resolve, music at the
 * next track start, level data at the next level load.
 * openpete_mods_tick_boundary is the apply point, called at the
 * canonical present commit with the running canonical-tick counter;
 * OPENPETE_MODS_TOGGLE="tick:id:0|1[,...]" schedules for CI/dev. */
/* Loaded-mod enumeration (the mod menu's data source). */
int         openpete_mods_count(void);
const char* openpete_mods_id(int i);
int         openpete_mods_is_enabled(const char* id);

/* Per-mod row for the dev overlay's Mods panel. Strings are loader-
 * owned and live for the session. */
typedef struct {
    const char* id;
    const char* name;    /* manifest display name (falls back to id) */
    const char* version;
    int enabled;         /* live-toggle state */
    int entered;         /* openpete_mod_entry has run (deferred-entry
                          * load-all: boot-disabled mods enter on first
                          * enable) */
    int sim_mutative;    /* effective verdict: does the mod write guest
                          * RAM? Declared, or derived by the engine. */
    int has_ui;          /* a UI section (register_ui / annex descriptor)
                          * or a live ui_status block */
    int ui_always;       /* the section carries OPENPETE_MOD_UI_ALWAYS: its
                          * body draws from the overlay's top-level pass on
                          * every present, not inside this mod's node */
    int has_assets;      /* assets/ overlay root (boot-scoped tier) */
    int has_src;
    int has_cfg;         /* [[config]] schema rows in the manifest */
    uint64_t imgui_calls; /* ImGui annex calls the section's last body
                           * made; 0 = the mod does not use the annex,
                           * or its section has not drawn yet */
} openpete_mods_menu_info_t;
int openpete_mods_menu_info(int i, openpete_mods_menu_info_t* out);

/* ── Config schema ([[config]] rows in mod.toml) ─────────────────────
 * The author declares what is configurable; the ENGINE renders each
 * mod's settings section in the Mods panel from these rows. Values
 * live in the user's config.toml (written back with a comment-
 * preserving surgical edit) and reach the mod through the config_*
 * getters when its entry re-runs after the queued reload. */
typedef struct {
    const char* key;    /* dotted config path ("gems.sfx") */
    char        type;   /* 'b' bool, 'i' int, 'd' float, 's' string */
    const char* label;  /* row label (falls back to key) */
    const char* group;  /* section header within the mod's block, "" = none */
    const char* help;   /* tooltip text, "" = none */
    double      min, max;   /* numeric range; min >= max = free input */
    const char* const* items; /* string type: combo choices (else NULL) */
    int         n_items;
    /* Current effective value: config.toml if present, else the schema
     * default. sval is loader-owned and valid until the next row call. */
    int64_t     ival;   /* bool/int */
    double      fval;   /* float */
    const char* sval;   /* string */
} openpete_mods_cfg_row_t;

typedef struct {
    const char* key;
    char        type;   /* same encoding as the row */
    int64_t     ival;
    double      fval;
    const char* sval;
} openpete_mods_cfg_edit_t;

int openpete_mods_cfg_row_count(int mod_index);
int openpete_mods_cfg_row(int mod_index, int row,
                          openpete_mods_cfg_row_t* out);
/* Write the edits into the mod's config.toml and queue a reload at the
 * next tick boundary (entry re-reads the fresh values through
 * config_*). 0 = written; the reload itself is asynchronous. */
int openpete_mods_cfg_apply(int mod_index, int n,
                            const openpete_mods_cfg_edit_t* edits);

/* Run mod i's registered UI section with the overlay's widget vtable.
 * Sets the render-purity guard (openpete_mods_in_present) around the
 * callback. No-op for mods without a registration / disabled mods.
 *
 * Draws the mod's ui_status block, then its body — unless the section is
 * flagged OPENPETE_MOD_UI_ALWAYS, whose body belongs to the always pass
 * below and would otherwise run twice per present. */
struct openpete_mod_ui;
void openpete_mods_ui_draw(int i, const struct openpete_mod_ui* ui);

/* The always pass: run mod i's body if, and only if, its section is
 * flagged OPENPETE_MOD_UI_ALWAYS. No ui_status lines — those stay in the
 * mod's node of the Mods panel. @p no_input raises the guard bit that puts
 * ImGuiWindowFlags_NoInputs on every window the body opens, which the
 * caller sets while the dev overlay is closed and the game owns the mouse. */
void openpete_mods_ui_draw_always(int i, const struct openpete_mod_ui* ui,
                                  int no_input);

/* Enabled mods with an always-flagged body, counted by one scan of the mod
 * table. The overlay asks per present whether an ImGui frame is wanted at
 * all; a count derived here cannot drift out of step with the loader's own
 * enable flips and registration strips the way a cross-thread counter
 * would. */
int openpete_mods_ui_always_count(void);

/* Drop a mod's UI section for the rest of the session, with one log line
 * naming @p why. The mod keeps running; only its section stops drawing.
 * The overlay calls this when a body misbehaves often enough that leaving
 * it in the panel costs more than losing it. */
int openpete_mods_ui_refuse_section(int i, const char* why);

int  openpete_mods_set_mod_enabled(const char* id, int on);
/* cpu: live game CPU — applied toggles notify the mod's toggle hook in
 * tick context (guest access legal). */
void openpete_mods_tick_boundary(CPUState* cpu, unsigned canonical_tick);
/* 1 while disabling this owner would corrupt the session: it holds a
 * claimed level slot and the player is inside (or entering) that level.
 * The apply loop DEFERS such a disable until the player leaves; the
 * menu greys the switch. */
int  openpete_mods_registry_owner_busy(const char* mod_id);
/* Bumped once per APPLIED toggle (state actually changed). The run loop
 * polls it to invalidate timeline-derived state: rewind history recorded
 * under the old modset must not replay into the new one, and runahead
 * must re-run its capability gate against the new enabled set. */
unsigned openpete_mods_toggle_epoch(void);
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on);

/* Owner teardown and live enable/disable for the three subsystems that
 * key their entries on a mod id. Declared here beside their registry
 * sibling: mod_loader.c calls all of them, and without a prototype in
 * scope C assumes int(), which silently defeats argument checking on
 * calls that pass a pointer. */
void openpete_mods_chain_remove_owner(const char* owner_id);
void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on);
void openpete_mods_draw_remove_owner(const char* owner_id);
void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on);
void openpete_mods_registry_remove_owner(const char* mod_id);

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
 * modset ("id@ver+id@ver", ids sorted; mods that do not write guest
 * RAM excluded). "" = vanilla. Stable after openpete_mods_init(); consumed
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
int  openpete_mods_capture_actor_pose(int kind, void* buf, int cap);
int  openpete_mods_draw_actor(int kind, const void* pose, int len,
                              uint32_t tint_rgba);
void openpete_mods_suppress_player_draw(void);
int  openpete_mods_register_present_hook(openpete_mod_present_fn fn,
                                         const char* owner_id);

/* 1 while a present hook is executing on this thread — the loader's
 * guest/call/override entry points refuse during it (render-purity). */
#ifndef OPENPETE_THREAD_LOCAL
#  ifdef __cplusplus
#    define OPENPETE_THREAD_LOCAL thread_local
#  else
#    define OPENPETE_THREAD_LOCAL _Thread_local
#  endif
#endif
extern OPENPETE_THREAD_LOCAL int openpete_mods_in_present;

/* ── Content registries (core/mods/mod_registry.c) ─────────────────────
 *
 * Namespaced additive content (`modid:entry`): LEVEL SLOTS and the custom
 * levels served over them. Lifts beyond the vanilla table sizes ride
 * openpete-ram-virtualization — acknowledged, not hidden.
 *
 * The registries share one per-tick driver: a chain link on the game's
 * master-update function, added lazily on first registration (nothing
 * registered = no hook = inert). */

/* Game ops the registry delegates to (game addresses stay game-side;
 * Spyro registers spyro1_moby_ops from main.cpp). */
typedef struct {
    uint32_t update_hook_addr; /* master per-tick update fn (chain anchor) */
    /* Current level id, 0 = none/not in gameplay yet. */
    int (*level_id)(CPUState* cpu);
    /* Fill `out` with the moby classes the CURRENT LEVEL has loaded — the
     * classes that can appear at all — ascending, and return the count.
     * -1 while the table cannot be trusted (mid-load). Level-scoped, so the
     * answer is stable as the camera moves; it is a superset of the classes
     * with a live instance right now. */
    int (*classes_loaded)(CPUState* cpu, uint16_t* out, int cap);
} openpete_moby_ops_t;

void openpete_mods_set_moby_ops(const openpete_moby_ops_t* ops);

/* Host-side snapshot of the loaded-class set, refreshed once per tick from
 * the ops above. Mods read it through the api's moby_classes, which must be
 * callable from a UI section — i.e. inside a present, where guest access is
 * refused — so the read never touches guest RAM. */
#define OPENPETE_MOBY_CLASSES_MAX 512
void openpete_mods_moby_class_census(CPUState* cpu);
int  openpete_mods_moby_classes(uint16_t* out, int cap);

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
    /* Re-target an already-claimed slot at a new donor: re-copy the
     * donor's TOC entry into the slot and swap the alias donor. 0 ok. */
    int (*retarget_slot)(CPUState* cpu, int donor_level_id);
    /* Declare `len` as a level entry's Data length in the guest WAD TOC
     * (len 0 = restore the stock length). 0 ok, -1 = TOC not live. */
    int (*declare_data_len)(CPUState* cpu, int level_id, uint32_t len);
    /* lower_snake level name (or decimal id string) -> id, -1 unknown. */
    int (*resolve_name)(const char* name);
    /* Trigger a transition into level_id from live gameplay (the
     * level-select cheat's writes). 0 = queued, -1 = not in gameplay. */
    int (*enter_level)(CPUState* cpu, int level_id);
    /* Read a level's raw Data WAD entry host-side from the user's disc.
     * buf=NULL probes length. Returns byte length or -1. */
    int (*read_level_data)(int level_id, void* buf, uint32_t cap);
    /* Show `name` as level_id's in-game title (guest name table:
     * inventory, entry title card, portal name arc). NULL restores the
     * stock name. 0 ok. */
    int (*set_level_name)(CPUState* cpu, int level_id, const char* name);
    /* Per-tick driver: while `on`, the donor level's homeworld portal
     * renders the custom level's own skybox (parsed from
     * <dir>/levels/<stem>.WAD, parked in the guest arena);
     * restores the baked preview when `on` drops or the level changes.
     * Idempotent, cheap when nothing changes. */
    int (*replace_sky_tick)(CPUState* cpu, int donor_id, const char* dir,
                            const char* stem, int on);
} openpete_level_ops_t;

void openpete_mods_set_level_ops(const openpete_level_ops_t* ops);

/* ── Declarative custom levels ([[level]] manifest blocks) ───────────
 * A mod ships assets/levels/<stem>.WAD (+ optional .OVL) and a
 * manifest block; the registry owns slot time-sharing, guest TOC
 * declarations and warps — no mod code involved. Rows sort by NAME
 * (the menu's alphabetical Custom World buckets). */
int  openpete_mods_custom_register(const char* mod_id, const char* name,
                                   const char* stem, const char* dir,
                                   const char* donor, int replace,
                                   const char* music, const char* title,
                                   const char* log_tag);
typedef struct {
    const char* name;   /* human name (sort key) */
    const char* owner;  /* mod id */
    int donor_id;
    int replace_mode;   /* 1 = serve over the donor's own entry */
    int active;         /* currently owns the shared slot */
    int level_id;       /* enterable id right now (slot id or donor), 0 = none */
    int files_ok;       /* data file present at registration */
} openpete_mods_custom_info_t;
int  openpete_mods_custom_count(void);
int  openpete_mods_custom_info(int i, openpete_mods_custom_info_t* out);
/* Queue a warp into row i (applies in the registry's tick driver). */
int  openpete_mods_custom_warp(int i);
/* Flip a row between slot (0) and replace (1) serving. */
void openpete_mods_custom_set_mode(int i, int replace);
/* Overlay hook: the files serving logical level `level_id` right now —
 * the active slot row, or an enabled declared replace row. 1 = found. */
int  openpete_mods_custom_files_for(int level_id, const char** dir_out,
                                    const char** stem_out);
/* Path of a custom level's data (<stem>.WAD) or code-overlay (<stem>.OVL)
 * file under <dir>/levels/. The extension is matched case-insensitively.
 * 0 = found; -1 = absent, with the canonical path still written to `out`
 * so callers can name the missing file. */
int  openpete_mods_custom_asset_path(const char* dir, const char* stem,
                                     int is_ovl, char* out, size_t cap);
/* Music hook: the custom song for logical level `level_id` right now
 * (same row selection as files_for). Returns the WAV path (and its
 * duration via dur_sec_out, may be NULL) or NULL when the level has no
 * custom song / the file was missing at registration. */
const char* openpete_mods_custom_music_for(int level_id,
                                           double* dur_sec_out);

int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag);
int openpete_mods_level_slot_id(const char* mod_id, const char* name);
int openpete_mods_level_enter(CPUState* cpu, int level_id);
int openpete_mods_read_level_data(int level_id, void* buf, uint32_t cap);

/* Level-ready event (mod_registry.c): hooks fire once per level on the
 * first master-update tick with gameplay live. Loader-facing
 * registration. */
int openpete_mods_on_level(const char* mod_id, openpete_mod_level_fn fn,
                           const char* log_tag);
int openpete_mods_registry_ensure_driver(const char* log_tag);

/* ── SFX replacement verb (core/mods/mod_sfx.c) ────────────────────
 * sfx_replace registers a symbolic sfx-pack entry from memory; the
 * substitution itself is the pack's (core/host/sfx_pack.h), below
 * everything the guest observes. */
/* Name canonicaliser: the game maps a mod-facing alias ("gem_pickup")
 * to the SoundTable slot name the pack's resolver reports ("gemPickup");
 * NULL or identity when no alias exists. Applied to sfx_replace names
 * before they are stored. */
typedef const char* (*openpete_mods_sfx_canon_fn)(const char* name);
void openpete_mods_sfx_set_canon(openpete_mods_sfx_canon_fn fn);

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
/* The same material fragment compiled for the RETAINED TERRAIN pipelines,
 * whose engine half of the contract reads flat per-face varyings instead of
 * a per-draw push block (OP_PROFILE_TERRAIN in openpete_psx_material.glsl).
 * NULL when the shader has no terrain profile — not a material fragment, or
 * its terrain compile failed; terrain then draws stock. */
const uint32_t* openpete_mods_shader_spv_terrain(int handle, size_t* words);
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

/* Registry read side for a compose: held for the whole compose so the
 * shader/postfx/material tables cannot change under it (their mutators
 * take the write side). */
void openpete_mods_registry_read_lock(void);
void openpete_mods_registry_read_unlock(void);

/* Draw-state variants a material can be built for. Index 5 is the DECAL
 * profile; see openpete_psx_decal.glsl for why that is a separate shading
 * contract rather than another blend class. */
#define OPENPETE_MATERIAL_PIPE_VARIANTS 6
#define OPENPETE_MATERIAL_PIPE_DECAL    5

/* Material selectors (S8a/S8b) — draw-time resolve for the batcher.
 * NULL spv side = keep the stock stage (executor falls back to the
 * embedded psx_scene shaders). uniform_slot > 0 = a per-instance
 * override (S8b): bind material-UBO block (1 + uniform_slot) as the mod
 * block; 0 = declarative whole-class (engine block only). */
typedef struct {
    const uint32_t* vert_spv; size_t vert_words;
    const uint32_t* frag_spv; size_t frag_words;
    /* One cached pipeline per BLEND CLASS (0 opaque, 1 average, 2 additive,
     * 3 subtract): a material reshades within whatever group it lands in and
     * inherits that group's compositing, so one shader needs one pipeline per
     * blend it is drawn under. */
    void** pipeline_slot[OPENPETE_MATERIAL_PIPE_VARIANTS];
        /* [0..3] PSX blend classes, [4] portal sky (opaque, no depth
         * write, stencil TEST against the aperture mask), [5] the decal
         * profile (openpete_psx_decal.glsl). */
    void** pipeline_mark_slot;  /* stencil-MARK twin (hole-fill groups) */
    const char* owner;
    int   uniform_slot;
} openpete_mod_material_gpu_t;
int  openpete_mods_material_resolve(uint32_t tag,
                                    openpete_mod_material_gpu_t* out);
void openpete_mods_material_remove_owner(const char* mod_id);
void openpete_mods_material_retire_all(void);

/* True when any registered selector carries a per-instance callback —
 * the moby walk skips the resolve entirely otherwise (single bool). */
int  openpete_mods_material_has_instance(void);
/* Resolve ONE moby instance at extraction (moby walk, cpu available):
 * returns the batch-split slot stamped into the provenance tag — 0 =
 * defer to draw-time class resolve (declarative / no per-instance
 * winner), OPENPETE_MATERIAL_SLOT_STOCK = the callback declined (force
 * stock), else an interned per-instance slot (1..). */
#define OPENPETE_MATERIAL_SLOT_STOCK 0xFFF
/* Terrain-untextured twin of SLOT_STOCK: the terrain tag's slot field is
 * 9 bits ([24:16]), so its "declined -> stock" sentinel is 0x1FF. */
#define OPENPETE_MATERIAL_TERRAIN_STOCK 0x1FF
int  openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                      uint32_t moby_vaddr);
/* Untextured-terrain twin (S8c): resolve the beyond-LOD gouraud band's
 * per-instance material once per extraction (callback gets moby_vaddr 0).
 * Returns the slot for OPENPETE_TAG_TERRAIN_UNTEX (0 = declarative,
 * OPENPETE_MATERIAL_TERRAIN_STOCK = declined). */
int  openpete_mods_material_terrain_untex_slot(CPUState* cpu);
/* Texture-keyed twin (S9): resolve the per-instance material for one textured
 * terrain face's Tiledef id. Same slot semantics as the untex resolve; the
 * feeder memoizes per (present, texid). */
int  openpete_mods_material_terrain_tex_slot(CPUState* cpu, int texid);
/* Monotonic epoch over everything a terrain selector scan reads: selector
 * registration/removal and the mod enabled set. The retained cache latches
 * its BAKE against it — a baked face carries its material's bucket, so a
 * selector set that moved makes the bake stale. */
unsigned openpete_mods_material_terrain_epoch(void);

/* ── Retained-terrain material buckets ───────────────────────────────
 * A retained face is baked once and redrawn for many presents without a
 * walk, so it cannot carry a per-present slot id. It carries a BUCKET
 * instead: the banked identity of the declarative selector that wins for
 * its face key, 0 = none. The bake stamps it; the vertex stage keeps only
 * the faces whose bucket matches the pass being drawn; the material epoch
 * invalidates the bake. Identity, never an outcome — a slot number would
 * be stale one present later. */
#define OPENPETE_MATERIAL_TER_VARIANTS 3  /* arena vertex stage: plain/anim/near */
/* Colour slot: world, world-MARK (hole-fill coverage), fill (LP backing),
 * then the three PSX blend classes. Depth and key passes shade nothing and
 * need no material pipeline. */
#define OPENPETE_MATERIAL_TER_SLOTS    6
#define OPENPETE_MATERIAL_TER_PIPES \
    (OPENPETE_MATERIAL_TER_VARIANTS * OPENPETE_MATERIAL_TER_SLOTS)
/* Bucket ids are stamped into 8 spare bits of the baked face material. */
#define OPENPETE_MATERIAL_TER_BUCKET_MAX 255

/* The winning bucket for a face key, or 0 for none. Same predicate and the
 * same priority as the draw-time resolve — a face must shade identically
 * whether it is walked or retained. */
int  openpete_mods_material_terrain_bucket(int texid, int env, int lp);
/* Highest live bucket id (0 = no terrain material installed), so the draw
 * loop knows how many material passes an arena can need. */
int  openpete_mods_material_terrain_bucket_max(void);

typedef struct {
    const uint32_t* frag_spv;    /* the OP_PROFILE_TERRAIN module          */
    size_t          frag_words;
    void**          pipeline_slot; /* cached rhi_pipeline* for this variant */
    const char*     owner;
    int             uniform_slot;  /* material-UBO block (0 = no params)    */
} openpete_mod_material_terrain_t;
/* Fill `out` for one bucket and one pipeline variant index (0 ..
 * OPENPETE_MATERIAL_TER_PIPES-1). Returns 0 on success, -1 when the bucket
 * is dead, disabled, or has no terrain profile — draw those faces stock. */
int  openpete_mods_material_terrain_bucket_gpu(
        int bucket, int pipe_idx, openpete_mod_material_terrain_t* out);
/* Per-present interned-slot table lifecycle + readout for the material
 * UBO (native_render): reset at scene-begin, then the extractor interns
 * during the walk, then native_render writes each slot's bytes into the
 * UBO before the scene draw. */
void openpete_mods_material_frame_reset(void);
/* Game -> mods: the level currently being rendered, for level-scoped
 * selector keys (sector and texture ids are per-level). Push it while
 * rendering; -1 until something does, which keeps level keys inert. */
void openpete_mods_set_level(int level_id);
int  openpete_mods_material_slot_count(void);   /* 1 + interned slots */
const void* openpete_mods_material_slot_uniforms(int slot, uint32_t* len);
typedef struct { uint32_t len; unsigned char bytes[256]; } openpete_mods_material_slot_snap_t;
int  openpete_mods_material_slots_snapshot(openpete_mods_material_slot_snap_t* out, int cap);
/* Enabled-list position of a mod (0-based load order; -1 unknown). */
int  openpete_mods_load_index_of(openpete_mod_t* self);
/* Last present-flush ctx (mod_draw.c) — the postfx engine UBO's
 * tick/alpha/canonical source, identical to the present-hook ctx. */
void openpete_mods_present_ctx(uint32_t* tick, float* alpha, int* canonical);
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance);
int openpete_mods_material_enable(openpete_mod_t* self, int material, int on);
int openpete_mods_material_set_params(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len);
int openpete_mods_material_set_selector(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel);
/* Installs selectors retuned from inside a present. Called at the tick
 * boundary, where a key change cannot split a frame in half. */
void openpete_mods_material_apply_pending(void);

/* Loader-facing (wired into the mod api). */
int openpete_mods_sfx_replace(openpete_mod_t* self, const char* name,
                              const void* body, uint32_t len,
                              uint16_t pitch);

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

/* Comment-preserving config.toml writer (the Mods panel's Apply): each
 * edit rewrites the value of `leaf = ...` inside its dotted-path
 * section, keeping every comment; missing keys/sections are appended.
 * Returns 0 when the file was rewritten. */
int openpete_mods_config_write(const char* mod_dir, const char* log_tag,
                               int n, const openpete_mods_cfg_edit_t* e);

/* Mod-owned RGBA8 textures (mod_texture.c): registered under a
 * mod-chosen 16-bit id; model faces select them with the PSX-reserved
 * Tiledef tpage MODE 3 encoding + the id in the CLUT u16; the native
 * renderer resolves via _get and binds through the HD pipe. Host-side
 * only (guest RAM / VRAM / oracle untouched). */
int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h);
const unsigned char* openpete_mods_texture_get(uint16_t id, int* w, int* h);
/* Scan an asset root's textures/own/ for <hexid>.png files and register
 * each under its filename id (the declarative own-texture road). Called
 * by the asset-root registrar; returns the count registered. */
int openpete_mods_texture_scan_root(const char* dir, const char* owner);

/* ── Engine services (core/mods/mod_services.c) ────────────────────── */

/* Queue a mod-requested savestate op (1=save, 2=load) for execution at
 * the host loop's quiescent point. One pending op at a time. */
int openpete_mods_savestate_request(int op, unsigned slot,
                                    const char* owner);

/* run.c: consume the pending op (0 = none; clears). Execute it exactly
 * like the user hotkeys (game fiber yielded, before pad refresh). */
int openpete_mods_pending_state_op(unsigned* out_slot);

/* Asset-overlay roots: one per enabled mod with an assets/ dir, in
 * enabled order (= priority; later wins). Consumers (texture pack,
 * music override) layer these over their base dirs. */
int         openpete_mods_register_asset_root(const char* dir,
                                              const char* owner);
int         openpete_mods_asset_root_count(void);
const char* openpete_mods_asset_root(int i, const char** owner);

/* Compile-on-boot content key for mod `id` (65-byte hex), NULL if unknown. */
const char* openpete_mods_build_key(const char* id);
/* 1 iff every enabled code mod declares a manifest runahead posture
 * (runahead = "pure" | "host-fx" — the arming gate); else 0 with
 * *offender = first blocker's id. */
int openpete_mods_runahead_capable(const char** offender);
/* Rewind gate: every enabled code mod declares a manifest state
 * posture. 0 = refuse timeline jumps; *offender names the first
 * undeclared mod. */
int openpete_mods_state_safe(const char** offender);

/* Arena state section (the ledger + the arena's used bytes),
 * serialized with the engine state. verdict() prefix-matches the live
 * ledgers against the saved ones pre-commit, superseded marks included
 * (-1 = refuse); apply() adopts saved rows the live session has not
 * made, restores the arena bytes and re-anchors the cursors to
 * max(live, saved) post-commit. */
uint32_t openpete_mods_arena_state_size(void);
void     openpete_mods_arena_state_save(void* dst);
int      openpete_mods_arena_state_verdict(const void* src, uint32_t size);
void     openpete_mods_arena_state_apply(const void* src, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MODS_H */
