#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

/* Runtime interface between the recompiled game code and the engine.
 * Mods use CPUState (the register file an override receives); everything
 * after it is engine linkage.
 *
 * Only <stdint.h> is included: the generated code uses fixed-width
 * integers alone, so the recompiled module builds against the compiler's
 * freestanding headers on a machine with no libc development package. */
#include <stdint.h>

/* Data symbols shared across the exe/module boundary. On Windows a module
 * writing an exe-exported data symbol would otherwise depend on MinGW
 * pseudo-relocation; marking the symbol dllimport in module builds routes
 * the access through the import table. OPENPETE_RECOMP_MODULE_BUILD is
 * defined only for module translation units. */
#if defined(_WIN32) && defined(OPENPETE_RECOMP_MODULE_BUILD)
#  define OPENPETE_ABI_DATA __declspec(dllimport)
#else
#  define OPENPETE_ABI_DATA
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* MIPS R3000A register file, GTE registers and memory callbacks. The only
 * coupling between generated code and the runtime. Overrides receive one
 * of these: arguments in a0..a3, return value in v0. */
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

/* Initialise a CPUState and install the memory callbacks. Call before
 * running any recompiled function. */
void psx_runtime_init(CPUState* cpu);

/* Copy bytes into PSX RAM at a virtual address. */
void psx_runtime_load(uint32_t addr, const uint8_t* data, uint32_t size);

/* Log every hardware register access to mmio_trace.log. Call with
 * enable=1 after psx_runtime_init(). */
void psx_mmio_trace_enable(int enable);

/* Install SIGSEGV/SIGABRT handlers that dump the CPU state. */
void psx_install_crash_handler(void);

/* Dispatch a call by address: BIOS calls and jump-register targets. An
 * unknown address is logged, never fatal. */
void call_by_address(CPUState* cpu, uint32_t addr);

/* Override gate emitted at the top of every recompiled function. Returns
 * 1 when the call was intercepted and the body is skipped. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr);

/* SYSCALL instruction handler. */
void psx_syscall(CPUState* cpu, uint32_t code);

/* GTE coprocessor command. */
void gte_execute(CPUState* cpu, uint32_t cmd);

/* PC of the instruction that issued the most recent GTE command, written
 * by recompiled code before each gte_execute() call. The render cache
 * tags each vertex with it to classify the producing function. 0 =
 * unknown. */
OPENPETE_ABI_DATA extern uint32_t openpete_gte_caller_pc;

/* GTE register transfers for the overlay interpreter. */
uint32_t gte_read_data(CPUState* cpu, uint8_t reg);
uint32_t gte_read_ctrl(CPUState* cpu, uint8_t reg);
void     gte_write_data(CPUState* cpu, uint8_t reg, uint32_t val);
void     gte_write_ctrl(CPUState* cpu, uint8_t reg, uint32_t val);

/* Direct access to PSX RAM (2 MB) and the scratchpad (1 KB). */
uint8_t* psx_get_ram(void);
uint8_t* psx_get_scratch(void);

/* Frame-gated GP1 display command tracker, called from gpu_write_gp1. */
void diag_track_gp1(uint32_t cmd);

/* Base of the g_SonyImage / D_8006FCF4 scratch pool. Default 0x8006FCF4,
 * the canonical address. A consumer that relocates the pool sets the new
 * base; the recompiler's lui+addiu substitution for every pair landing in
 * [0x8006FCF4, 0x80072894) resolves to base + offset. */
OPENPETE_ABI_DATA extern uint32_t psx_runtime_g_sony_image_base;

/* --no-lod display policy flag, read by the native feeders' detail
 * selection. No generated code reads it. Defined in core/features/no_lod.c. */
extern int openpete_no_lod;

/* Per-iteration flag set inside each render-bucket walker's cone test
 * (sub at 0x8001F30C / 0x80020AB8, addi at 0x80022C0C) to 1 when the moby
 * passed the canonical 4:3 cone test. The substituted `sb $a0, 0x51($fp)`
 * sites (0x8001F4CC / 0x80020C7C / 0x80022D98) set m_WasDrawn=1 only when
 * it is set. */
extern uint8_t psx_runtime_native_cone_pass;

/* Distance half of the moby cull. The dist-gate hooks at the box-cull
 * comparison PCs (shaded path 0x80022B50/B58/B6C/B74/B84/B8C and the
 * corresponding regular/specular PCs) clear it when the comparand
 * rejects; the m_WasDrawn=1 stores gate on (native_cone_pass &&
 * native_dist_pass). Reset to 1 at the iteration's m_WasDrawn clear
 * (0x80022B28 and siblings). */
extern uint8_t psx_runtime_native_dist_pass;

/* Append-call hooks emitted after the canonical box-cull add/sub at the
 * distance-cull PCs, receiving the destination-register comparand:
 *   _lower : lower bound (cull if v - delta <= 0)
 *   _upper : upper bound and post-MVMVA projected Z (cull if v + delta >= 0)
 * Each clears native_dist_pass when the box cull rejects. Defined in
 * games/spyro-1/features/render_decouple.c. */
void psx_runtime_dist_gate_lower(int32_t v);
void psx_runtime_dist_gate_upper(int32_t v);

/* Moby cone-X cull ReplaceCall hooks at the m_WasDrawn cone-compute
 * sites. Record the canonical 4:3 verdict in native_cone_pass and return
 * the canonical comparand. _permoby(mac1,a2,mac3,a3) for the regular and
 * specular paths; _shaded(mac1,mac3) for the fixed-102/77 shaded path.
 * Defined in games/spyro-1/features/render_decouple.c. */
