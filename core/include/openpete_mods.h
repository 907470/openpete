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

/** @brief Register the generated name-to-address table that override names resolve against.
 *
 * Stores the pointer without copying, so the table must outlive the mod
 * loader. The name lookup binary-searches it, so rows must be sorted by
 * strcmp on name; the address-to-name lookup scans it linearly. Later
 * calls replace the table, and a NULL table makes every lookup fail. The
 * game's main calls it once before openpete_run().
 * @param table First row, or NULL.
 * @param count Number of rows. */
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
/** @brief Allocate a block in the guest arena, or replay the owner's ledger row for this call.
 *
 * Runs the owner's next call index. When the owner's live row at that
 * index is an allocation with the same size, align and flags, that row is
 * served again: same vaddr, same host view, bytes as last written. A row
 * of another shape is marked superseded and the call takes a fresh block
 * at the arena cursor, aligned up and zeroed. The first call that passes
 * argument validation registers the arena as a psx_runtime region at
 * 0x80700000. A failure
 * logs and returns 0, except a bad argument, which returns 0 silently.
 * @param size the block size in bytes; 0 or more than the 8 MiB arena
 *        fails.
 * @param align the alignment, a power of two; 0 selects 8, any other
 *        value fails.
 * @param flags OP_GALLOC_* flags, stored in the row as passed.
 * @param host_view receives the block's host address, or NULL on failure;
 *        may be NULL.
 * @param owner the calling mod's id; NULL or empty fails.
 * @return the block's guest vaddr, or 0 on failure: a bad argument,
 *         region registration failure, exhausted arena, or an owner or
 *         ledger table allocation failure. */
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
/** @brief Return the arena's backing bytes.
 *
 * The buffer spans the whole 8 MiB arena and stays valid for the process
 * lifetime; offsets [0, openpete_mods_arena_used()) hold allocated data.
 * The runahead hash and the in-memory snapshot read that prefix.
 * @return the arena's first byte. */
const uint8_t* openpete_mods_arena_bytes(void);
/** @brief Copy a snapshot's arena bytes over the arena's prefix and raise the watermark to cover them.
 *
 * Bytes past @p used keep their contents and the watermark never
 * decreases, so a handle a mod took after the snapshot stays valid. The
 * arena region is not registered here.
 * @param bytes the snapshot's arena bytes; NULL does nothing.
 * @param used the byte count; 0 does nothing, and a value above the arena
 *        size is clamped to it. */
void           openpete_mods_arena_mem_restore(const void* bytes,
                                               uint32_t used);
/** @brief Check every live IMMUTABLE row's bytes against its first-seen checksum.
 *
 * The first check of a row records the FNV-1a 64-bit hash of its bytes; a
 * later mismatch logs one warning naming the owner and call index and
 * stops checking that row. Superseded rows are skipped.
 * @param when the caller's context, named in the warning; NULL reads as
 *        "snapshot". */
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
/** @brief Return the id of loaded mod i.
 *
 * The string lives in the loader's mod table for the session.
 * @param i Index into the loaded mod table, 0 <= i < openpete_mods_count().
 * @return The id, or NULL for an index out of range. */
const char* openpete_mods_id(int i);
/** @brief Report whether the loaded mod with an id is currently enabled.
 *
 * Reads the live-toggle flag by a linear scan of the mod table by id.
 * @param id Mod id, non-NULL.
 * @return 1 when enabled, 0 when disabled or when no loaded mod has the id. */
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
/** @brief Fill the Mods panel row for loaded mod i.
 *
 * The name falls back to the id when the manifest gives none, has_ui is
 * set for a UI body or a live ui_status block, has_cfg is the number of
 * [[config]] rows, and sim_mutative is the effective verdict.
 * @param i Index into the loaded mod table.
 * @param out Row to fill; its strings point into the mod table.
 * @return 1 when filled, 0 for an index out of range or a NULL @p out. */
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

/** @brief Return the number of [[config]] rows loaded mod mod_index declares.
 *
 * @param mod_index Index into the loaded mod table.
 * @return The row count, 0 for an index out of range. */
int openpete_mods_cfg_row_count(int mod_index);
/** @brief Fill a config row with its schema and current effective value.
 *
 * The value comes from the mod's config getters (config.toml, else the
 * schema default) into ival for bool and int rows, fval for float rows,
 * and sval for string rows; the other value fields are 0 or "". The label
 * falls back to the key. The combo choices in items and the string in sval
 * live in buffers the function reuses, so they stay valid only until the
 * next call, and a string longer than 127 bytes is truncated. Not safe to
 * call from two threads.
 * @param mod_index Index into the loaded mod table.
 * @param row Row index, 0 <= row < openpete_mods_cfg_row_count().
 * @param out Row to fill.
 * @return 1 when filled, 0 for an index out of range or a NULL @p out. */
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

