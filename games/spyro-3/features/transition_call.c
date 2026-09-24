// games/spyro-3/features/transition_call.c — the drain of host-requested
// calls into the game's transition functions. See transition_call.h.
//
// The game's triggers (portal mobys, GoToLevel func_80053F50, the title
// update) call WarpToLevel and EnterSubArea from inside Update func_80055400,
// and main then calls Draw func_8001E638 on the same tick. The drain runs at
// Draw's entry, so the call follows the tick's whole Update and Draw reads
// the transition's stores as it does after the game's own call. A hook at
// Update's entry would instead replace that tick's gameplay update with the
// loading dispatch.
//
// The route predicate is re-read at the drain because the tick's Update may
// have paused or started a transition of its own since the request was
// queued; a request that fails it is dropped with a log line.

#include "features/transition_call.h"

#include "feature_registry.h"
#include "openpete_fiber.h"          /* openpete_speculative */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read/write callbacks */

#define ADDR_DRAW 0x8001E638u

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

static s3_transition_request_t s_req;
static int                     s_pending = 0;

int s3_transition_enqueue(const s3_transition_request_t* req) {
    if (s_pending) return 0;
    s_req     = *req;
    s_pending = 1;
    return 1;
}

int s3_transition_pending(void) { return s_pending; }

static void store(CPUState* cpu, const s3_transition_store_t* s) {
    uint32_t v = s->value;
    if (s->size == 1u) {
        if (s->or_mask) v |= cpu->read_byte(s->addr);
        cpu->write_byte(s->addr, (uint8_t)v);
    } else if (s->size == 2u) {
        if (s->or_mask) v |= cpu->read_half(s->addr);
        cpu->write_half(s->addr, (uint16_t)v);
    } else {
        if (s->or_mask) v |= cpu->read_word(s->addr);
        cpu->write_word(s->addr, v);
    }
}

/* Draw takes no arguments and this is its entry, so only the return address
 * is live across the nested calls. */
static void call_guest(CPUState* cpu, uint32_t fn, uint32_t a0, uint32_t a1) {
    uint32_t ra = cpu->ra;
    cpu->a0 = a0;
    cpu->a1 = a1;
    call_by_address(cpu, fn);
    cpu->ra = ra;
}

static int on_draw(CPUState* cpu, uint32_t addr) {
    size_t i;
    s3_transition_request_t r;
    (void)addr;
    if (!s_pending || s_req.entry != S3_TRANSITION_AT_DRAW) return 0;
    /* A speculative (runahead) tick's guest writes roll back; the host-side
     * dequeue would not. */
    if (openpete_speculative) return 0;

    r         = s_req;
    s_pending = 0;
    if (r.route && !r.route(cpu)) {
        OPL_INFO_F(r.tag, "request dropped at the drain: route closed "
                   "(game.state %u)", (unsigned)cpu->read_word(0x8006E344u));
        if (r.dropped) r.dropped(cpu);
        return 0;
    }
    for (i = 0; i < r.n_pre_calls; i++) call_guest(cpu, r.pre_calls[i], 0u, 0u);
    for (i = 0; i < r.n_stores; i++) store(cpu, &r.stores[i]);
    call_guest(cpu, r.fn, r.a0, r.a1);
    OPL_INFO_F(r.tag, "called 0x%08X(0x%X, 0x%X) at vbl=%u", (unsigned)r.fn,
               (unsigned)r.a0, (unsigned)r.a1, (unsigned)openpete_frame);
    if (r.called) r.called(cpu);
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_DRAW, on_draw },
    { 0, NULL },
};

const feature_module_t transition_call_module = {
    .name           = "transition-call",
    .apply_options  = NULL,
    .dispatch_table = k_dispatch,
};
