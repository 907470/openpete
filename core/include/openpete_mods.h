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
 * machinery survives (custom pipelines, materials,
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

/** @brief Name the guest function that contains a guest address.
 *
 * Picks the registered symbol with the largest address <= addr by a
 * linear scan (the table is sorted by name); on an address tie a
 * non-legacy row wins, since legacy rows are raw-name aliases of the same
 * address. The table carries no sizes, so a symbol 0x10000 bytes or more
 * below addr (SYM_ADDR_SPAN_MAX in mod_loader.c) is rejected as an
 * unrelated function. Read-only over a static table, so the crash handler
 * (core/diag/crash.c) may call it.
 * @param addr Guest address; 0 returns NULL.
 * @param off_out Receives addr minus the symbol address on a hit; may be
 *        NULL.
 * @return The symbol name, or NULL when no table is registered or no
 *         symbol is near enough. */
const char* openpete_mods_symbol_for_addr(uint32_t addr, uint32_t* off_out);

/** @brief Discover, build, load and enter mods, once at boot.
 *
 * core/runtime/run.c calls it after options parsing. The mods root is
 * opts->mods_dir, else env OPENPETE_MODS_DIR, else the writable data
 * location openpete_paths_data_dir resolves for "mods" (next to the exe in
 * a portable install, the per-user data directory otherwise). The enabled
 * set is opts->mods_enabled (--mods), else env OPENPETE_MODS, else
 * enabled.txt, else every discovered mod in lexicographic order; the order
 * is the priority (later is higher). With enabled.txt present, no --mods
 * list and a windowed run, every other discovered mod also loads with its
 * entry deferred to its first enable. Manifests are parsed, topo-sorted by
 * hard deps, then built and loaded in that order; a failed load disables
 * its dependents. A missing or empty directory loads nothing, leaves
 * openpete_mods_active at 0 and keeps the sim path identical to a modless
 * build. A broken mod is disabled with a log line and never fails the
 * launch.
 * @param opts Parsed options.
 * @return The number of mods loaded, including those loaded disabled. */
int openpete_mods_init(const openpete_options_t* opts);

/** @brief Register every discovered mod's [[binding]] defaults with the keymap without loading any mod.
 *
 * Discovers mods the way openpete_mods_init does, parses each mod.toml,
 * registers each binding through openpete_keymap_set_mod_default (rank 0
 * under the mod.<id> domain) and records the mod's boot enabled state with
 * openpete_keymap_set_mod_enabled. A manifest that fails to parse is
 * skipped. The Spyro 1 main calls it for --keys-report, which prints the
 * keymap and exits before openpete_mods_init runs; init registers the same
 * rows itself.
 * @param opts Parsed options; opts->mods_dir overrides the mods root. */
void openpete_mods_register_key_defaults(const openpete_options_t* opts);

/* Callbacks openpete_mods_prepare takes: a progress sink, the compile task,
 * and a runner that executes the task (on a worker or inline). */
typedef void (*openpete_mods_progress_fn)(const char* stage, int done,
                                          int total);
typedef int (*openpete_mods_task_fn)(void* userdata);
typedef int (*openpete_mods_run_task_fn)(openpete_mods_task_fn fn,
                                         void* userdata);
/** @brief Compile every discovered source mod whose build key is stale, before openpete_mods_init.
 *
 * The game calls it while its splash is up, so the compiles show as a
 * progress stage. Returns 0 at once when no compiler resolves, no mod is
 * discovered, or the SDK is missing (a notice is logged when a source mod
 * exists). Pass 1 hashes each discovered mod's sources, skipping
 * assets-only and binary-only mods and mods whose cached build matches the
 * key; when nothing is stale neither callback runs. Otherwise the stale
 * set compiles inside one @p run_task call, reporting
 * progress("COMPILING MODS", done, total). A compile failure is logged and
 * left for openpete_mods_init to disable the mod. Init re-checks the cache
 * and compiles anything still stale inline, so whether prepare ran changes
 * only when the compile time is spent.
 * @param opts Parsed options; opts->mods_dir overrides the mods root.
 * @param progress Progress sink called from the task, or NULL.
 * @param run_task Runs the compile task, or NULL to run it inline.
 * @return The number of stale mods found. */
int openpete_mods_prepare(const openpete_options_t* opts,
                          openpete_mods_progress_fn progress,
                          openpete_mods_run_task_fn run_task);

/** @brief Compile every source mod in the resolved mods root, for the --compile-mods utility mode.
 *
 * Scans every candidate in the directory, ignoring enabled.txt so a
 * disabled mod's breakage still surfaces, and compiles against <exe>/sdk
 * through the same key, stamp and compile path boot uses: a mod whose
 * cached build matches its key is skipped, so a boot after a clean pass
 * recompiles nothing. Assets-only and binary-only mods are skipped. The
 * CMake target spyro1_mods runs it on native presets; cross presets
 * compile the staged mods in CMake instead (games/spyro-1/CMakeLists.txt).
 * @param opts Parsed options; opts->mods_dir overrides the mods root.
 * @return The number of failed compiles (0 on success), or -1 when no
 *         compiler resolves, no mods root resolves, or the SDK is
 *         missing. */
int openpete_mods_compile_all(const openpete_options_t* opts);

/** @brief Run the override chain registered for a guest function at its entry.
 *
 * Called from Spyro 1's psx_override_dispatch_game behind the
 * openpete_mods_active gate. Returns 0 without running anything when a
 * base() fallthrough armed a bypass for addr (the bypass is consumed and
 * the recompiled body runs), when no link on any chain is enabled, when
 * addr has no chain, or when every link on its chain is disabled.
 * Otherwise runs the first enabled link.
 * @param cpu the guest CPU.
 * @param addr the entered function's guest vaddr.
 * @return 1 when a chain ran (skip the recompiled body), 0 to run the
 *         body. */
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

/** @brief Reset an owner's ledger call cursor so its next calls replay from its first ledger row.
 *
 * The cursor is shared by openpete_mods_guest_alloc and
 * openpete_mods_vstream_handle calls. The loader calls this immediately
 * before running openpete_mod_entry at load and on hot reload, so a
 * matching call sequence gets back the same vaddrs and handles.
 * @param owner the mod id; NULL, empty, or an owner with no ledger calls
 *        does nothing. */
void openpete_mods_arena_entry_begin(const char* owner);

/* Snapshot support: the arena's used prefix is timeline state. save
 * copies [0, used); restore is a RE-ANCHOR — bytes allocated after the
 * snapshot keep their contents and the cursor never moves backwards.
 * immutable_check verifies every IMMUTABLE row's checksum (captured on
 * first check) and warns once per broken promise; call it once per
 * keyframe / file save, never per speculative tick. */
/** @brief Return the arena watermark: the end offset of the last allocation, alignment padding included.
 *
 * Snapshots copy the arena bytes [0, used); the runahead hash covers the
 * same span. A restore raises the watermark, never lowers it.
 * @return the used prefix in bytes. */
uint32_t       openpete_mods_arena_used(void);
const uint8_t* openpete_mods_arena_bytes(void);
void           openpete_mods_arena_mem_restore(const void* bytes,
                                               uint32_t used);
void           openpete_mods_arena_immutable_check(const char* when);

/** @brief Return the stream handle for a stream vaddr, the index a mod writes into a frame word's 21-bit field in place of the vaddr.
 *
 * The same vaddr, from any owner, gets the same handle; handle 0 is never
 * issued. Every call is a ledger row keyed by (owner, call index), sharing
 * the call cursor with openpete_mods_guest_alloc: on re-entry a call whose
 * row holds the same vaddr gets that row's handle back and rebinds it in
 * the vstream table; a different vaddr or an allocation row at that index
 * marks the row superseded and takes a fresh handle, and a savestate
 * written against a superseded row is refused on load.
 * @param vaddr the stream's guest vaddr.
 * @param owner the calling mod's id.
 * @return the handle, or 0 when refused: vaddr 0, a vaddr in the kernel
 *         area (physical address below 0x10000), a NULL or empty owner,
 *         the handle table full (VSTREAM_HANDLE_MAX), or a ledger or
 *         owner-table allocation failure. */
uint32_t openpete_mods_vstream_handle(uint32_t vaddr, const char* owner);

/** @brief Play the PCM WAV at path once as a host-side OpenAL sound.
 *
 * The sound plays on a source of mod_sound.c's pool on PsyCross's AL
 * context, outside the SPU, so it writes no guest state and is legal from
 * tick and present hooks. The WAV is loaded through op_wav_load on its
 * first play and its AL buffer is cached by path. While muted the call
 * plays nothing and returns 0.
 * @param path WAV file path.
 * @param gain Source gain, clamped to [0, 4].
 * @return 0 when played or muted; -1 when path is NULL, no AL context
 *         exists, or the load or upload fails (logged). */
int openpete_mod_sound_play(const char* path, float gain);

/** @brief Play in-memory interleaved s16 PCM once as a host-side OpenAL sound.
 *
 * The counterpart of openpete_mod_sound_play for audio the caller already
 * holds, so nothing is staged on disk. The first call for a key uploads
 * the samples to an AL buffer cached under the key; later calls with the
 * same key play the cached buffer and ignore pcm, frame_count, channels
 * and sample_rate. AL copies the samples, so the caller keeps ownership.
 * @param key Cache label for the sound, not a path.
 * @param pcm Interleaved samples.
 * @param frame_count Sample frames in pcm.
 * @param channels 1 or 2.
 * @param sample_rate Sample rate in Hz.
 * @param gain Source gain, clamped to [0, 4].
 * @return 0 when played or muted; -1 when key is NULL, no AL context
 *         exists, an uncached key comes with NULL pcm, zero frames or a
 *         channel count other than 1 or 2, or the upload fails. */
int openpete_mod_sound_play_pcm(const char* key, const int16_t* pcm,
                                uint32_t frame_count, int channels,
                                uint32_t sample_rate, float gain);

/** @brief Set the engine mute for mod sounds; muting stops every source in the pool.
 *
 * Called by openpete_set_mute(). The mod-sound sources live on PsyCross's
 * AL context but outside its SPU mixer, so the SPU's mute flag does not
 * reach them. While muted, play calls return 0 without playing.
 * @param on Nonzero mutes. */
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
/** @brief Return the number of loaded mods, enabled or disabled.
 *
 * @return The row count of the loaded mod table. */
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
    int has_ui;          /* a UI section (register_ui / section descriptor)
                          * or a live ui_status block */
    int ui_always;       /* the section carries OPENPETE_MOD_UI_ALWAYS: its
                          * body draws from the overlay's top-level pass on
                          * every present, not inside this mod's node */
    int has_assets;      /* assets/ overlay root (boot-scoped tier) */
    int has_src;
    int has_cfg;         /* [[config]] schema rows in the manifest */
    uint64_t imgui_calls; /* ImGui calls the section's last body
                           * made; 0 = the mod does not use ImGui,
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
/** @brief Write config edits into a mod's config.toml and queue its reload for the next tick boundary.
 *
 * openpete_mods_config_write writes the file. openpete_mods_tick_boundary
 * then reloads a source mod whose entry has run, so the entry re-reads
 * the values through config_*, and re-parses the file for any other mod.
 * The dev overlay's Mods panel and the OPENPETE_MODS_CFG_APPLY schedule
 * call it.
 * @param mod_index Index into the loaded mod table.
 * @param n Number of edits.
 * @param edits The edits.
 * @return 0 when written (the reload is asynchronous), -1 for an index
 *         out of range, n <= 0, NULL edits, or a failed write. */
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

/** @brief Run a mod's UI body in the always pass when its section is flagged OPENPETE_MOD_UI_ALWAYS.
 *
 * No-op for an index out of range, a NULL @p ui, a disabled mod, an
 * unflagged section or a mod without a body. Draws no ui_status lines;
 * those stay in the mod's node of the Mods panel. The body runs under the
 * render-purity guard (openpete_mods_in_present).
 * @param i Index into the loaded mod table.
 * @param ui The overlay's widget vtable.
 * @param no_input Nonzero raises the guard bit that puts
 *        ImGuiWindowFlags_NoInputs on every window the body opens; the
 *        caller sets it while the dev overlay is closed and the game owns
 *        the mouse. */
void openpete_mods_ui_draw_always(int i, const struct openpete_mod_ui* ui,
                                  int no_input);

/** @brief Count the enabled mods whose UI section is flagged OPENPETE_MOD_UI_ALWAYS and still has a body.
 *
 * One scan of the mod table. The overlay asks per present whether an
 * ImGui frame is wanted at all; a count derived here cannot drift out of
 * step with the loader's enable flips and body clears (a registration
 * strip, a refused section) the way a cross-thread counter would.
 * @return The count. */
int openpete_mods_ui_always_count(void);

/** @brief Stop drawing a mod's UI section for the rest of the run.
 *
 * Clears the mod's ui_body and its per-present ImGui call count and logs
 * one error line naming @p why; the mod keeps running. The overlay calls it
 * after a mod's body raises an ImGui error on three consecutive presents.
 * A section already refused returns 0 with no log line.
 * @param i Index into the loaded mod table.
 * @param why Reason text for the log line, or NULL.
 * @return 0, or -1 for an index out of range. */
int openpete_mods_ui_refuse_section(int i, const char* why);

int  openpete_mods_set_mod_enabled(const char* id, int on);
/** @brief Apply the queued mod state changes at a canonical tick boundary.
 *
 * core/mods/mod_draw.c calls it at the canonical present commit, when any
 * mod is loaded, with the running canonical-tick counter. In order: fires
 * this tick's OPENPETE_MODS_TOGGLE and OPENPETE_MODS_CFG_APPLY schedule
 * entries, polls hot reload, applies pending material selector changes,
 * runs the moby class census, reloads mods with a pending config apply,
 * and drains the toggle queue. An applied toggle flips owner-keyed
 * disabled flags on the chain, draw, registry and keymap surfaces without
 * tearing anything down, bumps openpete_mods_toggle_epoch, then calls the
 * mod's toggle hook. A disable of a mod that owns the level the player is
 * in stays queued until openpete_mods_registry_owner_busy returns 0; an
 * enable of a mod whose entry never ran runs the entry first and leaves
 * the mod disabled when it fails.
 * @param cpu Live game CPU; the toggle hook runs in tick context, where
 *        guest access is legal.
 * @param canonical_tick The running canonical-tick counter. */
void openpete_mods_tick_boundary(CPUState* cpu, unsigned canonical_tick);
/** @brief Return 1 while the player is inside, or entering, a level that @p mod_id owns.
 *
 * True when one of the mod's claimed level slots, or its active custom
 * level, is the resident level or the level being entered. A disabled slot
 * refuses entry but stays claimed, so the mod cannot be turned off from
 * under the running level: the live-toggle apply loop keeps such a disable
 * queued until this returns 0, and the M overlay greys the mod's switch.
 * @param mod_id Mod id; NULL returns 0.
 * @return 1 when busy, else 0. */
int  openpete_mods_registry_owner_busy(const char* mod_id);
/** @brief Return the toggle epoch, bumped once per applied toggle that changed a mod's enabled state.
 *
 * The run loop polls it to invalidate timeline-derived state: rewind
 * history recorded under the old modset must not replay into the new one,
 * and runahead re-runs its capability gate against the new enabled set.
 * The texture pack, music override and mod texture and material resolvers
 * fold it into their cache keys.
 * @return The epoch. */
unsigned openpete_mods_toggle_epoch(void);
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on);