struct openpete_mod_ui;
/** @brief Draw mod i's ui_status lines and run its UI section body with the overlay's widget table.
 *
 * Draws the status lines first, then the body under the render-purity
 * guard (openpete_mods_in_present), unless the section is flagged
 * OPENPETE_MOD_UI_ALWAYS: that body belongs to the always pass below and
 * would otherwise run twice per present. A mod without a body draws its
 * status lines only.
 * @param i Index into the loaded mod table.
 * @param ui The overlay's widget vtable. A NULL table, a disabled mod, or
 *        an index out of range draws nothing. */
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

/** @brief Queue enabling or disabling a mod, applied at the next tick boundary.
 *
 * Copies the id into the request queue, truncated to the loader's id
 * width, and treats any nonzero @p on as 1. Nothing is validated here: an
 * id that names no loaded mod is logged as an error when the queue drains,
 * and a disable that would strand the player in the mod's level stays
 * queued until the player leaves it. The queue has no lock.
 * @param id Mod id.
 * @param on Nonzero to enable, 0 to disable.
 * @return 0 when queued, -1 for a NULL or empty id or a failed queue
 *         allocation. */
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
 * The resident level is the last live level the per-tick registry driver
 * observed, so a pause or a level teardown does not end residency. Reads
 * host-side statics only, so the menu thread may call it without guest
 * access.
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
/** @brief Enable or disable every override link an owner registered, keeping chain order.
 *
 * A disabled link is skipped by dispatch and by base(), and stays in its
 * chain. The owner's state is recorded apart from its links, so links
 * registered later (hot reload) start in it. Call only at a tick boundary.
 * @param owner_id the mod id; NULL does nothing.
 * @param on nonzero to enable, 0 to disable. */
void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on);
/** @brief Remove every present hook an owner registered, compacting the table.
 *
 * @param owner_id the mod id; NULL does nothing. */
void openpete_mods_draw_remove_owner(const char* owner_id);
/** @brief Enable or disable every present hook an owner registered.
 *
 * A disabled hook stays in the table and is skipped by the flush.
 * @param owner_id the mod id; NULL does nothing.
 * @param on nonzero to enable, 0 to disable. */
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
/** @brief Register the game's text rasterizer for the mod overlay.
 *
 * The flush calls it once per queued text. Without one, or while it
 * returns 0, queued text draws nothing.
 * @param fn the emitter; NULL unregisters it. */
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
/** @brief Add a hook that runs once per present with that present's context.
 *
 * The hook starts disabled when the loader lists the owner and reports it
 * disabled. Sets the flag that keeps the draw flush active.
 * @param fn the hook; NULL fails.
 * @param owner_id the registering mod's id, stored by pointer, or NULL.
 * @return 0, or -1 for a NULL fn or a table allocation failure. */
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
/** @brief Queue a warp into the custom level at alphabetical position @p i.
 *
 * The registry's per-tick driver consumes the warp; one queued warp is
 * held, and a later call replaces it.
 * @param i Position in the name order openpete_mods_custom_info uses.
 * @return 0, or -1 when @p i is out of range. */
int  openpete_mods_custom_warp(int i);
/** @brief Set the custom level at alphabetical position @p i to slot or replace serving.
 *
 * The per-tick driver declares or withdraws the replace over the donor's
 * entry on its next tick. An out-of-range @p i is ignored.
 * @param i Position in the name order openpete_mods_custom_info uses.
 * @param replace 0 serves the level from the shared slot, nonzero over its
 *        donor's own entry. */
void openpete_mods_custom_set_mode(int i, int replace);
/** @brief Name the custom-level files that serve a logical level id right now.
 *
 * The WAD overlay calls it. The active slot row serves the claimed slot
 * id, and a replace row whose replace is declared serves its donor's id;
 * either only while its mod is enabled.
 * @param level_id Logical level id.
 * @param dir_out Receives the owning mod's assets directory on a hit.
 * @param stem_out Receives the file stem on a hit.
 * @return 1 when a row serves the level, else 0. */
int  openpete_mods_custom_files_for(int level_id, const char** dir_out,
                                    const char** stem_out);
