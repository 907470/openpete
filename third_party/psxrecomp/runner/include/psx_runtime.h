#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIPS R3000A register file + GTE coprocessor registers + memory callbacks.
 * This struct is the only coupling between generated code and the runtime. */
typedef struct CPUState {
    uint32_t zero, at, v0, v1, a0, a1, a2, a3;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;
    uint32_t t8, t9, k0, k1, gp, sp, fp, ra;
    uint32_t pc, hi, lo;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];

    uint32_t (*read_word)(uint32_t addr);
    void     (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void     (*write_half)(uint32_t addr, uint16_t value);
    uint8_t  (*read_byte)(uint32_t addr);
    void     (*write_byte)(uint32_t addr, uint8_t value);
    uint32_t (*lwl)(uint32_t addr, uint32_t rt_value);
    uint32_t (*lwr)(uint32_t addr, uint32_t rt_value);
    void     (*swl)(uint32_t addr, uint32_t rt_value);
    void     (*swr)(uint32_t addr, uint32_t rt_value);
} CPUState;

/* Initialize a CPUState and install memory callbacks. Call before running any
 * recompiled function. */
void psx_runtime_init(CPUState* cpu);

/* Load bytes into PS1 RAM at the given virtual address. */
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* MMIO tracing — logs all hardware register accesses to mmio_trace.log.
 * Call with enable=1 after psx_runtime_init(). */
void psx_mmio_trace_enable(int enable);

/* Install signal handlers for SIGSEGV/SIGABRT to dump CPU state on crash. */
void psx_install_crash_handler(void);

/* Dynamic call dispatch — handles BIOS calls and jump-register targets.
 * Logs any address not yet implemented. Never crashes on unknown calls. */
void call_by_address(CPUState* cpu, uint32_t addr);

/* Override gate — emitted by code_generator at the top of every function.
 * Returns 1 if intercepted (body skipped), 0 if normal execution continues.
 * Default: always returns 0. Add real implementations in runtime.c when needed. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/* SYSCALL handler — emitted for SYSCALL instructions. */
void psx_syscall(CPUState* cpu, uint32_t code);

/* GTE coprocessor — implemented in gte.cpp. */
void gte_execute(CPUState* cpu, uint32_t cmd);

/* Calling-instruction PC for the most recently issued GTE command,
 * written by recompiled code immediately before each gte_execute()
 * call. Read by the render cache emit to tag each cached vertex with
 * the function that produced it — used for per-prim-class
 * discrimination (cyclorama vs world-moby vs Spyro mesh vs HUD).
 * 0 = unknown (pre-regen generated code or interpreter path). */
extern uint32_t openpete_gte_caller_pc;

/* GTE register transfer helpers (for overlay interpreter) */
uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);

/* Direct access to PS1 RAM (2MB) and scratchpad (1KB). Used by automation scripts. */
uint8_t* psx_get_ram(void);
uint8_t* psx_get_scratch(void);

/* Frame-gated GP1 display command tracker (called from gpu_write_gp1). */
void diag_track_gp1(uint32_t cmd);

/* g_SonyImage / D_8006FCF4 base redirect. Default 0x8006FCF4 (canonical
 * PSX address; substitution is identity). When (any future
 * consumer of the env-renderer scratch pool) registers a relocated
 * region, this knob is set to the new base; the recompiler's lui+addiu
 * pair substitution emits code that resolves to the new base + offset
 * for any (lui $X, 0x8007 / addiu $X, $X, lo) pair landing in the pool
 * range [0x8006FCF4, 0x80072894). See code_generator translate_lui. */
extern uint32_t psx_runtime_g_sony_image_base;

/* --no-lod feature flag (native-renderer display policy; the native
 * feeders key detail selection on it). No generated code reads it —
 * the game-side producer stack was deleted 2026-07-06. Defined in
 * core/features/no_lod.c. */
extern int openpete_no_lod;

/* Per-iteration scratch flag set inside each render-bucket walker's
 * cone test (sub at 0x8001F30C / 0x80020AB8, addi at 0x80022C0C) to 1
 * iff the moby passed the native 4:3 cone test. The substituted
 * `sb $a0, 0x51($fp)` sites (0x8001F4CC / 0x80020C7C / 0x80022D98) read
 * this flag and only set m_WasDrawn=1 when it's set — the m_WasDrawn
 * render→gameplay channel's verdict gate. */
extern uint8_t psx_runtime_native_cone_pass;

/* Companion to native_cone_pass for the DISTANCE half of the moby cull.
 * The dist-gate hooks at the box-cull comparison PCs (shaded path
 * 0x80022B50/B58/B6C/B74/B84/B8C; analogous PCs in the regular/specular
 * paths) clear native_dist_pass when the cull comparand rejects; the
 * m_WasDrawn=1 stores gate on (native_cone_pass && native_dist_pass).
 * Reset to 1 at the iteration's m_WasDrawn clear (0x80022B28 etc.). */
extern uint8_t psx_runtime_native_dist_pass;

/* Append-call hooks the recompiler emits AFTER the canonical box-cull
 * add/sub at the distance-channel PCs (channel
 * no_lod_moby_distance_cull_verdict), passing the dest-register comparand:
 *   _lower : box-cull lower-bound (cull if v - delta <= 0)
 *   _upper : box-cull upper-bound + post-MVMVA proj-Z (cull if v + delta >= 0)
 * Each clears native_dist_pass when the box cull rejects. Defined in
 * games/spyro-1/features/render_decouple.c. This is the named-hook form of
 * what used to be an inline `if (...) native_dist_pass = 0;` in the
 * recompiler — the verdict math now lives host-side, not in third_party. */
