// core/include/openpete_psx.h - slim PSX runtime public API.
//
// Replaces psxrecomp's bundled (Tomba-shaped) runner with a
// minimal layer. We keep psxrecomp's CPUState definition (so generated
// code stays compatible) but own boot/dispatch/memory/crash here.

#ifndef OPENPETE_PSX_H
#define OPENPETE_PSX_H

#include <stddef.h>
#include <stdint.h>

/* CPUState lives in psxrecomp's psx_runtime.h - vendored, untouched. */
#include "psx_runtime_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Boot ---------------------------------------------------------------- */

/** @brief Load a PSX-EXE into guest RAM and seed the CPU registers from its header.
 *
 * Checks the `PS-X EXE` magic and that the 2048-byte header and its
 * file_size fit in @p len. Seeds the BIOS kernel's fixed words in low RAM,
 * copies the body to load_address, and zeroes the memfill (BSS) range when
 * it fits in the 2 MiB of RAM, else logs a warning and skips it. sp is
 * initial_sp_base + initial_sp_offset, with a base of 0 read as
 * 0x801FFFF0; fp equals sp, gp and pc come from the header, and ra is
 * 0xBFC03D60, the return address the SCPH-1001 BIOS Exec passes.
 * @param cpu CPU state to seed.
 * @param bytes The EXE file.
 * @param len Size of @p bytes.
 * @return 0 on success, 1 with a logged error on a NULL argument, a short
 *         or truncated file, or a bad magic. */
int openpete_psx_boot(CPUState* cpu, const uint8_t* bytes, size_t len);

/* --- PSX bus constants --------------------------------------------------- *
 * Plain PSX hardware values, named so core/ doesn't carry bare hex
 * literals that the purity grep would mistake for game-specific
 * addresses. KSEG0 = cached unmapped, KSEG1 = uncached unmapped, KUSEG
 * = user - all alias the same 2MB RAM in this emulator. */
#define OPENPETE_PSX_KSEG0_BASE     0x80000000u
#define OPENPETE_PSX_KSEG1_BASE     0xA0000000u
#define OPENPETE_PSX_SCRATCH_BASE   0x9F800000u
#define OPENPETE_PSX_RAM_SIZE       (2u * 1024u * 1024u)
#define OPENPETE_PSX_SCRATCH_SIZE   1024u
#define OPENPETE_PSX_KSEG0_END      (OPENPETE_PSX_KSEG0_BASE + OPENPETE_PSX_RAM_SIZE)
#define OPENPETE_PSX_KSEG1_END      (OPENPETE_PSX_KSEG1_BASE + OPENPETE_PSX_RAM_SIZE)

/* --- Memory map ---------------------------------------------------------- */

/** @brief Zero the CPU state, main RAM and scratchpad, and install the memory callbacks.
 *
 * Installs the word, halfword and byte read and write callbacks and the
 * unaligned LWL, LWR, SWL and SWR helpers of core/psx/cpu.c.
 * @param cpu CPU state to reset. */
void openpete_psx_runtime_init(CPUState* cpu);

/** @brief Return the 2 MiB main RAM array, indexed by physical address.
 *
 * @return The array. */
uint8_t* openpete_psx_get_ram(void);
/** @brief Return the 1 KiB scratchpad array.
 *
 * @return The array. */
uint8_t* openpete_psx_get_scratch(void);

/* Gameplay-state regions for the determinism harness. The runtime's
 * --gameplay-hash-log path hashes the union of these regions per
 * yield. Per-game TUs (games/<game>/...) define the actual table; if
 * a game doesn't supply one, openpete_state_region_count == 0 and the
 * hash log is just the FNV1a-64 offset basis (effectively a no-op
 * sentinel). See games/<game>/manifest/gameplay_state_regions.toml +
 * tools/gen_state_regions.py for the canonical generation path. */
/* DIRECT regions live at a fixed PSX vaddr.
 * INDIRECT_ARRAY regions live behind a pointer at `start`: the runtime
 * reads a 4-byte PSX vaddr from RAM[start] and hashes `length` bytes
 * starting there. Used for moby pools whose backing storage moves with
 * level overlays. NULL/unmapped pointers are skipped (the slot just
 * isn't allocated this frame). */
