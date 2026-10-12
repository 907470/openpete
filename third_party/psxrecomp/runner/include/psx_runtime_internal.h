#ifndef PSX_RUNTIME_INTERNAL_H
#define PSX_RUNTIME_INTERNAL_H

/**
 * @file psx_runtime_internal.h
 * @brief Engine-side runtime interface: everything the recompiled game
 *        code and the engine share beyond CPUState.
 *
 * Not part of the mod SDK. The generated code, the runtime and the
 * feature layers include this header; mods include psx_runtime.h.
 */
#include "psx_runtime.h"

/**
 * @brief Marks data symbols shared across the exe/module boundary.
 *
 * On Windows a module writing an exe-exported data symbol would otherwise
 * depend on MinGW pseudo-relocation; marking the symbol dllimport in
 * module builds routes the access through the import table.
 * `OPENPETE_RECOMP_MODULE_BUILD` is defined only for module translation
 * units.
 */
#if defined(_WIN32) && defined(OPENPETE_RECOMP_MODULE_BUILD)
#  define OPENPETE_ABI_DATA __declspec(dllimport)
#else
#  define OPENPETE_ABI_DATA
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialise a CPUState and install the memory callbacks.
 *
 * Call before running any recompiled function. The openpete runtime
 * forwards to openpete_psx_runtime_init, which zeroes the CPU state, main
 * RAM and the scratchpad.
 * @param cpu CPU state to reset and wire to the memory callbacks. */
void psx_runtime_init(CPUState* cpu);

/** @brief Copy bytes into PSX RAM at a virtual address.
 *
 * The openpete runtime forwards to openpete_psx_load, which clamps the copy
 * to the end of main RAM and logs an unmapped target.
 * @param addr Destination PSX address.
 * @param data Source bytes.
 * @param size Byte count. */
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size);

/** @brief Log every hardware register access to mmio_trace.log.
 *
 * Call with @p enable = 1 after psx_runtime_init(). The openpete runtime
 * defines this as an empty function and traces nothing.
 * @param enable Nonzero to enable tracing. */
void psx_mmio_trace_enable(int enable);

/** @brief Install the fatal-fault handlers that dump the guest CPU state.
 *
 * openpete forwards to openpete_psx_install_crash_handler: on POSIX a
 * sigaction handler for SIGSEGV, SIGABRT, SIGFPE, SIGILL and SIGBUS, on
 * Windows an SEH unhandled-exception filter. */
void psx_install_crash_handler(void);

/** @brief Dispatch a call by address: BIOS calls and jump-register targets.
 *
 * Tries the registered recomp module's dispatch, then the BIOS service
 * handler (0xA0, 0xB0, 0xC0). An unknown address is logged through a
 * rate-limited miss table, never fatal: v0 and v1 are zeroed so the caller
 * sees a function that returned 0, and the game fiber yields to the host
 * without advancing the tick counter so a stubbed main loop still reaches
 * the frame-limit and runtime checks.
 * @param cpu CPU state of the caller.
 * @param addr Guest address to call. */
void call_by_address(CPUState* cpu, uint32_t addr);

/**
 * @brief Override gate emitted at the top of every recompiled function.
 *
 * In the openpete runtime it runs, in order: the hang watchdog (a ring of
 * recent entries and an exit with code 2 once OPENPETE_HANG_THRESHOLD
 * entries pass with no VBL), the pad-read probe, the halt-at-PC dump, the
 * oracle anchor hooks, and then the game's psx_override_dispatch_game when
 * the game defines one.
 * @param cpu CPU state of the function being entered.
 * @param addr Guest address of the function being entered.
 * @return 1 when the call was intercepted and the body is skipped. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/** @brief SYSCALL instruction handler.
 *
 * The openpete runtime logs a warning and ignores the call.
 * @param cpu CPU state of the caller.
 * @param code The 20-bit code field of the instruction. */
void psx_syscall(CPUState* cpu, uint32_t code);

