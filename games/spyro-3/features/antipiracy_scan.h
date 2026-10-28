#pragma once
/* antipiracy_scan.h - Spyro 3's anti-piracy scan observer: the level
 * overlays' scanner mobys and the boot, title and loading walks.
 *
 * 24 of Spyro 3's 37 level overlays carry a scanner moby whose update walks
 * a CRC-16 over EXE .text and then over the overlay's own .text, a few
 * dozen bytes per call; 8 of them carry a second scanner over one EXE
 * function. A failed scan clears an egg-collected byte in D_80070300. The
 * observer reads each scanner's cursor at the scanner's entry and takes the
 * CRC word at the store that ends the scan (a one-function scan: at the
 * entry of the call that compares), logs one line per completed scan with
 * the game's own pass test, and counts the scans. It stores nothing to
 * guest RAM: its store hook passes every store down the chain unchanged.
 * Each scan's and walk's state is published to mods through
 * spyro3_antipiracy_scan_query (the mod api's antipiracy_scan).
 *
 * Called from game_extras_impl.c (the entry verdict hook). */

#include <stdint.h>

typedef struct CPUState CPUState;
struct openpete_mod_scan;

/** @brief Observe a level overlay's anti-piracy scanner, and the boot, title and loading walks, at a function entry.
 *
 * First removes the end-store hook once the call it was installed for has
 * returned (the stack pointer is back at that call's entry value). Then the
 * walks: crc16's entry from one of func_8002AB38's check loops (the boot
 * walk), func_title_80074DEC's entry and the first entry after its loops
 * (the title walk), func_8002CA50's entry (the loading walk); each walk
 * check logs `<walk> walk check <n>: PASS|FAIL` and a failure goes through
 * spyro3_antipiracy_report_fail with the walk's name as source. Returns
 * at once below the lowest scanner address. Otherwise the resident overlay
 * (spyro_get_active_overlay_magic) selects its scanner rows; an entry at a
 * row's address reads the scanner moby's data block (the word at a0) and
 * its cursor. A cursor that moved counts one scan step. An EXE plus overlay
 * scan within 64 bytes of its end installs a hook on the guest word store;
 * the store that sets the cursor to the end reads the CRC word, which is
 * the value the game's compare reads before a failing compare (in the
 * scanner or in a companion moby) zeroes it. A one-function scan reads its
 * CRC word at the entry that finds the cursor at the end, the call whose
 * compare runs. Each completed scan logs, once, a line
 * `level <n> scan <addr> moby <data> complete: PASS|FAIL crc 0x<crc> steps
 * <k> vbl <v>` under the `antipiracy` subsystem with the run's cumulative
 * counts. The pass test is the scanner's own: the CRC word is 0 for an EXE
 * plus overlay scan, and for a one-function scan
 * `((crc >> 12) & 0xFFFF0) | (D_800722B8 & 0xFFF0000F)` equals the check
 * word in the overlay header. A FAIL goes through
 * spyro3_antipiracy_report_fail. A scan left unfinished when its overlay
 * is replaced or the process exits logs an `incomplete` line with its
 * cursor; an EXE plus overlay scan first seen at its end without its end
 * store (a state loaded past it) logs `complete: UNOBSERVED` at warning
 * level and is not counted.
 * Speculative ticks are skipped.
 * @param cpu The guest CPU state at the entry (a0 is the moby).
 * @param addr The entered function's guest vaddr. */
void spyro3_antipiracy_scan_on_entry(CPUState* cpu, uint32_t addr);

/** @brief Log an anti-piracy failure line and count it.
 *
 * Writes `FAIL <source>: <detail>` at warning level under the `antipiracy`
 * subsystem; check_determinism_matrix.sh and check_psx_oracle.sh fail a run
 * whose log carries that prefix. A scan verdict that fails calls it with
 * source `scan`; a store watch over the scanned ranges reports a store
 * there with source `watch` and the writer in the detail.
 * @param source The failing check: `scan`, `watch`.
 * @param detail One line naming what failed, without a newline. */
void spyro3_antipiracy_report_fail(const char* source, const char* detail);

/** @brief Report a guest or host store into a scanned range.
 *
 * The failure path of a store watch over EXE .text 0x8001200C..0x80064F9C
 * and the resident overlay's scanned text: formats the store and its
 * writer and calls spyro3_antipiracy_report_fail with source `watch`.
 * games/spyro-3/scan_watch.c's store watch calls it for each reported hit.
 * @param addr Guest address of the store.
 * @param size Bytes stored.
 * @param writer Who stored: a mod name, `host poke`, `savestate load`, or
 *        the recompiled store's PC. */
void spyro3_antipiracy_report_watch_hit(uint32_t addr, uint32_t size, const char* writer);

/** @brief Record the loading walk's verdict, taken by the loader-state hook at the game's compare (loader state 8).
 *
 * Logs a `loading walk check` line, reports a failure through
 * spyro3_antipiracy_report_fail with source `loading`, and publishes the
 * verdict to antipiracy_scan.
 * @param passed 1 when the cursor reached loading_text_end with a CRC of 0.
 * @param cursor The cursor word the compare read.
 * @param crc The CRC word the compare read. */
void spyro3_antipiracy_loading_verdict(int passed, uint32_t cursor, uint32_t crc);

/** @brief Fill a mod's view of one scan: a walk (OP_SCAN_KEY_*) or a scanner moby's data block.
 *
 * The descriptor hook behind the mod api's antipiracy_scan. Walks report
 * IDLE until first entered; the boot walk is observed at crc16's entry
 * from each of func_8002AB38's four check loops, the title walk's two
 * checks are computed at func_title_80074DEC's entry and published once
 * its loops have run, the loading walk runs while the loader is in states
 * 2..7 with its cursor set. A scanner moby reports the slot this observer
 * keeps for its data block.
 * @param key The walk key or data block.
 * @param out Receives the state (struct_size already checked).
 * @return 0 when filled, -1 for a data block no scanner of the resident
 *         level has run with. */
int spyro3_antipiracy_scan_query(uint32_t key, struct openpete_mod_scan* out);

/** @brief Return the run's anti-piracy counts.
 *
 * @param completed Receives the scans that reached their end, or NULL.
 * @param failed Receives the failures reported, scan and watch, or NULL.
 * @param steps Receives the scan steps observed, or NULL. */
void spyro3_antipiracy_counts(uint32_t* completed, uint32_t* failed, uint32_t* steps);
