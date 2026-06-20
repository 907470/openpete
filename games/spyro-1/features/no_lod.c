// games/spyro-1/features/no_lod.c — Spyro 1's --no-lod feature module.
//
// Owns every Spyro-specific knob that the recompiler-generated C reads
// when --no-lod is active, plus the relocated env-renderer scratch
// region and the polybuf-grow call. Registered as `no_lod_module` via
// games/spyro-1/feature_list.c.
//
// Sibling to games/spyro-1/features/widescreen.c. Verbatim moves from
// game_extras_impl.c + core/runtime/run.c; no logic changes.
//
// See notes/plans/openpete-disable-lods.md.

#include "feature_registry.h"
#include "features/no_lod.h"
#include "features/widescreen.h"
#include "openpete_world_geo.h"  /* openpete_world_geo_set_current (dbg hybrid) */
#include "psx_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* === Recompiler-facing knob storage =================================
 * Reads land in recompiler-generated C via psx_runtime.h externs; the
 * defining TU has to be linked in. Identity defaults keep --no-lod
 * off byte-identical. */

/* g_SonyImage / D_8006FCF4 base. Default == canonical PSX address; the
 * recompiler's lui+addiu pair substitution is then identity. --no-lod
 * (or any other env-renderer scratch-pool consumer) will swing this to
 * a relocated region in openpete-only vaddr space. */
uint32_t psx_runtime_g_sony_image_base = 0x8006FCF4u;

/* Queue-head sub-offsets — defaults match the canonical g_SonyImage
 * struct so the substituted addi sites in r_environment.s land on the
 * same bytes as before. --no-lod boot swings these past the canonical
 * 0x31A0 splash range. */
uint32_t psx_runtime_env_hp_queue_offset  = 0x1C00u;
uint32_t psx_runtime_env_mid_queue_offset = 0x1E00u;
uint32_t psx_runtime_env_lp_queue_offset  = 0x2000u;

/* r_moby.s LP-face-load offset. Canonical 0x1C reads m_LpFaces (low-poly
 * face list at AnimationHeader +0x1C). --no-lod swings to 0x14 so the
 * LP path reads m_Faces (high-poly) instead. Substituted at the two
 * `lw $fp, 0x1C($ra)` sites in r_moby.s (PCs 0x8001FE9C, 0x80021674). */
uint32_t psx_runtime_moby_lp_face_offset = 0x1Cu;

/* Phase 8.5: regular/specular moby visible-queue head + limit offsets.
 * Defaults match the canonical g_SonyImage layout (head 0x1600, limit
 * 0x2200) so the substituted addi sites are identity when --no-lod off.
 * --no-lod boot swings them into a relocated, grown host region (see
 * MOBY_QUEUE_* below) so the cull loop never overflows its output queue
 * and drops late-list canonical mobys. */
uint32_t psx_runtime_moby_reg_queue_offset       = 0x1600u;
uint32_t psx_runtime_moby_reg_queue_limit_offset = 0x2200u;

/* Debug knob: when non-zero, force the env-renderer's
 * `g_SkipLowPolyWorld` read at PC 0x800261E0 to return 1, which makes
 * the env-renderer skip its LP-walk entirely (`bnez .L80026788`).
 * Affects ONLY the env-renderer, not r_moby.s / r_cyclorama.s where
 * the same flag has different semantics. Wired from
 * OPENPETE_DBG_KILL_LP=1 env var inside this module's setup. */
int psx_runtime_dbg_kill_env_lp_walk = 0;

/* === Relocated region layout =========================================
 *
 *   region base = 0x80300000       (openpete-only vaddr, above 2 MB)
 *   region size = 0xC000 (48 KB)
 *
 * Each grown queue is 16 KB (4096 entries × 4 B), NULL-terminated:
 *   HP head  at region+0x0000 (vaddr NO_LOD_REGION_VADDR)
 *   mid head at region+0x4000
 *   LP head  at region+0x8000
 *
 * The 11 queue-head addi substitutions in r_environment.s compute
 *   $rt = canonical_pool_base + psx_runtime_env_{hp|mid|lp}_queue_offset
 * Setting the offset to (region - canonical_pool_base + slot_offset)
 * lands the queue heads at openpete-only vaddr space. We register a
 * single region at that base with enough size to cover all three
 * queues.
 *
 * The g_SonyImage base stays canonical (0x8006FCF4) so every base
 * reference (lui+addiu D_8006FCF4) resolves to canonical RAM and the
 * splash bitmap bytes stay pristine — only the grown queues are
 * relocated. */
