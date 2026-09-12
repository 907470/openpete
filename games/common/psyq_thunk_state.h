/* games/common/psyq_thunk_state.h — the guest-mutated latches of the shared
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
 */
#ifndef OPENPETE_PSYQ_THUNK_STATE_H
#define OPENPETE_PSYQ_THUNK_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSYQ_THUNK_STATE_PACK_VER 2u
#define PSYQ_THUNK_STATE_WORDS    25u
#define PSYQ_THUNK_STATE_BYTES    (PSYQ_THUNK_STATE_WORDS * 4u)
#define PSYQ_THUNK_STATE_V1_BYTES 96u

uint32_t psyq_thunk_state_size(void);
void     psyq_thunk_state_save(void* dst);   /* dst has PSYQ_THUNK_STATE_BYTES */
/* 0 applied; -1 left the latches alone (short blob or unknown pack_ver). */
int      psyq_thunk_state_load(const void* src, uint32_t size);

/* The layout Spyro 1's extras carried before the pack: callback vaddrs and one
 * word holding the armed bit, the filter file at bits 8..15 and the filter
 * channel at 16..23. The tables it never saved stay as they are. */
void     psyq_thunk_state_load_v4(uint32_t cd_cb, uint32_t vs_cb,
                                  uint32_t xa_word);

/* Round-trip check: save, overwrite every latch with a sentinel, load, save
 * again; the two images must match and the sentinel image must differ from
 * them. Returns 0 on pass. Run by OPENPETE_THUNK_STATE_SELFTEST=1 at boot. */
void     psyq_thunk_state_scramble(void);
int      psyq_thunk_state_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
