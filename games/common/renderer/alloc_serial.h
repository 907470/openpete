/* alloc_serial.h - allocation serials keyed to the guest timeline, shared by
 * the moby and particle slot allocator hooks.
 *
 * A serial names one allocation: the tick it ran in (openpete_frame) and its
 * ordinal among that tick's allocations of the same pool. Both are functions
 * of the guest timeline, so a tick that runs again from a restored state (a
 * runahead rollback, a rewind, a savestate load) hands its allocations the
 * serials its first run did, and a record captured at a speculated tick pairs
 * with the same record captured after the rollback re-runs that tick. The
 * ordinal restarts when the tick changes or a restore intervenes
 * (openpete_guest_restore_count). Two allocations of one timeline share a
 * serial only 2^20 ticks apart. */
#ifndef OPENPETE_RENDERER_ALLOC_SERIAL_H
#define OPENPETE_RENDERER_ALLOC_SERIAL_H

#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* One pool's serial source: the tick and restore count its ordinal counts in. */
typedef struct {
    uint32_t frame;      /* openpete_frame at the last allocation */
    uint32_t restores;   /* openpete_guest_restore_count at the last allocation */
    uint32_t ordinal;    /* allocations of this pool in that tick */
    int      seen;       /* 0 until the first allocation */
} alloc_serial_t;

/** @brief The serial of the allocation the pool's hook just observed: the tick in the high 20 bits, the 1-based ordinal in the low 12, never 0.
 *
 * Restarts the ordinal when openpete_frame or the guest restore count differs
 * from the previous allocation's. Read on the thread holding the `sim` role.
 * @param s The pool's serial source.
 * @return The serial; 0 is reserved for a slot with no allocation seen. */
uint32_t alloc_serial_next(alloc_serial_t* s) OP_REQUIRES_SHARED(sim);

#endif /* OPENPETE_RENDERER_ALLOC_SERIAL_H */
