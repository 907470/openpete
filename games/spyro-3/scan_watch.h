// games/spyro-3/scan_watch.h - the watch and guard over the code ranges
// Spyro 3's anti-piracy scans hash.
//
// The title, loading and per-level scans run CRC-16 over EXE .text
// (main_TEXT_START..main_TEXT_END, 0x8001200C..0x80064F9C) and over the
// resident overlay's text (its text_start up to data_start, or text_end for
// the title and loading overlays); a changed byte there fails the scan, and
// a failed scan sabotages save and level state. Three parts:
//
// - The store watch wraps the guest store hooks and reports every store into
//   a scanned range, naming the writer: a recompiler store (game fiber), a
//   host poke (host fiber) or a savestate load, as a `FAIL watch:` line
//   (spyro3_antipiracy_report_watch_hit), on which check_psx_oracle.sh and
//   check_determinism_matrix.sh fail a run that arms the watch. The overlay
//   load is not reported: the level overlays' decryption pass and a guest
//   copy that replaces the resident overlay are recognised and counted
//   apart. Armed only by OPENPETE_S3_SCAN_WATCH=1; otherwise the store
//   hooks are left untouched and a guest store costs nothing more.
// - The mod guard (spyro3_scan_guard, game_mod_desc_t.guest_guard) copies
//   the scanned ranges to a shadow at the start of every stretch of mod code
//   and compares them at its end, restoring a changed byte and naming the
//   mod. The stretches are the core's brackets (openpete_mods.h): an
//   override link and each half of it around base() and call(), a mod's
//   entry and hot-reload entry, its toggle hook, its level-ready hooks and
//   its material refine callbacks; a stretch entered inside an open one
//   closes the outer stretch first, so each change is named against the mod
//   whose code made it. A stretch costs one pass over the EXE range (332 KB)
//   and the resident overlay's range (27 to 171 KB) at each end.
//   --scan-guard 0 (OPENPETE_SCAN_GUARD=0) keeps the brackets and skips
//   every compare: a mod's store into a scanned range persists, and boot
//   logs once that the guard is off. The store watch is unaffected.
//   CARVE-OUT: a store the guard cannot see. A mod that keeps a guest()
//   pointer from a stretch and stores through it from a present hook, a UI
//   body or a thread of its own, or a refine callback the renderer's present
//   thread runs, changes the bytes outside any stretch; the next stretch
//   adopts the change as its baseline and nothing restores it. A loading
//   walk failed by such a store still names every enabled mod.
// - The loading walk's verdict counters (pass, fail), with a VACUOUS report
//   at exit for a run that loaded a level and saw no verdict.
//
// OPENPETE_S3_SCAN_POKE=<openpete_frame>:<addr>:<value> stores one word from
// the host fiber at that frame, a fault injector for the watch's host-poke
// arm.

#ifndef SPYRO3_SCAN_WATCH_H
#define SPYRO3_SCAN_WATCH_H

#include "psx_runtime_internal.h"
#include "openpete_mods.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The mod guard; Spyro 3's game_mod_desc_t names it, so the core installs it
 * before the first mod entry runs. */
extern const openpete_mods_guest_guard_t spyro3_scan_guard;

/** @brief Arm the store watch (OPENPETE_S3_SCAN_WATCH), log a guard turned off by --scan-guard 0, and register the exit report.
 *
 * Call from the boot hook, after every other store-hook installer, while
 * guest RAM holds the loaded EXE. Binds the resident-overlay lookup for
 * the watch and the guard.
 * @param cpu Guest CPU state whose store hooks the watch wraps. */
void spyro3_scan_watch_boot(CPUState* cpu);

/** @brief Run the per-tick half: resolve deferred overlay-window hits, resync the shadow, fire the host-poke injector and note a resident level.
 *
 * Call from the host fiber before the tick runs.
 * @param cpu Guest CPU state. */
void spyro3_scan_watch_tick(CPUState* cpu);

/** @brief Report the scanned bytes a savestate load changed, then adopt them.
 *
 * Call after the restore has rewritten guest RAM and resynced the resident
 * overlay. */
void spyro3_scan_watch_state_loaded(void);

/** @brief Count a loading-walk verdict; a failure names the enabled mods and lets the game's own zeroing store pass the watch unreported.
 *
 * @param passed 1 for a passing walk, 0 for a failed one. */
void spyro3_scan_watch_verdict(int passed);

#ifdef __cplusplus
}
#endif

#endif
