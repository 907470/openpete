/* psx_bios.c - the PSX BIOS A0/B0/C0 call gates.
 *
 * A game reaches a BIOS service through a fixed trampoline: it loads the
 * table address into $t2 and the function number into $t1, then jumps to
 * $t2. The recompiler turns that into a call to an address of 0xA0, 0xB0 or
 * 0xC0, which resolves to no translated function, because the BIOS is not
 * part of the recompiled image.
 *
 * Several PSYQ libraries that wrap these services are intercepted by name in
 * the thunk layer (Spyro 1's libmcrd among them), so their calls never
 * arrive here. Every library that runs recompiled does arrive: Spyro 2's
 * and Spyro 3's libmcrd open events on the HwCARD and bu classes, issue
 * frame commands, and reach the card's files through the kernel's file
 * calls (B(32h) open, B(33h) lseek, B(34h) read, B(35h) write, B(36h)
 * close) on the bu device.
 *
 * The kernel's file table, errno, event table, card driver and bu layer
 * state live in guest RAM at the SCPH-1001 addresses, seeded at boot as the
 * ROM leaves them (openpete_psx_bios_seed_ram), and the calls read and
 * write them there, so the game, the oracle and a savestate see the
 * kernel's bytes.
 *
 * The card frames come from the same host-backed images the libmcrd surface
 * uses (core/host/memcard.c), so both routes see one card.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_fiber.h"
#include "openpete_fnv1a64.h"
#include "openpete_guest_regs.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_modsave.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"
#include "openpete_psxtrace.h"
#include "game_extras.h"
#include "psx_bios_kernel_seed.h"

/* Event classes and specs from libapi.h. */
#define EV_CLASS_CARD   0xF0000011u   /* HwCARD */
#define EV_CLASS_CARD_BU 0xF4000001u  /* the bu driver's own class */
#define EV_CLASS_SPU    0xF0000009u   /* HwSPU */
#define EV_SPEC_IOE     0x0004u       /* EvSpIOE - transfer finished */
#define EV_SPEC_COMP    0x0020u       /* EvSpCOMP - DMA transfer complete */
#define EV_SPEC_ERROR   0x8000u       /* EvSpERROR */
#define EV_SPEC_TIMOUT  0x0100u       /* EvSpTIMOUT */
#define EV_SPEC_NEW     0x2000u       /* EvSpNEW - card newly inserted */
#define EV_MODE_INTR    0x1000u       /* EvMdINTR - run the handler */
#define EV_MODE_NOINTR  0x2000u       /* EvMdNOINTR - latch for TestEvent */
#define EV_HANDLE_BASE  0xF1000000u

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

/* Card window: with OPENPETE_CARD_WINDOW=<path> every card-layer event is
 * one row of <path>, stamped with openpete_vbl and openpete_frame:
 *   A vbl frame fn a0 v0              A0 card call (70h, ABh, ACh) and its result
 *   B vbl frame fn a0 a1 a2 v0        B0 file call (32h-36h) and its result
 *   R vbl frame port buf bytes hash   an asynchronous read finished: its span
 *                                     and the FNV-1a 64 hash of the bytes it holds
 *   C vbl frame port op sector key    a transfer started by the vblank step
 *                                     keyed on reference VBLANK `key`
 *   F vbl frame port flags            a transfer finished, with the port's status
 *   D vbl frame class spec            DeliverEvent of a card or bu class, or an fd
 *   H vbl frame func                  an EvMdINTR handler that delivery ran
 *   E vbl frame class spec mode func handle   OpenEvent
 *   X vbl frame handle                CloseEvent
 * The rows are the openpete side of the reference recorder's card-window
 * file (C, D, H, E rows), so the two compare per operation. */
static FILE* s_cw = NULL;
static int s_cw_state = -1;

/** @brief Write one card-window row, opening the file on first use.
 *
 * @param fmt printf format of the row's fields after the stamp.
 * @param ... The fields. */
static void cw_row(const char* fmt, ...) {
    if (s_cw_state < 0) {
        const char* p = getenv("OPENPETE_CARD_WINDOW");
        s_cw = (p && *p) ? fopen(p, "w") : NULL;
        s_cw_state = s_cw ? 1 : 0;
        if (s_cw) fprintf(s_cw, "# card window: A/B calls, C start, F finish, D deliver, H handler, E open, X close\n");
    }
    if (!s_cw_state) return;
    fprintf(s_cw, "%c %d %u", fmt[0], SDL_GetAtomicInt(&openpete_vbl), openpete_frame);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_cw, fmt + 1, ap);
    va_end(ap);
    fputc('\n', s_cw);
    fflush(s_cw);
}

/** @brief Run a guest event handler through the recomp dispatch and restore every register.
 *
 * Calls the function at addr with ra set to 0xDEADBEEF, then restores the
 * general-purpose registers, hi and lo to their values on entry, as the
 * kernel's exception return does (openpete_guest_regs.h). A handler with
 * no compiled body logs a warning. Its gate hits are host-initiated for
 * the oracle's clock replay.
 * @param cpu Guest CPU the handler runs on.
 * @param addr Handler address; 0 does nothing. */
static void invoke_guest(CPUState* cpu, uint32_t addr) {
    if (!addr) return;
    openpete_guest_regs_t saved;
    openpete_guest_regs_save(cpu, &saved);
    cpu->ra = 0xDEADBEEFu;
    openpete_psxtrace_gate_host_enter();
    if (!openpete_recomp_module_get()->dispatch(cpu, addr)) {
        OPL_WARN_F("bios", "event handler 0x%08X has no compiled body", addr);
    }
    openpete_psxtrace_gate_host_leave();
    openpete_guest_regs_restore(cpu, &saved);
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

/* The kernel's file layer, at the SCPH-1001 addresses: errno at 0x8640 and
 * 16 file control blocks of 0x2C bytes from 0x8648, the fd being the
 * index (the ROM's getFileFromHandle, RAM 0x30C8). The calls follow the
 * ROM's RAM-resident code, B0 entries 0x32 open (0x2958), 0x33 lseek
 * (0x2A64), 0x35 write (0x2C94), 0x36 close (0x2E00), 0x54 _get_errno
 * (0x2EEC) and 0x55 _get_error (0x2E98), whose control flow OpenBIOS
 * fileio/stdio.c psxopen, psxlseek, psxwrite and psxclose reproduce. */
#define K_ERRNO        0x8640u
#define K_FCB          0x8648u
#define K_FCB_SIZE     0x2Cu
#define K_FCB_COUNT    16u
#define FCB_FLAGS      0x00u   /* the open mode; 0 marks the entry free */
#define FCB_DEVICE_ID  0x04u   /* the digits after the device name: bu00 0, bu10 0x10 */
#define FCB_BUFFER     0x08u
#define FCB_COUNT      0x0Cu
#define FCB_OFFSET     0x10u
#define FCB_DEV_FLAGS  0x14u   /* the device's DCB flags */
#define FCB_ERRNO      0x18u
#define FCB_DCB        0x1Cu   /* the device control block */
#define FCB_LENGTH     0x20u
#define FCB_LBA        0x24u   /* bu: the first directory index */
#define FCB_FD         0x28u
#define DCB_FLAGS      0x04u
#define DT_FS          0x10u   /* PSXDTTYPE_FS: read and write go to the device */
#define DT_BLOCK       0x04u
/* The device control blocks the ROM installs, from 0x6EE0, 0x50 each. */
#define K_DCB_TTY      0x6EE0u
#define K_DCB_CDROM    0x6F30u
#define K_DCB_BU       0x6F80u
/* The bu device's directory cache: 15 entries of 32 bytes per port, port 1
 * 0x1E0 after port 0 (allocation state word, file size word, next-block
 * half, name), filled by _bu_init and _card_load (OpenBIOS
 * card/backupunit.h struct BuDirectoryEntry; the SCPH-1001 address is the
 * one its _card_load fills). */
#define K_BU_DIR       0xBA88u
#define K_BU_DIR_PORT  0x1E0u
#define BU_DIR_ENTRY   0x20u
#define BU_DIR_FIRST   0x51u   /* a file's first block */
#define PSXF_CREAT     0x0200u
#define PSXF_ASYNC     0x8000u
#define PSX_ENOENT     2u
#define PSX_EBADF      9u
#define PSX_EBUSY      16u
#define PSX_ENODEV     19u
#define PSX_EINVAL     22u
#define PSX_EMFILE     24u

/** @brief Read the guest word at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the word.
 * @return The word. */
static uint32_t kw(uint32_t addr) {
    uint32_t v;
    memcpy(&v, openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 4u)), 4);
    return v;
}

/** @brief Write the guest word at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the word.
 * @param v The value. */
static void kw_set(uint32_t addr, uint32_t v) {
    memcpy(openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 4u)), &v, 4);
}

/** @brief Read the guest byte at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the byte.
 * @return The byte. */
static uint32_t kb(uint32_t addr) {
    return openpete_psx_get_ram()[addr & (PSX_RAM_BYTES - 1u)];
}

/** @brief Write the guest byte at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the byte.
 * @param v The value; its low 8 bits are stored. */
static void kb_set(uint32_t addr, uint32_t v) {
    openpete_psx_get_ram()[addr & (PSX_RAM_BYTES - 1u)] = (uint8_t)v;
}

/** @brief Read the guest halfword at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the halfword.
 * @return The halfword. */
static uint32_t kh(uint32_t addr) {
    uint16_t v;
    memcpy(&v, openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 2u)), 2);
    return v;
}

/** @brief Write the guest halfword at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the halfword.
 * @param v The value; its low 16 bits are stored. */
static void kh_set(uint32_t addr, uint32_t v) {
    uint16_t h = (uint16_t)v;
    memcpy(openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 2u)), &h, 2);
}

/* --- Event control blocks ------------------------------------------------
 * The kernel's event table, in guest RAM where the table of tables names
 * it (word 0x120 the address, 0x124 the size; SCPH-1001 0xE028, 22 entries
 * of 0x1C bytes). Each entry holds the class, the status, the spec, the
 * mode and the handler; the handle OpenEvent returns is 0xF1000000 | index.
 * The calls follow the ROM's RAM-resident code, B0 entries 0x07 DeliverEvent
 * (0x1B44), 0x08 OpenEvent (0x1D8C), 0x09 CloseEvent (0x1E1C), 0x0A
 * WaitEvent (0x1E44), 0x0B TestEvent (0x1EC8), 0x0C EnableEvent (0x1F10),
 * 0x0D DisableEvent (0x1F4C) and 0x20 UnDeliverEvent (0x1C5C); OpenBIOS
 * kernel/events.c is the same table with other status values. */
#define K_TOT_EVCB         0x120u
#define EVCB_BYTES         0x1Cu
#define EVCB_CLASS         0x00u
#define EVCB_STATUS        0x04u
#define EVCB_SPEC          0x08u
#define EVCB_MODE          0x0Cu
#define EVCB_FUNC          0x10u
#define EV_STATUS_FREE     0x0000u
#define EV_STATUS_DISABLED 0x1000u
#define EV_STATUS_ENABLED  0x2000u
#define EV_STATUS_DELIVERED 0x4000u

/** @brief Return the event table's first entry and its entry count from the table of tables.
 *
 * @param count Receives the entry count (the table's size over 0x1C).
 * @return The first entry's guest address. */
static uint32_t ev_table(uint32_t* count) {
    *count = kw(K_TOT_EVCB + 4u) / EVCB_BYTES;
    return kw(K_TOT_EVCB);
}

/** @brief Return the entry an event handle names, as the ROM indexes it: the handle's low 16 bits, unchecked.
 *
 * @param handle The handle.
 * @return The entry's guest address. */
static uint32_t ev_entry(uint32_t handle) {
    uint32_t n;
    return ev_table(&n) + (handle & 0xFFFFu) * EVCB_BYTES;
}

/** @brief Signal every enabled event of exactly this class and spec.
 *
 * An entry matches with status enabled and equal class and spec (an entry
 * already delivered is not signalled again). An EvMdNOINTR entry becomes
 * delivered, for TestEvent and WaitEvent; an EvMdINTR entry with a handler
 * runs the handler now, which is where the card driver's clients do their
 * bookkeeping; any other mode ignores the delivery.
 * @param cpu Guest CPU the handlers run on.
 * @param cls Event class.
 * @param spec Event spec. */
static void deliver_event(CPUState* cpu, uint32_t cls, uint32_t spec) {
    if (cls == EV_CLASS_CARD || cls == EV_CLASS_CARD_BU || cls < 16u) cw_row("D %08x %04x", cls, spec);
    uint32_t n, e = ev_table(&n);
    for (uint32_t i = 0; i < n; i++, e += EVCB_BYTES) {
        if (kw(e + EVCB_STATUS) != EV_STATUS_ENABLED) continue;
        if (kw(e + EVCB_CLASS) != cls || kw(e + EVCB_SPEC) != spec) continue;
        const uint32_t mode = kw(e + EVCB_MODE), func = kw(e + EVCB_FUNC);
        if (mode == EV_MODE_NOINTR) {
            kw_set(e + EVCB_STATUS, EV_STATUS_DELIVERED);
        } else if (mode == EV_MODE_INTR && func) {
            cw_row("H 0x%08X", func);
            invoke_guest(cpu, func);
        }
    }
}

/** @brief Return every delivered EvMdNOINTR event of exactly this class and spec to enabled.
 *
 * @param cls Event class.
 * @param spec Event spec. */
static void undeliver_event(uint32_t cls, uint32_t spec) {
    uint32_t n, e = ev_table(&n);
    for (uint32_t i = 0; i < n; i++, e += EVCB_BYTES) {
        if (kw(e + EVCB_STATUS) == EV_STATUS_DELIVERED && kw(e + EVCB_CLASS) == cls
            && kw(e + EVCB_SPEC) == spec && kw(e + EVCB_MODE) == EV_MODE_NOINTR)
            kw_set(e + EVCB_STATUS, EV_STATUS_ENABLED);
    }
}

/** @brief Open an event in the first free entry, disabled.
 *
 * @param cls Event class.
 * @param spec Event spec.
 * @param mode EvMdINTR or EvMdNOINTR.
 * @param func Handler address, for EvMdINTR.
 * @return The handle, or 0xFFFFFFFF when every entry is in use. */
static uint32_t ev_open(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    uint32_t n, e = ev_table(&n);
    for (uint32_t i = 0; i < n; i++, e += EVCB_BYTES) {
        if (kw(e + EVCB_STATUS) != EV_STATUS_FREE) continue;
        kw_set(e + EVCB_CLASS, cls);
        kw_set(e + EVCB_SPEC, spec);
        kw_set(e + EVCB_MODE, mode);
        kw_set(e + EVCB_STATUS, EV_STATUS_DISABLED);
        kw_set(e + EVCB_FUNC, func);
        return EV_HANDLE_BASE | i;
    }
    OPL_WARN_F("bios", "OpenEvent: all %u entries in use", n);
    return 0xFFFFFFFFu;
}

/** @brief Set an open event's status, leaving a free entry free (EnableEvent and DisableEvent).
 *
 * @param handle The handle.
 * @param status EV_STATUS_ENABLED or EV_STATUS_DISABLED. */
static void ev_set_open_status(uint32_t handle, uint32_t status) {
    const uint32_t e = ev_entry(handle);
    if (kw(e + EVCB_STATUS) != EV_STATUS_FREE) kw_set(e + EVCB_STATUS, status);
}

/** @brief Consume an event's delivery: a delivered entry returns to enabled (TestEvent).
 *
 * @param handle The handle.
 * @return 1 when the entry was delivered, else 0. */
static uint32_t ev_test(uint32_t handle) {
    const uint32_t e = ev_entry(handle);
    if (kw(e + EVCB_STATUS) != EV_STATUS_DELIVERED) return 0u;
    kw_set(e + EVCB_STATUS, EV_STATUS_ENABLED);
    return 1u;
}

/** @brief Handle the B0 event services 0x07-0x0D and 0x20 over the event table.
 *
 * WaitEvent returns 1 at once for an enabled entry, where the kernel spins
 * until a delivery: the gate cannot suspend the guest. It returns 0 for an
 * entry neither enabled nor delivered, and consumes a delivery as TestEvent
 * does.
 * @param cpu Guest CPU; the argument registers are read and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is one of these services, 0 otherwise. */