typedef enum {
    OPENPETE_REGION_DIRECT                  = 0,
    OPENPETE_REGION_INDIRECT_ARRAY          = 1,
    /* Like INDIRECT_ARRAY, but clamps the hashed length to the live
     * range derived from a runtime end-pointer or count value. The
     * static `length` field is still the cap (max_elements *
     * element_size); the live extent is computed each frame from
     * `aux`. Lets us avoid hashing stale heap garbage in unallocated
     * tail slots of bump-allocated pools (e.g. g_LevelMobys past
     * g_MobyAllocPtr). */
    OPENPETE_REGION_INDIRECT_ARRAY_END_PTR  = 2, /* aux = addr of end ptr (u32 PSX vaddr); live = *aux - *start */
    OPENPETE_REGION_INDIRECT_ARRAY_COUNT    = 3, /* aux = addr of int count; live = *aux * elem_size */
    /* Pool terminated by a sentinel byte inside an element: aux = sentinel
     * offset within the element (low 8 bits) | sentinel value << 8; live =
     * the elements before the first whose byte at that offset equals the
     * value, the terminator excluded. Cap at `length` when none is found. */
    OPENPETE_REGION_INDIRECT_ARRAY_SENTINEL = 4,
} OpenpeteStateRegionKind;

typedef struct {
    uint32_t    kind;        /* OpenpeteStateRegionKind */
    uint32_t    start;       /* PSX virtual address (KSEG0); for INDIRECT*, the addr of the base pointer */
    uint32_t    length;      /* bytes to hash; for INDIRECT*, the cap (element_size * max_elements) */
    uint32_t    aux;         /* end-ptr or count addr for INDIRECT_ARRAY_{END_PTR,COUNT}; 0 otherwise */
    uint32_t    elem_size;   /* element_size for INDIRECT_ARRAY_COUNT; 0 otherwise */
    const char* name;        /* for diagnostics */
    /* Byte-mask: list of byte offsets whose value is excluded from
     * the region hash. Lets the gameplay-state hash ignore bytes
     * that are functionally tied to host-emulator-specific behavior
     * (e.g. SPU voice-slot indices) without dropping the surrounding
     * gameplay-meaningful state from the gate.
     *
     *   mask_per_element=0: offsets are absolute into the region
     *                       (length-bounded; e.g. g_Spyro 0x2A0).
     *   mask_per_element=1: offsets repeat per element_size - for
     *                       INDIRECT_ARRAY pools, masks every
     *                       element's offset N (e.g. Moby+0x54).
     * mask_count==0 means hash everything.
     *
     * INDIRECT_ARRAY_END_PTR has no elem_size set (length is the cap),
     * so per-element masks must specify the per-Moby stride implicitly
     * via the offset (here, `length / max_elements`); manifest emits
     * elem_size for these regions when a per-element mask is present. */
    const uint16_t* mask_offsets;
    uint16_t    mask_count;
    uint8_t     mask_per_element;
    /* CD-load transient region: its backing storage is refreshed by a
     * synchronous psyq_CdRead while the oracle (real-hardware-timed CD)
     * fills the same buffer gradually, so this region legitimately
     * diverges for the duration of a load and then re-converges. The
     * .psxtrace gate tolerates such a divergence ONLY if it heals within
     * a bounded window (a permanent divergence here still fails); every
     * other region must match bit-exact every tick. Set per-region in
     * games/<game>/manifest/gameplay_state_regions.toml (`cd_load_transient = true`).
     * Becomes a no-op once openpete models CD timing faithfully. */
    uint8_t     cd_load_transient;
    /* Render-side region, audited gameplay-neutral. Its divergence is
     * counted/logged for diagnostics but NEVER hard-fails the gate,
     * because the region holds presentation-only state (e.g. particle
     * drawn-flags) that no gameplay path consumes. The audit obligation
     * is discharged by keeping the DOWNSTREAM regions any real coupling
     * would flow through (psyq_rand_seed, g_Spyro, g_Sparx, ...) fully
     * tracked: if this region ever did affect gameplay, those would
     * trip. Same spirit as the 0x80071xxx moby-render-queue removal, but
     * keeps the region visible in per-region diagnostics rather than
     * dropping it (and avoids a golden recapture). Set per-region in
     * games/<game>/manifest/gameplay_state_regions.toml (`gameplay_neutral = true`);
     * the proof of neutrality must accompany it in the manifest. */
    uint8_t     gameplay_neutral;
    /* The row is the state of a host device model that resolves at a
     * coarser precision than the hardware (the SPU envelope, advanced per
     * VBLANK where hardware advances it per sample), so on some ticks its
     * bytes lead or lag the reference's by a tick. Its divergence is
     * counted against diverge_baseline and never hard-fails the gate;
     * every gameplay region that consumes the row's answers stays
     * strict. Set per-region in the manifest (`host_precision = true`)
     * with the mechanism named. */
    uint8_t     host_precision;
    /* The row's bytes are expected not to change over an oracle run (a
     * value set once per level, a record table): the verifier lists every
     * region whose hash never changed between consecutive records, and the
     * gate fails a PASS that lists one without this flag. The manifest row
     * carries `constant_ok = true` and the reason. */
    uint8_t     constant_ok;
    /* Ratchet for a tolerated row (cd_load_transient, gameplay_neutral or
     * host_precision):
     * the most records the row may diverge on over a full run. A run that
     * reaches the trace's end with a higher count fails; -1 = unbounded.
     * The manifest row carries `diverge_baseline = N` from the measured
     * golden. */
    int32_t     diverge_baseline;
} OpenpeteStateRegion;
extern const OpenpeteStateRegion* openpete_state_regions;
extern size_t                     openpete_state_region_count;
/* FNV-1a-64 of the flat table text (gen_state_regions.py --out-flat) the
 * registered table was generated from; a .psxtrace golden carries the hash
 * of the table its capture hashed with, and the verifier refuses one that
 * differs. */