void psx_runtime_dist_gate_lower(int32_t v);
void psx_runtime_dist_gate_upper(int32_t v);

/* Moby cone-X cull ReplaceCall hooks (channel m_WasDrawn cone-compute sites).
 * Record the canonical 4:3 cone verdict in native_cone_pass and return the
 * canonical comparand. _permoby(mac1,a2,mac3,a3) for regular/specular;
 * _shaded(mac1,mac3) for the fixed-102/77 shaded path. Defined in
 * games/spyro-1/features/render_decouple.c. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3);
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3);

/* Flag-gated store hooks (void ReplaceCall — emitted as `hook(cpu, ea, val);`
 * replacing the original sb). Defined in games/spyro-1/features/
 * render_decouple.c. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val);

/* Particle / sparkle canonical-SX cull hooks (defined in
 * games/spyro-1/features/render_decouple.c). Set native_part_pass from the
 * canonical SX side channel / return the canonical SX, replacing inline
 * recompiler math. */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_4vert(int32_t v);
int32_t psx_runtime_sparkle_canon_sx(void);

/* Per-iteration scratch flag for the particle renderer (r_particles.s).
 * Same shape as psx_runtime_native_cone_pass but for the screen-space
 * X-cull test. The part_cull hooks at the `sub _, _, $s6` sites record
 * the canonical 4:3 verdict from the canonical-SX q16 FIFO below; the
 * substituted sb at 0x80057720 / 0x80057A10 / 0x80057B20 / 0x80057CDC
 * reads this flag and only writes m_03=1 when the canonical cull
 * admitted the particle — the particle_drawn_flag channel's verdict
 * gate. */
extern uint8_t psx_runtime_native_part_pass;

/* Canonical (un-widened) SX values in q16, stashed by gte.cpp's RTPS
 * after each projection. 3-deep FIFO matching push_sxy semantics —
 * sx2 is the latest, sx1 the previous, sx0 the oldest. Used by
 * r_particles.s cull substitutions to test canonical 4:3 verdicts
 * directly against fractional-precision SX values (avoids the 1-pixel
 * boundary ambiguity that plain int16 SX after mfc2 has). Canonical
 * 4:3 verdict: 0 < canonical_sx_q16 < 0x02000000. */
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;

/* Diagnostic hook for the moby cone-cull at 0x80022C0C. Implemented in
 * games/spyro-1/game_extras_impl.c. Activated by env var
 * OPENPETE_DIAG_CONE_SLOT (with optional FROM/TO frame range). The
 * substitution at 0x80022C0C calls this with |MAC1|, MAC3, and the
 * computed native cone-pass verdict so we can compare across aspects. */
struct CPUState;
void openpete_diag_cone_dump(void* cpu,
                             int32_t mac1_abs,
                             int32_t mac3,
                             uint8_t native_pass);

/* §7.5 terrain-reproject validation probe. Recompiler emits a call at
 * r_environment.s 0x80025A34 with the camera-free packed world vertex
 * (w0=X<<16|Y, w1=Z<<16|flags), vert ptr, scaled camera position.
 * Read-only; gated by OPENPETE_ENVVERT_LOG. Impl game_extras_impl.c. */
void openpete_envvert_probe(void* cpu, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9);

/* --- PSX RAM virtualization (presentation-side region table) ------------
 *
 * Lets renderer-side code register a host buffer at any PSX vaddr above
 * the canonical 2 MB. Consulted by cold-path memory accessors
 * (cpu->read_X / write_X, mem_ptr, openpete_psx_to_host_ptr, savestate
 * scans). NOT auto-rerouted into recompiler-emitted inline g_ram[]
 * accesses — consumers redirect those via psxrecomp instruction
 * substitutions at the relevant lui+addiu sites. See
 * .claude/plans/openpete-ram-virtualization.md.
 *
 * Determinism: regions are presentation-only by contract. Gameplay code
 * never addresses a registered range. Debug builds with
 * OPENPETE_DEBUG_REGION_GUARD assert if they do. */

/* psx_base must be a KSEG0/KSEG1/KUSEG vaddr whose phys (& 0x1FFFFFFF)
 * is >= 0x200000 — i.e. above the canonical 2 MB. Returns 0 on success;
 * non-zero on overlap with canonical RAM, scratchpad, or another region,
 * or if the table is full. host_buffer ownership stays with the caller. */
int      psx_runtime_register_region(uint32_t psx_base,
                                     uint32_t size,
                                     void*    host_buffer);

/* Unregister by base. No-op if no region matches. */
void     psx_runtime_unregister_region(uint32_t psx_base);

/* Returns 1 if psx_addr falls in a registered region. If so, *out_host
 * (when non-NULL) gets the host pointer for that exact address, and
 * *out_remaining (when non-NULL) gets bytes-until-region-end. */
int      psx_runtime_resolve_region(uint32_t psx_addr,
                                    void**   out_host,
                                    uint32_t* out_remaining);

/* Debug-build guard for "this site is canonical-only" 2 MB-mask sites.
 * In release: returns addr & 0x1FFFFF. In debug: same, plus asserts the
 * address does not fall in any registered region's band — i.e. that
 * the caller is on a path that should only ever see canonical RAM. */
uint32_t psx_runtime_canonicalize(uint32_t addr);

/* Number of currently-registered regions. Zero means cold-path
 * resolvers can skip the region check entirely. Tests + tooling. */
uint32_t psx_runtime_region_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
