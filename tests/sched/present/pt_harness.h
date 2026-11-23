/* pt_harness.h - the present-thread harness's engagement counters and the
 * notes its stubs and compose stand-in send it. */
#ifndef PT_HARNESS_H
#define PT_HARNESS_H

#include <stdint.h>

/** Seam sites of the protocol, counted across every schedule. */
typedef enum {
    PT_SITE_PT_LOCK, PT_SITE_PT_UNLOCK, PT_SITE_PT_COND_WAIT, PT_SITE_PT_SIGNAL, PT_SITE_PT_JOIN,
    PT_SITE_PT_CLOCK, PT_SITE_PT_SLEEP, PT_SITE_PT_ACTIVE_UNLOCKED, PT_SITE_PT_REWARM,
    PT_SITE_CLAIM_ACTIVATION, PT_SITE_CLAIM_DRAIN, PT_SITE_STAGE_ACQUIRE, PT_SITE_RENDER_STAGED,
    PT_SITE_RENDER_UNSTAGED, PT_SITE_STAGE_PRESENT, PT_SITE_STAGE_RELEASE, PT_SITE_FLIP_WAIT,
    PT_SITE_WINDOW_COMPOSE, PT_SITE_WINDOW_RHI, PT_SITE_RING_LOCK, PT_SITE_RING_UNLOCKED,
    PT_SITE_VK_ACQUIRE, PT_SITE_VK_FENCE_WAIT, PT_SITE_VK_FENCE_POLL, PT_SITE_VK_SUBMIT, PT_SITE_VK_PRESENT,
    PT_SITE_VK_REBUILD, PT_SITE_SDL_TICKS, PT_SITE_DISPLAY_OWNER, PT_SITE_DISPLAY_CHANGED,
    PT_SITE_STATE_UNLOCKED, PT_SITE_COUNT
} pt_site;

/** Objects naming the clock seams in the trace and the step hook (stubs.c). */
extern const char pt_seam_op_now[], pt_seam_op_sleep[], pt_seam_window[];

/** @brief Count one hit of a seam site in the suite-wide counters.
 * @param site The site. */
void pt_site_hit(pt_site site);

/** @brief Note a compose about to record: the scene (or inline tag) and its stage target.
 * @param content Scene id, or an inline tag.
 * @param stage Stage image, or -1 for a swapchain present. */
void pt_note_compose(uint32_t content, int stage);

/** @brief Note an op_sleep_until call that blocks or returns at once. */
void pt_note_sleep(void);

#endif