static int bios_b0_event(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x07:      /* DeliverEvent(class, spec) */
        deliver_event(cpu, cpu->a0, cpu->a1);
        cpu->v0 = 0u;
        return 1;
    case 0x08:      /* OpenEvent(class, spec, mode, func) */
        if (s_census > 0) OPL_INFO_F("bios", "OpenEvent(class=0x%08X spec=0x%04X mode=0x%04X func=0x%08X)", cpu->a0, cpu->a1, cpu->a2, cpu->a3);
        cpu->v0 = ev_open(cpu->a0, cpu->a1, cpu->a2, cpu->a3);
        cw_row("E %08x %04x %04x %08x %08x", cpu->a0, cpu->a1, cpu->a2, cpu->a3, cpu->v0);
        return 1;
    case 0x09:      /* CloseEvent(ev) */
        cw_row("X %08x", cpu->a0);
        kw_set(ev_entry(cpu->a0) + EVCB_STATUS, EV_STATUS_FREE);
        cpu->v0 = 1u;
        return 1;
    case 0x0A: {    /* WaitEvent(ev) */
        const uint32_t st = kw(ev_entry(cpu->a0) + EVCB_STATUS);
        if (st == EV_STATUS_ENABLED) OPL_WARN_F("bios", "WaitEvent(0x%08X) on an undelivered event returns without waiting", cpu->a0);
        cpu->v0 = (st == EV_STATUS_ENABLED) ? 1u : ev_test(cpu->a0);
        return 1;
    }
    case 0x0B:      /* TestEvent(ev) */
        cpu->v0 = ev_test(cpu->a0);
        return 1;
    case 0x0C:      /* EnableEvent(ev): a delivery is dropped */
    case 0x0D:      /* DisableEvent(ev) */
        ev_set_open_status(cpu->a0, fn == 0x0C ? EV_STATUS_ENABLED : EV_STATUS_DISABLED);
        cpu->v0 = 1u;
        return 1;
    case 0x20:      /* UnDeliverEvent(class, spec) */
        undeliver_event(cpu->a0, cpu->a1);
        cpu->v0 = 0u;
        return 1;
    default:
        return 0;
    }
}

void openpete_psx_bios_reset_callback_events(void) {
    for (uint32_t h = 0; h < 5u; h++) kw_set(ev_entry(EV_HANDLE_BASE | h) + EVCB_STATUS, EV_STATUS_FREE);
}

void openpete_psx_bios_spu_start_events(void) {
    const uint32_t h = ev_open(EV_CLASS_SPU, EV_SPEC_COMP, EV_MODE_NOINTR, 0u);
    if (h != 0xFFFFFFFFu) ev_set_open_status(h, EV_STATUS_ENABLED);
}

/* --- Interrupt handler queues ----------------------------------------------
 * The exception control blocks, one list head per priority at the address
 * word 0x100 names (SCPH-1001 0xE004, 8 bytes per priority); a node's first
 * word links the next node. C0 entries 0x02 SysEnqIntRP (RAM 0x1420) and
 * 0x03 SysDeqIntRP (0x1444). */
#define K_TOT_EXCB 0x100u

/** @brief Push a handler node at the head of a priority's queue (SysEnqIntRP).
 *
 * @param pri The priority.
 * @param node The node's guest address. */
static void int_enq(uint32_t pri, uint32_t node) {
    const uint32_t head = kw(K_TOT_EXCB) + 8u * pri;
    kw_set(node, kw(head));
    kw_set(head, node);
}

/** @brief Unlink a handler node from a priority's queue, leaving its own link word (SysDeqIntRP).
 *
 * The walk stops at the end of the list, or after 64 nodes.
 * @param pri The priority.
 * @param node The node's guest address. */
static void int_deq(uint32_t pri, uint32_t node) {
    uint32_t link = kw(K_TOT_EXCB) + 8u * pri;
    for (int i = 0; i < 64; i++) {
        const uint32_t cur = kw(link);
        if (!cur) return;
        if (cur == node) { kw_set(link, kw(cur)); return; }
        link = cur;
    }
}

/* --- The memory-card driver ------------------------------------------------
 * The SCPH-1001 card driver, kept in guest RAM at the addresses its
 * RAM-resident code (0x4C70-0x6578) uses, so the game, the oracle and a
 * savestate see the kernel's bytes. OpenBIOS sio0/driver.c is the same
 * driver; the names below are its.
 *
 * A command (_card_read, _card_write, _card_info) records the port's device
 * id, buffer, sector and handler and sets the port's status byte to 2, 4 or
 * 8; it is refused while the status lacks bit 0 (the port's previous
 * command has not finished). The vblank step (firstStageCardAction, RAM
 * 0x5000) undelivers the card class's five specs, then either aborts a
 * transfer still in progress (no card answered: status 0x11, EvSpTIMOUT) or
 * flips the port it serves and, when that port has a command, starts the
 * transfer: the fast-track pointers, the handler node on exception queue 1,
 * and the handler's first byte (the select), which marks the transfer in
 * progress. A present card answers through the SIO0 interrupt, which on
 * hardware completes the transfer about half a VBL later, after the VBLANK
 * handlers have returned; openpete_psx_bios_vbl_complete steps that
 * transfer byte by byte after the VBL's VSync callbacks (the transfer in
 * flight, below) and delivers HwCARD at its end. An empty port leaves the
 * transfer in progress for the next step's abort. */
#define K_MC_FLIP        0x7264u  /* byte: the port the step serves */
#define K_MC_FT_COUNT    0x72F0u  /* fast track: bytes moved */
#define K_MC_SIO_MASK    0x74A0u  /* the port select bit, 0 or 0x2000 */
#define K_MC_74A4        0x74A4u  /* cleared by each command and each end */
#define K_SIO0_NODE      0x74A8u  /* the VBLANK handler node {next, handler, verifier, pad} */
#define K_PAD_ENABLE     0x74B8u  /* InitCARD's pad_enable */
#define K_MC_STARTED     0x74BCu  /* StartCARD hooked the step */
#define K_MC_SKIP_NEW    0x74C0u  /* _new_card: skip the next new-card error */
#define K_MC_WFLAG       0x74F8u  /* per port: the FLAG byte a write read */
#define K_MC_DEVICE      0x7500u  /* per port: device id */
#define K_MC_SECTOR      0x7508u  /* per port: sector */
#define K_MC_OPERATION   0x7514u  /* the handler's byte counter */
#define K_MC_LAST_PORT   0x751Cu  /* the port of the last finished command */
#define K_MC_GOT_ERROR   0x7520u  /* a handler delivered its own error */
#define K_MC_HANDLER     0x7528u  /* per port: the handler of the command */
#define K_MC_NODE        0x7540u  /* the SIO0 handler node {next, handler, verifier, pad} */
#define K_MC_BUFFER      0x7550u  /* per port: buffer */
#define K_MC_RD_BYTE     0x7558u  /* byte per port: the read handler's pad byte */
#define K_MC_ACTIVE      0x755Au  /* byte: a transfer started and not finished */
#define K_MC_CHECKSUM    0x7560u  /* per port: running checksum */
#define K_MC_FLAGS       0x7568u  /* byte per port: _card_status */
#define K_MC_INITED      0x756Cu  /* InitCARD ran */
#define K_MC_FT_ACTIVE   0x75C0u  /* fast track moving data bytes */
#define K_MC_FT_BUFFER   0x75C4u  /* fast track: next data byte */
#define K_MC_FT_FLAGSP   0x75C8u  /* fast track: the port's status byte */
#define K_MC_FT_SUMP     0x75CCu  /* fast track: the port's checksum */
#define K_PAD_CLEAR      0x8914u  /* ChangeClearPad's flag */
#define K_TIMER3_ACK     0x860Cu  /* setTimerAutoAck(3, ...) */
#define K_EXC_HOOK       0x0CF0u  /* the exception handler's card patch */
/* The ROM's handler and node routine addresses, as the driver stores them. */
#define MC_HANDLER_WRITE 0x51F4u
#define MC_HANDLER_READ  0x5688u
#define MC_HANDLER_INFO  0x5B64u
#define MC_NODE_HANDLER  0x4D6Cu
#define MC_NODE_VERIFIER 0x4F90u
#define SIO0_NODE_HANDLER  0x49BCu
#define SIO0_NODE_VERIFIER 0x4A4Cu
#define MC_STATUS_READ   2u
#define MC_STATUS_WRITE  4u
#define MC_STATUS_INFO   8u
/* Bytes the card answers, from the card's protocol: the ID bytes 0x5A 0x5D,
 * the command acknowledges 0x5C 0x5D, and the end byte 'G' (good). */
#define MC_FLAG_WRITE_ERROR 0x04u
#define MC_FLAG_NEW         0x08u

/* The card's own FLAG byte per slot, which the card answers to every
 * command: bit 3 set from insertion until the card's first successful
 * write. A card present at boot counts as inserted then. */
static uint8_t s_card_flag[2];
static int s_card_flag_init = 0;

/** @brief Return a slot's FLAG byte, setting bit 3 on both slots on first use.
 *
 * @param port Port 0 or 1.
 * @return The FLAG byte. */
static uint32_t card_flag(int port) {
    if (!s_card_flag_init) { s_card_flag_init = 1; s_card_flag[0] = s_card_flag[1] = MC_FLAG_NEW; }
    return s_card_flag[port & 1];
}

void openpete_psx_bios_card_flags_get(uint8_t out[2]) {
    out[0] = (uint8_t)card_flag(0);
    out[1] = (uint8_t)card_flag(1);
}

void openpete_psx_bios_card_flags_set(const uint8_t in[2]) {
    s_card_flag_init = 1;
    s_card_flag[0] = in[0];
    s_card_flag[1] = in[1];
}

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

/** @brief Record a card command on its port (B0 0x4D _card_info, 0x4E _card_write, 0x4F _card_read).
 *
 * The ROM's mcGetInfo (RAM 0x5FB8), mcWriteSector (0x5F04) and mcReadSector
 * (0x5E50): refused while the port's status lacks bit 0, and a read or
 * write of a sector outside 0-1024; otherwise the handler's counter and
 * the word at 0x74A4 clear, and the port's device id, buffer, handler,
 * sector and status are stored.
 * @param device_id Device id as the game passed it.
 * @param handler MC_HANDLER_INFO, MC_HANDLER_READ or MC_HANDLER_WRITE.
 * @param sector Sector of a read or write; 0 for info.
 * @param buffer Guest address of the 128-byte buffer; 0 for info.
 * @return 1 when recorded, 0 when refused. */
static uint32_t mc_queue(uint32_t device_id, uint32_t handler, uint32_t sector, uint32_t buffer) {
    const int port = mc_port(device_id);
    const uint32_t status = handler == MC_HANDLER_READ ? MC_STATUS_READ
                          : handler == MC_HANDLER_WRITE ? MC_STATUS_WRITE : MC_STATUS_INFO;
    if (s_census > 0) OPL_INFO_F("bios", "card command 0x%X on port %d queued at record %u, position %u (flip %u, status 0x%X)", handler, port, openpete_sched_current_record(), openpete_sched_pos(), kb(K_MC_FLIP), kb(K_MC_FLAGS + port));
    cw_row("Q %d %x %u %x %u", port, handler, kb(K_MC_FLIP), kb(K_MC_FLAGS + (uint32_t)port), sector);
    if ((kb(K_MC_FLAGS + (uint32_t)port) & 1u) == 0u) return 0u;
    if (handler != MC_HANDLER_INFO && sector > 0x400u) return 0u;
    kw_set(K_MC_OPERATION, 0u);
    kw_set(K_MC_74A4, 0u);
    kw_set(K_MC_DEVICE + 4u * (uint32_t)port, device_id);
    kw_set(K_MC_BUFFER + 4u * (uint32_t)port, buffer);
    kw_set(K_MC_HANDLER + 4u * (uint32_t)port, handler);
    kw_set(K_MC_SECTOR + 4u * (uint32_t)port, sector);
    kb_set(K_MC_FLAGS + (uint32_t)port, status);
    return 1u;
}

static void bu_ll_done(CPUState* cpu);
static void bu_ll_error(CPUState* cpu, uint32_t flag_addr, uint32_t spec);

/** @brief Deliver a handler's own error for the port being served: status 1, the bu callback, then HwCARD.
 *
 * The read, write and info handlers' new-card and write-error arms
 * (RAM 0x5CA4, 0x57E8, 0x534C, 0x55D0): the skip flag clears, the port's
 * status becomes 1 and the last port the served one, the bu layer's
 * callback runs, HwCARD signals spec, and the error is marked so mcHandler
 * delivers nothing more.
 * @param cpu Guest CPU the event handlers run on.
 * @param flag_addr The bu callback's flag word (0xB9D4, 0xB9DC or 0xB9E0).
 * @param spec HwCARD spec: 0x8000, 0x2000 or 0x8001. */
static void mc_handler_error(CPUState* cpu, uint32_t flag_addr, uint32_t spec) {
    const uint32_t port = kb(K_MC_FLIP);
    kw_set(K_MC_SKIP_NEW, 0u);
    kb_set(K_MC_FLAGS + port, 1u);
    kw_set(K_MC_LAST_PORT, port);
    bu_ll_error(cpu, flag_addr, spec == 0x2000u ? EV_SPEC_NEW : EV_SPEC_ERROR);
    deliver_event(cpu, EV_CLASS_CARD, spec);
    kw_set(K_MC_GOT_ERROR, 1u);
}

/* --- The transfer in flight -----------------------------------------------
 * A present card's transfer is a sequence of steps, one per byte the SIO0
 * interrupt services after the select byte: mcHandler's states 2 onward
 * (it increments the handler's byte counter, then runs the command's
 * handler state) and, between them, the exception handler's card patch
 * (RAM 0x641C), which moves the sector's data bytes one per interrupt
 * while the fast track is active (OpenBIOS sio0/driver.c mcHandler,
 * sio0/card.c mcReadHandler, mcWriteHandler and mcInfoHandler,
 * sio0/cardfasttrack.s). A step leaves the bytes its interrupt leaves; the
 * last one ends the transfer (mc_end). Steps are timed from the store
 * that marks the transfer active (state 1, as the select byte goes out)
 * at the byte cadence the SIO0 port and the card give the kernel.
 *
 * openpete_psx_bios_vbl_complete runs every step in the VBL body. Under
 * the oracle, when the reference's anchor of the next record falls inside
 * the transfer (the trace's anchor tick and the `.card` start of the same
 * VBLANK), the steps run up to the anchor tick before the record is
 * compared (openpete_psx_bios_card_anchor) and the rest after it
 * (openpete_psx_bios_card_anchor_end), as on the reference, where the
 * anchor reads the transfer half done and the completion's events follow
 * it. */

/* Steps after state 1: write: states 2-6, 128 fast-track bytes, states
 * 7-10; read: states 2-11, 127 fast-track bytes, states 12-13; info:
 * states 2-4. */
#define MC_W_FT_FIRST  5u      /* write step of the first fast-track byte */
#define MC_W_STEPS     137u
#define MC_R_FT_FIRST  10u
#define MC_R_STEPS     139u
#define MC_I_STEPS     3u
/* The byte cadence in CPU ticks, measured on the reference (DuckStation
 * SCPH-1001, Spyro 3's card-present attract demo, the median over the
 * transfers its anchors fall in, with the main thread idle in its VSync
 * wait): from the active store to state 2, between handler states, from
 * the last state before the fast track to its first byte, between
 * fast-track bytes, and from the last fast-track byte to the next state.
 * Info commands take the read cadence. A held transfer whose `.card` row
 * carries the reference's own step ticks uses those instead: the spacing
 * depends on the kernel's interrupt queues and on the rest of the VBLANK
 * exception that started the transfer, so it differs per game. */
typedef struct { uint32_t first, state, ft_in, ft, ft_out; } mc_cadence_t;
static const mc_cadence_t k_mc_cadence_write = { 5202u, 2978u, 1958u, 1530u, 2806u };
static const mc_cadence_t k_mc_cadence_read  = { 5163u, 2989u, 1977u, 1525u, 2784u };

typedef struct {
    int       live;        /* steps remain */
    int       port;
    uint32_t  handler;
    uint32_t  step;        /* next step */
    uint32_t  steps;       /* the step that ends the transfer, plus one */
    int       read_ok;     /* read: the host card delivered the sector */
    uint8_t   data[MC_FRAME_BYTES];   /* read: the sector the card sends */
    int       held;        /* oracle: steps wait for the record's anchor */
    uint32_t  hold_rec;    /* the record whose anchor falls inside */
    uint32_t  hold_ticks;  /* that anchor's ticks after the active store */
    const uint32_t* ref_steps; /* oracle: the reference's step ticks, when its `.card` row has them */
    uint32_t  ref_n;       /* their count; 0 takes the cadence table */
    CPUState* cpu;         /* the CPU the completion's handlers run on */
} mc_xfer_t;
static mc_xfer_t s_xf;
static uint32_t s_xf_held = 0, s_xf_ref_timed = 0, s_xf_anchor_steps = 0, s_xf_fallbacks = 0;

