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
#include "openpete_mod_api.h"       /* openpete_mod_scan_t, OP_SCAN_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

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
    int               state;     /* OP_SCAN_*, as antipiracy_scan reports it */
    uint32_t          crc;       /* the CRC word the last comparison tested */
    uint32_t          completed; /* comparisons observed */
} scan_slot_t;

/* One walk (boot, title, loading) as antipiracy_scan reports it, indexed
 * by its OP_SCAN_KEY_*. */
typedef struct {
    int      state;
    uint32_t func, cursor, end, crc, crc_addr, steps, completed;
} walk_t;
static walk_t s_walks[OP_SCAN_KEY_LOADING + 1];

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
    OP_ROLE_ASSERT(sim);
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
    s->state = cursor ? OP_SCAN_RUNNING : OP_SCAN_IDLE;
    s->crc = 0;
    s->completed = 0;
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
    s->state = pass ? OP_SCAN_PASS : OP_SCAN_FAIL;
    s->crc = crc;
    s->completed++;
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
        if (cursor != row->hi && cursor != 0u) s->state = OP_SCAN_RUNNING;
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

/* ---- the boot, title and loading walks ------------------------------- */

#define S3_CRC16_FN        0x8002AAFCu /* crc16, one CRC-16 byte step */
#define S3_LOADER_FN       0x8002CA50u /* the level loader that runs the loading walk */
#define S3_TITLE_FN        0x80074DECu /* func_title_80074DEC, title overlay */
#define S3_TITLE_TEXT_LO   0x80074DECu /* title_text_start */
#define S3_TITLE_TEXT_HI   0x8007EC9Cu /* title_text_end */
#define S3_TITLE_POST_LO   0x80075504u /* first address after the title walk's loops */
#define S3_TITLE_POST_HI   0x8007566Cu /* end of func_title_80074DEC */
#define S3_LOAD_STATE      0x8006C518u /* D_8006C518, the loader state */
#define S3_LOAD_CURSOR     0x8006C6B8u /* D_8006C6B8, biased by -0x473E while walking */
#define S3_LOAD_CRC        0x8006C6B4u /* D_8006C6B4 */
#define S3_LOAD_END        0x8007B6E4u /* loading_text_end, the cursor that passes */
#define S3_LOAD_BIAS       0x473Eu

/* One boot-walk check: the crc16 call site's return address and the range
 * its loop walks one byte per call; the check after the loop passes when
 * the low 16 bits of the last call's result are 0 (func_8002AB38
 * 0x8002AC28, 0x8002AC8C, 0x8002AD34, 0x8002AD98). The stepped title
 * passes (return addresses 0x8002ABD4, 0x8002ACE0) feed the next check and
 * are not compared themselves. */
typedef struct {
    uint32_t ra, lo, hi;
} boot_check_t;
static const boot_check_t k_boot_checks[] = {
    {0x8002AC14u, S3_EXE_TEXT_START, S3_EXE_TEXT_END},
    {0x8002AC78u, S3_TITLE_TEXT_LO, S3_TITLE_TEXT_HI},
    {0x8002AD20u, S3_EXE_TEXT_START, S3_EXE_TEXT_END},
    {0x8002AD84u, S3_TITLE_TEXT_LO, S3_TITLE_TEXT_HI},
};
#define N_BOOT_CHECKS ((uint32_t)(sizeof k_boot_checks / sizeof k_boot_checks[0]))

/** @brief Apply crc16's byte step (src/init.c crc16, func_8002AAFC).
 *
 * @param crc The running value.
 * @param byte The byte.
 * @return The new value. */
static uint32_t crc16_step(uint32_t crc, uint8_t byte) {
    uint32_t out = crc ^ ((uint32_t)byte << 8);
    for (int i = 0; i < 8; i++) out = (out & 0x8000u) ? (out << 1) ^ 0x8005u : out << 1;
    return out;
}

/** @brief Record one walk check and log it, reporting a failure.
 *
 * @param key OP_SCAN_KEY_* of the walk.
 * @param name The walk's name, for the log.
 * @param pass 1 when the game's test passed.
 * @param crc The value the test read.
 * @param last 1 when this is the walk's last check. */
