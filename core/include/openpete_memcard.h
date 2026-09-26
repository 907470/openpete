/* openpete_memcard.h — host-backed PlayStation memory card.
 *
 * Spyro reaches the memory card only through the PSYQ libmcrd MemCard*
 * wrappers, which openpete intercepts as psyq_MemCard* thunks
 * (games/common/psyq/psyq_thunks.cpp). Those thunks marshal the MIPS ABI and
 * call into this module, which owns two real 128 KiB PSX memory-card
 * images (.mcr) persisted to disk in a `cards` directory beside the
 * `states` one the save-states use — in the game folder for a portable
 * install, under the per-user state dir otherwise (openpete_paths.h). The
 * images are standard raw cards — portable to DuckStation / PCSX / real
 * hardware.
 */
#ifndef OPENPETE_MEMCARD_H
#define OPENPETE_MEMCARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration (call from run.c before the game's MemCardInit runs) --- */

/** @brief Set the directory holding cardN.mcr (--card-dir).
 *
 * Precedence when a slot's path resolves: this override, then the
 * OPENPETE_CARD_DIR environment variable, then the `cards` state
 * directory. The directory is created when a slot's path resolves.
 * @param dir Directory path; NULL or "" clears the override. */
void openpete_memcard_set_dir_override(const char* dir);

/** @brief Set explicit per-card image paths (--card1 / --card2).
 *
 * A non-empty path also marks its slot present; a later
 * openpete_memcard_set_presence call can still remove it.
 * @param card_a Image path for slot 0; NULL or "" selects <carddir>/card1.mcr.
 * @param card_b Image path for slot 1; NULL or "" selects <carddir>/card2.mcr. */
void openpete_memcard_set_paths(const char* card_a, const char* card_b);

/** @brief Insert or remove the card in a slot.
 *
 * Both slots are present by default. A slot other than 0 or 1 is ignored.
 * @param slot 0 for port A, 1 for port B.
 * @param present Nonzero inserts the card, zero removes it. */
void openpete_memcard_set_presence(int slot, int present);

/** @brief Select an all-zero unformatted image instead of a formatted empty card when provisioning a card (--card-unformatted).
 *
 * Provisioning happens on first access when the image file is absent or
 * short; the unformatted image is written to the file, and the game's own
 * format flow then runs.
 * @param on Nonzero selects the unformatted image. */
void openpete_memcard_set_unformatted(int on);

/** @brief Retag each card's modset sidecar to the running modset when the card is opened (--adopt-saves).
 *
 * The sidecar `<image>.modset` records the modset tag a card's progress
 * belongs to. With adoption on, a mismatch at open rewrites the sidecar
 * and logs instead of warning. With it off, a mismatch warns and never
 * blocks, and the tag is kept so the warning repeats; an untagged card is
 * still tagged on its first write under active mods.
 * @param on Nonzero enables adoption. */
void openpete_memcard_set_adopt(int on);

/** @brief Run the modset tag check at startup for each present slot whose image file exists.
 *
 * The game may not touch the card until its save UI, and the check
 * concerns the launch configuration. The check runs once per slot per
 * process, so the later open by the game does not repeat it. A missing
 * image is skipped: it holds no progress, and the first write tags it.
 * Call after mods init and the configuration setters. */
void openpete_memcard_startup_check(void);

/* --- libmcrd surface (called by the psyq_MemCard* thunks) ----------------- */

/** @brief Clear the sync latch and set both slots' new-card flags.
 *
 * Called from the game's MemCardInit. Presence, paths and the directory
 * set by the configuration setters are kept. */
void openpete_memcard_init(void);

void openpete_memcard_start(void);   /* arm comms (no-op; always ready) */
void openpete_memcard_stop(void);    /* disarm + clear the sync latch    */

/** @brief Probe the card in chan for libmcrd MemCardExist.
 *
 * The probe loads or provisions the slot's image and latches command
 * MC_FUNC_EXIST with the result for openpete_memcard_sync. The result is
 * McErrCardNotExist for an empty slot, McErrNotFormat for an unformatted
 * image, McErrNewCard on the first probe of a present formatted card after
 * openpete_memcard_init (the flag then clears), and McErrNone otherwise.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @param out_cmds Receives the libmcrd command code when non-NULL.
 * @param out_rslt Receives the libmcrd result code when non-NULL.
 * @return 1 when a card is present, 0 when the slot is empty. */
int  openpete_memcard_exist (int chan, int* out_cmds, int* out_rslt);
/** @brief Probe the card in chan for libmcrd MemCardAccept.
 *
 * Identical to openpete_memcard_exist except that the latched command is
 * MC_FUNC_ACCEPT.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @param out_cmds Receives the libmcrd command code when non-NULL.
 * @param out_rslt Receives the libmcrd result code when non-NULL.
 * @return 1 when a card is present, 0 when the slot is empty. */
