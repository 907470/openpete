/* games/common/psyq/psyq_thunk_state.h - the guest-mutated latches of the shared
 * PSYQ thunk layer, packed for the savestate's game-extra payload.
 *
 * A thunk latch is host state a guest call set and a later guest call or the
 * music route depends on: the CD-ready and VSync callback vaddrs, the drive
 * mode and XA filter, the interrupt-callback table and the DMA-callback table.
 * A rolled-back speculative tick that ran transition code leaves them on the
 * abandoned branch unless they travel with the snapshot, and the committed
 * timeline's next read then routes down the wrong path.
 *
 * Not packed: the CD and SPU main volumes (host gain only; a rewind past a
 * fade holds the post-fade gain until the guest's next fader write), and the
 * native-decomp read callback (a host function pointer). The virtual music
 * head is in the core CD section already.
 *
 * Layout, little-endian words, PSYQ_THUNK_STATE_BYTES total:
 *   [0]      pack_ver (PSYQ_THUNK_STATE_PACK_VER)
 *   [1]      cd_cb      guest CdReadCallback handler vaddr
 *   [2]      vs_cb      guest VSyncCallback handler vaddr
 *   [3]      cd_mode    the drive's mode byte (CdlSetmode); pack_ver 1 held
 *                       an armed bit here, widened on load
 *   [4]      cd_filter_file, [5] cd_filter_chan (CdlSetfilter)
 *   [6..16]  intr_cb[11]  interrupt-callback table
 *   [17..23] dma_cb[7]    DMA-callback table
 *   [24]     setloc_sector  last CdlSetloc (pack_ver 2)
 *   [25]     cd_reading     1 while a CdlReadS stream runs, cleared by
 *                           CdlPause, CdlStop or a data read (pack_ver 3)
 *   [26]     defer_owed     VBLs until the deferred data read completes
 *   [27]     defer_pending  1 while a data read's completion is deferred
 *   [28]     load_budget    VBLs the current level load has left to spend
 *   [29]     load_prev      in-load bit of the previous read (pack_ver 4)
 *   [30]     pad_busy       vsyncs left in libpad's controller command sequence
 *   [31..37] card_state[7] the BIOS card driver: its flag word (started,
 *                           port flip, transfer in progress, per-port
 *                           status, bu-layer and command kind bits) and
 *                           each port's pending sector, buffer and device
 *                           id (pack_ver 5)
 *   [38]     pad_seq        length of that sequence (pack_ver 5)
 *   [39..40] defer_rec/vbl  the reference's completion record and VBLANK
 *                           of the deferred read under the schedule
 *                           (pack_ver 5)
 *   [41..42] defer_clock    that completion's clock value on the reference,
 *                           low word first (pack_ver 5)
 *   [43]     defer_stream   1 while the deferred read is a drive-timed
 *                           stream read (pack_ver 6)
 *   [44]     pause_owed     VBLs until that read's closing CdlPause
 *                           completes (pack_ver 6)
 *   [45]     spu_clk_frac   remainder of the SPU's samples per VBL, in
 *                           1/715909 samples (pack_ver 7)
 */
#ifndef OPENPETE_PSYQ_THUNK_STATE_H
#define OPENPETE_PSYQ_THUNK_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSYQ_THUNK_STATE_PACK_VER 7u
#define PSYQ_THUNK_STATE_WORDS    46u
#define PSYQ_THUNK_STATE_BYTES    (PSYQ_THUNK_STATE_WORDS * 4u)
#define PSYQ_THUNK_STATE_V1_BYTES 96u
#define PSYQ_THUNK_STATE_V2_BYTES 100u
#define PSYQ_THUNK_STATE_V3_BYTES 104u
#define PSYQ_THUNK_STATE_V4_BYTES 120u
#define PSYQ_THUNK_STATE_V5_BYTES 172u
#define PSYQ_THUNK_STATE_V6_BYTES 180u

/** @brief Return the byte length of a pack whose first word is `ver`.
 *
 * @param ver A pack_ver, 1 through PSYQ_THUNK_STATE_PACK_VER.
 * @return The pack's length in bytes, or 0 for a version this layer does
 *         not know. */
uint32_t psyq_thunk_state_bytes_for(uint32_t ver);

/** @brief Return the byte length of a pack this layer writes.
 *
 * @return PSYQ_THUNK_STATE_BYTES. */
uint32_t psyq_thunk_state_size(void);
/** @brief Write the latches to `dst` as a pack of the current pack_ver.
 *
 * The layout is the one in this file's header comment; the card driver state
 * is included.
 * @param dst Buffer of at least PSYQ_THUNK_STATE_BYTES bytes. */
void     psyq_thunk_state_save(void* dst);   /* dst has PSYQ_THUNK_STATE_BYTES */
/** @brief Restore the latches from a pack of any known pack_ver.
 *
 * A field the pack's version predates is set to its idle value: the
 * streaming latch, the load-delay words, the pad countdowns, the deferred
 * read's reference position and the card driver state become 0; the
 * CdlSetloc sector of a version-1 pack keeps its current value. A
 * version-1 mode word holds an armed bit and is widened to the drive mode it
 * stood for.
 * @param src The pack.
 * @param size Bytes available at `src`.
 * @return 0 when applied; -1 when the latches were left alone (`size`
 *         shorter than the pack's version needs, or pack_ver 0 or above
 *         PSYQ_THUNK_STATE_PACK_VER). */
int      psyq_thunk_state_load(const void* src, uint32_t size);

/** @brief Restore the latches from the layout Spyro 1's extras carried before the pack.
 *
 * The layout holds the two callback vaddrs and one word with the XA armed
 * bit at bit 0, the filter file at bits 8..15 and the filter channel at
 * 16..23; the armed bit widens to the drive mode it stood for. Every latch
 * the layout never saved (the interrupt and DMA tables, the CdlSetloc
 * sector, the streaming, load-delay, pad and card state) stays as it is.
 * @param cd_cb Guest CdReadCallback handler vaddr.
 * @param vs_cb Guest VSyncCallback handler vaddr.
 * @param xa_word The armed bit and XA filter word. */
void     psyq_thunk_state_load_v4(uint32_t cd_cb, uint32_t vs_cb,
                                  uint32_t xa_word);

/** @brief Overwrite every latch the pack covers with a value no guest registration produces.
 *
 * Callback slots get distinct odd vaddrs, the XA filter fields values
 * outside 0..255, the mode byte its complement, and the counters and card
 * driver words distinct sentinels, so a save after it differs from a save
 * of guest-produced state in every word but pack_ver. */
void     psyq_thunk_state_scramble(void);
/** @brief Run the latch pack's round-trip check.
 *
 * Saves, scrambles, saves, loads the first image, and saves again. The
 * check passes when the first and last images match and the scrambled image
 * differs from the first in every word but pack_ver; the verdict is logged.
 * psyq_ResetGraph runs it once when OPENPETE_THUNK_STATE_SELFTEST is set and
 * not "0", then exits the process.
 * @return 0 on pass, -1 on failure. */
int      psyq_thunk_state_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