/** @brief Build the path of a custom level's data or code-overlay file under `<dir>/levels/`.
 *
 * The data file is `<stem>.WAD` (the level's WAD data segment) and the
 * code overlay `<stem>.OVL` (the donor's overlay); level editors emit
 * those extensions in either case, and case matters on Linux, so a miss on
 * the literal lowercase path scans the directory for a case-insensitive
 * match of the whole file name.
 * @param dir The mod's assets directory.
 * @param stem File stem.
 * @param is_ovl Nonzero for the code overlay, 0 for the data file.
 * @param out Receives the path; on a miss, the lowercase path, so callers
 *        can name the missing file.
 * @param cap Capacity of @p out.
 * @return 0 when the file exists, -1 when it is absent or an argument is
 *         NULL or @p cap is 0. */
int  openpete_mods_custom_asset_path(const char* dir, const char* stem,
                                     int is_ovl, char* out, size_t cap);
/** @brief Return the custom song for a logical level id right now.
 *
 * Row selection matches openpete_mods_custom_files_for: the active slot
 * row for the claimed slot id, else the first replace row whose replace is
 * declared for that donor. The song plays only while the row's mod is
 * enabled.
 * @param level_id Logical level id.
 * @param dur_sec_out Receives the song's duration in seconds; may be NULL.
 * @return The WAV path, or NULL when no row serves the level, the row has
 *         no song, the file was missing at registration, or the mod is
 *         disabled. */
const char* openpete_mods_custom_music_for(int level_id,
                                           double* dur_sec_out);

int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag);
int openpete_mods_level_slot_id(const char* mod_id, const char* name);
int openpete_mods_level_enter(CPUState* cpu, int level_id);
int openpete_mods_read_level_data(int level_id, void* buf, uint32_t cap);

/** @brief Register a mod's level-ready hook.
 *
 * The hook fires once per level id, on the first master-update tick with
 * gameplay live (level_id > 0), in tick context, where guest reads, writes
 * and call_addr are legal. A new hook fires on the next live tick,
 * including for a level already in progress. Registration arms the
 * per-tick registry driver. openpete_mods_registry_set_owner_enabled
 * gates the hook and re-arms it on enable;
 * openpete_mods_registry_remove_owner drops it.
 * @param mod_id Owning mod id; NULL returns -1.
 * @param fn The hook; NULL returns -1.
 * @param log_tag Log tag for the registration lines.
 * @return 0, or -1 on a NULL argument, when the driver cannot be armed, or
 *         when the hook table allocation fails. */
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
 * GLSL compile service, postfx passes and material selectors. */
/** @brief Register a mod's GLSL shader, compiling it or loading its cached or prebuilt SPIR-V, and return its handle.
 *
 * The stage comes from the extension (`.vert` or `.frag`). A path holding
 * `..`, of MOD_SHADER_PATH_MAX bytes or more, or registered inside a
 * present hook is refused. With the source present, the SPIR-V is cached
 * under `<mod>/.build/shaders/` keyed on a SHA-256 build key, and a
 * material fragment is compiled a second time for the retained-terrain
 * profile; a failed terrain compile logs and leaves that profile absent.
 * Without the source, `<path>.spv` loads only when `<path>.spv.ihash`
 * holds the current interface hash and the file is a nonempty whole number
 * of 32-bit words. Takes the registry write lock.
 * @param self Registering mod.
 * @param path Shader source path relative to the mod directory.
 * @return The handle, an index that stays valid until the owner is swept,
 *         or -1 on failure. */
int openpete_mods_shader_register(openpete_mod_t* self, const char* path);
/** @brief Return the SPIR-V words of a live shader handle.
 *
 * @param handle Shader handle.
 * @param words Receives the word count; may be NULL.
 * @param stage Receives the glslang_stage_t value (vertex or fragment);
 *        may be NULL.
 * @return The words, or NULL for an out-of-range or swept handle. */
const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage);
/** @brief Return the retained-terrain profile of a material fragment shader.
 *
 * The same source compiled with OP_PROFILE_TERRAIN
 * (openpete_psx_material.glsl), whose engine half reads flat per-face
 * varyings instead of a per-draw push block. NULL when the handle is out
 * of range or swept, or the shader has no terrain profile (not a material
 * fragment, or its terrain compile failed); terrain then draws stock.
 * @param handle Shader handle.
 * @param words Receives the word count; may be NULL.
 * @return The words, or NULL. */
