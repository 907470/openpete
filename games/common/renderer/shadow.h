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

/* A projected point: screen position in float plus the GTE's MAC1..MAC3
 * (sz = MAC3, the view-Z). */
typedef struct { float sx, sy, sz; float m1, m2; } shadow_pt;

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

/* A player ground-shadow fan as the footprint decal (shadow_decal.frag) draws
 * it: the anchor and the 16 ring points as projected, the triangles the
 * routine linked, and the camera the points were projected through. The
 * shadow is the fan's own surface where the depth rule keeps it, united with
 * the terrain under the ring's vertical projection between the fan and the
 * caster; the drawn geometry is the screen convex hull of that prism, and the
 * fragment stage decides every pixel from the fan data the feed hands the
 * renderer (openpete_shadow_footprint). */
#define SHADOW_FAN_RING 16
typedef struct {
    shadow_pt anchor;
    shadow_pt ring[SHADOW_FAN_RING];
    uint32_t  tri_mask;      /* bit k: (anchor, ring k, ring k+1) is linked */
    int       slot[SHADOW_FAN_RING]; /* triangle k's ordering-table slot */
    uint32_t  anchor_rgb;    /* the gouraud at the anchor; the rim is 0 */
    double    anchor_up;     /* world height (game z) of the anchor */
    double    caster_up;     /* world height of the caster, read from the same
                              * staged player record as the anchor's block */
    int       C[3][3];       /* camera matrix the points projected through */
    double    ofx, ofy, H;   /* projection registers of that view */
} shadow_fan;

/* Hand the renderer the fan's footprint data and feed, as untextured
 * subtractive SHADOW triangles under the current scene tag, the linked fan
 * triangles with their gouraud (vertex alpha 1: what the depth-tested
 * fallback pipe draws; the decal discards them) and the prism hull (vertex
 * alpha 0, black: what the decal shades; the fallback subtracts nothing). */
void   shadow_fan_feed(CPUState* cpu, const shadow_fan* f);
/* The caster height a player feeder stores beside its anchor
 * (shadow_fan.caster_up); `anchor_up` when the game has no player block. */
double shadow_caster_up(CPUState* cpu, double anchor_up);

/* The ordering-table slot a shadow fan triangle is inserted at. Every shadow
 * routine keys a (anchor, p1, p2) triangle the same way: the sum of the two
 * rim points' SZ plus half that sum plus the anchor's SZ, shifted into slot
 * units and less the routine's bias. The SZ values are the GTE's 16-bit
 * saturated view-Z; `shift` is 9 for the player fans (full-world view-Z) and
 * 7 for the moby shadows (quarter-world view-Z), so both land on the terrain's
 * 128-world slots. A negative slot is a triangle the routine does not link. */
int    shadow_ot_slot(float sza, float sz1, float sz2, int shift, int bias);
/* A slot as the scene tri's key channel (openpete_scene_tri.koff): the slot's
 * absolute key in world units plus the 0.5 sentinel; 0 (not keyed) for a
 * negative slot. */
float  shadow_slot_koff(int slot);

/* View-Z to native depth. `scale` lifts a walk's view-Z to the one shared
 * full-world scale (4 for a walk working in world/4 coordinates). */
float  shadow_depth(float viewz, float scale, float bias);
float  shadow_bias_spyro(void);
float  shadow_bias_moby(void);

/* OPENPETE_SHADOW_VERIFY: run the A/B harness and the one-shot dump. */
int      shadow_verify_enabled(void);
unsigned shadow_verify_frame(void);

/** @brief Sub-tick re-extract: rebuild the moby drop-shadows from the current
 * gamestate, extract-only, when the last canonical tick rendered them
 * (RP_MOBY_SHADOW canon-drawn).
 * @param cpu guest state, the sub-tick view staged */
void   shadow_reextract(CPUState* cpu);

/* The same for the player's ground shadow (renderer/shadow_player.c, the leg
 * game_layout.shadow.player describes). `slot` is the model record the fan
 * renders under; the caller stages that record's shadow state first. */
void   shadow_player_reextract(CPUState* cpu, int slot);

/* OPENPETE_SHADOW_PLAYER=0 suppresses the player's own ground shadow, whatever
 * renderer a game draws it with, leaving the moby drop-shadows alone on the
 * shadow render path: the A/B that prices the player fan's painted pixels
 * without a second binary, and what leaves a leave-one-out of the shadow path
 * holding moby shadows only. The guest draw still runs. */
int    shadow_player_feed_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_SHADOW_H */
