#ifndef OPENPETE_RHI_H
#define OPENPETE_RHI_H

/*
 * openpete RHI — thin Render Hardware Interface.
 *
 * A minimal, opaque-handle GPU abstraction sized to exactly what the
 * native Spyro renderer needs, so Vulkan (the first backend) can later be
 * joined by Metal / WebGPU without touching renderer code. It grows
 * function-by-function as renderer milestones require; it is deliberately
 * NOT a general render graph.
 *
 * Milestone 1a surface (this file): device + swapchain + a single
 * offscreen color target, a clear-the-offscreen pass, a blit of that
 * target onto the swapchain, and present. No pipelines, buffers, or
 * textures yet — those arrive in 1b alongside the native-res VRAM model
 * and the ported PSX texture-sampling shader.
 *
 * The offscreen-target -> blit -> present structure is intentional from
 * day one: the 3D scene will render INTO the offscreen target, the 2D
 * layer composites over it, framebuffer effects (StoreImage) read back
 * from it, and only then is it blitted to the swapchain. 1a establishes
 * that path by clearing the offscreen target rather than the swapchain.
 *
 * Threading: single-threaded. Every call happens on the loop that drives
 * the frame (the host fiber in openpete; main() in the smoke test).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SDL_Window;

typedef struct rhi_device rhi_device;

/* Create the device against an SDL window created with SDL_WINDOW_VULKAN.
 * `vsync` selects the swapchain present mode: 1 = force FIFO (vsync ON, present
 * locked to the monitor refresh), 0 = force a vsync-OFF mode (immediate, else
 * mailbox — uncapped throughput / low latency), -1 = unset (defer to the
 * OPENPETE_PRESENT_MODE env var, else default FIFO). Falls back to FIFO with a
 * log if a requested vsync-OFF mode isn't supported by the surface.
 * Returns NULL on failure (a diagnostic is logged to stderr). */
rhi_device *rhi_create(struct SDL_Window *window, const char *app_name, int vsync);

/* Create a SURFACELESS device: no window, no swapchain, no present. Renders into
 * the offscreen + render-target images only; retrieve pixels with
 * rhi_read_render_target / rhi_read_offscreen. width/height set the offscreen
 * extent (the would-be window size). For headless CI visual-regression capture
 * under a software rasterizer (lavapipe). Returns NULL on failure. */
rhi_device *rhi_create_headless(const char *app_name, uint32_t width, uint32_t height);

/* True for a device created via rhi_create_headless. Such a device's begin/end
 * frame submit work without acquiring or presenting a swapchain image, and
 * rhi_blit_to_swapchain is a no-op. */
bool        rhi_is_headless(const rhi_device *dev);

void        rhi_destroy(rhi_device *dev);

/* The GPU's maximum 2D image dimension (VkPhysicalDeviceLimits.maxImageDimension2D).
 * The renderer uses it to cap the SSAA supersampled render target so a large
 * --ssaa factor degrades gracefully instead of failing image creation. */
uint32_t    rhi_max_image_dimension(const rhi_device *dev);

/* Block until the GPU is idle. Call before destroying buffers/pipelines
 * that recent frames may still be using. */
void rhi_wait_idle(rhi_device *dev);

/* Begin a frame: waits on the previous use of this frame slot, acquires a
 * swapchain image, and opens the command buffer. Returns false if the
 * swapchain could not be used this frame (e.g. it needed recreation, or
 * the window is minimized) — the caller should skip rendering this
 * iteration; the device handles recreation internally. */
bool rhi_begin_frame(rhi_device *dev);

/* Offscreen color pass. For 1a, begin+end with nothing drawn between
 * clears the offscreen target to the given (linear, 0..1) color. From 1b
 * on, draw calls are recorded between begin and end.
 *
 * off_image is a SINGLE instance shared by all frames in flight — an
 * explicit serialization point (W2.2 decision, not an oversight): frame
 * N+1's clear-on-load waits frame N's reads via the render pass's
 * EXTERNAL src dependency, so back-to-back frames serialize on it. Ring
 * it per frame-in-flight if that dependency ever shows up as the
 * bottleneck (it also backs rhi_read_offscreen and the swapchain blit,
 * so a ring must thread the current slot through both). */
void rhi_begin_offscreen(rhi_device *dev, float r, float g, float b, float a);
void rhi_end_offscreen(rhi_device *dev);

/* Blit the offscreen color target onto the acquired swapchain image
 * (linear filter; handles the required layout transitions). */
void rhi_blit_to_swapchain(rhi_device *dev);

