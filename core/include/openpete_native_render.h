/* core/include/openpete_native_render.h — native Vulkan renderer A/B harness.
 *
 * The host-side entry points for openpete's native renderer (the Phase 1c
 * foundation of notes/plans/openpete-native-renderer.md). The native
 * renderer is a pure *display* consumer: it reads gamestate + asset data +
 * the VRAM mirror and draws into its own Vulkan window. It NEVER writes
 * PSX-readable RAM, so it is determinism-safe by construction — the
 * original `.s` renderers keep running (here, via PsyCross) as the headless
 * gameplay-verdict producers, and the oracle gate is unaffected.
 *
 * Two-window model: a single SDL window cannot host both PsyCross's OpenGL
 * context and our Vulkan surface, so the native renderer creates its own
 * window. During development it runs ALONGSIDE a visible PsyCross window for
 * A/B comparison; the end state hides PsyCross (FBO-only, read back as the
 * "reference") and the Vulkan window is the only one shown.
 *
 * Gated entirely by --native-ab. With the flag absent every entry point is
 * an inert no-op, so normal/headless runs are byte-for-byte unaffected.
 */
#ifndef OPENPETE_NATIVE_RENDER_H
#define OPENPETE_NATIVE_RENDER_H

#include <stdbool.h>

#include "openpete.h" /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the native Vulkan window + RHI device. Call AFTER PsyX_Initialise
 * (SDL_INIT_VIDEO must already be up). `win_w`/`win_h` are the resolved
 * window size (the same the caller hands PsyCross), so the native window
 * matches the game's window rather than a fixed default. Returns 0 on success
 * OR when the native path is simply not requested; returns non-zero only on a
 * real bring-up failure (the caller logs and continues without the native
 * path — it is never fatal to the game). */
int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h);

/* True once init succeeded and the native A/B display is live. */
bool openpete_native_render_enabled(void);

/* Present one native frame to the Vulkan window. Call after PsyX_EndScene so
 * PsyCross's present FBO is freshly rendered (the future "reference"). Does
 * NOT pump SDL events — the host loop owns the event queue, which PsyCross's
 * input path depends on; window-size changes are detected by polling. No-op
 * if the native path is disabled. */
void openpete_native_render_frame(void);

/* Tear down the device + window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
