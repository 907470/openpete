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

/** @brief Initialise a CPUState and install the memory callbacks. Call before running any recompiled function. */
void psx_runtime_init(CPUState* cpu);

/** @brief Copy bytes into PSX RAM at a virtual address. */
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size);

/** @brief Log every hardware register access to mmio_trace.log. Call with @p enable = 1 after psx_runtime_init(). */
void psx_mmio_trace_enable(int enable);

/** @brief Install SIGSEGV/SIGABRT handlers that dump the CPU state. */
void psx_install_crash_handler(void);

/** @brief Dispatch a call by address: BIOS calls and jump-register targets. An unknown address is logged, never fatal. */
void call_by_address(CPUState* cpu, uint32_t addr);

/**
 * @brief Override gate emitted at the top of every recompiled function.
 * @return 1 when the call was intercepted and the body is skipped.
 */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/** @brief SYSCALL instruction handler. */
void psx_syscall(CPUState* cpu, uint32_t code);

/** @brief GTE coprocessor command. */
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

/** @brief Direct access to PSX RAM (2 MB). */
uint8_t* psx_get_ram(void);
/** @brief Direct access to the scratchpad (1 KB). */
uint8_t* psx_get_scratch(void);

/** @brief Frame-gated GP1 display command tracker, called from the GP1 write path. */
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

/**
 * @name Distance-cull hooks
 * Append-call hooks emitted after the canonical box-cull add/sub at the
 * distance-cull PCs, receiving the destination-register comparand. Each
 * clears psx_runtime_native_dist_pass when the box cull rejects.
 * @{ */
/** @brief Lower bound: cull if `v - delta <= 0`. */
void psx_runtime_dist_gate_lower(int32_t v);
/** @brief Upper bound and post-MVMVA projected Z: cull if `v + delta >= 0`. */
void psx_runtime_dist_gate_upper(int32_t v);
/** @} */

/**
 * @name Moby cone-X cull hooks
 * ReplaceCall hooks at the m_WasDrawn cone-compute sites. Record the
 * canonical 4:3 verdict in psx_runtime_native_cone_pass and return the
 * canonical comparand.
 * @{ */
/** @brief Regular and specular paths. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3);
/** @brief The fixed-102/77 shaded path. */
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3);
/** @} */

/**
 * @name Moby vertex-stream field hooks
 * ReplaceCall hooks substituting the `srl rd, rt, 11|10` of the game's
 * field-decode idiom. The source register holds the frame word << 11; the
 * hook undoes the shift and resolves the 21-bit field through the
 * stream-handle table. Stock values pass through bit-identical.
 * @{ */
uint32_t psx_runtime_vstream_moby(uint32_t rt);
uint32_t psx_runtime_vstream_halved(uint32_t rt);
/** @} */

/**
 * @name Flag-gated store hooks
 * Emitted as `hook(cpu, ea, val);` in place of the original sb.
 * @{ */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
/** @} */

/**
 * @name Particle and sparkle canonical-SX cull hooks
 * Set psx_runtime_native_part_pass from the canonical SX side channel and
 * return the canonical SX.
 * @{ */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_4vert(int32_t v);
int32_t psx_runtime_sparkle_canon_sx(void);
/** @} */

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

/**
 * @brief Terrain-reproject probe at 0x80025A34.
 *
 * Receives the camera-free packed world vertex (`w0 = X<<16|Y`,
 * `w1 = Z<<16|flags`), the vertex pointer and the scaled camera position.
 * Read-only; active under `OPENPETE_ENVVERT_LOG`.
 */
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
 * @param host_buffer  Stays owned by the caller.
 * @return 0 on success, non-zero on overlap with canonical RAM, the
 *         scratchpad or another region, or when the table is full.
 */
int      psx_runtime_register_region(uint32_t psx_base,
                                     uint32_t size,
                                     void*    host_buffer);

/** @brief Unregister by base; no-op when nothing matches. */
void     psx_runtime_unregister_region(uint32_t psx_base);

/**
 * @brief Resolve a PSX address inside a registered region.
 * @param out_host       If non-NULL, receives the host pointer for that address.
 * @param out_remaining  If non-NULL, receives the bytes until the region's end.
 * @return 1 when @p psx_addr falls in a registered region, 0 otherwise.
 */
int      psx_runtime_resolve_region(uint32_t psx_addr,
                                    void**   out_host,
                                    uint32_t* out_remaining);

/**
 * @brief Inverse of psx_runtime_resolve_region().
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
 * canonical RAM.
 */
uint32_t psx_runtime_canonicalize(uint32_t addr);

/** @brief Number of registered regions; zero lets cold-path resolvers skip the region check. */
uint32_t psx_runtime_region_count(void);
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_INTERNAL_H */
