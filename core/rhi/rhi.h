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

/* Change the swapchain present mode (vsync) at runtime. `vsync` has the same
 * meaning as rhi_create's argument: 1 = FIFO (vsync ON), 0 = a vsync-OFF mode
 * (immediate/mailbox), -1 = unset (defer to env, else FIFO). Flags the swapchain
 * for recreation; the next rhi_begin_frame rebuilds it with the new present mode
 * (choose_present_mode). No-op on a headless device (no swapchain) or when the
 * preference is unchanged. Display-only. */
void rhi_set_vsync(rhi_device *dev, int vsync);

/* The device's current vsync preference (the value rhi_create/rhi_set_vsync
 * last received): 1 = FIFO, 0 = a vsync-OFF mode, -1 = unset (env, else FIFO —
 * the surface decides, so there is no resolved 0/1 to report yet). */
int rhi_get_vsync(const rhi_device *dev);

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
/* Monotonic count of rhi_begin_frame calls. A pass that must write its ring
 * vertex buffer more than once per frame (e.g. one batch per render target)
 * bump-allocates within the buffer and resets its offset when this changes —
 * a second same-frame rhi_update_buffer to the same offset clobbers data a
 * recorded-but-unexecuted pass will read. */
uint64_t rhi_frame_serial(rhi_device *dev);

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
/* Re-begin the offscreen pass LOADing off_image's existing contents (the
 * SCENE postfx point, S5): after a SCENE-point chain post-processed the
 * scene into off_image (leaving it TRANSFER_SRC), resume drawing (HUD)
 * over it. Depth clears. Pairs with rhi_end_offscreen. */
void rhi_begin_offscreen_load(rhi_device *dev);

/* Blit the offscreen color target onto the acquired swapchain image
 * (linear filter; handles the required layout transitions). */
void rhi_blit_to_swapchain(rhi_device *dev);

/* Submit the recorded command buffer and present the swapchain image. */
void rhi_end_frame(rhi_device *dev);

/* Tell the device the window size changed; the swapchain + offscreen
 * target are recreated on the next rhi_begin_frame. Safe to call on every
 * resize event. */
void rhi_notify_resize(rhi_device *dev);
/* Same, for an app-driven (discrete) size change such as a fullscreen toggle:
 * rebuilds at the next begin_frame rather than waiting out the settle window
 * that exists to absorb a live drag's per-pixel steps. */
void rhi_notify_resize_immediate(rhi_device *dev);

/* Register a handler called once on an unrecoverable VK_ERROR_DEVICE_LOST
 * (acquire/present). Lets the owning layer flag a clean shutdown; the RHI itself
 * stays free of core-runtime symbols. NULL clears it. Global, not per-device. */
void rhi_set_device_lost_handler(void (*fn)(void));

/* Diagnostics: the selected physical device name (valid until destroy). */
const char *rhi_device_name(const rhi_device *dev);

/* The present mode the swapchain was actually GRANTED, as a short name
 * ("fifo" / "immediate" / "mailbox"). The requested mode proves nothing: a
 * surface that does not advertise IMMEDIATE silently falls back to FIFO, and
 * a pacing baseline taken against the wrong one is not comparable. */
const char *rhi_present_mode_name(const rhi_device *dev);

/* Driver version of the selected device, vendor-decoded. Part of a pacing
 * baseline's environment key: a driver update changes present behaviour and
 * must not compare as the same machine. */
const char *rhi_driver_version(const rhi_device *dev);

/* Present-path block time, for the pacing log. Under the host thread split
 * the presenting thread is also the sim thread, so time blocked here is time
 * the tick did not run — which makes "which call blocked, and for how long"
 * a pacing quantity rather than a rendering one. Read-and-clear: each call
 * returns the nanoseconds accumulated since the previous one. */
enum { RHI_BLOCK_FENCE = 0, RHI_BLOCK_ACQUIRE, RHI_BLOCK_PRESENT,
       RHI_BLOCK_RECREATE, RHI_BLOCK_IMAGE_FENCE, RHI_BLOCK_PHASE_COUNT };
void rhi_take_block_times(int64_t *out, int n);

