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

/* --- Per-frame callbacks (rare; e.g., DMA timing hacks).
 * `game_on_frame_pre`  fires immediately BEFORE handing control to the
 *                      game fiber for a tick — use it to mutate state
 *                      that the upcoming tick will read.
 * `game_on_frame_post` fires AFTER the tick yields back, only on iters
 *                      where the frame counter advanced. */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter);
void game_on_frame(CPUState* cpu, uint32_t frame_counter);

/* --- Save-state resync hook ---
 * Called from core's save-state loader AFTER cpu/RAM/scratch have been
 * restored, BEFORE the game fiber resumes. Per-game impl resyncs any
 * host-side state that mirrors RAM-resident game state — for Spyro that
 * means re-pointing the active-overlay dispatcher at whichever overlay
 * id is currently in RAM at the well-known marker word. Without this
 * the dispatcher keeps routing into the previous level's overlay and
 * the game hangs on the next overlay-resident call. */
void game_on_state_loaded(CPUState* cpu);

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
