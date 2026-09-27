// core/include/game_extras.h — the per-game abstraction.
//
// Every behavior that differs between Spyro 1, 2, 3 (identity, overlay
// handling, runtime quirks, per-tick hooks, disc layout) is declared here and
// implemented by the per-game `games/<game>/game_extras_impl.c`, except the
// identity group, which the build generates from `games/<game>/recomp.conf`.
//
// Rule: nothing in core/ may reference a game-specific address, symbol,
// or struct. If `grep -r "0x800" core/` returns a hit, it's a bug.

#ifndef OPENPETE_GAME_EXTRAS_H
#define OPENPETE_GAME_EXTRAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward decl — the recompiled CPU state (psx_runtime.h).
struct CPUState;
typedef struct CPUState CPUState;

/* --- Identity ---
 * Defined by the generated game_identity.c (tools/gen_game_identity.py over
 * games/<id>/recomp.conf), not by the game's hand-written sources. */
const char* game_get_exe_filename(void);      /* e.g. "SCUS_942.28" */
const char* game_get_display_name(void);      /* "Spyro the Dragon" */
const char* game_get_id(void);                /* "spyro-1" — the [game.<id>] config scope */

/** @brief Guest address of the function through which this game reads its pad buffer.
 *
 * The read decides which host input sample a tick plays. openpete_run arms
 * the pacing pad-read probe at this address unless OPENPETE_PAD_READ_PC
 * overrides it. A return of 0 arms no probe, and the pacing census reports
 * the pad-read phase as unmeasured rather than as zero reads.
 * @return Guest function address, or 0 when the read site is unknown. */
uint32_t    game_get_pad_read_pc(void);

/* VBLs of guest time every level load waits when --load-delay is unset: the
 * game's shipped load duration. A game whose loader depends on disc reads
 * staying in flight across its main-loop frames (a checksum walk that only
 * advances while a read is pending) needs a floor here; 0 = instant. */
uint32_t    game_get_load_delay_default(void);

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

/* --- Post-boot, pre-execution --- */
/** @brief Run the game's one-time boot setup once the EXE image is in guest RAM, before any guest instruction runs.
 *
 * The host loop calls it after the EXE image and the host-side seed blocks
 * are in guest RAM and before the game fiber is created, so no guest
 * instruction has run and no --state-load-on-frame restore has happened.
 * A game uses it to snapshot EXE .data the engine overwrites later (a
 * snapshot taken later can capture a value the engine itself wrote, and a
 * savestate load can replace the RAM it would have read), to register the
 * overlay-resync callback, and to install the write census.
 * @param cpu Guest CPU state. */
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

/* --- Save-state resync hook --- */
/** @brief Resync host-side state that mirrors guest RAM after a savestate restore.
 *
 * Both savestate loaders call it after CPU, RAM and scratch are restored
 * and before the game fiber resumes: the file loader before
 * game_state_extra_load, the in-memory loader after it, and the in-memory
 * loader skips it on a quiet restore (runahead's per-tick rollback). The
 * Spyro implementations re-point the active-overlay dispatcher at the
 * overlay whose id word is resident in the restored RAM; without that the
 * dispatcher routes overlay-window calls into the previous overlay's
 * translated code.
 * @param cpu Guest CPU state. */
void game_on_state_loaded(CPUState* cpu);

/* --- Game-side extra savestate payload --- */
/** @brief Return the byte size of the game's host-side savestate payload.
 *
 * The payload is per-game host state that travels with every snapshot but
 * is not guest RAM: the shared thunk layer's latch pack, and on Spyro 1
 * also the WAD-overlay disc cursor the guest's next mod-asset read depends
 * on. The size is fixed per game build and may be 0; core writes and reads
 * the payload as an opaque section through game_state_extra_save and
 * game_state_extra_load. The file loader calls game_state_extra_load after
 * game_on_state_loaded, the in-memory loader before it.
 * @return Payload size in bytes. */
uint32_t game_state_extra_size(void);
void     game_state_extra_save(void* dst);            /* dst has extra_size bytes */
void     game_state_extra_load(const void* src, uint32_t size);

/** @brief Whether gameplay is live and steady-state: in a level, not booting, loading or on the title.
 *
 * Gates the in-memory snapshot users (rewind keyframes, runahead, the
 * --state-stress rollback cycle), because the memory snapshot excludes the
 * sections that mutate only outside this state (VRAM, SPU sample RAM). The
 * debug free camera uses it as its default level-live test.
 * @param cpu Guest CPU state.
 * @return 1 when gameplay is live, else 0. */