static void mc_end(CPUState* cpu, int result);

/** @brief Return the ticks from the active store to a step of a transfer.
 *
 * @param handler MC_HANDLER_WRITE, MC_HANDLER_READ or MC_HANDLER_INFO.
 * @param k Step index.
 * @return The step's tick offset. */
static uint32_t mc_step_ticks(uint32_t handler, uint32_t k) {
    const mc_cadence_t* c = handler == MC_HANDLER_WRITE ? &k_mc_cadence_write : &k_mc_cadence_read;
    const uint32_t ft0 = handler == MC_HANDLER_WRITE ? MC_W_FT_FIRST : handler == MC_HANDLER_READ ? MC_R_FT_FIRST : 0xFFFFu;
    const uint32_t ftn = handler == MC_HANDLER_WRITE ? MC_FRAME_BYTES : MC_FRAME_BYTES - 1u;
    if (k < ft0) return c->first + k * c->state;
    const uint32_t ft_start = c->first + (ft0 - 1u) * c->state + c->ft_in;
    if (k < ft0 + ftn) return ft_start + (k - ft0) * c->ft;
    return ft_start + (ftn - 1u) * c->ft + c->ft_out + (k - ft0 - ftn) * c->state;
}

/** @brief Return the ticks from the active store to step k of the transfer in flight.
 *
 * The reference's own step tick when the oracle's sidecar carries the
 * transfer's steps, else the cadence table's.
 * @param k Step index.
 * @return The step's tick offset. */
static uint32_t mc_xf_step_ticks(uint32_t k) {
    return k < s_xf.ref_n ? s_xf.ref_steps[k] : mc_step_ticks(s_xf.handler, k);
}

/** @brief Report whether a transfer stops at its FLAG byte (state 3): a new card, a write error on info, or a sector the host card cannot give.
 * @return 1 when state 3 ends the transfer. */
static int mc_xfer_fails_at_flag(void) {
    const uint32_t flag = card_flag(s_xf.port);
    if (s_xf.handler == MC_HANDLER_READ && !s_xf.read_ok) return 1;
    if (kw(K_MC_SKIP_NEW)) return 0;
    if (s_xf.handler == MC_HANDLER_INFO) return (flag & (MC_FLAG_NEW | MC_FLAG_WRITE_ERROR)) != 0;
    return (flag & MC_FLAG_NEW) != 0;
}

/** @brief Run one fast-track byte (the exception handler's card patch, RAM 0x641C).
 *
 * The byte goes to the read buffer or comes from the write buffer, the
 * pointer and the halfword counter advance, the byte is XORed into the
 * port's checksum, and the patch stops itself after its last byte.
 * @param i The byte's index, 0 first.
 * @param last The index of the patch's last byte. */
static void mc_ft_byte(uint32_t i, uint32_t last) {
    const uint32_t buf = kw(K_MC_FT_BUFFER);
    uint8_t b = 0;
    if (s_xf.handler == MC_HANDLER_READ) {
        b = s_xf.data[i];
        uint8_t* dst = guest_ptr(buf, 1u);
        if (dst) *dst = b;
    } else {
        const uint8_t* src = guest_ptr(buf, 1u);
        b = src ? *src : 0u;
    }
    kw_set(K_MC_FT_BUFFER, buf + 1u);
    const uint32_t sum = K_MC_CHECKSUM + 4u * (uint32_t)s_xf.port;
    kw_set(sum, kw(sum) ^ b);
    kh_set(K_MC_FT_COUNT, i + 1u);
    if (i == last) kh_set(K_MC_FT_ACTIVE, 0u);
}

/** @brief Run step k of an info command (handler RAM 0x5B64, states 2-4).
 *
 * The FLAG byte is checked unless _new_card set the skip flag: bit 2
 * (write error) delivers EvSpERROR, bit 3 (new card) EvSpNEW.
 * @param cpu Guest CPU the event handlers run on.
 * @param k Step index.
 * @return 0 to continue, 1 or -1 when the transfer ends. */
static int mc_info_step(CPUState* cpu, uint32_t k) {
    kw_set(K_MC_OPERATION, 2u + k);
    if (k != 1u || kw(K_MC_SKIP_NEW)) return k == 2u ? 1 : 0;
    const uint32_t flag = card_flag(s_xf.port);
    if (flag & MC_FLAG_WRITE_ERROR) { mc_handler_error(cpu, 0xB9D4u, EV_SPEC_ERROR); return -1; }
    if (flag & MC_FLAG_NEW) { mc_handler_error(cpu, 0xB9DCu, EV_SPEC_NEW); return -1; }
    return 0;
}

/** @brief Run step k of a read command (handler RAM 0x5688, states 2-13, and the fast track).
 *
 * State 3 checks the FLAG byte (bit 3 delivers EvSpNEW unless skipped),
 * state 10 seeds the checksum with the sector bytes and clears the pad
 * byte, state 11 starts the fast track, which stores 127 data bytes, and
 * state 12 stores the 128th.
 * @param cpu Guest CPU the event handlers run on.
 * @param k Step index.
 * @return 0 to continue, 1 or -1 when the transfer ends. */
static int mc_read_step(CPUState* cpu, uint32_t k) {
    const uint32_t port = (uint32_t)s_xf.port;
    if (k >= MC_R_FT_FIRST && k < MC_R_FT_FIRST + MC_FRAME_BYTES - 1u) {
        mc_ft_byte(k - MC_R_FT_FIRST, MC_FRAME_BYTES - 2u);
        return 0;
    }
    const uint32_t state = k < MC_R_FT_FIRST ? 2u + k : k - (MC_FRAME_BYTES - 1u) + 2u;
    kw_set(K_MC_OPERATION, state);
    const uint32_t sector = kw(K_MC_SECTOR + 4u * port);
    switch (state) {
    case 3u:
        if (!kw(K_MC_SKIP_NEW) && (card_flag(s_xf.port) & MC_FLAG_NEW)) { mc_handler_error(cpu, 0xB9DCu, EV_SPEC_NEW); return -1; }
        return s_xf.read_ok ? 0 : -1;
    case 10u:
        kw_set(K_MC_CHECKSUM + 4u * port, ((sector >> 8) & 0xFFu) ^ (sector & 0xFFu));
        kb_set(K_MC_RD_BYTE + port, 0u);
        return 0;
    case 11u:
        kw_set(K_MC_FT_ACTIVE, 1u);
        return 0;
    case 12u: {
        uint8_t* dst = guest_ptr(kw(K_MC_BUFFER + 4u * port) + MC_FRAME_BYTES - 1u, 1u);
        if (dst) *dst = s_xf.data[MC_FRAME_BYTES - 1u];
        kw_set(K_MC_CHECKSUM + 4u * port, kw(K_MC_CHECKSUM + 4u * port) ^ s_xf.data[MC_FRAME_BYTES - 1u]);
        return 0;
    }
    case 13u:
        return 1;
    default:
        return 0;
    }
}

/** @brief Run step k of a write command (handler RAM 0x51F4, states 2-10, and the fast track).
 *
 * State 3 stores the FLAG byte for the port (bit 3 delivers EvSpNEW unless
 * skipped), states 5 and 6 seed the checksum with the sector bytes and
 * start the fast track, which sends the 128 data bytes; state 10 delivers
 * the handler's 0x8001 on a stored write-error bit, the sector goes to the
 * host card and the card clears its new-card bit.
 * @param cpu Guest CPU the event handlers run on.
 * @param k Step index.
 * @return 0 to continue, 1 or -1 when the transfer ends. */
static int mc_write_step(CPUState* cpu, uint32_t k) {
    const uint32_t port = (uint32_t)s_xf.port;
    if (k >= MC_W_FT_FIRST && k < MC_W_FT_FIRST + MC_FRAME_BYTES) {
        mc_ft_byte(k - MC_W_FT_FIRST, MC_FRAME_BYTES - 1u);
        return 0;
    }
    const uint32_t state = k < MC_W_FT_FIRST ? 2u + k : k - MC_FRAME_BYTES + 2u;
    kw_set(K_MC_OPERATION, state);
    const uint32_t sector = kw(K_MC_SECTOR + 4u * port);
    switch (state) {
    case 3u:
        kw_set(K_MC_WFLAG + 4u * port, card_flag(s_xf.port));
        if (!kw(K_MC_SKIP_NEW) && (card_flag(s_xf.port) & MC_FLAG_NEW)) { mc_handler_error(cpu, 0xB9DCu, EV_SPEC_NEW); return -1; }
        return 0;
    case 5u:
        kw_set(K_MC_CHECKSUM + 4u * port, (sector >> 8) & 0xFFu);
        return 0;
    case 6u:
        kw_set(K_MC_CHECKSUM + 4u * port, kw(K_MC_CHECKSUM + 4u * port) ^ (sector & 0xFFu));
        kw_set(K_MC_FT_ACTIVE, 1u);
        return 0;
    case 10u: {
        const uint8_t* src = guest_ptr(kw(K_MC_BUFFER + 4u * port), MC_FRAME_BYTES);
        if (!src) return -1;
        if (!kw(K_MC_SKIP_NEW) && (kw(K_MC_WFLAG + 4u * port) & MC_FLAG_WRITE_ERROR))
            mc_handler_error(cpu, 0xB9E0u, 0x8001u);
        if (openpete_memcard_write_frame(s_xf.port, (int)sector, src) != 0) return -1;
        s_card_flag[port & 1u] &= (uint8_t)~MC_FLAG_NEW;
        return 1;
    }
    default:
        return 0;
    }
}

/** @brief Run the in-flight transfer's steps up to a tick, ending the transfer with its last.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param until The last tick after the active store to run steps at;
 *        UINT32_MAX runs every step. */
static void mc_xfer_run(CPUState* cpu, uint32_t until) {
    while (s_xf.live && (until == UINT32_MAX || mc_xf_step_ticks(s_xf.step) <= until)) {
        const uint32_t k = s_xf.step++;
        const int r = s_xf.handler == MC_HANDLER_WRITE ? mc_write_step(cpu, k)
                    : s_xf.handler == MC_HANDLER_READ  ? mc_read_step(cpu, k)
                                                       : mc_info_step(cpu, k);
        if (until != UINT32_MAX) s_xf_anchor_steps++;
        if (r != 0 || s_xf.step >= s_xf.steps) {
            s_xf.live = 0;
            s_xf.held = 0;
            mc_end(cpu, r != 0 ? r : -1);
        }
    }
}

/** @brief Start stepping the transfer the vblank step began on a present card's port.
 *
 * A read takes its sector from the host card here, the card's answer to
 * the read command. The transfer ends at its FLAG state when that state
 * fails, else at its last state.
 * @param cpu Guest CPU the event handlers run on.
 * @param port The served port.
 * @param handler The command's handler. */
static void mc_xfer_begin(CPUState* cpu, int port, uint32_t handler) {
    memset(&s_xf, 0, sizeof(s_xf));
    s_xf.live = 1;
    s_xf.port = port;
    s_xf.handler = handler;
    s_xf.cpu = cpu;
    if (handler == MC_HANDLER_READ) {
        const uint32_t sector = kw(K_MC_SECTOR + 4u * (uint32_t)port);
        s_xf.read_ok = guest_ptr(kw(K_MC_BUFFER + 4u * (uint32_t)port), MC_FRAME_BYTES) != NULL
                    && openpete_memcard_read_frame(port, (int)sector, s_xf.data) == 0;
    }
    s_xf.steps = mc_xfer_fails_at_flag() ? 2u
               : handler == MC_HANDLER_WRITE ? MC_W_STEPS : handler == MC_HANDLER_READ ? MC_R_STEPS : MC_I_STEPS;
}

/** @brief Under the oracle, hold the transfer when the reference's next record anchors inside it.
 *
 * The record the next anchor hit compares must carry the VBLANK of the
 * step that began the transfer, the `.card` sidecar must hold that
 * VBLANK's start, and the anchor tick must fall after the start and
 * before the transfer's last step.
 * @param vbl The reference VBLANK of the step.
 * @return 1 when the transfer is held for the anchor. */
static int mc_xfer_hold(uint32_t vbl) {
    const uint32_t rec = openpete_sched_current_record();
    uint32_t start, rvbl, rtick;
    if (!openpete_sched_card_start(vbl, &start) || !openpete_sched_anchor(rec, &rvbl, &rtick)) return 0;
    /* A row whose step count is not the transfer's (a sidecar without
     * steps, or a transfer that ended differently) takes the table. */
    s_xf.ref_steps = openpete_sched_card_steps(vbl, &s_xf.ref_n);
    if (s_xf.ref_n != s_xf.steps) { s_xf.ref_steps = NULL; s_xf.ref_n = 0; }
    if (rvbl != vbl || rtick <= start || rtick - start >= mc_xf_step_ticks(s_xf.steps - 1u)) return 0;
    s_xf.held = 1;
    if (s_xf.ref_n) s_xf_ref_timed++;
    s_xf.hold_rec = rec;
    s_xf.hold_ticks = rtick - start;
    s_xf_held++;
    return 1;
}

void openpete_psx_bios_card_anchor(uint32_t rec) {
    if (!s_xf.live || !s_xf.held) return;
    if (rec == s_xf.hold_rec) { mc_xfer_run(s_xf.cpu, s_xf.hold_ticks); return; }
    s_xf_fallbacks++;
    mc_xfer_run(s_xf.cpu, UINT32_MAX);
}

void openpete_psx_bios_card_anchor_end(void) {
    if (s_xf.live && s_xf.held) mc_xfer_run(s_xf.cpu, UINT32_MAX);
}

void openpete_psx_bios_card_anchor_counts(uint32_t* held, uint32_t* ref_timed, uint32_t* anchor_steps, uint32_t* fallbacks) {
    *held = s_xf_held;
    *ref_timed = s_xf_ref_timed;
    *anchor_steps = s_xf_anchor_steps;
    *fallbacks = s_xf_fallbacks;
}

/** @brief End the served port's transfer with the handler's result (mcHandler's result arms, RAM 0x4DE4 and 0x4E34).
 *
 * Success (1) clears the transfer, the handler's counter and the
 * exception-queue node and, unless a handler error was delivered, sets the
 * port's status to 1, runs the bu layer's completion and delivers EvSpIOE
 * on HwCARD. Failure clears the transfer and, unless a handler error was
 * delivered, sets status 0x21, runs the bu layer's error and delivers
 * EvSpERROR; the error mark clears.
 * @param cpu Guest CPU the event handlers run on.
 * @param result 1 or -1. */
static void mc_end(CPUState* cpu, int result) {
    const uint32_t port = kb(K_MC_FLIP);
    if (result == 1) {
        kw_set(K_MC_74A4, 0u);
        kb_set(K_MC_ACTIVE, 0u);
        kw_set(K_MC_OPERATION, 0u);
        int_deq(1u, K_MC_NODE);
        if (!kw(K_MC_GOT_ERROR)) {
            kw_set(K_MC_SKIP_NEW, 0u);
            kb_set(K_MC_FLAGS + port, 1u);
            kw_set(K_MC_LAST_PORT, port);
            bu_ll_done(cpu);
            deliver_event(cpu, EV_CLASS_CARD, EV_SPEC_IOE);
        }
    } else {
        kb_set(K_MC_ACTIVE, 0u);
        if (!kw(K_MC_GOT_ERROR)) {
            kw_set(K_MC_SKIP_NEW, 0u);
            kb_set(K_MC_FLAGS + port, 0x21u);
            kw_set(K_MC_LAST_PORT, port);
            bu_ll_error(cpu, 0xB9D4u, EV_SPEC_ERROR);
            deliver_event(cpu, EV_CLASS_CARD, EV_SPEC_ERROR);
        }
        kw_set(K_MC_OPERATION, 0u);
        kw_set(K_MC_GOT_ERROR, 0u);
        int_deq(1u, K_MC_NODE);
    }
    cw_row("F %u %02x", port, kb(K_MC_FLAGS + port));
}

static uint32_t s_step_key = 0;   /* the reference VBLANK index of the vblank step running */

