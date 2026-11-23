/* fakevk.h - a fake Vulkan device beneath rhi_vulkan.c for the present-thread
 * interleaving harness.
 *
 * The harness defines the volk function pointers rhi_vulkan.o references
 * (vk_list.h, generated from its undefined symbols) and volkInitialize
 * installs this device. A pointer the device does not model is a trap that
 * aborts naming the function. The modelled synchronisation entry points
 * (acquire, submit, present, fence waits and status, present waits, device
 * and queue idle, swapchain creation and destruction) are yields of the
 * scheduler, take outcome and duration choices from the scenario's domain,
 * block in virtual time, and check the Vulkan rules the protocol relies on;
 * a broken rule fails the schedule. A GPU actor completes submissions in
 * order at chosen virtual times, and FIFO flips put queued images on screen
 * at vblanks. */
#ifndef PT_FAKEVK_H
#define PT_FAKEVK_H

#include <stdint.h>

/** Outcome bits of a modelled call a scenario may choose besides success. */
enum {
    FK_OUT_SUBOPTIMAL = 1u << 0,
    FK_OUT_TIMEOUT    = 1u << 1,
    FK_OUT_NOT_READY  = 1u << 2,
    FK_OUT_OUT_OF_DATE = 1u << 3,
    FK_OUT_DEVICE_LOST = 1u << 4,
};

/** Modelled calls that take outcome choices. */
enum { FK_CALL_ACQUIRE, FK_CALL_SUBMIT, FK_CALL_PRESENT, FK_CALL_FENCE_WAIT, FK_CALL_PRESENT_WAIT, FK_CALL_KINDS };

/** The scenario's choice domain for the fake device. */
typedef struct fakevk_domain {
    unsigned outcomes[FK_CALL_KINDS]; /**< FK_OUT_* bits offered per call */
    int64_t gpu_ns[4];                /**< submission latencies; [0] is the default */
    int n_gpu;                        /**< latencies offered (at least 1) */
    int64_t vblank_ns;                /**< display refresh period */
} fakevk_domain;

/** What one present showed. */
typedef struct fakevk_shown {
    uint32_t content;   /**< scene id or inline tag the image carries */
    int fresh;          /**< the image was written since its acquire (else a repeat: a stage release) */
    int presenter;      /**< scheduler id of the presenting thread */
    int acquirer;       /**< scheduler id of the thread that acquired the image */
    int result;         /**< VkResult returned */
} fakevk_shown;

/** Hooks the harness installs: every modelled call, and every present. */
typedef struct fakevk_hooks {
    void (*call)(const char *name, const void *obj);
    void (*shown)(const fakevk_shown *s);
} fakevk_hooks;

/** @brief Set the scenario's choice domain; call in the child before the scenario's threads run.
 * @param d The domain (copied). */
void fakevk_set_domain(const fakevk_domain *d);

/** @brief Install the harness hooks.
 * @param h The hooks (copied). */
void fakevk_set_hooks(const fakevk_hooks *h);

/** @brief Set the window size every reader sees (SDL_GetWindowSizeInPixels and the surface extent).
 * @param w Width in pixels; 0 for a minimised window.
 * @param h Height in pixels. */
void fakevk_set_window(int w, int h);

/** @brief Read the window size.
 * @param w Receives the width.
 * @param h Receives the height. */
void fakevk_window(int *w, int *h);

/** @brief Name the content a compose writes: the scene id (ring) the offscreen target holds until the next set.
 * @param content Scene id, or an inline tag. */
void fakevk_set_render(uint32_t content);

/** @brief Set the tag an inline compose writes.
 * @param tag The tag. */
void fakevk_set_inline_tag(uint32_t tag);

/** @brief Return the tag an inline compose writes.
 * @return The tag. */
uint32_t fakevk_inline_tag(void);

/** @brief Map an RHI frame serial to the most recent submission, when one was made since the last fakevk_set_render.
 * @param serial The serial of the frame just ended. */
void fakevk_frame_done(uint64_t serial);

/** @brief Report whether the submission that carried an RHI frame serial has completed on the fake GPU.
 * @param serial The serial.
 * @return 1 complete, 0 still executing, -1 no submission carried it. */
int fakevk_serial_complete(uint64_t serial);

/** @brief Return the images of the current swapchain acquired and neither presented nor given back.
 * @return The count. */
int fakevk_acquired_outstanding(void);

/** @brief Report whether the surface can take a present: a nonzero window and no device loss.
 * @return 1 when presentable. */
int fakevk_presentable(void);

/** @brief Return the acquires that handed out no image (any outcome but success or suboptimal).
 * @return The count. */
unsigned long fakevk_acquire_refusals(void);

/** @brief Force the outcome of the k-th call of a kind (counted from 0 in the schedule), in place of a choice.
 * @param kind FK_CALL_*.
 * @param ordinal Which call of that kind.
 * @param result The VkResult to return. */
void fakevk_force(int kind, int ordinal, int result);

/** @brief Report whether the device was lost (every later call returns VK_ERROR_DEVICE_LOST).
 * @return 1 when lost. */
int fakevk_lost(void);

/** @brief Return the virtual time the calling thread has spent inside modelled calls.
 * @param thread Scheduler thread id.
 * @return Nanoseconds. */
int64_t fakevk_thread_call_ns(int thread);

/** @brief Return the number of distinct modelled calls reached, and print them when verbose.
 * @param verbose Print each with its count.
 * @return The count. */
int fakevk_report(int verbose);

/** @brief Raise a display change on the window, as the event pump does (calls the RHI's event watch). */
void fakevk_display_change(void);

/** @brief Return the address of the device's display-change flag, found by probing the RHI's event watch before the first fork.
 * @return The address, or NULL when the watch was never registered. */
const void *fakevk_display_changed_addr(void);

#endif
