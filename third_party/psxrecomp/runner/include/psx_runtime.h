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

/* r_environment.s queue-head sub-offsets within g_SonyImage (or its
 * relocated mirror). Default values match the canonical splash struct:
 *   HP  queue head at +0x1C00 (m_WorldNearQueued)
 *   mid queue head at +0x1E00 (HP-walk deferred bucket)
 *   LP  queue head at +0x2000 (m_WorldFarQueued)
 * --no-lod swings these to fresh sub-offsets past the canonical 0x31A0
 * struct so the queues can grow past 256 entries without colliding
 * with each other or with gameplay-readable bytes (m_ShadedMobys at
 * +0x2400, g_LevelTransGems at +0x2800).
 *
 * Substituted at PC-keyed addi sites in r_environment.s; see
 * code_generator.cpp case 0x08. Identity at default. */
extern uint32_t psx_runtime_env_hp_queue_offset;
extern uint32_t psx_runtime_env_mid_queue_offset;
extern uint32_t psx_runtime_env_lp_queue_offset;

/* g_SonyImage / D_8006FCF4 base redirect. Default 0x8006FCF4 (canonical
 * PSX address; substitution is identity). When --no-lod (or any future
 * consumer of the env-renderer scratch pool) registers a relocated
 * region, this knob is set to the new base; the recompiler's lui+addiu
 * pair substitution emits code that resolves to the new base + offset
 * for any (lui $X, 0x8007 / addiu $X, $X, lo) pair landing in the pool
 * range [0x8006FCF4, 0x80072894). See code_generator translate_lui. */
extern uint32_t psx_runtime_g_sony_image_base;

/* Spyro r_moby.s LP-face-load offset. Default 0x1C (canonical PSX:
 * AnimationHeader.m_LpFaces lives at +0x1C, m_Faces at +0x14). When
 * --no-lod is active, swung to 0x14 so the LP-path face load reads
 * the high-poly face list instead. Substituted at the two
 * `lw $fp, 0x1C($ra)` sites at PC 0x8001FE9C and 0x80021674
 * (LP-band branch in r_moby.s). Identity at default. */
extern uint32_t psx_runtime_moby_lp_face_offset;

/* Spyro r_moby.s regular/specular moby visible-queue relocation (Phase
 * 8.5). The cull/queue-build func_8001F158 writes each cull-passing moby
 * into a fixed-capacity output queue at g_SonyImage(0x8006FCF4)+0x1600,
 * stride 0x38, hard limit base+0x2200 (~54 entries). When the queue
 * fills the cull loop TERMINATES (early-exit), dropping every later moby
 * in the list. Under --no-lod, m_Distance saturation makes far mobys
 * pass the cull, overflowing the queue before late-list canonical mobys
 * are reached → they're dropped → m_WasDrawn never set → AI despawn →
 * g_MobyAllocPtr/g_Particles diverge from the oracle (the @2468 leak).
 *
 * Default 0x1600 (head) / 0x2200 (limit) == canonical (substitution is
 * identity). --no-lod swings them to a relocated, grown host region so
 * the queue never overflows and no canonical moby is dropped — drawing
 * the extra distant mobys without perturbing any gameplay-readable byte
 * (the 0x80071xxx queue is audited render-side, gameplay_state_regions
 * .toml:160). The consumer func_8001F798 walks to NULL so only the head
 * needs relocating there. Substituted at:
 *   head  : F158 0x8001F19C (addi $fp,$at,0x1600), F798 0x8001F7E4
 *   limit : F158 0x8001F730 (addi $v0,$v0,0x2200)
 * See .claude/plans/openpete-disable-lods.md Phase 8.5. */
extern uint32_t psx_runtime_moby_reg_queue_offset;       /* default 0x1600 */
extern uint32_t psx_runtime_moby_reg_queue_limit_offset; /* default 0x2200 */

/* --no-lod feature flag. Read by the recompiler-emitted lw substitution
 * at PC 0x80026214 (r_environment.s m_LodDistance load) to inject an
 * always-false LOD predicate without mutating g_Environment.m_LodDistance.
 * Defined in core/runtime/fiber.c. */
extern int openpete_no_lod;

