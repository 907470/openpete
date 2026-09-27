#pragma once
/* transition_call.h — one queue for host-requested calls into the game's own
 * transition functions (WarpToLevel func_800584BC, EnterSubArea
 * func_80052918), made in guest context at the entry of Draw
 * func_8001E638, after the tick's whole Update, as the game's own triggers
 * call them from inside Update.
 *
 * A request carries the function and its two arguments, the constant stores
 * and guest calls the game's trigger makes before its call, a route
 * predicate re-read at the drain, and completion callbacks. The queue holds
 * one request. Enqueueing writes no guest memory; every store happens in the
 * drain, on a non-speculative tick. */

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;

/* Guest functions a request calls. */
#define S3_FN_WARP_TO_LEVEL  0x800584BCu
#define S3_FN_ENTER_SUB_AREA 0x80052918u

/* One store made before the call: `size` 1, 2 or 4 bytes; with `or_mask`
 * nonzero the value is read, ORed with `value` and written back. */
typedef struct {
    uint32_t addr;
    uint8_t  size;
    uint8_t  or_mask;
    uint32_t value;
} s3_transition_store_t;

/* The function entry a request drains at. */
typedef enum {
    S3_TRANSITION_AT_DRAW = 0,  /* Draw 0x8001E638 */
} s3_transition_entry_t;

typedef struct {
    const char*                  tag;        /* log tag */
    s3_transition_entry_t        entry;
    uint32_t                     fn;         /* guest function address */
    uint32_t                     a0, a1;
    const uint32_t*              pre_calls;  /* guest calls before the stores */
    size_t                       n_pre_calls;
    const s3_transition_store_t* stores;
    size_t                       n_stores;
    /* Re-read at the drain; 0 drops the request. */
    int  (*route)(CPUState* cpu);
    /* Run after the call returns, or at a drop. Nullable. */
    void (*called)(CPUState* cpu);
    void (*dropped)(CPUState* cpu);
} s3_transition_request_t;

/** @brief Queues a transition request for the next drain at its entry.
 *
 * The request is copied; the arrays it points to must outlive the drain. The
 * queue holds one request.
 * @param req The request.
 * @return 1 when queued, 0 when a request is already pending. */
int s3_transition_enqueue(const s3_transition_request_t* req);

/** @brief Reports whether a request is queued and not yet drained.
 *
 * @return 1 while a request is pending, else 0. */
int s3_transition_pending(void);