extern uint64_t                   openpete_state_regions_table_hash;

/** @brief Install the per-game gameplay-state region table that the hash log and .psxtrace paths read.
 *
 * Stores the three arguments in openpete_state_regions,
 * openpete_state_region_count and openpete_state_regions_table_hash.
 * game_state_regions_register calls it from game main before
 * openpete_run(). Registration is an explicit call because a weak-symbol
 * override mis-resolves under the MinGW PE linker. Without a call the table
 * stays empty and every gameplay hash is the FNV-1a-64 offset basis.
 * @param regions The generated region table.
 * @param count The number of entries in regions.
 * @param table_hash FNV-1a-64 of the flat table text the table was generated
 *        from; the .psxtrace verifier refuses a golden carrying another. */
void openpete_state_regions_register(const OpenpeteStateRegion* regions,
                                     size_t count, uint64_t table_hash);

/** @brief Map a PSX virtual address to a host pointer.
 *
 * Main RAM and the scratchpad resolve under any segment (the top three
 * address bits are dropped); any other address resolves through the
 * registered presentation region table. PSYQ thunks use it to translate
 * pointer arguments, held in cpu->aN as PSX addresses, to host pointers
 * PsyCross can dereference. The pointer is valid for one byte; a host
 * site that reads or writes more resolves through
 * openpete_psx_to_host_span().
 * @param addr PSX virtual address; 0 is PSX NULL.
 * @return The host pointer, or NULL for 0 or an unmapped address. */
void* openpete_psx_to_host_ptr(uint32_t addr);

/** @brief Map a PSX virtual address to a host pointer valid for @p len bytes.
 *
 * The span [addr, addr + len) must lie inside one region: main RAM (any
 * segment), the scratchpad, or one registered presentation region. A
 * span that crosses the end of its region is refused, so a guest pointer
 * near the top of RAM cannot carry a host read or write past g_ram or
 * the scratchpad array. Every host site that reads or writes more than
 * one byte through a guest pointer resolves it here with the width it
 * touches.
 * @param addr PSX virtual address; 0 is PSX NULL.
 * @param len Bytes the caller reads or writes from the returned pointer.
 * @return The host pointer, or NULL for 0, an unmapped address, or a span
 *         crossing its region's end. */
void* openpete_psx_to_host_span(uint32_t addr, uint32_t len);