/** @brief Start the served port's command: the fast track, the handler node, and the handler's select byte (firstStageCardAction's start, then mcHandler's first call).
 *
 * @param port The port the step flipped to. */
static void mc_begin(uint32_t port) {
    kw_set(K_MC_FT_COUNT, 0u);
    kw_set(K_MC_FT_ACTIVE, 0u);
    kw_set(K_MC_FT_BUFFER, kw(K_MC_BUFFER + 4u * port));
    kw_set(K_MC_FT_FLAGSP, K_MC_FLAGS + port);
    kw_set(K_MC_FT_SUMP, K_MC_CHECKSUM + 4u * port);
    int_deq(1u, K_MC_NODE);
    int_enq(1u, K_MC_NODE);
    kw_set(K_MC_OPERATION, 0u);
    kw_set(K_MC_GOT_ERROR, 0u);
    const uint32_t mask = port ? 0x2000u : 0u;
    kw_set(K_MC_SIO_MASK, mask);
    kw_set(K_MC_OPERATION, 1u);
    kb_set(K_MC_ACTIVE, 1u);
    cw_row("C %u %x %u %u", port, kb(K_MC_FLAGS + port), kw(K_MC_SECTOR + 4u * port), s_step_key);
}

/** @brief Abort the transfer no card answered (firstStageCardAction, RAM 0x5068): status 0x11, the bu timeout, EvSpTIMOUT.
 *
 * @param cpu Guest CPU the event handlers run on. */
static void mc_timeout(CPUState* cpu) {
    const uint32_t port = kb(K_MC_FLIP);
    if (s_census > 0) OPL_INFO_F("bios", "card timeout on port %u delivered at record %u, position %u", port, openpete_sched_current_record(), openpete_sched_pos());
    kb_set(K_MC_ACTIVE, 0u);
    kw_set(K_MC_74A4, 0u);
    kw_set(K_MC_OPERATION, 0u);
    kw_set(K_MC_SKIP_NEW, 0u);
    kb_set(K_MC_FLAGS + port, 0x11u);
    kw_set(K_MC_LAST_PORT, port);
    bu_ll_error(cpu, 0xB9D8u, EV_SPEC_TIMOUT);
    deliver_event(cpu, EV_CLASS_CARD, EV_SPEC_TIMOUT);
    int_deq(1u, K_MC_NODE);
    cw_row("F %u %02x", port, 0x11u);
}

/* One vblank interrupt of the card driver (firstStageCardAction).
 * The handler runs only on the vblanks the reference's controller handler
 * ran on: a vblank the schedule marks unserved is skipped, unless a
 * transfer is in progress and the following vblank is served, which is
 * the abort's own vblank (the abort returns before the pad poll, so the
 * recorder saw no command there either). */
void openpete_psx_bios_vbl(CPUState* cpu) {
    openpete_psx_bios_vbl_at(cpu, openpete_sched_pos() + 1u);
}

int openpete_psx_bios_card_busy(void) { return kw(K_MC_STARTED) && kb(K_MC_ACTIVE) != 0u; }

void openpete_psx_bios_vbl_at(CPUState* cpu, uint32_t vbl) {
    /* A held transfer whose anchor never came ends before the next step. */
    if (s_xf.live) { s_xf_fallbacks++; mc_xfer_run(cpu, UINT32_MAX); }
    if (!kw(K_MC_STARTED)) return;
    s_step_key = vbl;
    int known = 0;
    const int served = openpete_sched_sio0_served(vbl, &known);
    const int active = kb(K_MC_ACTIVE) != 0u;
    if (known && !served && !(active && openpete_sched_sio0_served(vbl + 1u, NULL))) { cw_row("P - %u", vbl); return; }
    static const uint16_t k_specs[5] = { EV_SPEC_IOE, EV_SPEC_ERROR, EV_SPEC_TIMOUT, 0x0200u, EV_SPEC_NEW };
    for (int i = 0; i < 5; i++) undeliver_event(EV_CLASS_CARD, k_specs[i]);
    if (active) { cw_row("P a %u", vbl); mc_timeout(cpu); return; }
    const uint32_t port = 1u - (kb(K_MC_FLIP) & 1u);
    kb_set(K_MC_FLIP, port);
    cw_row("P %u %u", port, vbl);
    if (kb(K_MC_FLAGS + port) & 1u) return;
    if (s_census > 0) OPL_INFO_F("bios", "card command on port %u started at record %u, position %u", port, openpete_sched_current_record(), openpete_sched_pos());
    mc_begin(port);
}

void openpete_psx_bios_vbl_complete(CPUState* cpu) {
    if (!kb(K_MC_ACTIVE) || s_xf.live) return;
    const int port = (int)(kb(K_MC_FLIP) & 1u);
    if (!openpete_memcard_present(port)) return;
    const uint32_t handler = kw(K_MC_HANDLER + 4u * (uint32_t)port);
    if (handler != MC_HANDLER_INFO && handler != MC_HANDLER_READ && handler != MC_HANDLER_WRITE) { mc_end(cpu, -1); return; }
    mc_xfer_begin(cpu, port, handler);
    if (mc_xfer_hold(s_step_key)) return;
    mc_xfer_run(cpu, UINT32_MAX);
}

/** @brief Handle the B0 card driver services 0x4A-0x4F, 0x50, 0x58 and 0x5B-0x5D over the driver's RAM.
 *
 * InitCARD (RAM 0x5DA8), StartCARD (0x4C70), StopCARD (0x4CF4), _card_info
 * (0x5FB8), _card_write (0x5F04), _card_read (0x5E50), _new_card (0x4D3C),
 * _card_chan (0x5E30), ChangeClearPAD (0x43D0), _card_status (0x605C) and
 * _card_wait (0x6070). _card_wait spins on hardware until the port's status
 * has bit 0; the vblank steps that spin needs run in the VBL body, which
 * this gate cannot enter, so the status is returned as it stands.
 * @param cpu Guest CPU; the argument registers are read and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is one of these services, 0 otherwise. */
static int bios_b0_card(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x4A: {    /* InitCARD(pad_enable) */
        static const uint32_t hook[4] = { 0x3C020000u, 0x2442641Cu, 0x0040F809u, 0u };
        kw_set(K_SIO0_NODE, 0u);
        kw_set(K_SIO0_NODE + 4u, SIO0_NODE_HANDLER);
        kw_set(K_SIO0_NODE + 8u, SIO0_NODE_VERIFIER);
        kw_set(K_SIO0_NODE + 12u, 0u);
        kb_set(K_MC_ACTIVE, 0u);
        s_xf.live = 0;
        kb_set(K_MC_FLIP, 0u);
        kb_set(K_MC_FLAGS, 1u);
        kb_set(K_MC_FLAGS + 1u, 1u);
        kw_set(K_MC_NODE + 4u, MC_NODE_HANDLER);
        kw_set(K_MC_NODE + 8u, MC_NODE_VERIFIER);
        kw_set(K_MC_NODE, 0u);
        kw_set(K_MC_NODE + 12u, 0u);
        kw_set(K_MC_FT_ACTIVE, 0u);
        for (uint32_t i = 0; i < 4u; i++) kw_set(K_EXC_HOOK + 4u * i, hook[i]);
        cpu->v0 = kw(K_MC_INITED);
        kw_set(K_MC_INITED, 1u);
        kw_set(K_PAD_ENABLE, cpu->a0);
        return 1;
    }
    case 0x4B:      /* StartCARD: hook the vblank step; the kernel body
                     * ends in leaveCriticalSection */
        if (s_census > 0) OPL_INFO_F("bios", "StartCARD at record %u, position %u", openpete_sched_current_record(), openpete_sched_pos());
        kw_set(K_MC_SKIP_NEW, 0u);
        int_deq(2u, K_SIO0_NODE);
        int_enq(2u, K_SIO0_NODE);
        kw_set(K_PAD_CLEAR, 1u);
        kw_set(K_TIMER3_ACK, 0u);
        kw_set(K_MC_STARTED, 1u);
        cpu->v0 = 1u;
        openpete_crit_leave(cpu);
        return 1;
    case 0x4C:      /* StopCARD; leaves the critical section */
        kw_set(K_TIMER3_ACK, 1u);
        int_deq(2u, K_SIO0_NODE);
        kw_set(K_MC_STARTED, 0u);
        cpu->v0 = 1u;
        openpete_crit_leave(cpu);
        return 1;
    case 0x4D:      /* _card_info(chan) */
    case 0x4E:      /* _card_write(chan, sector, buf) */
    case 0x4F: {    /* _card_read(chan, sector, buf) */
        const uint32_t h = fn == 0x4D ? MC_HANDLER_INFO : fn == 0x4E ? MC_HANDLER_WRITE : MC_HANDLER_READ;
        cpu->v0 = fn == 0x4D ? mc_queue(cpu->a0, h, 0u, 0u) : mc_queue(cpu->a0, h, cpu->a1, cpu->a2);
        cw_row("B %02x %08x %08x %08x %08x", fn, cpu->a0, cpu->a1, cpu->a2, cpu->v0);
        return 1;
    }
    case 0x50:      /* _new_card: v0 is left as the caller set it */
        kw_set(K_MC_SKIP_NEW, 1u);
        return 1;
    case 0x58:      /* _card_chan: the device id of the last finished command */
        cpu->v0 = kw(K_MC_DEVICE + 4u * (kw(K_MC_LAST_PORT) & 1u));
        return 1;
    case 0x5B:      /* ChangeClearPAD(on): the previous value */
        cpu->v0 = kw(K_PAD_CLEAR);
        kw_set(K_PAD_CLEAR, cpu->a0);
        return 1;
    case 0x5C:      /* _card_status(chan) */
    case 0x5D:      /* _card_wait(chan) */
        if (fn == 0x5D && (kb(K_MC_FLAGS + cpu->a0) & 1u) == 0u)
            OPL_WARN_F("bios", "_card_wait on a pending command returns without waiting");
        cpu->v0 = kb(K_MC_FLAGS + cpu->a0);
        return 1;
    default:
        return 0;
    }
}

/* --- The bu layer -----------------------------------------------------------
 * The kernel's backup-unit layer over the card driver, at the SCPH-1001
 * addresses its ROM code uses (OpenBIOS card/backupunit.c and
 * card/device.c are the same layer): per port the bu operation (1 info, 2
 * read, 3 write, 4 directory load, 7 a write's closing info), the
 * directory load's stage and index, a transfer's sector index, sectors
 * left, file and buffer; the low-level callbacks' flags; the broken-sector
 * lists, the directory cache and the sector buffers. The card driver calls the layer at each command's end (A0
 * 0xA7 done, 0xA8 error, 0xA9 timeout, 0xAA new card, 0xAE write error),
 * before HwCARD; an operation chains its next sector from there and ends
 * by delivering on the bu class. */
#define K_BU_OP       0x9F20u   /* per port */
#define K_BU_STAGE    0x9F28u   /* per port: directory load stage 1-3 */
#define K_BU_INDEX    0x9F30u   /* per port: directory entry or broken-list index */
#define K_BU_SECTOR   0x9F50u   /* per port: a read's sector index in the file */
#define K_BU_REMAIN   0x9F58u   /* per port: a read's sectors left */
#define K_BU_FILE     0x9F70u   /* per port: a read's or write's file control block */
#define K_BU_BUFFER   0x9F78u   /* per port: a read's or write's buffer */
#define K_BU_OP_ERROR 0x9F80u   /* per port: a synchronous transfer's failing status index */
#define K_BU_ACTUAL   0x9F88u   /* halfword per port: the card sector a write issued */
#define K_BU_FOUND    0x9F8Cu   /* the last directory index a name search found */
#define K_BU_AUTOFMT  0x9F90u   /* format a card without "MC" (cleared by _bu_init) */
#define K_DEV_STATUS  0x8644u   /* C(1Ah) setDeviceStatus, RAM 0x3E5C */
#define K_BU_LL_DONE  0xB9D0u   /* A0 0xA7 ran; 0xB9D4-0xB9E0 the error callbacks */
#define K_BU_BROKEN   0xB9E8u   /* per port 20 words: the broken sectors */
#define K_BU_BROKEN_PORT 0x50u
#define K_BU_SECBUF   0xA000BE48u  /* per port 128 bytes, as the ROM addresses them */
#define BU_OP_INFO    1u
#define BU_OP_READ    2u
#define BU_OP_TOC     4u
#define BU_OP_WRITE   3u
#define BU_OP_WRITE_INFO 7u     /* a write's trailing card info */

/** @brief Return the name of the file a bu file control block names, from the port's directory cache.
 *
 * @param port The port.
 * @param f The file control block.
 * @return The name (20 bytes at entry + 0xA), or NULL when the block's index is out of range. */
static const char* bu_fcb_name(uint32_t port, uint32_t f) {
    const uint32_t idx = kw(f + FCB_LBA);
    if (idx >= 15u) return NULL;
    return (const char*)openpete_psx_get_ram() + K_BU_DIR + port * K_BU_DIR_PORT + idx * BU_DIR_ENTRY + 0xAu;
}

/** @brief Report a bu write's end to the per-mod save records: the file the port's write names, and whether it completed.
 *
 * @param port The port.
 * @param ok 1 for the trailing card info's completion, 0 for a failure. */
static void bu_write_capture_end(uint32_t port, int ok) {
    const uint32_t f = kw(K_BU_FILE + 4u * port);
    openpete_modsave_bu_write_done((int)port, f ? bu_fcb_name(port, f) : NULL, ok);
}

/** @brief Finish a port's bu operation and signal the bu class (buFinishAndTrigger, ROM 0xBFC0BFF0).
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param spec The bu spec. */
static void bu_finish(CPUState* cpu, uint32_t port, uint32_t spec) {
    const uint32_t op = kw(K_BU_OP + 4u * port);
    if (op == BU_OP_WRITE_INFO && spec == EV_SPEC_IOE) bu_write_capture_end(port, 1);
    else if ((op == BU_OP_WRITE || op == 6u || op == BU_OP_WRITE_INFO) && spec != EV_SPEC_IOE) bu_write_capture_end(port, 0);
    kw_set(K_BU_OP + 4u * port, 0u);
    kw_set(K_BU_STAGE + 4u * port, 0u);
    kw_set(K_BU_INDEX + 4u * port, 0u);
    deliver_event(cpu, EV_CLASS_CARD_BU, spec);
}

/** @brief Return the port of the last finished card command, as _card_chan names it.
 *
 * @return 0 or 1. */
static uint32_t bu_chan_port(void) {
    return (uint32_t)mc_port(kw(K_MC_DEVICE + 4u * (kw(K_MC_LAST_PORT) & 1u)));
}

/** @brief Fail a bu operation whose next sector could not be issued: the done flag clears, the error flag sets, EvSpERROR on the bu class.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port. */
static void bu_issue_failed(CPUState* cpu, uint32_t port) {
    kw_set(K_BU_LL_DONE, 0u);
    kw_set(K_BU_LL_DONE + 4u, 1u);
    bu_finish(cpu, port, EV_SPEC_ERROR);
}

/** @brief Run a low-level error callback: its flag, then the port's bu operation ends with spec (A0 0xA8, 0xA9, 0xAA, 0xAE).
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param flag_addr The callback's flag word.
 * @param spec The bu spec. */
static void bu_ll_error(CPUState* cpu, uint32_t flag_addr, uint32_t spec) {
    kw_set(flag_addr, 1u);
    const uint32_t port = bu_chan_port();
    if (kw(K_BU_OP + 4u * port)) bu_finish(cpu, port, spec);
}

/** @brief Return the card sector holding a file's n-th sector, following the directory's block links (ROM 0xBFC0A3D4).
 *
 * @param port The port.
 * @param first The file's first directory index.
 * @param n The sector index in the file.
 * @return The card sector, or -1 when the chain ends first. */
static int32_t bu_file_sector(uint32_t port, uint32_t first, int32_t n) {
    uint32_t block = first;
    while (n >= 64) {
        block = kh(K_BU_DIR + port * K_BU_DIR_PORT + block * BU_DIR_ENTRY + 8u);
        n -= 64;
        if (block == 0xFFFFu) return -1;
    }
    return (int32_t)(block * 64u) + n + 64;
}

/** @brief Return the replacement sector of a broken card sector, or -1 (ROM 0xBFC09540).
 *
 * @param port The port.
 * @param sector The card sector.
 * @return 36 plus the broken-list index holding the sector, or -1. */