/* Owner teardown and live enable/disable for the three subsystems that
 * key their entries on a mod id. Declared here beside their registry
 * sibling: mod_loader.c calls all of them, and without a prototype in
 * scope C assumes int(), which silently defeats argument checking on
 * calls that pass a pointer. */
/** @brief Remove every override link an owner registered, compacting each chain.
 *
 * The loader calls it when it strips a mod's registrations (hot reload,
 * whose re-entry registers the links again, and a failed load). The
 * owner's enabled state is kept, so re-registered links come back in it.
 * Call only at a tick boundary.
 * @param owner_id the mod id; NULL does nothing. */
void openpete_mods_chain_remove_owner(const char* owner_id);
void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on);
void openpete_mods_draw_remove_owner(const char* owner_id);
void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on);
void openpete_mods_registry_remove_owner(const char* mod_id);

/** @brief Add @p ns of self time to @p owner's tick or present accumulator.
 *
 * The override chain runner feeds tick hooks (the override's span minus
 * its base() descent and the overrides nested inside it) and mod_draw.c
 * feeds present hooks (each hook's span). An owner gets a slot on first
 * use.
 * @param owner Mod id; NULL is ignored.
 * @param ns Nanoseconds; a negative value is ignored.
 * @param is_present Non-zero for a present hook, zero for a tick hook. */
