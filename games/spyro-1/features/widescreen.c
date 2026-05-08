// games/spyro-1/features/widescreen.c — Spyro 1's per-vaddr widescreen
// dispatch arms.
//
// Phase 2.1b of notes/plans/openpete-feature-modules.md. Verbatim moves
// of the five widescreen-specific arms previously inline in
// game_extras_impl.c::psx_override_dispatch_game; no logic changes.
// Each PSX vaddr override is a `static int on_<addr>_<purpose>` next to
// its math/commentary, and the dispatch table at the bottom is the
// authoritative list. The module is registered via
// games/spyro-1/feature_list.c; runtime walking happens in
// core/runtime/feature_registry.c.
//
// The `apply_options` slot is left NULL because Hor+ has two-stage
// derivation (pre/post PsyX_Initialise); core/features/widescreen.c
// owns those, called explicitly from core/runtime/run.c. Only the
// per-vaddr dispatch flows through the registry.

#include "feature_registry.h"
#include "openpete_psx.h"
#include "openpete_ui_moby.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void func_8001ED5C(CPUState* cpu);
extern void func_800258F0(CPUState* cpu);
extern void func_80022A2C(CPUState* cpu);

extern float    openpete_disp_w_factor;
extern unsigned openpete_frame;
extern int      openpete_render_pass_active;
extern int32_t  psx_runtime_moby_cone_extra_q12;
extern int32_t  openpete_moby_cone_extra_q12_render;
extern uint32_t psx_runtime_env_cell_x_bound;
extern uint32_t openpete_env_cell_x_bound_render;
extern uint32_t psx_runtime_sxy_sat_bound;
extern uint32_t openpete_sxy_sat_bound_render;

/* env-renderer instrumentation. Counts writes done by func_800258F0 to
 * help diagnose 16:9 residual: are wider-FOV cells producing more prim
 * traffic, or is the renderer doing the same work as 4:3? */
static struct {
    void (*orig_w)(uint32_t, uint32_t);
    void (*orig_h)(uint32_t, uint16_t);
    void (*orig_b)(uint32_t, uint8_t);
    CPUState* cpu;
    uint32_t writes_total;
    uint32_t writes_ram_low;     /* < 0x80020000 (scratch + early data) */
    uint32_t writes_sbss;        /* 0x80075000..0x800A0000 (g_WorldOT etc) */
    uint32_t writes_overlay;     /* >= 0x80100000 (level data) */
    uint32_t writes_dma;         /* GPU/DMA control regs (>= 0x1F000000) */
    uint32_t worldot_ptr_before;
    uint32_t worldot_ptr_after;
    int active;
} g_env_trace = {0};

static void env_trace_classify(uint32_t addr) {
    g_env_trace.writes_total++;
    if (addr >= 0x1F000000u && addr < 0x80000000u) g_env_trace.writes_dma++;
    else if (addr >= 0x80100000u)                  g_env_trace.writes_overlay++;
    else if (addr >= 0x80075000u)                  g_env_trace.writes_sbss++;
    else if (addr >= 0x80000000u)                  g_env_trace.writes_ram_low++;
}
static void env_trace_w(uint32_t a, uint32_t v) {
    env_trace_classify(a); g_env_trace.orig_w(a, v);
}
static void env_trace_h(uint32_t a, uint16_t v) {
    env_trace_classify(a); g_env_trace.orig_h(a, v);
}
static void env_trace_b(uint32_t a, uint8_t v) {
    env_trace_classify(a); g_env_trace.orig_b(a, v);
}

/* GamestateDraw bracket: confine Hor+ widening to the render pass so
 * GamestateUpdate's GTE work sees native 4:3 state and stays as
 * close to bit-identical between aspect modes as possible.
 *
 * Reduction observed (20000-frame demo): without bracket 13327
 * frames diverge with peak Δ²=6.04 B; with bracket 7824 / 88.6 M
 * (~68× peak shrink). The remaining drift comes from render-side
 * mutations of moby visibility/pointer tables in 0x70000-0x71E00,
 * which feed the next GamestateUpdate. Documented in
 * notes/plans/openpete-widescreen-fov.md. */
