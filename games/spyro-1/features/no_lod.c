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

/* === apply_options ===================================================
 * Installs the setup hook so core/features/no_lod.c can call it. No
 * per-vaddr dispatch arms (every --no-lod intercept is recompiler-
 * level substitution, not a function-entry hook), so dispatch_table
 * is NULL. */
static void no_lod_apply_options_module(const openpete_options_t* opts) {
    (void)opts;
    no_lod_set_setup_hook(no_lod_setup);
}

const feature_module_t no_lod_module = {
    .name           = "no_lod",
    .apply_options  = no_lod_apply_options_module,
    .dispatch_table = NULL,
};
