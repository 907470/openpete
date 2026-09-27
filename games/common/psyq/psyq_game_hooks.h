// games/common/psyq/psyq_game_hooks.h — what the shared PSYQ thunk layer asks of a
// game.
//
// The thunk layer translates PSYQ calls into PsyCross for every game. Three
// things in it are not shareable, and each is reached through this interface:
//
//  - PSYQ-internal RAM mirrors. A thunk writes PSYQ's own data structures into
//    PSX RAM because gameplay code reads them back, and the addresses and
//    values are per game AND per PSYQ revision. Spyro 1 links PSYQ 4.0; Spyro
//    2 and 3 link 4.7, so a table of pointers into PSYQ code cannot survive
//    the change and struct layouts need not either.
//  - The saved-register prologue a real PSYQ function leaves on the guest
//    stack. An empty C thunk leaves those slots at zero, which shows up as a
//    RAM difference; the frame is per function and per revision.
//  - Couplings to one game's own features (its asset overlay, its render-path
//    filter, its sfx tables).
//
// Every hook has an inert default (psyq_game_hooks_default.c), so a game
// supplies only what it has; a game with no mirror declared writes no PSX RAM
// from the thunk layer rather than writing another game's addresses. The
// three address-only hooks marked below come from the generated
// game_identity.c instead.

#ifndef OPENPETE_PSYQ_GAME_HOOKS_H
#define OPENPETE_PSYQ_GAME_HOOKS_H

#include "psx_runtime_internal.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- PSYQ prologue mirror ---------------------------------------------- */

/* The sites whose real PSYQ implementation leaves a stack frame behind. */
typedef enum {
    PSYQ_FRAME_MAIN = 0,
    PSYQ_FRAME_RESETGRAPH,
    PSYQ_FRAME_SETDISPMASK,
    PSYQ_FRAME_SETGRAPHDEBUG,
    PSYQ_FRAME_DRAWSYNC,
    PSYQ_FRAME_CLEARIMAGE,
    PSYQ_FRAME_PUTDISPENV,
    PSYQ_FRAME_PUTDRAWENV,
    PSYQ_FRAME_SETDEFDRAWENV,
    PSYQ_FRAME_MEMCARDINIT,
    PSYQ_FRAME__COUNT
} psyq_frame_site_t;

/* A PSYQ prologue: `sp -= frame_size`, `sw ra, ra_off(sp)`, and the first
 * n_sregs of s0..s4 stored at 16, 20, 24, 28, 32. The saved registers are
 * consecutive from s0 at a fixed offset in every observed frame, so a count is
 * enough and no register id has to be encoded. */
typedef struct {
    uint32_t frame_size;
    uint32_t ra_off;
    uint8_t  n_sregs;
} psyq_game_frame_t;

/** @brief Describe the stack frame a real PSYQ function leaves at a site, for the thunk to replay.
 *
 * psyq_mirror_stack_frame writes ra at ra_off and the first n_sregs saved
 * registers from s0 at offset 16 upward into a frame of frame_size bytes
 * below the current sp, then leaves sp unchanged, so the empty C thunk
 * leaves the RAM bytes the PSYQ prologue would.
 * @param site The PSYQ function whose prologue is replayed.
 * @param out Receives the frame descriptor when the return is 1.
 * @return 1 when this game mirrors that site, 0 to mirror nothing. */
int psyq_game_stack_frame(psyq_frame_site_t site, psyq_game_frame_t* out);

/* --- PSYQ-internal RAM mirrors ----------------------------------------- */

/* Each mirror writes one PSYQ library's own structures at this game's
 * addresses. */
/** @brief Write the RAM effects of PSYQ's __main other than its stack frame.
 *
 * psyq___main calls it before replaying __main's frame. Spyro 1 sets the
 * library's initialized flag at 0x800730BC to 1 when it reads 0; the
 * default writes nothing.
 * @param cpu Guest CPU state. */
void psyq_game_mirror_main(CPUState* cpu);
void psyq_game_mirror_resetgraph(CPUState* cpu, int mode);
void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb);
void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0, uint32_t buf1);
void psyq_game_mirror_pad_slot2(CPUState* cpu);
void psyq_game_mirror_memcard_init(CPUState* cpu);

/** @brief Return the address of PSYQ's rand seed word in the library's own data.
 *
 * psyq_rand loads and stores the LCG state there. When the address is 0 the
 * LCG runs on host-side state instead: the generator's output is the same,
 * but the seed is not visible in guest RAM.
 * @return The seed word's address, or 0 when this game's seed slot is not
 *     located. */
uint32_t psyq_game_rand_seed_addr(void);

