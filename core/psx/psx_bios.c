/* psx_bios.c - the PSX BIOS A0/B0/C0 call gates.
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
#include "openpete_psxtrace.h"

/* Event classes and specs from libapi.h. */
#define EV_CLASS_CARD   0xF0000011u   /* HwCARD */
#define EV_CLASS_CARD_BU 0xF4000001u  /* the bu driver's own class */
#define EV_SPEC_IOE     0x0004u       /* EvSpIOE - transfer finished */
#define EV_SPEC_ERROR   0x8000u       /* EvSpERROR */
#define EV_SPEC_TIMOUT  0x0100u       /* EvSpTIMOUT */
#define EV_SPEC_NEW     0x2000u       /* EvSpNEW - card newly inserted */
#define EV_MODE_INTR    0x1000u       /* EvMdINTR - run the handler */
#define EV_MODE_NOINTR  0x2000u       /* EvMdNOINTR - latch for TestEvent */

#define EV_MAX          32
#define EV_HANDLE_BASE  0xF1000000u

struct bios_event {
    uint32_t cls, spec, mode, func;
    int      open, enabled, delivered;
};
static struct bios_event s_ev[EV_MAX];


/* Census: with OPENPETE_BIOS_CENSUS=1 every distinct (table, function) pair
 * is logged with the arguments it carried, so a service this gate does not
 * implement is named rather than silently returning zero; =2 logs every
 * call. */
#define BIOS_SEEN_MAX 128
static uint32_t s_seen[BIOS_SEEN_MAX];
static int s_census = -1;

/** @brief Log a BIOS call according to the OPENPETE_BIOS_CENSUS mode.
 *
 * The mode is read once on the first call: 0 (unset, empty or starting with
 * 0) logs nothing, 2 logs every call, any other value logs the first call
 * of each distinct (table, function) pair, up to 128 pairs, with the
 * argument registers and ra.
 * @param cpu Guest CPU whose a0..a3 and ra are logged.
 * @param table BIOS table address, 0xA0, 0xB0 or 0xC0.
 * @param fn Function number. */
static void bios_census(CPUState* cpu, uint32_t table, uint32_t fn) {
    if (s_census < 0) {
        const char* e = getenv("OPENPETE_BIOS_CENSUS");
        s_census = (e && *e && *e != '0') ? (*e == '2' ? 2 : 1) : 0;
    }
    if (!s_census) return;
    if (s_census == 2) {
        OPL_INFO_F("bios", "0x%02X(0x%02X) a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X",
                   table, fn, cpu->a0, cpu->a1, cpu->a2, cpu->a3, cpu->ra);
        return;
    }
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

/** @brief Run a guest event handler through the recomp dispatch and restore the argument registers.
 *
 * Calls the function at addr with ra set to 0xDEADBEEF, then restores a0..a3,
 * ra, v0 and v1 to their values on entry. Other registers are left as the
 * handler set them. A handler with no compiled body logs a warning.
 * @param cpu Guest CPU the handler runs on.
 * @param addr Handler address; 0 does nothing. */
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

#define PSX_RAM_BYTES 0x200000u
#define MC_FRAME_BYTES 128u

/** @brief Resolve a guest buffer of @p len bytes in main RAM to a host pointer.
 *
 * The three segment mirrors all alias the same 2 MB, so the physical address
 * is the low 29 bits. A buffer that starts above the RAM window (scratchpad,
 * hardware registers, BIOS) or runs past its end has no host backing here.
 * @param addr Guest address of the buffer.
 * @param len Bytes the caller reads or writes.
 * @return The host pointer, or NULL. */
static void* guest_ptr(uint32_t addr, uint32_t len) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= PSX_RAM_BYTES || len > PSX_RAM_BYTES - phys) return NULL;
    return openpete_psx_get_ram() + phys;
}

/** @brief Map an event handle to its slot in the event table.
 *
 * @param handle Handle as OpenEvent returned it (EV_HANDLE_BASE | slot).
 * @return The slot index, or -1 when the handle has another base, names a
 *         slot beyond the table, or names a slot that is not open. */
static int ev_index(uint32_t handle) {
    if ((handle & 0xFFFF0000u) != EV_HANDLE_BASE) return -1;
    uint32_t i = handle & 0xFFFFu;
    return (i < EV_MAX && s_ev[i].open) ? (int)i : -1;
}

