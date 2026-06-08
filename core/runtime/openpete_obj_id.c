/* openpete_obj_id.c — see openpete_obj_id.h. */

#include "openpete_obj_id.h"
#include "openpete_world_geo.h"   /* openpete_world_geo_clear */
#include "openpete_moby_queue.h"  /* openpete_moby_queue_pop */

static uint32_t s_obj_id = 0;

void openpete_obj_id_set(uint32_t obj_addr) {
    s_obj_id = obj_addr;
}

uint32_t openpete_obj_id_get(void) {
    return s_obj_id;
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

/* Regular/specular render iter: pop the cull-pass queue for this instance's
 * real moby vaddr (0 on empty -> world-ordinal fallback in gte), tag it, and
 * clear the world-geo strip tag. */
void openpete_moby_queue_pop_tag(void) {
    openpete_obj_id_set(openpete_moby_queue_pop());
    openpete_world_geo_clear();
}