/* Debug knob: when non-zero, force the env-renderer's
 * `g_SkipLowPolyWorld` read at PC 0x800261E0 to return 1, which makes
 * the env-renderer skip its LP-walk entirely (`bnez .L80026788`).
 * Affects ONLY the env-renderer, not r_moby.s / r_cyclorama.s where
 * the same flag has different semantics. Set via OPENPETE_DBG_KILL_LP=1
 * env var. Used to visually identify what cells the LP-walk renders. */
extern int psx_runtime_dbg_kill_env_lp_walk;

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

/* Companion X-bias scale to the slope widening above. The cone test in
 * all three moby paths reduces to
 *   admit if  |MAC1| < X_bias + slope*(MAC3 + Z_bias)
 * where slope=3/4 native, and X_bias is 102 (shaded) or the per-moby
 * `a2` value (regular/specular). The slope widening alone leaves a
 * near-Z slice with native cone width because the X-bias term doesn't
 * scale with aspect — and the `mac3p<0` clamp collapses the slope
 * widening to zero in that strip too. This knob multiplies the X-bias
 * by (1 + bias_scale_q12/4096), restoring aspect-proportional X-
 * headroom even at MAC3+Z_bias ≤ 0.
 *
 * Q12 fixed-point of (factor-1). Default 0 = bias unchanged. For 16:9
 * (factor 1.333) set 0.333*4096 = 1365. Render-pass-only — swapped in
 * around the GamestateDraw call, zeroed outside, so gameplay-side
 * func_80022A2C callers (moby ticking) still see the canonical bias. */
extern int32_t psx_runtime_moby_cone_bias_scale_q12;

/* Phase 7.2: near-Z clamp soften. The cone widening clamps mac3p to >=
 * this floor before multiplying by extra_q12; native floor is 0 (the
 * proj_cone_extra_signed_clamp fix) which collapses slope widening to
 * zero whenever MAC3+Z_bias is negative — i.e. mobys near or behind
 * the camera plane fall back to native cone width. Setting a small
 * positive floor preserves a partial slope widening in that strip,
 * giving aspect-proportional headroom even at near-Z. Default 0 =
 * preserve existing clamp behaviour. Render-pass-only; staged by
 * widescreen.c. */
extern int32_t psx_runtime_moby_cone_mac3p_floor;

/* r_environment.s func_800258F0 per-vertex lateral bound cull at
 * 0x80026A2C..0x80026A50. After RTPS the asm tests
 *   |MAC1| < 256 && |MAC2| < 256
 * via two `addi $a2, +0x100` / `addi $a2, -0x200` chains (and same
 * for $a3). The X-axis bound (0x80026A34 / 0x80026A3C) is what limits
 * the lateral world-extent admitted to per-vertex projection — at
 * 32:9 (factor 2.667) the visible horizontal half-extent is 2.667×
 * native, so the native ±256 admit rejects vertices whose post-MVMVA
 * camera-space X exceeds 256, leaving the right side of the rendered
 * frame uncovered (no env strips emitted there).
 *
 * Single knob in MAC-1 units. Default 0x100 (= +256, native pre-add).
 * The `+0x100` site uses `psx_runtime_env_cell_x_bound` directly; the
 * `-0x200` site subtracts 2× the bound. Y-axis (`$a3`, 0x80026A44 /
 * 0x80026A4C) is left native — vertical FOV doesn't change. Render-
 * pass-only: armed in the GamestateDraw pre/post hook so gameplay-
 * side GTE work outside the render pass sees the canonical native
 * bound. */
extern uint32_t psx_runtime_env_cell_x_bound;

/* GTE push_sxy SX/SY saturation bound (in pixels, magnitude). Native
 * PSX hardware clamps to ±0x400 (signed 11-bit), which cuts off
 * Hor+-widescreen vertices whose projected SX exceeds 0x400 — visible
 * as a hard right-edge cap at PSX-X 1023 regardless of how wide the
 * GL ortho is. Default 0x400 (native). Render-pass setup widens to
 * (uint32_t)(0x400 * thresh_factor) so 32:9 (factor 2.667) admits SX
 * up to ~2730. Reset to 0x400 outside render pass so gameplay GTE
 * reads see canonical SXY. */
extern uint32_t psx_runtime_sxy_sat_bound;

