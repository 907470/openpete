// games/spyro-3/features/antipiracy_scan.c - the level overlays' anti-piracy
// scan observer (antipiracy_scan.h).
//
// Every scanner is a moby update whose state lives in the moby's data block,
// the word at a0 (`lw $sN, 0x0($a0 copy)` at each scanner's head). An EXE
// plus overlay scanner (func_level_16_80079AF4 is the model) keeps its
// cursor and CRC as two words of that block: a zero cursor is set to
// main_TEXT_START + 2 + levelIndex (0x8006C58C), the EXE pass steps
// (crc & 3) + 1 bytes per CRC-16 byte up to main_TEXT_END, then the cursor
// is set to the overlay's first text address and the overlay pass walks
// 64 bytes per call to the overlay's data start, where the scan ends; a
// CRC word other than 0 there clears an egg-collected byte of D_80070300.
// A one-function scanner (func_level_16_80079654) walks one byte per call
// over an EXE function with a 32-bit-aligned CRC (poly 0x80050000), its
// cursor biased by a per-level constant, and compares
// ((crc >> 12) & 0xFFFF0) | (D_800722B8 & 0xFFF0000F) with a word of the
// overlay header once the cursor reaches its end. The rows below are read
// off the decomp's asm (asm/nonmatchings/overlays/level_NN/moby/).
//
// The verdict of an EXE plus overlay scan is taken at the store that ends
// it: the overlay pass's `sw` of the cursor that makes it equal to the
// row's end. The CRC word is final there (each byte's CRC stores precede the
// cursor store, and the loop exits on the cursor test), and it is the value
// the game's compare (cursor == end, CRC word != 0) reads next. A compare
// that fails stores 0 to the CRC word in most overlays, with no call between
// the scan's last step and that store (func_level_33_8008DF54 compare
// 0x8008E1FC..0x8008E218, store 0x8008E238; func_level_44_80081188 cursor
// store 0x80081234, compare 0x80081244..0x80081260, store 0x80081284) or in
// another moby's update before the scanner's next call
// (func_level_22_8008E278 0x8008E2D8..0x8008E318, func_level_11_8007F630
// 0x8007F9E8..0x8007FA14), so neither the scanner's next entry nor any
// function entry in between sees the CRC the compare read. A store hook on the guest word store is installed at the
// entry of a scanner call that starts within one overlay-pass call (64
// bytes) of the end and removed at the first function entry after that call
// returns (stack pointer back at the entry's value) or at the end store.
// A one-function scan compares in the call after the one that ends it
// (func_level_16_80079654 0x80079934..0x80079968, reached only when the
// cursor test at 0x800798C8 fails), so its verdict is read at that call's
// entry.

#include "antipiracy_scan.h"

#include "openpete_fiber.h"          /* openpete_speculative, openpete_vbl */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read callbacks */
#include "spyro_overlay_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S3_EXE_TEXT_START 0x8001200Cu
#define S3_EXE_TEXT_END   0x80064F9Cu
#define S3_SCAN_ADDR_MIN  0x80077000u
#define S3_SCAN_ADDR_MAX  0x8008DF54u
#define S3_SECONDARY_HI   0x800722B8u /* D_800722B8, OR-ed into the check */
#define S3_OVERLAY_STRIDE 64u          /* overlay-pass bytes per scanner call (slti a1, 0x40) */
#define S3_KSEG_MASK      0x1FFFFFFFu

typedef enum { SCAN_EXE_OVERLAY, SCAN_FUNCTION } scan_kind_t;

/* One scanner. For SCAN_EXE_OVERLAY, lo..hi is the overlay pass's range and
 * the scan ends with the cursor at hi; for SCAN_FUNCTION, lo..hi are the
 * biased cursor's start and end values and check is the overlay-header
 * word the result is compared with. */
typedef struct {
    uint8_t     level;
    scan_kind_t kind;
    uint32_t    func;
    uint16_t    cursor_off;
    uint16_t    crc_off;
    uint32_t    lo, hi;
    uint32_t    check;
} scan_row_t;

