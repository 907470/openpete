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

typedef enum { RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD } rhi_blend_mode;

typedef struct {
    const char    *vert_spv;    /* filename within RHI_SHADER_DIR */
    const char    *frag_spv;
    rhi_blend_mode blend;
    bool           depth_test;  /* needs a depth attachment (added later) */
    bool           uses_vram;   /* layout includes the VRAM sampler (set 0)   */
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
void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex);

/* Recorded between rhi_begin_offscreen and rhi_end_offscreen. */
void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe);
void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf);
/* Bind the VRAM texture for sampling by the current (uses_vram) pipeline;
 * call after rhi_bind_pipeline, before the draw. */
void rhi_bind_vram(rhi_device *dev, rhi_texture *tex);
/* Set the current pipeline's fragment push constants (size must match the
 * pipeline desc's push_const_size). */
void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size);
void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex);

#endif /* OPENPETE_RHI_H */