/** @brief Count one iteration of a classified polling loop at its back-edge.
 *
 * The engine accumulates the body's instruction count and delivers a VBL
 * once a VBL's worth has accumulated.
 * @param cpu The looping function's CPU state.
 * @param header The loop's header address.
 * @param insns The body's instruction count.
 * @param pure 1 for a pure spin, 0 for a counted poll. */
void psx_loop_poll(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t pure);

/** @brief GTE coprocessor command.
 *
 * Runs the command on cpu->gte_data/gte_ctrl and leaves the register file
 * in its hardware read form (core/psx/gte.c).
 * @param cpu CPU state holding the GTE register file.
 * @param cmd The cop2 command field (the instruction's low 25 bits). */
void gte_execute(CPUState* cpu, uint32_t cmd);

/**
 * @brief PC of the instruction that issued the most recent GTE command.
 *
 * Written by recompiled code before each gte_execute() call. The render
 * cache tags each vertex with it to classify the producing function.
 * 0 = unknown.
 */
OPENPETE_ABI_DATA extern uint32_t openpete_gte_caller_pc;

/** @name GTE register transfers for the overlay interpreter
 * @{ */
uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);
/** @} */

/** @brief Direct access to PSX RAM (2 MB).
 *
 * @return The array, indexed by physical address. */
uint8_t* psx_get_ram(void);
/** @brief Direct access to the scratchpad (1 KB).
 *
 * @return The array. */
uint8_t* psx_get_scratch(void);

/** @brief Frame-gated GP1 display command tracker, called from the GP1 write path.
 *
 * The openpete runtime defines this as an empty function.
 * @param cmd The GP1 command word. */
void diag_track_gp1(uint32_t cmd);

/**
 * @brief Base of the g_SonyImage / D_8006FCF4 scratch pool.
 *
 * Default 0x8006FCF4, the canonical address. A consumer that relocates
 * the pool sets the new base; the recompiler's lui+addiu substitution for
 * every pair landing in [0x8006FCF4, 0x80072894) resolves to base +
 * offset.
 */
OPENPETE_ABI_DATA extern uint32_t psx_runtime_g_sony_image_base;

/** @brief `--no-lod` display policy flag, read by the native feeders' detail selection. No generated code reads it. */
extern int openpete_no_lod;

/**
 * @brief Per-iteration flag: the moby passed the canonical 4:3 cone test.
 *
 * Set inside each render-bucket walker's cone test (sub at 0x8001F30C /
 * 0x80020AB8, addi at 0x80022C0C). The substituted `sb $a0, 0x51($fp)`
 * sites (0x8001F4CC / 0x80020C7C / 0x80022D98) set `m_WasDrawn = 1` only
 * when it is set.
 */
extern uint8_t psx_runtime_native_cone_pass;

/**
 * @brief Distance half of the moby cull.
 *
 * The dist-gate hooks at the box-cull comparison PCs (shaded path
 * 0x80022B50/B58/B6C/B74/B84/B8C and the corresponding regular/specular
 * PCs) clear it when the comparand rejects; the `m_WasDrawn = 1` stores
 * gate on `(native_cone_pass && native_dist_pass)`. Reset to 1 at the
 * iteration's m_WasDrawn clear (0x80022B28 and siblings).
 */
extern uint8_t psx_runtime_native_dist_pass;

/*
 * Distance-cull hooks.
 * Append-call hooks emitted after the canonical box-cull add/sub at the
 * distance-cull PCs, receiving the destination-register comparand. Each
 * clears psx_runtime_native_dist_pass when the box cull rejects.
 */
/** @brief Clears psx_runtime_native_dist_pass when a box-cull lower-bound value is not positive.
 *
 * Appended after the add of each lower-bound test of the regular, specular
 * and shaded moby passes; the asm culls the moby when the value is at most
 * zero.
 * @param v The destination register of the add. */
void psx_runtime_dist_gate_lower(int32_t v);
/** @brief Clears psx_runtime_native_dist_pass when a box-cull upper-bound value is not negative.
 *
 * Appended after the sub of each upper-bound test and after the post-MVMVA
 * projected-Z cull of the three moby passes; the asm culls the moby when the
 * value is at least zero.
 * @param v The destination register of the sub. */