/** @brief Signal every open, enabled, undelivered event of exactly this class and spec.
 *
 * The kernel compares both words for equality (OpenBIOS kernel/events.c
 * deliverEvent). An EvMdINTR event runs its handler now, which is where
 * the card driver does its bookkeeping; an EvMdNOINTR event latches, for
 * TestEvent to observe; any other mode ignores the delivery.
 * @param cpu Guest CPU the handlers run on.
 * @param cls Event class.
 * @param spec Event spec. */
static void deliver_event(CPUState* cpu, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < EV_MAX; i++) {
        if (!s_ev[i].open || !s_ev[i].enabled || s_ev[i].delivered) continue;
        if (s_ev[i].cls != cls || s_ev[i].spec != spec) continue;
        if (s_ev[i].mode == EV_MODE_INTR) invoke_guest(cpu, s_ev[i].func);
        else if (s_ev[i].mode == EV_MODE_NOINTR) s_ev[i].delivered = 1;
    }
}

/* The kernel's memory-card driver, after OpenBIOS sio0/driver.c and
 * card/backupunit.c. A card command (_card_info, _card_read, _card_write)
 * only marks its port pending and returns 1; it is refused with 0 while
 * the port's previous command has not finished. The driver runs from the
 * vblank interrupt once StartCARD has hooked it: a vblank that finds a
 * transfer in progress gives up on it (no card answered) and signals
 * EvSpTIMOUT, on the bu class first when the command came through the bu
 * layer (_card_info does; the frame commands do not) and then on HwCARD;
 * otherwise it flips the port it serves and, when the port it lands on has
 * a command pending, starts the transfer. A present card answers within
 * the same vblank, so its completion (EvSpIOE, same two-class order) is
 * signalled from the step that started it; an empty port leaves the
 * transfer in progress for the next vblank's timeout. The timeout thus
 * lands two or three vblanks after the call, by the port flip's parity.
 *
 * Per-port status as _card_status reports it: 1 ready, 0 an info command
 * pending, 2 a read, 4 a write, 0x11 timed out, 0x21 failed. */
enum { MC_OP_NONE = 0, MC_OP_INFO, MC_OP_READ, MC_OP_WRITE };
static struct {
    uint32_t started;        /* StartCARD hooked the vblank step */
    uint32_t flip;           /* the port the step serves next */
    uint32_t in_progress;    /* a transfer started, no answer yet */
    uint32_t flags[2];       /* _card_status value per port */
    uint32_t bu_op[2];       /* the command came through the bu layer */
    uint32_t op[2];          /* MC_OP_* pending or in progress */
    uint32_t sector[2], buffer[2];
    uint32_t device[2];      /* the device id of the last command */
} s_mc;
static uint32_t s_mc_last_port = 0u;

/** @brief Return the card port a device id addresses.
 *
 * The id divided by 16 with truncation toward zero, low bit, so 0x00 is
 * port 0 and 0x10 is port 1.
 * @param device_id Device id as the game passed it.
 * @return 0 or 1. */
static int mc_port(uint32_t device_id) {
    int32_t d = (int32_t)device_id;
    return ((d >= 0 ? d : d + 15) >> 4) & 1;
}

/** @brief Queue a _card_info, _card_read or _card_write command on its port.
 *
 * The port comes from the device id (0x00 port 0, 0x10 port 1). The
 * port's _card_status becomes 0 (info), 2 (read) or 4 (write); the vblank
 * step starts the transfer.
 * @param device_id Device id as the game passed it.
 * @param op MC_OP_INFO, MC_OP_READ or MC_OP_WRITE.
 * @param sector Frame number for a read or write.
 * @param buffer Guest address of the 128-byte frame buffer.
 * @param via_bu Nonzero when the command came through the bu layer, so its
 *        completion also signals the bu class.
 * @return 1 when queued; 0 when the port's previous command has not
 *         finished or a read or write names a sector above 0x400. */
static uint32_t mc_queue(uint32_t device_id, uint32_t op, uint32_t sector, uint32_t buffer, int via_bu) {
    int port = mc_port(device_id);
    if (s_census > 0) OPL_INFO_F("bios", "card command %u on port %d queued at record %u, position %u (flip %u, flags %u)", op, port, openpete_sched_current_record(), openpete_sched_pos(), s_mc.flip, s_mc.flags[port]);
    if ((s_mc.flags[port] & 1u) == 0u) return 0u;
    if (op != MC_OP_INFO && sector > 0x400u) return 0u;
    s_mc.op[port] = op;
    s_mc.device[port] = device_id;
    s_mc.sector[port] = sector;
    s_mc.buffer[port] = buffer;
    s_mc.bu_op[port] = via_bu ? 1u : 0u;
    s_mc.flags[port] = op == MC_OP_INFO ? 0u : (op == MC_OP_READ ? 2u : 4u);
    return 1u;
}