static const scan_row_t k_rows[] = {
    {11, SCAN_EXE_OVERLAY, 0x8007E800u, 0x024, 0x034, 0x800751C0u, 0x80091ED4u, 0},
    {11, SCAN_EXE_OVERLAY, 0x80080CE0u, 0x06C, 0x060, 0x800751C0u, 0x80091ED4u, 0},
    {12, SCAN_EXE_OVERLAY, 0x8007C2B8u, 0x038, 0x094, 0x80074F50u, 0x8008BD18u, 0},
    {13, SCAN_EXE_OVERLAY, 0x80080DFCu, 0x038, 0x02C, 0x8007507Cu, 0x8008C5CCu, 0},
    {14, SCAN_EXE_OVERLAY, 0x80083F9Cu, 0x030, 0x02C, 0x80075454u, 0x800996ECu, 0},
    {16, SCAN_EXE_OVERLAY, 0x80079AF4u, 0x110, 0x10C, 0x80074AB4u, 0x800866F8u, 0},
    {16, SCAN_FUNCTION,    0x80079654u, 0x014, 0x018, 0x8002A6E0u, 0x8002ACD4u, 0x800743B8u},
    {17, SCAN_EXE_OVERLAY, 0x8007774Cu, 0x0DC, 0x0D8, 0x80074CF8u, 0x80083EECu, 0},
    {17, SCAN_FUNCTION,    0x8007A770u, 0x040, 0x03C, 0x8003A40Cu, 0x8003A584u, 0x80074438u},
    {21, SCAN_EXE_OVERLAY, 0x8007FD94u, 0x01C, 0x02C, 0x80075060u, 0x8008F6A0u, 0},
    {22, SCAN_EXE_OVERLAY, 0x8008D4C8u, 0x034, 0x030, 0x8007574Cu, 0x8009B090u, 0},
    {23, SCAN_EXE_OVERLAY, 0x80080758u, 0x028, 0x024, 0x80075578u, 0x800963E4u, 0},
    {24, SCAN_EXE_OVERLAY, 0x8007EF38u, 0x058, 0x034, 0x8007572Cu, 0x80096B68u, 0},
    {26, SCAN_EXE_OVERLAY, 0x8007FC74u, 0x0F4, 0x0F0, 0x80074DC8u, 0x8008ADD8u, 0},
    {26, SCAN_FUNCTION,    0x8007C6F8u, 0x010, 0x008, 0x80026622u, 0x80026C16u, 0x800743C8u},
    {27, SCAN_EXE_OVERLAY, 0x8007797Cu, 0x078, 0x09C, 0x80074DECu, 0x80089BE0u, 0},
    {27, SCAN_FUNCTION,    0x8007A3C0u, 0x04C, 0x048, 0x8003A40Cu, 0x8003A584u, 0x80074448u},
    {31, SCAN_EXE_OVERLAY, 0x80081920u, 0x05C, 0x04C, 0x800757BCu, 0x8008FAF8u, 0},
    {32, SCAN_EXE_OVERLAY, 0x80085480u, 0x028, 0x018, 0x80075B98u, 0x8009D030u, 0},
    {33, SCAN_EXE_OVERLAY, 0x8008DF54u, 0x00C, 0x004, 0x80075D1Cu, 0x8009F264u, 0},
    {34, SCAN_EXE_OVERLAY, 0x8008A88Cu, 0x014, 0x00C, 0x80075618u, 0x8009886Cu, 0},
    {36, SCAN_EXE_OVERLAY, 0x8007B044u, 0x084, 0x080, 0x80074AD0u, 0x800867F4u, 0},
    {36, SCAN_FUNCTION,    0x80079D08u, 0x060, 0x064, 0x8002AE03u, 0x8002B3F7u, 0x80074358u},
    {37, SCAN_EXE_OVERLAY, 0x8007BD9Cu, 0x0DC, 0x0D8, 0x80074D90u, 0x8008BA38u, 0},
    {37, SCAN_FUNCTION,    0x8007EFFCu, 0x04C, 0x050, 0x8002AB38u, 0x8002AE00u, 0x80074454u},
    {41, SCAN_EXE_OVERLAY, 0x800806C8u, 0x048, 0x044, 0x80075544u, 0x80096324u, 0},
    {42, SCAN_EXE_OVERLAY, 0x8008017Cu, 0x034, 0x028, 0x80074F78u, 0x8008FEBCu, 0},
    {43, SCAN_EXE_OVERLAY, 0x80085FC4u, 0x040, 0x050, 0x8007584Cu, 0x80096464u, 0},
    {44, SCAN_EXE_OVERLAY, 0x80081188u, 0x030, 0x034, 0x800755ACu, 0x80093E5Cu, 0},
    {46, SCAN_EXE_OVERLAY, 0x8007F248u, 0x0C0, 0x0BC, 0x80074E40u, 0x80091B40u, 0},
    {46, SCAN_FUNCTION,    0x8008343Cu, 0x00C, 0x008, 0x800249A6u, 0x80024F9Au, 0x8007439Cu},
    {47, SCAN_EXE_OVERLAY, 0x8007CF38u, 0x0A0, 0x0A4, 0x80074D7Cu, 0x8008DF5Cu, 0},
    {47, SCAN_FUNCTION,    0x80079AC4u, 0x078, 0x07C, 0x8002AB38u, 0x8002AE00u, 0x80074460u},
};
#define N_ROWS (sizeof k_rows / sizeof k_rows[0])