/* Submit the recorded command buffer and present the swapchain image. */
void rhi_end_frame(rhi_device *dev);

/* Tell the device the window size changed; the swapchain + offscreen
 * target are recreated on the next rhi_begin_frame. Safe to call on every
 * resize event. */
void rhi_notify_resize(rhi_device *dev);

/* Register a handler called once on an unrecoverable VK_ERROR_DEVICE_LOST
 * (acquire/present). Lets the owning layer flag a clean shutdown; the RHI itself
 * stays free of core-runtime symbols. NULL clears it. Global, not per-device. */
void rhi_set_device_lost_handler(void (*fn)(void));

/* Diagnostics: the selected physical device name (valid until destroy). */
const char *rhi_device_name(const rhi_device *dev);

/* Read the offscreen color target into a host RGBA8 buffer (byte order
 * R,G,B,A, alpha forced opaque), for screenshots. Call after a rendered
 * frame (rhi_end_frame) — it reads the last frame's offscreen contents.
 * `out` must hold >= (*out_w)*(*out_h)*4 bytes; the extent is written back.
 * Returns false on failure (e.g. out too small). Blocks the GPU. */
bool rhi_read_offscreen(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h);

/* ---- async frame readback ring (--record real-speed capture) -------------
 *
 * rhi_read_offscreen blocks the GPU (vkDeviceWaitIdle) every call, which
 * collapses pipelining and forces recording into slow motion. This ring
 * records the offscreen->host copy INTO the frame's OWN command buffer (no
 * extra wait — it rides the frame's existing fence) and retrieves the pixels a
 * couple of presents later, once that fence has naturally signaled. `tag` is an
 * opaque token (the caller's PTS) carried with each frame so the out-of-band
 * pixels keep their timestamp.
 *
 * Call rhi_cycle_readback ONCE per frame, between rhi_blit_to_swapchain and
 * rhi_end_frame (while the frame slot is still current). It queues this frame's
 * capture (tagged `tag`) and, if a frame captured RHI_FRAMES_IN_FLIGHT presents
 * ago is ready, swizzles it into `out` (RGBA8, opaque alpha), writes its extent
 * + originating tag, and returns true. Returns false until the ring fills.
 *
 * At record shutdown, call rhi_drain_readback repeatedly (it blocks once via
 * vkDeviceWaitIdle, then returns the still-queued frames oldest-first by tag)
 * until it returns false, so the final presents aren't lost. */
bool rhi_cycle_readback(rhi_device *dev, uint64_t tag, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag);
bool rhi_drain_readback(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag);

/* ---- buffers, pipelines, draws (milestone 1b) --------------------- */

/* Native vertex format — one fixed layout for all renderer geometry, so a
 * single vertex buffer format feeds every pipeline. 1b.1 used position +
 * color; 1b.2 brought uv online for the PSX sampler. The per-draw texture
 * page / CLUT / bpp ride a push constant (one material per draw batch — a
 * native renderer sorts by material), so the packed per-vertex `tex` field
 * is currently unused/reserved (PSX-era per-vertex tpage packing would only
 * return if mixed-material batching is ever needed). */
typedef struct {
    float    x, y, z;   /* position (clip space in the smoke test; proj later) */
    float    u, v;      /* texture coords in TEXEL units (PSX integer indexing) */
    uint32_t rgba;      /* per-vertex color, byte order R,G,B,A                 */
    uint32_t tex;       /* reserved (packed tpage|clut|mode), see note above    */
} rhi_vertex;

typedef struct rhi_buffer   rhi_buffer;
typedef struct rhi_pipeline rhi_pipeline;
typedef struct rhi_texture  rhi_texture;

/* RHI_BLEND_SUBTRACT = PSX ABR mode 2 (dst = B - F): reverse-subtract, src ONE,
 * dst ONE — used by shadows (subtractive darkening). */
typedef enum { RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD, RHI_BLEND_SUBTRACT } rhi_blend_mode;

/* Stencil role of a pipeline (W2.6 — the aperture-mask investment; the depth
 * attachment is a combined depth+stencil format, stencil cleared to 0 at every
 * pass begin):
 *  - MARK: rasterize a mask — where the fragment passes the DEPTH test, write
 *    stencil=1 (depth-fail leaves 0, so world geometry in front punches the
 *    mask out). Pair with color_write_off for an invisible mask pass.
 *  - TEST: draw only where stencil==1 (inside the mask); stencil not written.
 *  - TEST_ZERO: draw only where stencil==0 (OUTSIDE the mask); stencil not
 *    written. The hole-fill role (W2.6-c): geometry that must paint only the
 *    pixels a prior MARK pass did NOT cover (e.g. the terrain LP backing layer
 *    filling LP↔HP T-junction seams without ever poking through the HP).
 * Reference is fixed at 1 — per-object isolation is done by rhi_clear_stencil
 * between mask groups, not by reference juggling. */
