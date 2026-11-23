// games/spyro-1/features/host_hop_test.c - the host-hop check: a moby a
// host call allocates at every draw and frees before the next update, whose
// slot moves onto a game moby's slot on a schedule.
//
// OPENPETE_S1_HOST_HOP_TEST=<period>[,<class>] (period in ticks, >= 2;
// class default 342) arms it; unset, the module writes nothing. At the head
// of each CameraUpdateMatrices (0x80033C50, the draw's first call) on a
// playing tick it allocates a moby through func_800524C4 (42CC4.s) from a
// host-initiated guest call, copies into it the record of the first live
// moby of the class the last draw drew (the copy's position raised 0x400,
// its collision cell +0x34 set to -1 so the free walks no chain, its drawn
// byte cleared), and names it to the host-hop instrument
// (moby_serial_hop_probe_set). At the head of GamestateUpdate (0x8003385C)
// it frees the moby through func_80052568, so the update never sees it.
// Every <period> ticks, before the allocation, it frees the live dynamic
// moby (at or above g_DynMobys, not Sparx) nearest below the check moby's
// previous slot: the free moves the allocator cursor down to that slot, so
// the check moby takes a slot the game drew on the previous tick. A slot
// the game spawns into while the check moby is freed moves it the same way.
// Each allocation into another slot than the previous tick's is a hop.
//
// Test-only: it stores into guest RAM (the copied record, the freed moby),
// which changes gameplay state; never set in a gated or shipped run.

#include "feature_registry.h"
#include "game_layout.h"
#include "openpete_psxtrace.h"     /* openpete_psxtrace_gate_host_enter: a host-initiated guest call */
#include "psx_runtime_internal.h"  /* CPUState, call_by_address */
#include "renderer/moby_serial.h"  /* the host-hop instrument */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

#define ADDR_CAMERA_UPDATE_MATRICES 0x80033C50u
#define ADDR_GAMESTATE_UPDATE       0x8003385Cu
#define ADDR_MOBY_ALLOC             0x800524C4u
#define ADDR_MOBY_FREE              0x80052568u
#define ADDR_G_GAMESTATE            0x800757D8u /* GS_Playing 0 */
#define MOBY_COLL_CELL              0x34u       /* lh: < 0 = in no collision chain */
#define ADDR_G_DYNMOBYS             0x80075890u /* g_DynMobys: the first slot func_80052568 returns to the pool */
#define SPARX_CLASS                 120u
#define PROBE_RAISE                 0x400       /* world units added to the copy's z */
#define PROBE_CLASS                 342u        /* default source class: drawn by the regular moby pass in Artisans (level 10) */

static int      s_period OP_GUARDED_BY(sim) = -1;  /* thread-roles: the hooks on sim; ticks between hops, 0 = off */
static uint16_t s_cls OP_GUARDED_BY(sim);          /* thread-roles: the hooks on sim; the source class, the check moby's */
static uint32_t s_probe OP_GUARDED_BY(sim);        /* thread-roles: the hooks on sim; the check moby's slot while allocated */
static uint32_t s_prev_slot OP_GUARDED_BY(sim);    /* thread-roles: the hooks on sim; the slot the previous draw allocated */
static unsigned s_draws OP_GUARDED_BY(sim);        /* thread-roles: the hooks on sim */

/** @brief The hop period and source class from OPENPETE_S1_HOST_HOP_TEST (`<period>[,<class>]`), read once.
 * @return The period in ticks, or 0 when the check is off. */
static int hop_period(void) OP_REQUIRES(sim) {
    if (s_period < 0) {
        const char* e = getenv("OPENPETE_S1_HOST_HOP_TEST");
        char* end = NULL;
        const long v = e ? strtol(e, &end, 0) : 0;
        s_period = v >= 2 ? (int)v : 0;
        s_cls = (end && *end == ',') ? (uint16_t)strtol(end + 1, NULL, 0) : (uint16_t)PROBE_CLASS;
    }
    return s_period;
}

/** @brief Run one guest function from host code with one argument, keeping every general register, hi and lo of the interrupted call.
 * @param cpu The guest CPU state.
 * @param addr The function's address.
 * @param a0 Its argument.
 * @return The function's v0. */
