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
 * Returns NULL on failure (a diagnostic is logged to stderr). */
rhi_device *rhi_create(struct SDL_Window *window, const char *app_name);
void        rhi_destroy(rhi_device *dev);

/* Set the live-window supersampling factor (1..4; 1 = no-op). The offscreen
 * render target is sized at N× the swapchain extent per axis and the present
 * blit downsamples to the window. A change marks the swapchain for recreate so
 * it takes effect on the next frame. Does not affect render targets created via
 * rhi_create_render_target (the A/B/diff path stays at its native resolution). */
void        rhi_set_ssaa(rhi_device *dev, int n);

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
 * on, draw calls are recorded between begin and end. */
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

/* Diagnostics: the selected physical device name (valid until destroy). */
const char *rhi_device_name(const rhi_device *dev);

/* Read the offscreen color target into a host RGBA8 buffer (byte order
 * R,G,B,A, alpha forced opaque), for screenshots. Call after a rendered
 * frame (rhi_end_frame) — it reads the last frame's offscreen contents.
 * `out` must hold >= (*out_w)*(*out_h)*4 bytes; the extent is written back.
 * Returns false on failure (e.g. out too small). Blocks the GPU. */
bool rhi_read_offscreen(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h);

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

typedef struct {
    const char    *vert_spv;    /* filename within RHI_SHADER_DIR */
    const char    *frag_spv;
    rhi_blend_mode blend;
    bool           depth_test;  /* needs a depth attachment (added later) */
    bool           depth_write_off; /* depth_test but NO depth write (transparent: test vs opaque, don't occlude) */
    bool           uses_vram;   /* layout includes a sampler at set 0          */
    bool           uses_vram2;  /* second sampler at set 1 (e.g. the diff pass:
                                 * set 0 = native render, set 1 = reference)   */
    uint32_t       push_const_size; /* fragment push-constant block bytes (0=none) */
} rhi_pipeline_desc;

/* Host-visible vertex buffer (map-and-memcpy updates). */
rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes);
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
/* Upload the whole CPU mirror (width*height uint16 PSX pixels). Sub-rect
 * uploads (for runtime LoadImage streaming) arrive with 1c. */
void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels);

/* An RGBA8 (R8G8B8A8_UNORM, bytes R,G,B,A) sampled image — e.g. a frame read
 * back from another renderer for A/B display. Shares the VRAM texture's
 * descriptor layout, so it binds via rhi_bind_vram and is drawn by a textured
 * pipeline that samples a normal sampler2D (shaders/blit.frag). `pixels` is
 * width*height*4 bytes. */
rhi_texture *rhi_create_rgba8(rhi_device *dev, uint32_t width, uint32_t height);
void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels);

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

/* Clear the depth attachment to far WITHIN the current pass (offscreen or render
 * target). Used to depth-isolate a late on-top overlay (3D HUD icons) so it
 * Z-sorts internally without being occluded by, or occluding, world geometry. */
void rhi_clear_depth(rhi_device *dev);

/* Read a render target back into a host RGBA8 buffer (byte order R,G,B,A, alpha
 * forced opaque). The RT must be in SHADER_READ_ONLY (i.e. after
 * rhi_end_render_target + the frame that rendered it was submitted); the layout
 * is restored on return. `out` must hold >= width*height*4 bytes; the RT's
 * extent is written back. Returns false on failure. Blocks the GPU. Used by the
 * screenshot dump to capture the native scene at the reference resolution (no
 * window letterbox) regardless of window size. */
bool rhi_read_render_target(rhi_device *dev, rhi_texture *rt, void *out,
                            size_t out_bytes, int *out_w, int *out_h);

void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex);

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
/* Set the current pipeline's fragment push constants (size must match the
 * pipeline desc's push_const_size). */
void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size);
void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex);

#endif /* OPENPETE_RHI_H */