void openpete_mods_time_add(const char* owner, int64_t ns, int is_present);
/** @brief Log and reset every owner's accumulated time for the window that just ended.
 *
 * Warns for each mod whose tick-hook time averages more than 4 ms per sim
 * tick; with OPENPETE_MOD_TIMERS set non-zero it logs every other owner's
 * figures too, including owners with no calls.
 * @param ticks_elapsed Sim ticks in the window; 0 counts as 1. */
void openpete_mods_time_report(unsigned ticks_elapsed);

/** @brief The mod whose override is innermost on the calling thread's override stack, for the crash handler.
 *
 * The frame stack is thread-local, so the crash handler calls it on the
 * faulting thread; core/diag/crash.c prints the result as a MOD ATTRIBUTION
 * line. The crash is in that mod's override or in code it called.
 * @param addr_out Receives the overridden guest vaddr; may be NULL.
 * @return The owner mod id, "?" when the link has no owner, or NULL when no
 *         override is running on this thread. */
const char* openpete_mods_crash_attribution(uint32_t* addr_out);

/** @brief Return the save modset tag, the canonical identity of the gameplay-relevant loaded modset.
 *
 * The form is "id@ver+id@ver" with ids sorted, over the mods whose entry
 * has run and that mod_is_mutative classifies as changing gameplay state;
 * the empty string is vanilla. Built on the first call and cached; the
 * first enable of a deferred-entry mod drops the cache, so the tag grows
 * with it and does not shrink on a disable. core/host/memcard.c tags cards
 * and warns on a mismatch; core/record/savestate.c records and compares
 * it.
 * @return The tag, owned by the loader. */