const uint32_t* openpete_mods_shader_spv_terrain(int handle, size_t* words);
/** @brief Free every shader a mod registered and mark its slots dead.
 *
 * Takes the registry write lock. Both SPIR-V profiles are freed and the
 * slot stays allocated with live 0, so a stale handle returns NULL from
 * openpete_mods_shader_spv() and is never reused. Callers remove the
 * owner's postfx passes and materials first, which retires their cached
 * pipelines.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_shader_remove_owner(const char* mod_id);
/** @brief Compute the hot-reload watch key of a mod's shaders/ directory.
 *
 * SHA-256 over the name and content of every `.vert`, `.frag`, `.glsl`,
 * `.spv` and `.ihash` file in `<mod_dir>/shaders/`, in strcmp order, at
 * most 64 files. Shader sources elsewhere in the mod directory are keyed
 * per registration but not watched.
 * @param mod_dir The mod's directory.
 * @param key_out Receives the 64-character hex digest, or "-" when the
 *        directory holds no such file.
 * @return 0. */
int openpete_mods_shader_watch_key(const char* mod_dir, char key_out[65]);
/** @brief Register a fragment shader as a disabled postfx pass and return its handle.
 *
 * Takes the registry write lock. Refused inside a present hook, for an
 * insertion point other than 0 (COMPOSITE) or 1 (SCENE), and for a handle
 * that is not a live fragment-stage shader. The pass runs only after
 * openpete_mods_postfx_enable(); enabled passes at one point run by
 * ascending owner load index, then @p order, then registration index.
 *
 * @param self Registering mod.
 * @param frag_shader Fragment shader handle.
 * @param point Insertion point (OP_POSTFX_*).
 * @param order Position among the same mod's passes; lower runs first.
 * @return The pass handle, or -1. */
int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order);
/** @brief Turn a postfx pass on or off.
 *
 * Takes the registry write lock.
 *
 * @param self Owning mod.
 * @param fx Pass handle.
 * @param enabled Nonzero to run the pass.
 * @return 0, or -1 for a NULL mod, an unknown or swept handle, or a pass
 *         another mod owns. */
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled);
/** @brief Copy a postfx pass's uniform block.
 *
 * Takes the registry write lock. The block is copied at the call, so the
 * caller keeps its buffer. A block longer than OPENPETE_POSTFX_UNIFORMS_MAX
 * is truncated to that length and the call still succeeds.
 *
 * @param self Owning mod.
 * @param fx Pass handle.
 * @param data Block bytes.
 * @param size Length of @p data.
 * @return 0, or -1 for a NULL mod, a NULL @p data, an unknown or swept
 *         handle, or a pass another mod owns. */
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
/** @brief Move retired pipeline pointers out of the retirement list for the renderer to destroy.
 *
 * Takes no lock: the renderer calls it inside a compose that holds the
 * registry read lock, and at shutdown. The taken entries are removed from
 * the list; the rest keep their order.
 *
 * @param out Receives the pointers.
 * @param cap Capacity of @p out.
 * @return The number of pointers written. */
int  openpete_mods_postfx_take_retired(void** out, int cap);
/** @brief Retire the pipelines of every postfx pass a mod registered and mark the passes dead.
 *
 * Takes the registry write lock. The slots stay allocated with live 0 and
 * are not reused.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_postfx_remove_owner(const char* mod_id);
/** @brief Move the cached pipeline of every live postfx pass, enabled or not, to the retirement list.
 *
 * Takes the registry write lock. Used at shutdown; the passes stay live and
 * rebuild their pipelines on next use. */
void openpete_mods_postfx_retire_all(void);

/** @brief Take the registry read lock for a compose.
 *
 * Held for the whole compose, so the shader, postfx and material tables
 * cannot change under it; their mutators take the write side. Records the
 * calling thread, so a mutator called from inside the compose on that
 * thread proceeds without the write lock instead of deadlocking. */
void openpete_mods_registry_read_lock(void);
/** @brief Release the registry read lock and clear the recorded reader thread. */
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
/** @brief Resolve a draw's provenance tag to the material override that applies.
 *
 * Takes no lock; the compose calls it under the registry read lock. A slot
 * the tag carries (moby bits 27:16, terrain bits 24:16) selects its
 * material directly; slot 0 falls through to the declarative winner among
 * live, user-enabled materials of enabled mods, the highest load index
 * winning. An OPENPETE_MATERIAL_SLOT_STOCK or
 * OPENPETE_MATERIAL_TERRAIN_STOCK slot, a slot past the view, and a slot
 * whose material is swept or owner disabled resolve to none.
 *
 * @param tag Provenance tag; 0 resolves to none.
 * @param out Receives the shaders, pipeline slots, owner and uniform slot.
 * @return The winning material index, or -1 for none. */
int  openpete_mods_material_resolve(uint32_t tag,
                                    openpete_mod_material_gpu_t* out);
