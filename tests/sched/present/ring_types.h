/* ring_types.h - the scene ring's slot and the native renderer state the
 * copied ring functions read, shared by the generated ring translation unit
 * and the harness.
 *
 * scene_slot_t holds every field the copied functions read; each payload
 * field has the smallest type the copies compile against (a triangle run
 * with a data pointer, single-member cover and snap tables). The scheduling
 * fields carry native_render.c's types. */
#ifndef PT_RING_TYPES_H
#define PT_RING_TYPES_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "openpete_time.h"
#include "mod_vec.h"
#include "rhi.h"
#include "pt_state.h"

#ifndef PT_SCENE_RING
#error "PT_SCENE_RING (the harness's scene ring size) is not defined"
#endif
#define SCENE_RING PT_SCENE_RING

/** A triangle of a scene: the tag the census reads. */
typedef struct { uint32_t tag; } scene_tri_t;
/** The portal planes a scene carries. */
typedef struct { int n; } nr_cover_block;
/** The contact planes a scene carries. */
typedef struct { int ne; } nr_snap_table;
/** The interned material slots a scene carries. */
typedef struct { int n; } openpete_mods_material_view_t;

/** One ring slot. */
typedef struct {
    scene_tri_t *tri; int n;
    mod_fvec  tri_v;
    uint32_t bg, fade, freeze; int hud_under, actor_front;
    int replay;
    unsigned  frame;
    uint32_t  stamp;
    nr_cover_block cover;
    nr_snap_table  snap;
    openpete_mods_material_view_t matview;
    uint32_t  scene_id;
    op_time_t target_ns;
    double    alpha; int paired; int kind;
    int       state;
    uint64_t  presented_serial;
} scene_slot_t;

/** The native renderer state the copies and the compose stand-in read. */
typedef struct {
    rhi_device *dev;
    int enabled, headless;
    SDL_Window *window;
    int win_w, win_h, pending_w, pending_h;
    int rec_pinned;
    uint64_t pending_since_ns, resize_immediate_at_ns;
} pt_nr_state;

extern scene_slot_t s_ring[SCENE_RING];
extern int s_ring_head;
extern void *s_ring_mx;
extern struct pt_ring_census { unsigned published, refused_full, superseded, presented; } s_ring_census;
extern pt_nr_state s_nr;
extern const scene_slot_t *s_ps_slot;
extern int s_stage_target;

/** @brief Compose the open scene inline on the calling thread, as openpete_native_render_frame does, through the compose stand-in.
 * @param tag The content id the fake device records for the images this compose writes. */
void pt_inline_compose(uint32_t tag);

#endif