const char* openpete_mods_modset_tag(void);

/* ── Draw service (core/mods/mod_draw.c) ───────────────────────────── */

#include "openpete_mod_api.h" /* openpete_mod_present_fn / ctx */

/** @brief Commit the tick's mod draw commands, run present hooks, and pass the overlay vertices to the native renderer.
 *
 * Called once per present: by the eager producer for each scene it
 * produces, and otherwise from run.c's canonical and sub-tick present
 * legs. A canonical call with a mod loaded first runs
 * openpete_mods_tick_boundary (the live-toggle apply point) and, every 512
 * canonical calls, openpete_mods_time_report. The rest runs only once a
 * mod has drawn or registered a present hook: a canonical call swaps the
 * staging and committed tick buffers and clears the new staging, with or
 * without a native renderer. With the native renderer enabled it records
 * the present context (tick openpete_frame; alpha 0 on a canonical call,
 * else the interpolation phase at the present instant), runs every enabled
 * present hook with openpete_mods_in_present set, lays out the committed
 * tick texts and this present's texts through the game's text emitter,
 * and passes the vertices to openpete_native_set_mod_overlay.
 * @param cpu the game CPU, passed to the tick boundary.
 * @param canonical 1 for a tick's canonical present, 0 for a sub-tick
 *        present. */