/** @brief Return root counter 2's count as the game's boot reads it (0x1F801120, an `lhu`) to seed rand.
 *
 * The value is the one the hardware reference produced on the captured
 * boot; game_main passes it to openpete_psx_set_root_counter2 before the
 * run.
 * @return The count, or 0 for a game that never reads it. */
uint16_t psyq_game_root_counter2(void);

/* --- Game feature couplings -------------------------------------------- */

/* The vaddr every overlay of this game loads at (game_identity.c, from
 * recomp.conf OVERLAY_LOAD_ADDR). After a disc read lands there, the first
 * word is the overlay id and the dispatcher is re-pointed at that overlay's
 * translated code. */
uint32_t psyq_game_overlay_vaddr(void);

/** @brief Return the address of libgpu's 128-byte environment block.
 *
 * The block holds the GPU type, the VRAM size, a copy of the current
 * DRAWENV at +0x10 (0x5C bytes) and of the current DISPENV at +0x6C (0x14
 * bytes). The PutDrawEnv and PutDispEnv thunks refresh the copies, and the
 * ClearImage mirror reads the VRAM size from it.
 * @return The block's address, or 0 when the game's block is not located. */
uint32_t psyq_game_libgpu_env_addr(void);

/** @brief Return the address of libgpu's fill-packet block, the 13 words its ClearImage builds and sends.
 *
 * The words are the tag, the draw-area and fill primitives, and the restore
 * of the current draw area. The ClearImage mirror writes nothing unless both
 * this block and psyq_game_libgpu_env_addr() are located.
 * @return The block's address, or 0 when not located. */
uint32_t psyq_game_libgpu_clear_block_addr(void);

/* libcd's state block (game_identity.c, from recomp.conf LIBCD_BLOCK_ADDR):
 * status byte at +0xC, the last CdlSetloc position at +0x18 (4 bytes), the
 * mode byte at +0x1C and the last command at +0x1D, which the library's
 * CdStatus, CdLastPos, CdMode and CdLastCom read. */
uint32_t psyq_game_libcd_block_addr(void);

/* Asset overlay: a game may redirect a disc read at its own granularity. */
/** @brief Receive the position of every CdlSetloc, so the game's asset overlay knows where the next read starts.
 *
 * cd_command_host calls it from every CdControl entry for a CdlSetloc with
 * a parameter, since CdRead carries no location. Spyro 1 converts it to a
 * sector and passes it to mod_wad_setloc.
 * @param cdl_loc The CdlLOC the game passed to CdControl, as a host pointer. */
void psyq_game_cd_setloc(const void* cdl_loc);
/** @brief Let the game substitute its own bytes in a data read's landed sector buffer.
 *
 * psyq_CdRead calls it after the read has drained and before the game's
 * loader sees the bytes. Spyro 1 passes it to mod_wad_apply.
 * @param buf The landed sectors, as a host pointer.
 * @param sectors Sector count of the read. */
void psyq_game_cd_apply_buf(unsigned char* buf, int sectors);

/** @brief Let the game check its read-in-flight bookkeeping after a data read's completion callback has run.
 *
 * Spyro 1 logs a warning when g_CdState.m_IsReading is still set.
 * @param cpu Guest CPU state.
 * @param read_cb The guest completion callback registered through the
 *        thunk, 0 when none. */
void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb);

/* Level-load delay (openpete_cd_load_delay): every level load costs a fixed
 * number of VBLs of guest time, spent by deferring the completion of the
 * load's reads. The thunk layer opens a budget at the first read of a load
 * and defers each read of that load by up to the cap until the budget is
 * spent. */
/** @brief Classify the data read being issued as part of a level load the delay budget applies to.
 *
 * A read belongs to a load when the game waits on it from its main loop,
 * returning to VSync between polls. The deferred completion fires from the
 * VBL, so a read the game spins on in guest code without a VBL never
 * completes under a delay and must answer 0. A load begins at a read
 * answering 1 after one answering 0.
 * @param cpu Guest CPU state.
 * @return 1 when the read is part of a level load, else 0. */
int      psyq_game_cd_read_in_load(CPUState* cpu);
/** @brief Return the most VBLs one read of a level load may stay in flight.
 *
 * The cap stays below the game's own retry timeout, the count after which
 * its loader re-issues the read.
 * @return The cap in VBLs, or 0 for no per-read cap. */
uint32_t psyq_game_cd_read_delay_cap(void);

/* The load window as the oracle sees it. The reference records every store
 * to the game's loader-state word (the golden's .watch sibling) and
 * openpete samples it at every anchor hit; a loader whose ticks are CPU time
 * (a checksum walk sliced per tick) takes a different number of ticks on
 * each side, so the verifier compares no record inside the window and
 * re-aligns the record index at its end. */
