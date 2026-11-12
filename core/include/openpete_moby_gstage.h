/* openpete_moby_gstage.h - the moby vertex stage's readback against the CPU
 * projection (OPENPETE_MOBY_GSTAGE_AB, diagnostic, unregistered).
 *
 * The moby feeder fills one slot per projected vertex, indexed by the
 * vertex's position in its projection array, with the inputs the GPU stage
 * (core/rhi/shaders/moby_stage.glsl) reads and the CPU projection's own
 * outputs, and one instance record at each record's first vertex. Each
 * record that reaches the scene is committed into a snapshot of the scene
 * epoch it fed. A dump picks the snapshot whose vertices the presented
 * scene's moby triangles carry, uploads its records into a read-only storage
 * buffer, draws the stage's readback variant (moby_rb.vert) as points into
 * an RGBA32_UINT target twice, the stage and its negative control, and
 * writes the CPU arm and the read texels beside the dump
 * (tools/moby_gstage_ab.py pairs them). The capture writes host memory
 * only; every Vulkan call is made by the dump on the present side. */
#ifndef OPENPETE_MOBY_GSTAGE_H
#define OPENPETE_MOBY_GSTAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rhi_device;

/* Slots per projection array: one per projected vertex (the record passes'
 * 32768 and the shaded pass's 8192 after them). */
#define OPENPETE_GSAB_PROJ_CAP 40960

/* Instance flags (moby_stage.glsl MOBY_F_*). */
enum {
    OPENPETE_GSAB_F_GPF    = 1u,    /* the record's GPF translation rescale ran */
    OPENPETE_GSAB_F_ANIM   = 2u,    /* two keyframes or two poses blend */
    OPENPETE_GSAB_F_LERP   = 4u,    /* the sub-tick previous-to-current pose lerp ran */
    OPENPETE_GSAB_F_MIRROR = 8u,    /* the rotation's determinant is negative */
    OPENPETE_GSAB_F_LIFT   = 16u,   /* a near-floor lift moved the record */
    OPENPETE_GSAB_F_SHELL  = 32u,   /* the sphere-shell pull moved the record */
    OPENPETE_GSAB_F_ENV    = 64u,   /* the record's metal byte is set (env-UV faces) */
    OPENPETE_GSAB_F_SHADED = 128u,  /* a shaded-pass record (unit 2, or 64 / draw-scale byte) */
    OPENPETE_GSAB_F_BILLBOARD = 256u, /* a shaded-pass billboard (screen-position offset) */
};

/* One record's per-record stage inputs, each rounded once to float. */
typedef struct {
    float M[3][3];      /* model-to-view rotation, 1.0 = 1.0 */
    float TR[3];        /* projection translation, rounded to float */
    float TR_lo[3];     /* the translation less TR: the double as a float pair */
    float ofx, ofy;     /* screen offsets */
    float H, H_canon;   /* display and GTE projection distance */
    float div_cap;      /* capped quotient times the --fov scale */
    float unit;         /* projected view-Z to the perspective W */
    float depth_k;      /* projected to written depth */
    float vz_bias;      /* GPF depth restore */
    float tie_pull;     /* tie pull, 0 when off */
    float lift;         /* decode near-floor lift, 0 when none */
    float rho;          /* sphere-shell pull, 0 when none */
    float lift2;        /* shell near-floor lift, 0 when none */
    float alpha;        /* pose lerp phase */
    uint32_t flags;     /* OPENPETE_GSAB_F_* */
    int n;              /* the record's vertex count */
    uint32_t serial;    /* the decode that wrote the record */
} openpete_gsab_inst;

/* One vertex: the stage's inputs and the CPU projection's outputs. */
typedef struct {
    float c[3], p[3];   /* current and previous model-space vertex */
    double sx, sy;      /* screen position (the GTE pair in the regime) */
    double mac3;        /* projected view-Z */
    double tvz;         /* perspective W */
    double hx, hy;      /* homogeneous screen position */
    double ex, ey, ez;  /* rotated offset */
    float vz, kvz;      /* written and key depth, as the walk reads them */
    uint8_t unr, sat;   /* regime and saturation flags */
} openpete_gsab_vert;

/** @brief Return whether the readback runs: OPENPETE_MOBY_GSTAGE_AB set and not "0", latched on first call.
 * @return 1 when on, else 0. */
int openpete_gsab_on(void);

/** @brief Return the instance slot of the record whose first vertex sits at a projection index, opened for a decode.
 *
 * Stamps the slot with a fresh decode serial and zeroes it.
 * @param idx The record's first projection index.
 * @return The slot, or NULL when the readback is off or idx is outside 0..OPENPETE_GSAB_PROJ_CAP-1. */
openpete_gsab_inst *openpete_gsab_rec_open(int idx);

/** @brief Return the instance slot at a projection index without opening it.
 * @param idx The projection index.
 * @return The slot, or NULL when the readback is off or idx is out of range. */
openpete_gsab_inst *openpete_gsab_rec_at(int idx);

/** @brief Return the vertex slot at a projection index.
 * @param idx The projection index.
 * @return The slot, or NULL when the readback is off or idx is out of range. */
openpete_gsab_vert *openpete_gsab_vert_at(int idx);

/** @brief Commit a record's slots into the snapshot of the scene epoch it fed.
 *
 * The first commit of a new epoch closes the snapshot being built into a
 * ring of recent snapshots and opens one for the epoch. A record whose
 * instance slot does not hold n vertices (another decode reused the slots)
 * is counted and skipped.
 * @param epoch The scene epoch (openpete_native_scene_epoch).
 * @param idx The record's first projection index.
 * @param n The record's vertex count.
 * @param mid The moby's scene identity (openpete_scene_tri.mid).
 * @param flags Flags known only at the scene push, ORed into the record's. */
void openpete_gsab_commit(unsigned epoch, int idx, int n, uint32_t mid, uint32_t flags);

/** @brief Draw the readback of the snapshot the presented scene shows into its own target.
 *
 * Call inside a frame, outside any render pass. The snapshot is the one of
 * the recent snapshots whose (identity, screen x, screen y) triples most
 * scene triangles' vertices carry. Nothing presented is drawn or changed.
 * @param dev Device.
 * @param tris The presented scene's triangles; each begins with an openpete_scene_tri.
 * @param n Their count.
 * @param stride Bytes between triangles.
 * @return 1 when a readback was recorded, else 0. */
int openpete_gsab_dump_record(struct rhi_device *dev, const void *tris, int n, size_t stride);

/** @brief Read the recorded target and write the CPU arm and the texels to a file.
 *
 * Waits for the device. Does nothing when no readback is recorded.
 * @param path Output path.
 * @return 0 written, 1 nothing recorded, -1 on failure. */
int openpete_gsab_dump_write(const char *path);

/** @brief Destroy the readback's GPU resources (renderer shutdown).
 * @param dev Device. */
void openpete_gsab_shutdown(struct rhi_device *dev);

#ifdef __cplusplus
}
#endif

#endif