#define NO_LOD_REGION_VADDR       0x80300000u
#define G_SONY_IMAGE_VADDR        0x8006FCF4u
#define NO_LOD_HP_QUEUE_OFFSET    (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x0000u)
#define NO_LOD_MID_QUEUE_OFFSET   (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x4000u)
#define NO_LOD_LP_QUEUE_OFFSET    (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x8000u)
#define NO_LOD_REGION_SIZE        0xC000u

static uint8_t s_no_lod_region[NO_LOD_REGION_SIZE];

/* Phase 8.5: relocated + grown regular/specular moby visible-queue.
 * Canonical capacity is ~54 entries (0x1600..0x2200, stride 0x38); under
 * --no-lod the saturated cull admits far more mobys, overflowing it and
 * early-exiting the cull loop. Grow to a fresh openpete-only region big
 * enough that the queue can never overflow (0x10000 / 0x38 ≈ 1170
 * entries — well above any level's live moby count). The queue head sits
 * at the region base; the limit is region_base + size, so the producer's
 * `(limit) - ($fp + 0x70) >= 0` check holds until the region is nearly
 * full. The producer marks the end with a PARTIAL terminator (word0=0
 * only) and, unlike canonical 4:3, nothing drains/zeroes this relocated
 * region per frame, so it must be re-zeroed before each producer pass
 * (no_lod_clear_moby_queue, the func_8001F158 pre-hook) — not just once
 * at setup — or the native walk reads a stale tail (the Sparx trail). */
#define MOBY_QUEUE_REGION_VADDR   0x80600000u
#define MOBY_QUEUE_REGION_SIZE    0x10000u
#define MOBY_REG_QUEUE_OFFSET       (MOBY_QUEUE_REGION_VADDR - G_SONY_IMAGE_VADDR)
#define MOBY_REG_QUEUE_LIMIT_OFFSET (MOBY_QUEUE_REGION_VADDR - G_SONY_IMAGE_VADDR + MOBY_QUEUE_REGION_SIZE)

static uint8_t s_moby_queue_region[MOBY_QUEUE_REGION_SIZE];

/* === Setup hook =====================================================
 *
 * Called by core/features/no_lod.c's apply_options entry when
 * --no-lod is set, after openpete_psx_boot has populated canonical
 * splash bytes and before the first dispatch into recompiled code.
 *
 * Avoids the channel-leak that the prior "redirect base" design
 * exposed: when the base was redirected, some paths cleared the
 * relocated region's m_Buf (substituted), but other paths read
 * canonical splash bytes. The substituted reads got zeros; canonical
 * reads got splash bytes; downstream interpreted differently. With
 * the base un-relocated, that whole class of bugs vanishes. */
