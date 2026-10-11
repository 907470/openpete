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

/** @brief Log the scans of the outgoing overlay that never reached their end, then forget every slot. */
static void flush_slots(void) {
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

void spyro3_antipiracy_scan_on_entry(CPUState* cpu, uint32_t addr) {
    if (addr < S3_SCAN_ADDR_MIN || addr > S3_SCAN_ADDR_MAX || openpete_speculative) return;
    sync_overlay();
    if (s_level == 0) return;
    for (size_t i = 0; i < N_ROWS; i++) {
        const scan_row_t* row = &k_rows[i];
        if (row->level != s_level || row->func != addr) continue;
        uint32_t data   = cpu->read_word(cpu->a0);
        uint32_t cursor = cpu->read_word(data + row->cursor_off);
        uint32_t crc    = cpu->read_word(data + row->crc_off);
        scan_slot_t* s = slot_for(row, data, cursor);
        if (!s) return;
        if (cursor != s->last_cursor) {
            s->steps++;
            s_steps++;
            if (s->reported && cursor != row->hi) s->reported = 0; /* restarted (state load) */
            s->last_cursor = cursor;
        }
        if (s->reported || cursor != row->hi) return;
        s->reported = 1;
        s_completed++;
        int pass = scan_passes(cpu, row, crc);
        char detail[200];
        snprintf(detail, sizeof detail, "level %d scan 0x%08X moby 0x%08X crc 0x%08X steps %u vbl %d",
                 s_level, (unsigned)row->func, (unsigned)data, (unsigned)crc, (unsigned)s->steps,
                 SDL_GetAtomicInt(&openpete_vbl));
        if (!pass) spyro3_antipiracy_report_fail("scan", detail);
        OPL_INFO_F("antipiracy", "level %d scan 0x%08X moby 0x%08X complete: %s crc 0x%08X steps %u "
                   "vbl %d (completed %u failed %u steps %u)",
                   s_level, (unsigned)row->func, (unsigned)data, pass ? "PASS" : "FAIL",
                   (unsigned)crc, (unsigned)s->steps, SDL_GetAtomicInt(&openpete_vbl),
                   (unsigned)s_completed, (unsigned)s_failed, (unsigned)s_steps);
        return;
    }
}