/** @brief Retire the pipelines of every material a mod registered and mark the materials dead.
 *
 * Takes the registry write lock. Also drops a parked selector retune, bumps
 * the terrain epoch, and re-arms the batcher tag split. The slots stay
 * allocated with live 0 and are not reused, so the table's capacity is
 * consumed by every registration, including re-registrations after a hot
 * reload.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_material_remove_owner(const char* mod_id);
/** @brief Move the cached pipelines of every live material to the retirement list.
 *
 * Takes the registry write lock. Used at shutdown; the materials stay live
 * and rebuild on next use. */
void openpete_mods_material_retire_all(void);

/** @brief Return 1 when any live material selector carries a per-instance callback.
 *
 * A cached flag, recomputed whenever a selector registers, toggles, is
 * retuned or is swept. The moby walk skips the per-instance resolve while
 * it is 0.
 * @return 1 or 0. */
int  openpete_mods_material_has_instance(void);
/* Moby tag slot field: 0 defers to the draw-time class resolve,
 * OPENPETE_MATERIAL_SLOT_STOCK marks a declined callback (draw stock),
 * anything else is an interned per-instance slot. */
#define OPENPETE_MATERIAL_SLOT_STOCK 0xFFF
/* Terrain-untextured twin of SLOT_STOCK: the terrain tag's slot field is
 * 9 bits ([24:16]), so its "declined -> stock" sentinel is 0x1FF. */
#define OPENPETE_MATERIAL_TERRAIN_STOCK 0x1FF
/** @brief Resolve the per-instance material slot of one moby at extraction.
 *
 * Takes no lock. Returns 0 when no live selector has a callback, or the
 * winning material for the class is declarative or absent. Otherwise runs
 * the winner's callback with a moby key; its block, capped at
 * OPENPETE_MATERIAL_UNIFORMS_MAX, reuses an interned slot of the same
 * material with identical bytes, else takes a new one. The slot table is
 * reset by openpete_mods_material_frame_reset(), so a slot is valid for one
 * present.
 *
 * @param cpu Unused.
 * @param moby_class Moby class id.
 * @param moby_vaddr Guest address of the moby.
 * @return 0 to defer to the draw-time resolve, OPENPETE_MATERIAL_SLOT_STOCK
 *         when the callback declined or the slot table is full, else the
 *         slot. */
int  openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                      uint32_t moby_vaddr);
/** @brief Resolve the per-instance material of the untextured terrain band to a uniform slot.
 *
 * The untextured band is the beyond-LOD gouraud terrain; the whole band
 * shares one outcome, and the terrain feed resolves it once per feed. The
 * winner is the highest-load_index live, user-enabled per-instance
 * material of an enabled mod on channel -1 or OP_CHAN_TERRAIN with
 * OP_SEL_UNTEXTURED and no texture key. Its refine callback runs at most
 * once per present and receives a terrain key with texture -1.
 * @param cpu Unused.
 * @return The slot for OPENPETE_TAG_TERRAIN_UNTEX: 0 when no per-instance
 *         material wins (the draw-time declarative resolve applies), an
 *         interned slot, or OPENPETE_MATERIAL_TERRAIN_STOCK when the
 *         callback declined or the slot table is full. */
int  openpete_mods_material_terrain_untex_slot(CPUState* cpu);
/** @brief Resolve the per-instance material of one textured terrain face's Tiledef id to a uniform slot.
 *
 * The winner is the highest-load_index live, user-enabled per-instance
 * material of an enabled mod on channel -1 or OP_CHAN_TERRAIN without
 * OP_SEL_UNTEXTURED whose texture key equals @p texid. Its refine callback
 * runs at most once per present and receives the Tiledef id in the key.
 * The terrain feed memoizes the result per feed and Tiledef id.
 * @param cpu Unused.
 * @param texid Tiledef id.
 * @return The slot for OPENPETE_TAG_TERRAIN_TEX, with the same values as
 *         openpete_mods_material_terrain_untex_slot(). */
int  openpete_mods_material_terrain_tex_slot(CPUState* cpu, int texid);
/** @brief Return the epoch the retained terrain cache latches its bake against.
 *
 * A baked face carries its material's bucket, so any change that can move
 * a terrain bucket makes the bake stale. The value is 1 + a counter bumped
 * on a change to a material that could bucket terrain (register, remove,
 * selector retune, enable toggle) and on a level change + the mod toggle
 * epoch; it strictly increases on each such change and is never 0, which
 * the latch keeps as its unset value.
 * @return The epoch. */
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

