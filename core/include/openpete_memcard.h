/* openpete_memcard.h — host-backed PlayStation memory card.
 *
 * Spyro reaches the memory card only through the PSYQ libmcrd MemCard*
 * wrappers, which openpete intercepts as psyq_MemCard* thunks
 * (games/spyro-1/psyq_thunks.cpp). Those thunks marshal the MIPS ABI and
 * call into this module, which owns two real 128 KiB PSX memory-card
 * images (.mcr) persisted to disk in a `cards` directory beside the
 * `states` one the save-states use — in the game folder for a portable
 * install, under the per-user state dir otherwise (openpete_paths.h). The
 * images are standard raw cards — portable to DuckStation / PCSX / real
 * hardware.
 *
 * See notes/plans/openpete-memcard-host-backed.md.
 */
#ifndef OPENPETE_MEMCARD_H
#define OPENPETE_MEMCARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration (call from run.c before the game's MemCardInit runs) --- */

/* Override the directory holding cardN.mcr. NULL/"" = the resolved
 * `cards` dir. Precedence: this override > OPENPETE_CARD_DIR env > default. */
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

/* Per-modset save safety (--adopt-saves): explicitly re-tag card
 * sidecars to the RUNNING modset at open, silencing the mismatch
 * warning. Without it, a mismatch warns (never blocks) and the tag is
 * preserved so the warning keeps surfacing. */
void openpete_memcard_set_adopt(int on);

/* Run the save-safety modset check at engine startup for existing card
 * images (the game may not touch the card for a long time; the warning
 * concerns the launch configuration). Call after mods init + setters. */
void openpete_memcard_startup_check(void);

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

/* --- BIOS card surface (called by the psx_bios.c A0/B0 gate) -------------
 * The BIOS addresses the card as 1024 numbered 128-byte frames rather than
 * by file name; both views are the same image. chan is 0/1 or the raw port
 * encoding. Return 0 on success, -1 when the slot is empty or out of range. */
int  openpete_memcard_present(int chan);
/* The BIOS card gate's vblank step (core/runtime/psx_bios.c): a command
 * to an empty port signals EvSpTIMOUT from here when its timeout ends. */
struct CPUState;
void openpete_psx_bios_vbl(struct CPUState* cpu);
/* The card driver's state, for the savestate's thunk latch pack: a flag
 * word and the two ports' pending command (sector, buffer, device id). */
#define PSX_BIOS_CARD_STATE_WORDS 7u
void openpete_psx_bios_card_state_get(uint32_t w[PSX_BIOS_CARD_STATE_WORDS]);
void openpete_psx_bios_card_state_set(const uint32_t w[PSX_BIOS_CARD_STATE_WORDS]);
/* A byte read from the BIOS ROM window (physical 0x1FC00000..0x1FC7FFFF):
 * returns 1 with the byte of the ROM's identification block, 0 elsewhere;
 * 0 when the address is outside the window. */
int      openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out);
/* Writes the kernel's fixed low-RAM words (exception and call vectors, the
 * RAM size and boot flags) into a 2 MB RAM image, before the EXE loads. */
void     openpete_psx_bios_seed_ram(uint8_t* ram);
uint32_t openpete_psx_bios_rom_reads(void);
int  openpete_memcard_read_frame (int chan, int frame, void* dst_host);
int  openpete_memcard_write_frame(int chan, int frame, const void* src_host);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MEMCARD_H */