void openpete_mods_present_flush(CPUState* cpu, int canonical);

/* Game-registered text rasterizer: lays out `str` at (x, y) with glyph
 * cap-height cap_h (overlay space) and tint rgb into out_verts (an
 * openpete_hud_vert array, cap entries), returning the vertex count.
 * Spyro registers a text_moby_native wrapper (main.cpp). */
typedef int (*openpete_mods_text_emit_fn)(const char* str, float x, float y,
                                          float cap_h, unsigned rgb,
                                          void* out_verts, int cap);
void openpete_mods_draw_set_text_fn(openpete_mods_text_emit_fn fn);

/* Loader-facing submission entry points (wired into the mod api). */
/** @brief Queue one text for the mod overlay.
 *
 * Inside a present hook the text goes to this present's buffer and draws
 * on this present only. Elsewhere (a tick hook) it goes to the tick
 * staging buffer, which the next canonical present commits for display on
 * every present of that tick, so a mod redraws every tick to keep text on
 * screen. The running hook's owner is recorded for overflow attribution.
 * @param x left edge in overlay space (window-height fractions, origin
 *        top-left, x in [0, aspect]).
 * @param y top edge in overlay space, +y down.
 * @param cap_h glyph cap height in overlay space.
 * @param rgb tint.
 * @param str the text, truncated to 95 characters; NULL or empty is
 *        ignored. */