int  openpete_memcard_accept(int chan, int* out_cmds, int* out_rslt);

/** @brief Copy bytes bytes at offset ofs of file name on the card in chan to a host buffer.
 *
 * The file is found by its first-block directory frame and read as
 * contiguous image bytes from that block. The result is also latched with
 * command MC_FUNC_READ for openpete_memcard_sync.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @param name On-card file name, compared over 20 bytes.
 * @param dst_host Destination; the thunk passes PTR(guest_addr).
 * @param ofs Byte offset within the file.
 * @param bytes Byte count.
 * @return McErrNone (0); McErrCardNotExist for an empty slot;
 *         McErrFileNotExist when the file is absent, dst_host is NULL, or
 *         the range falls outside the image. */
int  openpete_memcard_read (int chan, const char* name, void* dst_host,
                            int ofs, int bytes);
/** @brief Copy bytes bytes from a host buffer to offset ofs of file name on the card in chan and write the image to disk.
 *
 * The mirror of openpete_memcard_read; the latched command is
 * MC_FUNC_WRITE.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @param name On-card file name, compared over 20 bytes.
 * @param src_host Source; the thunk passes PTR(guest_addr).
 * @param ofs Byte offset within the file.
 * @param bytes Byte count.
 * @return McErrNone (0); McErrCardNotExist for an empty slot;
 *         McErrFileNotExist when the file is absent, src_host is NULL, or
 *         the range falls outside the image. */
int  openpete_memcard_write(int chan, const char* name, const void* src_host,
                            int ofs, int bytes);

/** @brief Allocate blocks blocks for a new file name on the card in chan and write the image to disk.
 *
 * Takes the lowest-numbered free directory frames (state 0xA0 or 0x00)
 * and chains them: state 0x51 on the first, 0x53 on the last, 0x52
 * between, each linked to the next with 0xFFFF ending the chain. The first
 * frame holds the size, blocks * 8192, and the name. The sync latch is
 * not updated.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @param name File name, stored in the 20-byte name field.
 * @param blocks Block count; values below 1 allocate 1.
 * @return McErrNone (0), McErrCardNotExist, McErrNotFormat,
 *         McErrAlreadyExist when the name exists, or McErrBlockFull when
 *         too few blocks are free. */
int  openpete_memcard_create(int chan, const char* name, int blocks);

/** @brief Replace the card in chan with a formatted empty image and write it to disk.
 *
 * The sync latch and the new-card flag are not updated.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @return McErrNone (0), or McErrCardNotExist for an empty slot. */
int  openpete_memcard_format(int chan);

/** @brief Report the sync latch for libmcrd MemCardSync.
 *
 * Host I/O completes inside each call, so no command is ever reported
 * active. The latch is set by exist, accept, read and write, and cleared by
 * openpete_memcard_init and openpete_memcard_stop.
 * @param mode 0 = sync, 1 = async; ignored.
 * @param out_cmds Receives the last latched command code when one is latched.
 * @param out_rslt Receives the last latched result code when one is latched.
 * @return 1 (finished) after a latched command, -1 (nothing registered)
 *         otherwise. */
int  openpete_memcard_sync(int mode, int* out_cmds, int* out_rslt);

/* --- BIOS card surface (called by the psx_bios.c A0/B0 gate) -------------
 * The BIOS addresses the card as 1024 numbered 128-byte frames rather than
 * by file name; both views are the same image. chan is 0/1 or the raw port
 * encoding. */
/** @brief Return 1 when the slot addressed by chan holds a card, else 0.
 *
 * The image is not loaded.
 * @param chan Slot as 0 or 1, or the raw port encoding 0x00 or 0x10.
 * @return 1 when present, 0 when empty. */
int  openpete_memcard_present(int chan);
/* The BIOS card gate's vblank step (core/psx/psx_bios.c): a command
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
/** @brief Copy one 128-byte frame of the card in chan to a host buffer.
 *
 * Loads or provisions the image on first access.
 * @param chan Slot as 0 or 1, or the raw port encoding 0x00 or 0x10.
 * @param frame Frame number, 0 to 1023.
 * @param dst_host Destination of 128 bytes.
 * @return 0 on success, -1 when the slot is empty, dst_host is NULL, or
 *         frame is out of range. */
int  openpete_memcard_read_frame (int chan, int frame, void* dst_host);
/** @brief Copy one 128-byte frame from a host buffer into the card in chan and write the image to disk.
 *
 * Applies the modset tag rule for a write before the copy.
 * @param chan Slot as 0 or 1, or the raw port encoding 0x00 or 0x10.
 * @param frame Frame number, 0 to 1023.
 * @param src_host Source of 128 bytes.
 * @return 0 on success, -1 when the slot is empty, src_host is NULL, or
 *         frame is out of range. */
int  openpete_memcard_write_frame(int chan, int frame, const void* src_host);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MEMCARD_H */
