/* openpete_launcher_sky.h - the launcher's sky pass: a level's cyclorama dome
 * (the mesh the asset extraction writes into the game's launcher cache)
 * drawn as the launcher's background, under its imgui screens.
 *
 * The launcher's thread hands the next present a view
 * (openpete_native_launcher_sky_next): up to two layers, the current dome
 * and the incoming one, each opaque, faded by an opacity, or revealed through
 * a noise-edged disc. Each layer is drawn with the stencil cleared: its
 * triangles with RHI_STENCIL_ONCE, so a pixel two triangles cover blends
 * once, then one triangle over the target in the level's clear colour, which
 * the same stencil confines to the pixels no triangle covered. The pipeline
 * is described in the ahead compile's first group (NR_AHEAD_LAUNCHER) and
 * created on its first draw. A dome's vertices are uploaded on its first
 * draw, keyed by its serial, and freed once no draw has used them for
 * LAUNCHER_SKY_RETIRE_DRAWS draws.
 *
 * The layers draw into a target of their own at LAUNCHER_SKY_SS samples per
 * offscreen pixel per axis (fewer when the target would pass the device's
 * image limit or LAUNCHER_SKY_SS_MAX_TEXELS), before the offscreen pass
 * opens; inside it one quad writes each offscreen pixel as the mean of its
 * samples in linear light (resolve.frag, the scene SSAA's resolve). At one
 * sample, or for a present whose extent the target does not match, the
 * layers draw straight into the offscreen pass. */
#ifndef OPENPETE_LAUNCHER_SKY_H
#define OPENPETE_LAUNCHER_SKY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rhi_device;

/* One level's dome mesh in host memory, as the launcher cache stores it. */
typedef struct openpete_sky_mesh {
    uint64_t        serial;   /* unique per mesh loaded; the GPU copy is keyed by it */
    uint32_t        nv, ntri;
    const uint8_t*  verts;    /* nv records of 9 bytes: the unit direction (x, y up, z) as
                               * three little-endian int16 scaled by 32767, then r, g, b */
    const uint16_t* tris;     /* ntri * 3 vertex indices, each below nv */
    uint8_t         bg[3];    /* the level's clear colour */
} openpete_sky_mesh;

/* One layer of the sky. */
typedef struct openpete_sky_layer {
    const openpete_sky_mesh* mesh;   /* NULL: the layer draws nothing */
    float alpha;                     /* opacity when not revealed */
    int   reveal;                    /* nonzero: the reveal mask is the opacity */
} openpete_sky_layer;

/* What one present draws behind the screens. */
typedef struct openpete_sky_view {
    openpete_sky_layer layer[2];   /* drawn in order over the clear colour */
    float yaw_deg;                 /* the pan */
    float pitch_deg;               /* up from level */
    float hfov_deg;                /* horizontal field of view */
    float clock_s;                 /* the reveal noise's clock, seconds */
    float reveal;                  /* the reveal's progress, 0..1 */
    float reveal_u, reveal_v;      /* its centre, 0..1 across and down the target */
    float reveal_soft;             /* the half-width of its edge, in target heights */
    float vignette;                /* radial darkening at the corners, 0..1 */
    float grain;                   /* per-pixel noise amplitude, 0..1 of full scale */
    float seed;                    /* the grain's offset, changed per present */
} openpete_sky_view;

/* Draws after which a dome's GPU copy no draw used is freed. */
#define LAUNCHER_SKY_RETIRE_DRAWS 4u

/* Samples per offscreen pixel per axis. */
#define LAUNCHER_SKY_SS 2
/* The supersampled target's largest texel count: 2x per axis up to a
 * 2560x1600 offscreen image; a larger image draws at one sample. At 2x a
 * 4096x2304 image needs an 8192x4608 target, 151 MB of colour before its
 * depth-stencil attachment, for a background of vertex-colour gradients
 * whose edges are a few pixels wide at that size. */
#define LAUNCHER_SKY_SS_MAX_TEXELS (2560LL * 1600LL * 4LL)

/** @brief Hand the next launcher present on the launcher's device a sky view, copied; NULL clears it.
 *
 * The meshes the view names must stay valid until that present returns; the
 * present consumes the view, so a present with none set draws the clear
 * colour alone. Launcher's thread only.
 * @param v The view, or NULL. */
void openpete_native_launcher_sky_next(const openpete_sky_view* v);

/** @brief Describe the sky and resolve pipelines into the open description (rhi_pipeline_describe_begin), in the group the caller set.
 *
 * Display owner only.
 * @param dev The device in description mode. */
void launcher_sky_describe(struct rhi_device* dev);

/** @brief Size the supersampled target for the offscreen extent, before the launcher's frame begins.
 *
 * Does nothing without a view. Creates the pipelines and the target when
 * the factor (LAUNCHER_SKY_SS, capped) exceeds 1; a target of another size
 * is destroyed after the device idles. Display owner only, outside a frame.
 * @param dev The launcher's device. */
void launcher_sky_before_frame(struct rhi_device* dev);

/** @brief Draw the view's layers into the supersampled target, inside the frame and before the offscreen pass opens, consuming the view.
 *
 * Does nothing without a view or a target matching the offscreen extent;
 * launcher_sky_draw then draws the layers itself.
 * @param dev The launcher's device.
 * @param r The offscreen pass's clear red, the target's clear.
 * @param g Its green.
 * @param b Its blue. */
void launcher_sky_prepass(struct rhi_device* dev, float r, float g, float b);

/** @brief Inside the open offscreen pass, resolve the target launcher_sky_prepass drew this present, else draw the view handed by openpete_native_launcher_sky_next at one sample per pixel; the view is then cleared.
 *
 * Creates the pipeline on the first draw and uploads each mesh on its first
 * draw; frees the copies no draw used for LAUNCHER_SKY_RETIRE_DRAWS draws.
 * A mesh whose upload fails, or a pipeline that cannot be created, draws
 * nothing. Display owner only, inside the launcher's offscreen pass.
 * @param dev The launcher's device.
 * @return The layers drawn. */
int launcher_sky_draw(struct rhi_device* dev);

/** @brief Release the sky pass's pipelines, target and every mesh copy on a device, waiting it idle first when it holds any.
 *
 * Called before the launcher's device is destroyed or adopted by bring-up.
 * When any present was handed a view, logs the presents that drew a layer,
 * the layers drawn (revealed, crossfaded), the triangles drawn, the
 * meshes uploaded and the presents resolved from the target. Display owner
 * only.
 * @param dev The launcher's device. */
void launcher_sky_release(struct rhi_device* dev);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_LAUNCHER_SKY_H */