void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str);
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

    /* Reconcile a level's target totals on the three EXE count tables.
     * A non-negative value is written; a negative one restores what the
     * table held at boot, which is how a level index is given back.
     * `eggs` is ignored for an index the egg table has no row for.
     * Writes only differing bytes, so a per-tick call is cheap once the
     * tables agree. Returns bytes written, or -1 for an unknown level. */
    int (*set_level_counts)(CPUState* cpu, int level_id,
                            int dragons, int gems, int eggs);
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
/* A level's target totals, as the manifest declared them. Each field is
 * negative when the block did not declare it, which leaves that table on
 * its stock row; no legal value is negative, so the sentinel cannot
 * collide with a declaration. `eggs` is dropped at registration for a
 * level index the 18-entry egg table has no row for. */
typedef struct {
    int dragons;   /* 0..255,   -1 undeclared */
    int gems;      /* 0..32767, -1 undeclared */
    int eggs;      /* 0..255,   -1 undeclared */
} openpete_level_counts_t;

int  openpete_mods_custom_register(const char* mod_id, const char* name,
                                   const char* stem, const char* dir,
                                   const char* donor, int replace,
                                   const char* music, const char* title,
                                   const openpete_level_counts_t* counts,
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

/** @brief Return a mod's stable id string.
 *
 * @param self The mod handle; NULL returns NULL.
 * @return The id, owned by the loader. */
const char* openpete_mods_id_of(openpete_mod_t* self);
/** @brief Return a mod's directory.
 *
 * @param self The mod handle; NULL returns NULL.
 * @return The directory, owned by the loader. */
const char* openpete_mods_dir_of(openpete_mod_t* self);
/** @brief Return the staged SDK directory, <exe>/sdk.
 *
 * @return The directory; empty until openpete_mods_init discovers a mod. */
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
/** @brief Fill @p out with the enabled postfx passes at one insertion point, highest priority first.
 *
 * A pass counts when it is live, enabled, at @p point, and its owning mod is
 * enabled. The whole set is insertion-sorted by the mod's load index, then
 * per-mod order, then registration index, before @p cap is applied, so a
 * truncated set keeps the highest-priority passes and logs one warning.
 * @param point Postfx insertion point (OP_POSTFX_*).
 * @param out Receives the passes.
 * @param cap Capacity of @p out.
 * @return Passes written. */
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
#define OPENPETE_MATERIAL_TER_VARIANTS 5  /* arena vertex stage: plain/anim/near,
                                          * then the anim/near cell records */
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
/* The live bucket ids, ascending, written into `out` (capacity `cap`;
 * OPENPETE_MATERIAL_TER_BUCKET_MAX always suffices). Returns the count.
 *
 * A bucket id is a material index + 1, so the ids are sparse: registering
 * one terrain material at index 200 leaves 199 ids below it with no live
 * material. Iterating 1..bucket_max would bind the stock pipeline and draw
 * the whole arena once per dead id. Iterating this list instead keeps the
 * pass count proportional to the live terrain materials, whatever the
 * table's size. */
int  openpete_mods_material_terrain_bucket_list(int* out, int cap);

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
/* The material view: the per-present interned-slot table (bytes AND the
 * slot -> material / material -> static-slot indices) as one immutable
 * snapshot. The sim thread captures it at scene publish; the present
 * thread composes a published scene through it, so every index it binds
 * refers to the bytes it uploaded. Reading the live table from the present
 * thread races openpete_mods_material_frame_reset, which zeroes every
 * static slot before re-minting it: a read in that window binds block 0
 * (the engine block) as the mod's uniforms. */
#define OPENPETE_MODS_MATERIAL_VIEW_SLOTS 128
#define OPENPETE_MODS_MATERIAL_VIEW_MATS   255
typedef struct {
    int slot_n;                                   /* 1 + interned slots */
    struct { int mat; uint32_t len; unsigned char bytes[256]; }
        slots[OPENPETE_MODS_MATERIAL_VIEW_SLOTS];
    int static_slot[OPENPETE_MODS_MATERIAL_VIEW_MATS];   /* 0 = no params */
    /* The live terrain bucket ids at capture, ascending. The highest is
     * the last entry, so this carries what ter_bucket_max used to. */
    int ter_bucket_n;
    int ter_bucket[OPENPETE_MODS_MATERIAL_VIEW_MATS];
} openpete_mods_material_view_t;
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out);
/* Present thread: resolve/bind through `v` until set back to NULL (live). */
void openpete_mods_material_view_set(const openpete_mods_material_view_t* v);
/** @brief Return a mod's 0-based position in the loaded mod table, which is its load order.
 *
 * The table holds boot-enabled and deferred-entry mods alike, in
 * dependency order.
 * @param self The mod handle; NULL returns -1.
 * @return The load index, or -1 for NULL. */