/* Presents skipped because the slot's previous submission had not retired.
 * Windowed only: rather than block the presenting thread on the display's
 * cadence — which under the host thread split is also the sim thread — the
 * frame is dropped. Monotonic; the caller diffs it. */
uint64_t rhi_fence_sheds(const rhi_device *dev);

/* Presents the ACQUIRE path refused, and how many of those were an expiry of
 * the acquire budget rather than a skip inside the back-off it opened. Both
 * are monotonic.
 *
 * Without these two, an acquire that waited and got an image is
 * indistinguishable from one that waited, gave up, and suppressed the next
 * several presents — and only the second means frames stopped reaching the
 * screen. The pacing log recorded both as presents. */
uint64_t rhi_acquire_sheds(const rhi_device *dev);
uint64_t rhi_acquire_timeouts(const rhi_device *dev);

/* Images the swapchain actually granted (we ask for minImageCount + 1; the
 * driver may give more). A back-pressure diagnosis is not checkable without
 * it. */
uint32_t rhi_swapchain_image_count(const rhi_device *dev);
/* True when the picked physical device is a software rasterizer (llvmpipe
 * reports VK_PHYSICAL_DEVICE_TYPE_CPU). Heavy vertex-shader paths (the
 * terrain sector-mesh cache) auto-disable on it. */
bool        rhi_device_is_software(const rhi_device *dev);
/* shaderFloat64 enabled on this device (the retained-terrain vertex stage
 * needs fp64 to mirror the CPU walk's double math bit-for-bit). */
bool        rhi_device_has_float64(const rhi_device *dev);

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
    float    koff;      /* og OT KEY (location 5): the face's ABSOLUTE .s
                         * insertion key in world units (corner-mean view-Z
                         * + 128*slot bias) + 0.5 sentinel; 0 = not keyed /
                         * contest off. POSITIVE: the face both EMITS its
                         * flat key into the key image (terrain_key.vert)
                         * and CONTESTS it (psx_scene_key.frag et al).
                         * NEGATIVE (|koff| = key): CONTEST-ONLY — near-band
                         * terrain, whose huge under-camera footprints must
                         * never stamp the image. Stored depth stays TRUE
                         * view-Z — faithfulness lives in the fragment
                         * SELECT, never in the depth value.                 */
    float    sx, sy;    /* PSX screen coord (location 3) — the textured shadow
                         * decal's depth-pre-pass lookup rides here, because its
                         * u,v are the sprite texel (unlike the untextured Spyro
                         * decal, which parks the screen coord in u,v). Ignored
                         * by every pipeline whose shader omits location 3.     */
    float    w;         /* perspective W = TRUE view-Z (location 4). z encodes
                         * ORDERING and may carry a feeder's OT-slot shift, so
                         * deriving W from it warps UV/colour interpolation.
                         * 0 = no shift: the vertex stages derive W from z.     */
} rhi_vertex;

typedef struct rhi_buffer   rhi_buffer;
typedef struct rhi_pipeline rhi_pipeline;
typedef struct rhi_texture  rhi_texture;

/* Terrain sector-mesh vertex (the terrain GPU cache — see
 * notes/plans/openpete-terrain-gpu-cache.md). A retained world-space
 * vertex: the camera transform, fog, CLUT ladder and the per-face verdicts
 * (NCLIP two-half cull, near/HQ variant select, LOD handoff) all run in the
 * vertex shader, so every vertex carries its PARENT FACE's four corner
 * positions and both tiledef halves' material/UV alongside its own data.
 * Positions are sector-local with 2 FRACTIONAL bits (value = world/16 * 4):
 * the HQ 2x2 subdivide's centre is the corner mean, a quarter-unit quantity.
 * Layout is load-bearing: the Vulkan backend's terrain vertex-input
 * attributes and terrain.vert's `location` declarations mirror these
 * offsets. */