/** @brief Complete the command on a port and signal its event.
 *
 * Clears the in-progress state and the port's pending command, stores the
 * status, and delivers spec on the bu class first when the command came
 * through the bu layer and then on HwCARD.
 * @param cpu Guest CPU the event handlers run on.
 * @param port Port 0 or 1.
 * @param flags New _card_status value of the port.
 * @param spec Event spec to deliver. */
static void mc_finish(CPUState* cpu, int port, uint32_t flags, uint32_t spec) {
    s_mc.in_progress = 0u;
    s_mc.op[port] = MC_OP_NONE;
    s_mc.flags[port] = flags;
    s_mc_last_port = (uint32_t)port;
    if (s_mc.bu_op[port]) { s_mc.bu_op[port] = 0u; deliver_event(cpu, EV_CLASS_CARD_BU, spec); }
    deliver_event(cpu, EV_CLASS_CARD, spec);
}

/** @brief Start the pending command on a port; a present host card answers at once.
 *
 * An empty port leaves the transfer in progress, for the next vblank's
 * timeout. Otherwise a read or write copies one frame between the card and
 * guest RAM, and the command finishes with status 1 and EvSpIOE, or with
 * 0x21 and EvSpERROR when the 128-byte buffer does not lie wholly in
 * main RAM or the card
 * access fails; an info command finishes with status 1.
 * @param cpu Guest CPU the event handlers run on.
 * @param port Port 0 or 1. */
static void mc_start(CPUState* cpu, int port) {
    int chan = port;
    if (!openpete_memcard_present(chan)) { s_mc.in_progress = 1u; return; }
    int ok = 1;
    if (s_mc.op[port] == MC_OP_READ) {
        void* dst = guest_ptr(s_mc.buffer[port], MC_FRAME_BYTES);
        ok = dst && openpete_memcard_read_frame(chan, (int)s_mc.sector[port], dst) == 0;
    } else if (s_mc.op[port] == MC_OP_WRITE) {
        void* src = guest_ptr(s_mc.buffer[port], MC_FRAME_BYTES);
        ok = src && openpete_memcard_write_frame(chan, (int)s_mc.sector[port], src) == 0;
    }
    mc_finish(cpu, port, ok ? 1u : 0x21u, ok ? EV_SPEC_IOE : EV_SPEC_ERROR);
}

void openpete_psx_bios_card_state_get(uint32_t w[PSX_BIOS_CARD_STATE_WORDS]) {
    w[0] = (s_mc.started & 1u) | (s_mc.flip & 1u) << 1 | (s_mc.in_progress & 1u) << 2
         | (s_mc.flags[0] & 0xFFu) << 8 | (s_mc.flags[1] & 0xFFu) << 16
         | (s_mc.bu_op[0] & 1u) << 24 | (s_mc.bu_op[1] & 1u) << 25
         | (s_mc.op[0] & 3u) << 26 | (s_mc.op[1] & 3u) << 28 | (s_mc_last_port & 1u) << 30;
    w[1] = s_mc.sector[0]; w[2] = s_mc.buffer[0]; w[3] = s_mc.device[0];
    w[4] = s_mc.sector[1]; w[5] = s_mc.buffer[1]; w[6] = s_mc.device[1];
}
void openpete_psx_bios_card_state_set(const uint32_t w[PSX_BIOS_CARD_STATE_WORDS]) {
    s_mc.started = w[0] & 1u; s_mc.flip = (w[0] >> 1) & 1u; s_mc.in_progress = (w[0] >> 2) & 1u;
    s_mc.flags[0] = (w[0] >> 8) & 0xFFu; s_mc.flags[1] = (w[0] >> 16) & 0xFFu;
    s_mc.bu_op[0] = (w[0] >> 24) & 1u; s_mc.bu_op[1] = (w[0] >> 25) & 1u;
    s_mc.op[0] = (w[0] >> 26) & 3u; s_mc.op[1] = (w[0] >> 28) & 3u; s_mc_last_port = (w[0] >> 30) & 1u;
    s_mc.sector[0] = w[1]; s_mc.buffer[0] = w[2]; s_mc.device[0] = w[3];
    s_mc.sector[1] = w[4]; s_mc.buffer[1] = w[5]; s_mc.device[1] = w[6];
}

