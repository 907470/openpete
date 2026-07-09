/* openpete_memcard.h — host-backed PlayStation memory card.
 *
 * Spyro reaches the memory card only through the PSYQ libmcrd MemCard*
 * wrappers, which openpete intercepts as psyq_MemCard* thunks
 * (games/spyro-1/psyq_thunks.cpp). Those thunks marshal the MIPS ABI and
 * call into this module, which owns two real 128 KiB PSX memory-card
 * images (.mcr) persisted to disk under the same per-user state dir the
 * save-states use ($XDG_STATE_HOME/openpete/cards, %LOCALAPPDATA%\openpete\
 * cards on Windows). The images are standard raw cards — portable to
 * DuckStation / PCSX / real hardware.
 *
 * See notes/plans/openpete-memcard-host-backed.md.
 */
#ifndef OPENPETE_MEMCARD_H
#define OPENPETE_MEMCARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration (call from run.c before the game's MemCardInit runs) --- */

/* Override the directory holding cardN.mcr. NULL/"" = default appdir.
 * Precedence: this override > OPENPETE_CARD_DIR env > default. */
void openpete_memcard_set_dir_override(const char* dir);

/* Explicit per-card image paths (--card1 / --card2). NULL = default
 * <carddir>/cardN.mcr. A path here also forces that slot present. */
void openpete_memcard_set_paths(const char* card_a, const char* card_b);

/* Insert (present=1) or remove (present=0) the card in a slot. slot 0 =
 * port A, slot 1 = port B. Default: both present. */
void openpete_memcard_set_presence(int slot, int present);

/* When newly provisioning a card image (no file on disk yet), leave it
 * blank/unformatted (so the game's own format UI runs) instead of the
 * default pre-formatted-empty card. */
void openpete_memcard_set_unformatted(int on);

/* --- libmcrd surface (called by the psyq_MemCard* thunks) ----------------- */

/* Reset the async-sync latch + "new card" flags. Driven by the game's
 * MemCardInit. Does not disturb presence/paths/dir set by the setters. */
void openpete_memcard_init(void);

void openpete_memcard_start(void);   /* arm comms (no-op; always ready) */
void openpete_memcard_stop(void);    /* disarm + clear the sync latch    */

/* chan is the raw PSX port encoding: port A = 0x00, port B = 0x10.
 * exist/accept return the libmcrd v0 (1 = card present, 0 = absent) and
 * update the internal sync latch that MemCardSync surfaces; the optional
 * out_cmds/out_rslt receive the libmcrd command + result codes. */
int  openpete_memcard_exist (int chan, int* out_cmds, int* out_rslt);
int  openpete_memcard_accept(int chan, int* out_cmds, int* out_rslt);

/* Read/write `bytes` at file offset `ofs` of file `name` on card `chan`,
 * to/from a host pointer (the thunk passes PTR(guest_addr)). Return a
 * libmcrd result code (0 = McErrNone). */
int  openpete_memcard_read (int chan, const char* name, void* dst_host,
                            int ofs, int bytes);
int  openpete_memcard_write(int chan, const char* name, const void* src_host,
                            int ofs, int bytes);

/* Allocate `blocks` blocks for a new file `name`. Result code. */
int  openpete_memcard_create(int chan, const char* name, int blocks);

/* Format (blank) the card in `chan`. Result code. */
int  openpete_memcard_format(int chan);

/* mode: 0 = sync, 1 = async (both complete instantly). Writes the last
 * command/result into out_cmds/out_rslt when known, returns the sync
 * status (1 = finished, 0 = active, -1 = nothing registered). */
int  openpete_memcard_sync(int mode, int* out_cmds, int* out_rslt);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MEMCARD_H */
