// core/include/game_extras.h - the per-game abstraction.
//
// Every behavior that differs between Spyro 1, 2, 3 (identity, per-tick
// hooks, savestate payload, CLI targets, disc layout) is declared here. Each entry is a
// core forwarder (core/runtime/game_runtime.c) over the registered runtime
// descriptor (openpete_game_runtime.h); a game fills the descriptor from
// its games/<game>/game_extras_impl.c and, for the identity group, from the
// TU the build generates from `games/<game>/recomp.conf`. An entry whose
// descriptor field is NULL returns the default its comment names.
//
// Rule: nothing in core/ may reference a game-specific address, symbol,
// or struct. If `grep -r "0x800" core/` returns a hit, it's a bug.

#ifndef OPENPETE_GAME_EXTRAS_H
#define OPENPETE_GAME_EXTRAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward decl - the recompiled CPU state (psx_runtime.h).
struct CPUState;
typedef struct CPUState CPUState;

/* --- Identity ---
 * The descriptor's identity, generated as game_identity.c
 * (tools/gen_game_identity.py over games/<id>/recomp.conf), not written in
 * the game's hand-written sources. */
/** @brief Return the registered game's executable file name on its disc, such as "SCUS_942.28".
 *
 * @return The name; "" before registration. */
const char* game_get_exe_filename(void);
/** @brief Return the registered game's display name, such as "Spyro the Dragon".
 *
 * @return The name; "" before registration. */
const char* game_get_display_name(void);
/** @brief Return the registered game's id, such as "spyro-1", which keys its [game.<id>] config scope.
 *
 * @return The id; "" before registration. */
const char* game_get_id(void);

/** @brief Guest address of the function through which this game reads its pad buffer.
 *
 * The read decides which host input sample a tick plays. openpete_run arms
 * the pacing pad-read probe at this address unless OPENPETE_PAD_READ_PC
 * overrides it. A return of 0 arms no probe, and the pacing census reports
 * the pad-read phase as unmeasured rather than as zero reads.
 * @return Guest function address, or 0 when the read site is unknown. */
uint32_t    game_get_pad_read_pc(void);

/** @brief Returns the VBLs of guest time every level load waits when --load-delay is unset.
 *
 * openpete_run reads it once per run into openpete_cd_load_delay. A game
 * whose loader depends on disc reads staying in flight across its main-loop
 * iterations (a checksum walk that only advances while a read is pending)
 * needs a floor here.
 * @return The delay in VBLs; 0 = instant loads. */
uint32_t    game_get_load_delay_default(void);

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

/* --- Per-tick callbacks --- */
/** @brief Runs the game's per-tick host hooks immediately before the game fiber runs a tick.
 *
 * The host loop calls it before every tick it hands to the game fiber: the
 * committed tick, each runahead speculative tick, and each tick a rewind
 * re-executes from a keyframe, so its guest writes are part of the timeline.
 * Implementations drive the skip targets, the level warp, the free camera's
 * scripted placement and engage, and the other per-tick feature hooks; the
 * Spyro 1 and 3 implementations also build the host-drawn text overlays.
 * It first latches the live settings the producers read
 * (openpete_native_producer_knobs_latch).
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter before the tick. */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter);
/** @brief Runs after the game fiber yields back, on host-loop iterations that ran a tick.
 *
 * Called before the runahead speculation of the iteration. No game fills
 * the descriptor's on_frame, so it does nothing.
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter after the tick. */
void game_on_frame(CPUState* cpu, uint32_t frame_counter);

/* --- Hash-emit hook (diagnostic) --- */
/** @brief Runs immediately before the gameplay-state hash log captures a tick.
 *
 * The host loop calls it on each ticked iteration once fast boot has ended,
 * while the gameplay hash log is open, so a game can dump state aligned
 * with the hash log's timing. No game fills the descriptor's on_hash_emit,
 * so it does nothing.
 * @param frame The openpete_frame counter. */
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
 * on, the active-overlay id, and the level-warp state machine. Core
 * appends the per-mod save-record block as a trailer (block, length,
 * magic) and strips it before the game's loader sees the payload. The
 * size is fixed per process once mods init ran, except that on a game
 * whose card writes complete asynchronously the trailer also carries the
 * pending card writes and grows while one is in flight; core writes and reads
 * the payload as an opaque section through game_state_extra_save and
 * game_state_extra_load. The file loader calls game_state_extra_load after
 * game_on_state_loaded, the in-memory loader before it.
 * @return Payload size in bytes. */
uint32_t game_state_extra_size(void);

/** @brief Write the game's host-side savestate payload.
 *
 * Spyro 2 and 3 write the shared thunk layer's latch pack
 * (psyq_thunk_state_extra_save); a descriptor without the hook writes
 * nothing.
 * @param dst Receives game_state_extra_size() bytes. */
void     game_state_extra_save(void* dst);

/** @brief Restore the game's host-side state from a payload game_state_extra_save wrote.
 *
 * The caller passes the size stored in the savestate header. Spyro 2 and
 * 3 hand the payload to the thunk layer's latch-pack loader, which leaves
 * state unchanged for a payload it does not recognize; a descriptor
 * without the hook ignores the payload.
 * @param src The payload.
 * @param size The payload's length in bytes. */
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

