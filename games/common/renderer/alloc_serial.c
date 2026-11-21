/* alloc_serial.c - allocation serials keyed to the guest timeline; see
 * alloc_serial.h.
 *
 * Determinism: reads openpete_frame and the restore count, never PSX RAM.
 * Its callers, the allocator hooks, run on the thread holding `sim`. */

#include "renderer/alloc_serial.h"
#include "openpete_fiber.h"          /* openpete_frame, openpete_guest_restore_count */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#define ALLOC_SERIAL_ORD_BITS 12u
#define ALLOC_SERIAL_ORD_MASK ((1u << ALLOC_SERIAL_ORD_BITS) - 1u)

uint32_t alloc_serial_next(alloc_serial_t* s) OP_REQUIRES_SHARED(sim) {
    const uint32_t frame = openpete_frame, restores = openpete_guest_restore_count();
    if (!s->seen || frame != s->frame || restores != s->restores) {
        s->frame = frame;
        s->restores = restores;
        s->ordinal = 0u;
        s->seen = 1;
    }
    s->ordinal++;
    const uint32_t v = (frame << ALLOC_SERIAL_ORD_BITS) | (s->ordinal & ALLOC_SERIAL_ORD_MASK);
    return v ? v : 1u;
}