static int32_t bu_broken_sector(uint32_t port, int32_t sector) {
    for (uint32_t i = 0; i < 20u; i++)
        if ((int32_t)kw(K_BU_BROKEN + port * K_BU_BROKEN_PORT + 4u * i) == (int32_t)(int16_t)sector) return (int32_t)(36u + i);
    return -1;
}

/** @brief Test a directory or broken-list sector's checksum: the XOR of its first 127 bytes equals its last (ROM 0xBFC08720).
 *
 * @param addr Guest address of the 128-byte sector.
 * @return 1 when the checksum holds. */
static int bu_sector_ok(uint32_t addr) {
    uint32_t x = 0;
    for (uint32_t i = 0; i < 127u; i++) x ^= kb(addr + i);
    return x == kb(addr + 127u);
}

/** @brief Clear a directory entry to free: state 0xA0, size 0, no next block.
 *
 * @param e The entry's guest address. */
static void bu_dir_free(uint32_t e) {
    kw_set(e, 0xA0u);
    kw_set(e + 4u, 0u);
    kh_set(e + 8u, 0xFFFFu);
}

/** @brief Free a file whose block chain reaches a block in the wrong state (ROM 0xBFC08FFC).
 *
 * A first block (state 0x51) whose chain meets a free block (state 0xA_),
 * or a deleted first block (0xA1) whose chain meets a used block (0x5_),
 * is freed with every block its size covers.
 * @param port The port.
 * @param s The entry index.
 * @return 1 when the file was freed, 0 otherwise. */
static int bu_dir_check(uint32_t port, uint32_t s) {
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT, e = dir + s * BU_DIR_ENTRY;
    const uint32_t st = kb(e);
    if (st != 0x51u && st != 0xA1u) return 0;
    const uint32_t bad = st == 0x51u ? 0xA0u : 0x50u;
    const int32_t blocks = (int32_t)kw(e + 4u) / 8192 - 1;
    const uint32_t first_next = kh(e + 8u);
    int broken = 0;
    uint32_t next = first_next;
    for (int32_t b = blocks; b > 0 && next != 0xFFFFu; b--) {
        if ((kw(dir + next * BU_DIR_ENTRY) & 0xF0u) == bad) { broken = 1; break; }
        next = kh(dir + next * BU_DIR_ENTRY + 8u);
    }
    if (!broken) return 0;
    kb_set(e + 10u, 0u);
    bu_dir_free(e);
    next = first_next;
    for (int32_t b = blocks; b > 0 && next != 0xFFFFu; b--) {
        const uint32_t le = dir + next * BU_DIR_ENTRY;
        next = kh(le + 8u);
        bu_dir_free(le);
    }
    return 1;
}

/** @brief Free every directory entry no file reaches (the directory load's pass after its last sector, ROM 0xBFC0BCD0).
 *
 * @param port The port. */
static void bu_dir_settle(uint32_t port) {
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
    uint32_t used[15];
    for (uint32_t s = 0; s < 15u; s++) used[s] = bu_dir_check(port, s) ? 82u : 0u;
    for (uint32_t s = 0; s < 15u; s++) {
        const uint32_t e = dir + s * BU_DIR_ENTRY;
        if (kw(e) != 0x51u) continue;
        used[s] = 1u;
        uint32_t next = kh(e + 8u);
        for (int32_t b = (int32_t)kw(e + 4u) / 8192 - 1; b > 0 && next != 0xFFFFu; b--) {
            if (next < 15u) used[next]++;
            next = kh(dir + next * BU_DIR_ENTRY + 8u);
        }
    }
    for (uint32_t s = 0; s < 15u; s++)
        if (!used[s]) kw_set(dir + s * BU_DIR_ENTRY, 0xA0u);
}

/** @brief Directory load stage 1, sector 0 arrived: without "MC" the load ends with EvSpNEW; otherwise the port's 15 directory entries are cleared to free and sector 1 is read.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_toc_header(CPUState* cpu, uint32_t port, uint32_t dev) {
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
    if (kb(buf) != 'M' || kb(buf + 1u) != 'C') {
        kw_set(K_BU_LL_DONE, 0u);
        kw_set(K_BU_LL_DONE + 12u, 1u);
        bu_finish(cpu, port, EV_SPEC_NEW);
        return;
    }
    for (uint32_t s = 0; s < 15u; s++) {
        void* e = guest_ptr(dir + s * BU_DIR_ENTRY, BU_DIR_ENTRY);
        if (e) memset(e, 0, BU_DIR_ENTRY);
        bu_dir_free(dir + s * BU_DIR_ENTRY);
    }
    if (mc_queue(dev, MC_HANDLER_READ, 1u, buf) != 1u) { bu_issue_failed(cpu, port); return; }
    kw_set(K_BU_INDEX + 4u * port, 0u);
    kw_set(K_BU_STAGE + 4u * port, 2u);
}

/** @brief Directory load stage 2, a directory sector arrived: its first 32 bytes become the next entry when its checksum holds; after the 15th the directory settles, the broken list resets to -1 and sector 16 is read.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_toc_dir(CPUState* cpu, uint32_t port, uint32_t dev) {
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    const uint32_t idx_at = K_BU_INDEX + 4u * port;
    const void* src = guest_ptr(buf, BU_DIR_ENTRY);
    void* dst = guest_ptr(K_BU_DIR + port * K_BU_DIR_PORT + kw(idx_at) * BU_DIR_ENTRY, BU_DIR_ENTRY);
    if (bu_sector_ok(buf) && src && dst) memcpy(dst, src, BU_DIR_ENTRY);
    kw_set(idx_at, kw(idx_at) + 1u);
    if ((int32_t)kw(idx_at) < 15) {
        if (mc_queue(dev, MC_HANDLER_READ, kw(idx_at) + 1u, buf) != 1u) bu_issue_failed(cpu, port);
        return;
    }
    bu_dir_settle(port);
    for (uint32_t i = 0; i < 20u; i++) kw_set(K_BU_BROKEN + port * K_BU_BROKEN_PORT + 4u * i, 0xFFFFFFFFu);
    if (mc_queue(dev, MC_HANDLER_READ, 16u, buf) != 1u) bu_finish(cpu, port, EV_SPEC_ERROR);
    kw_set(idx_at, 0u);
    kw_set(K_BU_STAGE + 4u * port, 3u);
}

/** @brief Directory load stage 3, a broken-list sector arrived: its first word becomes the next broken sector when its checksum holds; after the 20th the load ends with EvSpIOE.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_toc_broken(CPUState* cpu, uint32_t port, uint32_t dev) {
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    const uint32_t idx_at = K_BU_INDEX + 4u * port;
    if (bu_sector_ok(buf)) kw_set(K_BU_BROKEN + port * K_BU_BROKEN_PORT + 4u * kw(idx_at), kw(buf));
    kw_set(idx_at, kw(idx_at) + 1u);
    if ((int32_t)kw(idx_at) >= 20) { bu_finish(cpu, port, EV_SPEC_IOE); return; }
    if (mc_queue(dev, MC_HANDLER_READ, kw(idx_at) + 16u, buf) != 1u) bu_issue_failed(cpu, port);
}

/** @brief Advance the directory load after a sector arrived (ROM 0xBFC0BAA0), by its stage: sector 0's "MC", the 15 directory sectors, the 20 broken-list sectors; another stage fails the load.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_toc_step(CPUState* cpu, uint32_t port, uint32_t dev) {
    switch (kw(K_BU_STAGE + 4u * port)) {
    case 1u: bu_toc_header(cpu, port, dev); return;
    case 2u: bu_toc_dir(cpu, port, dev); return;
    case 3u: bu_toc_broken(cpu, port, dev); return;
    default: bu_issue_failed(cpu, port); return;
    }
}

/* The span of each port's asynchronous read, kept for the card window's R
 * row only: host state outside the guest, so a savestate taken mid-read
 * leaves the row's span from the run that started the read. */
static uint32_t s_cw_read_buf[2], s_cw_read_bytes[2];

/** @brief Write the card window's R row for a finished asynchronous read: its buffer, length and the FNV-1a 64 hash of the bytes the buffer holds.
 *
 * @param port The port whose read finished. */
static void cw_read_done(uint32_t port) {
    if (!s_cw_state || port > 1u) return;
    const void* p = guest_ptr(s_cw_read_buf[port], s_cw_read_bytes[port]);
    cw_row("R %u %08x %u %016llx", port, s_cw_read_buf[port], s_cw_read_bytes[port],
           (unsigned long long)(p ? openpete_fnv1a64(p, s_cw_read_bytes[port]) : 0u));
}

/** @brief Advance an asynchronous read after a sector arrived (ROM 0xBFC0B6BC).
 *
 * The last sector finishes the bu operation with EvSpIOE and then signals
 * the file's own class (its fd) with 0x0004; otherwise the buffer and the
 * file offset advance by 128 and the next sector, through the block chain
 * and the broken-sector list, is issued.
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_read_step(CPUState* cpu, uint32_t port, uint32_t dev) {
    const uint32_t remain = kw(K_BU_REMAIN + 4u * port) - 1u;
    kw_set(K_BU_REMAIN + 4u * port, remain);
    const uint32_t f = kw(K_BU_FILE + 4u * port);
    if (remain == 0u) {
        cw_read_done(port);
        bu_finish(cpu, port, EV_SPEC_IOE);
        deliver_event(cpu, kw(f + 0x28u), EV_SPEC_IOE);
        return;
    }
    kw_set(K_BU_BUFFER + 4u * port, kw(K_BU_BUFFER + 4u * port) + MC_FRAME_BYTES);
    kw_set(f + 0x10u, kw(f + 0x10u) + MC_FRAME_BYTES);
    const uint32_t n = kw(K_BU_SECTOR + 4u * port) + 1u;
    kw_set(K_BU_SECTOR + 4u * port, n);
    int32_t sector = bu_file_sector(port, kw(f + 0x24u), (int32_t)n);
    const int32_t sub = bu_broken_sector(port, sector);
    if (sub != -1) sector = sub;
    if (mc_queue(dev, MC_HANDLER_READ, (uint32_t)sector, kw(K_BU_BUFFER + 4u * port)) != 1u) bu_issue_failed(cpu, port);
}

static uint32_t bu_card_info(uint32_t dev);

/** @brief Advance an asynchronous write after a sector was written (ROM 0xBFC0B7F4).
 *
 * After the last sector the done flag clears and a card info command is
 * recorded (its failure ends the operation with EvSpERROR), and the port's
 * operation becomes 7 either way. Otherwise the buffer and the file offset
 * advance by 128 and the next sector, through the block chain and the
 * broken-sector list, is recorded at 0x9F88 and written.
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port.
 * @param dev The device id the commands address. */
static void bu_write_step(CPUState* cpu, uint32_t port, uint32_t dev) {
    const uint32_t remain = kw(K_BU_REMAIN + 4u * port) - 1u;
    kw_set(K_BU_REMAIN + 4u * port, remain);
    if (remain == 0u) {
        kw_set(K_BU_LL_DONE, 0u);
        if (bu_card_info(dev) != 1u) {
            kw_set(K_BU_LL_DONE + 4u, 1u);
            bu_finish(cpu, port, EV_SPEC_ERROR);
        }
        kw_set(K_BU_OP + 4u * port, BU_OP_WRITE_INFO);
        return;
    }
    const uint32_t f = kw(K_BU_FILE + 4u * port);
    kw_set(K_BU_BUFFER + 4u * port, kw(K_BU_BUFFER + 4u * port) + MC_FRAME_BYTES);
    kw_set(f + 0x10u, kw(f + 0x10u) + MC_FRAME_BYTES);
    const uint32_t n = kw(K_BU_SECTOR + 4u * port) + 1u;
    kw_set(K_BU_SECTOR + 4u * port, n);
    int32_t sector = bu_file_sector(port, kw(f + 0x24u), (int32_t)n);
    const int32_t sub = bu_broken_sector(port, sector);
    if (sub != -1) sector = sub;
    kh_set(K_BU_ACTUAL + 2u * port, (uint32_t)sector);
    if (mc_queue(dev, MC_HANDLER_WRITE, (uint32_t)sector, kw(K_BU_BUFFER + 4u * port)) != 1u) bu_issue_failed(cpu, port);
}

/** @brief End an asynchronous write when its trailing card info finished (ROM 0xBFC0BA44): the file's class (its fd) gets 0x0004, the offset advances by 128, the operation ends with EvSpIOE.
 *
 * @param cpu Guest CPU the event handlers run on.
 * @param port The port. */
static void bu_write_end(CPUState* cpu, uint32_t port) {
    const uint32_t f = kw(K_BU_FILE + 4u * port);
    deliver_event(cpu, kw(f + 0x28u), EV_SPEC_IOE);
    kw_set(f + 0x10u, kw(f + 0x10u) + MC_FRAME_BYTES);
    bu_finish(cpu, port, EV_SPEC_IOE);
}

/** @brief Run the bu layer's completion callback for the last finished command (A0 0xA7, ROM 0xBFC0C220 and 0xBFC0B648).
 *
 * The port's operation selects the step (the ROM's table at 0xBFC0DC90):
 * 1 and 8 finish, 2 reads on, 3 writes on, 4 loads the directory, 6
 * becomes 3 and writes on, 7 ends a write. Operation 5, the rewrite of a
 * broken sector's replacement, is not modelled and ends with EvSpERROR.
 * @param cpu Guest CPU the event handlers run on. */
static void bu_ll_done(CPUState* cpu) {
    kw_set(K_BU_LL_DONE, 1u);
    const uint32_t port = bu_chan_port();
    const uint32_t dev = kw(K_MC_DEVICE + 4u * (kw(K_MC_LAST_PORT) & 1u));
    switch (kw(K_BU_OP + 4u * port)) {
    case 0u:          return;
    case BU_OP_INFO:
    case 8u:          bu_finish(cpu, port, EV_SPEC_IOE); return;
    case BU_OP_READ:  bu_read_step(cpu, port, dev); return;
    case 6u:          kw_set(K_BU_OP + 4u * port, BU_OP_WRITE); /* fall through */
    case BU_OP_WRITE: bu_write_step(cpu, port, dev); return;
    case BU_OP_WRITE_INFO: bu_write_end(cpu, port); return;
    case BU_OP_TOC:   bu_toc_step(cpu, port, dev); return;
    default:
        OPL_WARN_F("bios", "bu operation %u on port %u is not modelled; ended with EvSpERROR", kw(K_BU_OP + 4u * port), port);
        bu_issue_failed(cpu, port);
        return;
    }
}

/** @brief A0 0xAB cardInfo(chan): bu operation 1 over a card info command (ROM 0xBFC0C0DC).
 *
 * @param dev The device id.
 * @return 1 when the command was recorded, else 0. */
static uint32_t bu_card_info(uint32_t dev) {
    const uint32_t port = (uint32_t)mc_port(dev);
    kw_set(K_BU_OP + 4u * port, BU_OP_INFO);
    if (mc_queue(dev, MC_HANDLER_INFO, 0u, 0u)) return 1u;
    kw_set(K_BU_OP + 4u * port, 0u);
    return 0u;
}

/** @brief A0 0xAC buReadTOC(chan): bu operation 4, reading sector 0 into the port's sector buffer (ROM 0xBFC0C140).
 *
 * @param dev The device id.
 * @return 1 when the read was recorded, else 0. */
static uint32_t bu_read_toc(uint32_t dev) {
    const uint32_t port = (uint32_t)mc_port(dev);
    kw_set(K_BU_OP + 4u * port, BU_OP_TOC);
    if (!mc_queue(dev, MC_HANDLER_READ, 0u, K_BU_SECBUF + port * MC_FRAME_BYTES)) {
        kw_set(K_BU_OP + 4u * port, 0u);
        return 0u;
    }
    kw_set(K_BU_STAGE + 4u * port, 1u);
    return 1u;
}

/** @brief Clear the low-level callback flags and undeliver the bu class's EvMdNOINTR specs (ROM 0xBFC0884C).
 */
static void bu_ll_reset(void) {
    for (uint32_t i = 0; i < 5u; i++) kw_set(K_BU_LL_DONE + 4u * i, 0u);
    static const uint16_t k_specs[4] = { EV_SPEC_IOE, EV_SPEC_ERROR, EV_SPEC_NEW, EV_SPEC_TIMOUT };
    for (int i = 0; i < 4; i++) undeliver_event(EV_CLASS_CARD_BU, k_specs[i]);
}