/** @brief Read the player's position, facing and level id while a level is live, placeable or not.
 *
 * The reader under game_get_player_pose, without its placeable gate, so
 * the attract demo and the fly-in report a pose. Reads only. Nothing is
 * written on a negative return.
 * @param cpu The guest CPU state.
 * @param out_x Receives x in world units.
 * @param out_y Receives y in world units.
 * @param out_z Receives z in world units.
 * @param out_yaw Receives the 12-bit facing, or -1 when the game has none
 *        located.
 * @param out_level Receives the level id, or -1 when none located.
 * @return 1 when the player is placeable, 0 when live but not placeable,
 *         -1 when no level is live. */
int game_read_player_pose(CPUState* cpu, int32_t* out_x, int32_t* out_y, int32_t* out_z,
                          int32_t* out_yaw, int32_t* out_level);

/* --- CLI overrides forwarded by core --- */
/** @brief Arms a skip into a level through the game's own title-screen transition.
 *
 * openpete_run calls it once before the run loop with --skip-to-level's
 * value; the level warp's title route and, on Spyro 1, the mod API's level
 * entry at the title screen call it at runtime. The implementation applies
 * the value from its per-tick hook, since the globals it writes are set up
 * by the game's own init.
 * @param level_id The target level id; 0 disables the skip. */
void game_set_skip_to_level(int level_id); /* 0 = disabled */

/** @brief Resolves a --skip-to-level argument to a level id.
 *
 * Accepts the game's numeric ids ("11") and its lower_snake_case level names
 * ("stone_hill"). Runs in CLI-parse context: on failure the implementation
 * prints the valid names to stderr, and an empty argument (`--skip-to-level`
 * with no value) asks for that list.
 * @param arg The argument.
 * @return The level id, or -1 for an unknown or empty argument. */
int game_resolve_level_arg(const char* arg);

/** @brief Arms a --skip-to-cutscene target.
 *
 * Called after game_set_skip_to_level, so the implementation can arm a level
 * skip to the level the target is entered from when none is armed, and
 * refuse a target whose source level contradicts an explicit level skip.
 * @param cutscene_id The game's cutscene id; 0 or less disables it. */
void game_set_skip_to_cutscene(int cutscene_id);

/** @brief Resolves a --skip-to-cutscene argument to a cutscene id.
 *
 * Follows game_resolve_level_arg: an empty argument asks for the list of
 * valid names, and a game without cutscene targets (no resolve_cutscene_arg
 * in its descriptor) says so.
 * @param arg The argument.
 * @return The cutscene id, or -1 after printing the valid names. */
int game_resolve_cutscene_arg(const char* arg);

/** @brief Resolves a --skip-to-area LEVEL:AREA argument.
 *
 * A game without sub-area targets (no resolve_area_arg in its descriptor)
 * prints that it has none.
 * @param arg The argument.
 * @param out_area Receives the sub-area index.
 * @return The level id, or -1 after printing the valid targets for an
 *     empty, unknown or unreachable argument. */
int game_resolve_area_arg(const char* arg, int* out_area);

/** @brief Arms --skip-to-area: the level's skip, then the sub-area entry.
 *
 * Called after game_set_skip_to_level, and replaces its level. A game
 * without sub-area targets ignores it.
 * @param level_id The level id; 0 disables it.
 * @param area The sub-area index. */
void game_set_skip_to_area(int level_id, int area);

/** @brief Validates a --cheats spec against the game's own built-in cheat codes.
 *
 * The spec is comma-separated names ("99_lives,unlock_all_levels"). This
 * runs in CLI-parse context, so a typo fails before the game boots; on
 * failure the implementation prints the valid names to stderr. A game
 * without --cheats support prints that it has no built-in cheats and
 * rejects every spec. The cheats themselves are
 * applied by the per-game feature module from opts->cheats.
 * @param spec The --cheats argument.
 * @return 0 when the spec names at least one cheat and every name is known,
 *         else -1. */
int game_validate_cheats_arg(const char* spec);

struct openpete_surface_sink;
/** @brief Report the game's feature rows and flag vocabularies to the surface dump.
 *
 * Emits, from the features' static tables and without running any
 * feature's apply_options: the registry rows the features register (cheat
 * buttons, the level-warp rows) through openpete_surface_row, and the
 * level, cutscene, area and cheat names the parser accepts through
 * openpete_surface_vocab.
 * @param sink The dump's collector. */
void game_surface(struct openpete_surface_sink* sink);

/* --no-lod setup is a feature module - see core/include/no_lod.h and
 * core/renderer/no_lod.c. */

/* --- Disc layout (each game's WAD/asset structure differs) --- */
typedef struct {
    uint32_t wad_sector;       /* sector 37 for Spyro 1 */
    uint32_t wad_header_addr;  /* PSX address where header gets loaded */
} game_disc_layout_t;
/** @brief Returns where the game's WAD lives on the disc, or NULL for a game without a WAD overlay.
 *
 * The Spyro 1 layout is a static object; the Spyro 2 and 3 implementations
 * return NULL.
 * @return Pointer to the layout, valid for the process lifetime, or NULL. */
const game_disc_layout_t* game_get_disc_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_EXTRAS_H */
