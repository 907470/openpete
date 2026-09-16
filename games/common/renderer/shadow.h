/* shadow.h — the moby drop-shadow feeder and the pieces a game's own
 * player-shadow feeder reuses.
 *
 * The moby walk in renderer/shadow.c reads every engine address and every
 * decode switch from game_layout.shadow, so one body serves both games. A
 * game whose player shadow is a separate renderer (Spyro 1's silhouette fan)
 * supplies its own feeder and reaches the polybuf decoder, the projection
 * helpers and the shadow-decal parameter push through this header. */
#ifndef OPENPETE_COMMON_RENDERER_SHADOW_H
#define OPENPETE_COMMON_RENDERER_SHADOW_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"
#include "openpete_native_render.h"   /* openpete_shadow_decal_params */

#ifdef __cplusplus
extern "C" {
#endif

/* One primitive decoded out of the polybuf: the ground truth an A/B compares
 * the native walk against. `nv` is 0 for a packet that is not a polygon (the
 * draw-mode mini-packet a player-shadow fan bundles ahead of each triangle). */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1] >> 24) */
    uint8_t  len;           /* OT tag length (word[0] >> 24) */
    int      nv;            /* 3 or 4 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} shadow_prim;

/* Decode [start, end) of the polybuf into the shared capture buffer. */
void                shadow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end);
int                 shadow_prim_count(void);
const shadow_prim*  shadow_prim_at(int i);
/* Print the capture's code histogram and the first few decoded primitives. */
void                shadow_dump_prims(const char* tag);

/* A projected point: screen position in float plus the GTE's view-Z (MAC3). */
typedef struct { float sx, sy, sz; } shadow_pt;

int    shadow_s16(int v);
int    shadow_rd_s16(CPUState* cpu, uint32_t addr);
/* MVMVA(sf=1, mx=rotation, v=V0, cv=none, lm=0): IR = clamp16((R . v) >> 12). */
void   shadow_mvmva_ir(const int R[3][3], int vx, int vy, int vz, int ir[3]);
/* MAC1..MAC3 of the camera-relative ground anchor, the GTE's TR for the
 * corners that follow. */
void   shadow_ground_tr(const int C[3][3], double vx, double vy, double vz, double TR[3]);
/* RTPS one local vertex through R and an integer TR, GTE-faithful. */
void   shadow_project(const int R[3][3], const double TR[3], double ofx, double ofy,
                      const ngte_view* v, double vx, double vy, double vz, shadow_pt* o);

/* Push the depth-aware shadow decal's camera-reconstruction parameters. Both
 * feeders call it so the parameters are valid whenever any shadow draws.
 * `out`, when non-NULL, also receives the pushed parameters. */
void   shadow_push_decal_params(CPUState* cpu, const ngte_view* v, const int C[3][3],
                                double ofx, double ofy, double H,
                                openpete_shadow_decal_params* out);

/* View-Z to native depth. `scale` lifts a walk's view-Z to the one shared
 * full-world scale (4 for a walk working in world/4 coordinates). */
float  shadow_depth(float viewz, float scale, float bias);
float  shadow_bias_spyro(void);
float  shadow_bias_moby(void);

/* OPENPETE_SHADOW_VERIFY: run the A/B harness and the one-shot dump. */
int      shadow_verify_enabled(void);
unsigned shadow_verify_frame(void);

/* Sub-tick re-extract: rebuild the moby drop-shadows from the current
 * gamestate. Extract-only. */
void   shadow_reextract(CPUState* cpu);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_SHADOW_H */