/* One vblank interrupt of the card driver (OpenBIOS firstStageCardAction).
 * The handler runs only on the vblanks the reference's controller handler
 * ran on: a vblank the schedule marks unserved is skipped, unless a
 * transfer is in progress and the following vblank is served, which is
 * the abort's own vblank (the abort returns before the pad poll, so the
 * recorder saw no command there either). */
void openpete_psx_bios_vbl(CPUState* cpu) {
    if (!s_mc.started) return;
    const uint32_t vbl = openpete_sched_pos() + 1u;
    int known = 0;
    int served = openpete_sched_sio0_served(vbl, &known);
    if (s_mc.in_progress) {
        if (known && !served && !openpete_sched_sio0_served(vbl + 1u, NULL)) return;
        int port = (int)s_mc.flip;
        if (s_census > 0) OPL_INFO_F("bios", "card timeout on port %d delivered at record %u, position %u", port, openpete_sched_current_record(), openpete_sched_pos());
        mc_finish(cpu, port, 0x11u, EV_SPEC_TIMOUT);
        return;
    }
    if (known && !served) return;
    s_mc.flip = 1u - s_mc.flip;
    int port = (int)s_mc.flip;
    if ((s_mc.flags[port] & 1u) != 0u) return;
    if (s_census > 0) OPL_INFO_F("bios", "card command on port %d started at record %u, position %u", port, openpete_sched_current_record(), openpete_sched_pos());
    mc_start(cpu, port);
}

/* Kernel low-RAM words; see openpete_memcard.h. */
void openpete_psx_bios_seed_ram(uint8_t* ram) {
    static const uint32_t vec80[4] = { 0x3C1A0000u, 0x275A0C80u, 0x03400008u, 0u };
    static const uint32_t call[3][4] = {
        { 0x3C080000u, 0x250805C4u, 0x01000008u, 0u },
        { 0x3C080000u, 0x250805E0u, 0x01000008u, 0u },
        { 0x3C080000u, 0x25080600u, 0x01000008u, 0u },
    };
    memcpy(ram + 0x00u, vec80, sizeof(vec80));
    memcpy(ram + 0x80u, vec80, sizeof(vec80));
    memcpy(ram + 0xA0u, call[0], sizeof(call[0]));
    memcpy(ram + 0xB0u, call[1], sizeof(call[1]));
    memcpy(ram + 0xC0u, call[2], sizeof(call[2]));
    const uint32_t ram_mb = 2u, boot_flags = 0xFFu;
    memcpy(ram + 0x60u, &ram_mb, 4);
    memcpy(ram + 0x68u, &boot_flags, 4);
    static const uint32_t c0_6 = 0xC80u, b0_5b = 0x43D0u;
    static const uint32_t slot_pair[2] = { 0x3C020000u, 0x2442641Cu };
    memcpy(ram + 0x68Cu, &c0_6, 4);
    memcpy(ram + 0x9E0u, &b0_5b, 4);
    memcpy(ram + 0xCF0u, slot_pair, sizeof(slot_pair));
}

/* The identification block of the SCPH-1001 ROM, the bytes a game reads
 * from the BIOS: the date and console-type words at 0x100 and 0x104, the
 * vendor and model strings at 0x108 and 0x12C, and the version string at
 * 0x7FF32 whose last character (0x7FF52, 'A') names the region. Every other
 * ROM byte reads as 0. */
