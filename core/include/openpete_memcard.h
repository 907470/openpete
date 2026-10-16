/* openpete_memcard.h - host-backed PlayStation memory card.
 *
 * Spyro reaches the memory card only through the PSYQ libmcrd MemCard*
 * wrappers, which openpete intercepts as psyq_MemCard* thunks
 * (games/common/psyq/psyq_thunks.cpp). Those thunks marshal the MIPS ABI and
 * call into this module, which owns two real 128 KiB PSX memory-card
 * images (.mcr) persisted to disk in a `cards` directory beside the
 * `states` one the save-states use - in the game folder for a portable
 * install, under the per-user state dir otherwise (openpete_paths.h). The
 * images are standard raw cards - portable to DuckStation / PCSX / real
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
 * was moved aside as not a card image; the unformatted image is written to
 * the file, and the game's own format flow then runs.
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

/** @brief Serve libmcrd MemCardStart; a no-op, since the host card is always ready.
 *
 * Called from the psyq_MemCardStart thunk. Leaves the sync latch and the
 * slot state unchanged. */
void openpete_memcard_start(void);

/** @brief Serve libmcrd MemCardStop by clearing the sync latch.
 *
 * Called from the psyq_MemCardStop thunk. The latched command and result
 * reset to -1 and the sync status to nothing pending, so a later
 * MemCardSync reports nothing until the next command latches. Presence,
 * paths, card images and new-card flags are kept. */
void openpete_memcard_stop(void);

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
 *         the range falls outside the image; McErrCardInvalid (2) when the
 *         image did not reach the disk, with the card left as it was. */
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
 *         McErrAlreadyExist when the name exists, McErrBlockFull when
 *         too few blocks are free, or McErrCardInvalid (2) when the image
 *         did not reach the disk, with the card left as it was. */
int  openpete_memcard_create(int chan, const char* name, int blocks);

/** @brief Replace the card in chan with a formatted empty image and write it to disk.
 *
 * The sync latch and the new-card flag are not updated.
 * @param chan Raw PSX port encoding: port A = 0x00, port B = 0x10.
 * @return McErrNone (0), McErrCardNotExist for an empty slot, or
 *         McErrCardInvalid (2) when the image did not reach the disk, with
 *         the card left as it was. */
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
struct CPUState;
/** @brief Run one vblank interrupt of the BIOS card driver.
 *
 * Does nothing until StartCARD2 has hooked the step. Follows the ROM's
 * firstStageCardAction (RAM 0x5000): the step undelivers HwCARD's five
 * specs; a step that finds a transfer in progress aborts it with status
 * 0x11 and EvSpTIMOUT; otherwise it flips the port it serves and starts
 * that port's pending command, which a present card completes in
 * openpete_psx_bios_vbl_complete. A vblank the trace schedule marks
 * unserved is skipped; a transfer in progress is still aborted on an
 * unserved vblank when the following vblank is served. A command to an
 * empty port therefore signals its timeout from here. The VBLANK is the
 * schedule position plus one (openpete_psx_bios_vbl_at).
 * @param cpu Guest CPU the event handlers run on. */
void openpete_psx_bios_vbl(struct CPUState* cpu);
/** @brief Report whether the card driver holds SIO0 with a transfer it started and has not finished.
 *
 * Reads the driver's transfer-active byte (RAM 0x755A). At a VBLANK the
 * byte is set, the kernel's step aborts that transfer and the controller
 * is sent no command on that vsync, so libpad's handler polls no pad data.
 * @return 1 when a transfer is in progress, else 0. */
int openpete_psx_bios_card_busy(void);
/** @brief Run the BIOS card driver's vblank step for reference VBLANK vbl.
 *
 * openpete_psx_bios_vbl with the VBLANK given: a VBLANK a critical section
 * held runs its step when the section ends, keyed on the VBLANK it fell
 * due at, so the served-set lookup reads that VBLANK's row.
 * @param cpu Guest CPU the event handlers run on.
 * @param vbl The reference VBLANK index the step belongs to. */
void openpete_psx_bios_vbl_at(struct CPUState* cpu, uint32_t vbl);
/** @brief Run the completion of the card transfer the vblank step started, when a card answers on the port it serves.
 *
 * The kernel completes a present card's transfer in the SIO0 interrupt
 * about half a VBL after the VBLANK that started it, after the VBLANK
 * handlers; the VBL body calls this after the VSync callbacks and the
 * CD-ROM controller's step and before the deferred disc completion. The
 * transfer runs byte by byte: the sector moves between the host card and
 * guest RAM, the bu layer chains or finishes its operation, and HwCARD is
 * signalled. Under the oracle, a transfer the reference's next record
 * anchors inside is held and stepped by openpete_psx_bios_card_anchor. A
 * transfer to an empty port stays in progress for the next step's abort.
 * @param cpu Guest CPU the event handlers run on. */
void openpete_psx_bios_vbl_complete(struct CPUState* cpu);
/** @brief Run the held card transfer's steps up to the reference's anchor tick of a record, before the record is compared.
 *
 * The oracle verifier calls this at each anchor. A transfer
 * openpete_psx_bios_vbl_complete held for this record shows the bytes the
 * reference's SIO0 interrupts had left at the anchor; a transfer held for
 * another record completes. Does nothing without a held transfer.
 * @param rec The trace record about to be compared. */
