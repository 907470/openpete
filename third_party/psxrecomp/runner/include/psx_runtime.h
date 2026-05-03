#ifndef PSX_RUNTIME_H
#define PSX_RUNTIME_H

#include <stdint.h>

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

/* Per-vertex screen-space clip mask used by handwritten asm cull
 * patterns (e.g. r_environment.s `andi $a1, $v0, 0xFE00`). The
 * mask tests bits 9..15 of SX from C2_SXY2, rejecting strips whose
 * vertices all lie outside [0, 512). For widescreen rendering
 * where SX legitimately exceeds 512, openpete widens this at boot
 * to e.g. 0xF800 (covers [-2048, 2047]). Default 0xFE00 preserves
 * native PSX behavior. Generated code with `andi imm=0xFE00`
 * substitutes this variable for the literal so the bound is
 * runtime-tunable without re-recompiling. */
extern uint32_t psx_runtime_clip_x_mask;

/* Cyclorama (r_cyclorama.s) right-edge clip threshold. Used at the
 * `sub $a, ($SX << 16), $reg` cull site to test SX < 512. Default
 * 0x02000000u (512 << 16) = native behavior; openpete widens to
 * 0x08000000u (2048 << 16) when --aspect is set. Substituted at
 * specific addresses by code_generator translate_lui — see comment
 * there. */
extern uint32_t psx_runtime_clip_x_threshold;

/* Spyro r_moby.s func_80022A2C inline cone-X cull. The cull tests
 *   bgez (4*(|MAC1|-102) - 3*(MAC3+77))
 * with slope 3/4 = tan(36.87°) = 4:3 horizontal half-FOV. For
 * widescreen the demanded horizontal cone is wider, so subtract an
 * additional `extra * (MAC3+77)` from a1 inside the addi at
 * 0x80022C0C — effectively raises z-multiplier from 3 to (3 + 4*extra),
 * widening the admit slope to 4 / (3 + 4*extra).
 *
 * Q12 fixed-point. Default 0 (slope unchanged, 4:3 native). For 16:9
 * (factor 1.333), set to 4096*(factor-1) = ~1366 → slope 4/4.33 ≈ 0.92,
 * admits ~43°. Tune via OPENPETE_MOBY_CONE_EXTRA env var. */
extern int32_t psx_runtime_moby_cone_extra_q12;

/* Per-iteration scratch flag set inside each render-bucket walker's
 * cone test (sub at 0x8001F30C / 0x80020AB8, addi at 0x80022C0C) to 1
 * iff the moby would have passed the *native 4:3* cone test, regardless
 * of the actual cone width used for emit. The substituted
 * `sb $a0, 0x51($fp)` sites (0x8001F4CC / 0x80020C7C / 0x80022D98) read
 * this flag and only set m_WasDrawn=1 when it's set. Effect: the
 * render→gameplay channel (m_WasDrawn) stays 4:3-equivalent while
 * widescreen rendering still admits + emits edge mobys. */
extern uint8_t psx_runtime_native_cone_pass;

/* Spyro r_cyclorama.s func_80050BD0 portal-cyclorama clip-rect X-upper
 * clamp. The dispatcher computes a per-portal screen-space clip rect
 * (D_8007AA00..0C) then clamps the upper-X corner with
 *   slti  $v0, $s4, 0x201   ; (s4 >= 513?)
 *   addiu $s4, $zero, 0x200 ; clamp to native 4:3 width 512
 * at two sites (0x800517F4 / 0x80051804 and 0x80051E88 / 0x80051E98).
 * With OFX widened, portals span past sx=512 but the clip rect can't,
 * so the cyclorama dome inside the portal opening is cut off on the
 * right and the portal shows BG color where the dome should fill.
 *
 * `_max` substitutes for the addiu imm (clamp ceiling); `_max_plus_1`
 * substitutes for the slti imm (test threshold = max + 1). Defaults
 * 512 / 513 preserve native 4:3 behavior. openpete widens these to
 * 512*thresh_factor / +1 when --aspect is set, capped to match the
 * env right-edge threshold so all clip surfaces line up. */
extern uint32_t psx_runtime_portal_clip_x_max;
extern uint32_t psx_runtime_portal_clip_x_max_plus_1;

#ifdef __cplusplus
}
#endif

#endif /* PSX_RUNTIME_H */
