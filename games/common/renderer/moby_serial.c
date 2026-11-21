/* moby_serial.c - per-slot allocation serials of the moby array; see
 * moby_serial.h.
 *
 * The allocator (Spyro 1 asm/42CC4.s func_800524C4, Spyro 2
 * asm/updatemobys.s func_80041290, Spyro 3 func_80055A7C) returns in v0 the
 * slot at the array's free cursor, clears its state byte (+0x48) and advances
 * the cursor to the next free slot, or returns 0 when the cursor sits on the
 * last slot (Spyro 1 has no full test). The
 * slot it returns may be one the free routine (func_80052568 /
 * func_8004132C / func_80055B18) released earlier in the same tick.
 *
 * Determinism: never writes PSX RAM. The guest allocator runs exactly once,
 * through the module dispatch, and the hook reads its return value and the
 * array head afterwards. */

#include "renderer/moby_serial.h"
#include "renderer/alloc_serial.h"
#include "game_layout.h"
#include "openpete_recomp_module.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_fiber.h"         /* openpete_frame */
#include "openpete_psxtrace.h"      /* openpete_psxtrace_gate_host_active */
#include "openpete_pace_log.h"      /* OP_PACE_SPLIT_SCOPE: the guest call's split */
#include "openpete_profile.h"       /* the guest call's profiler region */
#include "openpete_thread_roles.h"

#define MOBY_SLOTS_MAX 4096u

static uint32_t s_slot_serial[MOBY_SLOTS_MAX];
static uint32_t s_slot_base;     /* the array base the serials index */
static alloc_serial_t s_alloc_serial;  /* the array's serial source */
static int      s_alloc_in;      /* the hook's re-entrancy latch */
/* Host allocation passes: a pass is the host-call allocations of one
 * openpete_frame, numbered from 1. A slot's entry is the pass that last
 * allocated it from a host call, 0 after a guest allocation of it. */
static uint32_t s_slot_host_pass[MOBY_SLOTS_MAX] OP_GUARDED_BY(sim); /* thread-roles: written by the allocator hook on sim */
static uint32_t s_host_pass OP_GUARDED_BY(sim);       /* thread-roles: the allocator hook on sim */
static uint32_t s_host_pass_frame OP_GUARDED_BY(sim); /* thread-roles: the allocator hook on sim */

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

/** @brief Stamp slot `i` for the allocation the hook just ran: a new serial, or the slot's own on a host continuation.
 *
 * A host continuation is an allocation inside a host-initiated guest call
 * (openpete_psxtrace_gate_host_active) of a slot the previous host
 * allocation pass also allocated: draw-time state a mod rebuilds every tick
 * in the same slot, never seen by the game's update. Any other allocation,
 * and every guest one, takes alloc_serial_next.
 * @param i The slot index. */
static void moby_slot_stamp(int i) OP_REQUIRES(sim) {
    if (!openpete_psxtrace_gate_host_active()) {
        s_slot_host_pass[i] = 0u;
        s_slot_serial[i] = alloc_serial_next(&s_alloc_serial);
        return;
    }
    if (s_host_pass == 0u || s_host_pass_frame != openpete_frame) {
        s_host_pass++;
        s_host_pass_frame = openpete_frame;
    }
    const int cont = s_slot_host_pass[i] != 0u && s_slot_host_pass[i] + 1u == s_host_pass;
    if (!cont) s_slot_serial[i] = alloc_serial_next(&s_alloc_serial);
    s_slot_host_pass[i] = s_host_pass;
}

int moby_serial_on_alloc(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    if (s_alloc_in) return 0;
    s_alloc_in = 1;
    { OP_PROF_FLAGS_ACCUM_SCOPE("moby.alloc", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      openpete_recomp_module_get()->dispatch(cpu, addr); }
    s_alloc_in = 0;
    const uint32_t base = moby_array_base(cpu);
    if (base != s_slot_base) {
        memset(s_slot_serial, 0, sizeof s_slot_serial);
        memset(s_slot_host_pass, 0, sizeof s_slot_host_pass);
        s_slot_base = base;
    }
    const int i = moby_slot(cpu->v0);
    if (i >= 0) moby_slot_stamp(i);
    return 1;
}

int moby_pair_reborn(const openpete_interp_obj_t* o) {
    const uint32_t so = game_layout.moby.array_stride;
    if (!moby_serial_enabled() || !o->have_prev || o->blob_len < so + 4u) return 0;
    return memcmp(o->prev + so, o->curr + so, 4u) != 0;
}

/* Allocation census: per slot, the curr stamp of the pair each verdict last
 * counted, so every (slot, tick) counts once per verdict across presents. */
static _Atomic(int)           s_cen_on = -1;
static _Atomic(unsigned long) s_cen_lerped, s_cen_reuses, s_cen_reborn_lerped, s_cen_reborn_held;
static _Atomic(op_time_t)     s_cen_lerp_ns[MOBY_SLOTS_MAX], s_cen_reuse_ns[MOBY_SLOTS_MAX],
                              s_cen_rlerp_ns[MOBY_SLOTS_MAX], s_cen_rheld_ns[MOBY_SLOTS_MAX];

/** @brief Print the allocation census totals; registered with atexit by the census's first call. */
static void moby_serial_census_report(void) {
    fprintf(stderr, "[moby-serial-census] lerped=%lu reuses=%lu reborn_lerped=%lu reborn_held=%lu\n",
            (unsigned long)s_cen_lerped, (unsigned long)s_cen_reuses,
            (unsigned long)s_cen_reborn_lerped, (unsigned long)s_cen_reborn_held);
}

/** @brief Count one (slot, tick) under a verdict once: 1 when `stamp` first sees `ns` for the slot.
 *
 * @param stamp The verdict's per-slot stamps.
 * @param i The slot index.
 * @param ns The pair's curr stamp.
 * @return 1 on the first sighting, else 0. */
static int cen_first(_Atomic(op_time_t)* stamp, int i, op_time_t ns) {
    return atomic_exchange(&stamp[i], ns) != ns;
}

void moby_serial_census(uint32_t moby, const openpete_interp_obj_t* o, double alpha, int held) {
    int on = s_cen_on;
    if (on < 0) {
        const char* e = getenv("OPENPETE_MOBY_SERIAL_CENSUS");
        on = (e && *e && *e != '0') ? 1 : 0;
        int expect = -1;
        if (atomic_compare_exchange_strong(&s_cen_on, &expect, on) && on) atexit(moby_serial_census_report);
    }
    const int i = moby_slot(moby);
    if (!on || i < 0) return;
    const int sub = !held && alpha > 0.0 && alpha < 1.0;
    if (sub && cen_first(s_cen_lerp_ns, i, o->curr_ns)) s_cen_lerped++;
    if (!moby_pair_reborn(o)) return;
    if (cen_first(s_cen_reuse_ns, i, o->curr_ns)) s_cen_reuses++;
    int verdict = 0;
    if (held && cen_first(s_cen_rheld_ns, i, o->curr_ns)) { s_cen_reborn_held++; verdict = 1; }
    if (sub && cen_first(s_cen_rlerp_ns, i, o->curr_ns)) { s_cen_reborn_lerped++; verdict = 2; }
    if (!verdict) return;
    uint16_t cls;
    memcpy(&cls, o->curr + game_layout.moby.class_off, 2);
    fprintf(stderr, "[moby-serial-census] f=%u m=%08x cls=%03x verdict=%s a=%.3f\n",
            openpete_frame, moby, cls, verdict == 2 ? "lerp" : "held", alpha);
}