int  openpete_mods_load_index_of(openpete_mod_t* self);
/** @brief Return the tick, alpha and canonical flag of the last present flush that reached the present hooks.
 *
 * The postfx engine UBO and the native renderer read it, so shaders see the
 * values the present hooks received. Before the first such flush it reads
 * tick 0, alpha 0, canonical 1.
 * @param tick receives the tick, or NULL.
 * @param alpha receives the sub-tick phase, or NULL.
 * @param canonical receives 1 for a canonical present, or NULL. */
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

/* Per-mod user configuration (mod_config.c): mods/<id>/config.toml.
 * Keys are dotted paths ("chests.collision"); getters return `def` for an
 * absent file/key/type-mismatch. */
/** @brief Load a mod's config.toml into its key table, replacing what the previous load stored.
 *
 * The loader calls it right before the mod's entry runs, at load and on hot
 * reload, and after a config write; in dev hot-reload mode it watches the
 * file's content hash, so an edit re-enters the mod with fresh values.
 * Nested tables flatten to dotted keys; bool, integer, float and string
 * values are stored, and arrays and other types are logged and skipped. A
 * missing file leaves the table empty; a parse error is logged and leaves
 * it empty, so every getter returns its default and the mod stays enabled.
 * @param mod the mod whose table is filled.
 * @param mod_dir the mod's directory.
 * @param log_tag the mod's log tag. */
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