/* One scan in progress: a scanner row and the moby data block it runs in. */
typedef struct {
    const scan_row_t* row;
    uint32_t          data;
    uint32_t          last_cursor;
    uint32_t          steps;
    int               reported;
} scan_slot_t;

/* The scanner call whose end store the hook waits for: its slot, the
 * cursor and CRC word addresses, and the stack pointer at its entry. */
static scan_slot_t* s_arm_slot;
static CPUState*    s_arm_cpu;
static uint32_t     s_arm_cursor, s_arm_crc, s_arm_sp;
static void       (*s_arm_next)(uint32_t, uint32_t); /* the hook chain below this one */

#define MAX_SLOTS 16

static uint32_t    s_magic;              /* resident overlay the rows below belong to */
static int         s_level;              /* its level number, 0 for none */
static scan_slot_t s_slots[MAX_SLOTS];
static int         s_n_slots;
static uint32_t    s_completed, s_failed, s_steps;

void spyro3_antipiracy_report_fail(const char* source, const char* detail) {
    s_failed++;
    OPL_WARN_F("antipiracy", "FAIL %s: %s", source, detail);
}

void spyro3_antipiracy_report_watch_hit(uint32_t addr, uint32_t size, const char* writer) {
    char detail[160];
    snprintf(detail, sizeof detail, "store of %u byte(s) at 0x%08X by %s", (unsigned)size,
             (unsigned)addr, writer ? writer : "unknown writer");
    spyro3_antipiracy_report_fail("watch", detail);
}

void spyro3_antipiracy_counts(uint32_t* completed, uint32_t* failed, uint32_t* steps) {
    if (completed) *completed = s_completed;
    if (failed) *failed = s_failed;
    if (steps) *steps = s_steps;
}

static void scan_end_store(uint32_t addr, uint32_t value);
static void report_completion(CPUState* cpu, scan_slot_t* s, uint32_t crc);

/** @brief Remove the end-store hook, leaving the store chain as it was when it was installed.
 *
 * A hook chain replaced since the hook went in (a store bracket that began
 * inside the call) is left alone. */
static void disarm(void) {
    if (!s_arm_slot) return;
    if (s_arm_cpu->write_word == scan_end_store) s_arm_cpu->write_word = s_arm_next;
    s_arm_slot = NULL;
}