typedef struct rhi_terrain_vertex {   /* tagged: forward-declared by the
                                       * terrain-cache header */
    int16_t  px, py, pz;        /* position, sector-local, x4 fixed point   */
    uint16_t flags16;           /* bit0: HQ-subdivide variant (vs flat/LP);
                                 * bits1-2: HQ sub-quad index (0..3);
                                 * bits3-4: corner slot within the sub-quad
                                 * (0..3 in [TL,TR,BL,BR] order);
                                 * bits5-11: scroll tile id (flat: Tiledef,
                                 * HQ: record id); bit12: tile id valid —
                                 * the vertex stage adds the globals' per-tile
                                 * fractional V-scroll offset;
                                 * bit13: SECOND emitted tri of the (sub-)quad
                                 * — the vertex belongs to half 2 / sub-tri 2,
                                 * so per-emitted-tri verdicts (the portal
                                 * inner-rim cull) can rebuild their own tri  */
    uint8_t  uvf_u, uvf_v;      /* far-tile (LQ) texel coords, page-relative */
    uint8_t  uvn_u, uvn_v;      /* near-tile texel coords (HP textured only) */
    uint32_t rgba_far;          /* far colour (LP: THE colour; HQ: the baked
                                 * raw-near/midpoint colour), R,G,B,A        */
    uint32_t rgba_near;         /* near colour (HP flat; else == rgba_far)   */
    uint32_t mat_far0;          /* tpx(10) | tpy(9)<<10 | bppc(2)<<19 |
                                 * blend(2)<<21 | textured(1)<<23
                                 * (bppc: 0=4bpp, 1=8bpp, 2=16bpp)           */
    uint32_t mat_far1;          /* clut_raw halfword (paletted only)         */
    uint32_t mat_near0;         /* near-tile material, same packing          */
    uint32_t mat_near1;
    uint32_t face_flags;        /* sector_slot(9) | tier(1)<<9 | fogbias<<10 |
                                 * gapfill<<11 | dside<<12 (LP w0&0x80 /
                                 * HP w3&0x4) | flip<<13 | quad<<14 |
                                 * w3_40<<15 | w3_80<<16 | hq_eligible<<17 |
                                 * floor<<18 (LP up-facing) |
                                 * admit(5)<<19 (LP authored admission) |
                                 * otbias(5)<<24 (authored OT-slot bias)     */
    int16_t  c0x, c0y, c0z;     /* parent face corners in i0,i1,i2,i3 order, */
    int16_t  c1x, c1y, c1z;     /* sector-local, same x4 fixed point         */
    int16_t  c2x, c2y, c2z;     /* (c2==c3 for tris)                         */
    int16_t  c3x, c3y, c3z;
} rhi_terrain_vertex;           /* 64 bytes */

/* Poly-anim chunks only: previous-tick twin of the animated fields, a
 * PARALLEL array in the anim arenas' prev buffer (vertex binding 1). The
 * vertex stage lerps prev->cur in fp64 at the build's sub-tick alpha —
 * position for flat/LP verts, parent corners for everything corner-derived
 * (HQ nodes, fog ladder, NCLIP) — mirroring tsc_vanim_lerp's
 * float(p + (c-p)*a) cast exactly. A sector whose banks jumped (keyframe
 * reset) or has no prev bank yet bakes prev == cur (the CPU snap rule). */
typedef struct rhi_terrain_prev {
    int16_t  px, py, pz, pad;   /* prev position, sector-local x4 fixed */
    int16_t  c0x, c0y, c0z;     /* prev parent corners, i0,i1,i2,i3     */
    int16_t  c1x, c1y, c1z;
    int16_t  c2x, c2y, c2z;
    int16_t  c3x, c3y, c3z;
} rhi_terrain_prev;             /* 32 bytes */

/* Near-variant chunks only: the OTHER two emitted-tri members' attribute
 * context, a parallel array in the near arenas' ctx buffer (vertex binding
 * 1). The near-variant vertex stage rebuilds its whole emitted triangle
 * (positions come from the parent corners already in the base vertex; UVs
 * and both-half colours for the two non-self members come from here, in
 * emit order relative to self: a = (self+1)%3, b = (self+2)%3), then
 * mirrors the feed's near-plane clip on it and takes its own slot of the
 * clipped fan. */