/** @brief Return the retained-terrain material bucket for a face key, or 0 for none.
 *
 * Applies the draw-time resolve's predicates and priority, so a face
 * shades identically walked or retained: a per-instance selector whose
 * texture key matches (or, for the untextured band, one with
 * OP_SEL_UNTEXTURED) outranks every declarative one, and within each kind
 * the later-loaded mod wins. Only live, user-enabled selectors of enabled
 * mods on the terrain channel in scope for the current level count. The
 * bucket is the winning material index + 1; a winner whose bucket would
 * exceed OPENPETE_MATERIAL_TER_BUCKET_MAX returns 0.
 * @param texid Tiledef id, or -1 for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face.
 * @return The bucket id, 0 for none. */
int  openpete_mods_material_terrain_bucket(int texid, int env, int lp);
/** @brief Write the live retained-terrain bucket ids in ascending order.
 *
 * A bucket id is a material index + 1, listed when the material is live,
 * user-enabled, owned by an enabled mod, on channel -1 or OP_CHAN_TERRAIN,
 * and in scope for the current level. The ids are sparse: one terrain
 * material at index 200 leaves 199 ids below it with no live material.
 * The terrain cache draws one pass per listed id, which keeps the pass
 * count proportional to the live terrain materials; iterating every id up
 * to the highest would bind the stock pipeline and draw the whole arena
 * once per dead id. With a material view installed the list is the one
 * the view captured.
 * @param out Receives the ids.
 * @param cap Capacity of @p out; OPENPETE_MATERIAL_TER_BUCKET_MAX always
 *        suffices.
 * @return The count written; 0 for a NULL @p out or @p cap <= 0. */
int  openpete_mods_material_terrain_bucket_list(int* out, int cap);

typedef struct {
    const uint32_t* frag_spv;    /* the OP_PROFILE_TERRAIN module          */
    size_t          frag_words;
    void**          pipeline_slot; /* cached rhi_pipeline* for this variant */
    const char*     owner;
    int             uniform_slot;  /* material-UBO block (0 = no params)    */
} openpete_mod_material_terrain_t;
/** @brief Fill the GPU binding of one retained-terrain material bucket for one pipeline variant.
 *
 * The bucket's material must be live, user-enabled, owned by an enabled
 * mod, and carry a fragment shader with a terrain profile. A per-instance
 * material binds the uniform slot its refine callback produced this
 * present; a declined outcome returns -1. A declarative material binds its
 * static slot, read through the installed material view when one is set.
 * On -1 the caller draws the bucket's faces with the stock pipeline, since
 * the stock pass has already dropped them.
 * @param bucket Bucket id, a material index + 1.
 * @param pipe_idx Pipeline variant index, 0 to OPENPETE_MATERIAL_TER_PIPES - 1.
 * @param out Receives the terrain module, the cached pipeline slot, the
 *        owner and the uniform slot.
 * @return 0 on success, -1 otherwise. */
int  openpete_mods_material_terrain_bucket_gpu(
        int bucket, int pipe_idx, openpete_mod_material_terrain_t* out);
/** @brief Reset the per-present interned material slot table.
 *
 * native_render calls it at scene begin and at the canonical present's
 * feed reset; the extractor then interns per-instance slots during the
 * walk, and native_render writes each slot's bytes into the material UBO
 * before the scene draw. It advances the present counter that the terrain
 * refine memo keys on, and re-interns the static uniform block of every
 * live material with params whose owner is enabled, so a declarative
 * material has a slot for the whole present; once the table is full the
 * remaining materials get static slot 0 (no params). */
void openpete_mods_material_frame_reset(void);
/** @brief Set the level being rendered, for level-scoped material selector keys.
 *
 * Sector and texture ids are per-level, and the mods layer cannot read the
 * game's level symbol, so the terrain feed pushes the id while it renders.
 * Until a push the level is -1, which makes level keys inert. A change of
 * level bumps the terrain material epoch, since the retained terrain
 * latches level-dependent verdicts against it. One int written between
 * scenes and read at draw; no lock is taken.
 * @param level_id The game's level id. */
void openpete_mods_set_level(int level_id);
/** @brief Return one plus the number of slots interned this present in the live table.
 *
 * Slot 0 is reserved, so the valid slots are 1 up to the count minus one.
 * Reads the live table, not the installed view.
 *
 * @return The count. */