/** @brief Install the end-store hook for one scanner call.
 *
 * @param cpu The guest CPU state at the scanner's entry.
 * @param s The scan's slot. */
static void arm(CPUState* cpu, scan_slot_t* s) {
    disarm();
    s_arm_slot   = s;
    s_arm_cpu    = cpu;
    s_arm_cursor = (s->data + s->row->cursor_off) & S3_KSEG_MASK;
    s_arm_crc    = s->data + s->row->crc_off;
    s_arm_sp     = cpu->sp;
    s_arm_next   = cpu->write_word;
    cpu->write_word = scan_end_store;
}

/** @brief Word-store hook: pass the store down the chain, then take the verdict when it is the store that ends the armed scan.
 *
 * @param addr Guest address of the store.
 * @param value Stored word. */
static void scan_end_store(uint32_t addr, uint32_t value) {
    s_arm_next(addr, value);
    if (!s_arm_slot || openpete_speculative || (addr & S3_KSEG_MASK) != s_arm_cursor ||
        value != s_arm_slot->row->hi)
        return;
    scan_slot_t* s = s_arm_slot;
    CPUState* cpu  = s_arm_cpu;
    disarm();
    s->steps++;
    s_steps++;
    s->last_cursor = value;
    report_completion(cpu, s, cpu->read_word(s_arm_crc));
}

/** @brief Log the scans of the outgoing overlay that never reached their end, then forget every slot. */
static void flush_slots(void) {
    disarm();
    for (int i = 0; i < s_n_slots; i++) {
        const scan_slot_t* s = &s_slots[i];
        if (s->reported) continue;
        OPL_INFO_F("antipiracy", "level %d scan 0x%08X moby 0x%08X incomplete: cursor 0x%08X "
                   "(end 0x%08X) steps %u",
                   s_level, (unsigned)s->row->func, (unsigned)s->data, (unsigned)s->last_cursor,
                   (unsigned)s->row->hi, (unsigned)s->steps);
    }
    s_n_slots = 0;
}

/** @brief Log the scans still running when the process exits. */
static void flush_at_exit(void) {
    flush_slots();
}

/** @brief Select the rows of the resident overlay, flushing the previous overlay's scans when it changed. */
static void sync_overlay(void) {
    uint32_t magic = spyro_get_active_overlay_magic();
    if (magic == s_magic) return;
    flush_slots();
    s_magic = magic;
    s_level = 0;
    const spyro_overlay_entry_t* e = spyro_overlay_find(magic);
    if (e && e->name && strncmp(e->name, "level_", 6) == 0) s_level = (int)strtol(e->name + 6, NULL, 10);
}

/** @brief Return the slot of a scan, creating it on first sight.
 *
 * @param row The scanner row.
 * @param data The moby data block.
 * @param cursor The cursor word at first sight.
 * @return The slot, or NULL when every slot is taken. */
static scan_slot_t* slot_for(const scan_row_t* row, uint32_t data, uint32_t cursor) {
    for (int i = 0; i < s_n_slots; i++)
        if (s_slots[i].row == row && s_slots[i].data == data) return &s_slots[i];
    if (s_n_slots >= MAX_SLOTS) return NULL;
    static int s_atexit;
    if (!s_atexit) s_atexit = atexit(flush_at_exit) == 0;
    scan_slot_t* s = &s_slots[s_n_slots++];
    s->row = row;
    s->data = data;
    s->last_cursor = cursor;
    s->steps = 0;
    s->reported = 0;
    return s;
}

/** @brief Apply the scanner's own pass test to a scan that reached its end.
 *
 * @param cpu The guest CPU state.
 * @param row The scanner row.
 * @param crc The CRC word.
 * @return 1 on pass, else 0. */
static int scan_passes(CPUState* cpu, const scan_row_t* row, uint32_t crc) {
    if (row->kind == SCAN_EXE_OVERLAY) return crc == 0u;
    uint32_t got = (((uint32_t)((int32_t)crc >> 12)) & 0xFFFF0u) |
                   (cpu->read_word(S3_SECONDARY_HI) & 0xFFF0000Fu);
    return got == cpu->read_word(row->check);
}