typedef enum { RHI_STENCIL_NONE, RHI_STENCIL_MARK, RHI_STENCIL_TEST,
               RHI_STENCIL_TEST_ZERO } rhi_stencil_mode;

typedef struct {
    const char    *vert_spv;    /* filename within RHI_SHADER_DIR */
    const char    *frag_spv;
    rhi_blend_mode blend;
    bool           depth_test;  /* needs a depth attachment (added later) */
    bool           depth_write_off; /* depth_test but NO depth write (transparent: test vs opaque, don't occlude) */
    bool           depth_bias;  /* enable the hardware polygon offset (decal
                                 * tie-break) with DYNAMIC factors. Vulkan
                                 * invalidates dynamic state whenever a
                                 * pipeline with that state STATIC is bound
                                 * (i.e. any non-depth_bias pipeline), so the
                                 * caller MUST rhi_set_depth_bias after EVERY
                                 * bind of a depth_bias pipeline, before its
                                 * draws. */
    rhi_stencil_mode stencil;   /* NONE (default) / MARK / TEST — see enum docs */
    bool           color_write_off; /* rasterize without writing colour (an
                                 * invisible stencil-mask pass)               */
    bool           uses_vram;   /* layout includes a sampler at set 0          */
    bool           uses_vram2;  /* second sampler at set 1 (e.g. the diff pass:
                                 * set 0 = native render, set 1 = reference)   */
    uint32_t       push_const_size; /* fragment push-constant block bytes (0=none) */
} rhi_pipeline_desc;

/* Host-visible vertex buffer (map-and-memcpy updates).
 *
 * rhi_create_vertex_buffer — SINGLE instance. Safe only for content that is
 * static after init, or rewritten under rhi_wait_idle (rare re-layout): a
 * per-present rewrite would race the frame two presents ago, whose GPU read
 * is only fenced at rhi_begin_frame (write-after-read tear).
 *
 * rhi_create_vertex_buffer_dynamic — per-frame-in-flight RING (W2.2). For
 * buffers rewritten EVERY present: update writes and bind reads the current
 * frame's slot, which rhi_begin_frame's fence has proven idle. Contract: the
 * content must be written on every frame that binds it (a slot written once
 * and bound on a later frame of the other parity reads that parity's stale
 * slot). rhi_update_buffer outside an active frame fills ALL slots (init /
 * under-wait-idle updates stay parity-safe). */
rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes);
rhi_buffer *rhi_create_vertex_buffer_dynamic(rhi_device *dev, size_t bytes);
void        rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes);
void        rhi_destroy_buffer(rhi_device *dev, rhi_buffer *buf);

rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc);
void          rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe);

/* ---- the native-res PSX VRAM model (milestone 1b.2) -------------- */

/* A single 16-bit-per-texel image (R16_UINT) that mirrors PSX VRAM one
 * uint per pixel — texture pages, CLUTs, and (later) the framebuffer
 * regions all live here, sampled through the ported PSX 4/8/16bpp + CLUT
 * decoder (shaders/psx_sample.frag). 1024x512 is the faithful PSX VRAM
 * extent; the renderer keeps a CPU mirror and uploads from it. */
rhi_texture *rhi_create_vram(rhi_device *dev, uint32_t width, uint32_t height);
/* Upload the whole CPU mirror (width*height uint16 PSX pixels).
 *
 * Upload semantics (W2.2, also rhi_update_rgba8): if called while a frame is
 * being recorded (after rhi_begin_frame), the copy is appended to THAT
 * frame's command buffer through a per-frame staging ring — no GPU stall.
 * Callers MUST then invoke it before the frame's first render pass. Outside
 * a frame it falls back to the blocking one-shot submit (init-time). */
void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels);

/* Sub-rect upload (W2.4 dirty-rect path): copy only `rects` regions of the
 * CPU mirror (`mirror` is the FULL width-stride buffer) into the texture, in
 * one barrier pair + one N-region transfer. Same in-frame/one-shot semantics
 * as rhi_update_vram. Rects must be pre-clamped to the texture extent. If the
 * summed rect payload exceeds one full-image staging slot (heavily
 * overlapping rects), it silently falls back to a full upload. */