static void walk_check(uint32_t key, const char* name, int pass, uint32_t crc, int last) {
    walk_t* w = &s_walks[key];
    if (key != OP_SCAN_KEY_LOADING) w->steps++; /* the loading walk counts cursor advances */
    w->completed++;
    w->crc = crc;
    if (!pass) w->state = OP_SCAN_FAIL;
    else if (last && w->state != OP_SCAN_FAIL) w->state = OP_SCAN_PASS;
    else if (w->state != OP_SCAN_FAIL) w->state = OP_SCAN_RUNNING;
    char detail[160];
    snprintf(detail, sizeof detail, "%s walk check %u crc 0x%08X vbl %d", name,
             (unsigned)w->completed, (unsigned)crc, SDL_GetAtomicInt(&openpete_vbl));
    if (!pass) spyro3_antipiracy_report_fail(name, detail);
    OPL_INFO_F("antipiracy", "%s walk check %u: %s crc 0x%08X vbl %d", name, (unsigned)w->completed,
               pass ? "PASS" : "FAIL", (unsigned)crc, SDL_GetAtomicInt(&openpete_vbl));
}

/** @brief Observe one crc16 call of the boot walk (func_8002AB38): mark the walk running at a check loop's first byte and take the check's verdict at its last byte.
 *
 * The check's value is crc16's result for the last byte, computed here
 * with crc16's own step from the running value in a1, since the game keeps
 * it only in registers.
 * @param cpu The guest CPU state at crc16's entry (a0 the byte, a1 the running value). */
static void observe_boot_crc16(CPUState* cpu) {
    for (uint32_t i = 0; i < N_BOOT_CHECKS; i++) {
        const boot_check_t* c = &k_boot_checks[i];
        if (cpu->ra != c->ra) continue;
        walk_t* w = &s_walks[OP_SCAN_KEY_BOOT];
        w->func = 0x8002AB38u;
        w->end = c->hi;
        w->cursor = cpu->a0;
        if (cpu->a0 == c->lo && w->state == OP_SCAN_IDLE) w->state = OP_SCAN_RUNNING;
        if (cpu->a0 + 1u == c->hi) {
            uint32_t crc = crc16_step(cpu->a1, cpu->read_byte(cpu->a0));
            walk_check(OP_SCAN_KEY_BOOT, "boot", (crc & 0xFFFFu) == 0u, crc & 0xFFFFu,
                       i + 1u == N_BOOT_CHECKS);
        }
        return;
    }
}

/* The title walk's two values, computed at func_title_80074DEC's entry
 * and published at the first entry after its loops (or after it
 * returns). */
static int      s_title_pending;
static uint32_t s_title_sp, s_title_crc[2];

/** @brief Compute the title walk's two check values over the bytes func_title_80074DEC's loops read.
 *
 * Check 1 (0x80075504) is a stepped pass over title text, (crc & 3) + 1
 * bytes per CRC byte, continued one byte at a time over EXE text; check 2
 * (0x80075590) is a full pass over title text from 0. Both loops keep the
 * CRC in registers and call nothing, so the values are computed here with
 * the loops' own step over the same bytes, at the function's entry: no
 * store reaches those bytes between the entry and the loops, and check 1's
 * failure store into func_8002A9D0 follows its loop.
 * @param cpu The guest CPU state. */
static void title_compute(CPUState* cpu) {
    uint32_t crc = 0;
    for (uint32_t a = S3_TITLE_TEXT_LO; a < S3_TITLE_TEXT_HI;) {
        crc = crc16_step(crc, cpu->read_byte(a));
        a += (crc & 3u) + 1u;
    }
    for (uint32_t a = S3_EXE_TEXT_START; a < S3_EXE_TEXT_END; a++)
        crc = crc16_step(crc, cpu->read_byte(a));
    s_title_crc[0] = crc & 0xFFFFu;
    crc = 0;
    for (uint32_t a = S3_TITLE_TEXT_LO; a < S3_TITLE_TEXT_HI; a++)
        crc = crc16_step(crc, cpu->read_byte(a));
    s_title_crc[1] = crc & 0xFFFFu;
}

/** @brief Observe the title walk: compute its values at func_title_80074DEC's entry, publish them once its loops have run.
 *
 * Its loops have run at the first entry called from after them (rand on a
 * failure, VSync) or at the first entry after the function returned.
 * @param cpu The guest CPU state at a function entry.
 * @param addr The entered function. */