/** @brief Return the address of the guest word whose value says whether the game's loader is running.
 *
 * psyq_sched_anchor reads it at every anchor hit and passes the value to
 * openpete_sched_anchor_window.
 * @return The word's address, or 0 when the game has no such word. */
uint32_t psyq_game_load_window_word(void);
/** @brief Whether a value of the loader-state word means a load is running.
 *
 * @param value A value of the word psyq_game_load_window_word() names.
 * @return 1 while the value means a load, else 0. */
int      psyq_game_load_window_active(uint32_t value);

/** @brief Return the lowest address of code that drives the CD-ROM controller's registers itself (an overlay's disc check).
 *
 * The CD-ROM MMIO model treats a command issued from below it as libcd's.
 * The VBL body passes it to openpete_cdrom_mmio_set_raw_pc_min once.
 * @return The address, or 0xFFFFFFFF when the game has no such code. */
uint32_t psyq_game_cd_raw_pc_min(void);

/** @brief Return the return address of a CdSync poll the game makes only after it has seen a data read's completion callback.
 *
 * The poll is the not-in-flight branch of the game's loader poll. A row of
 * the reference's at this site, in the record a completion is deferred to,
 * places the callback before the game's flag read of that tick, and the
 * deferred completion is delivered after the call that row answers.
 * @return The return address, or 0 when the game has no such site. */
uint32_t psyq_game_cd_done_witness_ra(void);

/* Music route for a streaming read issued under an XA mode. The shared body
 * anchors the virtual head, calls this, and then starts a custom song by path,
 * a WAV override by track, or the XA decoder at the sector, in that order.
 * `track` is the game's own music index (-1: none, so no override by track);
 * `custom_path` is a WAV a mod mapped to the current level (NULL: none). Any
 * guest RAM a game writes here for a custom song runs during runahead
 * speculation too and rolls back with the tick. */
typedef struct {
    int         track;
    const char* custom_path;
} psyq_music_route_t;
void psyq_game_music_route(int sector, psyq_music_route_t* out);

/* Address of libcd's read-completion callback slot in PSX RAM
 * (game_identity.c, from recomp.conf CD_READ_CB_ADDR), for a game that links
 * libcd's own CdReadCallback rather than reaching the thunk: the registration
 * stores the guest handler there and never reaches psyq_CdRead's tracked
 * callback. 0 when the game registers through the thunk. */
uint32_t psyq_game_cd_read_cb_addr(void);

/* Render-path filter consumed by the OT walk. The two
 * render-path hooks are the one exception to the per-game rule above: the
 * isolation registry they forward to is shared code linked into every game
 * (renderer/render_paths.c), which defines them there. They take no
 * psyq_game_provides.h entry and have no inert default, so a game cannot link
 * the registry without arming the OT filter. */
/** @brief Reports whether a --render-paths selection is in effect; forwards render_paths_active.
 *
 * @return Nonzero when the OT walk filters geometry prims by path. */
int  psyq_game_render_paths_active(void);
int  psyq_game_render_path_addr_visible(uint32_t addr);

/** @brief Re-derive the resident overlay from the id word at psyq_game_overlay_vaddr() and re-point the dispatcher if it names a different overlay.
 *
 * Does nothing when the game has no overlay vaddr or guest RAM is not
 * mapped. A game passes this to spyro_overlay_set_resync() at boot when its
 * overlays can be copied into the overlay region rather than read straight
 * into it. */
void psyq_overlay_resync_from_ram(void);

/** @brief Register the game's sound-effect tables with the sfx pack and the mod layer.
 *
 * The SpuInit and SpuWrite thunks call it on every entry, so it must be
 * idempotent. Spyro 1 installs its pitch reader and slot-name resolver in
 * the sfx pack once and sets its sound-name canonicaliser (mod-facing alias
 * to slot name) in the mod layer on every call; the default does nothing. */
void psyq_game_sfx_arm(void);

/** @brief Return the SPU transfer length that means a whole new level sound bank, as opposed to a smaller write landing over part of it.
 *
 * A SpuWrite of this length bumps the sfx pack's generation and stops
 * orphaned voices; any other length invalidates only the range it covers.
 * @return The length in bytes, or 0 when the game has not measured it, in
 *         which case every transfer is a partial write. */
unsigned psyq_game_spu_bank_blast_len(void);
/** @brief Resolve an SPU sample address to the game's own sound-effect slot names.
 *
 * @param spu_addr SPU RAM address of the sample.
 * @param out Receives up to `max` name pointers.
 * @param max Capacity of `out`.
 * @return How many names were written; 0 when the game names no sounds. */
int  psyq_game_sfx_slot_names(unsigned spu_addr, const char** out, int max);

#ifdef __cplusplus
}
#endif

#endif