void psx_runtime_dist_gate_upper(int32_t v);

/*
 * Moby cone-X cull hooks.
 * ReplaceCall hooks at the m_WasDrawn cone-compute sites. Record the
 * canonical 4:3 verdict in psx_runtime_native_cone_pass and return the
 * canonical comparand.
 */
/** @brief Records the regular and specular moby cone-X verdict and returns mac1_abs - a2.
 *
 * Stores whether 4 * (mac1_abs - a2) - 3 * (mac3 + a3) is negative in
 * psx_runtime_native_cone_pass. The returned comparand is the asm's own
 * `|MAC1| - radius`; the generated code applies the remaining shifts.
 * @param mac1_abs The absolute value of MAC1.
 * @param a2 The moby's x radius.
 * @param mac3 The MAC3 term.
 * @param a3 The bias added to mac3.
 * @return mac1_abs - a2. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3);
/** @brief Records the shaded moby cone-X verdict and returns mac1_abs - 102.
 *
 * Stores whether 4 * (mac1_abs - 102) - 3 * (mac3 + 77) is negative in
 * psx_runtime_native_cone_pass. The constants 102 and 77 are the shaded
 * pass's fixed x radius and bias; mac3 is gte_data[27].
 * @param mac1_abs The absolute value of MAC1.
 * @param mac3 The MAC3 term.
 * @return mac1_abs - 102. */
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3);

/* Moby vertex-stream field hooks: ReplaceCall hooks substituting the
 * `srl rd, rt, 11|10` of the game's field-decode idiom. The source register
 * holds the frame word << 11; the hook undoes the shift and resolves the
 * 21-bit field through the stream-handle table. Stock values pass through
 * bit-identical. */
/** @brief Resolve a shifted moby frame word to the stream address the `srl rd, rt, 11` yields.
 *
 * Shifts @p rt right by 11 and resolves the field through
 * openpete_vstream_addr without halving: a field at or above the handle
 * bound returns unchanged, a bound handle returns its vaddr, an unbound
 * handle below the bound returns 0.
 * @param rt Source register of the substituted shift (frame word << 11).
 * @return The stream address. */
uint32_t psx_runtime_vstream_moby(uint32_t rt);
/** @brief Resolve a shifted moby frame word to the stream address the `srl rd, rt, 10` yields.
 *
 * Shifts @p rt right by 11 and resolves the field through
 * openpete_vstream_addr with halving: a field at or above the handle bound
 * returns doubled, a bound handle returns its vaddr, an unbound handle
 * below the bound returns 0.
 * @param rt Source register of the substituted shift (frame word << 11).
 * @return The stream address. */
uint32_t psx_runtime_vstream_halved(uint32_t rt);

/*
 * Flag-gated store hooks.
 * Emitted as `hook(cpu, ea, val);` in place of the original sb.
 */
/** @brief Stores the m_WasDrawn iteration-start byte and resets both moby verdict flags to pass.
 *
 * Sets psx_runtime_native_cone_pass and psx_runtime_native_dist_pass to 1, so
 * a billboard or extended-radius moby that bypasses the cone test does not
 * inherit the previous moby's verdict.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the m_WasDrawn byte.
 * @param val The value stored, truncated to a byte. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr, uint32_t val);
/** @brief Stores the m_WasDrawn byte only when both canonical cull verdicts passed.
 *
 * The byte is gameplay-readable, so the store happens only when
 * psx_runtime_native_cone_pass and psx_runtime_native_dist_pass are both
 * nonzero.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the m_WasDrawn byte.
 * @param val The value stored, truncated to a byte. */
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
/** @brief Stores the particle drawn flag (m_03) only when the canonical screen cull passed.
 *
 * The store happens only when psx_runtime_native_part_pass is nonzero.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the flag byte.
 * @param val The value stored, truncated to a byte. */
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val);