int game_is_gameplay_live(CPUState* cpu);

/* --- Position probe (for the BK2 determinism harness) --- */
/** @brief Read the player's world position for the --position-log probe.
 *
 * The position is the first three int32 of the player state block, at
 * game_layout.player.base + pos_off. The host loop calls it after each
 * ticked iteration once fast boot has ended, since the block holds garbage
 * before the EXE's BSS init.
 * @param out_x Receives x in world units.
 * @param out_y Receives y in world units.
 * @param out_z Receives z in world units.
 * @return 0 on success; non-zero, writing nothing, when guest RAM is absent
 *         or the game has not located the player block. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z);

/* --- Player pose (the exit pose line, --player-pose's paste-back) --- */
/** @brief Read the player's position, facing and level id while the player is placeable.
 *
 * Placeable means gameplay live, the player under pad control, and the
 * camera in its ordinary follow. The shared body in
 * games/common/freecam_game.c reads the addresses of the game's freecam
 * descriptor. Nothing is written on a non-zero return.
 * @param cpu The guest CPU state.
 * @param out_x Receives x in world units.
 * @param out_y Receives y in world units.
 * @param out_z Receives z in world units.
 * @param out_yaw Receives the 12-bit facing (4096 per turn), or -1 when the
 *        game has none located.
 * @param out_level Receives the level id, or -1 when none located.
 * @return 0 on success; -1 when no level is live; -2 when live but not
 *         placeable (attract demo, fly-in or exit sequence). */
int game_get_player_pose(CPUState* cpu, int32_t* out_x, int32_t* out_y, int32_t* out_z,
                         int32_t* out_yaw, int32_t* out_level);

/* --- CLI overrides forwarded by core ---
 * Called once before the run loop starts. Per-game impl decides where /
 * when to apply the value (typically from game_on_frame, since the
 * relevant globals don't exist until the game's own init has run). */
void game_set_skip_to_level(int level_id); /* 0 = disabled */

/* Resolve a --skip-to-level argument to a level id. Accepts the game's
 * numeric ids verbatim ("11") and per-game lower_snake_case level names
 * ("stone_hill", "doctor_shemp"). Returns the level id, or -1 if the
 * name is unknown or empty (impl prints the valid names to stderr — this
 * runs in CLI-parse context; an empty arg is `--skip-to-level` with no
 * value, asking for the list). */
int game_resolve_level_arg(const char* arg);

/* --skip-to-cutscene: a target the game enters from a live level rather
 * than through the title screen (Spyro 1: credits / credits_100). Called
 * after game_set_skip_to_level, so the impl can default the level it
 * enters from when no level skip is armed. 0 = disabled. */
void game_set_skip_to_cutscene(int cutscene_id);

/* Resolve a --skip-to-cutscene argument as game_resolve_level_arg does a
 * level: the id, or -1 with the valid names printed (an empty arg asks
 * for the list; a game without cutscene targets says so). */
int game_resolve_cutscene_arg(const char* arg);

/* --skip-to-area <level>:<area>: resolve the argument to the level id and,
 * through *out_area, the sub-area index. Returns -1 with the valid targets
 * printed for an empty, unknown or unreachable argument; a game without
 * sub-area targets says so. */
int game_resolve_area_arg(const char* arg, int* out_area);

/* Arm --skip-to-area: the level's skip, then the sub-area entry. Called
 * after game_set_skip_to_level and replaces its level. level 0 = disabled. */
void game_set_skip_to_area(int level_id, int area);

/** @brief Validates a --cheats spec against the game's own built-in cheat codes.
 *
 * The spec is comma-separated names ("99_lives,unlock_all_levels"). This
 * runs in CLI-parse context, so a typo fails before the game boots; on
 * failure the implementation prints the valid names to stderr. A game
 * without --cheats support rejects every spec. The cheats themselves are
 * applied by the per-game feature module from opts->cheats.
 * @param spec The --cheats argument.
 * @return 0 when the spec names at least one cheat and every name is known,
 *         else -1. */
int game_validate_cheats_arg(const char* spec);

/* --no-lod setup is a feature module — see core/include/no_lod.h and
 * core/renderer/no_lod.c. */

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