static void observe_title(CPUState* cpu, uint32_t addr) {
    if (s_title_pending &&
        ((cpu->ra >= S3_TITLE_POST_LO && cpu->ra < S3_TITLE_POST_HI) || cpu->sp >= s_title_sp)) {
        s_title_pending = 0;
        walk_t* w = &s_walks[OP_SCAN_KEY_TITLE];
        w->state = OP_SCAN_IDLE;
        w->steps = 0;
        w->func = S3_TITLE_FN;
        w->end = S3_TITLE_TEXT_HI;
        w->cursor = S3_TITLE_TEXT_HI;
        walk_check(OP_SCAN_KEY_TITLE, "title", s_title_crc[0] == 0u, s_title_crc[0], 0);
        walk_check(OP_SCAN_KEY_TITLE, "title", s_title_crc[1] == 0u, s_title_crc[1], 1);
    }
    if (addr != S3_TITLE_FN) return;
    const spyro_overlay_entry_t* e = spyro_overlay_find(spyro_get_active_overlay_magic());
    if (!e || !e->name || strcmp(e->name, "title") != 0) return;
    title_compute(cpu);
    s_title_sp = cpu->sp;
    s_title_pending = 1;
}

/** @brief Observe the loading walk at the loader's entry (func_8002CA50): running while its cursor is set and the loader has not reached its compare.
 *
 * The walk advances in loader states 2..7 and is compared in state 8
 * (0x8002CD24); that verdict comes through spyro3_antipiracy_loading_verdict.
 * @param cpu The guest CPU state. */
static void observe_loading(CPUState* cpu) {
    uint32_t state  = cpu->read_word(S3_LOAD_STATE);
    uint32_t cursor = cpu->read_word(S3_LOAD_CURSOR);
    walk_t* w = &s_walks[OP_SCAN_KEY_LOADING];
    if (state < 2u || state > 7u || cursor == 0u) return;
    uint32_t at = cursor == S3_LOAD_END ? cursor : cursor + S3_LOAD_BIAS;
    if (w->state != OP_SCAN_RUNNING) {
        w->state = OP_SCAN_RUNNING;
        w->steps = 0;
    }
    if (at != w->cursor) w->steps++;
    w->func = S3_LOADER_FN;
    w->end = S3_LOAD_END;
    w->cursor = at;
    w->crc_addr = S3_LOAD_CRC;
}

void spyro3_antipiracy_loading_verdict(int passed, uint32_t cursor, uint32_t crc) {
    walk_t* w = &s_walks[OP_SCAN_KEY_LOADING];
    w->func = S3_LOADER_FN;
    w->end = S3_LOAD_END;
    w->cursor = cursor;
    w->crc_addr = S3_LOAD_CRC;
    w->state = OP_SCAN_IDLE;
    walk_check(OP_SCAN_KEY_LOADING, "loading", passed, crc, 1);
}

int spyro3_antipiracy_scan_query(uint32_t key, openpete_mod_scan_t* out) {
    if (key >= OP_SCAN_KEY_BOOT && key <= OP_SCAN_KEY_LOADING) {
        const walk_t* w = &s_walks[key];
        out->state = w->state;
        out->func = w->func;
        out->cursor = w->cursor;
        out->end = w->end;
        out->crc = w->crc;
        out->crc_addr = w->crc_addr;
        out->steps = w->steps;
        out->completed = w->completed;
        return 0;
    }
    for (int i = 0; i < s_n_slots; i++) {
        const scan_slot_t* s = &s_slots[i];
        if (s->data != key) continue;
        out->state = s->state;
        out->func = s->row->func;
        out->cursor = s->last_cursor;
        out->end = s->row->hi;
        out->crc = s->crc;
        out->crc_addr = s->data + s->row->crc_off;
        out->steps = s->steps;
        out->completed = s->completed;
        return 0;
    }
    return -1;
}

void spyro3_antipiracy_scan_on_entry(CPUState* cpu, uint32_t addr) {
    if (openpete_speculative) return;
    if (s_arm_slot && cpu->sp >= s_arm_sp) disarm();
    if (addr == S3_CRC16_FN) observe_boot_crc16(cpu);
    else if (addr == S3_LOADER_FN) observe_loading(cpu);
    if (s_title_pending || addr == S3_TITLE_FN) observe_title(cpu, addr);
    if (addr < S3_SCAN_ADDR_MIN || addr > S3_SCAN_ADDR_MAX) return;
    sync_overlay();
    if (s_level == 0) return;
    const scan_row_t* row = row_at(addr);
    if (row) observe_scanner(cpu, row);
}