static int no_lod_setup(void) {
    memset(s_no_lod_region, 0, sizeof(s_no_lod_region));

    int rc = psx_runtime_register_region(
        NO_LOD_REGION_VADDR, NO_LOD_REGION_SIZE, s_no_lod_region);
    if (rc != 0) {
        fprintf(stderr, "[no-lod] psx_runtime_register_region failed rc=%d "
                "(base=0x%08X size=0x%X)\n", rc, NO_LOD_REGION_VADDR,
                NO_LOD_REGION_SIZE);
        return 1;
    }

    psx_runtime_env_hp_queue_offset  = NO_LOD_HP_QUEUE_OFFSET;
    psx_runtime_env_mid_queue_offset = NO_LOD_MID_QUEUE_OFFSET;
    psx_runtime_env_lp_queue_offset  = NO_LOD_LP_QUEUE_OFFSET;
    /* Leave psx_runtime_g_sony_image_base at canonical (0x8006FCF4). */

    /* Phase 8.5: relocate + grow the regular/specular moby visible-queue
     * so the cull loop never overflows and drops late-list canonical
     * mobys (the @2468 g_MobyAllocPtr leak). The 0x80071xxx queue is
     * audited render-side state, so this perturbs no gameplay-readable
     * byte (gameplay_state_regions.toml:160). */
    memset(s_moby_queue_region, 0, sizeof(s_moby_queue_region));
    rc = psx_runtime_register_region(
        MOBY_QUEUE_REGION_VADDR, MOBY_QUEUE_REGION_SIZE, s_moby_queue_region);
    if (rc != 0) {
        fprintf(stderr, "[no-lod] moby-queue register_region failed rc=%d "
                "(base=0x%08X size=0x%X)\n", rc, MOBY_QUEUE_REGION_VADDR,
                MOBY_QUEUE_REGION_SIZE);
        return 1;
    }
    psx_runtime_moby_reg_queue_offset       = MOBY_REG_QUEUE_OFFSET;
    psx_runtime_moby_reg_queue_limit_offset = MOBY_REG_QUEUE_LIMIT_OFFSET;

    /* Phase 4: redirect r_moby.s LP-face load to read m_Faces (HP). */
    psx_runtime_moby_lp_face_offset = 0x14u;

    /* Phase 6 polybuf grow: with m_LodDistance saturated, every
     * visible cell routes through HP-walk, ~2-3× the canonical prim
     * count per frame even at 4:3 and ~4-5× at 16:9 inside dense
     * levels (e.g. Level 11 Dr Shemp). The canonical polybuf at
     * 0x1C000 (115 KB) overflows; the openpete renderer then either
     * drops prims or walks corrupted P_TAG lists. The widescreen path
     * only enables the host-redirected polybuf for thresh_factor > 1.5
     * (i.e. ≥ 21:9), leaving 4:3 and 16:9 unprotected.
     *
     * Force-enable a 1 MB per-slot host polybuf under --no-lod so the
     * inflated prim load fits regardless of aspect. */
    if (widescreen_polybuf_setup(0x100000u, 0x80400000u) != 0) {
        fprintf(stderr, "[no-lod] widescreen_polybuf_setup failed; "
                "scene density may exceed the canonical polybuf at "
                "wide aspects or in dense levels.\n");
    }

    /* OPENPETE_DBG_KILL_LP env-var (debug-only): force the env-
     * renderer's g_SkipLowPolyWorld load to return 1. Affects only
     * the env-renderer's LP-walk pre-pass; used to visually identify
     * what cells the LP-walk renders. Tied to --no-lod since it
     * targets the same env-renderer subsystem. */
    {
        const char* dbg = getenv("OPENPETE_DBG_KILL_LP");
        if (dbg && dbg[0] != '\0' && dbg[0] != '0') {
            psx_runtime_dbg_kill_env_lp_walk = 1;
            fprintf(stderr,
                    "[no-lod] OPENPETE_DBG_KILL_LP=1: env-renderer "
                    "LP-walk disabled\n");
        }
    }

    fprintf(stderr,
            "[no-lod] grown env queues at base=0x%08X size=%u KB "
            "(HP=+0x%X mid=+0x%X LP=+0x%X from g_SonyImage)\n",
            NO_LOD_REGION_VADDR, NO_LOD_REGION_SIZE / 1024u,
            NO_LOD_HP_QUEUE_OFFSET,
            NO_LOD_MID_QUEUE_OFFSET,
            NO_LOD_LP_QUEUE_OFFSET);
    return 0;
}

/* === per-frame moby-queue clear (func_8001F158 pre-hook) =============
 *
 * The regular moby cull (func_8001F158) is the queue PRODUCER: it resets
 * its write pointer to the head (relocated to MOBY_QUEUE_REGION_VADDR
 * under --no-lod), writes one record per admitted moby, advances, and
 * marks the end by writing ONLY word0=0 at the next slot (r_moby.s
 * 0x8001F744) — it does NOT fully zero that slot.
 *
 * In canonical 4:3 the queue stays clean frame-to-frame because the .s
 * REGULAR renderer (the consumer) drains it from the canonical head and
 * zeroes entries as it goes. Under --no-lod the producer writes to the
 * RELOCATED host region but the .s consumer still reads the (now empty)
 * canonical 0x1600 slot, so it never drains/zeroes the relocated region.
 * Combined with the native consumer detecting the terminator by
 * header(+0x04)==0 && moby(+0x34)==0 (the producer's partial word0=0
 * terminator leaves those STALE-nonzero), the relocated region accrues a
 * stale tail: when fewer mobys are visible than a previous frame, the
 * native walk reads past the current terminator into old records — a
 * TRAIL of duplicate mobys (most visibly Sparx, written at a shifting
 * slot index every frame).
 *
 * Fix: replicate the canonical per-frame-clean-queue invariant by
 * zeroing the relocated region at the START of every producer pass (the
 * producer immediately resets to the head and overwrites, so after it
 * writes K records slots K.. stay fully zero → the terminator slot is
 * fully zero → both consumers stop at K). Returns 0 so the real producer
 * still runs. Host-only memory; no gameplay-readable byte touched. */
