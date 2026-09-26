// core/include/openpete_psx.h — slim PSX runtime public API.
//
// Phase 1b: replaces psxrecomp's bundled (Tomba-shaped) runner with a
// minimal layer. We keep psxrecomp's CPUState definition (so generated
// code stays compatible) but own boot/dispatch/memory/crash here.

#ifndef OPENPETE_PSX_H
#define OPENPETE_PSX_H

#include <stddef.h>
#include <stdint.h>

/* CPUState lives in psxrecomp's psx_runtime.h — vendored, untouched. */
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
 * = user — all alias the same 2MB RAM in this emulator. */
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
 * tools/gen_state_regions.py for the canonical generation path, and
 * notes/plans/openpete-determinism-enforcement.md Phase 1. */
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
     *   mask_per_element=1: offsets repeat per element_size — for
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
     * Becomes a no-op once openpete models CD timing faithfully — see
     * notes/plans/openpete-psx-oracle-ci.md. */
    uint8_t     cd_load_transient;
    /* Render-side region, audited gameplay-neutral. Its divergence is
     * counted/logged for diagnostics but NEVER hard-fails the gate,
     * because the region holds presentation-only state (e.g. particle
     * drawn-flags) that no gameplay path consumes. The audit obligation
     * is discharged by keeping the DOWNSTREAM regions any real coupling
     * would flow through (psyq_rand_seed, g_Spyro, g_Sparx, …) fully
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

/* Install the per-game region table. Call from game main before
 * openpete_run() (explicit registration — weak-symbol override
 * mis-resolves under the MinGW PE linker). */
void openpete_state_regions_register(const OpenpeteStateRegion* regions,
                                     size_t count, uint64_t table_hash);

/** @brief Map a PSX virtual address to a host pointer.
 *
 * Main RAM and the scratchpad resolve under any segment (the top three
 * address bits are dropped); any other address resolves through the
 * registered presentation region table. PSYQ thunks use it to translate
 * pointer arguments, held in cpu->aN as PSX addresses, to host pointers
 * PsyCross can dereference.
 * @param addr PSX virtual address; 0 is PSX NULL.
 * @return The host pointer, or NULL for 0 or an unmapped address. */
void* openpete_psx_to_host_ptr(uint32_t addr);

/** @brief Map a host pointer back to its PSX address.
 *
 * A pointer into main RAM maps to its KSEG0 address and one into the
 * scratchpad to 0x9F800000 + offset. Any other pointer whose low 32 bits
 * already form a KSEG0, KSEG1 or KUSEG RAM address returns those bits;
 * decompiled source that computes a pointer by arithmetic on an address
 * literal produces such values. The source rewriter calls it when native
 * code stores a host pointer into a PSX-aliased slot.
 * @param host_ptr Host pointer.
 * @return The PSX address, or 0 (PSX NULL) for NULL or any other pointer. */
uint32_t openpete_host_to_psx(const void* host_ptr);

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
 * descriptor (openpete_recomp_module.h) — core carries no link-time
 * reference to generated code. Game code that needs to re-enter the
 * recompiled world uses openpete_recomp_module_get()->dispatch. */

/* Call a PSX function by vaddr from native C/C++ code. Used by the source
 * rewriter for `(*FN_PTR_GLOBAL)(args...)` call sites in matching-decomp,
 * where FN_PTR_GLOBAL stores a PSX vaddr. Routes through call_by_address
 * — tries the native dispatch table first, falls back to compiled, then
 * to MIPS interpretation for overlay code. Saves/restores the caller's
 * cpu scratch slots. Pointer args must already be converted to PSX
 * vaddrs (use openpete_host_to_psx); int args pass through as uint32_t.
 * Up to 4 register args (a0..a3); 5+ args not yet supported. Returns
 * cpu->v0 — caller casts to int / pointer / etc. as appropriate. */
uint32_t openpete_psx_call_fnptr(uint32_t vaddr,
                                 uint32_t a0, uint32_t a1,
                                 uint32_t a2, uint32_t a3);

/* --- Root counter 2 ------------------------------------------------------ */

/** @brief Set the value a word or halfword read of root counter 2's current-count register (0x1F801120) returns.
 *
 * openpete runs no timers. A game that seeds its RNG from the counter at
 * boot gets this constant, the value the hardware reference produced on
 * the captured boot, so every boot seeds as that one did; game_main sets
 * it from psyq_game_root_counter2() before the run.
 * @param value Counter value. */
void     openpete_psx_set_root_counter2(uint16_t value);
/** @brief Return the value set by openpete_psx_set_root_counter2(), 0 until set.
 *
 * @return The counter value. */
uint16_t openpete_psx_root_counter2(void);

/* --- Crash handler ------------------------------------------------------- */

/** @brief Install the process crash handler that dumps host and guest state to stderr.
 *
 * On POSIX it takes SIGSEGV, SIGABRT, SIGFPE, SIGILL and SIGBUS through
 * sigaction with SA_SIGINFO; the handler prints the dump, drains the log
 * mirror and re-raises under the default disposition. On Windows it sets an
 * unhandled-exception filter that prints the dump and continues the search,
 * and records the calling thread as main for the dump's thread label. The
 * guest registers come from the CPUState passed to
 * openpete_psx_set_diag_cpu. */
void openpete_psx_install_crash_handler(void);

/* --- EXE loader (BIN/CUE → bytes) ---------------------------------------- */

/* Open a .cue/.bin disc image, find game_get_exe_filename() in the root,
 * and return its bytes. Caller frees with free(). On failure returns NULL
 * and logs the reason. */
uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                         const char* exe_filename,
                                         size_t* out_size);

/* General form of the above: fetch any file from the image by iso9660
 * path ("SYSTEM.CNF", "S0/S000000D.NSF" — one directory level deep at
 * most, matching the reader). Caller frees with free(). Returns NULL and
 * logs on failure. */
uint8_t* openpete_psx_load_file_from_disc(const char* cue_path,
                                          const char* filename,
                                          size_t* out_size);

/* Resolve a --disc path to the BIN holding the sector image. A .bin/.img/
 * .iso path resolves to itself; a .cue resolves to, in order, the file its
 * FILE line names (relative to the cue's directory), the cue's own basename
 * with a .bin extension, and the single .bin sitting beside the cue. Writes
 * the resolved path to out and returns 0; on failure returns non-zero and
 * writes the candidates tried, separated by ", ", to tried (may be NULL).
 * Throws nothing: filesystem errors resolve to "candidate absent". */
int openpete_psx_resolve_disc_bin(const char* disc_path,
                                  char* out, size_t out_cap,
                                  char* tried, size_t tried_cap);

/* The image's release serial ("SCUS-94228") from its SYSTEM.CNF BOOT line.
 * Cheap (a few sectors) — it is read at boot to select the [disc.SERIAL]
 * scope in openpete.toml. Returns 0 on success. */
int openpete_psx_read_disc_serial(const char* image_path,
                                  char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PSX_H */
