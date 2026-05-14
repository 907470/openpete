// core/runtime/fiber.c — cooperative scheduler globals (Phase 1d).
//
// See core/include/openpete_fiber.h. The actual fiber spawn/loop lives in
// run.c; this TU only owns the shared state so PSYQ thunks (in games/<g>/)
// can call `openpete_yield_to_host()` without dragging in libco directly.

#include "openpete_fiber.h"

#include <libco.h>
#include <stddef.h>

void*        openpete_host_fiber       = NULL;
void*        openpete_game_fiber       = NULL;
SDL_atomic_t openpete_vbl              = {0};
unsigned     openpete_frame            = 0u;
unsigned     openpete_primitives_drawn = 0u;
int          openpete_present_pending  = 0;
int          openpete_fast_boot_active = 1;
int          openpete_fov_h_override    = 0;
float        openpete_disp_w_factor     = 0.0f;
/* openpete_no_lod lives in core/features/no_lod.c. */
/* Set while GamestateDraw is on the call stack. Hor+ widening (GTE OFX,
 * moby-cone substitution) must only apply to render-pass GTE work; if it
 * leaks into GamestateUpdate, gameplay state diverges between aspects. */
int          openpete_render_pass_active = 0;
/* Render-pass cone-widening value, staged at startup by run.c and swapped
 * into psx_runtime_moby_cone_extra_q12 around the GamestateDraw call. */
int32_t      openpete_moby_cone_extra_q12_render = 0;
/* Render-pass cone X-bias scale (Q12, (factor-1) form). Companion to the
 * slope widening above — the slope-only widening leaves a near-Z slice with
 * native cone width because the X-bias terms (102/77 constants in shaded,
 * per-moby radius in regular/specular) don't scale with aspect. Staged by
 * widescreen.c and swapped into psx_runtime_moby_cone_bias_scale_q12 around
 * the GamestateDraw call. 0 = identity. */
int32_t      openpete_moby_cone_bias_scale_q12_render = 0;
/* Phase 7.2: near-Z clamp soften. Lifts the (mac3p < floor → floor)
 * clamp from 0 to a positive value during the render pass so slope
 * widening still contributes a partial cone in the strip right at the
 * camera plane. 0 = preserve native clamp. */
int32_t      openpete_moby_cone_mac3p_floor_render = 0;
/* Render-pass env-cell X-bound (in MAC1 units), staged by widescreen
 * setup and swapped into psx_runtime_env_cell_x_bound around the
 * GamestateDraw call. 0 sentinel = no widening. */
uint32_t     openpete_env_cell_x_bound_render = 0u;
/* Render-pass GTE SXY saturation bound (px), staged by widescreen
 * setup and swapped into psx_runtime_sxy_sat_bound around the
 * GamestateDraw call. 0 sentinel = no widening (keep native ±0x400). */
uint32_t     openpete_sxy_sat_bound_render = 0u;

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* `openpete_vbl` is the ticker's job now (free-running 60 Hz, see
     * openpete_ticker.h). Yields only bump the host-side render counter so
     * frame-limit / screenshot / bk2 alignment stay yield-driven. */
    openpete_frame++;
    co_switch((cothread_t)openpete_host_fiber);
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    co_switch((cothread_t)openpete_host_fiber);
}