/* Per-iteration scratch flag set inside each render-bucket walker's
 * cone test (sub at 0x8001F30C / 0x80020AB8, addi at 0x80022C0C) to 1
 * iff the moby would have passed the *native 4:3* cone test, regardless
 * of the actual cone width used for emit. The substituted
 * `sb $a0, 0x51($fp)` sites (0x8001F4CC / 0x80020C7C / 0x80022D98) read
 * this flag and only set m_WasDrawn=1 when it's set. Effect: the
 * render→gameplay channel (m_WasDrawn) stays 4:3-equivalent while
 * widescreen rendering still admits + emits edge mobys. */
extern uint8_t psx_runtime_native_cone_pass;

/* Companion to native_cone_pass for the DISTANCE half of the moby cull
 * (the m_WasDrawn channel closure under --no-lod). The per-bucket walker
 * box-cull compares |delta/4| against (m_Distance<<8)+bias; --no-lod
 * saturates m_Distance so distant mobys pass and get m_WasDrawn=1, which
 * leaks into gameplay AI / rand(). We replay the canonical box-cull at
 * the six addu/subu comparison PCs (shaded path 0x80022B50/B58/B6C/B74/
 * B84/B8C; analogous PCs in the regular/specular paths) using
 * `mdist_thresh_delta` (= (saturated−canonical)·256, captured at the
 * m_Distance lhu) to recover the canonical threshold, and clear
 * native_dist_pass if the canonical cull would reject. The m_WasDrawn=1
 * stores gate on (native_cone_pass && native_dist_pass). Identity when
 * --no-lod is off (mdist_thresh_delta == 0). Reset to 1 at the
 * iteration's m_WasDrawn clear (0x80022B28 etc.). */
extern uint8_t psx_runtime_native_dist_pass;
extern int32_t psx_runtime_mdist_thresh_delta;

/* Append-call hooks the recompiler emits AFTER the canonical box-cull
 * add/sub at the distance-channel PCs (channel
 * no_lod_moby_distance_cull_verdict), passing the dest-register comparand:
 *   _lower : box-cull lower-bound (cull if v - delta <= 0)
 *   _upper : box-cull upper-bound + post-MVMVA proj-Z (cull if v + delta >= 0)
 * Each clears native_dist_pass when the canonical (un-saturated) 4:3 cull
 * would reject. Defined in core/features/widescreen.c. Identity when
 * --no-lod is off (mdist_thresh_delta == 0). This is the named-hook form of
 * what used to be an inline `if (...) native_dist_pass = 0;` in the
 * recompiler — the verdict math now lives host-side, not in third_party. */
void psx_runtime_dist_gate_lower(int32_t v);
void psx_runtime_dist_gate_upper(int32_t v);

/* Per-iteration scratch flag for the particle renderer (r_particles.s).
 * Same shape as psx_runtime_native_cone_pass but for the screen-space
 * X-cull test against $s6 (= psx_runtime_clip_x_threshold). Each
 * `sub _, _, $s6` site preceding a `sb $s2, -0x1D($ra)` (m_03 = "drawn")
 * records whether the comparand would also pass the *canonical* 4:3
 * threshold (0x02000000). The substituted sb at 0x80057720 / 0x80057A10 /
 * 0x80057B20 / 0x80057CDC reads this flag and only writes m_03=1 when
 * the canonical 4:3 cull would have admitted the particle — keeping
 * gameplay-readable bytes 4:3-equivalent while the widened cull still
 * admits + emits prims for visual coverage outside 4:3. */
extern uint8_t psx_runtime_native_part_pass;

/* Canonical 4:3 right-edge threshold expressed in *widened*-SX
 * coordinates. OFX widening shifts every projected SX uniformly by
 * `(factor - 1) * 256`, so a particle's SX in widened coords is
 * SX_widened = SX_native + delta_OFX. To replay the canonical cull
 * verdict against widened SX values, compare against
 * (0x200 + delta_OFX) << 16 instead of the bare 0x02000000. Set per
 * frame in run.c alongside psx_runtime_clip_x_threshold; defaults to
 * 0x02000000 (native 4:3, no shift). Read by the particle-cull
 * substitution at PCs 0x80057A04 / B14 / CB4 (and the 4-vertex
 * canonical OR at 0x800576EC). */