typedef struct { uint32_t off; const uint8_t* bytes; uint32_t len; } rom_span_t;
static const rom_span_t s_rom_spans[] = {
    { 0x100u, (const uint8_t[]){ 0x04, 0x12, 0x95, 0x19, 0x03, 0x00, 0x00, 0x00 }, 8 },
    { 0x108u, (const uint8_t[]){ 0x53, 0x6F, 0x6E, 0x79, 0x20, 0x43, 0x6F, 0x6D, 0x70, 0x75, 0x74, 0x65, 0x72, 0x20, 0x45, 0x6E, 0x74, 0x65, 0x72, 0x74, 0x61, 0x69, 0x6E, 0x6D, 0x65, 0x6E, 0x74, 0x20, 0x49, 0x6E, 0x63, 0x2E }, 32 },
    { 0x12Cu, (const uint8_t[]){ 0x43, 0x45, 0x58, 0x2D, 0x33, 0x30, 0x30, 0x30, 0x2F, 0x31, 0x30, 0x30, 0x31, 0x2F, 0x31, 0x30, 0x30, 0x32, 0x20, 0x62, 0x79, 0x20, 0x4B, 0x2E, 0x53, 0x2E }, 26 },
    { 0x7FF32u, (const uint8_t[]){ 0x53, 0x79, 0x73, 0x74, 0x65, 0x6D, 0x20, 0x52, 0x4F, 0x4D, 0x20, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x20, 0x32, 0x2E, 0x32, 0x20, 0x31, 0x32, 0x2F, 0x30, 0x34, 0x2F, 0x39, 0x35, 0x20, 0x41 }, 33 },
    { 0x7FF54u, (const uint8_t[]){ 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x31, 0x39, 0x39, 0x33, 0x2C, 0x31, 0x39, 0x39, 0x34, 0x2C, 0x31, 0x39, 0x39, 0x35, 0x20, 0x28, 0x43, 0x29, 0x20, 0x53, 0x6F, 0x6E, 0x79, 0x20, 0x43, 0x6F, 0x6D, 0x70, 0x75, 0x74, 0x65, 0x72, 0x20, 0x45, 0x6E, 0x74, 0x65, 0x72, 0x74, 0x61, 0x69, 0x6E, 0x6D, 0x65, 0x6E, 0x74, 0x20, 0x49, 0x6E, 0x63, 0x2E }, 61 },
};
static uint32_t s_rom_reads = 0;

int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) {
    if ((phys & 0xFFF80000u) != 0x1FC00000u) return 0;
    uint32_t off = phys & 0x7FFFFu;
    *out = 0u;
    s_rom_reads++;
    for (size_t i = 0; i < sizeof(s_rom_spans) / sizeof(s_rom_spans[0]); i++) {
        const rom_span_t* sp = &s_rom_spans[i];
        if (off >= sp->off && off < sp->off + sp->len) { *out = sp->bytes[off - sp->off]; break; }
    }
    return 1;
}
uint32_t openpete_psx_bios_rom_reads(void) { return s_rom_reads; }

/** @brief Handle a call through the A0 table: FlushCache, _bu_init, _card_info and _card_load.
 *
 * FlushCache and _bu_init return 0. _card_info and _card_load queue an
 * info command through the bu layer and return whether it was queued.
 * @param cpu Guest CPU; a0 carries the device id and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is implemented, 0 otherwise. */
static int bios_a0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x44:      /* FlushCache */
    case 0x70:      /* _bu_init */
        cpu->v0 = 0u;
        return 1;
    case 0xAB:      /* _card_info(chan): probe the card, answer by event */
    case 0xAC:      /* _card_load(chan) */
        cpu->v0 = mc_queue(cpu->a0, MC_OP_INFO, 0u, 0u, 1);
        return 1;
    default:
        return 0;
    }
}

/** @brief Handle a call through the B0 table: the event services and the card driver services.
 *
 * Implements OpenEvent, CloseEvent, WaitEvent, TestEvent, EnableEvent,
 * DisableEvent, DeliverEvent and UnDeliverEvent over the event table, and
 * InitCARD2, StartCARD2, StopCARD2, _new_card, ChangeClearPAD, GetC0Table,
 * GetB0Table, _card_write, _card_read, _get_errno, _get_error, _card_chan,
 * _card_status and _card_wait over the card driver state. WaitEvent
 * returns at once with the latch cleared: the gate cannot suspend the guest,
 * and a queued card command completes in a later vblank step.
 * @param cpu Guest CPU; the argument registers are read and v0 receives the
 *        result.
 * @param fn Function number.
 * @return 1 when the function is implemented, 0 otherwise. */