typedef struct { int x, y, w, h; } rhi_rect;
void rhi_update_vram_rects(rhi_device *dev, rhi_texture *tex,
                           const uint16_t *mirror,
                           const rhi_rect *rects, int nrects);

/* An RGBA8 (R8G8B8A8_UNORM, bytes R,G,B,A) sampled image — e.g. a frame read
 * back from another renderer for A/B display. Shares the VRAM texture's
 * descriptor layout, so it binds via rhi_bind_vram and is drawn by a textured
 * pipeline that samples a normal sampler2D (shaders/blit.frag). `pixels` is
 * width*height*4 bytes. */
rhi_texture *rhi_create_rgba8(rhi_device *dev, uint32_t width, uint32_t height);
void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels);

/* Switch a sampled texture's descriptor to the LINEAR-filter sampler (the
 * default is NEAREST — pixel-exact VRAM/blit semantics). For high-res
 * replacement art whose minification/magnification should filter. Call at
 * load time, before the texture is first sampled in a frame. */
void rhi_texture_set_filter_linear(rhi_device *dev, rhi_texture *tex);

/* ---- render-to-texture (the diff pass; later: framebuffer effects) ----- */

/* An RGBA8 image that is BOTH a color attachment AND a sampler2D — geometry
 * can be rendered into it (rhi_begin/end_render_target) and the result then
 * sampled by a later pass in the SAME frame. Carries its own depth buffer and
 * framebuffer; binds as a normal texture via rhi_bind_vram. Sized in pixels;
 * recreate (destroy + create) on size change. */
rhi_texture *rhi_create_render_target(rhi_device *dev, uint32_t width, uint32_t height);

/* Render INTO a render target. begin clears color to (r,g,b,a) + depth to far,
 * sets a full-target Y-flipped viewport/scissor; record draws; end closes the
 * pass and leaves the color image in SHADER_READ_ONLY (ready to sample). Must
 * be called inside a frame (after rhi_begin_frame) but OUTSIDE the offscreen
 * pass (it is its own render pass). */
void rhi_begin_render_target(rhi_device *dev, rhi_texture *rt,
                             float r, float g, float b, float a);
void rhi_end_render_target(rhi_device *dev, rhi_texture *rt);

/* Like rhi_create_render_target, but the DEPTH attachment is ALSO sampleable:
 * the target renders through a render pass whose depth is STORED and finishes
 * SHADER_READ_ONLY, so a LATER pass in the same frame can sample the per-pixel
 * depth (a WORLD-opaque depth PRE-PASS for shadows / decals / soft particles —
 * per-pixel occlusion with no global depth bias). Bind the depth via
 * rhi_bind_depth_sampler after the pre-pass has ended. Returns NULL if the
 * device cannot sample the depth format (not spec-required for the combined
 * depth+stencil formats; universal in practice — callers keep a fallback). */
rhi_texture *rhi_create_render_target_depth_sampled(rhi_device *dev,
                                                    uint32_t width, uint32_t height);
/* Bind a depth-sampled RT's DEPTH aspect as a combined image sampler at
 * descriptor `set` (the consumer frag samples it as sampler2D; texel .r =
 * depth). Only valid after rhi_end_render_target on that RT this frame (the
 * pass's finalLayout + external dependency make the depth sample-visible).
 * Call after rhi_bind_pipeline. */
void rhi_bind_depth_sampler(rhi_device *dev, rhi_texture *rt, uint32_t set);
/* Async readback of a SMALL render target's color image (the occlusion grid),
 * ring-buffered exactly like rhi_cycle_readback: call once per frame AFTER the
 * pass that renders `rt`; queues this frame's copy into the frame slot and
 * returns the capture from RHI_FRAMES_IN_FLIGHT presents ago (true + fills
 * out/out_w/out_h in R,G,B,A byte order), riding the frame fence — no GPU
 * stall. Single ring per device (one client); `out` needs w*h*4 bytes. */
bool rhi_cycle_rt_readback(rhi_device *dev, rhi_texture *rt,
                           void *out, size_t out_bytes, int *out_w, int *out_h);
/* Override the current pass's viewport+scissor to a (w,h) Y-flipped rect. Used
 * so a depth pre-pass into an OVERSIZED depth target rasterizes only the
 * (w,h) sub-rect the consumers will map their lookups to. Recorded inside a
 * render pass, after begin. */
void rhi_set_viewport(rhi_device *dev, int w, int h);
/* The current swapchain/offscreen pixel extent (the resolution the offscreen
 * pass — rhi_begin_offscreen — renders at). Used to size a shared depth
 * pre-pass to cover the largest scene target. */
void rhi_swapchain_extent(const rhi_device *dev, int *w, int *h);