typedef struct rhi_terrain_nearctx {
    uint8_t  a_uf, a_vf, a_un, a_vn;   /* member a: far/near-half UVs   */
    uint32_t a_far, a_near;            /* member a: far/near colours    */
    uint8_t  b_uf, b_vf, b_un, b_vn;   /* member b likewise             */
    uint32_t b_far, b_near;
} rhi_terrain_nearctx;          /* 24 bytes */

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
    bool           depth_overwrite; /* depth_test with compare ALWAYS: every
                                 * rasterized fragment REPLACES the stored
                                 * depth, whatever was there. Pair with
                                 * color_write_off for a depth-only pass that
                                 * evicts an occluder from exactly the pixels
                                 * some geometry covers (the dragon-rescue
                                 * front pass). Note the depth TEST must stay
                                 * enabled to write at all — Vulkan disables
                                 * depth writes whenever depthTestEnable is
                                 * false — hence compare ALWAYS rather than
                                 * "write without testing". */
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
    bool           uses_vram3;  /* third sampler at set 2 (the terrain og-key
                                 * image). Implies sets 0..2 exist in the
                                 * layout; sets a shader never statically
                                 * accesses need no binding at draw.           */
    bool           uses_vram4;  /* fourth sampler at set 3 (the MOBY og-key
                                 * image — split from the terrain keys so the
                                 * near-band embed rule applies to moby keys
                                 * only). Implies uses_vram3.                  */
    bool           uses_vram5;  /* fifth sampler at set 4 (the moby og-RANK
                                 * image: per-pixel winning paint rank +
                                 * present-local moby ordinal, RGBA8 — the
                                 * same-moby painter select). Implies
                                 * uses_vram4.                                 */
    uint32_t       push_const_size; /* fragment push-constant block bytes (0=none) */
    bool           postfx;      /* mod-postfx layout: NO vertex input (fullscreen
                                 * triangle from gl_VertexIndex — pair with the
                                 * embedded postfx_fs.vert), sets 0..2 = one
                                 * sampler each (prev pass / depth / prev frame),
                                 * set 3 = the 2-binding dynamic UBO layout
                                 * (engine block, mod block — rhi_ubo_bind).
                                 * uses_vram/uses_vram2 are ignored.           */
    bool           mod_pipeline; /* mod custom-draw layout (S7): the STANDARD
                                 * rhi_vertex input + the postfx 4-set layout
                                 * (sets 0..2 samplers, set 3 UBO pair), so
                                 * mod vert/frag pairs see attributes at
                                 * locations 0..2 and the engine UBO at set 3
                                 * binding 0. uses_vram/uses_vram2 ignored.   */
    bool           material_pipeline; /* mod MATERIAL layout: mod_pipeline's
                                 * sets 0..3 (samplers, then the UBO pair)
                                 * PLUS sets 4..6 = the engine's og-key
                                 * occlusion inputs (terrain keys, moby keys,
                                 * actor keys / rank), which a material's
                                 * fragment stage evaluates before the mod's
                                 * colour maths. Above the UBO pair, not
                                 * replacing sets 1/2, so a material's own
                                 * uniform block stays at set 3 binding 1.
                                 * Needs maxBoundDescriptorSets >= 7; pipeline
                                 * creation FAILS (caller draws stock) below
                                 * that. Implies mod_pipeline's vertex input. */
    bool           terrain_pipeline; /* terrain sector-mesh layout: vertex
                                 * input = rhi_terrain_vertex (locations 0..8),
                                 * sets = {0: VRAM sampler, 1/2: og key images,
                                 * 3: the material contract's UBO pair,
                                 * 4: HD-replacement atlas sampler,
                                 * 5: TerrainGlobals, 6: TerrainSectors — both
                                 * single-binding via rhi_ubo_bind_at},
                                 * push constants visible to VERTEX|FRAGMENT
                                 * (the per-draw clip affine + class select).
                                 * The engine's own two blocks sit ABOVE the
                                 * contract set rather than inside it: set 3
                                 * binding 1 belongs to the mod, and one
                                 * descriptor set views one buffer per binding,
                                 * so terrain uniforms and a mod's uniforms
                                 * cannot share it. Needs
                                 * maxBoundDescriptorSets >= 7 (as materials
                                 * do); below that, pipeline creation fails and
                                 * the cache stays off. uses_vram/uses_vram2
                                 * ignored.                                   */
    bool           terrain_anim; /* with terrain_pipeline: add vertex binding 1
                                 * = rhi_terrain_prev (locations 9..12), the
                                 * previous-tick position + parent corners for
                                 * the vertex stage's sub-tick lerp (poly-anim
                                 * sector chunks). Bind the prev buffer with
                                 * rhi_bind_vertex_buffer2.                   */
    bool           terrain_near; /* with terrain_pipeline: add vertex binding 1
                                 * = rhi_terrain_nearctx (locations 9..12), the
                                 * emitted-tri member context for the vertex
                                 * stage's near-plane clip mirror (near-variant
                                 * chunks). Mutually exclusive with
                                 * terrain_anim; bind the ctx buffer with
                                 * rhi_bind_vertex_buffer2.                   */
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
/* SINGLE-instance vertex buffer preferring DEVICE_LOCAL host-mapped memory
 * (ReBAR): for retained geometry the GPU fetches every present but the CPU
 * writes rarely (the terrain sector-mesh arena). Falls back to the plain
 * host-visible type. Same overlap rules as rhi_create_vertex_buffer. */
