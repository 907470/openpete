// games/common/psyq/psyq_game_hooks.h - what the shared PSYQ thunk layer asks of a
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
// A game supplies what it has as one psyq_game_hooks_t, carried by its
// runtime descriptor (openpete_game_runtime_t.psyq). Every psyq_game_* entry
// below is a forwarder over that descriptor (psyq_game_hooks.c) with an inert
// default for a field the game leaves 0 or NULL, so a game with no mirror
// declared writes no PSX RAM from the thunk layer rather than writing another
// game's addresses. The recomp.conf addresses come from the generated
// game_identity.c and the abort-stub address table from the build's stub
// filter (tools/filter_psyq_stubs.py).

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
/** @brief Write the libgpu data that PSYQ ResetGraph leaves in PSX RAM.
 *
 * psyq_ResetGraph calls it after PsyCross's ResetGraph and before replaying
 * ResetGraph's stack frame. Spyro 1 writes, for modes 0, 3 and 5 (mode & 7),
 * the environment block at 0x80074A64, libgpu's control structures and the
 * fill-packet block at 0x80075978; the default writes nothing.
 * @param cpu Guest CPU state.
 * @param mode The ResetGraph mode, from a0. */
void psyq_game_mirror_resetgraph(CPUState* cpu, int mode);

/** @brief Write the VSync callback pointer where PSYQ keeps it in PSX RAM.
 *
 * psyq_VSyncCallback calls it after recording the handler host-side. Spyro 1
 * stores a0 in libgpu's vtable slot at 0x800749D0; the default writes
 * nothing.
 * @param cpu Guest CPU state.
 * @param cb The registered handler's guest address. */
void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb);

/** @brief Write the libpad state that PSYQ PadInitDirect leaves in PSX RAM.
 *
 * psyq_PadInitDirect calls it after PsyCross's PadInitDirect. Spyro 1 writes
 * libpad's pad-state dispatch table at 0x800751BC, its control block at
 * 0x80075D18 and the per-slot bookkeeping words around them, with the two
 * buffer addresses in their slots; the default writes nothing.
 * @param cpu Guest CPU state.
 * @param buf0 Guest address of the first port's pad buffer.
 * @param buf1 Guest address of the second port's pad buffer. */
void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0, uint32_t buf1);

/** @brief Write the libpad state of an empty second controller slot.
 *
 * psyq_vbl_body calls it every VBL, since PsyCross's pad poll overwrites
 * the slot's buffer each time. Spyro 1 rewrites the 8-byte buffer at
 * 0x80078E50 and the word at 0x80078E9C; the default writes nothing.
 * @param cpu Guest CPU state. */
void psyq_game_mirror_pad_slot2(CPUState* cpu);

/** @brief Write the libmcrd state that PSYQ MemCardInit leaves in PSX RAM.
 *
 * psyq_MemCardInit calls it after initializing the host card module. Spyro 1
 * writes the card control struct around 0x800748F0 and the registered
 * handler count at 0x800749E0; the default writes nothing.
 * @param cpu Guest CPU state. */
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

/** @brief Return the vaddr every overlay of this game loads at (recomp.conf OVERLAY_LOAD_ADDR).
 *
 * After a disc read lands there, the first word is the overlay id and the
 * dispatcher is re-pointed at that overlay's translated code.
 * @return The vaddr, or 0 when the game names none. */
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

/** @brief Return the address of libcd's state block (recomp.conf LIBCD_BLOCK_ADDR).
 *
 * The block holds the status byte at +0xC, the last CdlSetloc position at
 * +0x18 (4 bytes), the mode byte at +0x1C and the last command at +0x1D,
 * which the library's CdStatus, CdLastPos, CdMode and CdLastCom read.
 * @return The block's address, or 0 when the game names none. */
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

/* Stream reads: the disc reads a cutscene streams its speech and animation
 * through, timed by the thunk layer's drive model instead of completing at
 * issue. */
/** @brief Classify the data read being issued as a stream read the drive model times.
 *
 * A stream read is one the game issues through its asynchronous reader
 * while a cutscene plays and polls from its main loop, returning to VSync
 * between polls. A read the game spins on in guest code must answer 0
 * unless psyq_game_cd_spin_wait names that spin.
 * @param cpu Guest CPU state at the CdRead call (ra is the call site).
 * @return 1 when the read is a stream read, else 0. */