/* --- Waits inside a call ----------------------------------------------------
 * _bu_init, a synchronous open, O_CREAT's directory write-back and a
 * synchronous read or write spin in the kernel (mcWaitForStatus, ROM
 * 0xBFC09144; mcWaitForStatusAndReturnIndex, 0xBFC0895C) until the card
 * driver, run from the VBLANK interrupt, sets one of the low-level callback
 * flags. The gate reproduces the spin by delivering VBLs from inside the
 * call through the VBL delivery function, one per VBLANK, until a flag is
 * set. A wait entered with interrupts masked is counted: its deliveries are
 * held, and on hardware it never ends. */
#define BU_WAIT_MAX 600u        /* VBLs one wait may span before it reports and fails */
static uint64_t s_incall_waits = 0, s_incall_vbls = 0, s_incall_masked = 0, s_incall_capped = 0;
static uint32_t s_incall_call_vbls = 0;  /* VBLs delivered inside the current call */

/** @brief Wait inside a kernel call for the card driver's next callback flag (mcWaitForStatusAndReturnIndex).
 *
 * Delivers one VBL at a time until a flag of 0xB9D0-0xB9E0 is 1, then
 * clears the flags and undelivers the bu specs as the kernel does
 * (bu_ll_reset). Without the delivery function, or after BU_WAIT_MAX VBLs,
 * the wait reports and returns 1 (an error).
 * @param cpu Guest CPU the VBLs are delivered on.
 * @return The flag's index: 0 done, 1 error, 2 timeout, 3 new card, 4 write error. */
static int bu_wait_index(CPUState* cpu) {
    extern void psyq_deliver_incall_vbl(CPUState* cpu) __attribute__((weak));
    s_incall_waits++;
    if (openpete_crit_masked()) s_incall_masked++;
    for (uint32_t n = 0;; n++) {
        for (uint32_t i = 0; i < 5u; i++)
            if (kw(K_BU_LL_DONE + 4u * i) == 1u) { bu_ll_reset(); return (int)i; }
        if (!psyq_deliver_incall_vbl || n >= BU_WAIT_MAX) {
            s_incall_capped++;
            OPL_WARN_F("bios", "card wait inside a kernel call ended after %u VBLs with no card callback%s",
                       n, psyq_deliver_incall_vbl ? "" : " (no VBL delivery)");
            bu_ll_reset();
            return 1;
        }
        psyq_deliver_incall_vbl(cpu);
        s_incall_vbls++;
        s_incall_call_vbls++;
    }
}

/** @brief Wait inside a kernel call for the card driver's next callback flag (mcWaitForStatus).
 *
 * @param cpu Guest CPU the VBLs are delivered on.
 * @return 1 when the command finished, 0 on any error flag. */
static int bu_wait(CPUState* cpu) {
    return bu_wait_index(cpu) == 0;
}

/** @brief Log the in-call wait census line.
 *
 * @param what The call the line names. */
static void bu_wait_census(const char* what) {
    if (s_census > 0)
        OPL_INFO_F("bios", "in-call waits after %s: %u VBLs in this call; totals waits=%llu vbls=%llu masked=%llu capped=%llu",
                   what, s_incall_call_vbls, (unsigned long long)s_incall_waits, (unsigned long long)s_incall_vbls,
                   (unsigned long long)s_incall_masked, (unsigned long long)s_incall_capped);
}

/** @brief The directory and broken-list state a failed buInit leaves: every directory entry zeroed, every broken sector -1 (ROM 0xBFC08F8C).
 *
 * @param port The port. */
static void bu_init_failure(uint32_t port) {
    void* dir = guest_ptr(K_BU_DIR + port * K_BU_DIR_PORT, 15u * BU_DIR_ENTRY);
    if (dir) memset(dir, 0, 15u * BU_DIR_ENTRY);
    for (uint32_t i = 0; i < 20u; i++) kw_set(K_BU_BROKEN + port * K_BU_BROKEN_PORT + 4u * i, 0xFFFFFFFFu);
}

/** @brief Read a directory or broken-list sector into the port's sector buffer inside the call, checking its checksum.
 *
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id.
 * @param sector The card sector.
 * @return 1 when the sector arrived with a good checksum. */
static int bu_read_checked(CPUState* cpu, uint32_t dev, uint32_t sector) {
    const uint32_t buf = K_BU_SECBUF + (uint32_t)mc_port(dev) * MC_FRAME_BYTES;
    return mc_queue(dev, MC_HANDLER_READ, sector, buf) == 1u && bu_wait(cpu) && bu_sector_ok(buf);
}

/** @brief Count each directory entry a file's block chain reaches and free every entry no file reaches (buInit's pass, ROM 0xBFC08D98).
 *
 * Each entry is first checked for a chain that meets a block in the wrong
 * state (bu_dir_check). A first block (0x51) or deleted first block
 * (0xA1) marks itself and the blocks its size covers along the chain;
 * every unmarked entry becomes free with no next block.
 * @param port The port. */
static void bu_init_settle(uint32_t port) {
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
    uint32_t used[15] = { 0 };
    for (uint32_t s = 0; s < 15u; s++) (void)bu_dir_check(port, s);
    for (uint32_t s = 0; s < 15u; s++) {
        const uint32_t e = dir + s * BU_DIR_ENTRY;
        if (kw(e) != 0x51u && kw(e) != 0xA1u) continue;
        used[s] = 1u;
        uint32_t next = kh(e + 8u);
        for (int32_t b = (int32_t)kw(e + 4u) / 8192 - 1; b > 0 && next != 0xFFFFu; b--) {
            if (next < 15u) used[next]++;
            next = kh(dir + next * BU_DIR_ENTRY + 8u);
        }
    }
    for (uint32_t s = 0; s < 15u; s++) {
        if (used[s]) continue;
        const uint32_t e = dir + s * BU_DIR_ENTRY;
        kw_set(e + 4u, 0u);
        kw_set(e, 0xA0u);
        kh_set(e + 8u, 0xFFFFu);
    }
}

/** @brief buInit on one port inside the call (ROM 0xBFC08B3C): sector 0, the sector-63 write, the 15 directory sectors and the 20 broken-list sectors, each waited for.
 *
 * _new_card precedes the sector-0 read and the sector-63 write. A sector 0
 * without "MC" fails the port (the auto-format flag is clear after
 * _bu_init); a failed issue, wait or checksum fails it too, leaving the
 * directory zeroed and the broken list -1. The sector-63 write's result is
 * not checked.
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id, 0x00 or 0x10.
 * @return 1 when the port's directory loaded, 0 on failure. */
static int bu_init_port(CPUState* cpu, uint32_t dev) {
    const uint32_t port = (uint32_t)mc_port(dev);
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
    kw_set(K_MC_SKIP_NEW, 1u);
    if (mc_queue(dev, MC_HANDLER_READ, 0u, buf) != 1u || !bu_wait(cpu) || kb(buf) != 'M' || kb(buf + 1u) != 'C') {
        if (kw(K_BU_AUTOFMT) == 1u && kb(buf) != 'M')
            OPL_WARN_F("bios", "_bu_init: card format on port %u is not modelled", port);
        bu_init_failure(port);
        return 0;
    }
    kw_set(K_MC_SKIP_NEW, 1u);
    if (mc_queue(dev, MC_HANDLER_WRITE, 0x3Fu, buf) == 1u) (void)bu_wait(cpu);
    for (uint32_t s = 0; s < 15u; s++) {
        void* e = guest_ptr(dir + s * BU_DIR_ENTRY, BU_DIR_ENTRY);
        if (e) memset(e, 0, BU_DIR_ENTRY);
        kw_set(dir + s * BU_DIR_ENTRY, 0xA0u);
        kw_set(dir + s * BU_DIR_ENTRY + 4u, 0u);
    }
    for (uint32_t s = 0; s < 15u; s++) {
        if (!bu_read_checked(cpu, dev, s + 1u)) { bu_init_failure(port); return 0; }
        void* e = guest_ptr(dir + s * BU_DIR_ENTRY, BU_DIR_ENTRY);
        const void* src = guest_ptr(buf, BU_DIR_ENTRY);
        if (e && src) memcpy(e, src, BU_DIR_ENTRY);
    }
    bu_init_settle(port);
    for (uint32_t i = 0; i < 20u; i++) {
        if (!bu_read_checked(cpu, dev, i + 16u)) { bu_init_failure(port); return 0; }
        kw_set(K_BU_BROKEN + port * K_BU_BROKEN_PORT + 4u * i, kw(buf));
    }
    return 1;
}

/** @brief A0 0x70 _bu_init (ROM 0xBFC09914): clear both ports' bu operations, the auto-format flag and the callback flags, free both directories, then buInit port 0 and port 1 inside the call.
 *
 * @param cpu Guest CPU; v0 receives 0. */
static void bios_bu_init(CPUState* cpu) {
    const uint32_t vbl0 = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    s_incall_call_vbls = 0;
    kw_set(K_BU_OP + 4u, 0u);
    kw_set(K_BU_OP, 0u);
    kw_set(K_BU_AUTOFMT, 0u);
    bu_ll_reset();
    for (uint32_t port = 0; port < 2u; port++) {
        const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
        for (uint32_t s = 0; s < 15u; s++) {
            const uint32_t e = dir + s * BU_DIR_ENTRY;
            void* p = guest_ptr(e, BU_DIR_ENTRY);
            if (p) memset(p, 0, BU_DIR_ENTRY);
            kw_set(e, 0xA0u);
            kh_set(e + 8u, 0xFFFFu);
        }
    }
    (void)bu_init_port(cpu, 0x00u);
    (void)bu_init_port(cpu, 0x10u);
    cpu->v0 = 0u;
    cw_row("I 70 %u %u", vbl0, s_incall_call_vbls);
    bu_wait_census("_bu_init");
}

/** @brief The synchronous open's card check (buDevInit, ROM 0xBFC089C0): read sector 0 inside the call.
 *
 * A failed issue leaves the directory zeroed and the broken list -1 and
 * fails; a wait ending in the new-card flag runs buInit on the port; any
 * other error fails; sector 0 must hold "MC" (with the auto-format flag
 * clear, a card without it fails with the directory zeroed).
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id.
 * @return 1 when the card answered with a formatted sector 0. */
static int bu_dev_init(CPUState* cpu, uint32_t dev) {
    const uint32_t port = (uint32_t)mc_port(dev);
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    if (mc_queue(dev, MC_HANDLER_READ, 0u, buf) != 1u) { bu_init_failure(port); return 0; }
    const int st = bu_wait_index(cpu);
    if (st == 3) return bu_init_port(cpu, dev);
    if (st != 0) return 0;
    if (kb(buf) == 'M' && kb(buf + 1u) == 'C') return 1;
    if (kw(K_BU_AUTOFMT) == 1u) OPL_WARN_F("bios", "open: card format on port %u is not modelled", port);
    bu_init_failure(port);
    return 0;
}

/** @brief One pass of the directory write-back (ROM 0xBFC09720): each admitted entry is copied over the sector buffer's first 32 bytes, checksummed and written to its directory sector, then a card info command closes the pass; every command is waited for.
 *
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id.
 * @param bitmap The entry states O_CREAT assigned.
 * @param admit The state this pass writes (0x52 first, then 0x51).
 * @return 1 on success, 0 when an issue or a wait failed. */
static int bu_write_toc_pass(CPUState* cpu, uint32_t dev, const uint32_t* bitmap, uint32_t admit) {
    const uint32_t port = (uint32_t)mc_port(dev);
    const uint32_t buf = K_BU_SECBUF + port * MC_FRAME_BYTES;
    for (uint32_t i = 0; i < 15u; i++) {
        if (bitmap[i] != admit) continue;
        void* dst = guest_ptr(buf, BU_DIR_ENTRY);
        const void* src = guest_ptr(K_BU_DIR + port * K_BU_DIR_PORT + i * BU_DIR_ENTRY, BU_DIR_ENTRY);
        if (dst && src) memmove(dst, src, BU_DIR_ENTRY);
        uint32_t x = 0;
        for (uint32_t k = 0; k < 127u; k++) x ^= kb(buf + k);
        kb_set(buf + 127u, x);
        if (mc_queue(dev, MC_HANDLER_WRITE, i + 1u, buf) != 1u || !bu_wait(cpu)) return 0;
    }
    return bu_card_info(dev) == 1u && bu_wait(cpu);
}

static int bu_name_match(const char* name, size_t max, const char* pat);

/** @brief Find a file's first directory entry by name (buNextFileInternal from index 0, ROM 0xBFC0A754).
 *
 * The state sought is 0x51 while the device status word (0x8644) is 0 and
 * 0xA1 (deleted) otherwise; an entry with an empty name never matches. A
 * hit is recorded at 0x9F8C.
 * @param port The port.
 * @param path The name pattern ('?' matches any character).
 * @return The entry index, or -1. */
static int32_t bu_find(uint32_t port, const char* path) {
    const uint32_t want = kw(K_DEV_STATUS) ? 0xA1u : BU_DIR_FIRST;
    const uint8_t* ram = openpete_psx_get_ram();
    for (uint32_t i = 0; i < 15u; i++) {
        const uint32_t e = K_BU_DIR + port * K_BU_DIR_PORT + i * BU_DIR_ENTRY;
        if (kw(e) != want) continue;
        const char* name = (const char*)ram + e + 0xA;
        if (!name[0] || !bu_name_match(name, 22, path)) continue;
        kw_set(K_BU_FOUND, i);
        return (int32_t)i;
    }
    return -1;
}

/** @brief O_CREAT on the bu device inside the call (dev_bu_open's create arm, ROM 0xBFC09A14).
 *
 * The device status word clears; an existing first block of that name
 * fails with EEXIST; a file of more blocks than are free fails with
 * ENOSPC (the length, blocks x 8192, is stored first). Free entries in
 * index order become the file: the first state 0x51 with the length and
 * name (20 bytes), the rest 0x52 linked from the previous, the last with no
 * next block and, past one block, state 0x53. The directory is written
 * back (entries 0x52, then 0x51) and the call returns the first index. A
 * failed write-back fails with EBUSY; the ROM's cleanup of the new chain
 * does not terminate on hardware, so the model frees the first entry and
 * reports.
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param f The file control block.
 * @param path The file name.
 * @param mode The open mode; its high half is the block count.
 * @return The first directory index, or -1 with the block's errno set. */
static int32_t bu_create(CPUState* cpu, uint32_t f, const char* path, uint32_t mode) {
    const uint32_t dev = kw(f + FCB_DEVICE_ID), port = (uint32_t)mc_port(dev);
    const uint32_t dir = K_BU_DIR + port * K_BU_DIR_PORT;
    kw_set(K_DEV_STATUS, 0u);
    if (bu_find(port, path) != -1) { kw_set(f + FCB_ERRNO, 17u); return -1; }
    uint32_t bitmap[15] = { 0 }, free_blocks = 0;
    for (uint32_t i = 0; i < 15u; i++) if ((kw(dir + i * BU_DIR_ENTRY) & 0xF0u) == 0xA0u) free_blocks++;
    kw_set(f + FCB_LENGTH, ((mode >> 16) & 0xFFFFu) << 13);
    if ((free_blocks << 13) < kw(f + FCB_LENGTH)) { kw_set(f + FCB_ERRNO, 28u); return -1; }
    const uint32_t len = kw(f + FCB_LENGTH);
    const uint32_t blocks = (len >> 13) + ((len & 0x1FFFu) ? 1u : 0u);
    uint32_t count = 0, prev = 0, first = 0;
    for (uint32_t i = 0; i < 15u; i++) {
        const uint32_t e = dir + i * BU_DIR_ENTRY;
        if ((kw(e) & 0xF0u) != 0xA0u) continue;
        if (count == 0u) {
            kw_set(e, BU_DIR_FIRST);
            kw_set(e + 4u, len);
            char* name = (char*)openpete_psx_get_ram() + e + 0xA;
#pragma GCC diagnostic push
            // warning-ratchet: the entry's name field is 20 bytes; strncpy zero-pads a shorter name and cuts a longer one there
#pragma GCC diagnostic ignored "-Wstringop-truncation"
            strncpy(name, path, 20);
#pragma GCC diagnostic pop
            bitmap[i] = BU_DIR_FIRST;
            first = i;
        } else {
            kh_set(dir + prev * BU_DIR_ENTRY + 8u, i);
            kw_set(e, 0x52u);
            if (bitmap[i] != BU_DIR_FIRST) bitmap[i] = 0x52u;
        }
        prev = i;
        if (++count < blocks) continue;
        kh_set(e + 8u, 0xFFFFu);
        if (count >= 2u) kw_set(e, 0x53u);
        if (bu_write_toc_pass(cpu, dev, bitmap, 0x52u) && bu_write_toc_pass(cpu, dev, bitmap, BU_DIR_FIRST))
            return (int32_t)first;
        OPL_WARN_F("bios", "O_CREAT \"%s\": directory write-back failed; the kernel's cleanup does not end on hardware", path);
        bu_dir_free(dir + first * BU_DIR_ENTRY);
        kw_set(f + FCB_ERRNO, PSX_EBUSY);
        return -1;
    }
    return (int32_t)first;
}