int32_t psx_runtime_cone_widen_permoby(int32_t mac1_abs, int32_t a2,
                                       int32_t mac3, int32_t a3);
int32_t psx_runtime_cone_widen_shaded(int32_t mac1_abs, int32_t mac3);

/* Moby vertex-stream field ReplaceCall hooks, substituting the
 * `srl rd, rt, 11|10` of the game's field-decode idiom. The source
 * register holds the frame word << 11; the hook undoes the shift and
 * resolves the 21-bit field through the stream-handle table
 * (core/runtime/vstream.c). Stock values pass through bit-identical. */
uint32_t psx_runtime_vstream_moby(uint32_t rt);
uint32_t psx_runtime_vstream_halved(uint32_t rt);

/* Flag-gated store hooks emitted as `hook(cpu, ea, val);` in place of the
 * original sb. Defined in games/spyro-1/features/render_decouple.c. */
void psx_runtime_mwasdrawn_clear_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_mwasdrawn_store(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_runtime_part_drawn_store(CPUState* cpu, uint32_t addr, uint32_t val);

/* Particle and sparkle canonical-SX cull hooks. Set native_part_pass from
 * the canonical SX side channel and return the canonical SX. Defined in
 * games/spyro-1/features/render_decouple.c. */
int32_t psx_runtime_part_cull_sx1(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_sx2(int32_t a, int32_t b);
int32_t psx_runtime_part_cull_4vert(int32_t v);
int32_t psx_runtime_sparkle_canon_sx(void);

/* Per-iteration flag for the particle renderer (r_particles.s), the
 * screen-space X-cull counterpart of native_cone_pass. The part_cull
 * hooks at the `sub _, _, $s6` sites record the canonical 4:3 verdict
 * from the canonical-SX FIFO below; the substituted sb at 0x80057720 /
 * 0x80057A10 / 0x80057B20 / 0x80057CDC writes m_03=1 only when the
 * canonical cull admitted the particle. */
extern uint8_t psx_runtime_native_part_pass;

/* Canonical (un-widened) SX values in q16, stored by RTPS after each
 * projection. A 3-deep FIFO matching push_sxy: sx2 is the latest, sx0 the
 * oldest. The r_particles.s cull substitutions test the canonical 4:3
 * verdict against these fractional values, which the int16 SX after mfc2
 * cannot resolve at the 1-pixel boundary. Canonical 4:3 verdict:
 * 0 < sx_q16 < 0x02000000. */
extern int64_t openpete_canonical_sx0_q16;
extern int64_t openpete_canonical_sx1_q16;
extern int64_t openpete_canonical_sx2_q16;

/* Diagnostic hook for the moby cone cull at 0x80022C0C, active under
 * OPENPETE_DIAG_CONE_SLOT (optional FROM/TO frame range). Receives |MAC1|,
 * MAC3 and the native cone-pass verdict. Defined in
 * games/spyro-1/game_extras_impl.c. */
struct CPUState;
void openpete_diag_cone_dump(void* cpu,
                             int32_t mac1_abs,
                             int32_t mac3,
                             uint8_t native_pass);

/* Terrain-reproject probe at r_environment.s 0x80025A34, receiving the
 * camera-free packed world vertex (w0 = X<<16|Y, w1 = Z<<16|flags), the
 * vertex pointer and the scaled camera position. Read-only; active under
 * OPENPETE_ENVVERT_LOG. Defined in games/spyro-1/game_extras_impl.c. */
void openpete_envvert_probe(void* cpu, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9);

/* --- PSX RAM virtualisation: the region table --------------------------
 *
 * Registers a host buffer at any PSX vaddr above the canonical 2 MB. The
 * cold-path accessors (cpu->read_X / write_X, mem_ptr,
 * openpete_psx_to_host_ptr, savestate scans) consult it. Inline g_ram[]
 * accesses emitted by the recompiler are not rerouted; consumers redirect
 * those through instruction substitutions at the lui+addiu sites.
 *
 * Regions are presentation-only by contract: gameplay code never
 * addresses a registered range. Debug builds with
 * OPENPETE_DEBUG_REGION_GUARD assert if it does. */

/* psx_base is a KSEG0/KSEG1/KUSEG vaddr whose physical address
 * (& 0x1FFFFFFF) is >= 0x200000. Returns 0 on success, non-zero on
 * overlap with canonical RAM, the scratchpad or another region, or when
 * the table is full. The caller keeps ownership of host_buffer. */
int      psx_runtime_register_region(uint32_t psx_base,
                                     uint32_t size,
                                     void*    host_buffer);

/* Unregister by base; no-op when nothing matches. */
void     psx_runtime_unregister_region(uint32_t psx_base);

/* Returns 1 when psx_addr falls in a registered region, setting *out_host
 * (if non-NULL) to the host pointer for that address and *out_remaining
 * (if non-NULL) to the bytes until the region's end. */
int      psx_runtime_resolve_region(uint32_t psx_addr,
                                    void**   out_host,
                                    uint32_t* out_remaining);

/* Inverse of psx_runtime_resolve_region: returns 1 and sets *out_vaddr
 * (if non-NULL) to the KSEG0 vaddr when the host pointer falls inside a
 * registered region's buffer, 0 otherwise. */
int      psx_runtime_region_host_to_vaddr(const void* host,
                                          uint32_t* out_vaddr);

/* Returns addr & 0x1FFFFF. Debug builds additionally assert the address
 * is not inside a registered region, marking the call site as one that
 * only ever sees canonical RAM. */
uint32_t psx_runtime_canonicalize(uint32_t addr);

/* Number of registered regions; zero lets cold-path resolvers skip the
 * region check. */
uint32_t psx_runtime_region_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
