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
 * calls (B(32h) open, B(33h) lseek, B(35h) write, B(36h) close) on the bu
 * device.
 *
 * The kernel's file table, errno and the tables around them live in guest
 * RAM at the SCPH-1001 addresses, seeded at boot as the ROM leaves them
 * (openpete_psx_bios_seed_ram), and the file calls read and write them
 * there, so the game and the oracle see the kernel's bytes.
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
#include "game_extras.h"
#include "psx_bios_kernel_seed.h"

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
 * handler set them. A handler with no compiled body logs a warning. Its
 * gate hits are host-initiated for the oracle's clock replay.
 * @param cpu Guest CPU the handler runs on.
 * @param addr Handler address; 0 does nothing. */
static void invoke_guest(CPUState* cpu, uint32_t addr) {
    if (!addr) return;
    uint32_t a0 = cpu->a0, a1 = cpu->a1, a2 = cpu->a2, a3 = cpu->a3;
    uint32_t ra = cpu->ra, v0 = cpu->v0, v1 = cpu->v1;
    cpu->ra = 0xDEADBEEFu;
    openpete_psxtrace_gate_host_enter();
    if (!openpete_recomp_module_get()->dispatch(cpu, addr)) {
        OPL_WARN_F("bios", "event handler 0x%08X has no compiled body", addr);
    }
    openpete_psxtrace_gate_host_leave();
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
    memcpy(&v, openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 1u)), 4);
    return v;
}

/** @brief Write the guest word at a kernel RAM address.
 *
 * @param addr Guest address; its low 21 bits select the word.
 * @param v The value. */
static void kw_set(uint32_t addr, uint32_t v) {
    memcpy(openpete_psx_get_ram() + (addr & (PSX_RAM_BYTES - 1u)), &v, 4);
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

/** @brief Run the bu device's open on a file control block (OpenBIOS card/device.c dev_bu_open).
 *
 * The block's errno starts as EBUSY, the value a refused open leaves: a port
 * whose bu operation is in flight refuses. The open searches the cached
 * directory for the file's first block; the synchronous open's sector-0
 * read (buDevInit) and O_CREAT's allocation and directory write-back wait
 * on card operations inside the call, which this gate does not model, so a
 * synchronous open searches the cache as an asynchronous one does and
 * O_CREAT is refused with EBUSY. A miss leaves ENOENT; a hit sets the first
 * index, offset 0, errno 0 and the length from the entry.
 * @param f The file control block.
 * @param path The file name after "buXX:".
 * @param mode The open mode.
 * @return 0 on success, 1 on failure (the block's errno says why). */
static int bu_open(uint32_t f, const char* path, uint32_t mode) {
    int port = mc_port(kw(f + FCB_DEVICE_ID));
    kw_set(f + FCB_ERRNO, PSX_EBUSY);
    if (s_mc.bu_op[port]) return 1;
    if (mode & PSXF_CREAT) {
        OPL_WARN_F("bios", "bu open with O_CREAT (\"%s\", mode 0x%X) is not modelled; refused", path, mode);
        return 1;
    }
    const uint8_t* ram = openpete_psx_get_ram();
    for (uint32_t i = 0; i < 15u; i++) {
        uint32_t e = K_BU_DIR + (uint32_t)port * K_BU_DIR_PORT + i * BU_DIR_ENTRY;
        if (kw(e) != BU_DIR_FIRST) continue;
        const char* name = (const char*)ram + e + 0xA;
        if (!name[0] || !bu_name_match(name, 22, path)) continue;
        kw_set(f + FCB_LBA, i);
        kw_set(f + FCB_OFFSET, 0u);
        kw_set(f + FCB_ERRNO, 0u);
        kw_set(f + FCB_LENGTH, kw(e + 4));
        return 0;
    }
    kw_set(f + FCB_ERRNO, PSX_ENOENT);
    return 1;
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
    if (bu_open(f, p, cpu->a1)) {
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

/** @brief B(35h) write(fd, buf, n) on a character device, the tty fds 0 and 1.
 *
 * The block records the buffer and count, and the tty device's action (the
 * ROM's dummy console, 0xBFC06FDC) returns 0, so the offset and errno stay
 * and the call returns 0. A write to a file-system device (bu) is not
 * modelled here.
 * @param cpu Guest CPU; a0..a2 the arguments; v0 receives the result.
 * @return 1 when handled, 0 for a file-system device. */
static int bios_write(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    if (!f) { cpu->v0 = 0xFFFFFFFFu; return 1; }
    if (kw(f + FCB_DEV_FLAGS) & DT_FS) return 0;
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
 * The bu device refuses while its port has a bu operation in flight (the
 * block's errno is then the caller's errno and the call returns -1); the
 * tty device's close succeeds. The block is freed either way.
 * @param cpu Guest CPU; a0 the fd; v0 receives the fd or -1. */
static void bios_close(CPUState* cpu) {
    uint32_t f = fcb_open(cpu->a0);
    if (!f) { cpu->v0 = 0xFFFFFFFFu; return; }
    int busy = kw(f + FCB_DCB) == K_DCB_BU && s_mc.bu_op[mc_port(kw(f + FCB_DEVICE_ID))];
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

/** @brief Handle a call through the A0 table: InitHeap, FlushCache, _bu_init, _card_info and _card_load.
 *
 * InitHeap records the heap in kernel RAM. FlushCache and _bu_init return
 * 0. _card_info and _card_load queue an
 * info command through the bu layer and return whether it was queued.
 * @param cpu Guest CPU; a0 carries the device id and v0 receives the result.
 * @param fn Function number.
 * @return 1 when the function is implemented, 0 otherwise. */
static int bios_a0(CPUState* cpu, uint32_t fn) {
    switch (fn) {
    case 0x39:      /* InitHeap(addr, size) */
        bios_init_heap(cpu);
        return 1;
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
 * GetB0Table, _card_write, _card_read, _card_chan, _card_status and
 * _card_wait over the card driver state, and open, lseek, write (tty fds),
 * close, _get_errno and _get_error over the file table in guest RAM. WaitEvent
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
    case 0x32:      /* open(path, mode) */
        bios_open(cpu);
        return 1;
    case 0x33:      /* lseek(fd, offset, whence) */
        bios_lseek(cpu);
        return 1;
    case 0x35:      /* write(fd, buf, n): the tty fds; a bu write is unmodelled */
        return bios_write(cpu);
    case 0x36:      /* close(fd) */
        bios_close(cpu);
        return 1;
    case 0x54:      /* _get_errno: the kernel's errno word */
        cpu->v0 = kw(K_ERRNO);
        return 1;
    case 0x55:      /* _get_error(fd) */
        bios_get_error(cpu);
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