static int bios_b0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x07:      /* DeliverEvent(class, spec) */
        deliver_event(cpu, cpu->a0, cpu->a1);
        cpu->v0 = 0u;
        return 1;
    case 0x08: {    /* OpenEvent(class, spec, mode, func) */
        if (s_census > 0) OPL_INFO_F("bios", "OpenEvent(class=0x%08X spec=0x%04X mode=0x%04X func=0x%08X)", cpu->a0, cpu->a1, cpu->a2, cpu->a3);
        /* Slot 0 of the kernel's event table is in use before a game opens
         * its first event (SCPH-1001: the first handle a game receives is
         * 0xF1000001), so the game's events take the slots from 1. */
        for (int i = 1; i < EV_MAX; i++) {
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
    case 0x0A: {    /* WaitEvent(ev): returns at once, latch cleared */
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
    case 0x0C: {    /* EnableEvent(ev): a latched delivery is dropped */
        int i = ev_index(cpu->a0);
        if (i >= 0) { s_ev[i].enabled = 1; s_ev[i].delivered = 0; }
        cpu->v0 = 1u;
        return 1;
    }
    case 0x0D: {    /* DisableEvent(ev): a latched delivery is dropped */
        int i = ev_index(cpu->a0);
        if (i >= 0) { s_ev[i].enabled = 0; s_ev[i].delivered = 0; }
        cpu->v0 = 1u;
        return 1;
    }
    case 0x20: {    /* UnDeliverEvent(class, spec) */
        for (int i = 0; i < EV_MAX; i++) {
            if (s_ev[i].open && s_ev[i].cls == cpu->a0
                && s_ev[i].spec == cpu->a1 && s_ev[i].mode == EV_MODE_NOINTR) {
                s_ev[i].delivered = 0;
            }
        }
        cpu->v0 = 0u;
        return 1;
    }
    case 0x4A:      /* InitCARD2(pad_enable): both ports ready, port 0 next */
        memset(&s_mc, 0, sizeof(s_mc));
        s_mc.flags[0] = s_mc.flags[1] = 1u;
        cpu->v0 = 1u;
        return 1;
    case 0x4B:      /* StartCARD2: hook the vblank step */
        if (s_census > 0) OPL_INFO_F("bios", "StartCARD at record %u, position %u", openpete_sched_current_record(), openpete_sched_pos());
        s_mc.started = 1u;
        cpu->v0 = 1u;
        return 1;
    case 0x4C:      /* StopCARD2 */
        s_mc.started = 0u;
        cpu->v0 = 1u;
        return 1;
    case 0x50:      /* _new_card */
    case 0x5B:      /* ChangeClearPAD */
        cpu->v0 = 1u;
        return 1;
    case 0x56:      /* GetC0Table: the kernel's table address, 0x674 */
        cpu->v0 = 0x674u;
        return 1;
    case 0x57:      /* GetB0Table: 0x874 */
        cpu->v0 = 0x874u;
        return 1;
    case 0x4E:      /* _card_write(chan, frame, buf) */
        cpu->v0 = mc_queue(cpu->a0, MC_OP_WRITE, cpu->a1, cpu->a2, 0);
        return 1;
    case 0x4F:      /* _card_read(chan, frame, buf) */
        cpu->v0 = mc_queue(cpu->a0, MC_OP_READ, cpu->a1, cpu->a2, 0);
        return 1;
    case 0x54:      /* _get_errno */
    case 0x55:      /* _get_error */
        cpu->v0 = 0u;
        return 1;
    case 0x58:      /* _card_chan: the device id of the last finished command */
        cpu->v0 = s_mc.device[s_mc_last_port];
        return 1;
    case 0x5C:      /* _card_status(chan) */
        cpu->v0 = s_mc.flags[mc_port(cpu->a0)];
        return 1;
    case 0x5D:      /* _card_wait(chan): the kernel spins until the port is
                     * ready; the vblank steps that spin needs run in the
                     * VBL body, which this gate cannot enter, so the status
                     * is returned as it stands. */
        if ((s_mc.flags[mc_port(cpu->a0)] & 1u) == 0u)
            OPL_WARN_F("bios", "_card_wait on a pending command returns without waiting");
        cpu->v0 = s_mc.flags[mc_port(cpu->a0)];
        return 1;
    default:
        return 0;
    }
}

/** @brief Service a guest call to a BIOS function table entry (0xA0, 0xB0, 0xC0).
 *
 * The function number is $t1's low byte. Every call is counted by the BIOS
 * census; A0 and B0 services are dispatched to their handlers, and a
 * service with no handler (every C0 service among them) returns with $v0
 * and $v1 zeroed.
 * @param cpu Guest CPU state; NULL returns 0.
 * @param addr Call target.
 * @return 1 when @p addr is a BIOS table entry, whether or not the service
 *         is implemented, so call_by_address() does not log it as a
 *         missing translation; 0 otherwise. */
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