static int on_8001ED5C_render_bracket(CPUState* cpu, uint32_t addr) {
    (void)addr;
    static int s_in_render_pass = 0;
    if (openpete_disp_w_factor != 1.0f
        && openpete_disp_w_factor > 0.0f
        && !s_in_render_pass) {
        s_in_render_pass = 1;
        openpete_render_pass_active = 1;
        psx_runtime_moby_cone_extra_q12 = openpete_moby_cone_extra_q12_render;
        if (openpete_env_cell_x_bound_render != 0u)
            psx_runtime_env_cell_x_bound = openpete_env_cell_x_bound_render;
        if (openpete_sxy_sat_bound_render != 0u)
            psx_runtime_sxy_sat_bound = openpete_sxy_sat_bound_render;
        uint32_t saved_gte_data[32], saved_gte_ctrl[32];
        memcpy(saved_gte_data, cpu->gte_data, sizeof(saved_gte_data));
        memcpy(saved_gte_ctrl, cpu->gte_ctrl, sizeof(saved_gte_ctrl));
        func_8001ED5C(cpu);
        memcpy(cpu->gte_data, saved_gte_data, sizeof(saved_gte_data));
        memcpy(cpu->gte_ctrl, saved_gte_ctrl, sizeof(saved_gte_ctrl));
        psx_runtime_moby_cone_extra_q12 = 0;
        psx_runtime_env_cell_x_bound = 0x100u;
        psx_runtime_sxy_sat_bound = 0x400u;
        openpete_render_pass_active = 0;
        s_in_render_pass = 0;
        return 1;
    }
    return 0;
}

/* Issue 4 (titlescreen UI centring) — the titlescreen overlay's
 * `TitlescreenDrawSprite(int pX, int pY, int pSprite, int pColorId)`
 * (at PSX vaddr 0x8007CD38) places every titlescreen sprite (Spyro
 * logo, "PRESS START", memcard window, options text) using PSX-X
 * coords in the native 4:3 [0..512] frame. With Hor+ widening the
 * host ortho is left-anchored at 0..disp.w*factor, so a sprite
 * authored to be 4:3-centred (e.g. logo at X=128 with W=256, centre
 * at 256) maps to host-px (256 / (512*factor)) * window_w =
 * window_w / (2*factor) — the leftmost 4:3 inset's centre, NOT the
 * host window's centre.
 *
 * Pre-shift cpu->a0 (= pX) by +(factor-1)*256 PSX-X so the sprite's
 * effective centre lands at host_w/2. Round half (+0.5) because
 * (factor-1)*256 is fractional at 16:9 (= 85.333). The recompiled
 * func_titlescreen_8007CD38 runs with the shifted a0 — no body
 * replacement needed — so we return 0 (fall through). Static guard
 * is conservative: the function is non-recursive, but the flag
 * prevents double-shifting if the sprite path ever calls itself
 * via a future code path.
 *
 * Render-side only (writes go to the OT / POLY_FT4 buffer that
 * DrawOTag consumes); no gameplay-readable RAM is touched, so the
 * aspect_determinism gate stays green. */
static int on_8007CD38_titlescreen_sprite(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (openpete_disp_w_factor > 1.0f) {
        int shift = (int)((openpete_disp_w_factor - 1.0f) * 256.0f + 0.5f);
        cpu->a0 = (uint32_t)((int32_t)cpu->a0 + shift);
    }
    return 0;  /* let the recompiled body run with shifted pX */
}

/* --aspect: bypass the level's per-occlusion-group visibility list at
 * widescreen and walk every sector in g_Environment.m_SectorPointer[]
 * instead. The list was authored for 4:3, so off-axis cells aren't
 * candidates regardless of cull-bound widening. The renderer already
 * supports this — passing pOcclusionGroup=-1 skips the list and walks
 * all sectors (see r_environment.s:80-93 / src/environment.c:34-42).
 * Native callers use that path only when the camera is *outside* any
 * known group, with a shorter m_CullingDistance (0x14000) to compensate
 * for the lost filter. We restore the in-group distance (0x28000) so
 * far cells aren't dropped, then call the renderer body.
 *
 * Risk: prim-buffer overflow. The OT pool at D_8006FCF4 is sized for
 * 4:3 candidate counts; walking 2-3× more cells could exceed it. So
 * far Artisans renders fine; if a denser level breaks, fall back to
 * the cull-only path or shrink m_CullingDistance. */