/* Clear the depth attachment to far WITHIN the current pass (offscreen or render
 * target). Used to depth-isolate a late on-top overlay (3D HUD icons) so it
 * Z-sorts internally without being occluded by, or occluding, world geometry. */
void rhi_clear_depth(rhi_device *dev);

/* Clear the STENCIL aspect to 0 WITHIN the current pass (offscreen or render
 * target). Used between per-object stencil-mask groups (e.g. one portal's
 * aperture mask + masked draw, then clear, then the next portal's), so a fixed
 * stencil reference of 1 isolates objects without reference juggling. */
void rhi_clear_stencil(rhi_device *dev);

/* Read a render target back into a host RGBA8 buffer (byte order R,G,B,A, alpha
 * forced opaque). The RT must be in SHADER_READ_ONLY (i.e. after
 * rhi_end_render_target + the frame that rendered it was submitted); the layout
 * is restored on return. `out` must hold >= width*height*4 bytes; the RT's
 * extent is written back. Returns false on failure. Blocks the GPU. Used by the
 * screenshot dump to capture the native scene at the reference resolution (no
 * window letterbox) regardless of window size. */
bool rhi_read_render_target(rhi_device *dev, rhi_texture *rt, void *out,
                            size_t out_bytes, int *out_w, int *out_h);

/* Hardware-blit a pixel region [sx,sy,sw,sh] of the (clean, window-sized)
 * offscreen into a render target, scaled to fill it. The native freeze backdrop
 * is captured this way — from the correctly-projected windowed render that's
 * already on screen — instead of from the off-display full-extent render target.
 * Call after rhi_end_offscreen (offscreen is TRANSFER_SRC), before
 * rhi_blit_to_swapchain; leaves dst in shader-read layout. */
void rhi_snapshot_offscreen_to_rt(rhi_device *dev, rhi_texture *dst,
                                  int sx, int sy, int sw, int sh);

void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex);

/* The actual pixel extent of a texture (e.g. a render target, which may have
 * been created smaller than requested if clamped to device limits). */
void rhi_texture_extent(const rhi_texture *tex, int *w, int *h);

/* Recorded between rhi_begin_offscreen and rhi_end_offscreen. */
void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe);
void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf);
/* Bind a texture's descriptor set to set 0 for the current (uses_vram)
 * pipeline; call after rhi_bind_pipeline, before the draw. Works for any
 * rhi_texture (the VRAM image or an RGBA8 image) — the set layout is the
 * same; only the shader's sampler type differs. */
void rhi_bind_vram(rhi_device *dev, rhi_texture *tex);
/* Bind a second texture to set 1 (for a uses_vram2 pipeline — e.g. the diff
 * pass's reference sampler). Call after rhi_bind_pipeline + rhi_bind_vram. */
void rhi_bind_vram2(rhi_device *dev, rhi_texture *tex);
/* Restrict rasterization to a framebuffer-pixel rect (top-left origin, y
 * down) for subsequent draws this frame — e.g. confine one panel's geometry
 * to its region so off-panel triangles are clipped. rhi_reset_scissor
 * restores the full target. Both must be called inside the offscreen pass. */
void rhi_set_scissor(rhi_device *dev, int x, int y, int w, int h);
void rhi_reset_scissor(rhi_device *dev);
/* Set the hardware depth-bias factors (vkCmdSetDepthBias order: constant,
 * clamp, slope) for subsequent draws whose pipeline was created with
 * desc.depth_bias. Negative values pull fragments TOWARD the camera under the
 * LESS_OR_EQUAL compare — the decal tie-break: constant×r beats
 * rasterization-rounding ties against coplanar geometry, slope×m grows the
 * pull at grazing angles, clamp bounds the total. r is the minimum resolvable
 * depth delta (~2^-24 relative for a float depth attachment). MUST be called
 * after every bind of a depth_bias pipeline, before its draws — binding any
 * non-depth_bias pipeline in between invalidates the dynamic state (Vulkan
 * static-state invalidation rule). If the depthBiasClamp device feature is
 * unavailable the clamp is forced to 0 (spec). */
void rhi_set_depth_bias(rhi_device *dev, float constant, float clamp, float slope);
/* Set the current pipeline's fragment push constants (size must match the
 * pipeline desc's push_const_size). */
void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size);
void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex);
/* Number of rhi_draw calls recorded since the last rhi_begin_frame (perf
 * census — the draw-submission count the material batching is minimizing). */
uint32_t rhi_draw_calls(rhi_device *dev);

#endif /* OPENPETE_RHI_H */