/** @brief Write a run of sectors inside the call, then a card info command (buWriteBuffer, ROM 0xBFC092EC).
 *
 * Each sector goes to its broken-list replacement when it has one; each
 * command is waited for. A failed issue returns 0; a failed wait stores its
 * status index at 0x9F80 and returns -1. The write-error status, which the
 * ROM answers by moving the sector to a spare, is not modelled and fails
 * with index 1.
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id.
 * @param sector The first card sector.
 * @param count Sectors to write.
 * @param buf Guest address of the data.
 * @return Bytes written, 0 or -1. */
static int32_t bu_write_buffer(CPUState* cpu, uint32_t dev, int32_t sector, int32_t count, uint32_t buf) {
    const uint32_t port = (uint32_t)mc_port(dev);
    int32_t ret = 0;
    for (int32_t i = 0; i < count; i++, sector++, buf += MC_FRAME_BYTES, ret += (int32_t)MC_FRAME_BYTES) {
        const int32_t sub = bu_broken_sector(port, sector);
        if (mc_queue(dev, MC_HANDLER_WRITE, (uint32_t)(sub != -1 ? sub : sector), buf) != 1u) return 0;
        const int st = bu_wait_index(cpu);
        if (st == 4) OPL_WARN_F("bios", "write error on port %u sector %d: the spare-sector rewrite is not modelled", port, sector);
        if (st != 0) { kw_set(K_BU_OP_ERROR + 4u * port, st == 4 ? 1u : (uint32_t)st); return -1; }
    }
    if (bu_card_info(dev) != 1u) return 0;
    const int st = bu_wait_index(cpu);
    if (st != 0) { kw_set(K_BU_OP_ERROR + 4u * port, st == 4 ? 1u : (uint32_t)st); return -1; }
    return ret;
}

/** @brief Read a run of sectors inside the call (buReadBuffer, ROM 0xBFC091CC).
 *
 * Each sector comes from its broken-list replacement when it has one; each
 * command is waited for. A failed issue returns 0; a failed wait stores its
 * status index at 0x9F80 and returns the bytes read before it.
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param dev The device id.
 * @param sector The first card sector.
 * @param count Sectors to read.
 * @param buf Guest address of the destination.
 * @return Bytes read, or 0. */
static int32_t bu_read_buffer(CPUState* cpu, uint32_t dev, int32_t sector, int32_t count, uint32_t buf) {
    const uint32_t port = (uint32_t)mc_port(dev);
    int32_t ret = 0;
    for (int32_t i = 0; i < count; i++, sector++, buf += MC_FRAME_BYTES, ret += (int32_t)MC_FRAME_BYTES) {
        const int32_t sub = bu_broken_sector(port, sector);
        if (mc_queue(dev, MC_HANDLER_READ, (uint32_t)(sub != -1 ? sub : sector), buf) != 1u) return 0;
        const int st = bu_wait_index(cpu);
        if (st != 0) { kw_set(K_BU_OP_ERROR + 4u * port, (uint32_t)st); return ret; }
    }
    return ret;
}

/** @brief A synchronous bu read or write inside the call (dev_bu_read and dev_bu_write, their blocking arms).
 *
 * The file's sectors from the offset are moved in runs that stop at each
 * 64-sector block, each run through the block chain; a short run ends the
 * transfer. The offset advances by the bytes moved, errno and the port's
 * operation clear, and the call returns the size, or minus the failing
 * status index at 0x9F80 when fewer bytes moved.
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param f The file control block.
 * @param buf Guest address of the data.
 * @param n Bytes requested.
 * @param write 1 for a write.
 * @return The result the call returns. */
static int32_t bu_transfer_sync(CPUState* cpu, uint32_t f, uint32_t buf, int32_t n, int write) {
    const uint32_t dev = kw(f + FCB_DEVICE_ID), port = (uint32_t)mc_port(dev);
    s_incall_call_vbls = 0;
    int32_t start = (int32_t)(kw(f + FCB_OFFSET) >> 7), left = n / 128, moved = 0;
    while (left > 0) {
        int32_t lim = start & 0x3F;
        if (start < 0 && lim != 0) lim -= 64;
        const int32_t chunk = left < 64 - lim ? left : 64 - lim;
        const int32_t abs = bu_file_sector(port, kw(f + FCB_LBA), start);
        const int32_t r = write ? bu_write_buffer(cpu, dev, abs, chunk, buf) : bu_read_buffer(cpu, dev, abs, chunk, buf);
        left -= chunk;
        if (r != chunk * (int32_t)MC_FRAME_BYTES) break;
        start += chunk;
        buf += (uint32_t)r;
        moved += chunk;
    }
    kw_set(f + FCB_OFFSET, kw(f + FCB_OFFSET) + (uint32_t)moved * MC_FRAME_BYTES);
    kw_set(f + FCB_ERRNO, 0u);
    kw_set(K_BU_OP + 4u * port, 0u);
    bu_wait_census(write ? "write" : "read");
    return n == moved * (int32_t)MC_FRAME_BYTES ? n : -(int32_t)kw(K_BU_OP_ERROR + 4u * port);
}

/** @brief Report an accepted bu write of whole sectors to the per-mod save records: the file, the bytes and the span.
 *
 * @param port The port.
 * @param f The file control block.
 * @param buf Guest address of the data.
 * @param off The write's offset in the file.
 * @param count Sectors written; 0 covers nothing and reports nothing. */
static void bu_write_capture_issue(uint32_t port, uint32_t f, uint32_t buf, uint32_t off, int32_t count) {
    if (count <= 0) return;
    const uint32_t bytes = (uint32_t)count * MC_FRAME_BYTES;
    const uint8_t* src = (const uint8_t*)guest_ptr(buf, bytes);
    openpete_modsave_bu_write_issue((int)port, bu_fcb_name(port, f), src, (int)off, (int)bytes);
}

/** @brief The bu device's write (ROM dev_bu_write, 0xBFC0A080).
 *
 * The block's errno starts as EBUSY; a port whose bu operation is in flight
 * refuses; the callback flags clear; an offset off a 128-byte boundary or
 * at or past the file's length fails with EINVAL. With O_NOWAIT the port
 * takes bu operation 3 with the file, buffer, the offset's sector index
 * and n / 128 sectors; a write of no whole sector finishes at once with
 * EvSpIOE; otherwise the first sector, through the block chain and the
 * broken-sector list, is recorded at 0x9F88 and written, and each
 * completion chains the next (bu_write_step). Without O_NOWAIT the
 * transfer runs inside the call.
 * @param cpu Guest CPU the event handlers run on.
 * @param f The file control block.
 * @param buf Guest address of the data.
 * @param n Bytes requested.
 * @return 0 when an asynchronous write started or finished, a synchronous write's result, or -1 on failure. */
static int32_t bu_write(CPUState* cpu, uint32_t f, uint32_t buf, uint32_t n) {
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    const uint32_t dev = kw(f + FCB_DEVICE_ID), port = (uint32_t)mc_port(dev);
    if (kw(K_BU_OP + 4u * port)) return -1;
    bu_ll_reset();
    const uint32_t off = kw(f + FCB_OFFSET);
    if ((off & 0x7Fu) || off >= kw(f + FCB_LENGTH)) { kw_set(f + FCB_ERRNO, PSX_EINVAL); return -1; }
    const int32_t count = (int32_t)n / 128;
    if (!(kw(f + FCB_FLAGS) & PSXF_ASYNC)) {
        bu_write_capture_issue(port, f, buf, off, count);
        const int32_t r = bu_transfer_sync(cpu, f, buf, (int32_t)n, 1);
        if (count > 0) openpete_modsave_bu_write_done((int)port, bu_fcb_name(port, f), r == (int32_t)n);
        return r;
    }
    kw_set(K_BU_OP + 4u * port, BU_OP_WRITE);
    kw_set(K_BU_SECTOR + 4u * port, off >> 7);
    kw_set(K_BU_REMAIN + 4u * port, (uint32_t)count);
    kw_set(K_BU_BUFFER + 4u * port, buf);
    kw_set(K_BU_FILE + 4u * port, f);
    bu_ll_reset();
    if (count == 0) {
        kw_set(f + FCB_ERRNO, 0u);
        kw_set(K_BU_LL_DONE, 1u);
        bu_finish(cpu, port, EV_SPEC_IOE);
        return 0;
    }
    int32_t sector = bu_file_sector(port, kw(f + FCB_LBA), (int32_t)(off >> 7));
    const int32_t sub = bu_broken_sector(port, sector);
    if (sub != -1) sector = sub;
    kh_set(K_BU_ACTUAL + 2u * port, (uint32_t)sector);
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    if (mc_queue(dev, MC_HANDLER_WRITE, (uint32_t)sector, buf) != 1u) return -1;
    kw_set(f + FCB_ERRNO, 0u);
    bu_write_capture_issue(port, f, buf, off, count);
    return 0;
}

void openpete_psx_bios_card_state_load_legacy(const uint32_t w[PSX_BIOS_CARD_STATE_WORDS]) {
    static const uint32_t k_handler[4] = { 0u, MC_HANDLER_INFO, MC_HANDLER_READ, MC_HANDLER_WRITE };
    kw_set(K_MC_STARTED, w[0] & 1u);
    kb_set(K_MC_FLIP, (w[0] >> 1) & 1u);
    kb_set(K_MC_ACTIVE, (w[0] >> 2) & 1u);
    s_xf.live = 0;
    kw_set(K_MC_LAST_PORT, (w[0] >> 30) & 1u);
    for (uint32_t p = 0; p < 2u; p++) {
        const uint32_t op = (w[0] >> (26u + 2u * p)) & 3u, flags = (w[0] >> (8u + 8u * p)) & 0xFFu;
        kb_set(K_MC_FLAGS + p, op == 1u && flags == 0u ? MC_STATUS_INFO : flags);
        if (op) kw_set(K_MC_HANDLER + 4u * p, k_handler[op]);
        kw_set(K_BU_OP + 4u * p, (w[0] >> (24u + p)) & 1u ? BU_OP_INFO : 0u);
        kw_set(K_MC_SECTOR + 4u * p, w[1u + 3u * p]);
        kw_set(K_MC_BUFFER + 4u * p, w[2u + 3u * p]);
        kw_set(K_MC_DEVICE + 4u * p, w[3u + 3u * p]);
    }
}


/** @brief Return the file control block of an fd, or 0 when the fd is outside 0-15.
 *
 * @param fd The fd as the game passed it.
 * @return The block's RAM address, or 0. */
static uint32_t fcb_of(uint32_t fd) {
    return (int32_t)fd >= 0 && fd < K_FCB_COUNT ? K_FCB + fd * K_FCB_SIZE : 0u;
}

/** @brief Return the open file control block of an fd, setting errno to EBADF when there is none.
 *
 * @param fd The fd as the game passed it.
 * @return The block's RAM address, or 0 when the fd names no open file. */
static uint32_t fcb_open(uint32_t fd) {
    uint32_t f = fcb_of(fd);
    if (!f || kw(f + FCB_FLAGS) == 0u) { kw_set(K_ERRNO, PSX_EBADF); return 0u; }
    return f;
}

/** @brief Test a bu directory name against an open pattern whose question marks match any character.
 *
 * OpenBIOS card/device.c patternMatch: every name character must match the
 * pattern's, and the pattern may run on only with a '?'.
 * @param name The entry's name, NUL-terminated within @p max bytes.
 * @param max Bytes of @p name.
 * @param pat The pattern.
 * @return 1 on a match. */
static int bu_name_match(const char* name, size_t max, const char* pat) {
    for (size_t i = 0; i < max && name[i]; i++, pat++)
        if (*pat != '?' && *pat != name[i]) return 0;
    return !*pat || *pat == '?';
}

/** @brief Run the bu device's open on a file control block (ROM dev_bu_open, 0xBFC0996C).
 *
 * The block's errno starts as EBUSY, the value a refused open leaves: a port
 * whose bu operation is in flight refuses. The callback flags clear. An open
 * without O_NOWAIT first checks the card inside the call (bu_dev_init) and
 * fails with EBUSY when it does not answer formatted. O_CREAT creates the
 * file (bu_create); otherwise the device status word clears and the cached
 * directory is searched for the file's first block, a miss leaving ENOENT.
 * Success sets the first index, offset 0, errno 0 and the length from the
 * entry.
 * @param cpu Guest CPU the VBLs of a synchronous open are delivered on.
 * @param f The file control block.
 * @param path The file name after "buXX:".
 * @param mode The open mode.
 * @return 0 on success, 1 on failure (the block's errno says why). */
static int bu_open(CPUState* cpu, uint32_t f, const char* path, uint32_t mode) {
    const uint32_t dev = kw(f + FCB_DEVICE_ID);
    int port = mc_port(dev);
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    if (kw(K_BU_OP + 4u * (uint32_t)port)) return 1;
    bu_ll_reset();
    if (!(mode & PSXF_ASYNC)) {
        s_incall_call_vbls = 0;
        const uint32_t vbl0 = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
        const int ok = bu_dev_init(cpu, dev);
        cw_row("I 32 %u %u", vbl0, s_incall_call_vbls);
        bu_wait_census("open's card check");
        if (!ok) return 1;
    }
    int32_t first = -1;
    if (mode & PSXF_CREAT) {
        s_incall_call_vbls = 0;
        const uint32_t vbl0 = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
        first = bu_create(cpu, f, path, mode);
        cw_row("I 32 %u %u", vbl0, s_incall_call_vbls);
        bu_wait_census("O_CREAT");
        if (first < 0) return 1;
    } else {
        kw_set(K_DEV_STATUS, 0u);
        first = bu_find((uint32_t)port, path);
        if (first < 0) { kw_set(f + FCB_ERRNO, PSX_ENOENT); return 1; }
    }
    const uint32_t e = K_BU_DIR + (uint32_t)port * K_BU_DIR_PORT + (uint32_t)first * BU_DIR_ENTRY;
    kw_set(f + FCB_LBA, (uint32_t)first);
    kw_set(f + FCB_OFFSET, 0u);
    kw_set(f + FCB_ERRNO, 0u);
    kw_set(f + FCB_LENGTH, kw(e + 4));
    return 0;
}

/** @brief B(32h) open(path, mode) over the file table in guest RAM.
 *
 * Takes the first free block (EMFILE when none), splits "name[digits]:file"
 * into the device and its id (the digits read as hex), and runs the
 * device's open with the block's mode, id, device and device flags set.
 * Only the bu device is served; another device name fails with ENODEV.
 * @param cpu Guest CPU; a0 the path, a1 the mode; v0 receives the fd or -1. */
static void bios_open(CPUState* cpu) {
    uint32_t f = 0u;
    for (uint32_t fd = 0; fd < K_FCB_COUNT && !f; fd++)
        if (kw(K_FCB + fd * K_FCB_SIZE + FCB_FLAGS) == 0u) f = K_FCB + fd * K_FCB_SIZE;
    if (!f) { kw_set(K_ERRNO, PSX_EMFILE); cpu->v0 = 0xFFFFFFFFu; return; }
    char path[64] = { 0 };
    for (uint32_t i = 0; i + 1 < sizeof(path); i++) {
        const char* c = (const char*)guest_ptr(cpu->a0 + i, 1);
        if (!c || !*c) break;
        path[i] = *c;
    }
    const char* p = path;
    while (*p == ' ') p++;
    char dev[32] = { 0 };
    size_t n = 0;
    for (; *p && *p != ':' && n + 1 < sizeof(dev); p++) dev[n++] = *p;
    uint32_t id = 0u;
    if (*p == ':') {
        p++;
        char* d = dev;
        while (*d && (*d < '0' || *d > '9')) d++;
        for (char* q = d; *q; q++) id = id * 0x10u + (uint32_t)((*q >= '0' && *q <= '9' ? *q : '0') - '0');
        *d = 0;
    }
    if (strcmp(dev, "bu") != 0) {
        OPL_WARN_F("bios", "open(\"%s\"): device \"%s\" is not modelled", path, dev);
        kw_set(K_ERRNO, PSX_ENODEV);
        kw_set(f + FCB_FLAGS, 0u);
        cpu->v0 = 0xFFFFFFFFu;
        return;
    }
    kw_set(f + FCB_FLAGS, cpu->a1);
    kw_set(f + FCB_DEVICE_ID, id);
    kw_set(f + FCB_DCB, K_DCB_BU);
    kw_set(f + FCB_DEV_FLAGS, kw(K_DCB_BU + DCB_FLAGS));
    if (bu_open(cpu, f, p, cpu->a1)) {
        kw_set(K_ERRNO, kw(f + FCB_ERRNO));
        kw_set(f + FCB_FLAGS, 0u);
        cpu->v0 = 0xFFFFFFFFu;
        return;
    }
    uint32_t fd = (f - K_FCB) / K_FCB_SIZE;
    kw_set(f + FCB_OFFSET, 0u);
    kw_set(f + FCB_FD, fd);
    cpu->v0 = fd;
}