/** @brief Map a guest C string to a host pointer whose bytes up to its terminator lie inside one region.
 *
 * The scan for the NUL stops at the end of the address's region (main RAM,
 * the scratchpad, or one registered presentation region) and after @p max
 * bytes. A string whose first @p max bytes hold no NUL is accepted with
 * length @p max, as a bounded reader (strncmp, a printf precision) sees it;
 * a string whose region ends before both its NUL and @p max is refused, so
 * a host strlen or strcmp on the result never reads past the region.
 * @param addr PSX virtual address; 0 is PSX NULL.
 * @param max The most bytes the caller reads; UINT32_MAX for a C string
 *        read to its terminator.
 * @param len_out Receives the length in bytes, the NUL excluded, when
 *        non-NULL and the string is accepted.
 * @return The host pointer, or NULL for 0, an unmapped address, or a
 *         string that runs off its region's end. */
const char* openpete_psx_to_host_str(uint32_t addr, uint32_t max, uint32_t* len_out);

/** @brief Copy bytes into main RAM at a PSX virtual address.
 *
 * The copy is clamped, with a warning, to the bytes between the target and
 * the end of main RAM, so a target outside main RAM copies nothing; an
 * unmapped target logs an error.
 * @param addr Destination PSX address.
 * @param data Source bytes.
 * @param size Byte count. */
void openpete_psx_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* --- Dispatch ------------------------------------------------------------ */

/* The recompiled-code dispatch entry lives behind the recomp-module
 * descriptor (openpete_recomp_module.h) - core carries no link-time
 * reference to generated code. Game code that needs to re-enter the
 * recompiled world uses openpete_recomp_module_get()->dispatch. */

/* --- Root counter 2 ------------------------------------------------------ */

/** @brief Set the value a word or halfword read of root counter 2's current-count register (0x1F801120) returns.
 *
 * openpete runs no timers. A game that seeds its RNG from the counter at
 * boot gets this constant, the value the hardware reference produced on
 * the captured boot, so every boot seeds as that one did; game_main sets
 * it from psyq_game_root_counter2() before the run.
 * @param value Counter value. */
void     openpete_psx_set_root_counter2(uint16_t value);
/** @brief Set the function that may answer a read of root counter 2's count register before the constant does.
 *
 * The oracle schedule sets it to answer the game's reads with the counts
 * the hardware reference read (openpete_sched_rcnt2_read); a reader that
 * returns 0 leaves the read to the constant.
 * @param reader The function, or NULL for none. */
void     openpete_psx_set_root_counter2_reader(int (*reader)(uint16_t* value_out));
/** @brief Return the value set by openpete_psx_set_root_counter2(), 0 until set.
 *
 * @return The counter value. */
uint16_t openpete_psx_root_counter2(void);

/* --- Crash handler ------------------------------------------------------- */

/** @brief Install the process crash handler that writes host and guest state to the original stderr and the log file.
 *
 * The report bypasses the stderr pipe of the log mirror (it is written to
 * the descriptors openpete_log_file_crash_fds names), so it reaches the
 * log file before the process dies. On POSIX it takes SIGSEGV, SIGABRT,
 * SIGFPE, SIGILL and SIGBUS through sigaction with SA_SIGINFO | SA_ONSTACK
 * | SA_RESETHAND; the handler makes only async-signal-safe calls, runs on
 * the calling thread's alternate stack (openpete_crash_thread_attach in
 * openpete_crash.h) and
 * re-raises under the default disposition. On Windows it sets an
 * unhandled-exception filter that writes the report, writes a minidump
 * (openpete-crash.dmp in the logs state directory) and continues the
 * search, and records the calling thread as main for the thread label.
 * The guest registers come from the CPUState passed to
 * openpete_psx_set_diag_cpu. */
void openpete_psx_install_crash_handler(void);

/** @brief Fault the calling thread when OPENPETE_CRASH_TEST asks for it, to exercise the crash path.
 *
 * "segv" stores through a null pointer; "overflow" recurses with a 4 KiB
 * frame until the stack runs out. Any other value, or none, returns at
 * once ("pipe" faults inside the first pipeline create instead, from the
 * native renderer's create hook). The game fiber calls it before dispatching the game's entry, so
 * both faults land on the fiber's stack. */
void openpete_crash_test(void);

/* --- EXE loader (BIN/CUE -> bytes) ---------------------------------------- */

