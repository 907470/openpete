// core/include/game_extras.h — the per-game abstraction.
//
// Every behavior that differs between Spyro 1, 2, 3 (entry point, CRC,
// overlay layout, runtime quirks, disc layout) is declared here and
// implemented by the per-game `games/<game>/game_extras_impl.c`.
//
// Rule: nothing in core/ may reference a game-specific address, symbol,
// or struct. If `grep -r "0x800" core/` returns a hit, it's a bug.
//
// See notes/plans/openpete-pc-port.md.

#ifndef OPENPETE_GAME_EXTRAS_H
#define OPENPETE_GAME_EXTRAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward decl — the recompiled CPU state. Defined in core/runtime/cpu_state.h
// once Phase 1b lands. For Phase 0b, treat it as opaque.
struct CPUState;
typedef struct CPUState CPUState;

/* --- Identity --- */
uint32_t    game_get_entry_point(void);
const char* game_get_exe_filename(void);      /* e.g. "SCUS_942.28" */
uint32_t    game_get_expected_crc32(void);    /* sanity check against user's dump */
const char* game_get_display_name(void);      /* "Spyro the Dragon" */
const char* game_get_id(void);                /* "spyro-1" — the [game.<id>] config scope */

/* Guest address of the function through which this game reads its pad
 * buffer — the moment that decides which host input sample a tick plays.
 * Return 0 if unknown: the pacing census then reports the pad-read phase as
 * unmeasured rather than as zero reads. */
uint32_t    game_get_pad_read_pc(void);

/* --- Memory layout ---
 * compiled_range / overlay_range are PSX-side virtual addresses; the
 * impl supplies the values for its game. */
uint32_t game_get_ram_size(void);             /* typically 2 MiB */
uint32_t game_get_compiled_range_start(void);
uint32_t game_get_compiled_range_end(void);
uint32_t game_get_overlay_range_start(void);
uint32_t game_get_overlay_range_end(void);

/* --- Overlay handling (intercept point) ---
 * Called when the game issues its native overlay-load. Implementations
 * swap the active dispatcher, copy the overlay's data into g_ram, and
 * update entry-point function pointers. Return 0 on success. */
int  game_on_overlay_load(uint32_t overlay_id);
void game_on_overlay_init(CPUState* cpu);     /* one-time, post-WadInit */

/* --- Per-game runtime quirks ---
 * Called once at boot, before main(). Apply per-game patches:
 * preamble fixes, known-bad-instruction workarounds, etc. */
void game_apply_runtime_quirks(CPUState* cpu);

/* --- Post-boot, pre-execution ---
 * Fires once after the EXE image is resident in guest RAM and after any
 * host-side seeding, but before the game fiber is created, so no guest
 * instruction has run. Use it to snapshot EXE .data the engine intends
 * to overwrite later: a snapshot taken any later can capture a value the
 * engine itself wrote, and a savestate load can replace the RAM it would
 * have read. */
void game_on_boot(CPUState* cpu);

/* --- Per-frame callbacks (rare; e.g., DMA timing hacks).
 * `game_on_frame_pre`  fires immediately BEFORE handing control to the
 *                      game fiber for a tick — use it to mutate state
 *                      that the upcoming tick will read.
 * `game_on_frame_post` fires AFTER the tick yields back, only on iters
 *                      where the frame counter advanced. */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter);
void game_on_frame(CPUState* cpu, uint32_t frame_counter);

/* --- Hash-emit hook (diagnostic) ---
 * Fires immediately before the per-frame gameplay-state hash is
 * captured. Per-game impls can use it for ad-hoc state dumps that
 * need to be aligned with the hash log's exact timing. */
void game_on_hash_emit(uint32_t frame);

/* --- Save-state resync hook ---
 * Called from core's save-state loader AFTER cpu/RAM/scratch have been
 * restored, BEFORE the game fiber resumes. Per-game impl resyncs any
 * host-side state that mirrors RAM-resident game state — for Spyro that
 * means re-pointing the active-overlay dispatcher at whichever overlay
 * id is currently in RAM at the well-known marker word. Without this
 * the dispatcher keeps routing into the previous level's overlay and
 * the game hangs on the next overlay-resident call. */
void game_on_state_loaded(CPUState* cpu);

/* --- Game-side extra savestate payload ---
 * Small per-game host state that must travel with the state blob but is
 * not guest RAM (e.g. Spyro's WAD-overlay append cursor — a host-side
 * disc cursor the guest's next mod-asset read depends on). Fixed size per
 * game build; core writes/reads it as an opaque section. `size` may be 0
 * (no extra state). Load is called AFTER game_on_state_loaded. */
uint32_t game_state_extra_size(void);
void     game_state_extra_save(void* dst);            /* dst has extra_size bytes */
void     game_state_extra_load(const void* src, uint32_t size);

/* 1 iff gameplay is live and steady-state (in-level, not booting/loading/
 * title): the gate for the in-memory snapshot users (rewind keyframes,
 * runahead, the --state-stress harnesses) — the mem snapshot excludes
 * sections that only mutate outside this state (VRAM, SPU sample RAM). */
int game_is_gameplay_live(CPUState* cpu);

/* --- Position probe (for the BK2 determinism harness) ---
 * Returns 0 and writes the player's world position into [out_x, out_y,
 * out_z] (engine units, signed) when game state is meaningful. Returns
 * non-zero to signal "not yet ready" (e.g. before BSS init runs over the
 * struct's RAM). The host loop calls this each yield post-fast-boot
 * when --position-log is on. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z);

/* --- CLI overrides forwarded by core ---
 * Called once before the run loop starts. Per-game impl decides where /
 * when to apply the value (typically from game_on_frame, since the
 * relevant globals don't exist until the game's own init has run). */
void game_set_skip_to_level(int level_id); /* 0 = disabled */

/* Resolve a --skip-to-level argument to a level id. Accepts the game's
 * numeric ids verbatim ("11") and per-game lower_snake_case level names
 * ("stone_hill", "doctor_shemp"). Returns the level id, or -1 if the
 * name is unknown (impl prints the valid names to stderr — this runs in
 * CLI-parse context). */
int game_resolve_level_arg(const char* arg);

/* Validate a --cheats spec ("99_lives,unlock_all_levels") against the game's
 * own built-in cheat codes. Returns 0 when every name is known, -1 otherwise
 * (impl prints the valid names to stderr — this runs in CLI-parse context, so
 * a typo fails before the game boots). The cheats themselves are applied by
 * the per-game feature module from opts->cheats. */
int game_validate_cheats_arg(const char* spec);

/* --no-lod setup is now a feature module — see
 * core/features/no_lod.h + games/<game>/features/no_lod.c. */

/* --- Disc layout (each game's WAD/asset structure differs) --- */
typedef struct {
    uint32_t wad_sector;       /* sector 37 for Spyro 1 */
    uint32_t wad_header_addr;  /* PSX address where header gets loaded */
} game_disc_layout_t;
const game_disc_layout_t* game_get_disc_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_EXTRAS_H */