/*
 * Particle and sparkle canonical-SX cull hooks.
 * Set psx_runtime_native_part_pass from the canonical SX side channel and
 * return the canonical SX.
 */
/** @brief Records the canonical 0 < SX < 512 particle verdict from the sx1 channel and returns a - b.
 *
 * For particle classes that read the screen X between two RTPS operations.
 * Stores whether openpete_canonical_sx1_q16 >> 16 lies strictly between 0
 * and 512 in psx_runtime_native_part_pass.
 * @param a The minuend.
 * @param b The subtrahend.
 * @return a - b. */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b);
/** @brief Records the canonical 0 < SX < 512 particle verdict from the sx2 channel and returns a - b.
 *
 * For particle classes with a single RTPS. Same verdict as
 * psx_runtime_part_cull_sx1, read from openpete_canonical_sx2_q16.
 * @param a The minuend.
 * @param b The subtrahend.
 * @return a - b. */
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b);
/** @brief Records the canonical particle verdict of a 4-vertex strip and returns v << 16.
 *
 * Stores in psx_runtime_native_part_pass whether any of the canonical SX
 * values openpete_canonical_sx0_q16, sx1_q16 and sx2_q16 (each >> 16; FIFO
 * vertices 1, 2 and 3) lies strictly between 0 and 512.
 * @param v The register value the asm shifts left by 16.
 * @return v << 16. */
int32_t psx_runtime_part_cull_4vert(int32_t v);
/** @brief Returns the canonical screen X as the value the sparkle render-cull clip tests.
 *
 * Substitutes $a1 (the sll of $a3 by 16) so the left and right kill verdict
 * uses openpete_canonical_sx2_q16 and the g_Sparkles allocation stays
 * identical across aspects.
 * @return (openpete_canonical_sx2_q16 >> 16) << 16. */
int32_t psx_runtime_sparkle_canon_sx(void);

/**
 * @brief Per-iteration flag for the particle renderer, the screen-space
 *        X-cull counterpart of psx_runtime_native_cone_pass.
 *
 * The part_cull hooks at the `sub _, _, $s6` sites record the canonical
 * 4:3 verdict from the canonical-SX FIFO below; the substituted sb at
 * 0x80057720 / 0x80057A10 / 0x80057B20 / 0x80057CDC writes `m_03 = 1`
 * only when the canonical cull admitted the particle.
 */
extern uint8_t psx_runtime_native_part_pass;

/**
 * @name Canonical SX FIFO
 * Un-widened SX values in q16, stored by RTPS after each projection. A
 * 3-deep FIFO matching push_sxy: sx2 is the latest, sx0 the oldest. The
 * particle cull substitutions test the canonical 4:3 verdict against
 * these fractional values, which the int16 SX after mfc2 cannot resolve
 * at the 1-pixel boundary. Canonical 4:3 verdict: `0 < sx_q16 <
 * 0x02000000`.
 * @{ */
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;
/** @} */

/**
 * @brief Diagnostic hook for the moby cone cull at 0x80022C0C.
 *
 * Active under `OPENPETE_DIAG_CONE_SLOT` (optional FROM/TO frame range).
 * Receives |MAC1|, MAC3 and the native cone-pass verdict.
 */
void openpete_diag_cone_dump(void* cpu,
                             int32_t mac1_abs,
                             int32_t mac3,
                             uint8_t native_pass);