rhi_buffer *rhi_create_vertex_buffer_device(rhi_device *dev, size_t bytes);
rhi_buffer *rhi_create_vertex_buffer_dynamic(rhi_device *dev, size_t bytes);
void        rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes);
/* Ranged variant: write `bytes` at byte `offset` within the (per-slot)
 * buffer. For passes that bump-allocate several batches into one ring slot
 * per frame (the terrain key pass) — a plain update at offset 0 would
 * clobber batches an earlier recorded pass reads. */
void        rhi_update_buffer_range(rhi_buffer *buf, const void *data, size_t offset, size_t bytes);
void        rhi_destroy_buffer(rhi_device *dev, rhi_buffer *buf);

rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc);
void          rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe);

/* Select the scene shaders' depth encoding (vertex spec-constant 0), applied to
 * pipelines created AFTER this call: 0 = legacy z=vz/(vz+4096), 1 = screen-affine
 * z_ndc=1-1/vz. Call once at bring-up (from OPENPETE_DEPTH_AFFINE) before the
 * scene pipelines are built. Default 0. */
void          rhi_set_depth_affine(int on);

/* Select PSX-affine UV/colour interpolation (vertex spec-constant 1, w=1 —
 * the PSX GPU's screen-linear texture/gouraud swim), applied to pipelines
 * created AFTER this call. Call at bring-up (from --texture-mapping) before
 * the scene pipelines are built; for a live toggle, follow with
 * rhi_rebuild_pipelines. Default 0 = perspective-correct. */
void          rhi_set_affine_uv(int on);

/* Untextured lighting view (fragment spec-constant 2): the textured scene
 * fragments replace the sampled texel with modulation-neutral grey so the
 * output is the raw gouraud vertex colour; transparency cutouts keep their
 * real texel shapes. Applied to pipelines created after this call; live
 * toggle via rhi_rebuild_pipelines. Default 0. */
void          rhi_set_notex(int on);

/* Key-contest scoping (fragment spec-constant 3): moby og-key yields
 * additionally require the key to beat the covering fragment's own TRUE
 * view-Z. Applied to pipelines created after this call; live toggle via
 * rhi_rebuild_pipelines. Default 1; 0 restores og's compound painter. */
void          rhi_set_moby_truescope(int on);

/* Re-bake EVERY live pipeline on the device from its stored desc + shader
 * sources, capturing the current spec-constant values (rhi_set_depth_affine /
 * rhi_set_affine_uv) — the live-toggle path for constants that are otherwise
 * latched at pipeline creation. Waits for device idle (one-time hitch), then
 * swaps each pipeline's Vulkan objects IN PLACE: every rhi_pipeline* handle
 * stays valid, so owners (renderer fields, mod material/mesh caches) need no
 * involvement. A pipeline whose recompile fails keeps its old objects (stale
 * constants but functional). Returns the number of failures (0 = clean). */
int           rhi_rebuild_pipelines(rhi_device *dev);

