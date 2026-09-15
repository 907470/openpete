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
 * n_sregs of s0..s3 stored at 16, 20, 24, 28. The saved registers are
 * consecutive from s0 at a fixed offset in every observed frame, so a count is
 * enough and no register id has to be encoded. */
typedef struct {
    uint32_t frame_size;
    uint32_t ra_off;
    uint8_t  n_sregs;
} psyq_game_frame_t;

/* Fill `out` and return 1 if this game mirrors that site; 0 to mirror nothing. */
int psyq_game_stack_frame(psyq_frame_site_t site, psyq_game_frame_t* out);

/* --- PSYQ-internal RAM mirrors ----------------------------------------- */

/* Each writes one PSYQ library's own structures at this game's addresses. */
void psyq_game_mirror_main(CPUState* cpu);
void psyq_game_mirror_resetgraph(CPUState* cpu, int mode);
void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb);
void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0, uint32_t buf1);
void psyq_game_mirror_pad_slot2(CPUState* cpu);
void psyq_game_mirror_memcard_init(CPUState* cpu);

/* PSYQ's rand seed lives in the library's own data. 0 means this game has no
 * seed slot located yet, and the LCG then runs on host-side state: still
 * correct as a generator, just not observable in guest RAM. */
uint32_t psyq_game_rand_seed_addr(void);

/* Root counter 2's count as the game reads it at boot (0x1F801120, an
 * `lhu`) to seed rand: the value the hardware reference produced on the
 * captured boot, 0 for a game that never reads it. */
uint16_t psyq_game_root_counter2(void);

/* --- Game feature couplings -------------------------------------------- */

/* The vaddr every overlay of this game loads at (game_identity.c, from
 * recomp.conf OVERLAY_LOAD_ADDR). After a disc read lands there, the first
 * word is the overlay id and the dispatcher is re-pointed at that overlay's
 * translated code. */
uint32_t psyq_game_overlay_vaddr(void);

/* libgpu's own environment block: 128 bytes holding the GPU type, the VRAM
 * size, a copy of the current DRAWENV at +0x10 (0x5C bytes) and of the current
 * DISPENV at +0x6C (0x14 bytes), which PutDrawEnv and PutDispEnv refresh. 0
 * when the game's block is not located. */
uint32_t psyq_game_libgpu_env_addr(void);

/* libgpu's fill-packet block, the 13 words its ClearImage builds and sends
 * (tag, draw-area and fill primitives, restore of the current draw area). 0
 * when not located. */
uint32_t psyq_game_libgpu_clear_block_addr(void);

/* libcd's state block (game_identity.c, from recomp.conf LIBCD_BLOCK_ADDR):
 * status byte at +0xC, the last CdlSetloc position at +0x18 (4 bytes), the
 * mode byte at +0x1C and the last command at +0x1D, which the library's
 * CdStatus, CdLastPos, CdMode and CdLastCom read. */
uint32_t psyq_game_libcd_block_addr(void);

/* Asset overlay: a game may redirect a disc read at its own granularity.
 * setloc takes the CdlLOC the game passed to CdControl; apply is handed the
 * landed sector buffer so the game can substitute its own bytes. */
void psyq_game_cd_setloc(const void* cdl_loc);
void psyq_game_cd_apply_buf(unsigned char* buf, int sectors);

/* Called after a disc read's completion callback has run, so a game can check
 * its own read-in-flight bookkeeping actually cleared. */
void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb);

/* Level-load delay (openpete_cd_load_delay): every level load costs a fixed
 * number of VBLs of guest time, spent by deferring the completion of the
 * load's reads. The thunk layer opens a budget at the first read of a load
 * and defers each read of that load by up to the cap until the budget is
 * spent.
 *
 * cd_read_in_load: 1 when the read being issued belongs to a level load the
 * game waits on from its main loop, returning to VSync between polls; the
 * deferred completion fires from the VBL, so a read the game spins on in
 * guest code without a VBL never completes under a delay and must answer 0.
 * A load begins at a read answering 1 after one answering 0.
 *
 * cd_read_delay_cap: the most VBLs one read may stay in flight, below the
 * game's own retry timeout (the count after which its loader re-issues the
 * read); 0 = no per-read cap. */
int      psyq_game_cd_read_in_load(CPUState* cpu);
uint32_t psyq_game_cd_read_delay_cap(void);

/* The load window as the oracle sees it. load_window_word: the guest word
 * whose value says whether the game's loader is running (0 = the game has
 * no such word); load_window_active(value): 1 while that value means a
 * load. The reference records every store to the word (the golden's
 * .watch sibling) and openpete samples it at every anchor hit; a loader
 * whose ticks are CPU time (a checksum walk sliced per frame) takes a
 * different number of ticks on each side, so the verifier compares no
 * record inside the window and re-aligns the record index at its end. */
uint32_t psyq_game_load_window_word(void);
int      psyq_game_load_window_active(uint32_t value);

/* The lowest address of code that drives the CD-ROM controller's registers
 * itself (an overlay's disc check); commands issued from below it are
 * libcd's. 0xFFFFFFFF: the game has no such code. */
uint32_t psyq_game_cd_raw_pc_min(void);

/* The call site of a CdSync poll the game makes only after it has seen a
 * data read's completion callback (the not-in-flight branch of its
 * loader's poll). A row of the reference's at this site in the record a
 * completion is deferred to places the callback before the game's flag
 * read of that tick. 0: no such site. */
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

/* Render-path filter and moby ranking consumed by the OT walk. */
int  psyq_game_render_paths_active(void);
int  psyq_game_render_path_addr_visible(uint32_t addr);
void psyq_game_ot_begin(void);
void psyq_game_ot_prim_full(unsigned code_byte, const void* payload,
                            unsigned payload_bytes);
void psyq_game_ot_end(void);

/* Re-derive the resident overlay from the id word at psyq_game_overlay_vaddr()
 * and re-point the dispatcher if it names a different overlay. A game passes
 * this to spyro_overlay_set_resync() at boot when its overlays can be copied
 * into the overlay region rather than read straight into it. */
void psyq_overlay_resync_from_ram(void);

/* Sound-effect name tables, registered with the sfx pack when a game has them.
 * slot_names resolves an SPU sample address back to the game's own slot names
 * and returns how many it wrote; 0 when the game names no sounds. */
void psyq_game_sfx_arm(void);

/* The transfer length that means "a whole new level sound bank", as opposed to
 * a smaller write landing over part of it. A game that has not measured it
 * returns 0, and every transfer is then treated as a partial write. */
unsigned psyq_game_spu_bank_blast_len(void);
int  psyq_game_sfx_slot_names(unsigned spu_addr, const char** out, int max);

#ifdef __cplusplus
}
#endif

#endif
