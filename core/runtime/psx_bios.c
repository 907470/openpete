/* psx_bios.c — the PSX BIOS A0/B0/C0 call gates.
 *
 * A game reaches a BIOS service through a fixed trampoline: it loads the
 * table address into $t2 and the function number into $t1, then jumps to
 * $t2. The recompiler turns that into a call to an address of 0xA0, 0xB0 or
 * 0xC0, which resolves to no translated function, because the BIOS is not
 * part of the recompiled image.
 *
 * PSYQ's libraries mostly wrap these services, and those wrappers are
 * intercepted by name in the thunk layer, so a game that reaches the memory
 * card through libmcrd never arrives here. A game that drives the BIOS card
 * API itself does: it opens an event on the HwCARD class, issues frame reads
 * and writes, and waits for the event its handler receives on completion.
 *
 * The card frames come from the same host-backed images the libmcrd surface
 * uses (core/host/memcard.c), so both routes see one card.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"

/* Event classes and specs from libapi.h. */
#define EV_CLASS_CARD   0xF0000011u   /* HwCARD */
#define EV_CLASS_CARD_BU 0xF4000001u  /* the bu driver's own class */
#define EV_SPEC_IOE     0x0004u       /* EvSpIOE — transfer finished */
#define EV_SPEC_ERROR   0x8000u       /* EvSpERROR */
#define EV_SPEC_TIMOUT  0x0100u       /* EvSpTIMOUT */
#define EV_SPEC_NEW     0x0200u       /* EvSpNEW — card newly inserted */
#define EV_MODE_INTR    0x1000u       /* EvMdINTR — run the handler */

#define EV_MAX          32
#define EV_HANDLE_BASE  0xF1000000u

struct bios_event {
    uint32_t cls, spec, mode, func;
    int      open, enabled, delivered;
};
static struct bios_event s_ev[EV_MAX];

/* The channel the last card command addressed, reported by _card_chan. */
static uint32_t s_card_chan = 0u;

/* Census: with OPENPETE_BIOS_CENSUS=1 every distinct (table, function) pair
 * is logged with the arguments it carried, so a service this gate does not
 * implement is named rather than silently returning zero. */
#define BIOS_SEEN_MAX 128
static uint32_t s_seen[BIOS_SEEN_MAX];
static int s_census = -1;

static void bios_census(CPUState* cpu, uint32_t table, uint32_t fn) {
    if (s_census < 0) {
        const char* e = getenv("OPENPETE_BIOS_CENSUS");
        s_census = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!s_census) return;
    uint32_t key = (table << 8) | (fn & 0xFFu);
    for (int i = 0; i < BIOS_SEEN_MAX; i++) {
        if (s_seen[i] == key) return;
        if (s_seen[i] == 0u) {
            s_seen[i] = key;
            OPL_INFO_F("bios", "0x%02X(0x%02X) a0=0x%08X a1=0x%08X a2=0x%08X "
                               "a3=0x%08X ra=0x%08X",
                       table, fn, cpu->a0, cpu->a1, cpu->a2, cpu->a3, cpu->ra);
            return;
        }
    }
}

static void invoke_guest(CPUState* cpu, uint32_t addr) {
    if (!addr) return;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2, a3 = cpu->a3;
    uint32_t ra = cpu->ra, v0 = cpu->v0, v1 = cpu->v1;
    cpu->ra = 0xDEADBEEFu;
    if (!openpete_recomp_module_get()->dispatch(cpu, addr)) {
        OPL_WARN_F("bios", "event handler 0x%08X has no compiled body", addr);
    }
    cpu->a0 = a0; cpu->a1 = a1; cpu->a2 = a2; cpu->a3 = a3;
    cpu->ra = ra; cpu->v0 = v0; cpu->v1 = v1;
}

/* A guest pointer into main RAM. The three segment mirrors all alias the
 * same 2 MB, so the physical address is the low 29 bits; anything above the
 * RAM window (scratchpad, hardware registers, BIOS) has no host backing
 * here and yields NULL. */
#define PSX_RAM_BYTES 0x200000u

static void* guest_ptr(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PSX_RAM_BYTES) return NULL;
    return openpete_psx_get_ram() + phys;
}

static int ev_index(uint32_t handle) {
    if ((handle & 0xFFFF0000u) != EV_HANDLE_BASE) return -1;
    uint32_t i = handle & 0xFFFFu;
    return (i < EV_MAX && s_ev[i].open) ? (int)i : -1;
}

/* Signal every open event on this class/spec. An EvMdINTR event runs its
 * handler now, which is where the card driver does its bookkeeping; any
 * other mode just latches, for TestEvent/WaitEvent to observe. */
static void deliver_event(CPUState* cpu, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < EV_MAX; i++) {
        if (!s_ev[i].open || !s_ev[i].enabled) continue;
        if (s_ev[i].cls != cls || (s_ev[i].spec & spec) == 0u) continue;
        if (s_ev[i].mode == EV_MODE_INTR) invoke_guest(cpu, s_ev[i].func);
        else s_ev[i].delivered = 1;
    }
}

/* A card command completes within the call on the host, so the completion
 * event is delivered before returning — the game's handler then clears the
 * in-flight flag its wait loop polls. */
static void card_complete(CPUState* cpu, int ok) {
    uint32_t spec = ok ? EV_SPEC_IOE : EV_SPEC_ERROR;
    deliver_event(cpu, EV_CLASS_CARD, spec);
    deliver_event(cpu, EV_CLASS_CARD_BU, spec);
}