/** @brief B(33h) lseek(fd, offset, whence): set (0), add (1) or keep (2) the offset; no I/O.
 *
 * Another whence sets errno and the block's errno to EINVAL and returns -1.
 * @param cpu Guest CPU; a0..a2 the arguments; v0 receives the offset or -1. */
static void bios_lseek(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    cpu->v0 = 0xFFFFFFFFu;
    if (!f) return;
    if (cpu->a2 == 0u) kw_set(f + FCB_OFFSET, cpu->a1);
    else if (cpu->a2 == 1u) kw_set(f + FCB_OFFSET, kw(f + FCB_OFFSET) + cpu->a1);
    else if (cpu->a2 != 2u) {
        kw_set(K_ERRNO, PSX_EINVAL);
        kw_set(f + FCB_ERRNO, PSX_EINVAL);
        return;
    }
    cpu->v0 = kw(f + FCB_OFFSET);
}

/** @brief The bu device's asynchronous read (ROM dev_bu_read, 0xBFC09D3C, its O_NOWAIT arm).
 *
 * The block's errno starts as EBUSY; a port whose bu operation is in flight
 * refuses. An offset off a 128-byte boundary or at or past the file's
 * length fails with EINVAL. Otherwise the port takes bu operation 2 with
 * the file, buffer, the offset's sector index and n / 128 sectors, and the
 * first sector's read is recorded; a read of no whole sector finishes at
 * once with EvSpIOE on the bu class. Each completion chains the next
 * sector (bu_read_step).
 * @param cpu Guest CPU the event handlers run on.
 * @param f The file control block.
 * @param buf Guest address of the buffer.
 * @param n Bytes requested.
 * @return 0 when the read started or finished, -1 on failure (the block's errno says why). */
static int32_t bu_read_async(CPUState* cpu, uint32_t f, uint32_t buf, uint32_t n) {
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    const uint32_t dev = kw(f + FCB_DEVICE_ID), port = (uint32_t)mc_port(dev);
    if (kw(K_BU_OP + 4u * port)) return -1;
    bu_ll_reset();
    const uint32_t off = kw(f + FCB_OFFSET);
    if ((off & 0x7Fu) || off >= kw(f + FCB_LENGTH)) { kw_set(f + FCB_ERRNO, PSX_EINVAL); return -1; }
    const int32_t count = (int32_t)n / 128;
    kw_set(K_BU_OP + 4u * port, BU_OP_READ);
    kw_set(K_BU_SECTOR + 4u * port, off >> 7);
    kw_set(K_BU_REMAIN + 4u * port, (uint32_t)count);
    kw_set(K_BU_BUFFER + 4u * port, buf);
    kw_set(K_BU_FILE + 4u * port, f);
    if (port <= 1u) {
        s_cw_read_buf[port] = buf;
        s_cw_read_bytes[port] = (uint32_t)count * 128u;
    }
    bu_ll_reset();
    if (count == 0) {
        kw_set(f + FCB_ERRNO, 0u);
        kw_set(K_BU_LL_DONE, 1u);
        bu_finish(cpu, port, EV_SPEC_IOE);
        return 0;
    }
    int32_t sector = bu_file_sector(port, kw(f + FCB_LBA), (int32_t)(off >> 7));
    const int32_t sub = bu_broken_sector(port, sector);
    if (sub != -1) sector = sub;
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    if (mc_queue(dev, MC_HANDLER_READ, (uint32_t)sector, buf) != 1u) return -1;
    kw_set(f + FCB_ERRNO, 0u);
    return 0;
}

/** @brief The bu device's blocking read (ROM dev_bu_read, 0xBFC09D3C, without O_NOWAIT).
 *
 * The block's errno starts as EBUSY; a port whose bu operation is in flight
 * refuses; the callback flags clear; an offset off a 128-byte boundary or
 * at or past the file's length fails with EINVAL; otherwise the transfer
 * runs inside the call (bu_transfer_sync).
 * @param cpu Guest CPU the VBLs are delivered on.
 * @param f The file control block.
 * @param buf Guest address of the buffer.
 * @param n Bytes requested.
 * @return The bytes read, or a negative result. */
static int32_t bu_read_sync(CPUState* cpu, uint32_t f, uint32_t buf, uint32_t n) {
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    if (kw(K_BU_OP + 4u * (uint32_t)mc_port(kw(f + FCB_DEVICE_ID)))) return -1;
    bu_ll_reset();
    const uint32_t off = kw(f + FCB_OFFSET);
    if ((off & 0x7Fu) || off >= kw(f + FCB_LENGTH)) { kw_set(f + FCB_ERRNO, PSX_EINVAL); return -1; }
    return bu_transfer_sync(cpu, f, buf, (int32_t)n, 0);
}

/** @brief B(34h) read(fd, buf, n) on a bu file.
 *
 * The kernel's read (RAM 0x2B28) hands a file-system device's read to the
 * device and, on a negative result, copies the block's errno to errno. A
 * read of another device is not reached by the games and is not modelled.
 * @param cpu Guest CPU; a0..a2 the arguments; v0 receives the result.
 * @return 1 when handled, 0 for a read this gate does not model. */
static int bios_read(CPUState* cpu) {
    const uint32_t f = fcb_open(cpu->a0);
    if (!f) { cpu->v0 = 0xFFFFFFFFu; return 1; }
    if (kw(f + FCB_DCB) != K_DCB_BU) {
        OPL_WARN_F("bios", "read on fd %u (mode 0x%X) is not modelled", cpu->a0, kw(f + FCB_FLAGS));
        return 0;
    }
    const int32_t r = (kw(f + FCB_FLAGS) & PSXF_ASYNC) ? bu_read_async(cpu, f, cpu->a1, cpu->a2)
                                                       : bu_read_sync(cpu, f, cpu->a1, cpu->a2);
    if (r < 0) kw_set(K_ERRNO, kw(f + FCB_ERRNO));
    cpu->v0 = (uint32_t)r;
    return 1;
}

/** @brief B(35h) write(fd, buf, n) on the bu device or a character device (the tty fds 0 and 1).
 *
 * The kernel's write (RAM 0x2C94) hands a file-system device's write to the
 * device (bu_write) and, on a negative result, copies the block's errno to
 * errno. For a character device the block records the buffer and count,
 * and the tty device's action (the ROM's dummy console, 0xBFC06FDC)
 * returns 0, so the offset and errno stay and the call returns 0. Another
 * file-system device is not modelled.
 * @param cpu Guest CPU; a0..a2 the arguments; v0 receives the result.
 * @return 1 when handled, 0 for a file-system device other than bu. */
static int bios_write(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    if (!f) { cpu->v0 = 0xFFFFFFFFu; return 1; }
    if (kw(f + FCB_DEV_FLAGS) & DT_FS) {
        if (kw(f + FCB_DCB) != K_DCB_BU) return 0;
        const int32_t r = bu_write(cpu, f, cpu->a1, cpu->a2);
        if (r < 0) kw_set(K_ERRNO, kw(f + FCB_ERRNO));
        cpu->v0 = (uint32_t)r;
        return 1;
    }
    kw_set(f + FCB_BUFFER, cpu->a1);
    kw_set(f + FCB_COUNT, cpu->a2);
    if (kw(kw(f + FCB_DCB) + DCB_FLAGS) & DT_BLOCK) {
        OPL_WARN_F("bios", "write to a block device on fd %u is not modelled", cpu->a0);
    }
    cpu->v0 = 0u;
    return 1;
}

/** @brief B(36h) close(fd): run the device's close, free the block, return the fd.
 *
 * The bu device (ROM dev_bu_close, 0xBFC09CDC) refuses while its port has a
 * bu operation in flight (the block's errno is then the caller's errno and
 * the call returns -1), else clears the callback flags; the tty device's
 * close succeeds. The block is freed either way.
 * @param cpu Guest CPU; a0 the fd; v0 receives the fd or -1. */
static void bios_close(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    if (!f) { cpu->v0 = 0xFFFFFFFFu; return; }
    const int bu = kw(f + FCB_DCB) == K_DCB_BU;
    const int busy = bu && kw(K_BU_OP + 4u * (uint32_t)mc_port(kw(f + FCB_DEVICE_ID)));
    if (bu && !busy) bu_ll_reset();
    kw_set(f + FCB_FLAGS, 0u);
    if (busy) { kw_set(K_ERRNO, kw(f + FCB_ERRNO)); cpu->v0 = 0xFFFFFFFFu; return; }
    cpu->v0 = cpu->a0;
}

/** @brief B(55h) _get_error(fd): the file's errno, or -1 with errno EBADF when the fd names no open file.
 *
 * @param cpu Guest CPU; a0 the fd; v0 receives the result. */
static void bios_get_error(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    cpu->v0 = f ? kw(f + FCB_ERRNO) : 0xFFFFFFFFu;
}

/** @brief A(39h) InitHeap(addr, size): record the heap and clear its first word.
 *
 * The SCPH-1001 routine (ROM 0xBFC01E24) stores the start at 0x9000, the
 * size at 0x9004, the end addr + (size & ~3) + 4 at 0x9008 and 0 at
 * 0x900C, and clears the word at addr; it sets no return value. OpenBIOS
 * kernel/alloc.c user_initheap keeps a different heap layout.
 * @param cpu Guest CPU; a0 the heap start, a1 its size. */
static void bios_init_heap(CPUState* cpu) {
    kw_set(0x9000u, cpu->a0);
    void* head = guest_ptr(cpu->a0, 4);
    if (head) memset(head, 0, 4);
    kw_set(0x9008u, cpu->a0 + (cpu->a1 & ~3u) + 4u);
    kw_set(0x9004u, cpu->a1);
    kw_set(0x900Cu, 0u);
}

/* The words of file 2 the boot leaves from opening the disc's EXE through
 * cdrom: (flags 0 once closed): the uninitialised device id the ROM's path
 * split writes for a device name without digits, the offset and length
 * (the EXE's size in whole sectors) and the EXE's first sector, read from
 * the reference's RAM (Spyro 2 and 3 at the EXE entry, Spyro 1 at VBL 481,
 * before any file call). */
typedef struct { const char* game; uint32_t device_id, length, lba; } boot_file_t;
static const boot_file_t s_boot_files[] = {
    { "spyro-1", 0x6C0D45FCu, 0x66000u, 0xD273u },
    { "spyro-2", 0x0001D4C1u, 0x57800u, 0x18u },
    { "spyro-3", 0x101489A4u, 0x5D000u, 0x18u },
};

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
    const uint32_t word0 = 3u;
    memcpy(ram + 0x00u, &word0, 4);
    memcpy(ram + 0xA0u, call[0], sizeof(call[0]));
    memcpy(ram + 0xB0u, call[1], sizeof(call[1]));
    memcpy(ram + 0xC0u, call[2], sizeof(call[2]));
    const uint32_t ram_mb = 2u, boot_flags = 0xFFu;
    memcpy(ram + 0x60u, &ram_mb, 4);
    memcpy(ram + 0x68u, &boot_flags, 4);
    for (unsigned i = 0; i < g_psx_kernel_seed_count; i++) {
        const psx_kernel_seed_run_t* r = &g_psx_kernel_seed[i];
        memcpy(ram + r->addr, r->words, r->count * 4u);
    }
    const char* game = game_get_id();
    for (size_t i = 0; game && i < sizeof(s_boot_files) / sizeof(s_boot_files[0]); i++) {
        const boot_file_t* b = &s_boot_files[i];
        if (strcmp(game, b->game) != 0) continue;
        const uint32_t f2 = K_FCB + 2u * K_FCB_SIZE;
        memcpy(ram + f2 + FCB_DEVICE_ID, &b->device_id, 4);
        memcpy(ram + f2 + FCB_OFFSET, &b->length, 4);
        memcpy(ram + f2 + FCB_LENGTH, &b->length, 4);
        memcpy(ram + f2 + FCB_LBA, &b->lba, 4);
    }
    static const uint32_t slot_pair[2] = { 0x3C020000u, 0x2442641Cu };
    memcpy(ram + 0xCF0u, slot_pair, sizeof(slot_pair));
}

void openpete_psx_bios_reseed_ram(uint8_t* ram) {
    uint32_t tables;
    memcpy(&tables, ram + 0x100u, 4);
    if (tables == 0u) openpete_psx_bios_seed_ram(ram);
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

/** @brief Handle a call through the A0 table: InitHeap, FlushCache, _bu_init, cardInfo and buReadTOC.
 *
 * InitHeap records the heap in kernel RAM. FlushCache returns 0; _bu_init
 * loads both ports' directories inside the call and returns 0.
 * cardInfo (_card_info) and buReadTOC (_card_load) start bu operations 1
 * and 4 and return whether the card command was recorded.
 * @param cpu Guest CPU; a0 carries the device id and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is implemented, 0 otherwise. */
static int bios_a0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x39:      /* InitHeap(addr, size) */
        bios_init_heap(cpu);
        return 1;
    case 0x44:      /* FlushCache */
        cpu->v0 = 0u;
        return 1;
    case 0x70:      /* _bu_init */
        bios_bu_init(cpu);
        cw_row("A %02x %08x %08x", fn, cpu->a0, cpu->v0);
        return 1;
    case 0xAB:      /* _card_info(chan) */
    case 0xAC:      /* _card_load(chan) */
        cpu->v0 = fn == 0xAB ? bu_card_info(cpu->a0) : bu_read_toc(cpu->a0);
        cw_row("A %02x %08x %08x", fn, cpu->a0, cpu->v0);
        return 1;
    default:
        return 0;
    }
}

/** @brief Handle the B0 file services over the file table in guest RAM: open, lseek, read, write, close, _get_errno, _get_error.
 *
 * @param cpu Guest CPU; the argument registers are read and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is handled, 0 otherwise. */
static int bios_b0_file(CPUState* cpu, uint32_t fn) {
    int handled = 1;
    switch (fn) {
    case 0x32: bios_open(cpu); break;            /* open(path, mode) */
    case 0x33: bios_lseek(cpu); break;           /* lseek(fd, offset, whence) */
    case 0x34: handled = bios_read(cpu); break;  /* read(fd, buf, n) */
    case 0x35: handled = bios_write(cpu); break; /* write(fd, buf, n) */
    case 0x36: bios_close(cpu); break;           /* close(fd) */
    case 0x54: cpu->v0 = kw(K_ERRNO); break;     /* _get_errno */
    case 0x55: bios_get_error(cpu); break;       /* _get_error(fd) */
    default:   return 0;
    }
    if (handled && fn <= 0x36) cw_row("B %02x %08x %08x %08x %08x", fn, cpu->a0, cpu->a1, cpu->a2, cpu->v0);
    return handled;
}

/** @brief Handle a call through the B0 table: the event services, the card driver services and the file services.
 *
 * GetC0Table and GetB0Table return the kernel's table addresses.
 * @param cpu Guest CPU; the argument registers are read and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is implemented, 0 otherwise. */
static int bios_b0(CPUState* cpu, uint32_t fn) {
    if (fn == 0x56 || fn == 0x57) {     /* GetC0Table, GetB0Table */
        cpu->v0 = fn == 0x56 ? 0x674u : 0x874u;
        return 1;
    }
    return bios_b0_event(cpu, fn) || bios_b0_card(cpu, fn) || bios_b0_file(cpu, fn);
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