/** @brief Log one terrain vertex's camera-free world position and the camera to a binary file.
 *
 * The recompiler appends the call after `lw $a0, 4($s1)` at 0x80025A34 in
 * func_800258F0 (r_environment.s), the per-vertex transform of the terrain
 * renderer. Each call writes a 48-byte record: the host yield counter
 * `openpete_frame`, the vertex pointer, the packed world words, the scaled
 * camera position, and the 9 shorts of g_Camera.m_ViewMatrix read from
 * 0x80076DD0 + 0x14. An offline tool recomputes M * (world - campos) from
 * the records to check a reprojection of terrain against the game's output.
 *
 * The first call latches the environment: OPENPETE_ENVVERT_LOG names the
 * output file (unset: every call returns at once), OPENPETE_ENVVERT_LOG_SKIP
 * the counter value before which calls are ignored, and
 * OPENPETE_ENVVERT_LOG_FRAMES the number of distinct counter values logged
 * (default 80) before the file is closed. The file opens with an 8-word
 * header ("EVRT", version 1, record size 48). The probe reads PSX RAM and
 * writes none.
 * @param cpu CPUState of the running guest.
 * @param w0 First packed world word (guest $v0): high half paired with s7, low half with t8.
 * @param w1 Second packed world word (guest $a0): high half paired with t9.
 * @param s1 Guest vertex pointer.
 * @param s7 Camera position word at +0x28 of the camera block, shifted right by 4.
 * @param t8 Camera position word at +0x2C, shifted right by 4.
 * @param t9 Camera position word at +0x30, shifted right by 4. */
void openpete_envvert_probe(void* cpu, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9);

/**
 * @name PSX RAM virtualisation: the region table
 * Registers a host buffer at any PSX vaddr above the canonical 2 MB. The
 * cold-path accessors (`cpu->read_X` / `write_X`, the pointer resolvers,
 * savestate scans) consult it. Inline RAM accesses emitted by the
 * recompiler are not rerouted; consumers redirect those through
 * instruction substitutions at the lui+addiu sites.
 *
 * Regions are presentation-only by contract: gameplay code never
 * addresses a registered range. Debug builds with
 * `OPENPETE_DEBUG_REGION_GUARD` assert if it does.
 * @{ */

/**
 * @brief Register a region.
 * @param psx_base     A KSEG0/KSEG1/KUSEG vaddr whose physical address
 *                     (`& 0x1FFFFFFF`) is >= 0x200000.
 * @param size         Region length in bytes.
 * @param host_buffer  Stays owned by the caller.
 * @return 0 on success, non-zero on a zero size or NULL buffer, on overlap
 *         with canonical RAM, the scratchpad or another region, or when
 *         the 16-entry table is full.
 */
int      psx_runtime_register_region(uint32_t psx_base,
                                     uint32_t size,
                                     void*    host_buffer);

/** @brief Unregister by base; no-op when nothing matches.
 *
 * The base is compared as a physical address, so KSEG0 and KSEG1 spellings
 * of the same base match.
 * @param psx_base Base address the region was registered with. */
void     psx_runtime_unregister_region(uint32_t psx_base);

/**
 * @brief Resolve a PSX address inside a registered region.
 * @param psx_addr       The PSX address to resolve.
 * @param out_host       If non-NULL, receives the host pointer for that address.
 * @param out_remaining  If non-NULL, receives the bytes until the region's end.
 * @return 1 when @p psx_addr falls in a registered region, 0 otherwise.
 */
int      psx_runtime_resolve_region(uint32_t psx_addr,
                                    void**   out_host,
                                    uint32_t* out_remaining);

/**
 * @brief Inverse of psx_runtime_resolve_region().
 * @param host       A host pointer.
 * @param out_vaddr  If non-NULL, receives the KSEG0 vaddr.
 * @return 1 when the host pointer falls inside a registered region's
 *         buffer, 0 otherwise.
 */
int      psx_runtime_region_host_to_vaddr(const void* host,
                                          uint32_t* out_vaddr);

/**
 * @brief Returns `addr & 0x1FFFFF`.
 *
 * Debug builds additionally assert the address is not inside a
 * registered region, marking the call site as one that only ever sees
 * canonical RAM. The guard exists only in a build defining
 * OPENPETE_DEBUG_REGION_GUARD; it prints the offending address and calls
 * abort().
 * @param addr A PSX address.
 * @return The address masked to the 2 MB main RAM offset.
 */
uint32_t psx_runtime_canonicalize(uint32_t addr);

/** @brief Number of registered regions; zero lets cold-path resolvers skip the region check.
 *
 * @return The count of regions currently registered. */
uint32_t psx_runtime_region_count(void);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_INTERNAL_H */
