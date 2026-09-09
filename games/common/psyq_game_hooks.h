// games/common/psyq_game_hooks.h — what the shared PSYQ thunk layer asks of a
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
// Every hook has a weak default that does nothing, so a game supplies only
// what it has. A game with no mirror declared writes no PSX RAM from the thunk
// layer rather than writing another game's addresses.

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

/* How many consecutive VSync query calls (mode < 0) with no intervening wait
 * should pass before the layer yields to the host and lets the VBL counter
 * advance.
 *
 * The counter is yield-driven so that a frame costs a fixed number of yields
 * whatever the wall clock does, which is what makes replay reproducible. A
 * game that waits by calling VSync(0) advances it itself and needs nothing
 * here, so the default is 0 -- never. A game that instead polls VSync(-1)
 * until the count changes would spin forever against a counter only it can
 * advance, and returns a small positive number to break that.
 *
 * Returning non-zero costs determinism in proportion: the counter then also
 * moves for reasons outside the game's own wait calls. */
unsigned psyq_game_vsync_poll_advance(void);

/* PSYQ's rand seed lives in the library's own data. 0 means this game has no
 * seed slot located yet, and the LCG then runs on host-side state: still
 * correct as a generator, just not observable in guest RAM. */
uint32_t psyq_game_rand_seed_addr(void);

/* --- Game feature couplings -------------------------------------------- */

/* The vaddr every overlay of this game loads at, or 0 if it uses none. After a
 * disc read lands there, the first word is the overlay id and the dispatcher
 * is re-pointed at that overlay's translated code. Every game that uses
 * overlays needs this; only the address differs. */
uint32_t psyq_game_overlay_vaddr(void);

/* Asset overlay: a game may redirect a disc read at its own granularity.
 * setloc takes the CdlLOC the game passed to CdControl; apply is handed the
 * landed sector buffer so the game can substitute its own bytes. */
void psyq_game_cd_setloc(const void* cdl_loc);
void psyq_game_cd_apply_buf(unsigned char* buf, int sectors);

/* Called after a disc read's completion callback has run, so a game can check
 * its own read-in-flight bookkeeping actually cleared. */
void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb);

/* Render-path filter and moby ranking consumed by the OT walk. */
int  psyq_game_render_paths_active(void);
int  psyq_game_render_path_addr_visible(uint32_t addr);
void psyq_game_ot_begin(void);
void psyq_game_ot_prim_full(unsigned code_byte, const void* payload,
                            unsigned payload_bytes);
void psyq_game_ot_end(void);

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