int  openpete_mods_material_slot_count(void);
/** @brief Return the uniform bytes of an interned slot in the live table.
 *
 * The pointer addresses the live table and changes at the next
 * openpete_mods_material_frame_reset().
 *
 * @param slot Slot index.
 * @param len Receives the byte count, 0 when the slot is out of range; may
 *        be NULL.
 * @return The bytes, or NULL for slot 0, a negative slot, or a slot at or
 *         past the count. */
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
/** @brief Copy the interned slots, per-material static slots and terrain bucket list into a view.
 *
 * Takes no lock; the sim thread calls it at scene publish. Copies slots 1
 * up to the live count, the static slot of every material index, and the
 * terrain bucket ids read from the live materials.
 *
 * @param out Receives the snapshot. */
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out);
/** @brief Install the material view the present thread resolves and binds through.
 *
 * native_render sets a published scene's view before composing it and
 * NULL after; NULL makes the resolve read the live tables.
 * @param v The view, or NULL. */
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
/** @brief Register a material selector with shader overrides, enabled, and return its handle.
 *
 * Takes the registry write lock. Refused inside a present hook, for a NULL
 * mod or selector, for a selector the engine cannot honour exactly, for a
 * shader handle that is not a live shader of the matching stage, when both
 * handles are negative, and when the table holds
 * OPENPETE_MATERIAL_TER_BUCKET_MAX entries, swept ones included. Bumps the
 * terrain epoch when the selector could bucket terrain and re-arms the
 * batcher tag split.
 *
 * @param self Registering mod.
 * @param sel Selector; copied.
 * @param vert_shader Vertex shader handle, or -1 for the stock stage.
 * @param frag_shader Fragment shader handle, or -1 for the stock stage.
 * @param per_instance Refine callback, or NULL for a declarative material.
 * @return The material handle, or -1. */
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance);
/** @brief Turn a registered material on or off.
 *
 * Takes the registry write lock. Bumps the terrain epoch when the material
 * could bucket terrain and re-arms the batcher tag split. The caller is not
 * checked against the owner.
 *
 * @param self Unused.
 * @param material Material handle.
 * @param on Nonzero to enable.
 * @return 0, or -1 for an unknown or swept material. */
int openpete_mods_material_enable(openpete_mod_t* self, int material, int on);
/** @brief Set a material's static uniform block.
 *
 * Takes the registry write lock. The block is copied and interned into a
 * slot at each present for a material whose owner is enabled. The caller is
 * not checked against the owner.
 *
 * @param self Unused.
 * @param material Material handle.
 * @param block Block bytes.
 * @param len Length of @p block.
 * @return 0, or -1 for an unknown or swept material, a @p len above
 *         OPENPETE_MATERIAL_UNIFORMS_MAX, or a NULL @p block with nonzero
 *         @p len. */
int openpete_mods_material_set_params(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len);
/** @brief Replace a live material's selector, keeping its handle, shaders, callback and enabled state.
 *
 * Takes the registry write lock. Only the owning mod may retune, and the
 * selector must be one openpete_mods_material_register() accepts. Inside a
 * present the selector is parked until
 * openpete_mods_material_apply_pending(); outside one it applies at once
 * and bumps the terrain epoch when the old or new selector could bucket
 * terrain.
 *
 * @param self Owning mod.
 * @param material Material handle.
 * @param sel New selector; copied.
 * @return 0, or -1 for a NULL argument, an unknown or swept material, a
 *         material another mod owns, or an invalid selector. */
int openpete_mods_material_set_selector(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel);
/** @brief Install material selectors retuned from inside a present.
 *
 * openpete_mods_tick_boundary calls it, where a selector change cannot
 * split one present between two keys. Returns without taking the registry
 * lock when nothing is parked. */
void openpete_mods_material_apply_pending(void);

/** @brief Mod API sfx_replace: register an in-memory SPU-ADPCM body as a symbolic sfx-pack replacement.
 *
 * The name passes through the game's alias canonicaliser, when one is set,
 * then sfx_pack_register_symbolic_adpcm registers the entry under the
 * calling mod's id; the substitution is audible only, and every byte the
 * guest observes stays stock. Logs the outcome.
 * @param self The calling mod.
 * @param name SoundTable slot name or a game alias of one.
 * @param body SPU-ADPCM body.
 * @param len Body length in bytes.
 * @param pitch SPU pitch that pins the playback rate, or 0 to follow the
 *        game's pitch writes.
 * @return 0, or -1 when an argument is NULL or zero or the pack refuses
 *         the entry. */
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
/** @brief Read a boolean config value.
 *
 * A bool or integer value reads as nonzero-is-true; a value of another
 * type logs a warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
int     openpete_mods_config_bool(openpete_mod_t* self, const char* key,
                                  int def);
/** @brief Read an integer config value.
 *
 * A bool reads as 0 or 1 and a float as its truncation; a string logs a
 * warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
int64_t openpete_mods_config_int(openpete_mod_t* self, const char* key,
                                 int64_t def);
/** @brief Read a float config value.
 *
 * A bool reads as 0.0 or 1.0 and an integer as its double conversion; a
 * string logs a warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
double  openpete_mods_config_float(openpete_mod_t* self, const char* key,
                                   double def);
/** @brief Read a string config value into a caller buffer.
 *
 * Copies the value, or @p def (empty when NULL) for an absent key or a
 * non-string value, truncated to fit and NUL-terminated. A non-string
 * value logs a warning. Values are stored at most 127 characters long.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the default text; may be NULL.
 * @param buf the destination; NULL returns 0 without writing.
 * @param cap the destination size in bytes; 0 or less returns 0 without
 *        writing.
 * @return 1 when the key held a string, else 0. */
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

