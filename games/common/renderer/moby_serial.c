/* moby_serial.c - per-slot allocation serials of the moby array; see
 * moby_serial.h.
 *
 * The allocator (Spyro 2 asm/updatemobys.s func_80041290, Spyro 3
 * func_80055A7C) returns in v0 the slot at the array's free cursor, clears its
 * state byte (+0x48) and advances the cursor to the next free slot, or returns
 * 0 when the cursor sits on the last slot. The slot it returns may be one the
 * free routine (func_8004132C / func_80055B18) released earlier in the same
 * tick.
 *
 * Determinism: never writes PSX RAM. The guest allocator runs exactly once,
 * through the module dispatch, and the hook reads its return value and the
 * array head afterwards. */

#include "renderer/moby_serial.h"
#include "game_layout.h"
#include "openpete_recomp_module.h"

#include <string.h>
#include "openpete_thread_roles.h"

#define MOBY_SLOTS_MAX 4096u

static uint32_t s_slot_serial[MOBY_SLOTS_MAX];
static uint32_t s_slot_base;     /* the array base the serials index */
static uint32_t s_alloc_serial;  /* the last serial handed out */
static int      s_alloc_in;      /* the hook's re-entrancy latch */

int moby_serial_enabled(void) { return game_layout.moby.alloc != 0u; }

/** @brief The slot index of the moby record at `moby` in the array the serials index.
 *
 * @param moby The record's guest address.
 * @return The index, or -1 when no array base is known, the address is not a
 *         record boundary of it, or the index lies beyond the slots tracked. */
static int moby_slot(uint32_t moby) {
    const uint32_t stride = game_layout.moby.array_stride;
    if (!s_slot_base || !stride || moby < s_slot_base) return -1;
    const uint32_t off = moby - s_slot_base;
    if (off % stride) return -1;
    const uint32_t idx = off / stride;
    return idx < MOBY_SLOTS_MAX ? (int)idx : -1;
}

uint32_t moby_slot_serial(uint32_t moby) {
    const int i = moby_slot(moby);
    return i < 0 ? 0u : s_slot_serial[i];
}

/** @brief The moby array's base as the capture walk reads it (game_layout.moby.array_head, direct or through its pointer).
 *
 * @param cpu Reads the head pointer.
 * @return The base, or 0 when it is not a KSEG0 RAM address. */
static uint32_t moby_array_base(CPUState* cpu) {
    const uint32_t head = game_layout.moby.array_head;
    const uint32_t base = game_layout.moby.array_direct ? head : cpu->read_word(head);
    return (base >= 0x80000000u && base < 0x80800000u) ? base : 0u;
}

int moby_serial_on_alloc(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    if (s_alloc_in) return 0;
    s_alloc_in = 1;
    openpete_recomp_module_get()->dispatch(cpu, addr);
    s_alloc_in = 0;
    const uint32_t base = moby_array_base(cpu);
    if (base != s_slot_base) {
        memset(s_slot_serial, 0, sizeof s_slot_serial);
        s_slot_base = base;
    }
    const int i = moby_slot(cpu->v0);
    if (i >= 0) {
        if (++s_alloc_serial == 0u) s_alloc_serial = 1u;   /* 0 means none seen */
        s_slot_serial[i] = s_alloc_serial;
    }
    return 1;
}