/** @brief Log a scan that reached its end with its verdict, once, and count it.
 *
 * @param cpu The guest CPU state.
 * @param s The scan's slot.
 * @param crc The CRC word the game's compare reads. */
static void report_completion(CPUState* cpu, scan_slot_t* s, uint32_t crc) {
    const scan_row_t* row = s->row;
    s->reported = 1;
    s_completed++;
    int pass = scan_passes(cpu, row, crc);
    char detail[200];
    snprintf(detail, sizeof detail, "level %d scan 0x%08X moby 0x%08X crc 0x%08X steps %u vbl %d",
             s_level, (unsigned)row->func, (unsigned)s->data, (unsigned)crc, (unsigned)s->steps,
             SDL_GetAtomicInt(&openpete_vbl));
    if (!pass) spyro3_antipiracy_report_fail("scan", detail);
    OPL_INFO_F("antipiracy", "level %d scan 0x%08X moby 0x%08X complete: %s crc 0x%08X steps %u "
               "vbl %d (completed %u failed %u steps %u)",
               s_level, (unsigned)row->func, (unsigned)s->data, pass ? "PASS" : "FAIL",
               (unsigned)crc, (unsigned)s->steps, SDL_GetAtomicInt(&openpete_vbl),
               (unsigned)s_completed, (unsigned)s_failed, (unsigned)s_steps);
}

/** @brief Return the resident level's scanner row at a function address.
 *
 * @param addr The entered function's guest vaddr.
 * @return The row, or NULL when no scanner of the resident level starts there. */
static const scan_row_t* row_at(uint32_t addr) {
    for (size_t i = 0; i < N_ROWS; i++)
        if (k_rows[i].level == s_level && k_rows[i].func == addr) return &k_rows[i];
    return NULL;
}

/** @brief Observe one scanner call at its entry: count a step, take a one-function scan's verdict, or arm the end store of an EXE plus overlay scan.
 *
 * @param cpu The guest CPU state at the entry (a0 is the moby).
 * @param row The scanner's row. */
static void observe_scanner(CPUState* cpu, const scan_row_t* row) {
    uint32_t data   = cpu->read_word(cpu->a0);
    uint32_t cursor = cpu->read_word(data + row->cursor_off);
    scan_slot_t* s = slot_for(row, data, cursor);
    if (!s) return;
    if (cursor != s->last_cursor) {
        s->steps++;
        s_steps++;
        if (s->reported && cursor != row->hi) s->reported = 0; /* restarted (state load) */
        s->last_cursor = cursor;
    }
    if (s->reported) return;
    if (cursor == row->hi && row->kind == SCAN_FUNCTION) {
        report_completion(cpu, s, cpu->read_word(data + row->crc_off));
    } else if (cursor == row->hi) {
        /* Ended where the end store was not watched (a state loaded past it). */
        s->reported = 1;
        OPL_WARN_F("antipiracy", "level %d scan 0x%08X moby 0x%08X complete: UNOBSERVED "
                   "(end store not seen) steps %u vbl %d",
                   s_level, (unsigned)row->func, (unsigned)data, (unsigned)s->steps,
                   SDL_GetAtomicInt(&openpete_vbl));
    } else if (row->kind == SCAN_EXE_OVERLAY && cursor >= row->lo &&
               row->hi - cursor <= S3_OVERLAY_STRIDE) {
        arm(cpu, s);
    }
}

void spyro3_antipiracy_scan_on_entry(CPUState* cpu, uint32_t addr) {
    if (openpete_speculative) return;
    if (s_arm_slot && cpu->sp >= s_arm_sp) disarm();
    if (addr < S3_SCAN_ADDR_MIN || addr > S3_SCAN_ADDR_MAX) return;
    sync_overlay();
    if (s_level == 0) return;
    const scan_row_t* row = row_at(addr);
    if (row) observe_scanner(cpu, row);
}