/** @brief Register or replace a mod-owned RGBA8 texture under a 16-bit id.
 *
 * Model faces select the texture with the PSX-reserved Tiledef tpage mode
 * 3 and the id in the CLUT halfword; the native renderer fetches it with
 * openpete_mods_texture_get() and binds it through the HD pipeline. The
 * texture is host-side only: guest RAM and VRAM are untouched. The pixels
 * are copied. Re-registering an id replaces its image; the old buffer is
 * freed when it was never drawn, else retired and never freed, so its
 * address cannot key a new GPU texture while the old one stays cached.
 * @param self Registering mod; its id becomes the owner.
 * @param id Texture id.
 * @param rgba @p w * @p h RGBA8 pixels.
 * @param w Width, 1 to 1024.
 * @param h Height, 1 to 1024.
 * @return 0, or -1 for a NULL @p rgba, an out-of-range size, or an
 *         allocation failure. */
int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h);
/** @brief Return the RGBA8 pixels of a registered mod texture and mark them handed to the renderer.
 *
 * Linear scan by id, called from the renderer. Marking the entry drawn
 * makes a later re-registration retire its buffer instead of freeing it. A
 * texture whose owner is disabled is still served, with one log line per
 * toggle epoch; the first draw of an id also logs once.
 *
 * @param id Texture id.
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL.
 * @return The pixels, which stay allocated for the session, or NULL when
 *         the id is not registered. */
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

/** @brief Queue a mod-requested savestate save or load for the host loop's quiescent point.
 *
 * One op is pending at a time; a request while one is pending is dropped
 * with a warning. core/runtime/run.c consumes it through
 * openpete_mods_pending_state_op.
 * @param op 1 = save, 2 = load.
 * @param slot Savestate slot, below OPENPETE_STATE_NUM_SLOTS.
 * @param owner Requesting mod id, kept by pointer until the op is
 *        consumed; NULL logs as "?".
 * @return 0 when queued, -1 for a bad op or slot or when an op is already
 *         pending. */
int openpete_mods_savestate_request(int op, unsigned slot,
                                    const char* owner);

/** @brief Take the pending mod savestate op, clearing it.
 *
 * core/runtime/run.c calls it once per host iteration and executes the op
 * at the same point as the savestate hotkeys: the game fiber is yielded
 * and the pad state is not yet refreshed. An op whose owner is a loaded
 * mod (openpete_mods_build_key returns non-NULL) that is no longer enabled
 * is dropped with a log line, since the tick boundary may disable the mod
 * between the request and this call; an owner that is not a loaded mod is
 * never filtered.
 * @param out_slot Receives the op's slot when an op is returned; may be
 *        NULL.
 * @return 1 = save, 2 = load, 0 when none is pending or the op was
 *         dropped. */
int openpete_mods_pending_state_op(unsigned* out_slot);

/** @brief Append a mod's assets directory to the asset-overlay root table.
 *
 * The loader registers one root per entered mod with an assets/ dir, in
 * enabled order, which is priority order: a later root wins. Consumers
 * (texture pack, music override) layer the roots over their base
 * directories. The table is append-only and never filtered by live-toggle
 * state, so indices stay stable; each consumer skips a root while
 * openpete_mods_is_enabled(owner) is false. Registration also scans the
 * root's textures/own/ with openpete_mods_texture_scan_root.
 * @param dir Root directory, copied.
 * @param owner Owning mod id, stored by pointer; it must live for the
 *        process (the loader's id).
 * @return 0, or -1 when @p dir is NULL or empty or the table allocation
 *         fails. */
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
/** @brief Write the arena state section to a buffer.
 *
 * The layout is a version-3 header (version, row count, used bytes), one
 * fixed-width record per ledger row, superseded marks included, then the
 * arena's used prefix. Host-only row state (the immutable digest) is not
 * written.
 * @param dst a buffer of at least openpete_mods_arena_state_size() bytes. */
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