int      psyq_game_cd_read_streamed(CPUState* cpu);
/** @brief Whether the function being entered is the game's synchronous reader polling the drive in a spin that never calls VSync.
 *
 * The dispatcher asks every 2048 entries without a VBL; while a stream
 * read or its closing CdlPause is owed, a yes passes one VBL there.
 * @param cpu Guest CPU state at the entry (ra is the caller's return address).
 * @param addr Guest address of the function being entered.
 * @return 1 for the spin's poll, else 0. */
int      psyq_game_cd_spin_wait(CPUState* cpu, uint32_t addr);

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

/** @brief Choose the music source for a streaming read issued under an XA mode.
 *
 * start_music_stream presets out to track -1 and no custom path, then calls
 * this before its speculation check, so any guest RAM the hook writes is
 * part of the tick. The default leaves both preset values. Spyro 1 reads
 * m_CurrentTrack (-1 outside 0..47), resolves the level id from
 * g_NextLevelId, or g_LevelId when that is outside 10..65, and returns the
 * mod-mapped song path for it.
 * @param sector The read's start LBA.
 * @param out Receives the track index and the custom song path. */
void psyq_game_music_route(int sector, psyq_music_route_t* out);

/** @brief Return the address of libcd's read-completion callback slot in PSX RAM (recomp.conf CD_READ_CB_ADDR).
 *
 * For a game that links libcd's own CdReadCallback rather than reaching the
 * thunk: the registration stores the guest handler there and never reaches
 * psyq_CdRead's tracked callback.
 * @return The slot's address, or 0 when the game registers through the
 *         thunk. */
uint32_t psyq_game_cd_read_cb_addr(void);

/* Render-path filter consumed by the OT walk. The two render-path hooks are
 * the one exception to the per-game rule above: the isolation registry they
 * forward to is shared code linked into every game (renderer/render_paths.c),
 * which defines them there. They take no descriptor field and have no inert
 * default, so a game cannot link the registry without arming the OT filter. */
/** @brief Reports whether a --render-paths selection is in effect; forwards render_paths_active.
 *
 * @return Nonzero when the OT walk filters geometry prims by path. */
int  psyq_game_render_paths_active(void);
/** @brief Reports whether the primitive at a polybuf address belongs to a selected path; forwards render_path_addr_visible.
 *
 * Looks the address up in the recorded intervals, then in the previous
 * tick's bank; an address in neither counts as not visible.
 * @param addr The primitive's guest address.
 * @return Nonzero when the OT walk keeps the primitive. */
int  psyq_game_render_path_addr_visible(uint32_t addr);

/** @brief Re-derive the resident overlay from the id word at psyq_game_overlay_vaddr() and re-point the dispatcher if it names a different overlay.
 *
 * Does nothing when the game has no overlay vaddr or guest RAM is not
 * mapped. A game passes this to spyro_overlay_set_resync() at boot when its
 * overlays can be copied into the overlay region rather than read straight
 * into it. */
void psyq_overlay_resync_from_ram(void);

/* --- Sound-effect slots ------------------------------------------------- */

/* Where a game keeps its sound tables. All three games load a level's
 * tables through one routine (Spyro 1 SetNewSoundTable, Spyro 2
 * func_8001D5FC, Spyro 3 func_8002C9F4): the SoundTable pointer gets the
 * blob's address, 256 one-byte slots map each semantic sound to a
 * definition index, the definition count follows as a word, and the
 * SoundDefinition pointer gets the address right after it, so a live level
 * table satisfies defs == table + 0x104. A SoundDefinition is 0x14 bytes
 * with the SPU address first. One ActiveSound per SPU voice holds the base
 * pitch the game keeps writing to that voice. */
typedef struct {
    uint32_t table_ptr_va;      /* word holding the SoundTable pointer */
    uint32_t defs_ptr_va;       /* word holding the SoundDefinition pointer */
    uint32_t active_va;         /* ActiveSound[24], index = SPU voice */
    uint32_t active_stride;     /* bytes per ActiveSound */
    uint32_t active_pitch_off;  /* offset of the pitch word in an ActiveSound */
    uint32_t bank_blast_len;    /* SpuWrite length of a level bank upload */
    const char* const* names;   /* 256 slot names */
} psyq_sfx_slots_t;

/** @brief Return the game's sound table layout, or NULL when the game names no sounds.
 * @return A descriptor with static lifetime, or NULL. */
const psyq_sfx_slots_t* psyq_game_sfx_slots(void);

/** @brief Register the game's sound-effect tables with the sfx pack.
 *
 * The SpuInit thunk calls it on every entry and the SpuWrite thunk on every
 * entry whose source span maps, so it must be idempotent. Installs the pitch
 * reader and slot-name resolver of psyq_sfx_slots.c in the sfx pack once
 * when psyq_game_sfx_slots() returns a descriptor. */