extern uint32_t psx_runtime_native_clip_x_in_widened_space;

/* Canonical 4:3 LEFT-edge threshold expressed in *widened*-SX
 * coordinates. The particle cull at 0x80057A00 (and friends) does
 * `blez (SX << 16), exit` — i.e. canonical-mode rejects when SX <= 0.
 * In widened coords SX_widened = SX_canonical + delta_OFX, so the
 * canonical-pass test in widened space is
 *   (SX_widened << 16) > (delta_OFX << 16).
 * This var holds (delta_OFX << 16); 0 means no widening (= native
 * 4:3 left edge at SX=0). Read by the same particle-cull substitution
 * at PCs 0x80057A04 / B14 / CB4 / F28 / 801C and the 4-vertex SLL OR
 * at 0x800576EC, AND-combined with the right-edge test. */
extern uint32_t psx_runtime_native_clip_x_low_in_widened_space;

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

/* Spyro 2D full-bleed primitive width clamp. Two render-side functions
 * emit POLY_F4s sized to the native 4:3 framebuffer width (PSX X = 0..512):
 *   - func_80018F30 (cutscene letterbox bars), li $s1, 0x200 at 0x80019004
 *   - func_800190D4 (fade-to-color overlay),   li $v1, 0x200 at 0x80019138
 * With Hor+ widening the host GL ortho widens to disp.w * factor, so the
 * 512-wide PSX prims map only to the leftmost 4:3 inset of the host window
 * — fades and cutscene bars leave the side bars unfilled. Substitute the
 * 512 immediate with this knob so openpete can widen the prim's right edge
 * to 512 * openpete_disp_w_factor under --aspect. The PsyCross ortho is
 * left-anchored (PSX x=0 -> host x=0), so widening the right edge is
 * sufficient. Default 512 = byte-identical 4:3 behaviour.
 *
 * Render-side only; no gameplay-readable RAM is touched (the writes go to
 * the OT/poly buffer that DrawOTag consumes). */
extern uint32_t psx_runtime_fullbleed_x_max;

/* Spyro double-buffered polygon-buffer slot size, in bytes. Spyro
 * allocates `0x1C000` per slot in 4BEF8.c:25-27 (matching decomp). The
 * end-cursor `D_80075780 = m_LowerPolyBuffer + 0x1C000` is recomputed
 * at three sites in gamestates/draw.c. The compiler emits `0x1C000`
 * via `lui $X, 0x1; ori $X, $X, 0xC000` (positive sites) or
 * `lui $X, 0xFFFE; ori $X, $X, 0x4000` (negated, in AllocateBuffers).
 *
 * The recompiler substitutes the ori at four PCs:
 *   0x8001A608 — func_8001A40C, draw.c:947 path
 *   0x8001C6AC — func_8001C694, draw.c:1561 path
 *   0x8001ED90 — func_8001ED5C, GamestateDraw db->m_PolyBuf path
 *   0x8005B73C — AllocateBuffers, default (non-reduced) path
 * with a runtime read of psx_runtime_polybuf_size (positive) or its
 * two's-complement negation (the AllocateBuffers site).
 *
 * Default 0x1C000 = byte-identical 4:3. Widescreen growth registers a
 * larger host-backed buffer via the RAM virtualization primitive and
 * sets this knob to the relocated buffer's size; see
 * .claude/plans/openpete-widescreen-fov.md and
 * .claude/plans/openpete-ram-virtualization.md. */
extern uint32_t psx_runtime_polybuf_size;

/* Spyro polybuf base override. Default 0 = sentinel ("use canonical
 * m_LowerPolyBuffer"). When non-zero (set by widescreen_polybuf_setup),
 * the recompiler substitutes this value at two LW PCs that read
 * m_LowerPolyBuffer for cursor-reset:
 *   0x8001A618 — func_8001A40C, draw.c:946 (pause-resume bracket)
 *   0x8001C6A0 — func_8001C694, draw.c:1559 (Gamestate 10 entry)
 * Without the substitution those resets re-anchor D_800757B0 at the
 * canonical buffer while D_80075780 (via polybuf_size) extends past
 * canonical_lower + 0x1C000, overrunning m_HigherPolyBuffer and the
 * OT chain — pause-resume hang in widened modes. The third LW site
 * (0x8001ED8C, GamestateDraw) reads `db->m_PolyBuf` which is already
 * redirected by the AllocateBuffers post-call hook, so it doesn't
 * need substitution. See core/features/widescreen.c. */