/** @brief Apply edits to a mod's config.toml as text, keeping every comment (the Mods panel's Apply).
 *
 * Each edit rewrites the value of `leaf = ...` inside the section its
 * dotted key names, keeping indentation and a trailing comment; a missing
 * key is added to its section and a missing section is appended at the end
 * of the file, which is created when absent. All edits are applied in
 * memory, then written to config.toml.tmp and renamed over the file, so
 * any failure leaves the file unchanged.
 * @param mod_dir the mod's directory.
 * @param log_tag the mod's log tag.
 * @param n number of edits.
 * @param e the edits.
 * @return 0 when the file was rewritten, -1 otherwise. */
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
/** @brief Register every <hexid>.png under an asset root's textures/own/ as a mod-owned texture.
 *
 * Mod-owned texture content without C code: each PNG registers under the
 * 16-bit id its filename stem spells in hex, at most four digits
 * ("53c1.png"); another stem is skipped with a warning, and a PNG that
 * fails to decode or register is skipped with an error. Faceblocks select
 * the texture with the same PSX-reserved mode-3 Tiledef encoding as
 * openpete_mods_texture_register(). Generators derive the pixels from the
 * user's disc into this directory, so no game-derived data ships. Called by
 * openpete_mods_register_asset_root() when a root registers.
 * @param dir Asset root directory (a mod's assets/).
 * @param owner Owning mod id; stored by pointer, so it must live for the
 *              process (the loader's id).
 * @return Textures registered. */
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

/** @brief Return the compile-on-boot build key of a loaded mod.
 *
 * The key is the 64-character SHA-256 hex digest over the mod API
 * version, the compiler identity, the SDK inputs and the mod's sorted
 * sources. It is empty for a mod without a source build (assets-only, or a
 * prebuilt object loaded without the SDK). core/mods/mod_services.c uses a
 * non-NULL result to tell a loaded mod owner from a non-mod owner.
 * @param id Mod id; NULL returns NULL.
 * @return The key, or NULL when no loaded mod has that id. */
const char* openpete_mods_build_key(const char* id);
/** @brief Test whether every enabled code mod declares a manifest runahead posture.
 *
 * The posture is runahead = "pure" or "host-fx" in the [mod] table. A code
 * mod is one with a loaded object, so a binary-only mod is held to the
 * same declaration as a source one; a mod without an object runs no code
 * and passes. core/runtime/run.c does not arm runahead on 0.
 * @param offender Receives the first undeclared mod's id on 0 and NULL on
 *        1; may be NULL.
 * @return 1 when every enabled code mod declares a posture, else 0. */
int openpete_mods_runahead_capable(const char** offender);
/** @brief Test whether every enabled code mod declares a manifest host-state posture.
 *
 * The posture is state = "none" or "rebuildable" in the [mod] table. It
 * gates timeline jumps (rewind): an undeclared mod's host state can desync
 * when the guest world moves under it. A mod without a loaded object
 * holds no host state and passes.
 * @param offender Receives the first undeclared mod's id on 0 and NULL on
 *        1; may be NULL.
 * @return 1 when every enabled code mod declares a posture, else 0. */
int openpete_mods_state_safe(const char** offender);

/** @brief Size of the arena state section: a header, the ledger rows, and the arena's used bytes.
 *
 * core/record/savestate.c serializes the section with the engine state.
 * @return Section size in bytes. */
uint32_t openpete_mods_arena_state_size(void);
/// @brief Write the arena state section, openpete_mods_arena_state_size() bytes, to @p dst.
void     openpete_mods_arena_state_save(void* dst);
/** @brief Check a saved arena section against the live ledger before a state load commits.
 *
 * The saved rows must equal the live rows over their common prefix,
 * superseded marks included, or the restored bytes would sit under a
 * different layout; a mismatch is logged naming the mod and entry.
 * @param src Saved section bytes.
 * @param size Section size in bytes.
 * @return 0 when the load may proceed (also when there is no section), -1
 *         to refuse it. */
int      openpete_mods_arena_state_verdict(const void* src, uint32_t size);
/** @brief Adopt a saved arena section after a state load commits.
 *
 * Saved rows beyond the live ledger are appended, so a mod's later call at
 * that index replays the saved row; the saved bytes are copied over the
 * arena and the cursor moves to max(live, saved).
 * @param src Saved section bytes.
 * @param size Section size in bytes. */
void     openpete_mods_arena_state_apply(const void* src, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MODS_H */