void openpete_psx_bios_card_anchor(uint32_t rec);
/** @brief Complete the held card transfer after the record is compared: its remaining bytes, the bu layer's step and HwCARD's event. */
void openpete_psx_bios_card_anchor_end(void);
/** @brief Read the counts of transfers held for an anchor, steps run at anchors, and held transfers completed without their anchor.
 * @param held Receives the transfers held.
 * @param anchor_steps Receives the steps run up to an anchor tick.
 * @param fallbacks Receives the held transfers completed at another record's anchor or at the next vblank step. */
void openpete_psx_bios_card_anchor_counts(uint32_t* held, uint32_t* anchor_steps, uint32_t* fallbacks);
/** @brief Close the five events the ROM's boot opened, as libetc's ResetCallback does through _96_remove (A0 0x72).
 *
 * The thunked ResetCallback calls this so the event table's first entries
 * are free for the game's own events, as on hardware. */
void openpete_psx_bios_reset_callback_events(void);
/** @brief Open and enable libspu's DMA-completion event (HwSPU, EvSpCOMP, EvMdNOINTR), as SpuStart does.
 *
 * The thunked SpuInit calls this once. */
void openpete_psx_bios_spu_start_events(void);
/* The card driver's state as savestate latch packs of versions 5-9 carried
 * it, before the driver moved into guest RAM. */
#define PSX_BIOS_CARD_STATE_WORDS 7u
/** @brief Load the card driver's state from a latch pack of version 5-9 into the driver's guest RAM.
 *
 * Word 0 packs the started, flip and in-progress bits, each port's status,
 * bu flag and pending op, and the last finished port; words 1..3 and 4..6
 * hold the sector, buffer and device id of port 0 and port 1.
 * @param w PSX_BIOS_CARD_STATE_WORDS words. */
void openpete_psx_bios_card_state_load_legacy(const uint32_t w[PSX_BIOS_CARD_STATE_WORDS]);
/** @brief Read one byte of the BIOS ROM window (physical 0x1FC00000..0x1FC7FFFF).
 *
 * Only the SCPH-1001 identification block is modelled: the date and
 * console-type words at 0x100, the vendor and model strings at 0x108 and
 * 0x12C, the version string at 0x7FF32 (its last character 'A' names the
 * region) and the copyright string at 0x7FF54. Every other ROM byte reads
 * as 0. Each call in the window increments the counter
 * openpete_psx_bios_rom_reads returns.
 * @param phys Physical address.
 * @param out Receives the byte.
 * @return 1 when @p phys is in the window, else 0 with @p out unwritten. */
int      openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out);
/** @brief Write the kernel RAM the SCPH-1001 boot leaves into a 2 MB RAM image.
 *
 * The general exception vector at 0x80 (lui k0,0; addiu k0,k0,0xC80; jr
 * k0; nop) and its copy at 0x00, whose first word the boot leaves as 3;
 * the A0/B0/C0 function call vectors (lui t0,0; addiu t0,t0,
 * 0x5C4|0x5E0|0x600; jr t0; nop); the RAM size in megabytes at 0x60 and
 * the boot flags word 0xFF at 0x68 (OpenBIOS kernel/handlers.c
 * installKernelHandlers and main/main.c set the same words). Then the
 * kernel's tables as the ROM leaves them at the EXE entry: the table of
 * tables at 0x100, the A0, C0 and B0 function tables, the device control
 * blocks, errno, the file control blocks (fds 0 and 1 open on the tty,
 * file 2 closed with the words the boot EXE's open left, per game), and
 * the kernel heap from 0xE000 (exception chain, event table, process and
 * thread blocks). libcard's patch routine reads C0 entry 6 (0x68C, 0xC80)
 * and B0 entry 0x5B (0x9E0, 0x43D0) among them, and the lui/addiu pair at
 * 0xCF0 (handler+0x70, also written here) whose immediates compose the
 * card fast-track patch slot (0x641C + 0x28), so its stores land where
 * they land on hardware. The kernel's code outside these tables is not
 * written. Called at boot before the EXE loads.
 * @param ram Host pointer to guest RAM. */
void     openpete_psx_bios_seed_ram(uint8_t* ram);
/** @brief Seed the kernel RAM of a loaded savestate that predates the seed.
 *
 * A state whose table of tables (the word at 0x100) is zero was written
 * before the boot seeded the kernel tables and gets the boot seed; any
 * other state carries the kernel RAM the run left and is not touched, since
 * the file calls and the game write it after boot.
 * @param ram Host pointer to guest RAM. */
void     openpete_psx_bios_reseed_ram(uint8_t* ram);
/** @brief Return the number of BIOS ROM window reads served so far.
 *
 * @return The count of openpete_psx_bios_rom_read8 calls that fell in the window. */
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
/** @brief Copy one 128-byte frame from a host buffer into the card in chan and write that frame to disk.
 *
 * The frame is written in place in the image file (128 bytes, synced),
 * not the whole 128 KiB image; the modset tag rule for a write applies
 * once it lands. A failed write restores the frame in memory, so the
 * BIOS reports a card error for a save that did not persist.
 * @param chan Slot as 0 or 1, or the raw port encoding 0x00 or 0x10.
 * @param frame Frame number, 0 to 1023.
 * @param src_host Source of 128 bytes.
 * @return 0 on success, -1 when the slot is empty, src_host is NULL,
 *         frame is out of range, or the write did not reach the file. */
int  openpete_memcard_write_frame(int chan, int frame, const void* src_host);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MEMCARD_H */