extern uint32_t psx_runtime_polybuf_lower;

/* Map a host-relocated poly-buffer address back to its canonical PSX
 * vaddr, preserving the within-slot byte offset. Identity when the
 * polybuf isn't relocated (4:3 default), so substituted sites stay
 * byte-identical.
 *
 * The per-frame write cursor (D_800757B0 / db->m_PolyBuf) is redirected
 * into the host-backed buffer for capacity. But gameplay-readable
 * pointers the game DERIVES from that cursor must keep the value real
 * hardware would hold — chiefly `g_HudMobys` (0x80075710, a hashed
 * gameplay-state region) and its sibling `D_800756FC`. The recompiler
 * wraps the stores at those derivation sites in this translator:
 *   0x8001D760 / 0x8001D768 — func_8001D718 (Gamestate 11), the
 *       `D_800756FC = g_HudMobys = D_800757B0 + 0x1BA00` reservation
 *       (draw.c:2051/2054); offset 0x1BA00 is preserved by the map.
 *   0x8001EDCC / 0x8001EDD4 — GamestateDraw frame-start reset
 *       (draw.c:2666/2667). Here the value carries the *grown* end-cursor
 *       size, so the substitution feeds `cursor_base = stored -
 *       psx_runtime_polybuf_size` to this map and re-adds the canonical
 *       0x1C000 slot stride.
 * Implemented in core/features/widescreen.c; canonical bases captured by
 * the AllocateBuffers post-call hook. See
 * .claude/plans/openpete-polybuf-host-redirect.md. */
uint32_t psx_runtime_polybuf_to_canon(uint32_t host_addr);

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

/* Per-moby UI tracking. The recompiler emits a call to
 * openpete_ui_moby_set_current at the per-moby loop iteration of
 * Spyro's func_80022A2C (0x80022B08) so gte_rtps_internal /
 * diagnostics can ask "is the moby being projected right now a UI
 * moby?". See core/include/openpete_ui_moby.h. */
void     openpete_ui_moby_set_current(uint32_t moby_addr);
int      openpete_ui_moby_is_current_ui(void);
uint32_t openpete_ui_moby_get_current(void);

/* Per-object render identity for transform-interp (SEPARATE from the
 * UI classifier above — see core/include/openpete_obj_id.h). Written by
 * every per-object render-loop iter (regular/specular via the moby
 * queue, shaded, Spyro, cyclorama); read only by the render-cache
 * obj_tag emit in gte_rtps_internal. Kept distinct from s_current so
 * non-shaded objects can never be mis-classified as UI mobys. */
void     openpete_obj_id_set(uint32_t obj_addr);
uint32_t openpete_obj_id_get(void);

/* Per-strip world-geometry identity (transform-interp §7.5-R3). The
 * recompiler emits openpete_world_geo_set_current at the dominant
 * terrain strip-entry of func_800258F0 (0x80026238, after the strip
 * base ptr is aligned) and openpete_world_geo_clear at the per-moby
 * loop (0x80022B08), so gte_rtps_internal can stamp a stable cross-
 * frame world batch tag. See core/include/openpete_world_geo.h. */
void     openpete_world_geo_set_current(uint32_t strip_addr);
void     openpete_world_geo_clear(void);
uint32_t openpete_world_geo_get_current(void);
uint16_t openpete_world_geo_next_vidx(void);

/* Per-instance moby vaddr FIFO. Recompiler emits push at the cull-pass-
 * confirmed PC in func_8001F158 (0x8001F268), and pop+set_current at
 * the regular/specular render-pass loop iters (0x8001F800 / 0x80020F9C)
 * so the matcher pairs each instance by real PSX vaddr instead of the
 * class-shared m_Props ptr that the render-list slot carries.
 * See core/include/openpete_moby_queue.h. */
void     openpete_moby_queue_push(uint32_t moby_vaddr);
uint32_t openpete_moby_queue_pop(void);
void     openpete_moby_queue_clear(void);

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
