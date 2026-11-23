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
/* Host allocation passes: a pass is the allocations made inside host-initiated
 * guest calls during one openpete_frame, numbered from 1. Each pass keeps its
 * allocations in order (slot index, serial); the previous pass's list is the
 * identity source of the current one (moby_slot_stamp). */
#define HOST_PASS_MAX 64u
typedef struct {
    uint32_t pass;                    /* the pass number, 0 before the first */
    uint32_t n;                       /* allocations recorded */
    int      slot[HOST_PASS_MAX];
    uint32_t serial[HOST_PASS_MAX];
} host_pass_t;
static host_pass_t s_pass[2] OP_GUARDED_BY(sim);      /* thread-roles: the allocator hook on sim; [pass & 1] */
static uint32_t s_host_pass OP_GUARDED_BY(sim);       /* thread-roles: the allocator hook on sim */
static uint32_t s_host_pass_frame OP_GUARDED_BY(sim); /* thread-roles: the allocator hook on sim */
/* Moves a capture has not yet applied: a continued allocation in another
 * slot than its predecessor's, as slot addresses and the serial they carry. */
#define HOP_MAX 64u
static uint32_t s_hop_from[HOP_MAX] OP_GUARDED_BY(sim), s_hop_to[HOP_MAX] OP_GUARDED_BY(sim),
                s_hop_serial[HOP_MAX] OP_GUARDED_BY(sim); /* thread-roles: the allocator hook and the capture, on sim */
static uint32_t s_hop_n OP_GUARDED_BY(sim);           /* thread-roles: the allocator hook and the capture, on sim */

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

/** @brief Record a continued allocation that moved from slot `from` to slot `to` for the next capture, extending a pending move that ended at `from`.
 * @param from The predecessor's slot index.
 * @param to The slot index the allocation took.
 * @param serial The serial both carry. */
static void moby_hop_note(int from, int to, uint32_t serial) OP_REQUIRES(sim) {
    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t fa = s_slot_base + (uint32_t)from * stride, ta = s_slot_base + (uint32_t)to * stride;
    for (uint32_t k = 0; k < s_hop_n; ++k)
        if (s_hop_to[k] == fa && s_hop_serial[k] == serial) {
            s_hop_to[k] = ta;
            return;
        }
    if (s_hop_n >= HOP_MAX) return;
    s_hop_from[s_hop_n] = fa;
    s_hop_to[s_hop_n] = ta;
    s_hop_serial[s_hop_n] = serial;
    s_hop_n++;
}

/** @brief Whether the predecessor's slot still holds the predecessor: live (state below 0x80) and still stamped with its serial.
 * @param cpu Reads the slot's state byte.
 * @param slot The predecessor's slot index.
 * @param serial The predecessor's serial.
 * @return 1 when the predecessor is still allocated, else 0. */
static int moby_slot_still_holds(CPUState* cpu, int slot, uint32_t serial) OP_REQUIRES(sim) {
    if (s_slot_serial[slot] != serial) return 0;
    const uint32_t m = s_slot_base + (uint32_t)slot * game_layout.moby.array_stride;
    return cpu->read_byte(m + game_layout.moby.state_off) < 0x80u;
}

/** @brief Stamp slot `i` for the allocation the hook just ran: a new serial, or its predecessor's on a host continuation.
 *
 * A host continuation is the n-th allocation of a host allocation pass (the
 * allocations inside host-initiated guest calls, openpete_psxtrace_gate_host_active,
 * of one openpete_frame) where the previous pass made an n-th allocation
 * whose slot no longer holds it (freed, or reallocated since): draw-time
 * state a mod rebuilds every tick, never seen by the game's update. It takes
 * the predecessor's serial; when the allocator handed it another slot (the
 * allocator returns the lowest free slot, so a slot the game freed below the
 * predecessor's takes it), the move is kept for the next capture
 * (moby_serial_take_hops), which moves the record's history to the new
 * slot. Any other allocation, and every guest one, takes alloc_serial_next.
 * @param cpu Reads the predecessor slot's state byte.
 * @param i The slot index. */