/** @brief Read the game's EXE out of a disc image.
 *
 * Callers pass game_get_exe_filename(); the lookup is
 * openpete_psx_load_file_from_disc's. A directory entry larger than 2 MiB
 * plus the 2048-byte header is refused before any allocation.
 * @param cue_path Disc path, resolved by openpete_psx_resolve_disc_bin.
 * @param exe_filename iso9660 path of the EXE.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure,
 *         with the reason logged. */
uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                         const char* exe_filename,
                                         size_t* out_size);

struct disc_image;

/** @brief Read the game's EXE out of an open disc image under its reads' tag.
 *
 * The lookup and the 2 MiB + 2048 size cap are
 * openpete_psx_load_exe_from_disc's.
 * @param img An image opened with disc_image_open.
 * @param exe_filename iso9660 path of the EXE.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure,
 *         with the reason logged. */
uint8_t* openpete_psx_read_exe_from_image(struct disc_image* img,
                                          const char* exe_filename,
                                          size_t* out_size);

/** @brief Read one file out of a disc image by iso9660 path.
 *
 * The path is a root file ("SYSTEM.CNF") or one directory level deep
 * ("S0/S000000D.NSF", '/' or '\'), looked up by disc_image_find; name
 * matching is case-insensitive. A directory size above 0x7FFFFFFF bytes
 * is refused before any allocation. The reads go through the disc cache
 * when it holds the image, trace under the calling thread's trace scope,
 * and never steer the cache loader.
 * @param cue_path Disc path, resolved by openpete_psx_resolve_disc_bin.
 * @param filename iso9660 path of the file.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL when an argument
 *         is NULL, the image is not found or opened, the file is absent or
 *         too large, or the read is short, with the reason logged except on
 *         allocation failure. */
uint8_t* openpete_psx_load_file_from_disc(const char* cue_path,
                                          const char* filename,
                                          size_t* out_size);

/** @brief Resolve a --disc path to the BIN file holding the sector image.
 *
 * A .bin, .img or .iso path resolves to itself when the file exists; a .cue
 * that exists resolves to the first existing file of, in order, the file
 * its FILE line names (relative to the cue's directory), the cue's own
 * basename with a .bin extension, and the single .bin beside the cue. Any
 * other extension resolves to nothing. Extensions match case-insensitively.
 * Throws nothing: a filesystem error resolves to no candidate.
 * @param disc_path The --disc path.
 * @param out Receives the resolved path.
 * @param out_cap Capacity of @p out; a path that does not fit fails.
 * @param tried Receives the candidates tried, separated by ", "; may be
 *        NULL.
 * @param tried_cap Capacity of @p tried.
 * @return 0 on success; -1 on failure or when @p disc_path or @p out is
 *         NULL or @p out_cap is 0. */
int openpete_psx_resolve_disc_bin(const char* disc_path,
                                  char* out, size_t out_cap,
                                  char* tried, size_t tried_cap);

/** @brief Read the image's release serial ("SCUS-94228") from its SYSTEM.CNF BOOT line.
 *
 * The BOOT value is read after optional spaces, an optional "cdrom:" and
 * leading slashes or backslashes, up to ';' or the line end
 * (SCUS_942.28); '.' and '_' become '-', and the second '-' is dropped.
 * The read is a few sectors, so it runs at every boot to select the
 * [disc.SERIAL] scope in openpete.toml for an image whose content hash is
 * unknown.
 * @param image_path Disc path, resolved by openpete_psx_resolve_disc_bin.
 * @param out Receives the serial.
 * @param out_cap Capacity of @p out; at least 12.
 * @return 0 on success; -1 when an argument is invalid, the image or
 *         SYSTEM.CNF (at most 4096 bytes) cannot be read, or no serial is
 *         found. */
int openpete_psx_read_disc_serial(const char* image_path,
                                  char* out, size_t out_cap);

/** @brief Report whether interrupts are masked: the kernel's critical-section flag.
 *
 * Set at boot (the kernel enters a critical section before the executable
 * runs), by EnterCriticalSection and for the duration of a delivered VBL;
 * cleared by ExitCriticalSection and the kernel calls that leave the
 * section (StartCARD2, StopCARD2).
 * @return 1 while masked, else 0. */
int openpete_crit_masked(void);

/** @brief Overwrite the critical-section flag, without taking a latched VBLANK.
 *
 * The delivery side sets it around a delivered VBL and restores the
 * interrupted code's value afterwards; savestates restore it.
 * @param masked Nonzero to mask. */