static uint32_t host_call(CPUState* cpu, uint32_t addr, uint32_t a0) {
    uint8_t saved[offsetof(CPUState, cop0)];
    memcpy(saved, cpu, sizeof saved);
    cpu->a0 = a0;
    openpete_psxtrace_gate_host_enter();
    call_by_address(cpu, addr);
    openpete_psxtrace_gate_host_leave();
    const uint32_t v0 = cpu->v0;
    memcpy(cpu, saved, sizeof saved);
    return v0;
}

/** @brief The moby array's base: game_layout.moby.array_head read through its pointer.
 * @param cpu Reads the head.
 * @return The base, or 0 outside KSEG0 RAM. */
static uint32_t array_base(CPUState* cpu) {
    const uint32_t b = cpu->read_word(game_layout.moby.array_head);
    return (b >= 0x80000000u && b < 0x80200000u) ? b : 0u;
}

/** @brief Find the copy source, the first live moby of the source class the last draw drew, and the hop victim, the live dynamic moby (at or above g_DynMobys, not Sparx) nearest below `below`.
 * @param cpu Reads the array.
 * @param below The victim's bound (0: no victim wanted).
 * @param victim Receives the victim, or 0.
 * @return The source, or 0 when no such moby is live and drawn. */
static uint32_t scan_array(CPUState* cpu, uint32_t below, uint32_t* victim) OP_REQUIRES(sim) {
    const uint32_t base = array_base(cpu), stride = game_layout.moby.array_stride;
    const uint32_t dyn = cpu->read_word(ADDR_G_DYNMOBYS);
    uint32_t src = 0;
    *victim = 0;
    if (!base) return 0;
    for (uint32_t m = base, n = 0; n < 4096u; ++n, m += stride) {
        const uint8_t st = cpu->read_byte(m + game_layout.moby.state_off);
        if (st == (uint8_t)game_layout.moby.state_term) break;
        if (st >= 0x80u) continue;
        const uint16_t cls = cpu->read_half(m + game_layout.moby.class_off);
        if (!src && cls == s_cls && cpu->read_byte(m + game_layout.moby.drawn_off)) src = m;
        if (below && m >= dyn && m < below && cls != s_cls && cls != SPARX_CLASS) *victim = m;
    }
    return src;
}

/** @brief Draw head: free the scheduled victim, allocate the check moby from a host call and fill it from a drawn gem.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return 0, so the guest function runs. */
static int on_draw(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    if (!hop_period() || s_probe || cpu->read_word(ADDR_G_GAMESTATE) != 0u) return 0;
    const int hop_due = s_prev_slot && (++s_draws % (unsigned)s_period) == 0u;
    uint32_t victim;
    const uint32_t src = scan_array(cpu, hop_due ? s_prev_slot : 0u, &victim);
    if (!src) { s_prev_slot = 0; moby_serial_hop_probe_set(0u, 0u, 0); return 0; }
    if (victim) host_call(cpu, ADDR_MOBY_FREE, victim);
    const uint32_t slot = host_call(cpu, ADDR_MOBY_ALLOC, 0u);
    if (slot < 0x80000000u) return 0;
    for (uint32_t o = 4u; o < game_layout.moby.array_stride; o += 4u)
        cpu->write_word(slot + o, cpu->read_word(src + o));
    cpu->write_half(slot + MOBY_COLL_CELL, 0xFFFFu);
    cpu->write_byte(slot + game_layout.moby.drawn_off, 0u);
    const uint32_t z = slot + game_layout.moby.pos_off + 8u;
    cpu->write_word(z, cpu->read_word(z) + PROBE_RAISE);
    s_probe = slot;
    if (s_prev_slot && slot != s_prev_slot) moby_serial_hop_probe_hop(s_prev_slot, slot);
    moby_serial_hop_probe_set(slot, s_cls, s_prev_slot != 0u);
    s_prev_slot = slot;
    return 0;
}

/** @brief Update head: free the check moby, so the update never sees it.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return 0, so the guest GamestateUpdate runs. */
static int on_update(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    (void)addr;
    if (!hop_period() || !s_probe) return 0;
    host_call(cpu, ADDR_MOBY_FREE, s_probe);
    s_probe = 0;
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_CAMERA_UPDATE_MATRICES, on_draw, 0u },
    { ADDR_GAMESTATE_UPDATE,       on_update, 0u },
    { 0, NULL, 0u },
};

const feature_module_t host_hop_test_module = {
    .name           = "host-hop-test",
    .apply_options  = NULL,
    .dispatch_table = k_dispatch,
};