static void moby_slot_stamp(CPUState* cpu, int i) OP_REQUIRES(sim) {
    if (!openpete_psxtrace_gate_host_active()) {
        s_slot_serial[i] = alloc_serial_next(&s_alloc_serial);
        return;
    }
    if (s_host_pass == 0u || s_host_pass_frame != openpete_frame) {
        s_host_pass++;
        s_host_pass_frame = openpete_frame;
        host_pass_t* fresh = &s_pass[s_host_pass & 1u];
        fresh->pass = s_host_pass;
        fresh->n = 0u;
    }
    host_pass_t* cur = &s_pass[s_host_pass & 1u];
    const host_pass_t* prev = &s_pass[(s_host_pass + 1u) & 1u];
    const uint32_t n = cur->n;
    int cont = 0;
    if (prev->pass != 0u && prev->pass + 1u == s_host_pass && n < prev->n) {
        const int from = prev->slot[n];
        const uint32_t serial = prev->serial[n];
        cont = from == i || !moby_slot_still_holds(cpu, from, serial);
        if (cont) {
            s_slot_serial[i] = serial;
            if (from != i) moby_hop_note(from, i, serial);
        }
    }
    if (!cont) s_slot_serial[i] = alloc_serial_next(&s_alloc_serial);
    if (n < HOST_PASS_MAX) {
        cur->slot[n] = i;
        cur->serial[n] = s_slot_serial[i];
        cur->n = n + 1u;
    }
}

uint32_t moby_serial_take_hops(uint32_t* from, uint32_t* to, uint32_t* serial, uint32_t cap) OP_REQUIRES(sim) {
    uint32_t n = s_hop_n < cap ? s_hop_n : cap;
    for (uint32_t k = 0; k < n; ++k) {
        from[k] = s_hop_from[k];
        to[k] = s_hop_to[k];
        serial[k] = s_hop_serial[k];
    }
    s_hop_n = 0u;
    return n;
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
        memset(s_pass, 0, sizeof s_pass);
        s_hop_n = 0u;
        s_slot_base = base;
    }
    const int i = moby_slot(cpu->v0);
    if (i >= 0) moby_slot_stamp(cpu, i);
    return 1;
}

int moby_pair_reborn(const openpete_interp_obj_t* o) {
    const uint32_t so = game_layout.moby.array_stride;
    if (!moby_serial_enabled() || !o->have_prev || o->curr_serial_off != so || o->prev_serial_off != so)
        return 0;
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

/* Host-hop check (OPENPETE_S1_HOST_HOP_TEST): the check moby, and per
 * present whether a sub-tick pass drew it. */
static _Atomic(uint32_t)      s_hp_moby;
static _Atomic(int)           s_hp_cls = -1;
static _Atomic(int64_t)       s_hp_present;
static _Atomic(int)           s_hp_drawn, s_hp_armed;
static _Atomic(unsigned long) s_hp_presents, s_hp_absent, s_hp_hops, s_hp_captured;

/** @brief Print the host-hop check totals; registered with atexit by the first hop_probe_set. */
static void hop_probe_report(void) {
    fprintf(stderr, "[host-hop-test] hops=%lu captured=%lu presents=%lu absent=%lu\n",
            (unsigned long)s_hp_hops, (unsigned long)s_hp_captured, (unsigned long)s_hp_presents,
            (unsigned long)s_hp_absent);
}

/** @brief Close the present the check last saw: count it, and count it absent when no pass drew the check moby. */
static void hop_probe_close(void) {
    if (!s_hp_present) return;
    s_hp_presents++;
    if (!s_hp_drawn) {
        s_hp_absent++;
        fprintf(stderr, "[host-hop-test] absent f=%u m=%08x\n", openpete_frame, (unsigned)s_hp_moby);
    }
    s_hp_present = 0;
    s_hp_drawn = 0;
}

void moby_serial_hop_probe_set(uint32_t moby, uint16_t cls, int expected) {
    hop_probe_close();
    int expect = 0;
    if (atomic_compare_exchange_strong(&s_hp_armed, &expect, 1)) atexit(hop_probe_report);
    s_hp_moby = expected ? moby : 0u;
    s_hp_cls = cls;
}

int moby_serial_hop_probe_target(uint32_t* moby, uint16_t* cls) {
    const uint32_t m = s_hp_moby;
    if (!m) return 0;
    *moby = m;
    *cls = (uint16_t)s_hp_cls;
    return 1;
}

void moby_serial_hop_probe_pass(int64_t present_ns, int drawn) {
    if (s_hp_present != present_ns) {
        hop_probe_close();
        s_hp_present = present_ns;
    }
    if (drawn) s_hp_drawn = 1;
}

void moby_serial_hop_probe_hop(uint32_t from, uint32_t to) {
    s_hp_hops++;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(to);
    uint16_t cls = 0;
    if (o) {
        s_hp_captured++;
        memcpy(&cls, o->curr + game_layout.moby.class_off, 2);
    }
    fprintf(stderr, "[host-hop-test] hop f=%u from=%08x to=%08x captured=%d cls=%03x\n", openpete_frame,
            (unsigned)from, (unsigned)to, o != NULL, cls);
}