void openpete_crit_set(int masked);

/** @brief Leave the critical section from a host-modelled kernel call, taking a VBLANK latched inside it.
 * @param cpu Guest CPU state at the call. */
void openpete_crit_leave(CPUState* cpu);

/** @brief Read the critical-section probe: the first clearing site and the call counts.
 * @param first_clear_ra Receives the return address of the first call that enabled interrupts, 0 before one.
 * @param first_clear_vbl Receives `openpete_vbl` there, -1 before one.
 * @param enters Receives the EnterCriticalSection count.
 * @param exits Receives the ExitCriticalSection count.
 * @param kernel_leaves Receives the count of leaving kernel calls. */
void openpete_crit_report(uint32_t* first_clear_ra, int32_t* first_clear_vbl,
                          uint32_t* enters, uint32_t* exits, uint32_t* kernel_leaves);

/** @brief Name the game's loop anchor: the function its main loop calls first, once per game tick.
 *
 * The dispatcher counts its entries (openpete_loop_anchor_ticks), the
 * host's tick marker.
 * @param pc The anchor's address; 0 counts nothing. */
void openpete_loop_anchor_set(uint32_t pc);

/** @brief Return how many times the game's loop anchor has been entered: game ticks begun since boot.
 * @return The entry count; 0 without an anchor. */
uint32_t openpete_loop_anchor_ticks(void);

/** @brief Log the per-site loop census and the loop watchdog's maximum.
 *
 * One line per classified loop header that took its back-edge: its polls,
 * runs (polls whose previous poll was at another header) and quanta, and
 * the VBLs of its first poll and first quantum. A header with no line never
 * polled: not entered, or closed on its first test. The summary line holds
 * the most quanta counted at one `openpete_vbl`, the watchdog's trip count
 * and the re-dispatches that skipped the entry observers. */
void openpete_loop_site_census_log(void);

/** @brief Stop the hang watchdog's stall monitor thread and join it; idempotent, a no-op when it never started.
 *
 * Runs before SDL_Quit on every teardown that reaches it: the host loop's
 * exits call it ahead of PsyX_Shutdown, and the monitor registers it with
 * atexit for exit() paths. The watchdog's own trips leave by _Exit and run
 * neither. Never call it on the monitor thread. */
void openpete_stall_monitor_stop(void);

/** @brief Return the classified-loop instructions counted since the loop anchor's last entry.
 * @return The static body counts summed over every back-edge poll since the anchor. */
uint64_t openpete_loop_insns_since_anchor(void);

/** @brief Restart the polling-loop accumulator: a VBLANK elapsed, taken or held. */
void openpete_loop_vbl(void);

/** @brief Snapshot guest RAM and the scratchpad before one device row of a VBLANK, when OPENPETE_VBL_LEAF_CHECK=1.
 *
 * Does nothing with the variable unset or while a snapshot is open. */
void openpete_vbl_leaf_begin(void);

/** @brief Compare guest RAM and the scratchpad with the open snapshot and count the row.
 *
 * A differing byte means the device row wrote CPU-half state: it is
 * counted against the row and its first address logged as an error (the
 * first eight per row). Does nothing without an open snapshot.
 * @param row The device row's name, a string literal the census keeps. */
void openpete_vbl_leaf_end(const char* row);

/** @brief Log the leaf check's census: runs and writing runs per device row, and the totals.
 *
 * Does nothing with the check off. */
void openpete_vbl_leaf_report(void);

/** @brief Read the polling-loop accumulator's run counters.
 * @param polls Receives the back-edge polls counted.
 * @param trips Receives the quanta reached.
 * @param acc_max_insns Receives the most loop instructions accumulated between two VBLs.
 * @param quanta_max Receives the most quanta reached between two VBLs.
 * @param last_header Receives the loop header of the last quantum reached, 0 before one.
 * @param enabled Receives 0 when OPENPETE_LOOP_DELIVERY=0 disabled delivery at a quantum. */
void openpete_loop_report(uint64_t* polls, uint64_t* trips, uint64_t* acc_max_insns,
                          uint32_t* quanta_max, uint32_t* last_header, int* enabled);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSX_H */