/* Pipeline from IN-MEMORY SPIR-V (mod shaders — S2 of
 * plans/openpete-mod-shaders.md). Words are copied into driver objects
 * during the call; the caller keeps ownership. Either side may be NULL
 * to fall back to the desc's embedded filename (e.g. mod postfx: vert
 * NULL + desc->vert_spv = "postfx_fs.vert.spv", frag from the mod). */
rhi_pipeline *rhi_create_pipeline_spv(rhi_device *dev,
                                      const rhi_pipeline_desc *desc,
                                      const uint32_t *vert_words, size_t vert_n,
                                      const uint32_t *frag_words, size_t frag_n);

/* ---- uniform-buffer blocks (S2: postfx/custom-pipeline uniforms) ----
 * A rhi_ubo is `blocks` fixed-size uniform blocks × the frame ring, in
 * one HOST_VISIBLE buffer, with a 2-binding DYNAMIC-offset descriptor
 * set (binding 0 = block_a, binding 1 = block_b at bind time; both
 * visible to vertex+fragment). block_size is rounded up to the device's
 * minUniformBufferOffsetAlignment. Write targets the CURRENT frame slot
 * (same parity contract as rhi_create_vertex_buffer_dynamic: write
 * every frame that binds). Bind attaches set 3 of a postfx pipeline.
 *
 * rhi_ubo_bind_at attaches ONE block through a single-binding layout at
 * an arbitrary set index instead. Set 3 is the shader-contract set — its
 * binding 0 is the engine block and its binding 1 is the MOD's block, and
 * a descriptor set can only view one buffer per binding, so an engine
 * subsystem with uniforms of its own has to put them somewhere a mod
 * never looks. That is what this is for. */
typedef struct rhi_ubo rhi_ubo;
rhi_ubo *rhi_create_ubo(rhi_device *dev, uint32_t block_size, uint32_t blocks);
void     rhi_ubo_write(rhi_ubo *ubo, uint32_t block, const void *data,
                       uint32_t len);
void     rhi_ubo_bind(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                      uint32_t block_b);
void     rhi_ubo_bind_at(rhi_device *dev, rhi_ubo *ubo, uint32_t set,
                         uint32_t block);
void     rhi_destroy_ubo(rhi_device *dev, rhi_ubo *ubo);

/* ---- offscreen-as-input (S2: the COMPOSITE postfx point) ------------
 * rhi_offscreen_to_sampled: barrier off_image TRANSFER_SRC →
 * SHADER_READ (call between render passes) so a postfx pass can sample
 * the composed frame. rhi_bind_offscreen_tex: bind it as the sampler at
 * `set` (inside a pass, after the pipeline bind). resume_attachment:
 * the return barrier before re-beginning the offscreen pass (fragment
 * reads → color writes) for a chain's final pass, whose off_rp end
 * restores TRANSFER_SRC for the present blit. */
void rhi_offscreen_to_sampled(rhi_device *dev);
void rhi_bind_offscreen_tex(rhi_device *dev, uint32_t set);
void rhi_offscreen_resume_attachment(rhi_device *dev);
void rhi_offscreen_resume_blit_src(rhi_device *dev);
/* Bind any texture/RT's sampler set at an arbitrary set index (postfx
 * inputs live at sets 0..2; rhi_bind_vram/vram2 remain the 0/1 pair). */
void rhi_bind_texture_set(rhi_device *dev, rhi_texture *tex, uint32_t set);
/* Postfx write-back blits: final ping RT → off_image (leaves it
 * TRANSFER_SRC for the present blit) / → another RT (leaves it
 * SHADER_READ for readback). Source is restored to SHADER_READ. */
void rhi_blit_rt_to_offscreen(rhi_device *dev, rhi_texture *src);
void rhi_blit_rt_to_rt(rhi_device *dev, rhi_texture *dst, rhi_texture *src);

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

/* Sub-rect uploads from TIGHT per-rect source buffers (pixels = w*h*4 RGBA8
 * bytes each) — the HD-atlas append path. Same in-frame/one-shot semantics
 * as rhi_update_rgba8; rects that overflow the staging slot are dropped
 * (re-queue them next frame). At most 96 rects per call. */