static int bios_a0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x44:      /* FlushCache */
    case 0x70:      /* _bu_init */
        cpu->v0 = 0u;
        return 1;
    case 0xAB: {    /* _card_info(chan): probe the card, answer by event */
        s_card_chan = cpu->a0;
        int present = openpete_memcard_present((int)cpu->a0);
        cpu->v0 = present ? 1u : 0u;
        card_complete(cpu, present);
        return 1;
    }
    case 0xAC:      /* _card_load(chan) */
        s_card_chan = cpu->a0;
        cpu->v0 = openpete_memcard_present((int)cpu->a0) ? 1u : 0u;
        card_complete(cpu, (int)cpu->v0);
        return 1;
    default:
        return 0;
    }
}

static int bios_b0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x07:      /* DeliverEvent(class, spec) */
        deliver_event(cpu, cpu->a0, cpu->a1);
        cpu->v0 = 0u;
        return 1;
    case 0x08: {    /* OpenEvent(class, spec, mode, func) */
        for (int i = 0; i < EV_MAX; i++) {
            if (s_ev[i].open) continue;
            s_ev[i].cls = cpu->a0;
            s_ev[i].spec = cpu->a1;
            s_ev[i].mode = cpu->a2;
            s_ev[i].func = cpu->a3;
            s_ev[i].open = 1;
            s_ev[i].enabled = 0;
            s_ev[i].delivered = 0;
            cpu->v0 = EV_HANDLE_BASE | (uint32_t)i;
            return 1;
        }
        OPL_WARN_F("bios", "OpenEvent: all %d slots in use", EV_MAX);
        cpu->v0 = 0xFFFFFFFFu;
        return 1;
    }
    case 0x09: {    /* CloseEvent(ev) */
        int i = ev_index(cpu->a0);
        if (i >= 0) memset(&s_ev[i], 0, sizeof(s_ev[i]));
        cpu->v0 = 1u;
        return 1;
    }
    case 0x0A: {    /* WaitEvent(ev) — everything completes within its call */
        int i = ev_index(cpu->a0);
        if (i >= 0) s_ev[i].delivered = 0;
        cpu->v0 = 1u;
        return 1;
    }
    case 0x0B: {    /* TestEvent(ev): 1 once, then the latch clears */
        int i = ev_index(cpu->a0);
        cpu->v0 = 0u;
        if (i >= 0 && s_ev[i].delivered) {
            s_ev[i].delivered = 0;
            cpu->v0 = 1u;
        }
        return 1;
    }
    case 0x0C: {    /* EnableEvent(ev) */
        int i = ev_index(cpu->a0);
        if (i >= 0) s_ev[i].enabled = 1;
        cpu->v0 = 1u;
        return 1;
    }
    case 0x0D: {    /* DisableEvent(ev) */
        int i = ev_index(cpu->a0);
        if (i >= 0) s_ev[i].enabled = 0;
        cpu->v0 = 1u;
        return 1;
    }
    case 0x20: {    /* UnDeliverEvent(class, spec) */
        for (int i = 0; i < EV_MAX; i++) {
            if (s_ev[i].open && s_ev[i].cls == cpu->a0
                && (s_ev[i].spec & cpu->a1)) {
                s_ev[i].delivered = 0;
            }
        }
        cpu->v0 = 0u;
        return 1;
    }
    case 0x4A:      /* InitCARD2(pad_enable) */
    case 0x4B:      /* StartCARD2 */
    case 0x4C:      /* StopCARD2 */
    case 0x50:      /* _new_card */
    case 0x5B:      /* ChangeClearPAD */
        cpu->v0 = 1u;
        return 1;
    case 0x56:      /* GetC0Table */
    case 0x57:      /* GetB0Table */
        /* No BIOS image exists to patch. The caller stores the result and
         * writes through it; a null table sends those stores nowhere the
         * game reads, and the services the patch would fix are implemented
         * here instead. */
        cpu->v0 = 0u;
        return 1;
    case 0x4E: {    /* _card_write(chan, frame, buf) */
        s_card_chan = cpu->a0;
        void* src = guest_ptr(cpu->a2);
        int ok = src && openpete_memcard_write_frame((int)cpu->a0,
                                                     (int)cpu->a1, src) == 0;
        cpu->v0 = ok ? 1u : 0u;
        card_complete(cpu, ok);
        return 1;
    }
    case 0x4F: {    /* _card_read(chan, frame, buf) */
        s_card_chan = cpu->a0;
        void* dst = guest_ptr(cpu->a2);
        int ok = dst && openpete_memcard_read_frame((int)cpu->a0,
                                                    (int)cpu->a1, dst) == 0;
        cpu->v0 = ok ? 1u : 0u;
        card_complete(cpu, ok);
        return 1;
    }
    case 0x54:      /* _get_errno */
    case 0x55:      /* _get_error */
        cpu->v0 = 0u;
        return 1;
    case 0x58:      /* _card_chan */
        cpu->v0 = s_card_chan;
        return 1;
    case 0x5C:      /* _card_status(chan): 1 = idle/ready */
        cpu->v0 = 1u;
        return 1;
    case 0x5D:      /* _card_wait(chan) */
        cpu->v0 = 1u;
        return 1;
    default:
        return 0;
    }
}

/* Returns 1 when `addr` was a BIOS table entry, whether or not the service
 * itself is implemented: either way the call is consumed here and must not
 * be reported as a missing translation. */
int openpete_psx_bios_call(CPUState* cpu, uint32_t addr) {
    if (!cpu) return 0;
    if (addr != 0xA0u && addr != 0xB0u && addr != 0xC0u) return 0;
    uint32_t fn = cpu->t1 & 0xFFu;
    bios_census(cpu, addr, fn);
    int handled = 0;
    if (addr == 0xA0u) handled = bios_a0(cpu, fn);
    else if (addr == 0xB0u) handled = bios_b0(cpu, fn);
    if (!handled) {
        cpu->v0 = 0u;
        cpu->v1 = 0u;
    }
    return 1;
}
