/* core/include/openpete_bugthumbs.h - the bug-report thumbnail ring: a
 * low-resolution copy of a presented image every OPENPETE_BUGTHUMB_PERIOD_NS
 * of present clock, the newest OPENPETE_BUGTHUMB_SLOTS of them held in host
 * memory for a bug-report press.
 *
 * The thread that holds the display (the present thread while eager
 * production is active, the host loop otherwise) queues a downscale of the
 * offscreen through rhi_cycle_thumb on its present and stores each
 * completed thumbnail here (openpete_bugthumbs_put); the store is a ring of
 * fixed slots under one mutex, so the display's owner can change between
 * two puts without a handoff of its own. The press copies the ring under
 * the same mutex (openpete_bugthumbs_copy) and never touches the device.
 * OPENPETE_BUGREPORT_THUMBS=0 turns the ring off. Nothing stores into PSX
 * RAM. */
#ifndef OPENPETE_BUGTHUMBS_H
#define OPENPETE_BUGTHUMBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The box a thumbnail is fitted into at the presented image's aspect. */
#define OPENPETE_BUGTHUMB_W 320
#define OPENPETE_BUGTHUMB_H 180
/* Thumbnails held: OPENPETE_BUGTHUMB_SLOTS x the period = 5 s. */
#define OPENPETE_BUGTHUMB_SLOTS 20
#define OPENPETE_BUGTHUMB_PERIOD_NS 250000000ll

/* One held thumbnail. */
typedef struct {
    int      w, h;
    int64_t  present_ns;  /* the present clock of the present it shows */
    uint32_t feed_key;    /* the feed key (VBL) that present showed */
    uint8_t* rgba;        /* w*h*4 bytes, R,G,B,A */
} openpete_bugthumb_t;

/** @brief Report whether the thumbnail ring is on: OPENPETE_BUGREPORT_THUMBS unset or nonzero, read once.
 * @return 1 when on. */
int openpete_bugthumbs_enabled(void);

/** @brief Store a completed thumbnail in the ring's next slot, replacing the oldest when full.
 *
 * Called by the display's owner; copies the pixels under the ring's mutex.
 * A thumbnail larger than the box is dropped.
 * @param rgba w*h*4 bytes.
 * @param w Width.
 * @param h Height.
 * @param present_ns The present clock of the present it shows.
 * @param feed_key The feed key that present showed. */
void openpete_bugthumbs_put(const void* rgba, int w, int h, int64_t present_ns, uint32_t feed_key);

/** @brief Copy the held thumbnails, oldest first, into heap copies; any thread.
 * @param out Receives the array (free with openpete_bugthumbs_free); NULL when none.
 * @return The count. */
size_t openpete_bugthumbs_copy(openpete_bugthumb_t** out);

/** @brief Free an array openpete_bugthumbs_copy returned.
 * @param v The array; NULL is ignored.
 * @param n Its count. */
void openpete_bugthumbs_free(openpete_bugthumb_t* v, size_t n);

/** @brief Add one present's thumbnail-cycle time to the cost counters.
 * @param ns Wall nanoseconds the cycle took on the display's thread.
 * @param queued Nonzero when the present queued a downscale. */
void openpete_bugthumbs_note_cycle(uint64_t ns, int queued);

/** @brief Log the ring's counters: presents cycled, downscales queued, thumbnails stored, cycle time mean and max, bytes held. */
void openpete_bugthumbs_report(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGTHUMBS_H */