void psyq_game_sfx_arm(void);

/** @brief Return the SPU transfer length that means a whole new level sound bank, as opposed to a smaller write landing over part of it.
 *
 * A SpuWrite of this length bumps the sfx pack's generation and stops
 * orphaned voices; any other length invalidates only the range it covers.
 * @return The descriptor's bank_blast_len, or 0 when the game names no
 *         sounds, in which case every transfer is a partial write. */
unsigned psyq_game_spu_bank_blast_len(void);
/** @brief Resolve an SPU sample address to the game's own sound-effect slot names.
 *
 * @param spu_addr SPU RAM address of the sample.
 * @param out Receives up to `max` name pointers.
 * @param max Capacity of `out`.
 * @return How many names were written; 0 when the game names no sounds. */
int  psyq_game_sfx_slot_names(unsigned spu_addr, const char** out, int max);

/* --- The game's descriptor --------------------------------------------- */

/* recomp.conf's PSYQ addresses, generated into game_identity.c as
 * <tag>_psyq_conf (tools/gen_game_identity.py). */
typedef struct psyq_game_conf {
    uint32_t overlay_vaddr;     /* OVERLAY_LOAD_ADDR */
    uint32_t libcd_block_addr;  /* LIBCD_BLOCK_ADDR */
    uint32_t cd_read_cb_addr;   /* CD_READ_CB_ADDR; 0: through the thunk */
} psyq_game_conf_t;

/* A PSYQ symbol of the game's manifest that no thunk defines, with its guest
 * address in this game. */
typedef struct psyq_stub_addr {
    const char* name;           /* "psyq_<symbol>", the stub's link name */
    uint32_t    addr;
} psyq_stub_addr_t;

/* The game's abort-stub addresses, generated by tools/filter_psyq_stubs.py
 * from the game's psyq_stubs.c as <tag>_psyq_stub_table. One stub body serves
 * every game (the union stub set), so psyq_stub_hit looks the running game's
 * address up here by name. */
typedef struct psyq_stub_table {
    const psyq_stub_addr_t* entries;
    size_t                  count;
} psyq_stub_table_t;

/* What one game supplies to the shared thunk layer. Each field backs the
 * psyq_game_* entry of the same name above; a 0 or NULL field takes that
 * entry's documented default. */
typedef struct psyq_game_hooks {
    const psyq_game_conf_t*  conf;     /* NULL: every address 0 */
    const psyq_stub_table_t* stubs;    /* NULL: stub addresses report 0 */

    int  (*stack_frame)(psyq_frame_site_t site, psyq_game_frame_t* out);
    void (*mirror_main)(CPUState* cpu);
    void (*mirror_resetgraph)(CPUState* cpu, int mode);
    void (*mirror_vsync_callback)(CPUState* cpu, uint32_t cb);
    void (*mirror_pad_init)(CPUState* cpu, uint32_t buf0, uint32_t buf1);
    void (*mirror_pad_slot2)(CPUState* cpu);
    void (*mirror_memcard_init)(CPUState* cpu);
    uint32_t rand_seed_addr;
    uint16_t root_counter2;
    uint32_t libgpu_env_addr;
    uint32_t libgpu_clear_block_addr;

    void (*cd_setloc)(const void* cdl_loc);
    void (*cd_apply_buf)(unsigned char* buf, int sectors);
    void (*cd_read_done)(CPUState* cpu, uint32_t read_cb);
    int  (*cd_read_in_load)(CPUState* cpu);
    uint32_t cd_read_delay_cap;
    int  (*cd_read_streamed)(CPUState* cpu);
    int  (*cd_spin_wait)(CPUState* cpu, uint32_t addr);
    uint32_t load_window_word;
    int  (*load_window_active)(uint32_t value);
    uint32_t cd_raw_pc_min;            /* 0: none (the entry answers 0xFFFFFFFF) */
    uint32_t cd_done_witness_ra;

    void (*music_route)(int sector, psyq_music_route_t* out);
    const psyq_sfx_slots_t* sfx_slots;
} psyq_game_hooks_t;

/** @brief Return the running game's PSYQ descriptor, or an all-zero one when the game registers none.
 *
 * @return The descriptor named by the registered openpete_game_runtime_t;
 *         never NULL. */
const psyq_game_hooks_t* psyq_game_hooks(void);

#ifdef __cplusplus
}
#endif

#endif
