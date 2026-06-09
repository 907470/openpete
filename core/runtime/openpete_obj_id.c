/* openpete_obj_id.c — see openpete_obj_id.h. */

#include "openpete_obj_id.h"
#include "openpete_world_geo.h"   /* openpete_world_geo_clear */
#include "openpete_psx.h"         /* openpete_psx_to_host_ptr (read-only) */

static uint32_t s_obj_id = 0;
static uint32_t s_vidx   = 0;    /* per-object emission ordinal (#25) */

void openpete_obj_id_set(uint32_t obj_addr) {
    s_obj_id = obj_addr;
    s_vidx   = 0;                /* new object → restart ordinal */
}

uint32_t openpete_obj_id_get(void) {
    return s_obj_id;
}

/* #25 animation lerp: the Nth GTE-projected vertex of the current
 * object gets ordinal N (G1: per-moby emission order is cross-frame
 * stable, perm-cost ratio 0.005). Keying the matcher's per-vertex
 * correspondence by (span, ordinal) instead of v_local makes the
 * SAME logical vertex pair across two ANIMATION poses — v_local
 * churns under morphing, which is exactly why animated mobys froze
 * at 30 Hz. Called once per moby-tagged vertex from
 * gte_rtps_internal's cache emit. */
uint32_t openpete_obj_id_next_vidx(void) {
    return s_vidx++;
}

/* === Per-renderer identity-tag wrappers (AppendCall hooks) ============
 * The recompiler appends one of these after the per-object loop-iteration
 * load so gte_rtps_internal stamps a stable cross-frame obj_tag. They
 * compose the existing primitives so the recompiler emits a single call
 * (the bespoke multi-call sequence moved out of code_generator.cpp). All
 * READ-ONLY w.r.t. gameplay — host-side identity tags for the transform-
 * interp matcher only. */

/* Shaded per-moby loop: the loaded moby vaddr feeds BOTH the widescreen UI
 * classifier (the shaded renderer is the only HUD/UI-moby path) and the
 * transform-interp identity; then stale world-geo strip state is cleared. */
void openpete_moby_loop_tag(uint32_t moby_addr) {
    openpete_ui_moby_set_current(moby_addr);
    openpete_obj_id_set(moby_addr);
    openpete_world_geo_clear();
}

/* Singleton renderers (Spyro / cyclorama): obj_id identity only (never UI),
 * then clear the world-geo strip tag. */
void openpete_obj_loop_tag(uint32_t obj_addr) {
    openpete_obj_id_set(obj_addr);
    openpete_world_geo_clear();
}

/* Untagged renderers (shadows / flames / particles): clear BOTH identity
 * side-channels at function entry so their cache entries emit as
 * obj_tag==0 (the ordinal/heuristic fallback they were designed for)
 * instead of inheriting whatever tag the PREVIOUS renderer left set.
 * The stale bleed made shadow batches carry the last moby's vaddr —
 * they then consumed (or failed) that moby's authoritative pairing and
 * flickered between frozen-red and ordinal-blue in the overlay, while
 * polluting the real owner's pair. Interim until these renderers get
 * true per-slot tags (§12.1 straggler pass). */
void openpete_untagged_renderer_entry(void) {
    openpete_obj_id_set(0);
    openpete_world_geo_clear();
}

/* Regular/specular render iter: read this iteration's moby instance vaddr
 * DIRECTLY from the render-queue entry under the loop cursor.
 *
 * The cull passes build 56-byte queue entries and store the surviving
 * moby's instance pointer at entry+52 (`sw $sp, 52($fp)`) before
 * committing the entry (fp += 56). The render loops walk those entries
 * with $gp as the cursor, so at the iter-top hook $gp+52 IS the
 * per-instance identity — no side-channel bookkeeping required. (Exact
 * game PCs live in manifest/render_channels.toml, channel
 * transform_interp_moby_queue — core/ stays address-free.)
 *
 * This replaces the f94e167 host-side FIFO, which assumed push/pop
 * balance that the game does not guarantee: the cull-join push site
 * fired for mobys that passed the distance/frustum cascade but were
 * never appended to the render list, producing a chronic push>pop
 * deficit that saturated the ring by ~4000 ticks and shuffled all
 * identity tags (the "same-class polys stretching between instances"
 * bug — transform-interp plan §11 F0). Reading the entry itself is
 * desync-proof by construction.
 *
 * The terminator iteration (entry word 0 == 0 -> loop exit) reads
 * whatever stale bytes sit at +52 past the live list; the plausibility
 * mask degrades that to tag 0 only if implausible, and no GTE verts are
 * emitted between the terminator read and the loop exit, so a stale-
 * but-plausible value is never consumed. READ-ONLY w.r.t. gameplay. */
void openpete_moby_render_entry_tag(uint32_t entry_vaddr) {
    uint32_t moby = 0;
    const uint32_t* p =
        (const uint32_t*)openpete_psx_to_host_ptr(entry_vaddr + 52u);
    if (p) moby = *p;
    /* Moby instances live in KSEG0 RAM. Anything else is an
     * uninitialised/terminator slot -> tag 0 (world-ordinal fallback,
     * the pre-queue baseline). */
    if (moby < OPENPETE_PSX_KSEG0_BASE || moby >= OPENPETE_PSX_KSEG0_END)
        moby = 0;
    openpete_obj_id_set(moby);
    openpete_world_geo_clear();
}
