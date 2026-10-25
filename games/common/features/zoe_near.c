// games/common/features/zoe_near.c - OPENPETE_VIDEO_ZOE_NEAR: the checkpoint
// fairy behaves as if the player stood near her, at any distance.
//
// Her update shows her, keeps her idle and triggers the checkpoint touch by
// comparing the planar distance game_layout.zoe.dist_fn returns against
// fixed thresholds. Under the knob the module's hook at that function
// answers a call made from her update with game_layout.zoe.near_dist in v0
// and skips the body; a call from anywhere else runs unchanged. The value
// shows her and keeps her in the idle branch without the touch. Her visible
// branch then runs where it would not, including its rand() calls, so the
// knob changes gameplay state. Boot-latched, default off; off, the hook is
// unbound.

#include "game_layout.h"
#include "feature_registry.h"
#include "openpete_log.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"

#include <stdint.h>

#define ZN (game_layout.zoe)

static int s_zoe_near = -1;   /* OPENPETE_VIDEO_ZOE_NEAR, latched at apply-options time */

/** @brief Answer a distance call made from the fairy's update with the near distance.
 *
 * Reads her update's entry from the resident overlay's per-class table
 * (game_layout.owner.moby_update) and tests the return address against
 * its `update_len` bytes.
 * @param cpu The guest CPU state.
 * @param addr The distance function's entry (unused).
 * @return 1 with v0 set when the caller is her update, else 0. */
static int zn_on_dist(CPUState* cpu, uint32_t addr) {
    (void)addr;
    const uint32_t tab = cpu->read_word(game_layout.owner.moby_update);
    if (tab < 0x80000000u || tab >= 0x80200000u) return 0;
    const uint32_t z = cpu->read_word(tab + 4u * ZN.cls);
    if (cpu->ra < z || cpu->ra >= z + ZN.update_len) return 0;
    cpu->v0 = ZN.near_dist;
    return 1;
}

static feature_dispatch_entry_t zn_dispatch[] = {
    { 0u, zn_on_dist, 0u },
    { 0u, NULL, 0u },
};

/** @brief Latch OPENPETE_VIDEO_ZOE_NEAR and bind the distance hook while it is on.
 *
 * A game whose layout names no distance function or per-class update
 * table leaves the hook unbound and warns when the knob is set.
 * @param opts Unused. */
static void zn_apply_options(const openpete_options_t* opts) {
    (void)opts;
    ENV_FLAG_LATCH(s_zoe_near, "OPENPETE_VIDEO_ZOE_NEAR");
    if (s_zoe_near > 0 && (ZN.dist_fn == 0u || game_layout.owner.moby_update == 0u)) {
        OPL_WARN_F("zoe-near", "OPENPETE_VIDEO_ZOE_NEAR is set but this game names no fairy distance call; ignored");
        s_zoe_near = 0;
    }
    zn_dispatch[0].addr = s_zoe_near > 0 ? ZN.dist_fn : 0u;
    if (s_zoe_near > 0)
        OPL_INFO_F("zoe-near", "OPENPETE_VIDEO_ZOE_NEAR: class 0x%X distance reads return 0x%X",
                   ZN.cls, ZN.near_dist);
}

const feature_module_t zoe_near_module = {
    .name           = "zoe_near",
    .apply_options  = zn_apply_options,
    .dispatch_table = zn_dispatch,
};