static int on_800258F0_env_renderer(CPUState* cpu, uint32_t addr) {
    (void)addr;
    static int s_in_env_renderer = 0;
    if (!(openpete_disp_w_factor != 1.0f
          && openpete_disp_w_factor > 0.0f
          && !s_in_env_renderer)) {
        return 0;
    }
    s_in_env_renderer = 1;
    /* Capture caller's a0 (occlusion-group id or -1) BEFORE we force
     * the all-walk path. The canonical_mask below replays the cell-
     * list iteration the 4:3 native call would have done, which is
     * group-filtered when a0 >= 0. Without this, our mask iterates
     * the master sector table and admits cells that lie in the
     * canonical cone but aren't in the canonical occlusion group —
     * surfaced at frame 3495 of title autoplay (5 cells leaked). */
    int32_t original_a0 = (int32_t)cpu->a0;
    cpu->a0 = (uint32_t)-1;        /* walk all sectors */

    /* Boost m_CullingDistance to the in-group value (0x28000) — the
     * caller (env.c func_8002B9CC) sets a shorter 0x14000 / 0x1c000
     * when going through the all-walk path because the original
     * design only used all-walk as a fallback. With wide aspects,
     * far-off-axis cells are needed; restore the full distance. */
    const uint32_t cd_addr = 0x800785A8u + 0x28u;
    uint32_t saved_cd = cpu->read_word(cd_addr);
    if (saved_cd > 0u && saved_cd < 0x28000u) {
        cpu->write_word(cd_addr, 0x28000u);
    }

    /* Cone-cull widening. r_environment.s loads g_Camera.m_ViewMatrix
     * (offset 0x14, 5 i32s) into CP2C[R11..R33] and runs MVMVA against
     * each cell's IR-vec to derive |IR1|/IR3 for the cone test
     * (slope 0.75 ≈ atan(256/341) = native 4:3). Scaling row 0
     * (`m_ViewMatrix.m[0][0..2]`) by 1/disp_w_factor shrinks IR1 by
     * the same factor so the cone admits cells in a wider horizontal
     * arc. Saved + restored around the call so other consumers of
     * m_ViewMatrix (collision, AI) aren't affected. */
    const uint32_t mvm_addr = 0x80078A58u - 0x80000000u;  /* TODO from manifest */
    (void)mvm_addr;  /* m_ViewMatrix lives at g_Camera + 0x14 */
    const uint32_t vm_base = 0x80076DD0u + 0x14u;
    /* Save the full 20-byte SHORTMATRIX (9 shorts + pad). The
     * native renderer body may compose row 0 with rows 1/2 via
     * MVMVA chains and write back the composed matrix; restoring
     * only row 0 leaves widened residue in rows 1/2 that gameplay's
     * next CameraUpdateMatrices reads. See RAM-diff under
     * notes/plans/openpete-widescreen-fov.md. */
    uint8_t saved_view_matrix[20];
    memcpy(saved_view_matrix,
           openpete_psx_get_ram() + (vm_base & 0x1FFFFFFFu), 20);

    /* Pre-compute the canonical 4:3 per-cell visibility mask used to
     * de-leak the class_skip_flags channel. r_environment.s writes
     * D_800771C8[cell_idx] = 0xFF for every cell that passes its
     * cone-cull (PC 0x80025B50). Gameplay (func_800521C0) reads that
     * table next frame to clear m_WasDrawn=0 on mobys whose owning
     * sector wasn't rendered.
     *
     * The matrix-row-0 widening below admits more cells in 16:9 →
     * widened D_800771C8 → wrong gameplay verdict. We replicate the
     * three cull conditions in C against the canonical (un-widened)
     * matrix here, then AND the widened-renderer's output with this
     * mask after func_800258F0 returns.
     *
     * The cull conditions (per asm at 0x80025A98..0x80025B04):
     *   1. Rz + bbox_E > 0   (front face in front of camera)
     *   2. 4*(|Rx| - 25E/32) - 3*(Rz + 9E/16) < 0   (X-cone)
     *   3. 32*(|Ry| - 7E/8) - 17*(Rz + 7E/16) < 0  (Y-cone)
     * Where (Rx,Ry,Rz) = canonical_view_matrix * cell_relative_to_cam,
     * with sf=1 (>>12) and signed int16 saturation.
     *
     * The asm has additional back-corner tests at 0x80025B0C..B40, but
     * those only adjust the queue-push value $a1; visibility is
     * always written 0xFF after the front-face cone passes. */
    uint8_t canonical_mask[256];
    memset(canonical_mask, 0, sizeof(canonical_mask));
    {
        int16_t M[3][3];
        const int16_t* vm_words = (const int16_t*)saved_view_matrix;
        M[0][0] = vm_words[0]; M[0][1] = vm_words[1]; M[0][2] = vm_words[2];
        M[1][0] = vm_words[3]; M[1][1] = vm_words[4]; M[1][2] = vm_words[5];
        M[2][0] = vm_words[6]; M[2][1] = vm_words[7]; M[2][2] = vm_words[8];

        const uint32_t cam_pos_addr = 0x80076DD0u + 0x28u;  /* g_Camera.m_Position */
        int32_t cam_x4 = (int32_t)cpu->read_word(cam_pos_addr + 0) >> 4;
        int32_t cam_y4 = (int32_t)cpu->read_word(cam_pos_addr + 4) >> 4;
        int32_t cam_z4 = (int32_t)cpu->read_word(cam_pos_addr + 8) >> 4;

        const uint32_t env_addr = 0x800785A8u;
        uint32_t cell_list_base = cpu->read_word(env_addr + 0);
        uint32_t cell_count     = cpu->read_word(env_addr + 4);
        if (cell_count > 256) cell_count = 256;

        /* Build the canonical iteration set. r_environment.s branches
         * on $ra (= caller's a0): bltz → all-walk env+0x0/+0x4; else
         * walk a per-group byte-index list at env+0x8 → groups[a0],
         * terminated by 0xFF. */
        uint8_t indices[256];
        uint32_t n_indices = 0;
        if (original_a0 < 0) {
            for (uint32_t i = 0; i < cell_count; i++) {
                indices[n_indices++] = (uint8_t)i;
            }
        } else {
            uint32_t groups_table = cpu->read_word(env_addr + 8);
            if (groups_table) {
                uint32_t group_list = cpu->read_word(
                    groups_table + (uint32_t)original_a0 * 4);
                if (group_list) {
                    uint8_t* gram = openpete_psx_get_ram();
                    for (uint32_t k = 0; k < 256; k++) {
                        uint8_t b = gram[(group_list + k) & 0x1FFFFFFFu];
                        if (b == 0xFF) break;
                        indices[n_indices++] = b;
                    }
                }
            }
        }

        for (uint32_t k = 0; k < n_indices; k++) {
            uint32_t i = (uint32_t)indices[k];
            uint32_t cell_ptr = cpu->read_word(cell_list_base + i * 4);
            if (cell_ptr == 0) continue;
            uint32_t v0w = cpu->read_word(cell_ptr + 0);
            uint32_t a0w = cpu->read_word(cell_ptr + 4);
            int16_t cell_v0_lo = (int16_t)(v0w & 0xFFFF);
            int16_t cell_v0_hi = (int16_t)((v0w >> 16) & 0xFFFF);
            int16_t cell_a0_hi = (int16_t)((a0w >> 16) & 0xFFFF);
            int32_t bbox_E = (int32_t)(a0w & 0x1FFF);

            int16_t V[3];
            V[0] = (int16_t)((int32_t)cam_y4 - (int32_t)cell_v0_lo);
            V[1] = (int16_t)((int32_t)cam_z4 - (int32_t)cell_a0_hi);
            V[2] = (int16_t)((int32_t)cell_v0_hi - (int32_t)cam_x4);

            int64_t mac1 = (int64_t)M[0][0]*V[0] + (int64_t)M[0][1]*V[1] + (int64_t)M[0][2]*V[2];
            int64_t mac2 = (int64_t)M[1][0]*V[0] + (int64_t)M[1][1]*V[1] + (int64_t)M[1][2]*V[2];
            int64_t mac3 = (int64_t)M[2][0]*V[0] + (int64_t)M[2][1]*V[1] + (int64_t)M[2][2]*V[2];
            mac1 >>= 12; mac2 >>= 12; mac3 >>= 12;
            int32_t Rx = (mac1 > 32767) ? 32767 : (mac1 < -32768 ? -32768 : (int32_t)mac1);
            int32_t Ry = (mac2 > 32767) ? 32767 : (mac2 < -32768 ? -32768 : (int32_t)mac2);
            int32_t Rz = (mac3 > 32767) ? 32767 : (mac3 < -32768 ? -32768 : (int32_t)mac3);

            if (Rz + bbox_E <= 0) continue;
            int32_t abs_Rx = (Rx < 0) ? -Rx : Rx;
            int32_t abs_Ry = (Ry < 0) ? -Ry : Ry;
            /* X half-cone radius = 25E/32. */
            int32_t t3 = (bbox_E >> 1) + (bbox_E >> 2) + (bbox_E >> 5);
            /* Front-face X-cone Z adjust = 19E/32. The asm computes
             *   t4 = E/2; t5 = E/16; t4 += t5            (9E/16)
             *   t5 = E/32
             *   ; delay slot of `blez at, cull` (behind-cam test)
             *   t4 += t5                                 (19E/32)
             * Missing the delay-slot add is what threw the C-side
             * canonical cull off — see the original investigation
             * in notes/plans/openpete-determinism-enforcement.md. */
            int32_t t4 = (bbox_E >> 1) + (bbox_E >> 4) + (bbox_E >> 5);
            /* Y half-cone radius = 7E/8. */
            int32_t t5 = bbox_E - (bbox_E >> 3);
            /* Front-face Y-cone Z adjust = 7E/16. */
            int32_t t6 = (bbox_E >> 1) - (bbox_E >> 4);
            if (4*(abs_Rx - t3) - 3*(Rz + t4) >= 0) continue;
            if (32*(abs_Ry - t5) - 17*(Rz + t6) >= 0) continue;
            canonical_mask[i] = 1;
        }
    }

    int16_t row0[3];
    for (int i = 0; i < 3; i++) {
        int16_t orig = (int16_t)cpu->read_half(vm_base + (uint32_t)(i * 2));
        float scaled = (float)orig / openpete_disp_w_factor;
        if (scaled >  32767.0f) scaled =  32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        row0[i] = (int16_t)scaled;
        cpu->write_half(vm_base + (uint32_t)(i * 2), (uint16_t)row0[i]);
    }

    /* Instrumentation: wrap write fns to count what env emits. */
    int env_trace_on = (getenv("OPENPETE_ENV_TRACE") != NULL);
    if (env_trace_on) {
        g_env_trace.orig_w = cpu->write_word;
        g_env_trace.orig_h = cpu->write_half;
        g_env_trace.orig_b = cpu->write_byte;
        g_env_trace.cpu    = cpu;
        g_env_trace.writes_total = 0;
        g_env_trace.writes_ram_low = 0;
        g_env_trace.writes_sbss = 0;
        g_env_trace.writes_overlay = 0;
        g_env_trace.writes_dma = 0;
        g_env_trace.worldot_ptr_before = cpu->read_word(0x80075820u);
        cpu->write_word = env_trace_w;
        cpu->write_half = env_trace_h;
        cpu->write_byte = env_trace_b;
        g_env_trace.active = 1;
    }

    func_800258F0(cpu);

    if (env_trace_on) {
        cpu->write_word = g_env_trace.orig_w;
        cpu->write_half = g_env_trace.orig_h;
        cpu->write_byte = g_env_trace.orig_b;
        g_env_trace.active = 0;
        g_env_trace.worldot_ptr_after = cpu->read_word(0x80075820u);
        if ((openpete_frame % 60u) == 0u) {
            fprintf(stderr,
                    "[env-trace f=%u factor=%.3f] writes total=%u "
                    "low=%u sbss=%u ovl=%u dma=%u  WorldOT %08X->%08X\n",
                    openpete_frame, openpete_disp_w_factor,
                    g_env_trace.writes_total,
                    g_env_trace.writes_ram_low,
                    g_env_trace.writes_sbss,
                    g_env_trace.writes_overlay,
                    g_env_trace.writes_dma,
                    g_env_trace.worldot_ptr_before,
                    g_env_trace.worldot_ptr_after);
        }
    }

    /* Restore the full m_ViewMatrix so any composed-matrix
     * writebacks inside func_800258F0 don't leak into gameplay. */
    memcpy(openpete_psx_get_ram() + (vm_base & 0x1FFFFFFFu),
           saved_view_matrix, 20);

    /* AND D_800771C8 with the canonical mask: any byte the widened
     * cull set to 0xFF that wouldn't have passed the canonical 4:3
     * cone gets cleared back to 0. Result: gameplay's func_800521C0
     * reads exactly the canonical visibility verdict next frame,
     * while the render queues + emitted prims (already populated by
     * the widened pass) cover the full Hor+ visible area. */
    const uint32_t class_skip_addr = 0x800771C8u;
    uint8_t* ram = openpete_psx_get_ram();
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t a = (class_skip_addr + i) & 0x1FFFFFFFu;
        if (ram[a] != 0u && canonical_mask[i] == 0u) {
            ram[a] = 0u;
        }
    }

    cpu->write_word(cd_addr, saved_cd);
    s_in_env_renderer = 0;
    return 1;
}