static int no_lod_clear_moby_queue(CPUState* cpu, uint32_t addr) {
    (void)cpu; (void)addr;
    if (openpete_no_lod) memset(s_moby_queue_region, 0, sizeof(s_moby_queue_region));
    return 0;  /* run the real func_8001F158 body */
}

static const feature_dispatch_entry_t no_lod_dispatch[] = {
    { 0x8001F158u, no_lod_clear_moby_queue },  /* regular moby cull producer */
    { 0u, NULL },
};

/* === apply_options ===================================================
 * Installs the setup hook so core/features/no_lod.c can call it. The
 * one dispatch arm zeroes the relocated moby-queue each producer pass
 * (see no_lod_clear_moby_queue); it is inert (returns 0, no memset)
 * when --no-lod is off. */
static void no_lod_apply_options_module(const openpete_options_t* opts) {
    (void)opts;
    no_lod_set_setup_hook(no_lod_setup);
}

const feature_module_t no_lod_module = {
    .name           = "no_lod",
    .apply_options  = no_lod_apply_options_module,
    .dispatch_table = no_lod_dispatch,
};

/* === Recompiler-called --no-lod hooks ================================
 * Named-hook implementations the recompiler calls (generated from
 * manifest/render_channels.toml, the no_lod_* + dbg_kill_env_lp_walk
 * channels). The bespoke read+conditional math lives here, host-side,
 * instead of in the vendored recompiler. Identity when --no-lod is off
 * (openpete_no_lod == 0). Moved out of core/features/widescreen.c so the
 * feature is siloed. */

/* env distance-field saturate: under --no-lod return INT32_MAX so the cull
 * admits every cell (m_LodDistance ×3, m_CullingDistance ×1). */
uint32_t psx_runtime_no_lod_saturate32(CPUState* cpu, uint32_t addr) {
    return openpete_no_lod ? 0x7FFFFFFFu : cpu->read_word(addr);
}
/* packed m_Renderer half-word: under --no-lod saturate the low-7-bit
 * m_Distance to 0x7F (only when non-zero — 0 is the uninit-slot sentinel),
 * preserving the shiny bit + byte 0x51, and record the canonical-distance
 * delta for the dist-gate verdict (psx_runtime_mdist_thresh_delta). */
uint32_t psx_runtime_no_lod_moby_distance(CPUState* cpu, uint32_t addr) {
    uint32_t v = cpu->read_half(addr);
    if (openpete_no_lod && (v & 0x7Fu) != 0u) {
        psx_runtime_mdist_thresh_delta = (int32_t)((0x7Fu - (v & 0x7Fu)) << 8);
        return (v & 0xFF80u) | 0x7Fu;
    }
    psx_runtime_mdist_thresh_delta = 0;
    return v;
}
/* env HP-walk texture LOD: suppress the `addi $t5,$t5,8` HQ->LQ descriptor
 * bump so every quad reads the HQ half. */
int32_t psx_runtime_no_lod_tier_bump(int32_t base, int32_t bump) {
    return openpete_no_lod ? base : (base + bump);
}
/* moby model LOD collapse: the LP-band face-list load reads m_LpFaces (offset
 * 0x1C) by default; --no-lod swings the knob to 0x14 (m_Faces, high-poly). */
uint32_t psx_runtime_moby_lp_face_load(CPUState* cpu, uint32_t base) {
    return cpu->read_word(base + psx_runtime_moby_lp_face_offset);
}
/* Debug-only (OPENPETE_DBG_KILL_LP): env-renderer g_SkipLowPolyWorld read —
 * OR in 1 when the knob is set so the LP-walk pre-pass is skipped. */
uint32_t psx_runtime_dbg_skiplpworld(CPUState* cpu, uint32_t addr) {
    return cpu->read_word(addr)
         | (psx_runtime_dbg_kill_env_lp_walk ? 1u : 0u);
}
/* Debug LP-kill + world-geo strip tag hybrid (7th strip-list loop head,
 * co-owned with world_geo_strip_tag): force the loaded value to 0 when the
 * LP-kill knob is set, else read normally; either way stamp the per-strip
 * world-geo identity. Identity at default knob. */
uint32_t psx_runtime_dbg_lpwalk_strip(CPUState* cpu, uint32_t addr) {
    uint32_t v = psx_runtime_dbg_kill_env_lp_walk ? 0u : cpu->read_word(addr);
    openpete_world_geo_set_current(v);
    return v;
}