typedef struct { const void *pixels; int x, y, w, h; } rhi_tex_rect_src;
void rhi_update_rgba8_rects(rhi_device *dev, rhi_texture *tex,
                            const rhi_tex_rect_src *rects, int nrects);

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
/* Bind binding 0 + binding 1 (terrain_anim pipelines: cur verts + prev twin). */
void rhi_bind_vertex_buffer2(rhi_device *dev, rhi_buffer *buf, rhi_buffer *prev);
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

/* ------------------------------------------------------------------ */
/* GPU per-pass timestamp zones (env OPENPETE_GPU_TIMERS=1).
 *
 * Bracket a named unit of GPU work between begin_frame and end_frame:
 *
 *     rhi_gpu_zone_begin(dev, "prepass");
 *     native_depth_prepass();
 *     rhi_gpu_zone_end(dev);
 *
 * Zones MAY nest (begin/end pair like a stack): a zone begun while another is
 * open becomes its child, and the result carries a `depth`. `name` must be a
 * string literal or otherwise outlive the frame — it is stored by pointer and
 * handed back in results a few presents later.
 * Results are harvested by rhi_begin_frame after the slot's fence wait
 * (never blocks; frame-lagged by RHI_FRAMES_IN_FLIGHT presents) and fetched
 * with rhi_gpu_zone_results — returns the zone count; each result has elapsed
 * µs (timestampPeriod applied), `start_us` (µs from the frame's first GPU
 * timestamp, for laying zones out on a timeline), and nesting `depth`.
 * rhi_gpu_frame_token tags the current frame with a caller id (e.g. the
 * present_index); rhi_gpu_results_token returns the tag of the harvested
 * (frame-lagged) results so the caller can attribute them to the right present.
 * All zone calls are no-ops when the env is unset or the queue lacks timestamp
 * support (one boot log line either way). */
#define RHI_GPU_ZONES_MAX 128
typedef struct { const char *name; double us; double start_us; int depth; } rhi_gpu_zone_result;
int  rhi_gpu_timers_on(const rhi_device *dev);
void rhi_gpu_zone_begin(rhi_device *dev, const char *name);
void rhi_gpu_zone_end(rhi_device *dev);
int  rhi_gpu_zone_results(rhi_device *dev, rhi_gpu_zone_result *out, int cap);
void rhi_gpu_frame_token(rhi_device *dev, uint64_t token);
uint64_t rhi_gpu_results_token(const rhi_device *dev);

/* ---- dev-UI (imgui) integration seam -------------------------------------
 *
 * The ONE deliberate hole in the opaque-handle abstraction: the Dear ImGui
 * Vulkan backend must be initialized against the real device + the offscreen
 * render pass. Handles are typed as void pointers / uint64_t so rhi.h stays
 * Vulkan-free; the sole consumer (core/runtime/devui.cpp) casts them back.
 *
 * render_pass is the offscreen pass (off_rp). It IS destroyed+recreated on a
 * swapchain rebuild, but always with identical attachment formats — and a
 * VkPipeline only uses its creation render pass for compatibility, so a
 * pipeline built against the old pass stays valid under the new one (the
 * renderer's own pipelines rely on the same rule). No re-init needed on
 * resize/vsync changes.
 *
 * command_buffer() is the frame's open command buffer, valid only between
 * rhi_begin_frame and rhi_end_frame — the dev UI records its draw data into
 * the offscreen pass, alongside the other overlays. */
typedef struct {
    void    *instance;         /* VkInstance */
    void    *physical_device;  /* VkPhysicalDevice */
    void    *device;           /* VkDevice */
    uint32_t queue_family;
    void    *queue;            /* VkQueue (graphics) */
    uint64_t render_pass;      /* VkRenderPass (offscreen color+depth pass) */
    uint32_t image_count;      /* swapchain image count (>= 2) */
    uint32_t api_version;      /* VK_API_VERSION the instance was created with */
} rhi_imgui_info;
/* False on a headless device (no window — the dev UI never initializes). */
bool  rhi_get_imgui_info(rhi_device *dev, rhi_imgui_info *out);
void *rhi_current_command_buffer(rhi_device *dev);

#endif /* OPENPETE_RHI_H */