/* Issue 5/6 follow-up — text-moby UI range. Spyro's text-emit
 * functions (func_80017FE4 / func_800181AC) allocate character
 * mobys in the polybuf's top half by decrementing g_HudMobys.
 * After all text is emitted, func_80018880 copies the mobys
 * onto m_ShadedMobys and RESETS g_HudMobys back to the polybuf
 * top — so by the time func_80022A2C iterates and projects, the
 * "low water mark" is gone. We can't recover it.
 *
 * Instead, register the FULL polybuf range as UI. Polybuf is
 * 0x1C000 bytes, top at D_80075780 (vaddr 0x80075780, stable
 * through the frame). Anything inside is either a 2D prim
 * (POLY_F4 / POLY_FT4 / LINE_G2 — never reaches GTE so the
 * UI tag is a no-op) or a text-character moby (the thing we
 * want to classify as UI). Safe by construction.
 *
 * Set the range at func_80022A2C entry so it's current for
 * this frame's projection pass. Returns 0 (fall-through) so the
 * recompiled body still runs after the range is registered. */
static int on_80022A2C_text_moby_range(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (openpete_disp_w_factor > 1.0f) {
        uint32_t poly_top = cpu->read_word(0x80075780u);
        if (poly_top >= 0x1C000u) {
            openpete_ui_moby_set_text_range(poly_top - 0x1C000u, poly_top);
        } else {
            openpete_ui_moby_set_text_range(0, 0);
        }
    }
    return 0;  /* fall through so the recompiled body runs */
}

static const feature_dispatch_entry_t widescreen_dispatch[] = {
    { 0x8001ED5Cu, on_8001ED5C_render_bracket        },
    { 0x8007CD38u, on_8007CD38_titlescreen_sprite    },
    { 0x800258F0u, on_800258F0_env_renderer          },
    { 0x80022A2Cu, on_80022A2C_text_moby_range       },
    { 0u, NULL },
};

const feature_module_t widescreen_module = {
    .name           = "widescreen",
    .apply_options  = NULL,  /* two-stage; called explicitly from run.c */
    .dispatch_table = widescreen_dispatch,
};
