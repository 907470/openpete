// games/common/renderer/moby.c - the MOBY (3D actor) renderer, shared across
// games.
//
// Mobys are the animated, textured actors: enemies, gems, props, HUD icons.
// A game's cull pass writes one render record per drawn moby into the render
// scratch; one or more draw passes then walk those records, decode each
// model's packed vertex stream and per-face material stream, project through
// the GTE and emit. This module is the scene-extraction half: it reads the
// records, re-derives the same geometry in float (mirroring
// core/psx/gte.c RTPS, with a real TR translation), and produces
// screen-space triangles for the native scene.
//
// Every engine address the walk reads comes from game_layout.moby (the moby
// array and its field offsets, the model table, the AnimationHeader layout,
// the record base/stride/fields, the draw hooks). The two stream encodings
// and the record shape are moby_game decoder slots
// (renderer/moby_game.h), as is the per-game data with no engine address
// behind it (HUD and cutscene classification, the one-shot instance table)
// and the addresses of scratch windows and material assets outside the
// descriptor.
//
// Verification (instruments over pixels): hook a draw pass, capture the
// actual prims it emits to the polybuf as ground truth, run the native float
// extraction on the same records, compare positions + colours + UV +
// tpage/clut per vertex. Headless, numerical. Gated by OPENPETE_MOBY_VERIFY.
//
// Determinism: leaves PSX RAM unchanged. When active it calls the real render
// function exactly once (behaviour-identical to the normal dispatch
// fall-through) and afterwards only READS the records + polybuf, except the
// keyframe faceblock patch mirror, which writes patch words into a faceblock
// and restores the originals within one decode (moby_frame_patch_apply).

#include "openpete_fiber.h"  /* openpete_feed_key_now: instrument line labels */
#include "feature_registry.h"
#include "renderer/orchestrator.h"  /* the canonical-render epoch + producer-camera brackets */
#include "renderer/moby.h"          /* this feeder's exported entries */
#include "renderer/moby_game.h"     /* per-game decoders, classifications and addresses */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "openpete_psx.h"
#include "openpete_vstream.h"       /* frame-word field -> stream address (handles) */
#include "openpete_subtick_view.h"            /* openpete_subtick_host_span (SPEC scratch bracket) */
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "no_lod.h"                /* openpete_no_lod */
#include "openpete_native_render.h"
#include "openpete_log.h"           /* OPL_ERROR_F - ABR/blend FIXME runtime alarms */
#include "openpete_interp.h"        /* op_now() - sub-tick clock for rotation interp */
#include "renderer/moby_rank.h"              /* same-moby paint rank (native key) */
#include "renderer/moby_rank_cal.h"          /* its calibration instrument */
#include "openpete_interp_objs.h"   /* per-object prev/curr snapshots (rotation interp) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/cut_guard.h"     /* the per-object discontinuity hold */
#include "openpete_ui_moby.h"  /* HUD/UI moby classifier (overlay-on-top tagging) */
#include "openpete_mods.h"     /* per-instance material slot resolve */
#include "openpete_workpool.h" /* data-parallel face walk (--extract-threads) */

#include <math.h>
#include <stdint.h>
#include <limits.h>   /* INT32_MAX/INT32_MIN: the removed extended-LOD cull ceiling */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"          /* game_layout.moby: the engine addresses this walk reads */
#include "openpete_recomp_module.h"  /* the game's own moby draws, run through the module dispatch */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */

extern op_time_t g_openpete_tick_stamp;   /* the newest object-capture stamp (openpete_interp_objs.c) */

extern unsigned openpete_frame;

/** @brief Run one of the game's own moby draw passes through the recomp module's dispatch.
 *
 * @param cpu The guest CPU state.
 * @param addr The draw routine's guest address. */
static void moby_run_draw(CPUState* cpu, uint32_t addr) {
    openpete_recomp_module_get()->dispatch(cpu, addr);
}

/** @brief The address of the first Moby in the moby array, 0 when the game names no array head.
 *
 * game_layout.moby.array_head is the array itself where the game keeps it at
 * a fixed address (array_direct), else a word holding the first Moby's
 * address, read here.
 * @param cpu The guest CPU state, read only.
 * @return The first Moby's address, or 0. */
static uint32_t moby_array_head(CPUState* cpu) {
    if (!game_layout.moby.array_head) return 0u;
    return game_layout.moby.array_direct ? game_layout.moby.array_head
                                         : cpu->read_word(game_layout.moby.array_head);
}

static const char* s_cur_mat = "REG";   /* material tag for the current pass (REG/SPEC) */
/* Clip-switch crossfade census (OPENPETE_MOBY_ANIMSW_DBG): how often a sub-tick moby
 * shape-lerp spans a clip switch (prev m_Animation != curr) - crossfaded when the two
 * clips share vertex count, snapped when they don't. Pure instrument, no behaviour. */
static long s_animsw_cross = 0, s_animsw_snap = 0, s_animsw_mirror = 0;
/* Clip-switch pairs shown from held vertices (heldx) and refused for want of
 * them (noheld), counted on every present the verdict runs. */
static long s_animsw_heldx = 0, s_animsw_noheld = 0;
/* Clip-switch LERP and HOLD verdicts whose two headers carry different scale
 * bytes (model units), counted on every present the verdict runs. */
static long s_animsw_scale_lerp = 0, s_animsw_scale_hold = 0;
/* Sub-tick previous-pose guard census, counted on presents below alpha 1:
 * holds shown, holds ended by the tick-period rule, and clip-switch pairs
 * refused because the previous header's content no longer matches the
 * signature the canonical pass drew it with (sig) or no canonical pass drew
 * that header for the moby within the last two headers it showed (unk). */
static long s_hold_shown = 0, s_hold_period = 0, s_prevhdr_sig = 0, s_prevhdr_unk = 0;
static long s_shmode[3]; /* shaded face-mode census: [0]=gouraud [1]=flat-basic [2]=flat-metal(bit1) */
/* Shaded metal census: [0] front kept, [1] back kept, [2] kept in the near
 * band, [3] blended, [4..6] mobys whose DR_TPAGE ABR is 0, 1, 2, [7] front
 * triangles composed over the back faces, [8] front triangles left blended. */
static long s_metal_cnt[9];
static int  s_sh_nohl = -1; /* OPENPETE_SHADED_NOHL=1 disables the gouraud spec boost (A/B) */

/** @brief The base address of the cull-record queue the game's cull pass built.
 *
 * Every material pass over the same model kind shares one queue. The base is
 * game_layout.moby.rec_base when the game fixes it, else the tick's
 * draw-scratch block rec_base_polybuf_off bytes below the polybuf end, which
 * moves with the draw buffer, so it is read per pass and never cached across
 * a tick.
 * @param cpu The guest CPU state, read only.
 * @return The queue's first record address. */
static uint32_t moby_rec_base(CPUState* cpu) {
    if (game_layout.moby.rec_base) return game_layout.moby.rec_base;
    return cpu->read_word(game_layout.polybuf.end) - game_layout.moby.rec_base_polybuf_off;
}
static uint32_t s_rec_base;   /* per-pass; set by moby_native_pass */
/* The base the last canonical pass read. A derived base rides the draw buffer
 * that tick allocated, and the allocator hands the next tick the other one, so
 * a sub-tick replay must use the base captured at the hook rather than
 * recompute it from a polybuf end that has since moved. */
static uint32_t s_rec_base_canon;
/* The queue the last canonical pass read, for sub-tick staging (moby.h). */
uint32_t moby_canon_rec_base(void)  { return s_rec_base_canon; }
uint32_t moby_canon_rec_bytes(void) { return game_layout.moby.rec_base_polybuf_off; }


#define REC_STRIDE    (game_layout.moby.rec_stride)
/* The AnimationHeader's geometry pointers, four bytes apart from
 * anim_ptr_block: HP faces, HP colours, LP faces, far colours (the walk
 * reaches the vertex array only through the game's own stream decoder). */
#define ANIM_FACES_HP (game_layout.moby.anim_ptr_block + 4u)
#define ANIM_COL_HP   (game_layout.moby.anim_ptr_block + 8u)
#define ANIM_FACES_LP (game_layout.moby.anim_ptr_block + 12u)
#define ANIM_COL_FAR  (game_layout.moby.anim_ptr_block + 16u)
/* Capacity for one pass's records / projected vertices, shared by the
 * queue walk and the host reveal builder. Extended LOD with the distance
 * ceiling removed feeds a whole level's mobys from a single distant vantage, so
 * these are sized for that rather than for the ~50 a canonical 4:3 view queues;
 * overflow BREAKS the reveal walk mid-list (a silent tail-of-the-level drop) and
 * truncates the straddling record's mesh. */
#define MAX_RECORDS   1024
#define SHADED_BASE   (moby_game.shaded_list)  /* second model kind: flat Moby* array (stride 4) */

/* ---- cull-record snapshot -------------------------------------------
 * The record list lives in the tick's draw scratch, and the draw pass that
 * follows the cull carves its own per-model vertex and faceblock-patch buffers
 * out of that same scratch. A record the draw has reached therefore holds that
 * draw's own output - packed screen XY and colour words - by the time this
 * feeder reads the list back after running the draw: its header field is no
 * longer a pointer, the walk stops or drops the record, and the moby is absent
 * from the native scene while the game's own pass drew it.
 *
 * Taking the list at the cull hook, while the bytes are still the cull's,
 * keeps every record readable. The copy holds the live records plus the
 * terminator word the walk stops on, since that word is inside the clobbered
 * range too. Reads outside the copy resolve against memory, so a game whose
 * cull is not hooked reads exactly what it read before. */
/* Sized for the walk's own record cap at the widest record the games use; a
 * wider stride copies fewer records and the rest read memory. */
#define REC_SNAP_RECS  MAX_RECORDS
#define REC_SNAP_WORDS (REC_SNAP_RECS * 0x44u / 4u + 1u)
/* Two copies: the one the current tick's cull filled, and the previous
 * canonical epoch's, which a held camera cut replays (moby_native_replay_prev_queue). */
static uint32_t s_rec_snap_buf[2][REC_SNAP_WORDS];
static uint32_t* s_rec_snap = s_rec_snap_buf[0];
static uint32_t s_rec_snap_base;    /* guest address the copy starts at */
static uint32_t s_rec_snap_words;   /* 0 = no copy; reads pass through */
/* The records the cull itself wrote, and whether the copy reached the cull's
 * terminator: the extended-LOD reveal appends its own records after them and
 * has to be able to put the terminator back where the cull left it. */
static uint32_t s_rec_snap_queued;
static int      s_rec_snap_ended;
static int      s_rec_snap_appended;
static void moby_rec_append_reset(void);

/* The mobys whose records this tick's reveal appended. The walk reads the
 * appended records out of the same copy the cull filled, so a triangle's origin
 * is not readable from the record it came from; the scene needs it to tell the
 * game's own geometry from what the reveal added. Bounded by the copy's record
 * count, and a reveal that overruns the array marks nothing, which
 * leaves the pre-reveal verdict. */
#define REC_APPEND_MOBYS 512
static uint32_t s_rec_append_moby[REC_APPEND_MOBYS];
static int      s_rec_append_moby_n;

/* The canonical epoch the copy was taken in, the object-capture stamp current
 * at that moment (every object state the tick's cull composed is stamped at or
 * before it), and the camera the cull composed the records against. */
static unsigned  s_rec_snap_epoch;
static op_time_t s_rec_snap_ref_ns;
static int       s_rec_snap_cam_ok;
static int16_t   s_rec_snap_cam_M[9];
static int32_t   s_rec_snap_cam_pos[3];

/* The previous canonical epoch's copy and everything above that describes it,
 * swapped wholesale with the current one by rec_snap_swap. */
static struct {
    uint32_t* snap;
    uint32_t  base, words, queued;
    int       ended, appended;
    uint32_t  append_moby[REC_APPEND_MOBYS];
    int       append_n;
    unsigned  epoch;
    op_time_t ref_ns;
    int       cam_ok;
    int16_t   cam_M[9];
    int32_t   cam_pos[3];
} s_rec_prev = { s_rec_snap_buf[1] };

/* While a held cut replays the previous queue: the records show the state the
 * objects had when that queue's cull ran, which is the snapshot pair's prev
 * for an object that rolled after s_rec_ref_ns and its curr otherwise. */
static int       s_rec_ref_prev;
static op_time_t s_rec_ref_ns;
/* That queue's cull camera, in the record re-base's form. The installed tick
 * camera stays the current tick's, which the second model kind's replay reads. */
static int       s_rec_cam_M[3][3];
static int32_t   s_rec_cam_pos[3];

/* Retired whenever the set is rebuilt, so a memo taken against one tick's set
 * cannot answer for the next. */
static unsigned s_rec_append_gen;

/** @brief Add a moby to this tick's appended-record set; a full set drops it.
 *
 * The set holds REC_APPEND_MOBYS entries. A moby past that is not marked, and
 * its records keep the verdict the walk gives a record the cull wrote.
 * @param moby The Moby address whose record a reveal appended. */
static void moby_rec_append_note(uint32_t moby) {
    if (s_rec_append_moby_n < REC_APPEND_MOBYS)
        s_rec_append_moby[s_rec_append_moby_n++] = moby;
}

/** @brief Whether a moby is in the appended-record set, by linear scan.
 *
 * @param moby The Moby address.
 * @return 1 when a reveal appended a record for the moby this tick, else 0. */
static int moby_rec_appended_moby(uint32_t moby) {
    for (int i = 0; i < s_rec_append_moby_n; i++)
        if (s_rec_append_moby[i] == moby) return 1;
    return 0;
}

/** @brief Whether a reveal appended a record for this moby, memoized on the pointer and the appended set's generation.
 *
 * The walk emits a moby's faces contiguously, so a per-triangle call costs
 * one scan of the appended set per moby; moby_rec_append_reset starts a new
 * generation, which invalidates the memo.
 * @param moby The Moby address.
 * @return 1 when the moby is in the appended set, else 0. */
static int moby_walk_tri_host_added(uint32_t moby) {
    static uint32_t last_moby;
    static unsigned last_gen;
    static int      last_hit;
    if (moby == last_moby && s_rec_append_gen == last_gen) return last_hit;
    last_moby = moby;
    last_gen  = s_rec_append_gen;
    last_hit  = moby_rec_appended_moby(moby);
    return last_hit;
}

/** @brief Exchange the current record copy, with its append set, epoch, stamp and camera, for the previous epoch's.
 *
 * The buffers trade by pointer; the append set by value. Retires the
 * appended-set generation, so no memo taken against one copy answers for the
 * other. */
static void rec_snap_swap(void) {
#define REC_SWAP(a, b) do { __typeof__(a) t_ = (a); (a) = (b); (b) = t_; } while (0)
    REC_SWAP(s_rec_snap, s_rec_prev.snap);
    REC_SWAP(s_rec_snap_base, s_rec_prev.base);
    REC_SWAP(s_rec_snap_words, s_rec_prev.words);
    REC_SWAP(s_rec_snap_queued, s_rec_prev.queued);
    REC_SWAP(s_rec_snap_ended, s_rec_prev.ended);
    REC_SWAP(s_rec_snap_appended, s_rec_prev.appended);
    REC_SWAP(s_rec_append_moby_n, s_rec_prev.append_n);
    REC_SWAP(s_rec_snap_epoch, s_rec_prev.epoch);
    REC_SWAP(s_rec_snap_ref_ns, s_rec_prev.ref_ns);
    REC_SWAP(s_rec_snap_cam_ok, s_rec_prev.cam_ok);
    for (int i = 0; i < REC_APPEND_MOBYS; i++) REC_SWAP(s_rec_append_moby[i], s_rec_prev.append_moby[i]);
    for (int i = 0; i < 9; i++) REC_SWAP(s_rec_snap_cam_M[i], s_rec_prev.cam_M[i]);
    for (int i = 0; i < 3; i++) REC_SWAP(s_rec_snap_cam_pos[i], s_rec_prev.cam_pos[i]);
#undef REC_SWAP
    s_rec_append_gen++;
}

/** @brief Copy the cull's record list and its terminator word into s_rec_snap.
 *
 * Reads records from moby_rec_base, game_layout.moby.rec_stride bytes apart,
 * until a record whose first word is 0 or the copy's capacity (REC_SNAP_RECS
 * records, REC_SNAP_WORDS words). The terminator is copied only when the
 * loop reached it. An unusable stride (0 or not a multiple of 4) or a base
 * below KSEG0 leaves no copy, and moby_rec_word then reads guest memory.
 * @param cpu The guest CPU state, read only. */
static void moby_rec_snapshot(CPUState* cpu) {
    /* The first copy of a new canonical epoch retires the current one, appends
     * included, to the previous slot; a second copy in the same epoch (a pause
     * screen feeding twice, runahead's speculated ticks) replaces the current. */
    const unsigned epoch = spyro_native_canon_epoch_stamp();
    if (s_rec_snap_words && s_rec_snap_epoch != epoch) rec_snap_swap();
    s_rec_snap_epoch  = epoch;
    s_rec_snap_ref_ns = g_openpete_tick_stamp;
    s_rec_snap_cam_ok = 0;
    if (game_layout.cam.proj_matrix && game_layout.cam.position) {
        int M[3][3];
        ngte_read_matrix(cpu, game_layout.cam.proj_matrix, M);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) s_rec_snap_cam_M[r * 3 + c] = (int16_t)M[r][c];
        for (int i = 0; i < 3; i++)
            s_rec_snap_cam_pos[i] = (int32_t)cpu->read_word(game_layout.cam.position + (uint32_t)i * 4u);
        s_rec_snap_cam_ok = 1;
    }
    s_rec_snap_words = 0;
    const uint32_t stride = game_layout.moby.rec_stride;
    if (!stride || (stride & 3u)) return;
    const uint32_t base = moby_rec_base(cpu);
    if (base < 0x80000000u) return;
    const uint32_t sw = stride / 4u;
    uint32_t n = 0;
    int ended = 0;
    while (n < REC_SNAP_RECS && (n + 1u) * sw < REC_SNAP_WORDS) {
        const uint32_t rec = base + n * stride;
        if (cpu->read_word(rec) == 0u) { ended = 1; break; }   /* the cull's terminator */
        for (uint32_t k = 0; k < sw; k++)
            s_rec_snap[n * sw + k] = cpu->read_word(rec + k * 4u);
        n++;
    }
    /* The terminator is copied only when the walk reached it. A copy that ran
     * out of room ends on a live record, and the records past it keep reading
     * memory: writing a zero there would stop the walk at the copy's end and
     * drop the tail of the queue. */
    if (ended) s_rec_snap[n * sw] = 0u;
    s_rec_snap_base  = base;
    s_rec_snap_words = n * sw + (ended ? 1u : 0u);
    s_rec_snap_queued = n;
    s_rec_snap_ended  = ended;
}

/** @brief Drop every appended record, leaving the copy exactly as the cull hook took it.
 *
 * The appends belong to one tick's reveal and are rebuilt on the next. Also
 * empties the appended-moby set and retires its generation, and puts the
 * terminator back after the cull's own records when the copy reached it. */
static void moby_rec_append_reset(void) {
    s_rec_append_moby_n = 0;
    s_rec_append_gen++;
    if (!s_rec_snap_words) return;
    const uint32_t sw = game_layout.moby.rec_stride / 4u;
    s_rec_snap_words = s_rec_snap_queued * sw + (s_rec_snap_ended ? 1u : 0u);
    if (s_rec_snap_ended) s_rec_snap[s_rec_snap_queued * sw] = 0u;
    s_rec_snap_appended = 0;
}

uint32_t* moby_rec_append(uint32_t* addr) {
    /* A copy that ran out of room ends on a live record, and the walk reads
     * the records past it out of guest memory, where an appended one would be
     * invisible. Only a copy that reached the cull's terminator has a tail
     * this can extend. */
    if (!s_rec_snap_words || !s_rec_snap_ended) return NULL;
    const uint32_t stride = game_layout.moby.rec_stride;
    const uint32_t sw = stride / 4u;
    const uint32_t n = s_rec_snap_queued + (uint32_t)s_rec_snap_appended;
    if (n >= REC_SNAP_RECS || (n + 2u) * sw > REC_SNAP_WORDS) return NULL;
    uint32_t* w = &s_rec_snap[n * sw];
    memset(w, 0, stride);
    w[sw] = 0u;                                   /* the terminator moves past it */
    s_rec_snap_words = (n + 1u) * sw + 1u;
    s_rec_snap_appended++;
    if (addr) *addr = s_rec_snap_base + n * stride;
    return w;
}

int moby_rec_holds_moby(CPUState* cpu, uint32_t moby) {
    if (!s_rec_snap_words || !moby_game.read_record) return 0;
    const uint32_t stride = game_layout.moby.rec_stride;
    const uint32_t n = s_rec_snap_queued + (uint32_t)s_rec_snap_appended;
    for (uint32_t r = 0; r < n; r++) {
        moby_record_t R;
        if (!moby_game.read_record(cpu, s_rec_snap_base + r * stride, &R)) break;
        if (R.moby == moby) return 1;
    }
    return 0;
}

uint32_t moby_rec_word(CPUState* cpu, uint32_t addr) {
    const uint32_t off = addr - s_rec_snap_base;
    if (s_rec_snap_words && (addr & 3u) == 0u && off < s_rec_snap_words * 4u)
        return s_rec_snap[off / 4u];
    return cpu->read_word(addr);
}

void moby_rec_matrix(CPUState* cpu, uint32_t addr, int M[3][3]) {
    ngte_unpack_matrix(moby_rec_word(cpu, addr),      moby_rec_word(cpu, addr + 4u),
                       moby_rec_word(cpu, addr + 8u), moby_rec_word(cpu, addr + 12u),
                       moby_rec_word(cpu, addr + 16u), M);
}

/* Sub-tick second-material scratch snapshot. A game whose second material
 * pass rebuilds the record queue with its own set - including mobys the first
 * pass did not draw - and overwrites the per-vertex colour scratch leaves the
 * first pass's snapshot unable to reproduce them. Capture the window
 * moby_game names at that pass's canonical hook and replay it in the
 * re-extract's second pass. The window must cover the FULL record region, not
 * just its head: a short window silently truncates the replayed queue and the
 * records past its end vanish from every sub-tick present. */
#define SPEC_SCR_LO   (moby_game.spec_window_off)
#define SPEC_SCR_LEN  (moby_game.spec_window_len)
#define SPEC_SCR_MAX  0x2000u                  /* storage bound for the snapshot */
static uint8_t s_canon_spec[SPEC_SCR_MAX];
static uint32_t s_canon_spec_epoch = 0;   /* canonical-render epoch at capture (freshness gate) */
static int     s_canon_spec_valid = 0;
/** @brief Copies the specular scratch window at the canonical moby hook for the sub-tick SPEC bracket. */
static void moby_native_capture_spec_scratch(CPUState* cpu) {
    /* Runahead: a speculative fire must not stamp or overwrite the canonical
     * SPEC capture - the stamp would be the uncommitted epoch and the SPEC
     * pass would skip on every intermediate present, strobing specular
     * mobys. */
    if (!spyro_native_canon_capture_open()) return;
    if (!SPEC_SCR_LEN || SPEC_SCR_LEN > SPEC_SCR_MAX) return;
    const void* h = openpete_subtick_host_span(moby_game.scratch + SPEC_SCR_LO, SPEC_SCR_LEN);
    if (h) { memcpy(s_canon_spec, h, SPEC_SCR_LEN); s_canon_spec_valid = 1;
             s_canon_spec_epoch = spyro_native_canon_epoch_stamp(); }
}

/* ---- captured prim (ground truth, decoded from the polybuf) --------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (prim[0x7]) */
    uint8_t  len;           /* OT tag length (prim[0]>>24) */
    int      nv;            /* 3 or 4 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];       /* first words, for the hex dump */
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
/* Cumulative prim-code coverage across the whole run (missing-poly diagnosis):
 * which GPU codes F798 emits, and which we DON'T decode (= dropped from feed). */
static int s_code_hist[256]; static int s_uncoded_hist[256];

/* ---- helpers (shared with the cyclorama harness conventions) -------- */
/* The projection view of the pass in flight: one per producer pass, built at
 * the pass entry (inside the producer-camera bracket on the sub-tick
 * re-extract, so a held producer camera reaches it). The regular and
 * specular walks, the reveal builder and the occlusion cache share s_view;
 * the shaded walk projects through s_view_shd. */
static ngte_view s_view;
static ngte_view s_view_shd;
/* Canonical twins (s = 1.0): the view for records whose screen position the
 * game authored - HUD icons (address-range tagged) and every billboard-path
 * record. Rebuilt with their source views. */
static ngte_view s_view_canon;
static ngte_view s_view_shd_canon;
/** @brief Build s_view from the guest camera and derive its canonical twin s_view_canon. */
static void moby_view_build(CPUState* cpu)     { ngte_view_build(cpu, &s_view);     ngte_view_canon(&s_view,     &s_view_canon); }
/** @brief Build s_view_shd from the guest camera and derive its canonical twin s_view_shd_canon. */
static void moby_view_shd_build(CPUState* cpu) { ngte_view_build(cpu, &s_view_shd); ngte_view_canon(&s_view_shd, &s_view_shd_canon); }
/* Nonzero while a regular pass walks the game's overlay moby list
 * (game_layout.moby.overlay_moby): every record of the pass draws HUD3D.
 * s_canon_overlay is the canonical pass's verdict, which the sub-tick
 * re-extract replays because the game restores the array head before the
 * present. */
static int s_pass_overlay;
static int s_canon_overlay;

/** @brief Whether the regular draw about to run walks the game's overlay moby list.
 *
 * The game builds that list as a one-moby array: the array head holds the
 * word at game_layout.moby.overlay_moby and the next element's state byte is
 * the array terminator.
 * @param cpu The guest CPU state, read only.
 * @return 1 for the overlay pass, else 0; 0 where the game names no overlay
 *         word. */
static int moby_overlay_pass_now(CPUState* cpu) {
    const uint32_t word = game_layout.moby.overlay_moby;
    if (!word || game_layout.moby.array_direct) return 0;
    const uint32_t mob = cpu->read_word(word);
    if (mob < 0x80000000u || cpu->read_word(game_layout.moby.array_head) != mob) return 0;
    return cpu->read_byte(mob + game_layout.moby.array_stride + game_layout.moby.state_off)
           == (uint8_t)game_layout.moby.state_term;
}

/** @brief Whether a regular-walk moby draws on the HUD: a static or UI HUD address, or any moby of the overlay pass.
 *
 * @param mob The Moby address.
 * @return 1 for a HUD moby, else 0. */
static int moby_is_hud(uint32_t mob) {
    return s_pass_overlay || openpete_ui_moby_is_addr_static(mob) || openpete_ui_moby_is_addr_ui(mob);
}

/** @brief The HUD tag of a regular-walk or second-kind moby: 2 HUD3D, 1 flat HUD, 0 actor.
 *
 * Every moby of the overlay pass and a moby in the static HUD address range
 * are HUD3D; a moby in the UI HUD range is flat HUD.
 * @param mob The Moby address.
 * @return The tag. */
static int moby_hud_tag(uint32_t mob) {
    if (s_pass_overlay || openpete_ui_moby_is_addr_static(mob)) return 2;
    return openpete_ui_moby_is_addr_ui(mob) ? 1 : 0;
}

/** @brief The regular-walk view a record projects through.
 *
 * A HUD moby (moby_is_hud) keeps the game's projection (s_view_canon); every
 * other moby takes the pass view (s_view).
 * @param mob The Moby address.
 * @return The view. */
static const ngte_view* moby_record_view(uint32_t mob) {
    return moby_is_hud(mob) ? &s_view_canon : &s_view;
}

/** @brief Float RTPS mirror with translation, through the shared ngte_project_raw.
 *
 * The caller owns the near-plane policy: the returned view Z is unfloored.
 * @param M The model-to-view rotation, 4096 = 1.0.
 * @param TR The projection translation.
 * @param vx The model-space vertex X.
 * @param vy The model-space vertex Y.
 * @param vz The model-space vertex Z.
 * @param ofx_px The screen offset X in pixels.
 * @param ofy_px The screen offset Y in pixels.
 * @param v The view, which supplies H.
 * @param out_sx Receives the screen X.
 * @param out_sy Receives the screen Y.
 * @param out_sat Receives the GTE's would-have-saturated flags.
 * @return The view Z (MAC3). */
static double project(const int M[3][3], const double TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    return ngte_project_raw(M, TR, vx, vy, vz, ofx_px, ofy_px, v, out_sx, out_sy, out_sat);
}

/** @brief GTE near-plane mirror: re-project one vertex through the GTE's divide cap when it lies in the saturation regime.
 *
 * project() is the float H/z: it keeps the true screen X the widescreen
 * reveal needs but has no counterpart to the GTE's UNR divide cap. RTPS
 * saturates the quotient at 0x1FFFF once SZ3 <= H/2 and clamps SXY to the
 * 11-bit signed window, so on hardware a vertex that close is pinned near the
 * screen edge, while the float divide by a small view Z sends it to |x| in
 * the hundred-thousands. Inside the regime (vzz <= 0 or H_canon >= 2 vzz, the
 * canonical H rather than the display's) the float coordinate carries nothing
 * the hardware had, so the vertex re-projects through ngte_project_gte with
 * the divide cap and the +-1024 SXY saturation. A vertex further than H/2
 * keeps its float projection. NGTE_LIMIT counts the regime at both H values.
 * @param M The model-to-view rotation, 4096 = 1.0.
 * @param TR The projection translation.
 * @param vx The model-space vertex X.
 * @param vy The model-space vertex Y.
 * @param vz The model-space vertex Z.
 * @param vzz The float projection's view Z.
 * @param ofx The screen offset X.
 * @param ofy The screen offset Y.
 * @param v The view.
 * @param sx Replaced with the GTE screen X inside the regime.
 * @param sy Replaced with the GTE screen Y inside the regime.
 * @return 1 when the vertex was in the regime, else 0. */
static int moby_gte_near_fixup(const int M[3][3], const double TR[3],
                               double vx, double vy, double vz, double vzz,
                               double ofx, double ofy, const ngte_view* v,
                               double* sx, double* sy) {
    int in = !(vzz > 0.0 && v->H_canon < 2.0 * vzz);   /* the GTE's regime: H_canon, not the display's H */
    NGTE_LIMIT(NGTE_SITE_MOBY_NEAR, in, !(vzz > 0.0 && v->H_eff < 2.0 * vzz));
    if (!in) return 0;
    ngte_proj gp;
    ngte_project_gte(M, TR, vx, vy, vz, ofx, ofy, v, 1, &gp);
    *sx = gp.sx; *sy = gp.sy;
    return 1;
}

/* Forward decl: the host-side moby record builder (widescreen reveal, below)
 * reuses the shaded path's euler matrix composer, defined later in this file.
 * Takes the moby vaddr (not the raw rotation word) so the composer can look up
 * the moby's prev/curr snapshots for sub-tick float rotation interpolation. */
static void shaded_compose_euler(CPUState* cpu, const int start[3][3], uint32_t moby, int out[3][3]);

/* Forward decls: the dragon-rescue cutscene actor selector and its ownership
 * window, defined with the cutscene struct offsets later in this file. The
 * widescreen reveal (below) consults both. */
static int moby_is_cutscene_actor(CPUState* cpu, uint32_t mob);
static int moby_cutscene_owns_draw(CPUState* cpu);

/* ---- ground-truth capture: decode prims F798 emitted ---------------- */
/** @brief Decode the primitives a moby render pass emitted in a polybuf interval into s_gte.
 *
 * s_gte is the ground truth the verify A/Bs compare the native decode
 * against. The walk steps by tag length (stride ((w0 >> 24) + 1) * 4 bytes),
 * independent of the per-code layout, and stops at a tag length of 0 or above
 * 24 or at MAX_PRIMS prims. Each prim keeps up to 20 raw words and, for a
 * polygon code ((code & 0xE0) == 0x20), its vertices decoded by command bit
 * (bit 4 gouraud, bit 3 quad, bit 2 textured): per vertex an RGB word when
 * gouraud past the first, the XY word, and a UV word when textured, with the
 * CLUT in vertex 0's UV high half and the tpage in vertex 1's. Every code
 * counts into s_code_hist; a code decoded to no vertices counts into
 * s_uncoded_hist and warns once per code, since its polygons are missing from
 * the native feed.
 * @param cpu The guest CPU state, read only.
 * @param start First polybuf address of the pass's output.
 * @param end One past the last. */
static void capture_gte(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag / corrupt - stop */
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);

        if (s_gte_n < MAX_PRIMS) {
            prim_t* t = &s_gte[s_gte_n];
            memset(t, 0, sizeof(*t));
            t->code = code; t->len = (uint8_t)len;
            t->nraw = (int)(stride / 4); if (t->nraw > 20) t->nraw = 20;
            for (int i = 0; i < t->nraw; i++) t->raw[i] = cpu->read_word(p + (uint32_t)i*4);
            /* GT3 (0x34, 10w) / GT4 (0x3C, 13w): per-vertex blocks of
             * {rgb|code, xy, uv|clut-or-tpage}. xy0 @ word[2], xy1 @ [5],
             * xy2 @ [8], xy3 @ [11]. */
            /* General PSX polygon decode by command bits (bit4=gouraud,
             * bit3=quad, bit2=textured). Handles every moby variant uniformly:
             * GT3/GT4 (0x34/0x3C), G3/G4 (0x30/0x38), FT3/FT4 (0x24/0x2C, incl.
             * the +0x02 semi-transparent 0x26/0x2E), F3/F4. Layout: word[1] =
             * rgb0|code, then per vertex {rgb if gouraud&&k>0}{xy}{uv if tex};
             * clut rides v0's uv-word hi, tpage rides v1's. */
            if ((code & 0xE0) == 0x20) {
                int gouraud = (code & 0x10) != 0;
                int quad    = (code & 0x08) != 0;
                int tex     = (code & 0x04) != 0;
                t->gouraud = gouraud; t->textured = tex; t->quad = quad;
                t->nv = quad ? 4 : 3;
                uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
                int c = 2;                          /* word cursor past rgb0|code */
                for (int k = 0; k < t->nv; k++) {
                    t->rgb[k] = (gouraud && k > 0) ? (t->raw[c++] & 0xFFFFFF) : rgb0;
                    uint32_t xy = t->raw[c++];
                    t->x[k] = (float)(int16_t)(xy & 0xFFFF);
                    t->y[k] = (float)(int16_t)(xy >> 16);
                    if (tex) {
                        uint32_t uv = t->raw[c++];
                        t->u[k] = (int)(uv & 0xFF); t->v[k] = (int)((uv >> 8) & 0xFF);
                        if (k == 0)      t->clut  = (int)(uv >> 16);
                        else if (k == 1) t->tpage = (int)(uv >> 16);
                    }
                }
            }
            s_code_hist[code & 0xFF]++;
            if (t->nv == 0) {
                s_uncoded_hist[code & 0xFF]++;  /* not fed = missing polys */
                /* Warn once per unknown code: skipping is only correct for
                 * explicitly culled geometry, never for a code the decoder
                 * does not recognize. */
                static uint8_t s_uncoded_warned[256];
                if (!s_uncoded_warned[code & 0xFF]) {
                    s_uncoded_warned[code & 0xFF] = 1;
                    fprintf(stderr, "native: WARNING undecoded moby prim code 0x%02X (vbl=%u) - polys "
                            "dropped from the native feed; add a decoder or explicitly cull\n",
                            code & 0xFF, openpete_frame);
                }
            }
            s_gte_n++;
        }
        p += stride;
    }
}

/** @brief Print the cumulative primitive-code coverage line of every captured moby draw pass.
 *
 * Lists each GPU code capture_gte has counted so far (s_code_hist) and the
 * codes it decoded to no vertices (s_uncoded_hist), whose polygons are
 * missing from the native feed. "UNDECODED(dropped): none" over a whole demo
 * run means the capture decodes every code the passes emit. The verify path
 * (OPENPETE_MOBY_VERIFY) prints it at most once per 2000 VBLs. */
static void report_code_hist(void) {
    char hb[400]; int hp = 0; hb[0] = 0;
    for (int c = 0; c < 256; c++) if (s_code_hist[c] && hp < (int)sizeof(hb) - 16)
        hp += snprintf(hb + hp, sizeof(hb) - hp, " %02X=%d", c, s_code_hist[c]);
    char ub[200]; int up = 0; ub[0] = 0;
    for (int c = 0; c < 256; c++) if (s_uncoded_hist[c] && up < (int)sizeof(ub) - 16)
        up += snprintf(ub + up, sizeof(ub) - up, " %02X=%d", c, s_uncoded_hist[c]);
    fprintf(stderr, "[moby-codes vbl=%u] cumulative F798 codes:%s | UNDECODED(dropped):%s\n",
            openpete_frame, hb, up ? ub : " none");
}

/** @brief Print the captured F798 prims, the first four cull records and a prim-code histogram to stderr.
 *
 * Each of the first four records prints its raw words, its decoded fields, its
 * rotation matrix and the leading words of the frame, faces and colour blocks
 * it points at (ngte_dump_words), and the loop stops at the terminator record.
 * The first ten captured prims then print raw, followed by a histogram of the
 * codes of all s_gte prims. The OPENPETE_MOBY_VERIFY path calls it once, on the
 * first VBL moby_verify_vbl selects.
 * @param cpu The guest CPU state, read only.
 * @param start The start of the prim stream the pass wrote.
 * @param end One past the end of that stream. */
static void hex_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    fprintf(stderr, "[moby-dump vbl=%u] F798 prims [%08X,%08X) %u bytes  OFX=%d OFY=%d H=%u\n",
            openpete_frame, start, end, end - start,
            (int32_t)cpu->gte_ctrl[24] >> 16, (int32_t)cpu->gte_ctrl[25] >> 16,
            cpu->gte_ctrl[26] & 0xFFFF);

    /* Records: first 4, with interpreted fields + followed pointers. */
    for (int r = 0; r < 4; r++) {
        uint32_t rec = s_rec_base + (uint32_t)r * REC_STRIDE;
        uint32_t w0 = moby_rec_word(cpu, rec);
        moby_record_t R; int live = moby_game.read_record(cpu, rec, &R);
        uint32_t frame_p = live ? R.header : 0u;
        uint32_t faces_p = live ? R.frame_a : 0u;
        uint32_t color_p = live ? R.frame_b : 0u;
        int32_t trx  = live ? R.TR_raw[0] : 0;
        int32_t tr_y = live ? R.TR_raw[1] : 0;
        int32_t trz  = live ? R.TR_raw[2] : 0;
        uint32_t mobyp = live ? R.moby : 0u;
        fprintf(stderr,
            "[rec %d @%08X] w0=%08X frame=%08X faces=%08X color=%08X TR=(%d,%d,%d) moby=%08X\n",
            r, rec, w0, frame_p, faces_p, color_p, trx, tr_y, trz, mobyp);
        if (w0 == 0 && frame_p == 0 && mobyp == 0) { fprintf(stderr, "  (terminator)\n"); break; }
        int M[3][3]; memcpy(M, R.M, sizeof M);
        fprintf(stderr, "  R=[%d %d %d | %d %d %d | %d %d %d]\n",
                M[0][0],M[0][1],M[0][2], M[1][0],M[1][1],M[1][2], M[2][0],M[2][1],M[2][2]);
        ngte_dump_words(cpu, "record", rec, 14);
        if (frame_p >= 0x80000000u && frame_p < 0x80800000u) ngte_dump_words(cpu, "->frame", frame_p, 12);
        if (faces_p >= 0x80000000u && faces_p < 0x80800000u) ngte_dump_words(cpu, "->faces", faces_p, 12);
        if (color_p >= 0x80000000u && color_p < 0x80800000u) ngte_dump_words(cpu, "->color", color_p, 8);
    }

    /* Prims: first 10 raw, with code/len. */
    int n = s_gte_n < 10 ? s_gte_n : 10;
    for (int i = 0; i < n; i++) {
        prim_t* t = &s_gte[i];
        fprintf(stderr, "[prim %d] code=%02X len=%u :", i, t->code, t->len);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        fprintf(stderr, "\n");
    }
    /* Code histogram. */
    int hist[256]; memset(hist, 0, sizeof(hist));
    for (int i = 0; i < s_gte_n; i++) hist[s_gte[i].code]++;
    fprintf(stderr, "[moby-dump] prim count=%d  codes:", s_gte_n);
    for (int c = 0; c < 256; c++) if (hist[c]) fprintf(stderr, " %02X=%d", c, hist[c]);
    fprintf(stderr, "\n");
}

/* ---- per-moby native vertex decode + projection -------------------- */
/* One moby vertex stream (frame A or B), decoded by the game's own codec
 * behind the moby_game vertex slots: the walk owns the loop and the
 * projection, the decoder owns what one step of the stream means. */
typedef moby_vstream_t vstream;
/** @brief Whether a record carries the pose (header, frame).
 *
 * A keyframe reference is a pointer into the model's frame array, or, where
 * the cull copies the keyframe's leading stream word into the record instead
 * (rec_frame_is_word), that word, which may hold any value, so the pose
 * exists exactly when its header does. A pointer exists when it is at or
 * above 0x80000000.
 * @param header The AnimationHeader.
 * @param frame The keyframe reference.
 * @return 1 when the pose is live, else 0. */
static inline int moby_pose_live(uint32_t header, uint32_t frame) {
    return game_layout.moby.rec_frame_is_word ? (header >= 0x80000000u)
                                              : (frame  >= 0x80000000u);
}
/* The keyframe pair one moby draws, derived from its animation state bytes
 * (Moby + anim_state_off: id, nextId, frame, nextFrame, progress) by the
 * cull's own rule. The progress byte becomes the record's blend byte, through
 * the game's table where it has one (moby_game.anim_blend_byte) and as it is
 * otherwise. A zero blend byte takes keyframe A alone; any other takes
 * keyframe B at nextFrame out of anims[nextId], which is the next animation
 * while the moby blends out of one clip into another and its own table
 * otherwise (Spyro 1 r_moby.s:198-214, 229-242; Spyro 2 mobydraw.s:456-492).
 * Keyframe B's header is carried because the Spyro 2/3 stream codec locates
 * a keyframe's stream from its header's data base; the Spyro 1 codec takes
 * the stream address from the keyframe record itself (r_moby.s:569-582). */
typedef struct {
    uint32_t header;     /* anims[id]: vertex counts, shift, faces */
    uint32_t header_b;   /* anims[nextId] when blending, else header */
    uint32_t frame_a;    /* keyframe A at frame, in the record's form */
    uint32_t frame_b;    /* keyframe B at nextFrame, in the record's form; frame_a alone */
    uint32_t blend;      /* the record's blend byte (w0 bits 8-15); 0 = keyframe A alone */
    const double (*held)[3]; /* the model-space vertices a canonical pass decoded for
                              * this pose (pose_hold_find), or NULL */
    int held_n;              /* their count */
} moby_pose_t;

/** @brief A keyframe named the way the cull record names it.
 *
 * The address of its record in the header's frame array (anim_frames +
 * index * anim_frame_stride), or, where the cull loads that record's leading
 * stream word into the draw record (rec_frame_is_word), the word
 * (mobydraw.s:463-467 the derivation and the load, :532-535 the store).
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @param index The keyframe index.
 * @return The keyframe reference. */
static uint32_t moby_keyframe_ref(CPUState* cpu, uint32_t header, uint32_t index) {
    uint32_t at = header + game_layout.moby.anim_frames + index * game_layout.moby.anim_frame_stride;
    return game_layout.moby.rec_frame_is_word ? cpu->read_word(at) : at;
}

/** @brief Derive the keyframe pair one moby draws from its animation state bytes by the cull's rule.
 *
 * Header A is anims[id] of the model's animation table. The progress byte
 * maps through moby_game.anim_blend_byte where the game has one; a nonzero
 * blend takes header B from anims[nextId], and a blend toward an unfilled
 * anims[nextId] holds keyframe A with blend 0, so the stream reader never
 * dereferences a non-header. moby_pose_t carries the rule's asm references.
 * @param cpu The guest CPU state, read only.
 * @param model The model-table entry.
 * @param st The five animation state bytes (id, nextId, frame, nextFrame,
 *        progress), live or from an interp snapshot.
 * @param p Receives the pose.
 * @return 0 when anims[id] is not a header, else 1. */
static int moby_pose_derive(CPUState* cpu, uint32_t model, const uint8_t st[5], moby_pose_t* p) {
    const uint32_t anims = model + game_layout.moby.model_anims_off;
    p->header = cpu->read_word(anims + (uint32_t)st[0] * 4u);
    if (p->header < 0x80000000u) return 0;
    uint32_t blend = moby_game.anim_blend_byte ? moby_game.anim_blend_byte(cpu, st[4]) : st[4];
    p->blend    = blend & 0xFFu;
    p->header_b = p->blend ? cpu->read_word(anims + (uint32_t)st[1] * 4u) : p->header;
    /* A blend toward an unfilled anims[nextId] holds keyframe A: the stream
     * reader never dereferences a non-header. */
    if (p->header_b < 0x80000000u) { p->header_b = p->header; p->blend = 0; }
    p->frame_a = moby_keyframe_ref(cpu, p->header, st[2]);
    p->frame_b = p->blend ? moby_keyframe_ref(cpu, p->header_b, st[3]) : p->frame_a;
    p->held = NULL; p->held_n = 0;
    return 1;
}

/** @brief Open a vertex stream on a keyframe through the game's decoder slot.
 *
 * Calls moby_game.vstream_init, which must be set.
 * @param cpu The guest CPU state.
 * @param s Receives the stream state.
 * @param header The AnimationHeader.
 * @param frame The keyframe.
 * @param s5 The game's scale argument for the decode. */
static void vstream_init(CPUState* cpu, vstream* s, uint32_t header, uint32_t frame, int s5) {
    moby_game.vstream_init(cpu, s, header, frame, s5);
}
/** @brief Decode the next vertex of a stream through the game's decoder slot.
 *
 * Calls moby_game.vstream_next, which must be set.
 * @param cpu The guest CPU state.
 * @param s The stream state, advanced in place.
 * @param first Nonzero for the stream's first vertex. */
static void vstream_next(CPUState* cpu, vstream* s, int first) {
    moby_game.vstream_next(cpu, s, first);
}

/* ---- per-model vertex extent ---------------------------------------------
 * The largest distance from the model origin to any vertex of any keyframe of
 * one AnimationHeader, in the units the decode's rotated vertex offset
 * ((M.v)/4096, added straight to the projection translation) carries.
 *
 * The header's own bounding-radius byte is the cull's sphere, authored for the
 * game's cone test and quantized to 16 units; a model whose mesh reaches
 * further than that sphere still draws every one of its vertices. Where a
 * screen-space bound must hold for the whole mesh - the widescreen reveal's
 * lateral gate, which decides whether a moby centred outside the window still
 * paints inside it - the mesh's own extent is the bound and the byte is not.
 *
 * The keyframe streams are static model data, so the extent is decoded once
 * per header and cached. The key carries a content signature (the header's
 * count word and its geometry pointer block) beside the address, so a level
 * load that puts a different model at the same address re-decodes rather than
 * answering from the old mesh. Returns -1 when the model cannot be measured:
 * the game's header layout does not name its keyframe count, the count or
 * vertex count is empty, or no keyframe is live. */
#define MEXT_SLOTS 256
typedef struct { uint32_t header, sig1, sig2; float ext; } mext_t;
static mext_t s_mext[MEXT_SLOTS];
static long s_mext_hit, s_mext_miss;      /* engagement census */

/** @brief Measure the largest vertex distance from the model origin over the live keyframes of an AnimationHeader.
 *
 * Decodes every vertex of every keyframe (at most 256) through the game's
 * vertex stream, in the decode's rotated-vertex units.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader's PSX address.
 * @return The distance, or -1.0 when the game names no keyframe count, a
 *         count or vertex count is 0, or no keyframe is live. */
static double model_extent_compute(CPUState* cpu, uint32_t header) {
    int nf = moby_game.anim_frame_count(cpu, header);
    if (nf < 1) return -1.0;
    if (nf > 256) nf = 256;                /* guard: a desynced header reads as a huge count */
    int s5    = (cpu->read_byte(header + game_layout.moby.anim_shift_off) & 0xFF) + 1;
    int count = cpu->read_byte(header + game_layout.moby.anim_vcount_hi) & 0xFF;
    if (count == 0) return -1.0;
    double best = 0.0;                     /* squared, rooted once at the end */
    for (int fi = 0; fi < nf; fi++) {
        uint32_t fr = moby_keyframe_ref(cpu, header, (uint32_t)fi);
        if (!moby_pose_live(header, fr)) continue;
        vstream A;
        vstream_init(cpu, &A, header, fr, s5);
        for (int i = 0; i < count; i++) {
            vstream_next(cpu, &A, i == 0);
            double r2 = (double)A.VX * A.VX + (double)A.VY * A.VY + (double)A.VZ * A.VZ;
            if (r2 > best) best = r2;
        }
    }
    return best > 0.0 ? sqrt(best) : -1.0;
}

typedef struct { uint32_t w0, w1; } anim_sig_t;
/** @brief The content signature of an AnimationHeader: two of its guest words.
 *
 * w0 is the header's first word and w1 the word at header +
 * game_layout.moby.anim_ptr_block, the first word of its geometry pointer
 * block. A header slot the game refills with another clip (a level load, or a
 * streamed animation buffer) reads a different signature at the same address.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader's PSX address.
 * @return The signature. */
static anim_sig_t anim_header_sig(CPUState* cpu, uint32_t header) {
    anim_sig_t g = { cpu->read_word(header),
                     cpu->read_word(header + game_layout.moby.anim_ptr_block) };
    return g;
}

/** @brief The cached vertex extent of an AnimationHeader, measured on a miss.
 *
 * The cache is direct-mapped on header >> 4 and a hit needs the address and
 * both signature words (anim_header_sig) to match. A failed measurement is
 * cached as well.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader's PSX address; one below KSEG0 is
 *        refused.
 * @return The extent as model_extent_compute returns it, or -1.0 for a game
 *         without moby_game.anim_frame_count or a header below KSEG0. */
static double model_extent(CPUState* cpu, uint32_t header) {
    if (!moby_game.anim_frame_count || header < 0x80000000u) return -1.0;
    anim_sig_t g = anim_header_sig(cpu, header);
    uint32_t sig1 = g.w0, sig2 = g.w1;
    mext_t* E = &s_mext[(header >> 4) & (MEXT_SLOTS - 1)];
    if (E->header == header && E->sig1 == sig1 && E->sig2 == sig2) { s_mext_hit++; return (double)E->ext; }
    s_mext_miss++;
    double e = model_extent_compute(cpu, header);
    E->header = header; E->sig1 = sig1; E->sig2 = sig2; E->ext = (float)e;
    return e;
}

/** @brief How far, in the units the cull's stored translation carries, a moby's body may reach either side of its projected centre.
 *
 * The .s cone's own bias is the header's bounding-radius byte
 * (4(|x| - 25r/32) < 3(z + 19r/32)): a sphere authored for the game's 4:3 cull
 * and quantized to 16 units, which a model whose mesh reaches further than it
 * still draws whole. Where the admission is the widened window rather than the
 * game's, that quantized sphere is not the bound; the model's real vertex
 * extent is. The reveal takes the larger of the two, so its admission stays a
 * superset of the cone's for a model whose authored sphere exceeds its mesh.
 *
 * model_extent is in the decode's rotated-vertex units; the centre projects
 * from the stored translation, which the header's m_Scale shifts
 * (TR = (stored << 2) >> m_Scale, the shift capped at 30 here), so the extent
 * takes that shift back.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @param rr The header's bounding radius, its radius byte << 4.
 * @return The lateral extent, at least 25 rr / 32. */
static double reveal_lateral_extent(CPUState* cpu, uint32_t header, double rr) {
    double lat = 25.0 * rr / 32.0;
    double ext = model_extent(cpu, header);
    if (ext <= 0.0) return lat;
    int ms = cpu->read_byte(header + game_layout.moby.anim_scale_off) & 0xFF;
    if (ms > 30) ms = 30;
    double ext_stored = ext * (double)(1u << ms) / 4.0;
    return ext_stored > lat ? ext_stored : lat;
}

/* The smallest stored view-Z the margin below will divide by: stored units are
 * world/4, so this floors the nearest-point depth at four world units. */
#define REVEAL_NEAR_RZ 1.0

/** @brief How far, in screen px, a record's body may reach either side of its projected centre under the widened-window admission.
 *
 * The centre projects at its own view-Z, but the body's nearest point sits up
 * to its lateral extent closer to the camera, and that point projects by the
 * nearer depth: a wide flat model seen obliquely spans more screen x than its
 * extent scaled at the centre's depth. reveal_lateral_extent returns the
 * extent in the units the stored translation carries (model_extent's
 * rotated-vertex units with the header's m_Scale shift taken back,
 * TR = (stored << 2) >> m_Scale), which is what rz holds, so rz - ext is the
 * nearest view-Z the body can occupy and ext * H / (rz - ext) its lateral
 * reach there. Dividing by rz instead bounds only a body whose vertices all
 * sit at the centre's depth, and drops a nearer-cornered model whose pixels
 * are inside the frame.
 *
 * A body whose nearest view-Z is at or below REVEAL_NEAR_RZ (its extent
 * sphere reaches the camera plane) has no bounded lateral projection: the
 * margin is infinite and the window test cannot reject it. The
 * behind-camera and camera-plane gates upstream already drop the records with
 * no projection at all.
 *
 * The bound is a superset of the centre-depth one for every record: ext >= 0,
 * so rz - ext <= rz and the margin never shrinks.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @param rr The header's bounding radius, its radius byte << 4.
 * @param rz The centre's stored view-Z.
 * @param H The projection plane distance of the admitting view.
 * @return The margin in screen px, or INFINITY. */
static double reveal_lateral_margin(CPUState* cpu, uint32_t header, double rr,
                                    double rz, double H) {
    double ext = reveal_lateral_extent(cpu, header, rr);
    double near_rz = rz - ext;
    if (near_rz <= REVEAL_NEAR_RZ) return INFINITY;
    return ext * H / near_rz;
}

/* Decode + float-project one record's vertices. Mirrors F798: the no-blend path
 * (block 8001FCC8) when the keyframe blend factor is 0, else the INTPL blend
 * path (block 8001FA44) -- decode BOTH frame A and frame B streams and lerp each
 * vertex in MODEL space by blend/4096 before RTPS (the asm INTPLs IR(=A) toward
 * FC(=B) by IR0(=blend) then RTPSs the result). Writes screen XY + view-Z.
 * Returns vertex count. The caller set-matches these vs the emitted prims. */
typedef struct { float sx, sy, vz; int sat; int unr; int rec;
                 float ex, ey, ez; float tvz;
                 moby_otk_vert_t iv;   /* the draw's integer RTPS output (paint key) */
                 moby_otk_rec_t otk;   /* the record's paint key; first vertex only */
               } proj_xy;  /* vz=view-Z; rec=source record (diag);
                 unr = this vertex fell in the GTE divide-overflow regime (see
                 moby_gte_near_fixup); sx/sy are then the hardware-clamped pair;
                 ex/ey/ez = the model->view rotated vertex offset (M.v)/4096 = viewPos-TR,
                 the input to the specular env-reflection UV map. */

/** @brief Open the previous pose's keyframe streams for the two-pass shape lerp.
 *
 * Both decode with that pose's own ShortShift, equal to the current one within
 * a clip and free to differ across a count-matched clip switch; keyframe B's
 * stream starts on keyframe B's own header. Stream B opens only for a
 * positive blend and a live keyframe B. A pose with held vertices opens no
 * stream: moby_prev_vertex reads them.
 * @param cpu The guest CPU state, read only.
 * @param prev The previous pose, or NULL.
 * @param count The vertex count the decode reads; held vertices short of it
 *        leave no previous pose.
 * @param A Receives keyframe A's stream.
 * @param B Receives keyframe B's stream when the blend is positive.
 * @return The pose's blend numerator (1.0 = 4096; 0 opens keyframe A alone,
 *         and is returned for held vertices), or -1 when there is no live
 *         previous pose. */
static int moby_prev_streams(CPUState* cpu, const moby_pose_t* prev, int count,
                             vstream* A, vstream* B) {
    if (prev && prev->held) return prev->held_n >= count ? 0 : -1;
    if (!prev || !moby_pose_live(prev->header, prev->frame_a)) return -1;
    int s5 = (cpu->read_byte(prev->header + game_layout.moby.anim_shift_off) & 0xFF) + 1;
    int blend = (int)(((prev->blend << 8) >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask);
    vstream_init(cpu, A, prev->header, prev->frame_a, s5);
    if (blend <= 0 || !moby_pose_live(prev->header_b, prev->frame_b)) return 0;
    vstream_init(cpu, B, prev->header_b, prev->frame_b, s5);
    return blend;
}

/* Decodes whose previous pose was rescaled into the current clip's model unit
 * (the two headers' scale bytes differ), counted since start; printed in the
 * OPENPETE_MOBY_ANIMSW_DBG line. */
static long s_prev_rescaled = 0;

/** @brief The factor that carries a previous pose's model-space vertices into the current clip's model unit.
 *
 * A clip's model unit is 2^s for its header's scale byte s (the translation
 * is shifted right by s, moby_decode_project), so a vertex decoded under the
 * previous pose's byte is multiplied by 2^(s_prev - s_curr). Held vertices
 * (pose_hold_find) were decoded under the same header and take the same
 * factor.
 * @param cpu The guest CPU state, read only.
 * @param prev The previous pose; a header that is not an address yields 1.
 * @param blend_p moby_prev_streams' result; negative (no previous pose to
 *        read) yields 1.
 * @param m_scale The current header's scale byte.
 * @return The factor, 1 when the two bytes agree. */
static double moby_prev_unit(CPUState* cpu, const moby_pose_t* prev, int blend_p, int m_scale) {
    if (blend_p < 0 || !prev || prev->header < 0x80000000u) return 1.0;
    const int sp = (int)(cpu->read_byte(prev->header + game_layout.moby.anim_scale_off) & 0xFFu);
    if (sp == m_scale) return 1.0;
    s_prev_rescaled++;
    return ldexp(1.0, sp - m_scale);
}

/** @brief The previous pose's model-space vertex i for the two-pass shape lerp.
 *
 * Held vertices are read as stored; otherwise keyframe A's stream advances,
 * and for a positive blend keyframe B's, and the vertex is A + (B - A) tp with
 * tp = blend / 4096, the arithmetic of the current pose's decode.
 * @param cpu The guest CPU state, read only.
 * @param prev The previous pose.
 * @param A Keyframe A's stream from moby_prev_streams.
 * @param B Keyframe B's stream, read for a positive blend.
 * @param blend The blend numerator moby_prev_streams returned.
 * @param i The vertex index; streams advance one vertex per call from 0.
 * @param o Receives the vertex. */
static void moby_prev_vertex(CPUState* cpu, const moby_pose_t* prev, vstream* A, vstream* B,
                             int blend, int i, double o[3]) {
    if (prev->held) { memcpy(o, prev->held[i], sizeof(double) * 3); return; }
    vstream_next(cpu, A, i == 0);
    o[0] = A->VX; o[1] = A->VY; o[2] = A->VZ;
    if (blend <= 0) return;
    const double tp = (double)blend / 4096.0;
    vstream_next(cpu, B, i == 0);
    o[0] += tp * (B->VX - A->VX);
    o[1] += tp * (B->VY - A->VY);
    o[2] += tp * (B->VZ - A->VZ);
}

/* ---- held poses ---------------------------------------------------------------
 * The canonical pass keeps, per moby, the model-space vertices it decoded
 * (after the keyframe blend, before rotation) for the moby's latest pose and
 * for the distinct pose before it, the two ends of the moby's interp pair. A
 * sub-tick present lerps from the earlier one; the game, or the CD stream
 * refilling a buffer, may have reused that clip's keyframe memory since the
 * pass drew it, so the lerp reads the held vertices instead of re-decoding a
 * clip the moby has left. A pass that decodes the latest pose again (a
 * second scene build in one tick, a moby holding still) refreshes it in
 * place. The table is open-addressed on the moby address and holds at most
 * POSE_HOLD_SLOTS - 1 mobys; each pose owns 256 vertices (the vertex count is
 * a byte). OPENPETE_MOBY_POSE_HOLD=0 re-decodes from guest memory instead. */
#define POSE_HOLD_SLOTS 2048
typedef struct { moby_pose_t pose; int n; double (*v)[3]; } pose_hold_pose;
typedef struct { uint32_t moby; pose_hold_pose p[2]; } pose_hold_slot;  /* p[0] latest */
static pose_hold_slot s_ph[POSE_HOLD_SLOTS];
static int s_ph_used;
static pose_hold_pose* s_ph_open;           /* the pose the running decode fills, or NULL */
static _Thread_local double (*t_ph_dst)[3]; /* the running decode's capture target, or NULL */
/** @brief Whether held poses are on (OPENPETE_MOBY_POSE_HOLD, default on), latched on first use.
 * @return 1 when on, else 0. */
static int pose_hold_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_POSE_HOLD"); s = !(e && *e == '0'); }
    return s;
}
/** @brief Whether two poses name the same keyframes at the same blend.
 *
 * Keyframe B counts only for a nonzero blend; its header only where the
 * record names keyframes by their leading stream word
 * (rec_frame_is_word), whose stream the decoder locates from that header.
 * A pointer-form keyframe names its stream itself, and the cull record need
 * not carry keyframe B's header.
 * @param a The first pose.
 * @param b The second pose.
 * @return 1 when they match, else 0. */
static int pose_hold_same(const moby_pose_t* a, const moby_pose_t* b) {
    if (a->header != b->header || a->frame_a != b->frame_a || a->blend != b->blend) return 0;
    if (a->blend == 0u) return 1;
    return a->frame_b == b->frame_b &&
           (!game_layout.moby.rec_frame_is_word || a->header_b == b->header_b);
}
/** @brief The table slot of a moby, claimed when absent and `add` is set.
 * @param moby The moby's PSX address, nonzero.
 * @param add Nonzero claims a free slot for a moby the table does not hold.
 * @return The slot, or NULL when the moby is absent and not added (or the
 *         table is full). */
static pose_hold_slot* pose_hold_slot_of(uint32_t moby, int add) {
    uint32_t h = (moby >> 2) * 2654435761u;
    for (int k = 0; k < POSE_HOLD_SLOTS; k++) {
        pose_hold_slot* t = &s_ph[(h + (uint32_t)k) & (POSE_HOLD_SLOTS - 1)];
        if (t->moby == moby) return t;
        if (t->moby != 0u) continue;
        if (!add || s_ph_used >= POSE_HOLD_SLOTS - 1) return NULL;
        t->moby = moby; s_ph_used++;
        return t;
    }
    return NULL;
}
/** @brief Point the next decode's model-space vertex capture at a moby's latest pose.
 *
 * A pose other than the held latest one moves that one to the earlier slot
 * first; the same pose is refreshed in place. Holds nothing when held poses
 * are off, for an address below KSEG0, or when the table or an allocation
 * fails.
 * @param moby The moby the decode draws.
 * @param header The pose's AnimationHeader.
 * @param header_b Keyframe B's header.
 * @param frame_a Keyframe A's reference.
 * @param frame_b Keyframe B's reference.
 * @param w0 The record flags word; bits 8-15 are the blend byte. */
static void pose_hold_begin(uint32_t moby, uint32_t header, uint32_t header_b, uint32_t frame_a,
                            uint32_t frame_b, uint32_t w0) {
    s_ph_open = NULL; t_ph_dst = NULL;
    pose_hold_slot* t = (pose_hold_on() && moby >= 0x80000000u) ? pose_hold_slot_of(moby, 1) : NULL;
    if (!t) return;
    const uint32_t bl = (w0 >> 8) & 0xFFu;
    const moby_pose_t p = { header, bl ? header_b : header, frame_a, bl ? frame_b : frame_a, bl,
                            NULL, 0 };
    if (t->p[0].n > 0 && !pose_hold_same(&t->p[0].pose, &p)) {
        const pose_hold_pose e = t->p[1]; t->p[1] = t->p[0]; t->p[0] = e;
    }
    pose_hold_pose* d = &t->p[0];
    if (!d->v && !(d->v = (double (*)[3])malloc(sizeof *d->v * 256))) return;
    d->pose = p; d->n = 0;
    s_ph_open = d; t_ph_dst = d->v;
}
/** @brief Commit the pose pose_hold_begin opened with the decode's vertex count.
 * @param got The vertices the decode wrote; 0 or less leaves the pose empty. */
static void pose_hold_end(int got) {
    if (s_ph_open) s_ph_open->n = got > 0 ? got : 0;
    s_ph_open = NULL; t_ph_dst = NULL;
}
/** @brief Attach the vertices the canonical pass held for a moby's pose to that pose.
 *
 * The moby's latest held pose is tried, then the distinct one before it.
 * Nothing is attached when held poses are off or neither matches
 * (pose_hold_same).
 * @param moby The moby.
 * @param p The pose; its held fields are written. */
static void pose_hold_find(uint32_t moby, moby_pose_t* p) {
    p->held = NULL; p->held_n = 0;
    const pose_hold_slot* t = pose_hold_on() ? pose_hold_slot_of(moby, 0) : NULL;
    for (int k = 0; t && k < 2; k++) {
        if (t->p[k].n <= 0 || !pose_hold_same(&t->p[k].pose, p)) continue;
        p->held = (const double (*)[3])t->p[k].v; p->held_n = t->p[k].n;
        return;
    }
}

/* ---- paint-key cost instrument (OPENPETE_MOBY_RANK_TIME=1) ----------------
 * TSC cycles spent in the paint-key stages, summed over the walk's workers and
 * printed per feed pass: 0 integer RTPS per vertex, 1 per-face NCLIP + key,
 * 2 intersection census, 3 rank build. Cycles convert to microseconds by the
 * TSC rate measured against openpete_phase_now_us over the run. */
enum { RT_RTPS = 0, RT_KEY, RT_ISECT, RT_RANK, RT_N };
static uint64_t s_rt_cyc[RT_N];
static long     s_rt_cnt[RT_N];
/** @brief Whether OPENPETE_MOBY_RANK_TIME=1 enables the paint-key cost instrument.
 *
 * Read from the environment on the first call and latched.
 * @return 1 when the variable begins with '1', else 0. */
static int rank_time_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_TIME"); s = e && *e == '1'; }
    return s;
}
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
/** @brief The TSC, or 0 while the paint-key cost instrument is off.
 *
 * @return The TSC value; 0 doubles as the "not sampling" start value rt_add
 *         ignores. */
static inline uint64_t rt_now(void) { return rank_time_on() ? __rdtsc() : 0; }
#else
/** @brief The TSC stand-in on a target without one: always 0.
 *
 * @return 0, so every rt_add on this target is a no-op. */
static inline uint64_t rt_now(void) { return 0; }
#endif
/* A per-vertex or per-face sample above 300000 cycles (about 0.1 ms) is a
 * descheduling, not work: it is dropped and counted. */
static long s_rt_drop;
/** @brief Add the TSC cycles since t0 to a paint-key stage's accumulator.
 *
 * Does nothing when t0 is 0. A sample of n > 0 items that spans more than
 * 300000 cycles is dropped and counted in s_rt_drop. Otherwise the cycles and
 * n are added atomically, so the walk's workers call it concurrently.
 * @param slot The stage, RT_RTPS to RT_RANK.
 * @param t0 The rt_now value at the start of the sample.
 * @param n The items sampled; 0 for a stage timed as a whole, which is never
 *          dropped. */
static inline void rt_add(int slot, uint64_t t0, long n) {
    if (!t0) return;
    uint64_t d = rt_now() - t0;
    if (n > 0 && d > 300000u) { __atomic_fetch_add(&s_rt_drop, 1, __ATOMIC_RELAXED); return; }
    __atomic_fetch_add(&s_rt_cyc[slot], d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_rt_cnt[slot], n, __ATOMIC_RELAXED);
}
#include <time.h>
/** @brief The calling thread's CPU time in nanoseconds, 0 with the cost instrument off.
 *
 * Excludes the time the thread spent descheduled, which the single-threaded
 * stages use to separate work from preemption.
 * @return The CPU time, or 0. */
static uint64_t rt_cpu_ns(void) {
    if (!rank_time_on()) return 0;
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
static double s_rt_cpu_us[RT_N];
/** @brief Print one feed pass's paint-key stage costs to stderr, then clear them.
 *
 * Does nothing while the instrument is off. Cycles convert to microseconds by
 * the TSC rate measured between the first call and this one against
 * openpete_phase_now_us; until 100 ms have elapsed the rate is 3 GHz. The
 * line carries the per-stage times and counts, the CPU times of the
 * intersection and rank stages, and the dropped-sample count; the
 * accumulators are zeroed afterwards.
 * @param mat The pass's material tag.
 * @param ntri The pass's triangle count. */
static void rt_report(const char* mat, int ntri) {
    if (!rank_time_on()) return;
    static uint64_t c0; static uint64_t u0;
    uint64_t c = rt_now(), u = openpete_phase_now_us();
    if (!c0) { c0 = c; u0 = u; }
    double us_per_cyc = (c > c0 && u > u0 + 100000u) ? (double)(u - u0) / (double)(c - c0)
                                                     : 1.0 / 3000.0;
    fprintf(stderr, "[rank-time] vbl=%u mat=%s tris=%d rtps_us=%.1f verts=%ld key_us=%.1f "
                    "faces=%ld isect_us=%.1f rank_us=%.1f isect_cpu_us=%.1f rank_cpu_us=%.1f drop=%ld\n",
            openpete_frame, mat, ntri, s_rt_cyc[RT_RTPS] * us_per_cyc, s_rt_cnt[RT_RTPS],
            s_rt_cyc[RT_KEY] * us_per_cyc, s_rt_cnt[RT_KEY], s_rt_cyc[RT_ISECT] * us_per_cyc,
            s_rt_cyc[RT_RANK] * us_per_cyc, s_rt_cpu_us[RT_ISECT], s_rt_cpu_us[RT_RANK], s_rt_drop);
    memset(s_rt_cyc, 0, sizeof s_rt_cyc); memset(s_rt_cnt, 0, sizeof s_rt_cnt);
    memset(s_rt_cpu_us, 0, sizeof s_rt_cpu_us); s_rt_drop = 0;
}

/* The feed is an intermediate present's (moby_reextract). */
static int s_in_reextract;
static int rank_carry_on(void);
/** @brief Whether this feed computes the paint key.
 *
 * Only while the rank select is on, and not on an intermediate present
 * (moby_reextract) that carries the tick's verdict.
 * @return 1 when the key is wanted, else 0. */
static int moby_otkey_wanted(void) {
    return openpete_mobyrank_on() && !(s_in_reextract && rank_carry_on());
}

/** @brief An animated record's paint key through the game's record decode.
 *
 * Computed on every feed, rank select on or off: moby_coincident_pull reads
 * it to order exactly coincident faces. The row is MOBY_OTK_NONE when the
 * game mirrors no key (no otkey_record slot), and the game's specular row
 * replaces the decoded row on the specular pass (material tag "SPEC", its
 * re-extract "SP").
 * @param cpu The guest CPU state, read only.
 * @param header Keyframe A's AnimationHeader.
 * @param headerB Keyframe B's AnimationHeader.
 * @param w0 The record's flags word.
 * @param TR_raw The record's stored translation.
 * @param tr Receives the integer translation the draw loads, zero with no row.
 * @param r Receives the record's paint key. */
static void moby_otk_anim_record(CPUState* cpu, uint32_t header, uint32_t headerB, uint32_t w0,
                                 const int TR_raw[3], int tr[3], moby_otk_rec_t* r) {
    int a = 0;
    moby_otk_row_t row = MOBY_OTK_NONE;
    tr[0] = tr[1] = tr[2] = 0;
    if (moby_game.otkey_record)
        row = moby_game.otkey_record(cpu, header, headerB, w0, TR_raw, tr, &a);
    if (row != MOBY_OTK_NONE && moby_game.specular_otkey_row != MOBY_OTK_NONE &&
        s_cur_mat[0] == 'S' && s_cur_mat[1] == 'P')
        row = moby_game.specular_otkey_row;
    moby_otk_record(row, a, tr[2], r);
}

/** @brief One vertex's integer RTPS for the record's paint key; nothing when the record carries none.
 *
 * @param r The record's paint key.
 * @param tr The integer translation the draw loads.
 * @param M The model-to-view rotation, 4096 = 1.0.
 * @param vx The model-space vertex X.
 * @param vy The model-space vertex Y.
 * @param vz The model-space vertex Z.
 * @param ofx The screen offset X.
 * @param ofy The screen offset Y.
 * @param H The projection distance.
 * @param o Receives the integer RTPS output in its iv field. */
static void moby_otk_vertex(const moby_otk_rec_t* r, const int tr[3], const int M[3][3],
                            double vx, double vy, double vz, double ofx, double ofy, int H,
                            proj_xy* o) {
    if (r->row == MOBY_OTK_NONE) return;
    uint64_t rt0 = rt_now();
    moby_otk_rtps(tr, M, vx, vy, vz, ofx, ofy, H, &o->iv);
    rt_add(RT_RTPS, rt0, 1);
}

/** @brief Decode one moby record's vertex stream with the game's codec and project every vertex in float.
 *
 * The cull-record path (moby_project_record) and the host-built record path
 * (widescreen reveal, sub-tick re-extract) pass the same fields, so a
 * host-built moby decodes and projects identically to a culled one.
 *
 * The projection translation is (TR << 2) >> m_Scale [F798
 * @8001F858/8001F8C8] where the cull stores the unscaled translation, and the
 * stored value as it is where the cull stored the final one (rec_tr_final).
 * A nonzero w0 low byte (the GPF scale) takes the .s GPF-TR path
 * [.L8001F870..F8DC; spec twin @80021024]: TRi = (IR0 * int16((TRi << 2) >>
 * m_Scale)) >> 5, which scales the moby on screen by moving it in depth, and
 * the record is skipped when MAC3 >> 5 exceeds 63000 [.L8001F888, spec twin
 * @8002103C], since the game draws nothing past that depth. The depth written
 * for a GPF record stays at the unscaled distance, as the .s OT key derives
 * from the raw TR.z; only sx and sy take the scaled projection. The vertex
 * count is the tier's (Spyro 1 [hdr + 8 + (ZSF4 >> 31)] @8001F9D4..FA04: the
 * LP count below ZSF4 0 unless --no-lod), and a count of 0 skips the record.
 * OPENPETE_MOBY_VERIFY_NEGCTL=1 reads the other tier's count, the negative
 * control of the verify A/B.
 *
 * With prev naming a live pose the decoder reconstructs both the previous
 * drawn pose (the vertices a canonical pass held for it, else its keyframes
 * at its own blend byte) and the current one, lerps
 * them in model space by blend_alpha, and projects once, so the shape moves
 * between two poses the game drew rather than through a synthesized keyframe
 * blend. Across a clip switch moby_prev_keyframe lerps only when both clips
 * share a vertex count, so vertex i is the same vertex in both; the previous
 * streams decode with prev->header's own ShortShift (moby_prev_streams). The
 * header's scale byte sets the model unit: the translation the vertices add
 * to is the quarter-world centre shifted left 2 and right by that byte, so
 * one model unit is 2^byte times as large as at byte 0. A previous pose
 * decoded under another clip's byte is multiplied by
 * 2^(previous byte - current byte) before the lerp (moby_prev_unit), so both
 * ends are in the current clip's unit.
 *
 * Per vertex: sx, sy from project(), replaced by moby_gte_near_fixup inside
 * the GTE's saturation regime; vz the full-world view Z (the projected Z
 * times 2^m_Scale) less the one-slot tie pull OPENPETE_MOBY_TIE_PULL_VZ when
 * the record centre is in front of the camera (moby_tie_pull_on() off drops
 * the pull), since the .s resolves a same-slot moby-terrain tie to the moby;
 * tvz the perspective W; ex, ey, ez the rotated vertex offset (M.v)/4096, the
 * specular env-UV input; iv the integer RTPS of the record's paint key. When
 * the pull puts the nearest in-front vertex below 1, the whole record lifts
 * uniformly onto the near floor. out[0].otk carries the paint key.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader (scale byte, ShortShift, tier vertex counts).
 * @param headerB Keyframe B's header; header when below KSEG0.
 * @param frameA Keyframe A's reference (moby_keyframe_ref form).
 * @param frameB Keyframe B's reference.
 * @param w0 The record flags word: blend in bits 8-15, m_DepthOffset in 16-23, the GPF scale in the low byte.
 * @param prev The previous pose for the two-pose shape lerp, or NULL for a single-pose decode.
 * @param blend_alpha The lerp phase from prev (0) to the current pose (1).
 * @param gpf_frac The sub-tick-lerped GPF scale on the TR_frac path; negative takes w0's byte.
 * @param TR_raw The stored translation, before the (<< 2) >> m_Scale rescale.
 * @param TR_frac The full-precision centre on TR_raw's scale (moby_centre_at), or NULL for the integer path.
 * @param M The model-to-view rotation, 4096 = 1.0.
 * @param zsf4 The record's tier; below 0 selects the LP vertex count.
 * @param v The view.
 * @param out Receives one projection per vertex.
 * @param cap Capacity of out; the vertex count is clamped to it.
 * @return The vertex count written, 0 when header or keyframe A is not live, the GPF far skip drops the record, or the tier's count is 0. */
static int moby_decode_project(CPUState* cpu, uint32_t header, uint32_t headerB,
                               uint32_t frameA,
                               uint32_t frameB, uint32_t w0,
                               const moby_pose_t* prev, double blend_alpha, double gpf_frac,
                               const int TR_raw[3],
                               const double TR_frac[3],
                               const int M[3][3], int zsf4, const ngte_view* v,
                               proj_xy* out, int cap) {
    if (header < 0x80000000u || !moby_pose_live(header, frameA)) return 0;
    if (headerB < 0x80000000u) headerB = header;
    /* C2_TR (projection translation) = (TR_stored << 2) >> m_Scale  [F798
     * @8001F858/8001F8C8]. m_Scale is the per-model shrink factor (AnimationHeader
     * +0x05): 2 = identity (the common case), 1 = ×2 size, 3 = /2, etc. Without
     * this, models whose m_Scale != 2 project at the wrong distance.
     * (a3 = w0&0xFF != 0 records take the GPF-TR rescale below instead.)
     *
     * TR_frac (host-rebuilt path) carries the moby-centre translation at FLOAT
     * precision - the camera/moby delta computed as ÷4.0 instead of >>2, so the
     * moby tracks the smooth sub-tick camera instead of stepping in 4-world-unit
     * chunks (the same fix terrain got). Same scale as TR_raw, just fractional;
     * the .s-record path passes NULL and keeps the byte-exact integer TR. */
    /* The draw-side translation rescale, and the far skip that rides it, exist
     * only on a game whose cull stores the unscaled translation; where the cull
     * stored the final one the record's translation goes to the GTE as it is.
     * Either way the header's scale byte is the shift the translation took
     * from world/4 (here or in the cull), so it is also how far the projected
     * view-Z shifts back to full world below. */
    const int tr_final = game_layout.moby.rec_tr_final;
    int m_scale = cpu->read_byte(header + game_layout.moby.anim_scale_off) & 0xFF;
    /* .s far-skip [.L8001F888 / spec twin @8002103C]: records with a non-zero
     * w0 low byte (the GPF-TR scale, m_ScaleOverride-derived) run IR0=(w0&0xFF),
     * IR3=(TR.z<<2)>>m_Scale through GPF sf=0 and SKIP the whole record when
     * MAC3>>5 exceeds 63000 (-31500 twice, .L800208A4) - the game draws NOTHING
     * for them beyond that depth. Mirror it or the native draws mobys the game
     * popped out. IR3 is an int16 GTE data reg, so the mtc2 truncates - keep
     * that. w0&0xFF==0 records (the common case) never take this test. Host-
     * built w0 (reveal/sub-tick rebuild) has no low byte; the sub-tick pass
     * re-applies this check from the live record's w0 (reextract_record_pass). */
    int gpf = tr_final ? 0 : (int)(w0 & 0xFFu);
    if (gpf) {
        int ir3 = (int)(int16_t)((TR_raw[2] << 2) >> m_scale);
        int mac3 = gpf * ir3;
        if ((mac3 >> 5) > 63000) return 0;
    }
    double TR[3];
    if (TR_frac) {
        double sc = tr_final ? 1.0
                  : 4.0 / (double)(1 << (m_scale < 0 ? 0 : (m_scale > 30 ? 30 : m_scale)));
        TR[0] = TR_frac[0] * sc;   /* == (TR_frac << 2) >> m_scale, fractional */
        TR[1] = TR_frac[1] * sc;
        TR[2] = TR_frac[2] * sc;
        if (gpf) {                 /* fractional flavour of the GPF rescale below;
                                    * gpf_frac >= 0 = the sub-tick-lerped byte
                                    * (prev->curr m_ScaleOverride, reextract_record_pass)
                                    * so the on-screen scale animates at present rate
                                    * instead of stepping per canonical tick */
            double g = (gpf_frac >= 0.0) ? gpf_frac : (double)gpf;
            TR[0] *= g / 32.0;
            TR[1] *= g / 32.0;
            TR[2] *= g / 32.0;
        }
    } else if (gpf) {
        /* .s GPF-TR path [.L8001F870..F8DC; spec twin @80021024]: when the record's
         * low byte is set, the PROJECTION translation is GPF-rescaled -
         *   TRi = (IR0(=w0&0xFF) × int16((TRi<<2)>>m_Scale)) >> 5   (GPF sf=0, then sra 5)
         * scl/32 > 1 pushes the moby AWAY along the view ray, < 1 pulls it in; the
         * class code animates the byte to scale the moby ON SCREEN by moving it in
         * depth (Gnasty-blast dome: the byte ticks down as the blast plays out).
         * Without this the native projects the dome at its true distance, at
         * constant full size. The mtc2 int16 truncation is kept. */
        TR[0] = (double)((gpf * (int)(int16_t)((TR_raw[0] << 2) >> m_scale)) >> 5);
        TR[1] = (double)((gpf * (int)(int16_t)((TR_raw[1] << 2) >> m_scale)) >> 5);
        TR[2] = (double)((gpf * (int)(int16_t)((TR_raw[2] << 2) >> m_scale)) >> 5);
    } else if (tr_final) {
        TR[0] = (double)TR_raw[0];
        TR[1] = (double)TR_raw[1];
        TR[2] = (double)TR_raw[2];
    } else {
        TR[0] = (double)((TR_raw[0] << 2) >> m_scale);
        TR[1] = (double)((TR_raw[1] << 2) >> m_scale);
        TR[2] = (double)((TR_raw[2] << 2) >> m_scale);
    }
    /* GPF records: the Z-BUFFER depth stays at the TRUE distance. The GPF rescale
     * moves the moby in depth purely to scale its screen projection; the .s OT key
     * still derives from the RAW TR.z (ctrl[14] = (TR.z_raw>>5) − w0 byte2 bias,
     * block_8001F80C..F844), which is how the shrunk blast dome paints OVER the
     * floor on PSX. Emitting the scaled depth here let the floor Z-occlude the
     * dome (it vanished at blast onset); depth at the true distance mirrors the
     * OT-sort intent while sx/sy keep the .s-exact scaled projection. */
    double vz_bias = 0.0;
    if (gpf) {
        double tr2_true = TR_frac ? TR_frac[2] * (4.0 / (double)(1 << (m_scale < 0 ? 0 : (m_scale > 30 ? 30 : m_scale))))
                                  : (double)((TR_raw[2] << 2) >> m_scale);
        vz_bias = tr2_true - TR[2];
    }
    /* .s OT bias, REGULAR/SPECULAR walks: the render loop keys the OT as
     * (TR.z_raw>>5) − signext(w0 byte2) [@8001F810..F828; spec twin @80020FB0];
     * the cull packs m_DepthOffset (the top byte of the moby angle word) into
     * record w0 bits 16-23 [@8001F464..F46C]. That bias is an OT ORDERING KEY
     * (slots, 128 world each), not geometry - per-pixel Z over TRUE depth
     * mirrors the per-face OT sort, so the authored slot pull never enters the
     * stored depth (a 512+-world pull here punched mobys through terrain that
     * genuinely occludes them). Its authored job - "win the surface I sit
     * on" - survives as the sub-slot tie pull below, which can only flip
     * pairs the OT calls a tie. */
    /* OPENPETE_DEPTHDBG="f0,f1": per-record OT-key inputs (header, w0/dofs,
     * pull, centre) for frames [f0,f1] - the OT-sort parity forensics
     * instrument (pairs with the Spyro-feed line and the moby TRIDBG box). */
    { static int s_dd = -1; static int df0, df1;
      if (s_dd < 0) { const char* e = getenv("OPENPETE_DEPTHDBG");
          s_dd = 0; if (e && sscanf(e, "%d,%d", &df0, &df1) == 2) s_dd = 1; }
      if (s_dd == 1 && (int)openpete_frame >= df0 && (int)openpete_frame <= df1) {
          double cf = TR_frac ? TR_frac[2] * 4.0 : (double)TR_raw[2] * 4.0;
          fprintf(stderr, "[mb-pull vbl=%u] hdr=%08X w0=%08X dofs=%d centre=%.0f\n",
                  openpete_frame, header, w0, (int)(((int32_t)(w0 << 8)) >> 24), cf);
      } }
    /* Tie-pull gate: applied only when the record centre is in front of the
     * camera, so behind-camera records keep raw signed depths and emit's
     * vz<=0 near cull stays authoritative. */
    double ot_centre = 0.0;        /* 0 = disabled */
    if (moby_tie_pull_on()) {
        double centre_full = TR_frac
            ? TR_frac[2] * 4.0                       /* full-world = stored x4 */
            : (double)TR_raw[2] * 4.0;
        if (centre_full > 0.0)
            ot_centre = centre_full;
    }
    double ofx = v->ofx, ofy = v->ofy;

    int s5    = (cpu->read_byte(header + game_layout.moby.anim_shift_off) & 0xFF) + 1;
    /* .s tier-adjusted vertex COUNT [@8001F9D4..FA04 / spec twin @80021178]:
     * count = [hdr + 8 + (ZSF4>>31)] - hdr+9 is the LP vertex count - and
     * count == 0 SKIPS the record entirely (bnez -> func_80020860 restore +
     * advance). Models with NO LP mesh store 0 there, so beyond the LOD band
     * the game draws NOTHING for them; reading the HP count unconditionally
     * would keep drawing them. --no-lod forces the HP
     * count with the same guard as the HP face select (moby_tier_faceblock). */
    /* OPENPETE_MOBY_VERIFY_NEGCTL=1 reads the OTHER tier's vertex count, so the
     * stream is decoded to the wrong length and every vertex past the first
     * divergence lands somewhere else: the negative control that proves a
     * verify PASS is measuring the decode rather than a match that any count
     * would produce. */
    static int s_negctl = -1;
    if (s_negctl < 0) { const char* e = getenv("OPENPETE_MOBY_VERIFY_NEGCTL");
                        s_negctl = (e && *e && *e != '0') ? 1 : 0; }
    int lo_tier = (zsf4 < 0 && !openpete_no_lod);
    if (s_negctl) lo_tier = !lo_tier;
    int count = cpu->read_byte(lo_tier ? header + game_layout.moby.anim_vcount_lo
                                       : header + game_layout.moby.anim_vcount_hi) & 0xFF;
    if (count > cap) count = cap;
    if (count == 0) return 0;

    /* Keyframe blend: IR0 = (w0 & 0xFF00) >> 2 (1.0=4096); frame B at frameB.
     * 0 (or B absent) => the no-blend path (frame A only). */
    int blend = (int)((w0 >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask);
    int do_blend = (blend > 0 && moby_pose_live(headerB, frameB));
    double t = (double)blend / 4096.0;

    moby_otk_rec_t otr;
    int itr[3];
    moby_otk_anim_record(cpu, header, headerB, w0, TR_raw, itr, &otr);
    double rec_minfvz = 1e30;   /* nearest in-front vertex after the pulls (near-floor lift) */
    vstream A, B, Ap, Bp;
    vstream_init(cpu, &A, header, frameA, s5);
    if (do_blend) vstream_init(cpu, &B, headerB, frameB, s5);
    /* Optional prev pose (two-pass sub-tick shape lerp). */
    int blend_p = moby_prev_streams(cpu, prev, count, &Ap, &Bp);
    const double prev_k = moby_prev_unit(cpu, prev, blend_p, m_scale);
    for (int i = 0; i < count; i++) {
        vstream_next(cpu, &A, i == 0);
        double vx = A.VX, vy = A.VY, vz = A.VZ;
        if (do_blend) {
            vstream_next(cpu, &B, i == 0);
            vx += t * (B.VX - A.VX);
            vy += t * (B.VY - A.VY);
            vz += t * (B.VZ - A.VZ);
        }
        /* A canonical decode keeps the pose it drew (pose_hold_begin). */
        if (t_ph_dst) { t_ph_dst[i][0] = vx; t_ph_dst[i][1] = vy; t_ph_dst[i][2] = vz; }
        if (blend_p >= 0) {
            /* curr pose is in (vx,vy,vz); lerp prev->curr by blend_alpha (the
             * moby's per-object sub-tick alpha). */
            double p[3];
            moby_prev_vertex(cpu, prev, &Ap, &Bp, blend_p, i, p);
            p[0] *= prev_k; p[1] *= prev_k; p[2] *= prev_k;
            vx = p[0] + (vx - p[0]) * blend_alpha;
            vy = p[1] + (vy - p[1]) * blend_alpha;
            vz = p[2] + (vz - p[2]) * blend_alpha;
        }
        double sx, sy; int sat;
        double vzz = project(M, TR, vx, vy, vz, ofx, ofy, v, &sx, &sy, &sat);
        int unr = moby_gte_near_fixup(M, TR, vx, vy, vz, vzz, ofx, ofy, v, &sx, &sy);
        /* The Z-buffer needs FULL-WORLD view-Z so mobys sort vs terrain / Spyro /
         * each other. project's mac3 is in the m_scale-adjusted world/4 scale
         * (C2_TR = (stored<<2)>>m_scale, stored is world/4), so multiply by
         * 2^m_scale to reach full-world (at m_scale 2, mac3 x4 is the camera Z).
         * Sign-preserving, so emit_wtri's vz<=0 near cull still holds; sx/sy and
         * the saturation flag keep the unscaled GTE-faithful mac3 (vz is depth-only). */
        int ds = m_scale; if (ds < 0) ds = 0; else if (ds > 8) ds = 8;
        out[i].sx = (float)sx; out[i].sy = (float)sy;
        double fvz = (vzz + vz_bias) * (double)(1 << ds);
        /* Fed depth is TRUE per-vertex view-Z. The game's OT is per-FACE -
         * each face inserts at its own average-Z slot minus the authored
         * bias, so one moby's faces bracket other producers' prims in the
         * real walk (the balloonist basket's far wall paints behind the
         * rider Spyro, its near wall and wires over him). Per-pixel Z over
         * true depth reproduces that ordering; collapsing a record toward
         * its centre depth cannot, and z-fights everything sharing the
         * squashed band. Only the uniform biases below shift depth. */
        /* One-slot tie pull: the .s resolves same-slot moby-vs-terrain ties to
         * the MOBY (moby prims insert before the environment, and the near
         * band is deferred behind the OT entirely), so a moby's surface wins
         * ground it touches. 127 world units < one OT slot: it can flip only
         * pairs og calls a tie. */
        if (ot_centre > 0.0) fvz -= OPENPETE_MOBY_TIE_PULL_VZ;
        if (vzz > 0.0 && fvz < rec_minfvz) rec_minfvz = fvz;
        out[i].vz = (float)fvz; out[i].sat = sat; out[i].unr = unr;
        moby_otk_vertex(&otr, itr, M, vx, vy, vz, v->ofx, v->ofy, (int)v->H_canon, &out[i]);
        /* Perspective W = the view-Z the SCREEN PROJECTION divided by, so it
         * excludes both the OT pulls below and the GPF depth restore: sx/sy came
         * from the rescaled TR, and only that vzz makes their UV interpolation
         * consistent. (Any positive multiple works - interpolation sees ratios
         * only - so the 1<<ds normalizer just keeps it in depth units.) */
        out[i].tvz = (float)(vzz * (double)(1 << ds));
        /* rotated vertex offset (M.v)/4096 = viewPos-TR -- the specular env-UV input. */
        out[i].ex = (float)((M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0);
        out[i].ey = (float)((M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0);
        out[i].ez = (float)((M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0);
    }
    /* Near-floor: if the pulls pushed this record past the near plane, lift
     * the WHOLE record uniformly so its nearest vertex sits on the floor -
     * a per-vertex clamp here flattened close-up mobys to one depth and
     * killed their self-occlusion. Uniform lift keeps depth monotonic. */
    if (rec_minfvz < 1.0 && rec_minfvz > -1e29) {
        float lift = (float)(1.0 - rec_minfvz);
        for (int i = 0; i < count; i++) out[i].vz += lift;
    }
    out[0].otk = otr;
    return count;
}

/** @brief The record's ZSF4 LOD and colour-fade tier verdict, after the --no-lod adjustment.
 *
 * The verdict is the record's tier (moby_record_t.tier, the high half of the
 * matrix word the renderer loads into GTE control 30); see
 * moby_tier_faceblock. Under --no-lod the distance term is neutralized
 * host-side on the read, which is idempotent over a verdict a game-side hook
 * already adjusted: the verdict becomes the moby's zero-distance tier
 * (m_Renderer byte << 8) when that tier is near-band ((md << 8) >= 1024). A
 * sub-near tier is an effect-authored fade (ring puffs shrink m_Renderer) and
 * keeps the canonical value.
 * @param cpu The guest CPU state, read only.
 * @param R The extracted record.
 * @return The tier verdict. */
static int record_zsf4(CPUState* cpu, const moby_record_t* R) {
    int zsf4 = R->tier;
    if (openpete_no_lod && moby_game.renderer_off && R->moby >= 0x80000000u) {
        int md = (int)(cpu->read_byte(R->moby + moby_game.renderer_off) & moby_game.renderer_mask);
        if ((md << 8) >= 1024) zsf4 = md << 8;
    }
    return zsf4;
}

/* Forward decl: the fractional view-space moby centre, the single site that
 * derives it (defined with the host record builder below). Every pass that
 * projects a moby reaches the centre through this one call, so the value a
 * record projects through is a function of the present's camera and the
 * moby's position alone - the canonical present and the sub-tick present at
 * alpha 1 are the same point of the same tick and read the same number by
 * construction, not by two derivations agreeing. */
static void build_moby_record_tr(CPUState* cpu, uint32_t moby, double tr[3]);
static int moby_record_centre_final(CPUState* cpu, uint32_t moby, uint32_t header, double tr[3]);
static void moby_centre_census(const char* pass);

/** @brief Project one live cull record, whose fields the game's record reader already extracted.
 *
 * The record's stored TR is the cull's MVMVA of the camera-moby delta taken
 * on the quarter-world grid ((cam - moby) >> 2, MAC >> 12), so it is the
 * centre truncated twice. The native path projects in float (no PS1
 * quantization), so the centre it projects is re-derived here at full
 * precision from the same camera and the same moby position. TR_raw still
 * goes to the decode: the GPF far-skip mirrors a .s test on the stored
 * int16, and the builder A/B compares against the stored value.
 *
 * On a game whose cull stores the final translation (rec_tr_final), that
 * translation also carries the cull's unit scaling - the factor of 4 off the
 * quarter-world grid, the header's shift and the per-moby size rescale - and
 * the decode passes a fractional centre straight to the GTE, so the centre is
 * scaled the same way first (moby_record_centre_final). When neither centre
 * derivation applies, the decode projects the stored TR.
 * @param cpu The guest CPU state, read only.
 * @param R The extracted record.
 * @param zsf4 The record's tier verdict after the --no-lod adjustment
 *        (record_zsf4), which selects the tier's vertex count.
 * @param out Receives the projected vertices.
 * @param cap The capacity of out.
 * @return The number of vertices written. */
static int moby_project_record(CPUState* cpu, const moby_record_t* R, int zsf4,
                               proj_xy* out, int cap) {
    const ngte_view* v = moby_record_view(R->moby);
    double tr[3];
    const double* TR_frac = NULL;
    if (!game_layout.moby.rec_tr_final && R->moby >= 0x80000000u) {
        build_moby_record_tr(cpu, R->moby, tr); TR_frac = tr;
    } else if (moby_record_centre_final(cpu, R->moby, R->header, tr)) {
        TR_frac = tr;
    }
    return moby_decode_project(cpu, R->header, R->header_b, R->frame_a, R->frame_b, R->w0,
                               NULL, 0.0, -1.0, R->TR_raw, TR_frac, R->M,
                               zsf4, v, out, cap);
}

#define MAX_PROJ 32768
static proj_xy s_proj[MAX_PROJ]; static int s_proj_n;

/* Per-record metadata for the independent face-stream walk: where this
 * record's vertices live in s_proj[] (filled in stream order, so a face's stream
 * vertex index k maps to s_proj[proj_off + k]) + the face-stream base. */
typedef struct { uint32_t addr; int proj_off, proj_n; uint32_t header, header_b, faceblock, w0;
                 int metal; uint32_t mscol; uint32_t moby; int zsf4;
                 float env_az;
                 uint32_t frameA;
                 uint32_t frameA_p, header_p; float clr_alpha;
                 uint8_t spec_solo;
                 uint32_t model_held; /* nonzero = this record's keyframe is a
                                       * held previous pose, not the one the
                                       * cull record names, so the game's
                                       * keyframe patch list is resolved from
                                       * this model instead of the record */
                 uint8_t heat;     /* m_SpecularMetalColor byte2 (moby+0x4E),
                                    * snapshotted at capture: the flame-heat
                                    * level (ApplyFlameHeat*) - consumed by the
                                    * mod-texture face red-shift */
                 int M[3][3];      /* model-to-view rotation, Q12: the per-face
                                    * special material rotates a face normal by
                                    * it */
                 double zf;        /* the present's ZSF4 at full precision,
                                    * read only when zf_on (fade_snap_present_z);
                                    * the tier and face set stay zsf4's */
                 uint8_t zf_on; } rec_meta;
                 /* spec_solo (trailing, set after the positional literal) =
                 * reveal-built record whose moby is in NO .s queue this tick
                 * (beyond canonical range under --lod extended, or off-4:3)
                 * and whose renderer byte selects the specular pass
                 * (moby_is_shiny). The record is the moby's sole feeder, so
                 * the walk must apply the SPEC per-face treatment (env
                 * reflection / diffuse highlight) that the .s SPEC pass would
                 * have drawn - the s_cur_mat=='SP' pass gate alone left
                 * revealed metal capes rendering their dark baked base texture
                 * (the cliff_town caped gnorc). A record outside the specular
                 * pass carries a zero material word, which would flatten
                 * every face to 000000. */
                 /* frameA = the record's keyframe (rec+0x08 / f.frameA): the frame
                 * may carry a render-time FACEBLOCK PATCH list (see
                 * moby_faces_for_record) - 0 = no patch lookup.
                 * frameA_p/header_p/clr_alpha = the PREV drawn keyframe + the pose
                 * crossfade alpha (sub-tick re-extract only; 0 = canonical/no prev):
                 * drives the per-face COLOUR crossfade for patch-animated effects,
                 * whose colour indices step per keyframe (see the FCp block in
                 * build_walk_tris_inner). */
                 /* env_az = view-space TR azimuth atan2(TR.z,TR.x) - the
                 * .s SPEC env per-moby u-bias input (.L800226F8 cfc2 TRZ/TRX octant atan) */  /* metal = m_SpecularMetalColor
                 (rec+0x30) high byte (env flag); mscol = full word (highlight BK + spec byte);
                 moby = Moby* (rec+0x34 for .s records; set directly for host-built reveal records,
                 whose addr is then 0 - there is no live +0x1600 record behind them);
                 zsf4 = per-moby colour fade level (high16 of the cull record's matrix word @+0x2C,
                 cfc2 gte_ctrl[30]); see moby_color_fade_ir0 */
static rec_meta s_recs[MAX_RECORDS]; static int s_nrecs;
/* Nonzero while a sub-tick re-extract pass runs. */
static int s_walk_in_rex;

/** @brief The flame-heat scalar of a moby's m_SpecularMetalColor word, 0..96.
 *
 * Two writer encodings exist. ApplyFlameHeat raises byte 2 through 0..96
 * directly. ApplyFlameHeatExternal (metal chests) packs the whole word as
 * 0xa00000 + (scaled + 24) << 12 + (24 - scaled) << 6 + (24 - scaled),
 * scaled = heat >> 2 in 0..20: cold silver 0xa18618 decodes to 0, the
 * maximum 0xa2c104 to 20. A byte 2 above 96 identifies the packed form, whose
 * scaled field is clamped to 0..20 and multiplied by 4.
 * @param ms The m_SpecularMetalColor word.
 * @return The heat, 0..96. */
static int moby_heat_scalar(uint32_t ms) {
    int b2 = (int)((ms >> 16) & 0xFFu);
    if (b2 <= 96) return b2;
    int scaled = (int)((ms >> 12) & 0x3Fu) - 24;
    if (scaled < 0) scaled = 0;
    if (scaled > 20) scaled = 20;
    return scaled * 4;
}

/* Per-moby ZSF4 (colour-fade) snapshot, keyed by Moby*. Filled from the live .s
 * cull records during the canonical pass; the high-FPS sub-tick re-extract builds
 * host-side records (no live cull record to read +0x2C from) and looks the fade up
 * here, so fading effect mobys (ring puffs) fade on intermediate presents too
 * instead of flickering back to full brightness. Rolled once per canonical frame:
 * the table the last tick filled becomes the previous tick's.
 *
 * ZSF4 = ((m_Renderer & 0x3F) << 8) - TR.z (cull @8001F404-8001F418, TR.z the
 * record's quarter-world view Z, truncated). Beside it each entry keeps tz, the
 * same centre's view Z at full precision (build_moby_record_tr), when the
 * verdict is that formula of it: A = ZSF4 + tz is then the authored term plus
 * the cull's truncation residue, and a present at any camera and moby position
 * reads its own ZSF4 as A - tz_present. `dist` is 0 where tz is not available
 * or the verdict is not the formula: a game whose record stores the rescaled
 * translation (rec_tr_final), a game with no host record builder to derive
 * tz_present on sub-ticks (host_reveal 0), and a verdict the --no-lod read
 * neutralised or the int16 wrapped (|ZSF4 + tz - (m_Renderer << 8)| >= 4, where
 * the two truncations of the centre MVMVA stay below that).
 *
 * Each entry also keeps the AnimationHeader the pass drew the moby with and
 * that header's content signature at the pass, and the header and signature
 * of the last different header an earlier pass drew it with (carried from
 * the previous table while the header stays unchanged). A sub-tick present
 * that shows a previous pose from another header checks that header against
 * these: the game may refill a header slot it no longer draws. */
typedef struct { uint32_t moby; int zsf4; uint8_t dist; double tz;
                 uint32_t hdr, hdr_p; anim_sig_t sig, sig_p; } fade_ent;
static fade_ent s_fade_snap[MAX_RECORDS], s_fade_prev[MAX_RECORDS];
static int s_fade_snap_n, s_fade_prev_n;
/** @brief Roll the fade tables: this tick's entries become the previous tick's and the current table empties.
 *
 * Copies the s_fade_snap_n current entries over s_fade_prev. on_draw_regular
 * calls it at the start of each canonical regular pass. */
static void fade_snap_clear(void) {
    memcpy(s_fade_prev, s_fade_snap, (size_t)s_fade_snap_n * sizeof s_fade_snap[0]);
    s_fade_prev_n = s_fade_snap_n;
    s_fade_snap_n = 0;
}
/** @brief The entry naming a moby in a fade table, or NULL.
 * @param t The table.
 * @param n The entries in `t`.
 * @param moby The moby's PSX address.
 * @return The entry, or NULL when none names the moby. */
static fade_ent* fade_snap_find(fade_ent* t, int n, uint32_t moby) {
    for (int i = 0; i < n; i++) if (t[i].moby == moby) return &t[i];
    return NULL;
}
/** @brief Fill an entry's full-precision centre view Z, or mark it unavailable with dist 0.
 *
 * dist stays 0 on a game whose record stores the rescaled translation
 * (rec_tr_final), on one with no host record builder (host_reveal 0) or no
 * renderer byte, and where |ZSF4 + tz - ((m_Renderer & mask) << 8)| >= 4,
 * the verdict then not being the formula of tz (see fade_ent). Otherwise
 * dist is 1 and tz is build_moby_record_tr's Z.
 * @param cpu The guest CPU state, read only.
 * @param e The entry, its moby and zsf4 already set. */
static void fade_snap_dist(CPUState* cpu, fade_ent* e) {
    e->dist = 0; e->tz = 0.0;
    if (game_layout.moby.rec_tr_final || !moby_game.host_reveal || !moby_game.renderer_off) return;
    double tr[3];
    build_moby_record_tr(cpu, e->moby, tr);
    int md = (int)(cpu->read_byte(e->moby + moby_game.renderer_off) & moby_game.renderer_mask);
    if (fabs((double)e->zsf4 + tr[2] - (double)(md << 8)) >= 4.0) return;
    e->dist = 1; e->tz = tr[2];
}
/** @brief Record in an entry the drawn AnimationHeader, its signature, and the last different header before it.
 *
 * The previous pass's entry for the moby supplies the earlier header: when
 * it drew the same header its carried pair is kept, otherwise its own header
 * and signature become the pair. With no previous entry hdr_p is 0.
 * @param cpu The guest CPU state, read only.
 * @param e The entry, its moby already set.
 * @param header The header the pass drew the moby with. */
static void fade_snap_header(CPUState* cpu, fade_ent* e, uint32_t header) {
    const fade_ent* p = fade_snap_find(s_fade_prev, s_fade_prev_n, e->moby);
    e->hdr = header;
    e->sig = anim_header_sig(cpu, header);
    if (!p)                    { e->hdr_p = 0u; }
    else if (p->hdr == header) { e->hdr_p = p->hdr_p; e->sig_p = p->sig_p; }
    else                       { e->hdr_p = p->hdr;   e->sig_p = p->sig; }
}
/** @brief Record a moby's ZSF4, centre view Z and drawn header in this tick's fade table.
 *
 * Canonical pass only: a call during a sub-tick re-extract returns, since a
 * sub-tick present re-derives what it draws from the tick's entries and never
 * replaces them. An address below KSEG0 is ignored and a new moby past
 * MAX_RECORDS entries is dropped. A header below KSEG0 clears both header
 * fields.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param zsf4 The record's ZSF4.
 * @param header The AnimationHeader the pass drew the moby with. */
static void fade_snap_put(CPUState* cpu, uint32_t moby, int zsf4, uint32_t header) {
    if (moby < 0x80000000u || s_walk_in_rex) return;
    fade_ent* e = fade_snap_find(s_fade_snap, s_fade_snap_n, moby);
    if (!e) {
        if (s_fade_snap_n >= MAX_RECORDS) return;
        e = &s_fade_snap[s_fade_snap_n++];
        e->moby = moby;
    }
    e->zsf4 = zsf4;
    fade_snap_dist(cpu, e);
    if (header >= 0x80000000u) fade_snap_header(cpu, e, header);
    else                       e->hdr = e->hdr_p = 0u;
}
/** @brief Whether a header a canonical pass's table records for a moby still holds the content it held when drawn.
 *
 * `header` is matched against the entry's drawn header and the last
 * different one before it, and its current signature against the one
 * recorded with it.
 * @param cpu The guest CPU state, read only.
 * @param t The pass's table.
 * @param n Entries in `t`.
 * @param moby The moby's PSX address.
 * @param header The header to check.
 * @return 1 it does, 0 its signature changed, -1 no entry names it. */
static int fade_header_intact(CPUState* cpu, fade_ent* t, int n, uint32_t moby,
                              uint32_t header) {
    const fade_ent* e = fade_snap_find(t, n, moby);
    if (!e || header < 0x80000000u) return -1;
    const anim_sig_t* want = (e->hdr == header) ? &e->sig
                           : (e->hdr_p == header) ? &e->sig_p : NULL;
    if (!want) return -1;
    anim_sig_t now = anim_header_sig(cpu, header);
    return now.w0 == want->w0 && now.w1 == want->w1;
}
/** @brief The ZSF4 the canonical pass recorded for a moby, or a default when it has no entry.
 *
 * Reads the current tick's table only. On a miss the caller supplies the tier
 * to draw at.
 * @param moby The moby's PSX address.
 * @param dflt The value returned on a miss.
 * @return The recorded ZSF4, or `dflt`. */
static int fade_snap_get(uint32_t moby, int dflt) {
    const fade_ent* e = fade_snap_find(s_fade_snap, s_fade_snap_n, moby);
    return e ? e->zsf4 : dflt;   /* snapshot miss: caller picks the safe tier */
}
/** @brief The full-precision ZSF4 a sub-tick present draws a moby's colour fade at.
 *
 * The authored term lerped between the two ticks, less the present's own
 * centre Z (tz_present, the build_moby_record_tr value this present projects
 * through):
 *   z = ZSF4 + (1 - alpha)(A_prev - A_curr) + (tz_curr - tz_present).
 * A moby absent from the previous tick holds A_curr. Both correction terms are
 * exactly 0 at alpha 1 (tz_present is the tick's own centre there), and the
 * result is then not marked fractional, so that present runs the integer GTE
 * mirror. A missing entry, one without dist, or one whose ZSF4 differs from
 * `zsf4` also yields `zsf4`.
 * @param moby The moby's PSX address.
 * @param zsf4 The record's integer ZSF4.
 * @param tz_present The present's centre view Z.
 * @param alpha The present's phase, 0 at the previous tick and 1 at this one.
 * @param zf Receives the value; `zsf4` when not fractional.
 * @return 1 when the present's value is fractional, else 0. */
static int fade_snap_present_z(uint32_t moby, int zsf4, double tz_present, double alpha,
                               double* zf) {
    *zf = (double)zsf4;
    const fade_ent* c = fade_snap_find(s_fade_snap, s_fade_snap_n, moby);
    if (alpha >= 1.0 || !c || !c->dist || c->zsf4 != zsf4) return 0;
    const fade_ent* p = fade_snap_find(s_fade_prev, s_fade_prev_n, moby);
    double dA = (p && p->dist) ? ((double)p->zsf4 + p->tz) - ((double)c->zsf4 + c->tz) : 0.0;
    *zf = (double)zsf4 + (1.0 - alpha) * dA + (c->tz - tz_present);
    return *zf != (double)zsf4;
}

/* Moby pointers fed by the REGULAR pass this frame - the SPECULAR pass dedups
 * against this so a moby drawn by BOTH passes (the common case; .s overlaps them
 * at one pose) is fed ONCE (by REGULAR, at its pre-specular-cull pose), while a
 * moby UNIQUE to the specular queue (e.g. the Artisans rescue dragon cls 0xFA,
 * which the specular cull writes into a slot a regular moby held at REGULAR-read
 * time) is still fed by SPECULAR. Snapshotted in the REGULAR pass (runs first). */
static uint32_t s_reg_moby[MAX_RECORDS]; static int s_reg_moby_n;

/* Mobys the CANONICAL .s render passes actually drew this tick, harvested from
 * their cull records (REGULAR + SPECULAR, deduped).
 *
 * The interp capture and the sub-tick spine's lerp walk both iterate
 * g_LevelMobys, but the game also renders mobys that live OUTSIDE that array -
 * the balloonist copies the balloon out of g_LevelMobys into the render scratch
 * (g_SonyImage.m_WorldFarQueued[106]) before the level swap and draws it from
 * there for the whole arrival flight. Such a moby has no prev/curr snapshot and
 * no fractional position, so it steps at the sim rate while the camera, Spyro
 * and the terrain interpolate around it. The cull records are the game's own
 * authoritative "what is being drawn" list, so harvesting them covers that class
 * generally instead of special-casing each relocation.
 *
 * Refreshed per canonical tick (keyed on the VBL the passes ran in, so REGULAR
 * resets and SPECULAR appends); read on the host fiber right after the game
 * yields, while the referenced records are still live. */
static uint32_t s_canon_drawn[MAX_RECORDS * 2]; static int s_canon_drawn_n;
static unsigned s_canon_drawn_frame = ~0u;

int moby_native_canon_drawn_mobys(const uint32_t** out) {
    *out = s_canon_drawn;
    /* Stale-bound: the game draws every other VBL, so a set older than that is
     * from before a stall - a CD load or level swap, after which the addresses
     * name whatever now occupies that memory. Reporting none is correct there:
     * nothing was drawn to interpolate. */
    return (openpete_frame - s_canon_drawn_frame <= 2u) ? s_canon_drawn_n : 0;
}

/** @brief Add the moby of each live cull record to the canonical drawn set, skipping duplicates.
 *
 * The set is emptied when openpete_frame differs from the VBL that last
 * harvested, so the regular pass resets it and the specular pass of the same
 * VBL appends. A record whose moby is below KSEG0 is skipped, and the set stops
 * at its capacity. moby_native_canon_drawn_mobys reads the set.
 * @param nrec The number of records in s_recs[] to read. */
static void canon_drawn_harvest(int nrec) {
    if (openpete_frame != s_canon_drawn_frame) {
        s_canon_drawn_frame = openpete_frame;
        s_canon_drawn_n = 0;
    }
    const int cap = (int)(sizeof s_canon_drawn / sizeof s_canon_drawn[0]);
    for (int r = 0; r < nrec && s_canon_drawn_n < cap; r++) {
        uint32_t m = s_recs[r].moby;
        if (m < 0x80000000u) continue;
        int dup = 0;
        for (int k = 0; k < s_canon_drawn_n; k++) if (s_canon_drawn[k] == m) { dup = 1; break; }
        if (!dup) s_canon_drawn[s_canon_drawn_n++] = m;
    }
}

/** @brief The face block a record draws under its LOD tier, mirroring func_8001F798 block_8001FDD0 (specular twin @80021670).
 *
 * The record's ZSF4 (+0x2E, sign-extended via ctc2/cfc2 gte_ctrl[30]) is the
 * game's own per-record LOD verdict: the cull pass computes it @8001F404 as
 * ((moby->m_Renderer(+0x4B) & 0x3F) << 8) - viewZ (viewZ = the record's TR.z)
 * and stores its low 16 bits. The renderer's tier ladder:
 *   ZSF4 >= 1024    -> HP faces (hdr+0x14) + raw model colours (hdr+0x18)
 *   0 < ZSF4 < 1024 -> HP faces + colours DPCS-faded by (1024-ZSF4)*4 toward
 *                      the per-vertex far palette (hdr+0x20)  [.L8001FDE8]
 *   ZSF4 <= 0       -> LP faces (hdr+0x1C) + the far palette (hdr+0x20) as-is;
 *                      below -2048 (g_SkipLowPolyWorld > 0) DPCS-faded toward
 *                      the cyclorama fog colour by min(-(ZSF4+2048),2047)<<1
 *                      [.L8001FE98..FECC]
 * Under --no-lod the face select takes the HP faces for every record, keyed
 * on openpete_no_lod: the .s producers run canonical and this walk owns the
 * tier. That includes the records whose ZSF4 stays <= 0 under --no-lod, the
 * effect-authored fades (m_Renderer & 0x3F < 4, which record_zsf4 does not
 * lift). Colours stay tier-faithful in both modes; the game keeps the far
 * palette under --no-lod too.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @param zsf4 The record's tier verdict.
 * @return The face-block address, or 0 when header is not an address. */
static uint32_t moby_tier_faceblock(CPUState* cpu, uint32_t header, int zsf4) {
    if (header < 0x80000000u) return 0u;
    uint32_t off = (zsf4 <= 0 && !openpete_no_lod) ? ANIM_FACES_LP : ANIM_FACES_HP;
    return cpu->read_word(header + off);
}

/* OPENPETE_MOBY_TIER_DBG: ~1/sec canonical tier census + validation that the
 * mirror formula reproduces the stored ZSF4 (mirror bad=0 proves the decode). */
static long s_tier_near, s_tier_fog, s_tier_far, s_tier_farfade, s_tier_gpfskip;
static long s_tier_lpskip;   /* far-tier records skipped by the LP vertex count == 0
                              * (no LP mesh): the .s draws NOTHING for them */
static long s_shband_cull;   /* SHADED-walk mobys skipped by the .s 3-axis distance-band
                              * cull mirror (block_80022B40..90 - the distant-gem hide) */
static long s_tier_mirror_ok, s_tier_mirror_bad;
static long s_hold_replayed;   /* Tier-2 hold: moby replays served per census window */
/** @brief Whether the tier census and extraction timers are on (OPENPETE_MOBY_TIER_DBG).
 *
 * Read on the first call and latched.
 * @return 1 when the variable is set to a value not starting with '0',
 *         else 0. */
static int tier_dbg_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_TIER_DBG"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s;
}

/* Extraction µs accumulators (printed in the OPENPETE_MOBY_TIER_DBG census):
 * split the moby extraction cost into its two halves - the vertex decode +
 * projection (proj) and the face-stream walk + tri build (walk) - across ALL
 * callers (canonical pass + per-present re-extract + SPEC), so a caching
 * change is measured by the same instrument before and after. */
static long s_us_proj, s_us_walk, s_us_walk_rex, s_us_calls;
/* Which pass the face walk is running under. The canonical producers walk once
 * per TICK and the sub-tick re-extract walks once per PRESENT; pooled into one
 * accumulator the total is neither quantity and has to be corrected by hand
 * before it can be compared against a per-present scope. */
/* Re-extract record-loop split, the two halves a worker fan-out would target:
 * the per-record field rebuild and the model decode + projection. */
static long s_us_dec, s_us_fields;
/* Fan-out overhead, split: the whole parallel run against the ordered
 * merge that follows it. The merge copies nine parallel arrays per
 * RECORD, so its cost tracks record count rather than triangle count. */
static long s_us_fanout, s_us_merge;
/* Face-stream size per walk, the quantity a fan-out decision has to be made
 * on: the prologue knows it before dispatching, and walk cost tracks it. */
static long s_walk_faces, s_walk_runs;

/** @brief The key the sub-tick interp ring stores a moby's prev and curr snapshots under.
 *
 * moby_game.interp_key maps the address where the game supplies one;
 * otherwise the key is the address itself.
 * @param moby The moby's PSX address.
 * @return The ring key. */
static uint32_t moby_interp_key(uint32_t moby) {
    return moby_game.interp_key ? moby_game.interp_key(moby) : moby;
}

/** @brief The snapshot pair a moby is interpolated with: the interp table's entry under its ring key.
 *
 * @param moby The moby's PSX address.
 * @return The pair, or NULL when the table holds none. */
static const openpete_interp_obj_t* moby_interp_find(uint32_t moby) {
    return openpete_interp_objs_find(moby_interp_key(moby));
}

/** @brief The moby OPENPETE_MOBY_TRACE names for a per-call gate trace, 0 when unset.
 *
 * The canonical extract logs the moby's queue membership and the reveal
 * builder logs every skip or admit verdict with the gate values. The variable
 * is parsed once, with strtoul base 0.
 * @return The traced Moby address, or 0. */
static uint32_t moby_trace_target(void) {
    static uint32_t t = 0xFFFFFFFFu;
    if (t == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_MOBY_TRACE");
                            t = e ? (uint32_t)strtoul(e, NULL, 0) : 0u; }
    return t;
}

static int s_max_blend;   /* max keyframe-blend factor (rec.w0 IR0) across this frame's records */
static int project_all_records_inner(CPUState* cpu);
/** @brief Project every live record's vertices into s_proj, timed under the tier census.
 *
 * Shared by the verify A/B and the scene feed. The projection is
 * project_all_records_inner's native float decode with RTPS and TR. While
 * OPENPETE_MOBY_TIER_DBG is on the call's microseconds are added to
 * s_us_proj and s_us_calls is incremented.
 * @param cpu The guest CPU state.
 * @return The record count. */
static int project_all_records(CPUState* cpu) {
    if (!tier_dbg_on()) return project_all_records_inner(cpu);
    uint64_t t0 = openpete_phase_now_us();
    int n = project_all_records_inner(cpu);
    s_us_proj += (long)(openpete_phase_now_us() - t0); s_us_calls++;
    return n;
}
/* Engagement census of the per-face special-material hook: one count per
 * treatment the game's slot reported applying. */
static long s_special_faces[3];
/* Faces the walk emitted carrying the flag that selects the draw's second
 * dispatcher, which the walk does not model: the engagement count of the tag
 * a material compare reads. */
static long s_bit4_faces;

/* OPENPETE_MOBY_SPECIAL_CENSUS=1: run-cumulative counts of the faces carrying
 * either dispatcher flag, the material mode each bit-5 face selected, the
 * first moby address and VBL seen in each mode, and the total faces walked.
 * The counters above are cleared per draw pass, so they answer "at this
 * instant"; these accumulate over the run and answer "does this level draw
 * such a face at all", which is what locating a row for a treatment needs.
 * The total makes a zero line evidence rather than silence: a run that walked
 * no face proves nothing about the flag. */
static long     s_cen_mode[8];      /* bit-5 faces by material mode 0..7 */
static uint32_t s_cen_mode_moby[8];
static unsigned s_cen_mode_vbl[8];
/* Screen box of the bit-5 faces emitted since the last report: the crop a
 * pixel comparison against the reference has to look inside, since these faces
 * cover a few percent of one moby and a whole-frame error is noise to them. */
static float    s_cen_bx0, s_cen_by0, s_cen_bx1, s_cen_by1;
static int      s_cen_box_n;
static long     s_cen_bit5, s_cen_bit4, s_cen_faces, s_cen_mat0;
static uint32_t s_cen_bit4_moby;
static unsigned s_cen_bit4_vbl, s_cen_at;
/* Distinct material words seen on a bit-5 face: the treatment reads all three
 * of the word's colour channels and its per-moby IR0 field, so which words
 * occur decides whether the two coded forms of mode 0 can differ at all. */
#define CEN_MATS 12
static uint32_t s_cen_mat[CEN_MATS];
static long     s_cen_mat_n[CEN_MATS];
/** @brief Whether the special-face census is on (OPENPETE_MOBY_SPECIAL_CENSUS).
 *
 * Read on the first call and latched.
 * @return 1 when the variable is set to a value not starting with '0',
 *         else 0. */
static int special_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SPECIAL_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}
/** @brief Print the special-face census line to stderr and restart the screen box.
 *
 * The line carries the face totals, the first moby and VBL per material mode
 * and for the bit-4 flag, the distinct material words seen, and the screen
 * box of the bit-5 faces since the last report.
 * Clears the box count. */
static void special_census_report(void) {
    fprintf(stderr, "[moby-special vbl=%u] faces=%ld bit5=%ld(mat0=%ld) bit4=%ld(m=%08X vbl=%u) modes",
            openpete_frame, s_cen_faces, s_cen_bit5, s_cen_mat0, s_cen_bit4,
            s_cen_bit4_moby, s_cen_bit4_vbl);
    for (int m = 0; m < 8; m++)
        if (s_cen_mode[m])
            fprintf(stderr, " %d:%ld(m=%08X vbl=%u)", m, s_cen_mode[m],
                    s_cen_mode_moby[m], s_cen_mode_vbl[m]);
    fprintf(stderr, " | mats");
    for (int i = 0; i < CEN_MATS && s_cen_mat_n[i]; i++)
        fprintf(stderr, " %08X:%ld", s_cen_mat[i], s_cen_mat_n[i]);
    if (s_cen_box_n)
        fprintf(stderr, " | box=(%.0f,%.0f)-(%.0f,%.0f) n=%d",
                s_cen_bx0, s_cen_by0, s_cen_bx1, s_cen_by1, s_cen_box_n);
    fprintf(stderr, "\n");
    s_cen_box_n = 0;
}
/** @brief Count one walked face into the special-face census.
 *
 * Does nothing while the census is off. A face with F->hl == 1 (bit 5)
 * extends the screen box by its unsaturated vertices, and is counted under
 * material mode (mscol >> 28) & 7 (0 when mscol is 0), the first moby key
 * and VBL of that mode, and the distinct material words. A face with
 * F->hl == 2 (bit 4) is counted with the first moby key and VBL seen. The
 * report prints once every 200 VBLs.
 * @param F The face.
 * @param mscol The record's material word (m_SpecularMetalColor).
 * @param key The moby key the census reports.
 * @param p0 The first projected vertex, or NULL.
 * @param p1 The second.
 * @param p2 The third. */
static void special_census_face(const moby_wface_t* F, uint32_t mscol, uint32_t key,
                                const proj_xy* p0, const proj_xy* p1, const proj_xy* p2) {
    if (!special_census_on()) return;
    s_cen_faces++;
    if (F->hl == 1u) {
        const proj_xy* pv[3] = { p0, p1, p2 };
        for (int i = 0; i < 3; i++) {
            if (!pv[i] || pv[i]->sat) continue;
            if (!s_cen_box_n) { s_cen_bx0 = s_cen_bx1 = pv[i]->sx; s_cen_by0 = s_cen_by1 = pv[i]->sy; }
            if (pv[i]->sx < s_cen_bx0) s_cen_bx0 = pv[i]->sx;
            if (pv[i]->sx > s_cen_bx1) s_cen_bx1 = pv[i]->sx;
            if (pv[i]->sy < s_cen_by0) s_cen_by0 = pv[i]->sy;
            if (pv[i]->sy > s_cen_by1) s_cen_by1 = pv[i]->sy;
            s_cen_box_n++;
        }
    }
    if (F->hl == 2u) {
        s_cen_bit4++;
        if (!s_cen_bit4_moby) { s_cen_bit4_moby = key; s_cen_bit4_vbl = openpete_frame; }
    } else if (F->hl == 1u) {
        int cm = mscol ? (int)((mscol >> 28) & 7u) : 0;
        s_cen_bit5++;
        s_cen_mat0 += (mscol == 0u);
        s_cen_mode[cm]++;
        if (!s_cen_mode_moby[cm]) { s_cen_mode_moby[cm] = key; s_cen_mode_vbl[cm] = openpete_frame; }
        for (int i = 0; i < CEN_MATS; i++) {
            if (s_cen_mat_n[i] && s_cen_mat[i] != mscol) continue;
            s_cen_mat[i] = mscol; s_cen_mat_n[i]++; break;
        }
    }
    if (openpete_frame >= s_cen_at + 200u) { s_cen_at = openpete_frame; special_census_report(); }
}

/** @brief Appends one line to the file OPENPETE_ROWSCALE_CENSUS names.
 *
 * The line names which row-scale source fired on which record, moby and row,
 * and the factor. A harness sets it to count the firings over a corpus; unset,
 * nothing is written. */
static void rowscale_census(const char* src, uint32_t rec, uint32_t moby, int row, double sc) {
    static const char* s_path = NULL;
    static int s_init = 0;
    if (!s_init) { s_init = 1; s_path = getenv("OPENPETE_ROWSCALE_CENSUS"); }
    if (!s_path || !*s_path) return;
    FILE* fp = fopen(s_path, "a");
    if (!fp) return;
    fprintf(fp, "%s vbl=%u rec=%08x moby=%08x row=%d sc=%.5f\n",
            src, openpete_frame, rec, moby, row, sc);
    fclose(fp);
}

/** @brief Applies the game's draw-time row scale of the record at `rec` to a matrix built for that record.
 *
 * The factors are the ones the game's draw multiplies into the record after its
 * cull (moby_game.record_row_scale); each row is scaled in double and rounded
 * once. The specular queue shares the record addresses and is never scaled, so
 * a specular pass ("SP"/"SPEC") applies nothing. Returns 1 when the game scales
 * the record. */
static int moby_draw_row_scale(CPUState* cpu, uint32_t rec, const char* mat, int M[3][3]) {
    if (!moby_game.record_row_scale || (mat && mat[0] == 'S' && mat[1] == 'P')) return 0;
    double sc[3];
    if (!moby_game.record_row_scale(cpu, rec, sc)) return 0;
    for (int row = 0; row < 3; row++) {
        for (int c = 0; c < 3; c++) M[row][c] = (int)lround(M[row][c] * sc[row]);
        rowscale_census("hook", rec, cpu->read_word(rec + game_layout.moby.rec_moby_off), row, sc[row]);
    }
    return 1;
}

/** @brief The canonical record of a draw-time row scale, rebuilt as the sub-tick rebuild derives it.
 *
 * The matrix is the camera·euler compose of the record's moby
 * (shaded_compose_euler, the builder's compose) with the game's factors applied
 * by moby_draw_row_scale. The record's own matrix carries the draw's FIXED_MUL
 * truncation of the same product; replacing it makes the canonical present and
 * the alpha-1 sub-tick present one computation. Records the draw does not scale
 * keep the cull's matrix. */
static void moby_canon_row_scale(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    double sc[3];
    if (!moby_game.host_reveal || !moby_game.record_row_scale ||
        (s_cur_mat && s_cur_mat[0] == 'S' && s_cur_mat[1] == 'P') ||
        R->moby < 0x80000000u || !moby_game.record_row_scale(cpu, rec, sc))
        return;
    int PM[3][3]; memcpy(PM, s_view.cam_M, sizeof PM);
    shaded_compose_euler(cpu, PM, R->moby, R->M);
    moby_draw_row_scale(cpu, rec, s_cur_mat, R->M);
}

/** @brief Re-applies a render-time per-row scale the game baked into the record's matrix.
 *
 * No moby_game.record_row_scale source names the scale, and the euler rebuild
 * in M cannot see it. The rebuild is a camera·euler rotation, so |rebuild row|
 * equals |unscaled record row| and the ratio |record row| / |rebuild row| is
 * that scale; a ratio inside 0.99-1.01 is Q12 truncation noise and is left
 * alone. */
static void moby_ratio_row_scale(CPUState* cpu, uint32_t rec, uint32_t moby, int M[3][3]) {
    int rM[3][3]; moby_rec_matrix(cpu, rec + game_layout.moby.rec_matrix_off, rM);
    for (int row = 0; row < 3; row++) {
        double mr = sqrt((double)rM[row][0]*rM[row][0] + (double)rM[row][1]*rM[row][1] + (double)rM[row][2]*rM[row][2]);
        double mb = sqrt((double)M[row][0]*M[row][0] + (double)M[row][1]*M[row][1] + (double)M[row][2]*M[row][2]);
        if (mb <= 1.0) continue;
        double sc = mr / mb;
        if (sc >= 0.99 && sc <= 1.01) continue;
        for (int c = 0; c < 3; c++) M[row][c] = (int)lround(M[row][c] * sc);
        rowscale_census("ratio", rec, moby, row, sc);
    }
}

static void moby_animsw_census(CPUState* cpu, uint32_t moby);

/** @brief Projects every live cull record's vertices into s_proj for the canonical pass and fills s_recs.
 *
 * The projection is the native float decode, RTPS and TR; s_recs is the record
 * metadata the walk and the sub-tick replays read. A record the game's draw
 * row-scales takes the rebuilt matrix (moby_canon_row_scale). Returns the
 * record count. */
static int project_all_records_inner(CPUState* cpu) {
    s_proj_n = 0; s_max_blend = 0; s_nrecs = 0;
    s_special_faces[0] = s_special_faces[1] = s_special_faces[2] = 0;
    s_bit4_faces = 0;
    int nrec = 0;
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = s_rec_base + (uint32_t)r * REC_STRIDE;
        moby_record_t R;
        if (!moby_game.read_record(cpu, rec, &R)) break;   /* queue terminator */
        uint32_t w0 = R.w0;
        int bl = (int)((w0 >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask);  /* INTPL blend 0..4096 */
        if (bl > s_max_blend) s_max_blend = bl;
        int off = s_proj_n;
        /* ZSF4 verdict BEFORE the projection - the decode's tier-adjusted
         * vertex-count read ([hdr+8+(ZSF4>>31)], 0 = record skipped) needs it.
         * Snapshotted into fade_snap below, so the sub-tick re-extract
         * inherits the (--no-lod-neutralized) tier too. */
        int zsf4 = record_zsf4(cpu, &R);
        moby_animsw_census(cpu, R.moby);
        moby_canon_row_scale(cpu, rec, &R);
        pose_hold_begin(R.moby, R.header, R.header_b, R.frame_a, R.frame_b, R.w0);
        int got = moby_project_record(cpu, &R, zsf4, s_proj + s_proj_n, MAX_PROJ - s_proj_n);
        pose_hold_end(got);
        for (int j = 0; j < got; j++) s_proj[s_proj_n + j].rec = nrec;  /* tag source record */
        if (nrec < MAX_RECORDS) {
            uint32_t header = R.header;
            s_recs[nrec] = (rec_meta){ rec, off, got, header, R.header_b,
                /* face stream per the record's tier (HP or LP, --no-lod-aware) */
                moby_tier_faceblock(cpu, header, zsf4), w0,
                /* env metal flag = high byte of the record's material word (the
                 * cull pass copies it; read @8002176C srl 24, bnez .L800226F8) */
                (int)((R.material >> 24) & 0xFFu),
                /* the full material word: the highlight back-colour (.L80021DB4) +
                 * byte2 spec-intensity. Carries the per-moby tint (flamed chest -> red). */
                R.material,
                R.moby,
                zsf4,
                atan2f((float)R.TR_raw[2], (float)R.TR_raw[0]),
                R.frame_a, 0u, 0u, 0.f };
            memcpy(s_recs[nrec].M, R.M, sizeof s_recs[nrec].M);
            s_recs[nrec].heat = moby_game.spec_colour_off
                ? (uint8_t)moby_heat_scalar(
                      cpu->read_word(s_recs[nrec].moby + moby_game.spec_colour_off))
                : 0u;
            fade_snap_put(cpu, s_recs[nrec].moby, zsf4, header);
            if (moby_trace_target() && s_recs[nrec].moby == moby_trace_target())
                fprintf(stderr, "[moby-trace vbl=%u] %08X IN-QUEUE rec=%08X zsf4=%d got=%d mat=%s\n",
                        openpete_frame, s_recs[nrec].moby, rec, zsf4, got,
                        s_cur_mat ? s_cur_mat : "?");
            if (tier_dbg_on()) {
                if (zsf4 >= 1024) s_tier_near++;
                else if (zsf4 > 0) s_tier_fog++;
                else if (zsf4 < -2048 && moby_game.skip_lowpoly &&
                         (int32_t)cpu->read_word(moby_game.skip_lowpoly) > 0) s_tier_farfade++;
                else s_tier_far++;
                if ((w0 & 0xFFu) && got == 0) s_tier_gpfskip++;
                if (!(w0 & 0xFFu) && got == 0 && zsf4 < 0) s_tier_lpskip++;
                /* mirror check: recompute ZSF4 from gamestate the way the cull
                 * does (@8001F404) and compare with the stored verdict. The
                 * cull subtracts the translation Z it holds before its
                 * rescale; a record that stores the rescaled translation
                 * (rec_tr_final) no longer carries that value, so the check
                 * has no input there. */
                uint32_t mob = R.moby;
                if (mob >= 0x80000000u && moby_game.renderer_off &&
                    !game_layout.moby.rec_tr_final) {
                    int md = (int)(cpu->read_byte(mob + moby_game.renderer_off) & moby_game.renderer_mask);
                    int ex = (int)(int16_t)(uint16_t)((((uint32_t)md << 8)
                             - (uint32_t)R.TR_raw[2]) & 0xFFFFu);
                    /* --no-lod: the zsf4-store hook neutralized the distance term
                     * for near-band-at-zero-distance records - mirror that */
                    if (openpete_no_lod && (md << 8) >= 1024) ex = md << 8;
                    if (ex == zsf4) s_tier_mirror_ok++; else s_tier_mirror_bad++;
                }
            }
        }
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    s_nrecs = nrec;
    moby_centre_census("canon");
    return nrec;
}

/* ---- widescreen live-moby reveal (host-built records) ---------------------
 * In widescreen the .s cull (run headless at canonical 4:3) only writes +0x1600
 * records for 4:3-visible mobys, so enemies/fodder/Sparx/gems pop in at the
 * screen edges. We must NOT re-run the cull (it sets m_WasDrawn - a render->
 * gameplay leak). Instead rebuild the per-moby record FIELDS host-side from
 * gamestate, exactly as func_8001F158 does, for the mobys the 4:3 cull skipped,
 * and decode/project them through moby_decode_project (the same core).
 *
 * Validated against the .s records for in-frustum mobys (OPENPETE_MOBY_BUILDDBG):
 * a host-built record for a moby the .s already culled in must match the .s record
 * field-for-field (M/TR within GTE truncation, header/frameA/frameB/blend exact).
 * Only after that A/B passes is the builder trusted on the revealed (off-4:3) set. */
typedef struct { uint32_t header, header_b, frameA, frameB, w0, mscol; int metal; int TR_raw[3]; double TR_frac[3]; int M[3][3]; } moby_fields;

/** @brief Build the regular (animated) render-record fields for one moby from game state.
 *
 * Header, second header, keyframes and blend come from moby_pose_derive over
 * the five anim state bytes; w0 carries the blend from bit 8 and
 * m_DepthOffset (the top byte of the angle word) in bits 16-23, like the .s
 * cull at 8001F464..F46C. M is the camera projection matrix times the moby's
 * euler rotation (shaded_compose_euler). TR_raw is the cull's centre MVMVA of
 * the camera-to-moby offset >> 2, accumulated in 64 bits and clamped to
 * int32, without m_Scale, which moby_decode_project applies; TR_frac is the
 * same centre at full precision from build_moby_record_tr.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param f Receives the fields.
 * @return 1 on success, 0 when the class has no model, the model is not
 *         animated (the shaded path draws it) or the pose does not derive. */
static int build_moby_record_fields(CPUState* cpu, uint32_t moby, moby_fields* f) {
    uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);            /* m_Class */
    uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);    /* g_Models[cls] */
    if (model < 0x80000000u) return 0;
    if (!moby_game.model_is_animated(cpu, model)) return 0;  /* the second model kind */
    uint8_t st[5];
    for (uint32_t k = 0; k < 5u; k++) st[k] = cpu->read_byte(moby + game_layout.moby.anim_state_off + k);
    moby_pose_t pose;
    if (!moby_pose_derive(cpu, model, st, &pose)) return 0;
    f->header   = pose.header;
    f->header_b = pose.header_b;
    f->frameA   = pose.frame_a;
    f->frameB   = pose.frame_b;
    f->w0       = pose.blend << 8;
    /* m_DepthOffset (top byte of the +0x44 word) -> w0 bits 16-23, like the .s
     * cull [@8001F464..F46C] - the render walk's OT near-pull reads it there. */
    f->w0 |= (cpu->read_word(moby + game_layout.moby.angle_off + 0u) >> 24) << 16;
    f->mscol = cpu->read_word(moby + moby_game.spec_colour_off);                 /* m_SpecularMetalColor */
    f->metal = (int)((f->mscol >> 24) & 0xFFu);
    /* M = ProjMatrix . Ry . Rx . Rz(m_Rotation) - the SAME euler compose the shaded
     * path uses, from the moby angle bytes. */
    int PM[3][3]; memcpy(PM, s_view.cam_M, sizeof PM);
    shaded_compose_euler(cpu, PM, moby, f->M);
    /* TR = ProjMatrix . swizzled (cam-moby)>>2  (the cull's centre MVMVA: IR1=(cy-my)>>2,
     * IR2=(cz-mz)>>2, IR3=(mx-cx)>>2; MAC=(PM·IR)>>12). NOT scaled by m_Scale or <<1
     * here - moby_decode_project applies (<<2)>>m_Scale, matching the .s record store. */
    int32_t cx=(int32_t)s_view.cam_ram[0], cy=(int32_t)s_view.cam_ram[1], cz=(int32_t)s_view.cam_ram[2];
    int32_t mx=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u), my=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u), mz=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    int32_t Vy=(cy-my)>>2, Vz=(cz-mz)>>2, Vx=(mx-cx)>>2;
    /* 64-bit accumulate: the products reach 4096*|V|, so an int32 sum wraps once
     * a component passes ~175 K view units (~700 K world) and the wrapped view-Z
     * reads as behind-camera. Reachable under extended LOD, which has no
     * distance ceiling. Identical to the 32-bit form
     * everywhere it did not overflow, and closer to the GTE, whose MAC
     * accumulator is 44-bit. */
    for (int i=0;i<3;i++) {
        int64_t acc = ((int64_t)PM[i][0]*Vy + (int64_t)PM[i][1]*Vz + (int64_t)PM[i][2]*Vx) >> 12;
        f->TR_raw[i] = (int32_t)(acc > INT32_MAX ? INT32_MAX
                                : (acc < INT32_MIN ? INT32_MIN : acc));
    }
    /* TR_frac: the same view-space centre at full precision, from the one site
     * that derives it. Every pass reaches the centre through that call, so the
     * canonical present and the alpha-1 sub-tick present of the same tick read
     * one number. */
    build_moby_record_tr(cpu, moby, f->TR_frac);
    return 1;
}

static void moby_centre_at(CPUState* cpu, uint32_t moby, const double woff[3], double tr[3]);
/** @brief The moby's fractional view-space centre at the present's camera, in stored-translation units.
 *
 * Calls moby_centre_at with no world offset, so the centre comes from the
 * interpolated moby position when one resolves and from the RAM position
 * otherwise. Every pass that projects a moby reads its centre through this
 * call.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param tr Receives the centre. */
static void build_moby_record_tr(CPUState* cpu, uint32_t moby, double tr[3]) {
    moby_centre_at(cpu, moby, NULL, tr);
}

/** @brief The moby's view-space centre at full precision, on the scale of the record's stored translation.
 *
 * The cull's centre MVMVA (IR1 = cam.y - moby.y, IR2 = cam.z - moby.z,
 * IR3 = moby.x - cam.x, MAC = (PM . IR) >> 12) with the delta divided by 4.0
 * instead of >> 2 and the MAC by 4096.0 instead of >> 12, taking the view's
 * camera (s_view.cam) and the moby's interpolated position
 * (openpete_native_interp_pos) over the floored RAM words. A moby then
 * follows the interpolated camera instead of stepping in 4-world-unit
 * increments, the rule the terrain path takes for its cam >> 4 delta, and
 * neither camera nor moby motion steps on the 1-world-unit lerp32 grid.
 *
 * This is the single derivation of the centre: the canonical record pass,
 * the host record builder (widescreen reveal and sub-tick re-extract), and
 * the hold capture and replay reach it directly or through
 * build_moby_record_tr, so a present's centre for a moby is a function of
 * that present's camera and the moby's position alone. At alpha 1 the interp
 * resolvers return the tick's own positions, so the sub-tick present's centre
 * is the canonical present's. No rotation compose and no model read happen
 * here: the hold replay needs only the origin projection.
 *
 * OPENPETE_REG_INTERP_MISS logs, at most 12 lines per second, a moby with no
 * fractional position on a pass whose camera is interpolated.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param woff World displacement added to the RAM position when no fractional position resolves (the record-replay games, whose positions reach the present through moby_record_world_offset), or NULL.
 * @param tr Receives the centre. */
static void moby_centre_at(CPUState* cpu, uint32_t moby, const double woff[3], double tr[3]) {
    const int (*PM)[3] = s_view.cam_M;
    double cfx=s_view.cam[0], cfy=s_view.cam[1], cfz=s_view.cam[2];
    int32_t mx=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u),
            my=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
            mz=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    double mfx=(double)mx, mfy=(double)my, mfz=(double)mz;
    int fpos_hit = 0;
    { double mf[3];
      if (openpete_native_interp_pos(moby,     mf)) { mfx=mf[0]; mfy=mf[1]; mfz=mf[2]; fpos_hit=1; } }
    if (!fpos_hit && woff) { mfx += woff[0]; mfy += woff[1]; mfz += woff[2]; }
    /* OPENPETE_REG_INTERP_MISS: regular/specular mobys with NO fractional position
     * (not reached by the spine's lerp walk -> they step at the sim rate while the
     * camera interpolates around them). The shaded twin is
     * OPENPETE_SHADED_INTERP_MISS. Gated on the CAMERA having a fractional
     * position: that is what marks a sub-tick re-extract, and on the canonical
     * pass (where the resolver is inert by design) every moby would "miss".
     * Rate-limited to a dozen lines a second. */
    {
        static int s_rm = -1;
        if (s_rm < 0) { const char* e = getenv("OPENPETE_REG_INTERP_MISS"); s_rm = (e && *e && *e != '0'); }
        if (s_rm && s_view.cam_interp && !fpos_hit) {
            static uint64_t t0 = 0; static int nn = 0;
            uint64_t nowp = openpete_phase_now_us();
            if (nowp - t0 >= 1000000u) { t0 = nowp; nn = 0; }
            if (nn < 12) { nn++;
                fprintf(stderr, "[reg-interp-MISS vbl=%u] moby=%08x cls=%03x pos=(%d,%d,%d)\n",
                        openpete_frame, moby, (unsigned)cpu->read_half(moby + game_layout.moby.class_off), mx, my, mz); }
        }
    }
    double dVy=(cfy-mfy)/4.0, dVz=(cfz-mfz)/4.0, dVx=(mfx-cfx)/4.0;
    for (int i=0;i<3;i++)
        tr[i] = (PM[i][0]*dVy + PM[i][1]*dVz + PM[i][2]*dVx) / 4096.0;
}

/** @brief OPENPETE_MOBY_BUILDDBG: compare each cull record with the host record builder's rebuild from its moby.
 *
 * A report prints at the first VBL at or past 1850 and then at each VBL 1500
 * past the previous report, 12 at most, so the samples span the attract
 * demo's level loads. Over the records build_moby_record_fields rebuilds it
 * counts those whose header, keyframes A and B, and blend byte match exactly
 * and whose matrix and translation lie within 2, prints records 0-5 side by
 * side, and counts the records with no animated model. Counts equal to the
 * built count mean the builder reproduces the cull, the precondition for
 * trusting it on revealed records.
 * @param cpu The guest CPU state, read only. */
static void moby_build_validate(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e=getenv("OPENPETE_MOBY_BUILDDBG"); s_on = (e&&*e&&*e!='0'); }
    if (!s_on) return;
    /* Sample across the demo's level loads (varied class / m_scale / blend) rather
     * than once: print on each crossing of a ~1500-frame grid, up to 12 reports. */
    static unsigned s_next = 1850u; static int s_reports = 0;
    if (openpete_frame < s_next || s_reports >= 12) return;
    s_next = openpete_frame + 1500u; s_reports++;
    int n=0, okHdr=0, okA=0, okB=0, okBl=0, okM=0, okTR=0, noModel=0, worstM=0, worstTR=0;
    for (int r=0; r<s_nrecs; r++) {
        uint32_t rec = s_recs[r].addr; if (rec < 0x80000000u) continue;
        moby_record_t S; if (!moby_game.read_record(cpu, rec, &S)) continue;
        uint32_t moby = S.moby;
        moby_fields f;
        if (!build_moby_record_fields(cpu, moby, &f)) { noModel++; continue; }
        n++;
        if (f.header == S.header)  okHdr++;
        if (f.frameA == S.frame_a) okA++;
        if (f.frameB == S.frame_b) okB++;
        if ((f.w0 & 0xFF00u) == (S.w0 & 0xFF00u)) okBl++;
        int sM[3][3]; memcpy(sM, S.M, sizeof sM);
        int dM=0; for(int i=0;i<3;i++)for(int j=0;j<3;j++){int d=f.M[i][j]-sM[i][j]; if(d<0)d=-d; if(d>dM)dM=d;}
        if (dM<=2) okM++; if (dM>worstM) worstM=dM;
        int sTR[3]={S.TR_raw[0],S.TR_raw[1],S.TR_raw[2]};
        int dT=0; for(int i=0;i<3;i++){int d=f.TR_raw[i]-sTR[i]; if(d<0)d=-d; if(d>dT)dT=d;}
        if (dT<=2) okTR++; if (dT>worstTR) worstTR=dT;
        if (r < 6) fprintf(stderr,
            "  [build r%d moby=%08X] hdr %08X/%08X A %08X/%08X B %08X/%08X bl %04X/%04X dM=%d dTR=%d\n",
            r, moby, f.header, S.header, f.frameA, S.frame_a,
            f.frameB, S.frame_b, (f.w0&0xFF00u), (S.w0&0xFF00u), dM, dT);
    }
    fprintf(stderr,
        "[moby-build vbl=%u] recs=%d built=%d noModel(shaded)=%d | hdr=%d frameA=%d frameB=%d blend=%d "
        "M(<=2)=%d TR(<=2)=%d worst{M=%d TR=%d}  (== built means builder reproduces the .s cull)\n",
        openpete_frame, s_nrecs, n, noModel, okHdr, okA, okB, okBl, okM, okTR, worstM, worstTR);
}

/* Last reveal set - hysteresis stand-in for host-revealed mobys (they never
 * get m_WasDrawn set, so the .s +0x200 range hysteresis needs a host mirror;
 * membership = revealed on the previous builder call). */
static uint32_t s_rev_prev[MAX_RECORDS]; static int s_rev_prev_n;
/** @brief Whether moby m was in the previous builder call's reveal set.
 *
 * @param m The moby's PSX address.
 * @return 1 when s_rev_prev holds m, else 0. */
static int reveal_prev_has(uint32_t m) {
    for (int i = 0; i < s_rev_prev_n; i++) if (s_rev_prev[i] == m) return 1;
    return 0;
}

/** @brief Publish one builder call's admitted set, drawn and occlusion-culled, as the next call's hysteresis set.
 *
 * Both lists hold 0 for a record that was admitted without a lateral verdict
 * (its extent sphere straddled the camera plane, so the window test could not
 * reject it), and those entries are dropped here: the record draws for that
 * present, but publishing it would widen t4e by 0x200 on every later call,
 * which keeps a record the game's distance cull drops drawn inside the
 * canonical window for the rest of the run. A straddle admission re-enters
 * through the window test, or through the game's own queue, on any later tick
 * that gives it a bounded projection. No record address is 0. The set holds
 * at most MAX_RECORDS entries, drawn ones first.
 * @param drawn The admitted mobys that drew.
 * @param drawn_n The length of drawn.
 * @param occl The admitted mobys the occlusion cull dropped.
 * @param occl_n The length of occl. */
static void reveal_publish_hysteresis(const uint32_t* drawn, int drawn_n,
                                      const uint32_t* occl, int occl_n) {
    int hn = 0;
    for (int k = 0; k < drawn_n && hn < MAX_RECORDS; k++) if (drawn[k]) s_rev_prev[hn++] = drawn[k];
    for (int k = 0; k < occl_n  && hn < MAX_RECORDS; k++) if (occl[k])  s_rev_prev[hn++] = occl[k];
    s_rev_prev_n = hn;
}

/** @brief Whether moby m is a run-once instance the walk emits nothing for and the reveal never admits.
 *
 * A run-once instance does a one-shot job on its first update and retires
 * (m_State >= 0x80). The game's update list includes every moby whose
 * m_WasDrawn is set, so canonically such an instance is on screen for the one
 * tick that admits it. The reveal must not write m_WasDrawn, so a revealed
 * instance would be drawn without entering the update list and stand until the
 * canonical draw range reaches it; in range, the canonical queue still flashes
 * it for one tick. Hiding it is display-side only: the cull, m_WasDrawn, the
 * update list and the one-shot job proceed untouched. The identification is
 * the game's (moby_game.run_once_hidden); a game without the slot hides none.
 * @param cpu The guest CPU state, read only.
 * @param m The Moby address.
 * @param cls The moby's class id.
 * @return 1 when the moby is hidden, else 0. */
static int moby_run_once_hidden(CPUState* cpu, uint32_t m, unsigned cls) {
    return moby_game.run_once_hidden ? moby_game.run_once_hidden(cpu, m, cls) : 0;
}

/* Append host-built records for mobys the canonical 4:3 cull skipped but that
 * the extended view or the --no-lod range draws. Dedups against the .s queue
 * set. Extends s_recs[]/s_proj[]; build_walk_tris then walks the union.
 * Inert at 4:3 without --no-lod. */
static uint32_t s_specq_moby[MAX_RECORDS]; static int s_specq_n;   /* last SPEC-pass moby set */
/* Reveal-producer split: the per-moby field rebuild and the model decode. Under
 * extended LOD this loop, not the queue pass, is where moby extraction lives. */
static long s_us_rvdec, s_us_rvfields, s_us_reveal;
static int tier_dbg_on(void);
static int occl_record_culled(CPUState* cpu, uint32_t moby);
static void occl_cache_update(CPUState* cpu, uint32_t moby, const proj_xy* p, int n,
                              const double tr[3], uint32_t header);
/* OPENPETE_MOBY_OCCL_VERIFY bracket for cull sites ahead of the occlusion
 * block: begin() returns 1 when the audit wants the record extracted anyway,
 * end() re-queries the freshly refreshed bounds and reports a wrong verdict. */
static int  occl_verify_begin(uint32_t moby);
static void occl_verify_end(uint32_t moby, const proj_xy* p, int n);
static long s_occl_seen, s_occl_culled;
/** @brief Whether a moby belongs to the specular pass, read off the moby's m_Renderer byte.
 *
 * The renderer-list builder (moby_lists.s func_800521C0) loads the word at
 * m_State and sorts the moby on its high byte, m_Renderer: zero to the shaded
 * list, bit 7 (m_ShinyRenderer) to the specular pass's list, otherwise to the
 * regular list. The bit alone selects the pass; the material word is the
 * treatment's input, not its predicate. The list is not the predicate either:
 * the builder appends a moby only while its sector is in the rendered set
 * (D_800771C8[m_SectorIndex], @8005225C), so membership follows visibility,
 * which a revealed record has lost. A game that names no such bit
 * (renderer_shiny_mask 0) answers 0, and a revealed record then claims no
 * specular draw. A hit counts into the reveal_spec_solo census.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @return 1 when the bit is set, else 0. */
static int moby_is_shiny(CPUState* cpu, uint32_t moby) {
    uint32_t bit = moby_game.renderer_shiny_mask;
    if (!bit) return 0;
    if (!(cpu->read_byte(moby + moby_game.renderer_off) & bit)) return 0;
    NGTE_CENSUS_HIT(reveal_spec_solo);
    return 1;
}
static int append_revealed_regular_inner(CPUState* cpu);
static void moby_fields_pose_from_snapshot(CPUState* cpu, uint32_t moby, moby_fields* f);
/** @brief Run append_revealed_regular_inner, adding its wall time to s_us_reveal while the tier census is on.
 *
 * The timing is taken only under OPENPETE_MOBY_TIER_DBG; otherwise the call is
 * a direct pass-through.
 * @param cpu The guest CPU state.
 * @return The number of records the reveal walk admitted. */
static int append_revealed_regular(CPUState* cpu) {
    if (!tier_dbg_on()) return append_revealed_regular_inner(cpu);
    uint64_t t0 = openpete_phase_now_us();
    int n = append_revealed_regular_inner(cpu);
    s_us_reveal += (long)(openpete_phase_now_us() - t0);
    return n;
}
/** @brief The reveal walk: host-built records for the live mobys the canonical cull did not queue.
 *
 * Those are the mobys at the widescreen edges and in the --no-lod range; each
 * record is decoded and appended to the scene. On a sub-tick present each
 * record takes the pose of the tick the canonical present showed. Returns the
 * records admitted, occlusion-culled ones included. */
static int append_revealed_regular_inner(CPUState* cpu) {
    /* The reveal rebuilds cull records in the game's own encoding, so it runs
     * only where the game's provider supplies that builder. Without it the walk
     * feeds exactly the records the cull queued - which is also what a game
     * whose cull writes a gameplay-readable verdict needs, since nothing the
     * host widens may reach that cull. */
    if (!moby_game.host_reveal) return 0;
    /* Two reveal modes, composable. Admission is deliberately
     * liveness-BLIND: the .s cull never tested liveness, and the canonical
     * PSX draws out-of-update-disk mobys stale-posed all the time (drawing
     * is camera-cull, animation is Spyro's update disk), so a live-only
     * test would drop every static prop, chest and idle enemy at the 4:3
     * boundary.
     *  - EXTENDED VIEW (Hor+ or a wider FOV): off-4:3 mobys the canonical
     *    4:3 X-cone skipped.
     *  - --no-lod (native display policy): mobys BEYOND the canonical draw
     *    range, with no distance ceiling (t4e below). */
    int nl = openpete_no_lod;
    if (!s_view.extended && !nl) return 0;              /* canonical view, LOD faithful - nothing to reveal */
    /* While the rescue cutscene owns the draw it renders its own actor set and
     * no level geometry, so the widened view must add none either. Revealing
     * only the off-window mobys put props in the side strips while the same
     * props inside the 4:3 window were ceded to a queue that does not draw
     * them - present at the edges, absent from the centre of one scene. The
     * cutscene actors are excluded by the same rule: they are drawn by the
     * cutscene renderer, are depth-punched as ACTOR_FRONT, and their radius
     * exceeds their view-Z through most of the cutscene, so a revealed copy
     * straddles the camera plane and has no valid projection. Applies at every
     * LOD - extended LOD reaches the cutscene through this builder too. */
    if (moby_cutscene_owns_draw(cpu)) return 0;
    float xlo = s_view.xlo, xhi = s_view.xhi;
    double ofx = s_view.ofx;
    double H   = s_view.H_eff;
    /* OPENPETE_REVEAL_DBG=frame - reveal-admission forensics: dumps every
     * builder candidate near the window edge (cls/state/rad/rz/sx/bok) on
     * the target frame, so a missing edge moby can be attributed to its
     * skip gate without a rebuild. */
    static unsigned s_rdbg = 0xFFFFFFFFu;
    if (s_rdbg == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_REVEAL_DBG");
                                 s_rdbg = e ? (unsigned)strtoul(e, NULL, 0) : 0xFFFFFFFEu; }
    int rdbg = (openpete_frame >= s_rdbg && openpete_frame <= s_rdbg + 1u);
    static uint32_t revlist[MAX_RECORDS];   /* this call's reveals (hysteresis publish) */
    static float revsx[MAX_RECORDS], revsy[MAX_RECORDS]; static int revnv[MAX_RECORDS];
    static uint32_t occl_rev[MAX_RECORDS]; int occl_rev_n = 0;   /* admitted but occlusion-culled */
    int revealed = 0;
    uint32_t m = moby_array_head(cpu);
    int guard = 0;
    uint32_t trc_m = moby_trace_target();
    #define TRC(fmt, ...) do { if (m == trc_m) fprintf(stderr, \
        "[moby-trace vbl=%u] %08X " fmt "\n", openpete_frame, m, ##__VA_ARGS__); } while (0)
    while (m >= 0x80000000u && guard++ < 4096) {
        uint8_t stt = cpu->read_byte(m + game_layout.moby.state_off);
        if (stt == 0xFF) break;                        /* array sentinel */
        if (stt >= 0x80) { TRC("skip=dead stt=%02X", stt); m += game_layout.moby.array_stride; continue; }      /* dead/inactive */
        if (moby_run_once_hidden(cpu, m, (unsigned)cpu->read_half(m + game_layout.moby.class_off))) {
            TRC("skip=run-once (retires on its first update)");
            m += game_layout.moby.array_stride; continue;
        }
        /* dedup vs every REG-handled moby (s_reg_moby: the .s queue set, plus -
         * on the sub-tick re-extract - occl-culled and hold-replayed mobys, so
         * a held off-4:3 speck is never double-drawn by the reveal) */
        int dup = 0;
        for (int k = 0; k < s_reg_moby_n; k++) if (s_reg_moby[k] == m) { dup = 1; break; }
        if (dup) { TRC("skip=REG-dedup (queued/culled/replayed this pass)"); m += game_layout.moby.array_stride; continue; }
        /* SPECULAR-queue mobys are fed by the SPEC pass (its cone cull is not
         * 4:3 - the native walk draws the whole queue), WITH the per-face
         * specular treatment the REG walk lacks. Revealing them here too drew
         * a second coplanar copy without env/highlight faces that z-fought the
         * SPEC copy.
         * The live +0x1600 queue is CLOBBERED at REG-render time (the scratch
         * is reused mid-frame; the spec cull rebuilds it later), so test the
         * snapshot taken at the last SPEC pass - 1 tick stale at worst. The
         * VERDICT is deferred below to the admit_canon gate: on the tick a
         * spec-queued moby crosses OUT of canonical draw range, the stale
         * snapshot still lists it while this tick's spec queue (canonical
         * cull ⊆ canonical range) does not, so trusting the snapshot there
         * would feed the moby from neither pass for one tick. While the moby
         * is still canonically in range (admit_canon) the snapshot is
         * trusted, which keeps the coplanar-copy protection. */
        int in_spec_snap = 0;
        for (int k = 0; k < s_specq_n; k++) if (s_specq_moby[k] == m) { in_spec_snap = 1; break; }
        moby_fields f;
        int bok;
        { uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          bok = build_moby_record_fields(cpu, m, &f);
          if (t0) s_us_rvfields += (long)(openpete_phase_now_us() - t0); }
        if (rdbg) {
            double dsx = 9999.0;
            if (bok && f.TR_raw[2] > 0) dsx = ofx + (double)f.TR_raw[0] * H / (double)f.TR_raw[2];
            if (!bok || (dsx > -120.0 && dsx < 40.0))
                fprintf(stderr, "[rdbg vbl=%u] m=%08X cls=%04X stt=%02X rad=%02X rz=%d sx=%.0f bok=%d\n",
                        openpete_frame, m, (unsigned)cpu->read_half(m + game_layout.moby.class_off), stt,
                        (unsigned)cpu->read_byte(m + moby_game.render_radius_off), bok ? f.TR_raw[2] : -1, dsx, bok);
        }
        if (!bok) { TRC("skip=no-anim-model"); m += game_layout.moby.array_stride; continue; }  /* shaded/invalid */
        /* The pose the canonical present showed: on a sub-tick present the
         * moby's anim-state bytes are the committed tick's under --runahead,
         * the snapshot's curr the led one (no-op on a canonical render). */
        moby_fields_pose_from_snapshot(cpu, m, &f);
        double rx = (double)f.TR_raw[0], rz = (double)f.TR_raw[2];           /* origin in view space (world/4) */
        /* per-moby radius = AnimationHeader byte[7]<<4 (view units) - the .s
         * cone's X-bias + its behind-camera reject both use it (@8001F2A0) */
        double rr = (double)((int)cpu->read_byte(f.header + game_layout.moby.anim_radius) << 4);
        if (rz + rr <= 0.0) { TRC("skip=behind-cam rz=%.0f rr=%.0f", rz, rr); m += game_layout.moby.array_stride; continue; }  /* behind camera (.s: MAC3 + r <= 0) */
        /* Distance admission = the .s cull's EXACT 3-axis box test (F158
         * @8001F1F8..F260; a radial view-Z test would be strictly narrower
         * and drop far-off-axis mobys the .s draws):
         *   t4 = (m_RenderRadius << 8) + (m_WasDrawn & 1 ? 0x200 : 0)
         *   admit iff every axis: -t4 < (moby[i]-cam[i])>>2 < t4
         * FULL byte (bit7 included, exactly like the lhu path); dist==0
         * needs no special sentinel - its box is degenerate and never
         * admits, same as the .s. Hysteresis: canonical mobys carry live
         * m_WasDrawn; host-revealed mobys never get the byte set, so
         * membership in the LAST reveal set stands in for it (same
         * pop-flicker damping the .s got from the widened render's byte).
         * --no-lod: a byte with nonzero low 7 bits has no ceiling on the
         * effective threshold (t4e below); bit 7 and the zero sentinel keep
         * their canonical meaning. */
        uint8_t rad_byte = cpu->read_byte(m + moby_game.render_radius_off);
        /* Hysteresis, split per consumer. The .s cull widens by 0x200 keyed
         * ONLY on the moby's live m_WasDrawn bit, so the canonical mirror
         * (t4c / admit_canon) must use exactly that - mixing in reveal_prev
         * made "what the .s would admit" depend on what the HOST drew: a
         * boundary moby inside the band (t4, t4+0x200) with m_WasDrawn clear
         * was revealed (hyst 0), which set reveal_prev, which widened t4c,
         * which flipped admit_canon true, which ceded it to a .s queue that
         * never admits it - one dark tick, reveal_prev cleared, repeat: a
         * period-2 strobe at the --lod original cull distance. reveal_prev
         * stands in for the
         * byte only on the reveal's OWN threshold t4e (host-revealed mobys
         * never get m_WasDrawn set; t4e is where the .s's pop-damping needs
         * a mirror - the widescreen edge / --no-lod ceiling). */
        int hyst_canon = (cpu->read_byte(m + game_layout.moby.drawn_off) & 1u) ? 0x200 : 0;
        int hyst = (hyst_canon || reveal_prev_has(m)) ? 0x200 : 0;
        int32_t cx = (int32_t)cpu->read_word(game_layout.cam.position + 0u),
                cy = (int32_t)cpu->read_word(game_layout.cam.position + 4u),
                cz = (int32_t)cpu->read_word(game_layout.cam.position + 8u);
        int32_t d0 = ((int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u) - cx) >> 2,
                d1 = ((int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u) - cy) >> 2,
                d2 = ((int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u) - cz) >> 2;
        int32_t t4c = ((int32_t)rad_byte << 8) + hyst_canon;         /* canonical threshold (.s mirror) */
        /* Effective threshold. Extended LOD means "draw it wherever it is", so
         * the ceiling is REMOVED rather than raised: saturating m_Distance to
         * 0x7F only bought (0x7F << 8) * 4 ~= 130 K world units, which a camera
         * high enough above a level clears outright - every moby then failed the
         * box while terrain (a PVS sector list with no distance term) kept
         * drawing. m_Distance == 0 keeps its canonical meaning: the dormant /
         * unpopulated slot sentinel, whose degenerate box admits nothing. */
        uint32_t eff = rad_byte;
        int32_t t4e;
        if (nl && (eff & 0x7Fu)) t4e = INT32_MAX;                    /* no ceiling */
        else                     t4e = ((int32_t)eff << 8) + hyst;
        #define REVEAL_BOX(t) (d0 > -(t) && d0 < (t) && d1 > -(t) && d1 < (t) && d2 > -(t) && d2 < (t))
        /* the .s pairs the box test with a view-Z cap on the SAME threshold
         * (@8001F2E8: reject MAC3 >= t4) - mirror both or far-axis mobys the
         * .s rejects get over-admitted */
        int admit_canon = REVEAL_BOX(t4c) && rz < (double)t4c;
        TRC("b51=%02X rad=%02X d=(%d,%d,%d) t4c=%d t4e=%d rz=%.0f admit_canon=%d prev=%d",
            (unsigned)cpu->read_byte(m + game_layout.moby.drawn_off), rad_byte, d0, d1, d2, t4c, t4e, rz,
            admit_canon, reveal_prev_has(m));
        if (in_spec_snap && admit_canon) { TRC("skip=SPEC-dedup (in range)"); m += game_layout.moby.array_stride; continue; }
        if (!REVEAL_BOX(t4e) || rz >= (double)t4e) {
            TRC("skip=out-of-range"); m += game_layout.moby.array_stride; continue;
        } /* out of draw range (canonical or nl ceiling) */
        #undef REVEAL_BOX
        if (rz <= 0.0) { TRC("skip=camera-plane"); m += game_layout.moby.array_stride; continue; }    /* camera-plane straddler: no stable projection */
        double sx = ofx + rx * H / rz;                                       /* projected centroid X (m_Scale cancels) */
        double sxc = ofx + rx * s_view.H_canon / rz;                         /* the same at the game's H: the .s queue's territory */
        double margin = reveal_lateral_margin(cpu, f.header, rr, rz, H);
        /* What this record publishes into the hysteresis set if it is admitted:
         * itself, or nothing when only the straddle clause can admit it (an
         * infinite margin is that clause's verdict). */
        uint32_t pub = isinf(margin) ? 0u : m;
        /* central is tested at the CANONICAL projection: the .s cone is wider
         * than the 4:3 screen by the radius bias, so no canonically-central
         * moby is cone-rejected; a moby central only in a wider view can be
         * cone-rejected and must not be ceded to a queue that does not hold
         * it. The window test below uses the effective projection. */
        int central = (sxc >= 0.0 && sxc <= 512.0);
        /* central + canonically-admitted = the .s queue's territory (anything
         * there and not in the queue was cull-rejected for a non-box reason -
         * leave it); central + beyond-canonical is the no-lod clause's NEW
         * coverage. */
        if (central && admit_canon) { TRC("skip=ceded-to-canon sx=%.0f", sx); m += game_layout.moby.array_stride; continue; }
        if (sx < (double)xlo - margin || sx > (double)xhi + margin) { TRC("skip=off-window sx=%.0f", sx); m += game_layout.moby.array_stride; continue; }  /* outside widened view (+radius) */
        /* Occlusion cull, exactly as the queue pass tests its own records: a
         * moby behind nearer world-opaque depth costs a full decode + walk for
         * pixels the depth test erases. Placed after the admission tests above
         * so the verdict below is "admitted, but hidden" - extended LOD removes
         * the distance ceiling entirely, so this admits every moby in a level
         * from a high vantage and occlusion is the only axis left to cut on.
         * A culled reveal still publishes into the hysteresis set: admission
         * stands, only the drawing is skipped, and dropping it from the set
         * would narrow t4e next present and strobe the threshold. */
        int ver_check = 0;
        if (occl_record_culled(cpu, m)) {
            ver_check = occl_verify_begin(m);
            if (!ver_check) {
                s_occl_seen++; s_occl_culled++;
                if (occl_rev_n < MAX_RECORDS) occl_rev[occl_rev_n++] = pub;
                TRC("skip=occluded");
                m += game_layout.moby.array_stride; continue;
            }
        }
        s_occl_seen++;
        if (s_nrecs >= MAX_RECORDS || s_proj_n >= MAX_PROJ) break;
        int off = s_proj_n;
        /* ZSF4 mirrored from the cull's formula (@8001F404) - there is no live
         * record behind a revealed moby to read the verdict from. Validated by
         * the OPENPETE_MOBY_TIER_DBG mirror check on the canonical records.
         * Computed BEFORE the decode: the tier-adjusted vertex-count read
         * ([hdr+8+(ZSF4>>31)], 0 = skip) hides far no-LP-mesh mobys (gems). */
        int rmd   = (int)(cpu->read_byte(m + moby_game.renderer_off) & moby_game.renderer_mask);
        int rzsf4 = (int)(int16_t)(uint16_t)((((uint32_t)rmd << 8)
                    - (uint32_t)f.TR_raw[2]) & 0xFFFFu);
        /* --no-lod: neutralize the distance term for records that are
         * near-band at zero distance */
        if (openpete_no_lod && (rmd << 8) >= 1024) rzsf4 = rmd << 8;
        else if (openpete_no_lod) {
            /* Sub-near (effect-authored fade) records keep the distance term -
             * but the .s holds the verdict in an int16, which WRAPS once view-Z
             * passes 32767. Canonically unreachable (nothing is drawn that far),
             * but reachable because the reveal has no distance ceiling: a wrapped
             * verdict re-enters the 0 < ZSF4 < 1024 fog band and re-fades a moby
             * that is unambiguously far-tier, cycling every 65536 view units.
             * Saturate instead. */
            int v = (rmd << 8) - (int)f.TR_raw[2];
            rzsf4 = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
        }
        int got;
        { uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          pose_hold_begin(m, f.header, f.header_b, f.frameA, f.frameB, f.w0);
          got = moby_decode_project(cpu, f.header, f.header_b, f.frameA, f.frameB, f.w0, NULL, 0.0, -1.0, f.TR_raw, f.TR_frac, f.M,
                                    rzsf4, moby_record_view(m), s_proj + s_proj_n, MAX_PROJ - s_proj_n);
          pose_hold_end(got);
          if (t0) s_us_rvdec += (long)(openpete_phase_now_us() - t0); }
        if (got <= 0) { TRC("skip=decode-0 zsf4=%d", rzsf4); m += game_layout.moby.array_stride; continue; }
        TRC("REVEALED got=%d zsf4=%d", got, rzsf4);
        if (ver_check) occl_verify_end(m, s_proj + off, got);
        else occl_cache_update(cpu, m, s_proj + off, got, f.TR_frac, f.header);
        for (int j = 0; j < got; j++) s_proj[off + j].rec = s_nrecs;
        s_recs[s_nrecs] = (rec_meta){ 0u, off, got, f.header, f.header_b,
            moby_tier_faceblock(cpu, f.header, rzsf4), f.w0,
            f.metal, f.mscol, m, rzsf4,
            atan2f((float)f.TR_raw[2], (float)f.TR_raw[0]), f.frameA, 0u, 0u, 0.f };
        memcpy(s_recs[s_nrecs].M, f.M, sizeof s_recs[s_nrecs].M);
        s_recs[s_nrecs].heat = (uint8_t)moby_heat_scalar(f.mscol);
        /* Sole feeder: the walk applies the SPEC face treatment for this moby,
         * but only for a moby the specular pass itself renders - everything
         * else keeps its baked per-vertex colours. */
        s_recs[s_nrecs].spec_solo = (uint8_t)moby_is_shiny(cpu, m);
        TRC("spec_solo=%d", (int)s_recs[s_nrecs].spec_solo);
        fade_snap_put(cpu, m, rzsf4, f.header);
        if (revealed < MAX_RECORDS) revlist[revealed] = pub;
        /* census diag: projected screen centroid + vert count of this reveal */
        if (revealed < MAX_RECORDS) {
            float cxm = 0.f, cym = 0.f;
            for (int j = 0; j < got; j++) { cxm += s_proj[off + j].sx; cym += s_proj[off + j].sy; }
            revsx[revealed] = got ? cxm / (float)got : -1.f;
            revsy[revealed] = got ? cym / (float)got : -1.f;
            revnv[revealed] = got;
        }
        s_proj_n += got; s_nrecs++; revealed++;
        NGTE_CENSUS_HIT(reveal_admitted);
        m += game_layout.moby.array_stride;
    }
    #undef TRC
    reveal_publish_hysteresis(revlist, revealed < MAX_RECORDS ? revealed : MAX_RECORDS,
                              occl_rev, occl_rev_n);
    static int s_logged = 0;
    if (!s_logged && revealed > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-reveal-feed] frame=%u F=%.3f s=%.3f host-built %d revealed regular mobys "
                "(off-4:3, live) appended to the native scene\n", openpete_frame, s_view.F, s_view.s, revealed);
    }
    /* Admitted, not drawn: a present whose every reveal was occlusion-culled
     * still exercised this producer, and the caller's render-path mark keys off
     * the return - reporting 0 there would retire the sub-tick moby passes for
     * as long as the wall stands in front of them. */
    return revealed + occl_rev_n;
}

/* ---- extended-LOD reveal for a game with its own record encoding ---------
 * The reveal above composes record fields and hands them to the decode, which
 * works only where the fields carry this project's Spyro 1 shapes: a keyframe
 * reference that is a pointer, a translation the draw still has to rescale, a
 * draw radius in the byte moby_game.render_radius_off names. A game whose cull
 * writes another encoding supplies two slots instead - the cull's own box
 * threshold and a record builder for its own queue - and this walk drives
 * them, so the admission is written once.
 *
 * The lateral cull is deliberately not repeated here: the game's own decode
 * runs the cull's cone tests on every record it reads, so a revealed record
 * that falls outside the view is dropped by the game's own predicate rather
 * than by a second approximation of it that could drop a wide model whose
 * centroid sits off the edge.
 *
 * Nothing here writes guest memory: the moby's drawn-verdict byte, which
 * gameplay reads on the next tick, stays the canonical cull's. */
static int moby_verify_enabled(void);

int moby_reveal_anim_fields(CPUState* cpu, uint32_t moby, moby_anim_fields_t* f) {
    const uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model < 0x80000000u || !moby_game.model_is_animated(cpu, model)) return 0;
    uint8_t st[5];
    for (uint32_t k = 0; k < 5u; k++)
        st[k] = cpu->read_byte(moby + game_layout.moby.anim_state_off + k);
    moby_pose_t pose;
    if (!moby_pose_derive(cpu, model, st, &pose)) return 0;
    f->model    = model;
    f->header   = pose.header;
    f->header_b = pose.header_b;
    f->frame_a  = pose.frame_a;
    f->frame_b  = pose.frame_b;
    f->blend    = pose.blend & 0xFFu;
    /* The rotation the cull composes from the moby's three angle bytes,
     * against the camera matrix this pass projects through. The byte-exact
     * table lookup, not the present-interpolated angles: the record this
     * builds is the tick's, and the sub-tick pass rebases it. */
    int PM[3][3]; memcpy(PM, s_view.cam_M, sizeof PM);
    ngte_euler_sc e;
    ngte_euler_sincos(cpu, cpu->read_word(moby + game_layout.moby.angle_off), &e);
    ngte_compose_euler(PM, &e, f->M);
    /* The cull's own centre MVMVA over the quarter-world camera delta: IR1 is
     * the Y delta, IR2 the Z, IR3 the X, MAC = (PM . IR) >> 12. Accumulated at
     * 64 bits, since the products reach 4096*|V| and an int32 sum wraps once a
     * component passes ~175 K view units - reachable under extended LOD,
     * which has no distance ceiling. */
    const int32_t cx = (int32_t)s_view.cam_ram[0],
                  cy = (int32_t)s_view.cam_ram[1],
                  cz = (int32_t)s_view.cam_ram[2];
    const int32_t mx = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u),
                  my = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
                  mz = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    const int32_t Vy = (cy - my) >> 2, Vz = (cz - mz) >> 2, Vx = (mx - cx) >> 2;
    for (int i = 0; i < 3; i++) {
        int64_t acc = ((int64_t)PM[i][0]*Vy + (int64_t)PM[i][1]*Vz
                     + (int64_t)PM[i][2]*Vx) >> 12;
        f->centre[i] = (int32_t)(acc > INT32_MAX ? INT32_MAX
                               : (acc < INT32_MIN ? INT32_MIN : acc));
    }
    return 1;
}

/* The drawn-verdict byte of every moby as the cull read it, before it cleared
 * and rewrote it. The cull's distance box widens by that byte's low two bits,
 * so re-evaluating the cull's admission needs the byte the cull tested, not the
 * verdict it wrote. Indexed by array slot; taken at the cull hook, and kept
 * until the next one, since a regular pass with no cull behind it still shows
 * that cull's verdicts. */
#define PRECULL_SLOTS 4096
static uint8_t  s_precull_drawn[PRECULL_SLOTS];
static uint32_t s_precull_head;
static uint32_t s_precull_n;

/** @brief Save the drawn-verdict byte of every moby in the array, in slot order, before the cull rewrites it.
 *
 * Walks from the array head to the 0xFF state sentinel, at most
 * PRECULL_SLOTS slots. Does nothing, leaving an empty snapshot, for a game
 * without moby_game.cull_view_admits or without a usable array head or
 * stride.
 * @param cpu The guest CPU state, read only. */
static void moby_precull_snapshot(CPUState* cpu) {
    s_precull_n = 0;
    if (!moby_game.cull_view_admits) return;
    const uint32_t stride = game_layout.moby.array_stride;
    s_precull_head = moby_array_head(cpu);
    if (s_precull_head < 0x80000000u || !stride) return;
    uint32_t m = s_precull_head;
    while (s_precull_n < PRECULL_SLOTS &&
           cpu->read_byte(m + game_layout.moby.state_off) != 0xFFu) {
        s_precull_drawn[s_precull_n++] = cpu->read_byte(m + game_layout.moby.drawn_off);
        m += stride;
    }
}

/** @brief The drawn-verdict byte the cull read for moby m, or the live byte when the snapshot does not cover the slot.
 *
 * The snapshot covers m when it was taken from the same array head and m's
 * slot index lies inside it.
 * @param cpu The guest CPU state, read only.
 * @param m The Moby address.
 * @param head The moby array head the caller walks from.
 * @return The drawn-verdict byte. */
static unsigned moby_precull_drawn(CPUState* cpu, uint32_t m, uint32_t head) {
    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t k = stride ? (m - head) / stride : PRECULL_SLOTS;
    if (head == s_precull_head && k < s_precull_n) return s_precull_drawn[k];
    return cpu->read_byte(m + game_layout.moby.drawn_off);
}

/** @brief The widened-view clause of the reveal for one live moby.
 *
 * The cull tests an animated moby against a 4:3 cone, so under a wider
 * presentation view a moby inside its distance box that the lateral arm alone
 * rejected is in the frame but has no record. A moby whose drawn-verdict byte
 * the cull left at 0 is offered to the game's record builder when
 * moby_game.cull_view_admits, the cull's admission re-evaluated with the
 * lateral slope scaled to the view, admits it and the occlusion cull does not
 * reject it. The drawn byte is read, never written.
 * @param cpu The guest CPU state, read only.
 * @param m The Moby address.
 * @param head The moby array head, for the pre-cull drawn byte.
 * @param widen The view's horizontal widening (moby_native_view_widen).
 * @param verdicts Tallies the admission verdict.
 * @param occl Counts an admitted moby the occlusion cull rejected.
 * @return 1 when a record was appended, else 0. */
static int moby_reveal_widened(CPUState* cpu, uint32_t m, uint32_t head, double widen,
                               long verdicts[MOBY_WIDE_VERDICTS], long* occl) {
    if (cpu->read_byte(m + game_layout.moby.drawn_off) != 0u) return 0;
    const moby_wide_verdict_t v = moby_game.cull_view_admits(
        cpu, m, moby_precull_drawn(cpu, m, head), widen);
    verdicts[v]++;
    if (v != MOBY_WIDE_ADMIT) return 0;
    if (occl_record_culled(cpu, m)) { (*occl)++; return 0; }
    moby_reveal_reason_t why = MOBY_REVEAL_OK;
    if (!moby_game.reveal_record(cpu, m, MOBY_REVEAL_ANIMATED, &why)) return 0;
    moby_rec_append_note(m);
    return 1;
}

/* One pass's candidate-walk tallies, printed on its census line. */
typedef struct {
    long seen, canon, occl;
    long declined[MOBY_REVEAL_REASONS];
    long wide_v[MOBY_WIDE_VERDICTS];
    int  added, wide_added;
} moby_reveal_census_t;

/** @brief The extended-LOD clause of the reveal for one live moby.
 *
 * Offers the moby to the game's record builder when the cull's distance box
 * (moby_game.cull_box_threshold with the live drawn byte, over the
 * quarter-world camera delta and the view-space Z) rejected it, it stands in
 * front of the camera plane, and the occlusion cull does not reject it. A
 * degenerate box (threshold <= 0) is a dormant slot and is skipped.
 * @param cpu The guest CPU state, read only.
 * @param m The Moby address.
 * @param kind The model kind whose queue the record joins.
 * @param c Tallies the moby's verdict. */
static void moby_reveal_extended(CPUState* cpu, uint32_t m, moby_reveal_kind_t kind,
                                 moby_reveal_census_t* c) {
    /* The cull's own threshold, read with the moby's live verdict byte:
     * inside the box is the cull's territory, and a moby there that is not
     * in the queue was rejected for a reason the box does not express. */
    const int t = moby_game.cull_box_threshold(cpu, m,
                      cpu->read_byte(m + game_layout.moby.drawn_off));
    if (t <= 0) return;                     /* degenerate box: dormant slot */
    const int (*PM)[3] = s_view.cam_M;
    const int32_t cx = (int32_t)s_view.cam_ram[0],
                  cy = (int32_t)s_view.cam_ram[1],
                  cz = (int32_t)s_view.cam_ram[2];
    const int32_t px = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u),
                  py = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u),
                  pz = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
    const int32_t d0 = (px - cx) >> 2, d1 = (py - cy) >> 2, d2 = (pz - cz) >> 2;
    /* View-space centre through the cull's own axis order: IR1 = the Y
     * delta, IR2 the Z, IR3 the X, MAC = (M . IR) >> 12. */
    const int32_t Vy = -d1, Vz = -d2, Vx = d0;
    int64_t rz = ((int64_t)PM[2][0]*Vy + (int64_t)PM[2][1]*Vz
                + (int64_t)PM[2][2]*Vx) >> 12;
    const int inbox = (d0 > -t && d0 < t && d1 > -t && d1 < t &&
                       d2 > -t && d2 < t && rz < (int64_t)t);
    if (inbox) { c->canon++; return; }
    if (rz <= 0) return;                    /* behind the camera plane */
    if (occl_record_culled(cpu, m)) { c->occl++; return; }
    moby_reveal_reason_t why = MOBY_REVEAL_OK;
    if (moby_game.reveal_record(cpu, m, kind, &why)) { c->added++; moby_rec_append_note(m); }
    else c->declined[why]++;
}

/** @brief Print one [moby-reveal] line with a candidate walk's census to stderr.
 *
 * @param c The walk's tallies.
 * @param kind The model kind the walk covered.
 * @param widen The view widening printed with the widened-view counts. */
static void moby_reveal_census_print(const moby_reveal_census_t* c, moby_reveal_kind_t kind,
                                     double widen) {
    fprintf(stderr, "[moby-reveal vbl=%u] kind=%s live=%ld canon=%ld occluded=%ld added=%d "
                    "declined{nomodel=%ld otherkind=%ld dup=%ld nopose=%ld full=%ld} "
                    "wide{widen=%.4f added=%d outside=%ld sector=%ld otherkind=%ld range=%ld "
                    "near=%ld lateral=%ld vertical=%ld canonical=%ld admit=%ld}\n",
            openpete_frame, (kind == MOBY_REVEAL_ANIMATED) ? "anim" : "simple",
            c->seen, c->canon, c->occl, c->added,
            c->declined[MOBY_REVEAL_NO_MODEL], c->declined[MOBY_REVEAL_OTHER_KIND],
            c->declined[MOBY_REVEAL_DUPLICATE], c->declined[MOBY_REVEAL_NO_POSE],
            c->declined[MOBY_REVEAL_QUEUE_FULL],
            widen, c->wide_added, c->wide_v[MOBY_WIDE_OUTSIDE],
            c->wide_v[MOBY_WIDE_SECTOR], c->wide_v[MOBY_WIDE_OTHER_KIND],
            c->wide_v[MOBY_WIDE_RANGE], c->wide_v[MOBY_WIDE_NEAR],
            c->wide_v[MOBY_WIDE_LATERAL], c->wide_v[MOBY_WIDE_VERTICAL],
            c->wide_v[MOBY_WIDE_CANONICAL], c->wide_v[MOBY_WIDE_ADMIT]);
}

int moby_reveal_candidates(CPUState* cpu, moby_reveal_kind_t kind) {
    if (!moby_game.cull_box_threshold || !moby_game.reveal_record) return 0;
    const double widen = moby_native_view_widen();
    const int wide = kind == MOBY_REVEAL_ANIMATED && moby_game.cull_view_admits && widen > 1.0;
    if (!openpete_no_lod && !wide) return 0; /* the widened-view and extended-LOD clauses only */
    moby_reveal_census_t c;
    memset(&c, 0, sizeof c);
    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t head = moby_array_head(cpu);
    int guard = 0;
    for (uint32_t m = head; m >= 0x80000000u && guard++ < 4096; m += stride) {
        const uint8_t stt = cpu->read_byte(m + game_layout.moby.state_off);
        if (stt == 0xFFu) break;                            /* array sentinel */
        if (stt >= 0x80u) continue;                         /* dead / inactive */
        if (moby_run_once_hidden(cpu, m,
                (unsigned)cpu->read_half(m + game_layout.moby.class_off))) continue;
        c.seen++;
        if (wide && moby_reveal_widened(cpu, m, head, widen, c.wide_v, &c.occl)) {
            c.wide_added++; continue;
        }
        if (openpete_no_lod) moby_reveal_extended(cpu, m, kind, &c);
    }
    if (moby_verify_enabled()) moby_reveal_census_print(&c, kind, wide ? widen : 1.0);
    return c.added + c.wide_added;
}

/* ---- native scene triangle (fed to panel 0) ------------------------------
 * Filled by the independent face-stream walk (below). The walk is
 * jitter-free (exact word0 indices) where a screen-proximity match against
 * the captured prims would jitter. */
typedef struct { float sx, sy, vz; int u, v; uint32_t rgb;
                 float ex, ey, ez; float tvz; } ntri_vert;  /* vz=depth; u,v=texel; ex/ey/ez=env-UV input;
                 tvz = the same vertex's UNSHIFTED view-Z, the perspective W (see
                 openpete_scene_vert.pvz): vz carries the OT-slot pull, which can
                 reach 16256 world units on a saturated authored bias. */
typedef struct {
    ntri_vert v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp;  /* PSX material (VRAM px) */
    int blend;   /* 0 = opaque, 1 = semi-transparent (prim ABE bit) */
    uint8_t tmix;  /* second fogged-CLUT row and weight (moby_texel_fog_mix), 0 = none */
} ntri_t;
#define MAX_NTRI (MAX_PRIMS * 2)              /* GT4 quad -> 2 tris */

/* ---- INDEPENDENT gamestate face-stream walk ------------------------------
 * Derive triangle TOPOLOGY from the moby face stream itself (not the captured
 * prims). Each face's word0 packs three stream-vertex indices (byte offsets
 * into the per-record SXY scratchpad; >>2 = the s_proj index); textured faces
 * bake a Tiledef at fp-0xC/-0x8/-0x4; per-vertex colour comes from the fog
 * scratchpad (g_SonyImage+0x1100), word0-indexed. Connectivity is exact,
 * where a nearest-neighbour match by screen proximity against the
 * captured prims is 55% ambiguous and jitters.
 *
 * The face-record stride is software-pipelined in the .s (2 faces in flight),
 * so the per-shape strides are validated EMPIRICALLY: compare_walk_vs_prims
 * A/Bs the walked triangles vs the captured prims while func_8001F798 is still
 * the oracle -- the match% is the desync detector.
 * word0: sign=fresh(<0)/strip(>=0), &0x1=quad-enable, &0x2=textured,
 * &0x4=sprite-FT4, &0x8=skip. */

#define COLOR_SCRATCH (moby_game.colour_scratch)   /* per-vertex depth-cued RGB the draw wrote */

static ntri_t s_wtri[MAX_NTRI];   /* cursor lives in s_gout.n (see mn_out) */
static uint8_t s_wtri_mode[MAX_NTRI]; /* shaded only: 0=gouraud 1=flat-basic 2=flat-metal */
static uint8_t s_wtri_ui[MAX_NTRI];   /* 1 = this tri's moby is a HUD/UI overlay (draw on top) */
static uint8_t s_wtri_front[MAX_NTRI]; /* 1 = this tri's moby is a dragon-rescue CUTSCENE actor
                                        * (moby_game.cutscene_actor): route to
                                        * OPENPETE_SCENE_ACTOR_FRONT so draw_native_scene front-
                                        * passes ONLY the rescue actors (per-pixel depth punch),
                                        * leaving every other moby's terrain occlusion
                                        * intact - the --lod extended leak fix. */
/* Per-record emit tags. Thread-local because the face walk fans records out
 * across the extraction worker pool: each worker holds the tags of the record
 * it is walking, and without that separation workers stamp their triangles with
 * each other's moby identity, HUD routing and OT bias. */
static _Thread_local int     s_cur_ui;      /* set per-record by the walks before emitting its tris */
static _Thread_local int     s_cur_front;   /* per-record: 1 = a cutscene actor (front-pass) */

/** @brief Whether a moby is in the actor set of the game's cutscene renderer.
 *
 * The actor set is the mobys that renderer draws while it owns the draw.
 * The moby pointer is matched through the game's own selector
 * (moby_game.cutscene_actor), with no class test or heuristic; a game with no
 * such renderer supplies no selector, and every moby is an ordinary actor.
 * @param cpu The guest CPU state, read only.
 * @param mob The moby's PSX address; below KSEG0 answers 0.
 * @return Nonzero for a cutscene actor. */
static int moby_is_cutscene_actor(CPUState* cpu, uint32_t mob) {
    if (mob < 0x80000000u || !moby_game.cutscene_actor) return 0;
    return moby_game.cutscene_actor(cpu, mob);
}

/** @brief Whether the game's cutscene renderer owns the draw.
 *
 * While it does, it renders its own actor set and no level geometry, so the
 * host-side reveal adds nothing either. A game with no such renderer
 * (moby_game.cutscene_owns_draw NULL) answers 0.
 * @param cpu The guest CPU state, read only.
 * @return Nonzero while the cutscene renderer owns the draw. */
static int moby_cutscene_owns_draw(CPUState* cpu) {
    return moby_game.cutscene_owns_draw ? moby_game.cutscene_owns_draw(cpu) : 0;
}

/* Render-time-animated colour cache (the boundary-barrier shimmer). Identity of
 * each emitted walk tri so the canonical frame can snapshot the .s's transient
 * per-vertex colour by FACE (moby pointer + topology vertex-index triple) and the
 * high-FPS sub-tick re-extract can REPLAY it by identity - instead of re-matching
 * by screen position every present. The old screen-space sub-tick match was both
 * O(walk×prim) per present (the 480->60fps moby regression) and flicker-prone
 * (interpolated walk vs the stale canonical prim snapshot crossed the match
 * tolerance frame-to-frame, mis-colouring neighbouring mobys). The face key is
 * pure topology (the SXY index triple comes from the face word, identical
 * canonical vs sub-tick regardless of pose/cull), so replay is exact and stable;
 * colour isn't interpolated between sim ticks, so holding the canonical colour
 * across the intermediates is correct. */
static uint32_t s_wtri_moby[MAX_NTRI]; /* moby ptr that emitted this tri */
static int8_t   s_wtri_dofs[MAX_NTRI]; /* authored OT bias of the emitting moby */
static uint8_t  s_wtri_mangle[MAX_NTRI]; /* any vertex near-fixed-up (unr) or
                                          * coordinate-saturated (sat): the
                                          * SCREEN footprint is not the true
                                          * projection - never a key emitter */
static uint32_t s_wtri_fkey[MAX_NTRI]; /* packed SXY vertex-index triple (face topology id) */
static uint16_t s_wtri_cls[MAX_NTRI];  /* m_Class of the emitting moby (provenance tag) */
/* Which per-face special material produced this tri's colours, 0 = none: the
 * colour A/B prices those faces against the prims separately, because they are
 * the only ones whose colour the walk computes rather than reads. */
static uint8_t  s_wtri_special[MAX_NTRI];
/* The triangle's bucket word in its record's paint order (moby_otk_face_bucket,
 * MOBY_OTK_Q2 on the second triangle of one prim); -1 for a triangle the
 * draw's ordering table does not receive. */
static int16_t  s_wtri_isxy[MAX_NTRI][6];   /* the draw's integer SXY per vertex (calibration) */
static int32_t  s_wtri_otk[MAX_NTRI];
static _Thread_local int32_t s_emit_otk = -1;
static _Thread_local uint32_t s_cur_moby_key;   /* moby for the record currently being walked */
static _Thread_local uint16_t s_cur_moby_cls;   /* m_Class of that moby (set beside the key) */
static _Thread_local int8_t   s_cur_moby_dofs;  /* authored OT bias (set beside the key) */
static _Thread_local float    s_cur_env_az;     /* current record's TR azimuth (env-UV bias) */
static _Thread_local uint32_t s_emit_fkey;      /* packed index triple for the next emit_wtri */
static _Thread_local uint8_t  s_emit_special;   /* special-material id for the next emit_wtri */
static _Thread_local uint8_t  s_emit_tmix;      /* texel fog-row mix for the next emit_wtri */
#define PACK_FKEY(a,b,c) (((uint32_t)(a)&0x3FFu) | (((uint32_t)(b)&0x3FFu)<<10) | (((uint32_t)(c)&0x3FFu)<<20))

/** @brief The signed doubled area of a screen-space triangle, the GTE NCLIP analog on float coordinates.
 *
 * (b - a) x (c - a), the sign convention NCLIP uses; the caller reads the
 * sign as the facing.
 * @param ax Vertex a's screen X.
 * @param ay Vertex a's screen Y.
 * @param bx Vertex b's screen X.
 * @param by Vertex b's screen Y.
 * @param cx Vertex c's screen X.
 * @param cy Vertex c's screen Y.
 * @return The signed doubled area. */
static double tri_nclip(float ax,float ay,float bx,float by,float cx,float cy) {
    return (double)(bx-ax)*(cy-ay) - (double)(cx-ax)*(by-ay);
}

/* The four decode pieces the player feeder shares with this walk; see
 * renderer/moby.h for what each one is. */
double moby_tri_nclip(float ax, float ay, float bx, float by, float cx, float cy) {
    return tri_nclip(ax, ay, bx, by, cx, cy);
}

/** @brief Latch one ordering-bias mechanism's switch: off when the master or the mechanism's own variable starts with '0'.
 *
 * An unset or empty variable reads as on. The master OPENPETE_MOBY_OTBIAS is
 * latched once for every mechanism; `slot` latches the combined verdict.
 * @param slot The mechanism's latch, seeded to -1; written on the first call.
 * @param var The mechanism's own environment variable.
 * @return 1 when the mechanism applies, else 0. */
static int moby_otbias_latch(int* slot, const char* var) {
    static int s_master = -1;
    if (*slot < 0) {
        if (s_master < 0) {
            const char* m = getenv("OPENPETE_MOBY_OTBIAS");
            s_master = !(m && *m == '0');
        }
        const char* e = getenv(var);
        *slot = s_master && !(e && *e == '0');
    }
    return *slot;
}

int moby_tie_pull_on(void) {
    static int s_on = -1;
    return moby_otbias_latch(&s_on, "OPENPETE_MOBY_TIE_PULL");
}

int moby_face_key_on(void) {
    static int s_on = -1;
    return moby_otbias_latch(&s_on, "OPENPETE_MOBY_FACE_KEY");
}

int moby_player_sort_pull_on(void) {
    static int s_on = -1;
    return moby_otbias_latch(&s_on, "OPENPETE_PLAYER_SORT_PULL");
}

int moby_abr_blend(int abr) {
    return ngte_abr_blend(abr, NGTE_ABR_EXACT);
}

void moby_tiledef_decode(uint32_t td0, uint32_t td1, uint32_t td2, moby_wface_t* F) {
    F->u[0] = (uint8_t)(td0 & 0xFFu); F->v[0] = (uint8_t)((td0 >> 8) & 0xFFu);
    F->u[1] = (uint8_t)(td1 & 0xFFu); F->v[1] = (uint8_t)((td1 >> 8) & 0xFFu);
    F->u[2] = (uint8_t)(td2 & 0xFFu); F->v[2] = (uint8_t)((td2 >> 8) & 0xFFu);
    F->u[3] = (uint8_t)((td2 >> 16) & 0xFFu); F->v[3] = (uint8_t)((td2 >> 24) & 0xFFu);
    unsigned cl = (td0 >> 16) & 0xFFFFu;
    unsigned tp = (td1 >> 16) & 0xFFFFu;
    ngte_tpage t;
    ngte_decode_tpage(tp, cl, NGTE_TP_REPLACED_ID, &t);
    /* NGTE_TP_REPLACED_ID: the hardware reserves colour depth 3, so the emitter
     * spends it on naming a replaced texture whose id rides in the CLUT half
     * and reaches the walk as tpx with bpp 32. */
    F->abr = (uint8_t)t.abr;
    F->tpx = (int16_t)t.tpx;
    F->tpy = (int16_t)t.tpy;
    F->bpp = (uint8_t)t.bpp;
    F->clx = (int16_t)t.clx;
    F->cly = (int16_t)t.cly;
}

int moby_face_emit(int fresh, int bit0, int bit0_unconditional,
                   double ncl1, double ncl2, int have_v3, int order[2][3]) {
    if (!fresh) {
        int keep = bit0 ? (bit0_unconditional ? 1 : (ncl1 != 0.0)) : (ncl1 > 0.0);
        if (!keep) return 0;
        order[0][0] = 0; order[0][1] = 1; order[0][2] = 2;
        return 1;
    }
    if (bit0 && bit0_unconditional) {          /* pre-marked quad: no test at all */
        if (!have_v3) { order[0][0]=0; order[0][1]=1; order[0][2]=2; return 1; }
        order[0][0]=0; order[0][1]=1; order[0][2]=2;
        order[1][0]=1; order[1][1]=3; order[1][2]=2;
        return 2;
    }
    double t6a = (bit0 && ncl1 > 0.0) ? -ncl1 : ncl1;
    double t6b = (bit0 && ncl2 < 0.0) ? -ncl2 : ncl2;
    if (t6a >= 0.0) {
        if (t6b > 0.0 && have_v3) { order[0][0]=3; order[0][1]=1; order[0][2]=2; return 1; }
        return 0;
    }
    if (t6b > 0.0 && have_v3) {
        order[0][0]=0; order[0][1]=1; order[0][2]=2;
        order[1][0]=1; order[1][1]=3; order[1][2]=2;
        return 2;
    }
    order[0][0]=0; order[0][1]=1; order[0][2]=2;
    return 1;
}

#define MN_MAX_WORKERS 8
#define MN_FANOUT_MIN_FACES 400   /* see walk_prologue: dispatch break-even */
/* Walk diagnostics (printed by compare_walk_vs_prims), one per worker. Aligned
 * to a cache line: workers bump their own counters throughout the walk, and
 * packed side by side every increment would bounce the line between cores. */
typedef struct {
    _Alignas(64) int faces;
    int emitted, oor, skip, sprite, strip, fresh_g, fresh_t, quad, nearstraddle;
    int align_lo, align_hi, desync;
    int coinc_pairs, coinc_pulls;   /* moby_coincident_pull: matches, faces pulled */
} mn_walkdiag;
/* Slot 0 is the serial walk's, and the slot every parallel run sums into. */
static mn_walkdiag s_wdiag[MN_MAX_WORKERS];
static _Thread_local mn_walkdiag* t_diag = &s_wdiag[0];

/* Where emitted triangles land: the global s_wtri* arrays for a serial walk, a
 * per-worker slab for a parallel one. Cursor and capacity travel with the
 * arrays so emit_wtri needs no knowledge of which it is filling. */
typedef struct {
    ntri_t*   tri;  uint32_t* fkey; uint32_t* moby; uint16_t* cls;
    uint8_t*  ui;   uint8_t*  front; uint8_t* mangle;
    int8_t*   dofs; uint8_t*  special;
    int32_t*  otk;  int16_t (*isxy)[6];
    int n, cap;
} mn_out;
static mn_out s_gout = { s_wtri, s_wtri_fkey, s_wtri_moby, s_wtri_cls, s_wtri_ui,
                         s_wtri_front, s_wtri_mangle, s_wtri_dofs, s_wtri_special,
                         s_wtri_otk, s_wtri_isxy, 0, MAX_NTRI };
static _Thread_local mn_out* t_out = &s_gout;

/** @brief Append one float triangle to the current emit target, unless the near-plane cull drops it.
 *
 * The target is the thread's t_out: the global s_gout arrays for a serial walk
 * or a worker slab for a parallel one; a full target drops the call. A triangle
 * with any vertex at view-Z <= 0 is dropped, since the float projection divides
 * by the raw Z and a vertex behind the camera would invert. A triangle with a
 * vertex flagged unr (in the GTE divide-overflow regime, its screen position
 * pinned by moby_gte_near_fixup) is dropped when its screen box exceeds 1023 x
 * 511 px, the class the GPU discards; that drop counts into nearstraddle.
 *
 * A kept triangle stores its per-triangle side data from the walk's
 * thread-local state (s_cur_ui, s_cur_front, s_cur_moby_key, s_cur_moby_dofs,
 * s_cur_moby_cls, s_emit_fkey, s_emit_special, s_emit_otk, s_emit_tmix), a
 * mangle flag when any vertex is unr or sat, and the integer SXY of each
 * vertex. It then resets s_emit_otk to -1 and s_emit_tmix to 0; a dropped
 * triangle leaves both unchanged.
 * @param a The first projected vertex.
 * @param b The second projected vertex.
 * @param c The third projected vertex.
 * @param ca The first vertex colour word.
 * @param cb The second vertex colour word.
 * @param cc The third vertex colour word.
 * @param tex Nonzero for a textured triangle.
 * @param tpx The texture page X origin.
 * @param tpy The texture page Y origin.
 * @param clx The CLUT X origin.
 * @param cly The CLUT Y origin.
 * @param bpp The texel depth in bits.
 * @param blend The blend code, 0 for opaque, else the moby_abr_blend result of
 *              a semi-transparent face. */
static void emit_wtri(proj_xy* a, proj_xy* b, proj_xy* c,
                      uint32_t ca, uint32_t cb, uint32_t cc,
                      int ua,int va,int ub,int vb,int uc,int vc,
                      int tex,int tpx,int tpy,int clx,int cly,int bpp,int blend) {
    mn_out* O = t_out;
    if (O->n >= O->cap) return;
    /* NEAR-PLANE CULL: drop any triangle with a vertex at/behind the camera
     * (view-Z <= 0). The .s relies on the GTE perspective divide + NCLIP to
     * reject behind-camera prims; our float project() does a raw H/Z, so a
     * negative Z flips the vertex (sign inverts both screen axes) and hurls it
     * to the top of the view. Without this, a moby that passes behind the camera
     * - e.g. the gem value-popup the player charges THROUGH - renders inverted
     * ("upside down") at screen top. Matches the .s (it draws nothing for a
     * fully-behind moby). Cheap whole-triangle cull (no near-plane clipping):
     * mobys rarely straddle the near plane, and a 1-frame early disappear beats
     * a flipped explosion. */
    if (a->vz <= 0.0f || b->vz <= 0.0f || c->vz <= 0.0f) return;
    /* NEAR-PLANE STRADDLE: a face with a vertex inside the GTE divide-overflow
     * regime got that vertex's screen position from the hardware-clamped path
     * (moby_gte_near_fixup), so it is pinned to the ±1024 SXY window rather
     * than the true perspective point - the face's shape is no longer the one
     * the camera sees. The GPU is what resolves this on hardware: it discards
     * any primitive spanning more than 1023 px horizontally or 511 vertically,
     * which is exactly the class the clamp produces. Mirror that and the
     * straddling face drops, as it does on PSX; without it the face survives
     * with one corner slammed to the window edge and sweeps the frame (mobys
     * the camera passes through - the 1-frame whole-screen flash).
     *
     * Gated on the flag so ordinary geometry is untouched: a legitimately huge
     * near face (or a wide face under Hor+ reveal, whose true X exceeds the
     * PSX screen by design) never enters the regime and never sees this test. */
    if (a->unr || b->unr || c->unr) {
        float xlo = a->sx, xhi = a->sx, ylo = a->sy, yhi = a->sy;
        if (b->sx < xlo) xlo = b->sx; if (b->sx > xhi) xhi = b->sx;
        if (c->sx < xlo) xlo = c->sx; if (c->sx > xhi) xhi = c->sx;
        if (b->sy < ylo) ylo = b->sy; if (b->sy > yhi) yhi = b->sy;
        if (c->sy < ylo) ylo = c->sy; if (c->sy > yhi) yhi = c->sy;
        if (xhi - xlo > 1023.0f || yhi - ylo > 511.0f) { t_diag->nearstraddle++; return; }
    }
    O->ui[O->n] = (uint8_t)s_cur_ui;       /* HUD/UI tag for this tri (current record) */
    O->front[O->n] = (uint8_t)s_cur_front;    /* cutscene actor (front-pass eligible) */
    O->moby[O->n] = s_cur_moby_key;           /* face identity: moby address */
    O->dofs[O->n] = s_cur_moby_dofs;
    O->mangle[O->n] = (uint8_t)((a->unr || b->unr || c->unr ||
                                 a->sat || b->sat || c->sat) ? 1 : 0);
    O->fkey[O->n] = s_emit_fkey;
    O->special[O->n] = s_emit_special;
    O->otk[O->n] = s_emit_otk;
    O->isxy[O->n][0] = a->iv.sx; O->isxy[O->n][1] = a->iv.sy;
    O->isxy[O->n][2] = b->iv.sx; O->isxy[O->n][3] = b->iv.sy;
    O->isxy[O->n][4] = c->iv.sx; O->isxy[O->n][5] = c->iv.sy;
    s_emit_otk = -1;
    O->cls[O->n] = s_cur_moby_cls;            /* provenance tag for mod shaders */
    ntri_t* t = &O->tri[O->n++];
    t->v[0]=(ntri_vert){a->sx,a->sy,a->vz,ua,va,ca, a->ex,a->ey,a->ez, a->tvz};
    t->v[1]=(ntri_vert){b->sx,b->sy,b->vz,ub,vb,cb, b->ex,b->ey,b->ez, b->tvz};
    t->v[2]=(ntri_vert){c->sx,c->sy,c->vz,uc,vc,cc, c->ex,c->ey,c->ez, c->tvz};
    t->textured=tex;t->tpage_x=tpx;t->tpage_y=tpy;t->clut_x=clx;t->clut_y=cly;t->bpp=bpp;t->blend=blend;
    t->tmix = s_emit_tmix;
    s_emit_tmix = 0;
    t_diag->emitted++;
}

/* ---- second model kind: the shared half of moby_game.simple_pass ---------
 * The provider owns the second kind's record, header, vertex and face
 * encodings and its model->view transform, because none of them is shared;
 * everything downstream of a view-space triangle is. The provider names the
 * record's moby once, then hands each triangle in view space, full world
 * units, and these two apply the same per-record tagging, the same raw float
 * projection, the same GTE near-plane mirror and the same emit the animated
 * walk uses, so both kinds reach the scene in one category and one depth
 * space. */
static const int MOBY_IDENT_Q12[3][3] = { {4096,0,0}, {0,4096,0}, {0,0,4096} };

/* The projection origin of the record being decoded, and the view it projects
 * through. A record whose model is placed in screen space names its own origin
 * in PSX pixels; every other record takes the origin of its own view, which is
 * what these two read before this existed. */
static int    s_cur_screen;
static double s_cur_screen_ofx, s_cur_screen_ofy;

/** @brief The view the current second-kind record projects through, with its projection origin.
 *
 * @param ofx Receives the origin X in PSX pixels: the record's own screen
 *        origin when it names one, else the view's.
 * @param ofy Receives the origin Y likewise.
 * @return The canonical view for a record that names a screen origin, else
 *         the view moby_record_view gives the current moby. */
static const ngte_view* moby_simple_view(double* ofx, double* ofy) {
    const ngte_view* view = s_cur_screen ? &s_view_canon
                                         : moby_record_view(s_cur_moby_key);
    *ofx = s_cur_screen ? s_cur_screen_ofx : view->ofx;
    *ofy = s_cur_screen ? s_cur_screen_ofy : view->ofy;
    return view;
}

void moby_simple_record(CPUState* cpu, uint32_t moby) {
    s_cur_moby_key  = moby;
    s_cur_moby_cls  = moby ? (uint16_t)cpu->read_half(moby + game_layout.moby.class_off) : 0;
    s_cur_moby_dofs = moby ? (int8_t)cpu->read_byte(moby + game_layout.moby.angle_off + 3u) : 0;
    s_cur_ui    = moby_hud_tag(moby);
    s_cur_front = s_cur_ui == 0 && moby_is_cutscene_actor(cpu, moby);
    s_cur_screen = 0;
    s_emit_fkey = 0u;
    s_emit_special = 0u;
}

void moby_simple_screen_origin(double ofx, double ofy) {
    s_cur_screen = 1;
    s_cur_screen_ofx = ofx;
    s_cur_screen_ofy = ofy;
    /* The record is a screen-anchored 3D model, the class the HUD3D category
     * carries: its own faces Z-sort against each other and the whole record
     * paints over the world. The canonical view keeps the game's projection
     * distance, so the origin the record names is the pixel it means. */
    s_cur_ui = 2;
    s_cur_front = 0;
}

void moby_simple_project(const double p[3], float* sx, float* sy) {
    double ofx, ofy;
    const ngte_view* view = moby_simple_view(&ofx, &ofy);
    double x, y; int sat;
    double vzz = project(MOBY_IDENT_Q12, NULL, p[0], p[1], p[2],
                         ofx, ofy, view, &x, &y, &sat);
    moby_gte_near_fixup(MOBY_IDENT_Q12, NULL, p[0], p[1], p[2],
                        vzz, ofx, ofy, view, &x, &y);
    *sx = (float)x; *sy = (float)y;
}

void moby_simple_otk_record(moby_otk_row_t row, int trz, moby_otk_rec_t* out) {
    moby_otk_record(moby_otkey_wanted() ? row : MOBY_OTK_NONE, 0, trz, out);
}

void moby_simple_otk_vertex(const moby_otk_rec_t* r, const int tr[3], const int R[3][3],
                            double vx, double vy, double vz, moby_otk_vert_t* out) {
    if (r->row == MOBY_OTK_NONE) return;
    double ofx, ofy;
    const ngte_view* view = moby_simple_view(&ofx, &ofy);
    moby_otk_rtps(tr, R, vx, vy, vz, ofx, ofy, (int)view->H_canon, out);
}

void moby_simple_tri(const double p[3][3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey, int32_t otk, const moby_otk_vert_t* const iv[3]) {
    double ofx, ofy;
    const ngte_view* view = moby_simple_view(&ofx, &ofy);
    proj_xy q[3];
    for (int k = 0; k < 3; k++) {
        double sx, sy; int sat;
        /* The point is already in view space, so the rotate step is identity
         * and the translation zero; only the divide is left to do. */
        double vzz = project(MOBY_IDENT_Q12, NULL, p[k][0], p[k][1], p[k][2],
                             ofx, ofy, view, &sx, &sy, &sat);
        int unr = moby_gte_near_fixup(MOBY_IDENT_Q12, NULL, p[k][0], p[k][1], p[k][2],
                                      vzz, ofx, ofy, view, &sx, &sy);
        q[k].sx = (float)sx; q[k].sy = (float)sy;
        q[k].vz = (float)vzz; q[k].tvz = (float)vzz;
        q[k].sat = sat; q[k].unr = unr; q[k].rec = -1;
        q[k].ex = q[k].ey = q[k].ez = 0.0f;
        if (iv) q[k].iv = *iv[k];
    }
    s_emit_fkey = fkey;
    s_emit_special = 0u;
    s_emit_otk = otk;
    emit_wtri(&q[0], &q[1], &q[2], rgb[0], rgb[1], rgb[2],
              u[0], v[0], u[1], v[1], u[2], v[2],
              textured, tpage_x, tpage_y, clut_x, clut_y, bpp, blend);
}

/* specular env-reflection UV -- definitions below (near recolor); forward-
 * declared so the walk can derive metal-face UVs natively. s_env_* = the static
 * metal Tiledef (D_800756F0, level-load asset), decoded to VRAM px by _load. */
static void specular_env_load(CPUState* cpu);
static void specular_env_uv(float ex, float ey, float ez, int* pu, int* pv);
static int  s_env_tpx, s_env_tpy, s_env_clx, s_env_cly, s_env_bpp;
static float s_specL[3], s_specLlen;   /* D_800770C8+0xC specular light dir (highlight) */

/** @brief GTE DPCS depth cue of one 8-bit colour channel toward a 12-bit far colour by IR0/4096.
 *
 * Delegates to ngte_dpcs_ch, the bit-exact mirror of the GTE's DPCS depth
 * cue at sf = 1 (gte_depth_cue in core/psx/gte.c):
 *   mac    = c<<16
 *   ir_d   = sat16(((fc<<12) - mac) >> 12)
 *   total  = mac + IR0*ir_d
 *   out    = sat8((total>>12) >> 4)
 * dpcs_ch(255, 2032, 1312) is 214, as on the live GTE.
 * @param c8 The channel.
 * @param fc12 The far colour's channel, 12-bit.
 * @param ir0 The GTE IR0.
 * @return The faded channel, 0..255. */
static int dpcs_ch(int c8, int fc12, int ir0) {
    return ngte_dpcs_ch(c8, fc12, ir0);   /* shared bit-exact GTE mirror */
}

/** @brief A model's far palette: the header's far-colour pointer, or 0 when the header or the pointer is below KSEG0.
 *
 * The pointer is ANIM_COL_FAR, header + 0x20 in the .s lw $v0, 0x20($ra)
 * @8001FDF4.
 * @param cpu The guest CPU state, read only.
 * @param header The AnimationHeader.
 * @return The far palette's address, or 0. */
static uint32_t moby_fog_farbase(CPUState* cpu, uint32_t header) {
    if (header < 0x80000000u) return 0u;
    uint32_t fb = cpu->read_word(header + ANIM_COL_FAR);
    return fb >= 0x80000000u ? fb : 0u;
}

/** @brief The fog-band colour fade of a record: IR0 toward the far palette, 0 outside the band.
 *
 * The cull record carries the per-record LOD tier ZSF4 in the high 16 bits
 * of its matrix word at +0x2C (cfc2 gte_ctrl[30]); the ladder is documented
 * at moby_tier_faceblock. This handles the middle band (.s moby colour pass,
 * block_8001FDD0/.L8001FDE8): 0 < ZSF4 < 1024 fades toward the far palette
 * by IR0 = (1024 - ZSF4) * 4. ZSF4 >= 1024 (near, raw colour) and ZSF4 <= 0
 * (the far palette itself and the cyclorama fog fade) are moby_record_fade's.
 * Effect mobys (the ring puffs, cls 0x77) drop ZSF4 from 1024 to 0 over their
 * last ticks and fade out through this band. A header with no far palette
 * yields no fade.
 * @param cpu The guest CPU state, read only.
 * @param zsf4 The record's ZSF4: the cull record's for a canonical record, the snapshot's for a sub-tick one.
 * @param header The AnimationHeader.
 * @param farbase Receives the far palette, 0 when there is no fade.
 * @return IR0, 0 for no fade. */
static int moby_color_fade_ir0(CPUState* cpu, int zsf4, uint32_t header, uint32_t* farbase) {
    *farbase = 0;
    if (zsf4 <= 0 || zsf4 >= 1024) return 0;
    uint32_t fb = moby_fog_farbase(cpu, header);
    if (!fb) return 0;
    *farbase = fb;
    return (1024 - zsf4) * 4;
}

/* A record's colour fade on one present. frac 0: ir0 is the GTE's integer IR0
 * and every channel goes through the bit-exact DPCS mirror. frac 1 (a sub-tick
 * present whose ZSF4 is fractional): ir0f is IR0 at full precision and the
 * channel is the DPCS lerp at that IR0 (dpcs_ch_f); ir0 is its floor.
 * row is the fogged-CLUT row the textured faces sample (IR0 >> 9 on the
 * integer path) and tmix the second row and its weight, packed as a textured
 * scene vertex's alpha byte (moby_texel_fog_mix); 0 = the one row alone. */
typedef struct { int ir0; double ir0f; int frac; int row; uint8_t tmix; } moby_fade_t;

/** @brief DPCS of one 8-bit channel toward a 12-bit far colour at a fractional IR0.
 *
 * floor((c<<16 + IR0 * (fc - (c<<4))) / 65536), clamped to 0..255, the GTE's own truncation
 * (ngte_dpcs_ch) extended to real IR0. At an integer IR0 the numerator is an
 * exact integer in double and the division by 2^16 is exact, so the result
 * equals the integer mirror; between integers it is continuous from the left
 * and moves by at most one level per IR0 step the mirror takes. The fade then
 * meets the canonical present at both ends of a tick interval, where
 * round-to-nearest would stand up to one level off the GTE's value.
 * @param c8 The channel.
 * @param fc12 The far colour's channel, 12-bit.
 * @param ir0 IR0 at full precision.
 * @return The faded channel, 0..255. */
static int dpcs_ch_f(int c8, int fc12, double ir0) {
    double v = ((double)c8 * 65536.0 + ir0 * (double)(fc12 - (c8 << 4))) / 65536.0;
    int o = (int)floor(v);
    return o < 0 ? 0 : (o > 255 ? 255 : o);
}

/** @brief One packed 0x00BBGGRR colour faded toward a 12-bit far colour by the record's fade.
 *
 * An integer fade runs the bit-exact DPCS mirror (dpcs_ch) at fd->ir0; a
 * fractional one runs dpcs_ch_f at fd->ir0f.
 * @param col The colour.
 * @param fcr The far colour's red, 12-bit.
 * @param fcg The far colour's green, 12-bit.
 * @param fcb The far colour's blue, 12-bit.
 * @param fd The record's fade.
 * @return The faded colour. */
static uint32_t moby_fade_rgb(uint32_t col, int fcr, int fcg, int fcb, const moby_fade_t* fd) {
    int r = (int)(col & 0xFFu), g = (int)((col >> 8) & 0xFFu), b = (int)((col >> 16) & 0xFFu);
    if (fd->frac) {
        r = dpcs_ch_f(r, fcr, fd->ir0f); g = dpcs_ch_f(g, fcg, fd->ir0f); b = dpcs_ch_f(b, fcb, fd->ir0f);
    } else {
        r = dpcs_ch(r, fcr, fd->ir0); g = dpcs_ch(g, fcg, fd->ir0); b = dpcs_ch(b, fcb, fd->ir0);
    }
    return (uint32_t)(r | (g << 8) | (b << 16));
}

/** @brief Whether a fade leaves every channel at its own colour.
 *
 * @param fd The record's fade.
 * @return Nonzero for IR0 0 (ir0f <= 0 on a fractional fade). */
static int moby_fade_none(const moby_fade_t* fd) {
    return fd->frac ? fd->ir0f <= 0.0 : !fd->ir0;
}

/** @brief A per-vertex moby colour from a colour table, faded by the record's DPCS fade when it has one.
 *
 * The colour is the low 24 bits of the word at cbase + ci. The far colour is
 * the word at farbase + ci with each of its three low bytes expanded to 12
 * bits (the fog band, .s 8001FE14-FE2C), or the constant fcc (the LP band
 * fades every vertex toward the one cyclorama fog colour, .s 8001FEE4-FF08).
 * @param cpu The guest CPU state, read only.
 * @param cbase The colour table.
 * @param farbase The far palette, read only when fcc is NULL and the fade is not none.
 * @param fcc The constant 12-bit far colour, or NULL.
 * @param fd The record's fade.
 * @param ci The colour's byte offset into both tables.
 * @return The colour, 0x00BBGGRR. */
static uint32_t moby_color_at(CPUState* cpu, uint32_t cbase, uint32_t farbase,
                              const int* fcc, const moby_fade_t* fd, uint32_t ci) {
    uint32_t col = cpu->read_word(cbase + ci) & 0xFFFFFFu;
    if (moby_fade_none(fd)) return col;
    if (fcc) return moby_fade_rgb(col, fcc[0], fcc[1], fcc[2], fd);
    uint32_t fw = cpu->read_word(farbase + ci);
    return moby_fade_rgb(col, (int)((fw << 4) & 0xFF0u), (int)((fw >> 4) & 0xFF0u),
                         (int)((fw >> 12) & 0xFF0u), fd);
}

/** @brief Load a little-endian 32-bit word from host memory.
 *
 * PSX RAM and the host are both little-endian; memcpy keeps the load
 * alignment-safe.
 * @param p The word's host address.
 * @return The word. */
static inline uint32_t le32_at(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
/** @brief moby_color_at on host pointers: the colour tables resolved to host memory once per record.
 *
 * Same arithmetic as moby_color_at without a cpu->read_word memory-map
 * dispatch per vertex (3-4 colour lookups per face, the walk's hottest reads
 * after the Tier-1 face cache). The walk calls moby_color_at when a base does
 * not resolve.
 * @param cb The colour table's host address.
 * @param fbp The far palette's host address, read only when fcc is NULL and the fade is not none.
 * @param fcc The constant 12-bit far colour, or NULL.
 * @param fd The record's fade.
 * @param ci The colour's byte offset into both tables.
 * @return The colour, 0x00BBGGRR. */
static uint32_t moby_color_at_host(const uint8_t* cb, const uint8_t* fbp,
                                   const int* fcc, const moby_fade_t* fd, uint32_t ci) {
    uint32_t col = le32_at(cb + ci) & 0xFFFFFFu;
    if (moby_fade_none(fd)) return col;
    if (fcc) return moby_fade_rgb(col, fcc[0], fcc[1], fcc[2], fd);
    uint32_t fw = le32_at(fbp + ci);
    return moby_fade_rgb(col, (int)((fw << 4) & 0xFF0u), (int)((fw >> 4) & 0xFF0u),
                         (int)((fw >> 12) & 0xFF0u), fd);
}

/** @brief The fog band's IR0 at a ZSF4, (1024 - ZSF4) * 4 clamped to the band's CLUT-row range 0..4092.
 *
 * @param zsf4 The ZSF4.
 * @return The IR0. */
static int moby_fog_band_ir0(int zsf4) {
    int v = (1024 - zsf4) * 4;
    return v < 0 ? 0 : (v > 4092 ? 4092 : v);
}

/** @brief Set the fogged-CLUT row and second-row mix a fractional fog-band present samples.
 *
 * The draw biases
 * a textured face's CLUT row by IR0 >> 9 (.s @8001FE04-FE08), so the texel side
 * of the fade steps once per 512 IR0 while the vertex side (dpcs_ch_f) moves
 * continuously. The present blends the rows of its two canonical ends instead:
 * t = (IR0 - IR0_curr) / (IR0_prev - IR0_curr), clamped to 0..1, weights the
 * previous tick's row, IR0_prev from the previous tick's integer ZSF4 when the
 * snapshot holds its full-precision centre (the entry fade_snap_present_z
 * lerps from), IR0_curr = this tick's. The nearer end is the base row (the
 * face's CLUT y) and the other rides the vertex alpha byte as
 * ((other - base + 7) << 4) | round(30 w), w = min(t, 1 - t) in 0..0.5; the
 * scene fragment stages decode it (psx_scene.frag). Both canonical ends read
 * one row: t 0 is the tick's own row and t 1 the previous tick's. When both
 * ends select the same row, or the previous tick has no snapshot entry with
 * a full-precision centre, the tick's row stands alone.
 * @param R The record.
 * @param fd The record's fade: ir0f in, row and tmix out. */
static void moby_texel_fog_mix(const rec_meta* R, moby_fade_t* fd) {
    const int ir0c = moby_fog_band_ir0(R->zsf4);
    const fade_ent* p = fade_snap_find(s_fade_prev, s_fade_prev_n, R->moby);
    const int ir0p = (p && p->dist) ? moby_fog_band_ir0(p->zsf4) : ir0c;
    const int rc = ir0c >> 9, rp = ir0p >> 9;
    fd->row = rc; fd->tmix = 0;
    if (rc == rp) return;
    double t = (fd->ir0f - (double)ir0c) / (double)(ir0p - ir0c);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const int base = t <= 0.5 ? rc : rp, other = t <= 0.5 ? rp : rc;
    const int wq = (int)lround((t <= 0.5 ? t : 1.0 - t) * 30.0);
    fd->row = base;
    if (wq > 0) fd->tmix = (uint8_t)(((other - base + 7) << 4) | wq);
}

/** @brief The record's per-vertex colour base and colour fade for this present, by its LOD tier.
 *
 * The ladder is documented at moby_tier_faceblock; the tier is always the
 * tick's zsf4. Near (ZSF4 >= 1024): model colours (hdr+0x18) raw. Fog band
 * (0 < ZSF4 < 1024): model colours DPCS-faded toward the per-vertex far palette,
 * IR0 = (1024 - ZSF4) * 4 (.s @8001FDD0-8001FE54). LP/far band (ZSF4 <= 0): the
 * far palette (hdr+0x20) itself, which the .s walks with the LP faces; under
 * --no-lod the faces become HP and the palette stays, as in the game,
 * DPCS-faded toward the single cyclorama fog colour once ZSF4 < -2048 while
 * g_SkipLowPolyWorld > 0 (.L8001FEA0..FECC: IR0 = min(-(ZSF4+2048), 2047) << 1).
 *
 * A record carrying a fractional present ZSF4 (zf_on, zf != zsf4) takes the
 * fade term from zf through the same formula of its colour table, in double:
 * the HP table fades toward the far palette by (1024 - zf) * 4 clamped to
 * 0..4092, the band's own range (ZSF4 1..1023; 4096 >> 9 would index a ninth
 * row past the 8-row fogged-CLUT ladder), the LP table toward the fog colour
 * by 2 * min(-(zf + 2048), 2047) floored at 0, the latter only while
 * g_SkipLowPolyWorld (moby_game.skip_lowpoly) > 0. Every other record runs
 * the integer path above unchanged.
 * @param cpu The guest CPU state, read only.
 * @param R The record.
 * @param farbase Receives the far palette the HP colours fade toward, 0 for
 *        none.
 * @param fogfc Storage for the 12-bit fog colour of the LP band's fade.
 * @param fcc Receives fogfc when the LP band fades toward the fog colour,
 *        else NULL.
 * @param fd Receives the fade.
 * @return The colour table: the HP model colours (hdr+0x18) for ZSF4 > 0,
 *         else the far palette (hdr+0x20); 0 when the header is not an
 *         address. */
static uint32_t moby_record_fade(CPUState* cpu, const rec_meta* R, uint32_t* farbase,
                                 int fogfc[3], const int** fcc, moby_fade_t* fd) {
    const int frac = R->zf_on && R->zf != (double)R->zsf4;
    const double z = frac ? R->zf : (double)R->zsf4;
    const int hdr_ok = R->header >= 0x80000000u;
    *farbase = 0; *fcc = NULL;
    fd->ir0 = 0; fd->ir0f = 0.0; fd->frac = frac; fd->row = 0; fd->tmix = 0;
    if (R->zsf4 > 0) {
        if (!frac) {
            fd->ir0 = moby_color_fade_ir0(cpu, R->zsf4, R->header, farbase);
            fd->ir0f = (double)fd->ir0;
            fd->row = fd->ir0 >> 9;
        } else if ((*farbase = moby_fog_farbase(cpu, R->header)) != 0u) {
            double v = (1024.0 - z) * 4.0;
            fd->ir0f = v < 0.0 ? 0.0 : (v > 4092.0 ? 4092.0 : v);
            fd->ir0 = (int)fd->ir0f;
            moby_texel_fog_mix(R, fd);
        }
        return hdr_ok ? cpu->read_word(R->header + ANIM_COL_HP) : 0u;
    }
    if (moby_game.skip_lowpoly && (int32_t)cpu->read_word(moby_game.skip_lowpoly) > 0 &&
        (frac || R->zsf4 < -2048)) {
        double v = -(z + 2048.0);
        v = v < 0.0 ? 0.0 : (v > 2047.0 ? 2047.0 : v);
        fd->ir0f = 2.0 * v;
        fd->ir0 = (int)fd->ir0f;
        uint32_t fog = cpu->read_word(game_layout.cyc.bg_colour);
        fogfc[0] = (int)((fog << 4) & 0xFF0u);   /* 12-bit expand, .s 8001FEF4-FF08 */
        fogfc[1] = (int)((fog >> 4) & 0xFF0u);
        fogfc[2] = (int)((fog >> 12) & 0xFF0u);
        *fcc = fogfc;
    }
    return hdr_ok ? cpu->read_word(R->header + ANIM_COL_FAR) : 0u;
}

/** @brief A face's fogged-CLUT row offset: the fog band's row (moby_fade_t.row), 0 under the LP band's constant-colour fade.
 *
 * @param fd The record's fade.
 * @param fcc The LP band's constant far colour, or NULL.
 * @return The row offset. */
static int moby_face_fog_row(const moby_fade_t* fd, const int* fcc) {
    return fcc ? 0 : fd->row;
}
/** @brief A face's second fogged-CLUT row and weight (moby_fade_t.tmix), 0 under the LP band's constant-colour fade.
 *
 * @param fd The record's fade.
 * @param fcc The LP band's constant far colour, or NULL.
 * @return The packed mix, 0 for the one row alone. */
static uint8_t moby_face_fog_mix(const moby_fade_t* fd, const int* fcc) {
    return fcc ? 0 : fd->tmix;
}

/* ---- Tier-1 face-decode cache (one entry per faceblock) -------------------
 * Everything the walk parses OUT OF THE FACE STREAM is static per faceblock:
 * vertex/colour indices, flags, Tiledef UV/tpage/CLUT/ABR. Only the projected
 * vertices, the colour VALUES and the view-dependent spec env-UV/highlight
 * change per present. Re-parsing every face word through cpu->read_word on
 * every pass (canonical, per-present re-extract, SPEC) would be the dominant
 * cost of extraction, so the walk decodes once per faceblock and iterates the
 * decoded array; keyed by vaddr + bytecount + a 3-word content
 * signature so a level load (same vaddr, new model data) re-decodes. Slot
 * collision = evict + re-decode (never lies). Display-only: reads static
 * model RAM; the per-present half (NCLIP emit tree, colours, env, blend map)
 * stays outside the cache. */
/* One decoded face: renderer/moby_game.h owns the layout, since the game's
 * face decoder fills it. */
typedef moby_wface_t wface_t;
typedef struct {
    uint32_t fb, bytecount, sig1, sig2, sig3;
    int      nfaces;              /* emittable faces in f[] */
    int      raw, sprites, skips; /* walkdiag bookkeeping (raw = total stream faces) */
    uint8_t  align;               /* 0 = align_lo, 1 = align_hi, 2 = desync */
    uint32_t gen;                 /* resolve generation that last handed this entry out */
    uint32_t col_span;            /* bytes of the colour table f[] indexes: max corner offset + 4 */
    wface_t* f;
} fcache_t;
#define FC_SLOTS 512
static fcache_t s_fcache[FC_SLOTS];
static long s_fc_hit, s_fc_miss;      /* tier-census counters */
static wface_t s_fc_tmp[8192];        /* decode scratch, sized to the decoder's face cap */

/* ---- resolve generation -------------------------------------------------
 * The record prologue hands out an fcache_t* per record and the walk reads
 * them all afterwards, so an entry a record already holds may not be evicted
 * before that walk ends. Each resolve pass takes a generation; every entry it
 * returns is stamped with it, and an entry stamped with the current generation
 * is not a victim. When both ways of a hash pair are stamped, the decode goes to
 * a private overflow entry instead - outside the cache, so it evicts nothing
 * and is dropped when the next pass reuses the slot. Without this a third
 * faceblock on one hash pair silently rewrites an entry an earlier record
 * points at, and that record then walks another model's faces and colours. */
#define FC_OVF_SLOTS 64
static fcache_t s_fc_ovf[FC_OVF_SLOTS];
static int      s_fc_ovf_n;                  /* overflow entries taken this pass */
static uint32_t s_fc_gen = 1;                /* current resolve generation */
static long     s_fc_ovf_used, s_fc_ovf_full;  /* engagement census */

/** @brief Open a resolve pass, which retires the previous one.
 *
 * Advances the generation, so every entry still carries an older one and none
 * of them is held any more, and frees every overflow entry. */
static void fcache_pass_begin(void) { s_fc_gen++; s_fc_ovf_n = 0; }
/** @brief Whether the open resolve pass holds a cache entry.
 * @param C The entry.
 * @return Nonzero when its generation is the current one. */
static int  fcache_pinned(const fcache_t* C) { return C->gen == s_fc_gen; }
/** @brief Take the next free overflow entry of this resolve pass.
 *
 * Counts each take into s_fc_ovf_used and each refusal into s_fc_ovf_full.
 * @return The entry, or NULL when the pass has taken all FC_OVF_SLOTS. */
static fcache_t* fcache_ovf_take(void) {
    if (s_fc_ovf_n >= FC_OVF_SLOTS) { s_fc_ovf_full++; return NULL; }
    s_fc_ovf_used++;
    return &s_fc_ovf[s_fc_ovf_n++];
}

/** @brief Where a missing faceblock may be decoded, given the two ways of its hash pair.
 *
 * In order: an empty way, then a way the open resolve pass does not hold,
 * then a private overflow entry.
 * @param C The pair's first way.
 * @param C2 The pair's second way.
 * @return The entry to decode into, or NULL when nothing is free (overflow
 *         exhausted): the caller drops the decode rather than rewriting an
 *         entry a record already points at. */
static fcache_t* fcache_victim(fcache_t* C, fcache_t* C2) {
    if (!C->f)  return C;
    if (!C2->f) return C2;
    if (!fcache_pinned(C))  return C;
    if (!fcache_pinned(C2)) return C2;
    return fcache_ovf_take();
}

static int fcache_decode_into(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                              uint32_t s1, uint32_t s2, uint32_t s3, fcache_t* C);

/** @brief The decoded face array of a faceblock, from the two-way cache or freshly decoded.
 *
 * The key is the faceblock address, its byte count and its next three words (fb
 * + 4, + 8, + 12), which change when the game refills the block. A hash pair of
 * two ways is probed; a hit stamps the entry with the current resolve
 * generation, so the open pass keeps it. A miss decodes into the victim of
 * fcache_victim.
 * @param cpu The guest CPU state, read only.
 * @param fb The faceblock's PSX address.
 * @return The entry, valid to the end of the open resolve pass, or NULL when
 *         the byte count is 0 or above 0x10000 (a desynchronized block), no way
 *         or overflow entry is free, or the decode fails. */
static const fcache_t* fcache_get(CPUState* cpu, uint32_t fb) {
    uint32_t bytecount = cpu->read_word(fb);
    if (bytecount == 0 || bytecount > 0x10000u) return NULL;    /* desynchronized block */
    uint32_t s1 = cpu->read_word(fb + 4u), s2 = cpu->read_word(fb + 8u), s3 = cpu->read_word(fb + 12u);
    /* 2-way probe: a scene holds ~80+ hot faceblocks, so a 1-way map
     * collides and evicts; the second slot absorbs pairs. */
    uint32_t h = (((fb >> 2) * 2654435761u) >> 14) & (FC_SLOTS - 1);
    fcache_t* C = &s_fcache[h];
    if (C->f && C->fb == fb && C->bytecount == bytecount
        && C->sig1 == s1 && C->sig2 == s2 && C->sig3 == s3) { s_fc_hit++; C->gen = s_fc_gen; return C; }
    fcache_t* C2 = &s_fcache[h ^ 1u];
    if (C2->f && C2->fb == fb && C2->bytecount == bytecount
        && C2->sig1 == s1 && C2->sig2 == s2 && C2->sig3 == s3) { s_fc_hit++; C2->gen = s_fc_gen; return C2; }
    s_fc_miss++;
    fcache_t* V = fcache_victim(C, C2);
    if (!V) return NULL;
    if (!fcache_decode_into(cpu, fb, bytecount, s1, s2, s3, V)) return NULL;
    return V;
}

/** @brief The colour-table bytes a record's faces index, over its current and crossfade faces.
 *
 * @param FC The record's decoded faces.
 * @param FCp The previous keyframe's faces for the colour crossfade, or NULL.
 * @return The larger col_span of the two. */
static uint32_t fcache_col_span(const fcache_t* FC, const fcache_t* FCp) {
    return (FCp && FCp->col_span > FC->col_span) ? FCp->col_span : FC->col_span;
}

/** @brief Decode one faceblock into cache entry C through the game's face decoder.
 *
 * The stream parse is moby_game.decode_faces into the s_fc_tmp scratch (at
 * most 8192 faces); this shell copies the faces into a malloc'd array, frees
 * C's previous array, and stamps C with the key (address, byte count, three
 * signature words), the walk bookkeeping and the current resolve generation.
 * fcache_get and the render-time patched decode (fpcache_get) share it. On
 * failure C is unchanged.
 * @param cpu The guest CPU state, read only.
 * @param fb The faceblock's PSX address.
 * @param bytecount The faceblock's byte count word.
 * @param s1 First signature word (fb + 4).
 * @param s2 Second signature word (fb + 8).
 * @param s3 Third signature word (fb + 12).
 * @param C The entry to fill.
 * @return 1 on success, 0 when the decoder or the allocation fails. */
static int fcache_decode_into(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                              uint32_t s1, uint32_t s2, uint32_t s3, fcache_t* C) {
    moby_face_stats_t st = { 0, 0, 0, 2 };
    int n = moby_game.decode_faces(cpu, fb, bytecount, s_fc_tmp, 8192, &st);
    if (n < 0) return 0;
    wface_t* arr = (wface_t*)malloc(sizeof(wface_t) * (size_t)(n > 0 ? n : 1));
    if (!arr) return 0;
    memcpy(arr, s_fc_tmp, sizeof(wface_t) * (size_t)n);
    free(C->f);
    C->fb = fb; C->bytecount = bytecount; C->sig1 = s1; C->sig2 = s2; C->sig3 = s3;
    C->nfaces = n; C->raw = st.raw; C->sprites = st.sprites; C->skips = st.skips;
    C->align = (uint8_t)st.align;
    C->gen = s_fc_gen;            /* the pass that decoded it holds it */
    C->f = arr;
    C->col_span = 4u;
    for (int i = 0; i < n; i++) {
        const uint16_t c[4] = { arr[i].c0, arr[i].c1, arr[i].c2, arr[i].c3 };
        for (int k = 0; k < 4; k++)
            if ((uint32_t)c[k] + 4u > C->col_span) C->col_span = (uint32_t)c[k] + 4u;
    }
    return 1;
}

/* ---- render-time faceblock patches (frame-driven transient) ---------------
 * An animation KEYFRAME may carry a faceblock patch list: frame word1 low16
 * (<<2) = byte offset of the list WITHIN the frame. Entries: header word
 * ((cnt<<24) | byte-offset-into-the-HP-faceblock), then cnt replacement
 * words; the list ends with the entry whose header is <= 0 (bit31 = last).
 * The .s applies the patches IN PLACE before rendering each record ([F7FC
 * block_8001F92C..F998], dest base = [header+0x14], gated on ZSF4 > 0; spec
 * twin @800210C8) saving originals to scratch 0x2A00, and RESTORES them right
 * after the record renders (func_80020860 / spec 80022990). It is how effect
 * models animate structure per keyframe: the Gnasty-blast dome shrinks and
 * fades ENTIRELY through this (each keyframe re-points every strip face's
 * advancing-vertex word + colour-index word onto inner rings / darker colour
 * entries). The native walk reads the faceblock AFTER the .s restore, so
 * without this mirror it decodes the unpatched model - the user-visible
 * "constant full-size dome".
 *
 * Mirror: patch -> fcache_decode_into -> restore (forward order, like the .s
 * scratch walk), byte-exact and synchronous inside the render hook, so no
 * game code can observe the transient. The decode is cached keyed
 * (faceblock, frameA): the patch is a pure function of the static frame
 * asset, so a keyframe's patched decode replays for every present that still
 * renders that keyframe (incl. the sub-tick re-extract path). */
#define FPATCH_MAX 384
typedef struct { uint32_t addr, val; } fpsave_t;
static long s_fp_hit, s_fp_miss;   /* tier-census counters */

/** @brief Stage in the sub-tick view the span a patched faceblock decode touches.
 *
 * The span covers the faceblock the decode reads (its count word and
 * bytecount bytes) and every word the list patches, which the .s writes at an
 * offset into the HP faceblock. The list is walked read-only first, since the
 * span must exist before the first write; the walk stops where
 * moby_frame_patch_apply stops or where the words exceed FPATCH_MAX. A span
 * above 0x20000 bytes is refused.
 * @param cpu The guest CPU state, read only.
 * @param fb The faceblock the decode reads.
 * @param bytecount The faceblock's byte count.
 * @param header The AnimationHeader whose HP faceblock the list patches.
 * @param list The patch list.
 * @return Nonzero when the span was staged; 0 forbids the patch. */
static int moby_frame_patch_stage(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                                  uint32_t header, uint32_t list) {
    uint32_t lo = fb, hi = fb + 4u + bytecount;
    uint32_t fbHP = cpu->read_word(header + ANIM_FACES_HP);
    if (fbHP >= 0x80000000u && list >= 0x80000000u) {
        uint32_t at = list;
        int n = 0, entries = 0;
        for (;;) {
            uint32_t a0 = cpu->read_word(at); at += 4;
            uint32_t dst = fbHP + (a0 & 0xFFFFu);
            int cnt = (int)((a0 >> 24) & 0x7Fu);
            if (n + cnt > FPATCH_MAX) break;         /* the apply rolls this back */
            n += cnt; at += 4u * (uint32_t)cnt;
            uint32_t end = dst + 4u * (uint32_t)cnt;
            if (dst < lo) lo = dst;
            if (end > hi) hi = end;
            if ((int32_t)a0 <= 0 || ++entries > 128) break;
        }
    }
    if (hi <= lo || hi - lo > 0x20000u) return 0;
    return openpete_subtick_view_stage(lo, hi - lo) != NULL;
}

/** @brief Apply a keyframe's faceblock patch list to PSX RAM, saving each original word.
 *
 * Each entry is a header word, (cnt << 24) | the byte offset into the HP
 * faceblock with cnt in bits 24-30, followed by cnt replacement words; the
 * entry whose header word is <= 0 ends the list, and at most 129 entries
 * apply. The caller restores the saved words after the decode.
 * @param cpu The guest CPU state; the patched words are written.
 * @param header The AnimationHeader whose ANIM_FACES_HP names the faceblock.
 * @param list The patch list.
 * @param sv Receives (address, original) per patched word, FPATCH_MAX entries.
 * @return The patched word count; 0 when the list or the HP faceblock is below KSEG0; -1 when the list exceeds FPATCH_MAX words, after rolling every write back. */
static int moby_frame_patch_apply(CPUState* cpu, uint32_t header, uint32_t list,
                                  fpsave_t* sv) {
    if (list < 0x80000000u) return 0;
    uint32_t fbHP = cpu->read_word(header + ANIM_FACES_HP);
    if (fbHP < 0x80000000u) return 0;
    uint32_t at = list;
    int n = 0, entries = 0;
    for (;;) {
        uint32_t a0 = cpu->read_word(at); at += 4;
        uint32_t dst = fbHP + (a0 & 0xFFFFu);
        int cnt = (int)((a0 >> 24) & 0x7Fu);
        for (int q = 0; q < cnt; q++) {
            if (n >= FPATCH_MAX) {                  /* malformed/huge: roll back */
                for (int k = 0; k < n; k++) cpu->write_word(sv[k].addr, sv[k].val);
                return -1;
            }
            sv[n].addr = dst; sv[n].val = cpu->read_word(dst);
            cpu->write_word(dst, cpu->read_word(at));
            at += 4; dst += 4; n++;
        }
        if ((int32_t)a0 <= 0 || ++entries > 128) break;
    }
    return n;
}

#define FP_SLOTS 32
static fcache_t  s_fpc[FP_SLOTS];         /* patched decodes (keyed fb + frameA) */
static uint32_t  s_fpc_frame[FP_SLOTS];
static int       s_fpc_next;

/** @brief The face decode of a faceblock with a keyframe's patch list applied, cached per keyframe.
 *
 * The 32-slot table s_fpc is keyed by the faceblock, keyframe A and the
 * unpatched signature words. On a miss the patch words are written into the
 * faceblock in guest RAM (moby_frame_patch_apply), the block is decoded
 * (fcache_decode_into), and the originals are written back in forward order
 * before the call returns. On a sub-tick present the faceblock is first staged
 * (moby_frame_patch_stage), so the transient runs in the view's copy of it.
 *
 * When staging is refused or the list applies no words, the unpatched
 * fcache_get result is returned. The slot is taken round-robin past every entry
 * the open resolve pass holds; when all are held the decode goes to a private
 * overflow entry.
 * @param cpu The guest CPU state; the patch words are written to guest RAM and
 *            restored.
 * @param fb The faceblock's PSX address.
 * @param header The AnimationHeader whose high-poly faceblock the patch words
 *               address.
 * @param frameA Keyframe A's reference, part of the cache key.
 * @param list The patch list's PSX address.
 * @return The entry, valid to the end of the open resolve pass, or NULL when
 *         the byte count is 0 or above 0x10000, the decode fails, or no
 *         overflow entry is free. */
static const fcache_t* fpcache_get(CPUState* cpu, uint32_t fb, uint32_t header,
                                   uint32_t frameA, uint32_t list) {
    uint32_t bytecount = cpu->read_word(fb);
    if (bytecount == 0 || bytecount > 0x10000u) return NULL;    /* desync, as fcache_get */
    /* key sigs are the UNPATCHED words (lookup always happens pre-patch) */
    uint32_t s1 = cpu->read_word(fb + 4u), s2 = cpu->read_word(fb + 8u), s3 = cpu->read_word(fb + 12u);
    for (int i = 0; i < FP_SLOTS; i++) {
        fcache_t* C = &s_fpc[i];
        if (C->f && C->fb == fb && s_fpc_frame[i] == frameA && C->bytecount == bytecount
            && C->sig1 == s1 && C->sig2 == s2 && C->sig3 == s3) { s_fp_hit++; C->gen = s_fc_gen; return C; }
    }
    s_fp_miss++;
    /* The apply writes replacement words into the faceblock and restores them
     * after the decode. On a sub-tick present that write may not reach guest
     * memory, so the faceblock is staged first and the whole transient runs
     * inside the view's copy. Staging refused leaves the unpatched decode,
     * which draws the keyframe's base structure rather than publishing a
     * display transient through gameplay RAM. */
    if (openpete_subtick_view_active()
        && !moby_frame_patch_stage(cpu, fb, bytecount, header, list))
        return fcache_get(cpu, fb);
    static fpsave_t sv[FPATCH_MAX];
    int n = moby_frame_patch_apply(cpu, header, list, sv);
    if (n <= 0) return fcache_get(cpu, fb);         /* no/oversized list: unpatched decode */
    /* Round-robin past every slot an open resolve pass still holds, so a walk
     * with many patched keyframes cannot clobber a pointer it handed out; all
     * of them held sends the decode to a private overflow entry. */
    int slot = -1;
    for (int i = 0; i < FP_SLOTS; i++) {
        int c = (s_fpc_next + i) & (FP_SLOTS - 1);
        if (!fcache_pinned(&s_fpc[c])) { slot = c; break; }
    }
    fcache_t* C = (slot >= 0) ? &s_fpc[slot] : fcache_ovf_take();
    if (slot >= 0) s_fpc_next = (slot + 1) & (FP_SLOTS - 1);
    int ok = C && fcache_decode_into(cpu, fb, bytecount, s1, s2, s3, C);
    for (int k = 0; k < n; k++) cpu->write_word(sv[k].addr, sv[k].val);   /* forward restore (.s order) */
    if (!ok) return NULL;
    if (slot >= 0) s_fpc_frame[slot] = frameA;
    return C;
}

/** @brief The face decode for one (faceblock, header, keyframe) triple.
 *
 * The patched decode (fpcache_get) when the keyframe carries a patch list
 * under the .s gate ZSF4 > 0 (the LP tier never patches), else the Tier-1
 * cache (fcache_get).
 * @param cpu The guest CPU state; a patched decode writes and restores the faceblock.
 * @param rec The cull record's address, 0 for a host-built record.
 * @param model The model whose table resolves the patch list where the game needs one.
 * @param fb The faceblock.
 * @param header The AnimationHeader.
 * @param frameA Keyframe A's reference.
 * @param zsf4 The record's tier.
 * @return The decode, or NULL when the faceblock does not decode. */
static const fcache_t* moby_faces_for(CPUState* cpu, uint32_t rec, uint32_t model,
                                      uint32_t fb,
                                      uint32_t header, uint32_t frameA, int zsf4) {
    if (zsf4 > 0 && header >= 0x80000000u && moby_pose_live(header, frameA)
        && moby_game.frame_patch_list) {
        uint32_t list = moby_game.frame_patch_list(cpu, rec, model, frameA);
        if (list >= 0x80000000u) return fpcache_get(cpu, fb, header, frameA, list);
    }
    return fcache_get(cpu, fb);
}
/** @brief The face decode for a cull record, from the record's own faceblock, header, keyframe and tier.
 *
 * @param cpu The guest CPU state; a patched decode writes and restores the
 *        faceblock.
 * @param R The record.
 * @return The decode, or NULL when the faceblock does not decode. */
static const fcache_t* moby_faces_for_record(CPUState* cpu, const rec_meta* R) {
    return moby_faces_for(cpu, R->addr, R->model_held, R->faceblock,
                          R->header, R->frameA, R->zsf4);
}
/** @brief The model whose own table resolves the patch list of a keyframe no cull record names.
 *
 * Where the record holds the keyframe as its stream word
 * (rec_frame_is_word) the list pointer is the cull's per-record store at
 * record +0x14, which exists for the keyframe this tick drew only; the
 * previous tick's keyframe resolves through the table that store was read
 * from (Model+0x34, mobydraw.s:563-569). Where the record holds a pointer
 * into the frame array the list sits inside the keyframe and no model is
 * needed: 0.
 * @param cpu The guest CPU state, read only.
 * @param R The record.
 * @return The model; 0 on a pointer-form game, and 0 on a word-form game
 *         when the record names no moby or the moby's class has no model,
 *         which the caller reads as no previous list. */
static uint32_t moby_prev_patch_model(CPUState* cpu, const rec_meta* R) {
    if (!game_layout.moby.rec_frame_is_word || R->moby < 0x80000000u) return 0u;
    uint32_t cls   = cpu->read_half(R->moby + game_layout.moby.class_off);
    uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    return (model >= 0x80000000u) ? model : 0u;
}
/** @brief The previous keyframe's faces for the sub-tick colour crossfade: the faces the game drew last tick, with last tick's patches.
 *
 * NULL, a snap, when the previous pose is not live. Face-for-face colour
 * correspondence needs the previous header to share the current one's HP
 * faceblock and both colour tables; a header differing in any of the three
 * returns NULL. A previous header below KSEG0 decodes against the current one.
 * @param cpu The guest CPU state; a patched decode writes and restores the faceblock.
 * @param R The record.
 * @param model The model whose table resolves the previous keyframe's patch list.
 * @return The decode, or NULL. */
static const fcache_t* moby_faces_for_prev(CPUState* cpu, const rec_meta* R,
                                           uint32_t model) {
    if (!moby_pose_live(R->header_p, R->frameA_p)) return NULL;
    uint32_t hp = (R->header_p >= 0x80000000u) ? R->header_p : R->header;
    if (hp != R->header) {
        if (cpu->read_word(hp + ANIM_FACES_HP) != cpu->read_word(R->header + ANIM_FACES_HP) ||
            cpu->read_word(hp + ANIM_COL_HP)   != cpu->read_word(R->header + ANIM_COL_HP)   ||
            cpu->read_word(hp + ANIM_COL_FAR)  != cpu->read_word(R->header + ANIM_COL_FAR)) return NULL;
    }
    return moby_faces_for(cpu, R->addr, model, R->faceblock, hp, R->frameA_p, R->zsf4);
}
/** @brief The previous keyframe's faces for a record whose colour crossfade runs this present, or NULL when it does not.
 *
 * NULL when the record is outside the patching tier (ZSF4 <= 0, the .s
 * gate), the game resolves no patch lists, the record has no previous pose,
 * a word-form game has no model for the previous keyframe
 * (moby_prev_patch_model), or neither keyframe carries a patch list, in
 * which case both decodes are the same unpatched faces and the crossfade is a
 * no-op. Also NULL when the previous decode is the current one or has
 * another face count, so faces correspond one for one.
 * @param cpu The guest CPU state; a patched decode writes and restores the faceblock.
 * @param R The record.
 * @param FC The current keyframe's decode.
 * @return The previous decode, or NULL. */
static const fcache_t* moby_clerp_prev_faces(CPUState* cpu, const rec_meta* R,
                                             const fcache_t* FC) {
    if (R->zsf4 <= 0 || R->header < 0x80000000u || !moby_game.frame_patch_list ||
        !moby_pose_live(R->header_p, R->frameA_p)) return NULL;
    uint32_t model = moby_prev_patch_model(cpu, R);
    if (game_layout.moby.rec_frame_is_word && !model) return NULL;
    int curr_p = moby_pose_live(R->header, R->frameA) &&
        moby_game.frame_patch_list(cpu, R->addr, R->model_held, R->frameA) >= 0x80000000u;
    int prev_p = moby_game.frame_patch_list(cpu, R->addr, model, R->frameA_p) >= 0x80000000u;
    if (!curr_p && !prev_p) return NULL;
    const fcache_t* FCp = moby_faces_for_prev(cpu, R, model);
    if (FCp == FC || (FCp && FCp->nfaces != FC->nfaces)) return NULL;
    return FCp;
}
/** @brief Per-channel lerp of two 0x00BBGGRR walk colours, each channel's step rounded to nearest.
 *
 * @param a The colour at t = 0.
 * @param b The colour at t = 1.
 * @param t The phase.
 * @return The colour. */
static inline uint32_t moby_clerp(uint32_t a, uint32_t b, float t) {
    int r = (int)(a & 0xFFu)         + (int)lroundf(((int)(b & 0xFFu)         - (int)(a & 0xFFu))         * t);
    int g = (int)((a >> 8) & 0xFFu)  + (int)lroundf(((int)((b >> 8) & 0xFFu)  - (int)((a >> 8) & 0xFFu))  * t);
    int bl = (int)((a >> 16) & 0xFFu) + (int)lroundf(((int)((b >> 16) & 0xFFu) - (int)((a >> 16) & 0xFFu)) * t);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)bl << 16);
}
static long s_clerp_recs;   /* tier-census: records colour-crossfaded this window */

static int build_walk_tris_inner(CPUState* cpu);
/** @brief Build s_wtri[] from the face-stream walk of the queued records and return the triangle count.
 *
 * Runs build_walk_tris_inner. Under OPENPETE_MOBY_TIER_DBG the wall time adds
 * to s_us_walk_rex during a sub-tick re-extract (s_walk_in_rex) and to
 * s_us_walk otherwise. Each face runs the dual-NCLIP emit tree of walk_records
 * on float screen coordinates, so a backface-culled face emits nothing and a
 * quad emits one or two triangles.
 * @param cpu The guest CPU state.
 * @return The triangle count, s_gout.n. */
static int build_walk_tris(CPUState* cpu) {
    if (!tier_dbg_on()) return build_walk_tris_inner(cpu);
    uint64_t t0 = openpete_phase_now_us();
    int n = build_walk_tris_inner(cpu);
    long dt = (long)(openpete_phase_now_us() - t0);
    if (s_walk_in_rex) s_us_walk_rex += dt; else s_us_walk += dt;
    return n;
}
/* Faces resolved by the serial prologue, one entry per record. The walk itself
 * must not resolve them: the decode patches guest RAM in place and restores it,
 * and both face caches allocate and evict - none of which is safe to run on
 * several workers at once. */
static const fcache_t* s_pre_FC[MAX_RECORDS];
static const fcache_t* s_pre_FCp[MAX_RECORDS];
static float           s_pre_clra[MAX_RECORDS];
/* Where each record's triangles landed, for the ordered merge. */
static struct { int worker, start, count; } s_pre_rec[MAX_RECORDS];

/** @brief The bucket word of the prim a regular-walk face becomes to the draw, or -1.
 *
 * The draw's facing test runs on the GTE's integer SXY (proj_xy.iv) and
 * picks the prim, and with it the vertices and weights the key sums; a face
 * the float emission draws but the integer test culls receives -1, as does
 * every face of a record with no paint key. A strip face and a fresh triangle
 * take the single-triangle test.
 * @param rec0 The record's first projection, which carries its paint key.
 * @param F The face.
 * @param p0 The face's first vertex.
 * @param p1 The second vertex.
 * @param p2 The third vertex.
 * @param p3 The fourth vertex, or NULL for a triangle.
 * @return The bucket word, or -1. */
static int moby_face_otkey(const proj_xy* rec0, const wface_t* F, const proj_xy* p0,
                           const proj_xy* p1, const proj_xy* p2, const proj_xy* p3) {
    if (rec0->otk.row == MOBY_OTK_NONE) return -1;
    moby_otk_face_t f = { { &p0->iv, &p1->iv, &p2->iv, (F->fresh && p3) ? &p3->iv : NULL },
                          F->otbias, INT32_MIN, (int8_t)F->bit0, 0 };
    return moby_otk_face_bucket(&rec0->otk, &f);
}

/* ---- exactly coincident faces of one moby ---------------------------------
 * Two faces of one model can carry the same three projected vertices (an
 * eye decal authored over the head face it sits on). The draw orders them
 * by its ordering table: func_8001F798 appends each prim at the TAIL of its
 * local bucket (r_moby.s 1153-1168 and 1190-1221 quads, 1335-1350 and
 * 1377-1409 triangles: the old tail's link is pointed at the new prim and the
 * bucket's end word replaced), and the splice links the local buckets from
 * the highest down (r_moby.s 1553-1599, $a0 from $s5 stepping -8), so the
 * prim painted last is the one in the lowest bucket and, within one bucket,
 * the one emitted last. Under the native depth test the pair's depths are
 * equal and the winner is whatever the rasteriser rounds per winding; the
 * pass below finds such pairs per record and gives the game's last-painted
 * member the coplanar-decal pull (vz * 0.998) the special-face path uses. */

/* One coincident-face class: its vertex key, the member the draw paints
 * last, and the member count. */
typedef struct { uint32_t key[12]; int top, n; } coinc_slot;
static _Thread_local coinc_slot* t_coinc;   /* open-addressed table, per worker */
static _Thread_local int t_coinc_cap;

/** @brief Write a walk triangle's order-free vertex key: (vz, tvz, sx, sy) bit patterns per vertex, sorted.
 *
 * Two triangles get equal keys exactly when their three vertices match as a
 * set, whatever the winding or the starting vertex. vz is part of the key so
 * a face that already carries a decal pull no longer matches its base.
 * @param t The triangle.
 * @param key Receives 12 words. */
static void coinc_key_of(const ntri_t* t, uint32_t key[12]) {
    uint32_t v[3][4];
    for (int q = 0; q < 3; q++) {
        memcpy(&v[q][0], &t->v[q].vz, 4);  memcpy(&v[q][1], &t->v[q].tvz, 4);
        memcpy(&v[q][2], &t->v[q].sx, 4);  memcpy(&v[q][3], &t->v[q].sy, 4);
    }
    static const int cs[3][2] = { {0, 1}, {1, 2}, {0, 1} };   /* 3-element sorting network */
    for (int k = 0; k < 3; k++) {
        uint32_t* a = v[cs[k][0]]; uint32_t* b = v[cs[k][1]];
        if (memcmp(a, b, sizeof v[0]) > 0) {
            uint32_t tmp[4]; memcpy(tmp, a, sizeof tmp); memcpy(a, b, sizeof tmp); memcpy(b, tmp, sizeof tmp);
        }
    }
    memcpy(key, v, sizeof v);
}

/** @brief Whether the draw paints prim i after prim j, i emitted after j, both of one moby.
 *
 * With both bucket words known the lower bucket paints last, and within one
 * bucket the later emission (the draw appends at the bucket's tail). A prim
 * the draw drops (word -1 while the other is keyed) paints never. With no
 * key on either side (the draw drops both, or the game mirrors no key) the
 * pair is taken as one bucket and the later emission paints last.
 * @param oi Prim i's bucket word.
 * @param oj Prim j's bucket word.
 * @return 1 when i paints after j. */
static int coinc_paints_after(int32_t oi, int32_t oj) {
    if (oi < 0 || oj < 0) return oi >= 0 || oj < 0;
    return MOBY_OTK_BUCKET(oi) <= MOBY_OTK_BUCKET(oj);
}

/** @brief Enter emitted triangle i into the coincidence table, updating its class's last-painted member.
 *
 * Under OPENPETE_MOBY_VERIFY each match prints one line on its VBL.
 * @param O The emit target.
 * @param i The triangle's index in O.
 * @param mask The table size less one. */
static void coinc_insert(mn_out* O, int i, int mask) {
    uint32_t key[12];
    coinc_key_of(&O->tri[i], key);
    uint32_t h = 2166136261u;
    for (int k = 0; k < 12; k++) h = (h ^ key[k]) * 16777619u;
    coinc_slot* s = &t_coinc[h & (uint32_t)mask];
    while (s->n && memcmp(s->key, key, sizeof key) != 0) {
        h++;
        s = &t_coinc[h & (uint32_t)mask];
    }
    if (!s->n) { memcpy(s->key, key, sizeof key); s->top = i; s->n = 1; return; }
    t_diag->coinc_pairs++;
    int j = s->top;
    if (coinc_paints_after(O->otk[i], O->otk[j])) s->top = i;
    s->n++;
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-coincident vbl=%u] moby=%08X tri=%d,%d otk=%08X,%08X tex=%d,%d "
                "fkey=%08X,%08X last=%d vz=%.2f,%.2f,%.2f\n",
                openpete_frame, O->moby[i], j, i, (uint32_t)O->otk[j], (uint32_t)O->otk[i],
                O->tri[j].textured, O->tri[i].textured, O->fkey[j], O->fkey[i], s->top,
                (double)O->tri[i].v[0].vz, (double)O->tri[i].v[1].vz, (double)O->tri[i].v[2].vz);
}

/** @brief Pull toward the camera the last-painted member of every set of exactly coincident triangles one moby record emitted.
 *
 * The triangles [start, O->n) are one record's. Keys are hashed into a
 * thread-local open-addressed table sized to twice the triangle count
 * (cleared per record); each class keeps the member the game's draw paints
 * last (coinc_paints_after), and that member alone takes vz * 0.998 on its
 * three vertices, so the depth test keeps the game's paint order for exactly
 * coincident faces. Non-coincident triangles are untouched. Matches and
 * pulls count into t_diag.
 * @param O The emit target.
 * @param start The record's first triangle in O. */
static void moby_coincident_pull(mn_out* O, int start) {
    int n = O->n - start;
    if (n < 2) return;
    int cap = 16;
    while (cap < 2 * n) cap <<= 1;
    if (cap > t_coinc_cap) {
        coinc_slot* nt = (coinc_slot*)realloc(t_coinc, sizeof(coinc_slot) * (size_t)cap);
        if (!nt) return;
        t_coinc = nt; t_coinc_cap = cap;
    }
    for (int s = 0; s < cap; s++) t_coinc[s].n = 0;
    for (int i = start; i < O->n; i++) coinc_insert(O, i, cap - 1);
    for (int s = 0; s < cap; s++) {
        if (t_coinc[s].n < 2) continue;
        ntri_t* t = &O->tri[t_coinc[s].top];
        t->v[0].vz *= 0.998f; t->v[1].vz *= 0.998f; t->v[2].vz *= 0.998f;
        t_diag->coinc_pulls++;
    }
}

/** @brief Walk the records [r0, r1) of s_recs into the calling worker's emit target.
 *
 * Per record: the specular pass skips a moby the regular pass already fed
 * (s_reg_moby), since the specular cull has advanced its pose; a run-once
 * moby emits nothing (moby_run_once_hidden); the record's HUD tag, front-pass
 * eligibility, colour bases and fade (moby_record_fade) are resolved once;
 * then each face of the record's resolved face cache runs the dual-NCLIP
 * emit tree on the float screen coordinates. A record with no faceblock, no
 * projected vertices or no resolved face cache emits nothing.
 * @param cpu The guest CPU state, read only.
 * @param is_spec Nonzero for the specular pass.
 * @param r0 The first record index.
 * @param r1 One past the last record index. */
static void walk_records(CPUState* cpu, int is_spec, int r0, int r1) {
    for (int r = r0; r < r1; r++) {
        rec_meta* R = &s_recs[r];
        uint32_t mob = R->moby;   /* set in all paths (==rec+0x34 for .s recs); host-built reveal/re-extract records have addr=0 so reading R->addr+0x34 would miss the dedup */
        /* SPECULAR dedup: skip a moby the REGULAR pass already fed (overlap -> would
         * double at a different pose). Specular-only mobys (not in s_reg_moby) feed. */
        if (is_spec) {
            int dup = 0;
            for (int k = 0; k < s_reg_moby_n; k++) if (s_reg_moby[k] == mob) { dup = 1; break; }
            if (dup) continue;
        }
        s_cur_moby_key = mob;     /* face identity for this record's tris */
        s_cur_moby_cls = mob ? (uint16_t)cpu->read_half(mob + game_layout.moby.class_off) : 0; /* m_Class */
        s_cur_moby_dofs = mob ? (int8_t)cpu->read_byte(mob + game_layout.moby.angle_off + 3u) : 0; /* og OT bias */
        /* Run-once mobys emit nothing. The record stays in s_recs (the .s cull
         * queued it, and s_reg_moby membership is what the reveal builder and
         * the occlusion/hold bookkeeping dedup against) - only its triangles
         * are dropped, so this is display-side alone: the .s pass still sets
         * m_WasDrawn @8001F4CC, the moby still auto-includes into the next
         * tick's update list, and its one-shot job still runs on schedule. */
        if (mob && moby_run_once_hidden(cpu, mob, s_cur_moby_cls)) continue;
        /* OPENPETE_MOBY_CLASSDBG=1: one line per drawn moby (class + vaddr +
         * world position) - names the producer when a scene object needs a
         * class-keyed treatment (e.g. a mod material selector). */
        {
            static int clsdbg = -1;
            if (clsdbg < 0) { const char* e = getenv("OPENPETE_MOBY_CLASSDBG"); clsdbg = (e && *e && *e != '0'); }
            if (clsdbg && mob) {
                static unsigned last_f = 0xFFFFFFFFu; static int printed;
                if (openpete_frame != last_f) { last_f = openpete_frame; printed = 0; }
                if (printed < 128) {
                    printed++;
                    fprintf(stderr, "[moby-class vbl=%u] cls=%u moby=%08x pos=(%d, %d, %d)\n",
                            openpete_frame, s_cur_moby_cls, mob,
                            (int)cpu->read_word(mob + game_layout.moby.pos_off + 0u), (int)cpu->read_word(mob + game_layout.moby.pos_off + 4u),
                            (int)cpu->read_word(mob + game_layout.moby.pos_off + 8u));
                }
            }
        }
        s_cur_env_az = R->env_az; /* per-moby env-UV bias input (SPEC metal) */
        { /* HUD tag: 2=HUD3D icon, 1=HUD flat, 0=actor */
          s_cur_ui = moby_hud_tag(mob);
          /* Regular-walk twin of the shaded HUD3D promotion: a UI-range moby
           * that is a real 3D MODEL self-occludes and needs its own Z pass -
           * the flat HUD layer's per-tri painter can't sort interpenetrating
           * faces (a fly-in body model shows its interior through its far
           * side). Promote by PROJECTED view-Z extent of this
           * record's verts (full-world units; a text glyph spans a few units,
           * a body model hundreds). Thin glyphs stay flat HUD, under the
           * player, as in the shaded walk. */
          if (s_cur_ui == 1 && R->proj_n > 1) {
              float lo = 1e30f, hi = -1e30f;
              for (int pv = 0; pv < R->proj_n; pv++) {
                  float pz = s_proj[R->proj_off + pv].vz;
                  if (pz <= 0.0f) continue;
                  if (pz < lo) lo = pz;
                  if (pz > hi) hi = pz;
              }
              if (hi > lo && hi - lo >= 96.0f) s_cur_ui = 2;
          } }
        /* Front-pass eligibility: this moby is one of the dragon-rescue cutscene
         * actors. Only for real actors (ui==0). */
        s_cur_front = s_cur_ui == 0 && moby_is_cutscene_actor(cpu, mob);
        uint32_t fb = R->faceblock;
        if (fb < 0x80000000u || R->proj_n <= 0) continue;
        const fcache_t* FC = s_pre_FC[r];   /* Tier-1 decode, resolved by the prologue */
        if (!FC) { t_diag->desync++; continue; }
        int base = R->proj_off, vcount = R->proj_n;
        /* per-vertex colour base + fade by the record's LOD tier and the
         * present's ZSF4 (moby_record_fade) */
        uint32_t farbase = 0; int fogfc[3]; const int* fcc = NULL; moby_fade_t fd;
        uint32_t cbase = moby_record_fade(cpu, R, &farbase, fogfc, &fcc, &fd);
        const int dpcs_ir0 = fd.ir0;
        if (cbase < 0x80000000u) cbase = COLOR_SCRATCH;   /* fall back to fog scratch */
        { /* OPENPETE_MOBY_FADEDBG=frameLo: per-record LOD-fade forensics (4 frames). */
            static int s_fd0 = -2;
            if (s_fd0 == -2) { const char* e = getenv("OPENPETE_MOBY_FADEDBG"); s_fd0 = (e && *e) ? atoi(e) : -1; }
            if (s_fd0 >= 0 && (int)openpete_frame >= s_fd0 && (int)openpete_frame < s_fd0 + 4) {
                uint32_t c0 = cpu->read_word(cbase), c1 = cpu->read_word(cbase + 4);
                uint32_t f0 = farbase ? cpu->read_word(farbase) : 0, f1 = farbase ? cpu->read_word(farbase + 4) : 0;
                fprintf(stderr, "[fade vbl=%u] rec=%d moby=%08x cls=%03x zsf4=%d zf=%.3f ir0f=%.3f hdr=%08x cbase=%08x farbase=%08x ir0=%d "
                        "c0=%06X c1=%06X f0=%06X f1=%06X\n",
                        openpete_frame, r, R->moby, R->moby >= 0x80000000u ? (cpu->read_half(R->moby + game_layout.moby.class_off) & 0xFFFu) : 0xFFF,
                        R->zsf4, R->zf_on ? R->zf : (double)R->zsf4, fd.ir0f, R->header, cbase, farbase, dpcs_ir0, c0 & 0xFFFFFFu, c1 & 0xFFFFFFu, f0 & 0xFFFFFFu, f1 & 0xFFFFFFu);
                if (dpcs_ir0 > 0 && farbase) {   /* one fog-band record: full table triplets */
                    int nc = (int)(cpu->read_half(R->header + game_layout.moby.anim_ccount_off) & 0xFFFFu); if (nc > 24) nc = 24;
                    for (int k = 0; k < nc; k++) {
                        uint32_t nw = cpu->read_word(cbase + (uint32_t)k*4u) & 0xFFFFFFu;
                        uint32_t fw2 = cpu->read_word(farbase + (uint32_t)k*4u) & 0xFFFFFFu;
                        uint32_t ov = moby_color_at(cpu, cbase, farbase, NULL, &fd, (uint32_t)k*4u);
                        fprintf(stderr, "  [ftab k=%d] near=%06X far=%06X out=%06X\n", k, nw, fw2, ov);
                    }
                }
            }
        }
        /* Resolve the colour bases to host pointers once per record; the loop
         * then reads colours directly (moby_color_at_host). Fallback = the
         * cpu->read_word path when a base doesn't map (never in practice). */
        const fcache_t* FCp = s_pre_FCp[r]; float clr_a = s_pre_clra[r];
        const uint32_t col_span = fcache_col_span(FC, FCp);
        const uint8_t* cb_host = (const uint8_t*)openpete_subtick_host_span(cbase, col_span);
        const uint8_t* fb_host = farbase ? (const uint8_t*)openpete_subtick_host_span(farbase, col_span) : NULL;
        int host_col = (cb_host != NULL) && (farbase == 0 || fb_host != NULL);
        /* Sub-tick COLOUR crossfade (patch-animated effects): the fade of the
         * Gnasty-blast dome/rings is per-KEYFRAME colour-index re-pointing down
         * a pre-baked ladder (faceblock patches) - the colour block itself is
         * static, so colours snap each keyframe (~15Hz). Crossfade per face
         * between the PREV drawn keyframe's colours and the curr ones by the
         * SAME alpha that lerps the vertex streams - endpoints are exactly the
         * two states the game drew. Only fetched when a patch list is in play
         * (prev==curr faces otherwise makes it a no-op). */
        t_diag->faces  += FC->raw;
        t_diag->sprite += FC->sprites;
        t_diag->skip   += FC->skips;
        if      (FC->align == 0) t_diag->align_lo++;
        else if (FC->align == 1) t_diag->align_hi++;
        else                     t_diag->desync++;
        const int rec_tri0 = t_out->n;   /* this record's first emitted triangle */
        for (int fi = 0; fi < FC->nfaces; fi++) {
            const wface_t* F = &FC->f[fi];
            int fresh = F->fresh, textured = F->textured;
            if (!fresh)        t_diag->strip++;
            else if (textured) t_diag->fresh_t++;
            else               t_diag->fresh_g++;
            int i0 = F->i0, i1 = F->i1, i2 = F->i2;
            int i3 = F->i3;   /* 4th-vertex SXY index (raw; strips never consume it) */
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount) { t_diag->oor++; continue; }
            proj_xy* p0 = &s_proj[base+i0]; proj_xy* p1 = &s_proj[base+i1]; proj_xy* p2 = &s_proj[base+i2];

            /* FRESH faces run the .s's dual-NCLIP (block_8002008C): NCLIP1=(v0,v1,v2),
             * then v3 is loaded into SXY0 so NCLIP2=(v3,v1,v2). bit0(=w0f&1) flips the
             * sign test (the `neg` dance). The emit TARGET (read from the .s by its GPU
             * command byte -- 0x30=tri, 0x38=quad) determines tri-vs-quad, and it is the
             * OPPOSITE of what the t6a>=0 "s1=0x10" path suggests:
             *   t6a>=0 && t6b>0  -> .L800203F4 (+8=v3): single 0x30 TRI (v3,v1,v2)
             *   t6a>=0 && t6b<=0 -> skip the whole face
             *   t6a<0  && t6b>0  -> .L80020158:        0x38 QUAD (v0,v1,v2)+(v1,v3,v2)
             *   t6a<0  && t6b<=0 -> .L800203F4 (+8=v0): single 0x30 TRI (v0,v1,v2)
             * (The inverted map draws one half of every quad.)
             * STRIPS (.L8002031C) are single triangles, keep if NCLIP>0. */
            proj_xy* p3 = (i3 < vcount) ? &s_proj[base+i3] : NULL;
            /* Backface NCLIP on the precise FLOAT screen coords (NOT the GTE's floored
             * int16): the 1px floor flips a near-zero-area face's sign at grazing angles,
             * culling genuinely front-facing thin-moby tops (Artisans water platforms,
             * cls 0x12 -> waterfall showed THROUGH the disc). Float is exact there, and the
             * native Z-buffer sorts what survives. (PSX/PsyCross floor here and show gaps.) */
            double ncl1 = tri_nclip(p0->sx,p0->sy,p1->sx,p1->sy,p2->sx,p2->sy);
            double ncl2 = p3 ? tri_nclip(p3->sx,p3->sy,p1->sx,p1->sy,p2->sx,p2->sy) : -1.0;
            int emit_a = 0, emit_b = 0, b_is_v3tri = 0;
            if (fresh) {
                int bit0 = (int)F->bit0;
                double t6a = (bit0 && ncl1 > 0.0) ? -ncl1 : ncl1;   /* asm .L800200D0 blez: negate NCLIP1 when >0 */
                double t6b = (bit0 && ncl2 < 0.0) ? -ncl2 : ncl2;   /* asm .L80020108/.L80020138 bgez: negate NCLIP2 when <0 */
                /* Emit-target codes read from the .s:
                 *   t6a>=0 && t6b>0  -> .L800203F4 (+8=v3): single 0x30 TRI (v3,v1,v2)
                 *   t6a>=0 && t6b<=0 -> SKIP
                 *   t6a<0  && t6b>0  -> .L80020158:        0x38 QUAD (v0,v1,v2)+(v1,v3,v2)
                 *   t6a<0  && t6b<=0 -> .L800203F4 (+8=v0): single 0x30 TRI (v0,v1,v2) */
                if (t6a >= 0.0) { if (t6b > 0.0 && p3) { emit_b = b_is_v3tri = 1; } }   /* tri (v3,v1,v2) */
                else if (t6b > 0.0 && p3) { emit_a = emit_b = 1; t_diag->quad++; }   /* QUAD */
                else emit_a = 1;                                                        /* tri (v0,v1,v2) */
            } else {
                /* strip: single tri. bit0=0 keep NCLIP>0; bit0=1 keep NCLIP!=0
                 * (.L800203B8 vs .L800203D0 -- bit0 strips carry either winding). */
                if (F->bit0 ? (ncl1 != 0.0) : (ncl1 > 0.0)) emit_a = 1;
            }
            if (!emit_a && !emit_b) continue;

            /* per-vertex colour: cbase indexed by the cached w1 fields. DPCS-faded
             * when the moby carries a fade level (effect ring puffs, etc). */
            uint32_t c0, c1, c2, c3;
            if (host_col) {
                c0 = moby_color_at_host(cb_host, fb_host, fcc, &fd, F->c0);
                c1 = moby_color_at_host(cb_host, fb_host, fcc, &fd, F->c1);
                c2 = moby_color_at_host(cb_host, fb_host, fcc, &fd, F->c2);
                c3 = moby_color_at_host(cb_host, fb_host, fcc, &fd, F->c3);
                { /* FADEDBG face-level dump for the fog-band record */
                    static int s_ff0 = -2;
                    if (s_ff0 == -2) { const char* e = getenv("OPENPETE_MOBY_FADEDBG"); s_ff0 = (e && *e) ? atoi(e) : -1; }
                    if (s_ff0 >= 0 && dpcs_ir0 > 0 && farbase &&
                        (int)openpete_frame >= s_ff0 && (int)openpete_frame < s_ff0 + 2)
                        fprintf(stderr, "  [fface] v0=(%.0f,%.0f) tex=%d ci=%d,%d,%d nr=%06X fr=%06X out=%06X,%06X,%06X clut=%d,%d tp=%d,%d\n",
                                s_proj[base + F->i0].sx, s_proj[base + F->i0].sy, F->textured,
                                F->c0, F->c1, F->c2,
                                le32_at(cb_host + F->c0) & 0xFFFFFFu, le32_at(fb_host + F->c0) & 0xFFFFFFu,
                                c0, c1, c2, F->clx, F->cly, F->tpx, F->tpy);
                }
            } else {
                c0 = moby_color_at(cpu, cbase, farbase, fcc, &fd, F->c0);
                c1 = moby_color_at(cpu, cbase, farbase, fcc, &fd, F->c1);
                c2 = moby_color_at(cpu, cbase, farbase, fcc, &fd, F->c2);
                c3 = moby_color_at(cpu, cbase, farbase, fcc, &fd, F->c3);
            }
            if (FCp) {   /* colour crossfade: prev-keyframe colours -> curr by clr_a */
                const wface_t* P = &FCp->f[fi];
                uint32_t p0, p1, p2, p3v;
                if (host_col) {
                    p0 = moby_color_at_host(cb_host, fb_host, fcc, &fd, P->c0);
                    p1 = moby_color_at_host(cb_host, fb_host, fcc, &fd, P->c1);
                    p2 = moby_color_at_host(cb_host, fb_host, fcc, &fd, P->c2);
                    p3v = moby_color_at_host(cb_host, fb_host, fcc, &fd, P->c3);
                } else {
                    p0 = moby_color_at(cpu, cbase, farbase, fcc, &fd, P->c0);
                    p1 = moby_color_at(cpu, cbase, farbase, fcc, &fd, P->c1);
                    p2 = moby_color_at(cpu, cbase, farbase, fcc, &fd, P->c2);
                    p3v = moby_color_at(cpu, cbase, farbase, fcc, &fd, P->c3);
                }
                uint32_t c0curr = c0;
                c0 = moby_clerp(p0, c0, clr_a);
                c1 = moby_clerp(p1, c1, clr_a);
                c2 = moby_clerp(p2, c2, clr_a);
                c3 = moby_clerp(p3v, c3, clr_a);
                /* OPENPETE_MOBY_CLERP_DBG=1: face-0 ramp per present - must run
                 * monotonically prev->curr within each keyframe hold */
                { static int s_cd = -1;
                  if (s_cd < 0) { const char* e = getenv("OPENPETE_MOBY_CLERP_DBG"); s_cd = (e && *e && *e != '0'); }
                  if (s_cd && fi == 0)
                      fprintf(stderr, "[clerp] vbl=%u m=%08X a=%.3f p=%06X c=%06X out=%06X\n",
                              openpete_frame, R->moby, (double)clr_a,
                              p0 & 0xFFFFFFu, c0curr & 0xFFFFFFu, c0 & 0xFFFFFFu); }
            }
            /* v3 (4th-vertex) gouraud colour: its index is the HIGH field of the
             * fp+8 word ((v4w>>9)&0x7FC), the sibling of the SXY index in the low
             * field. Substituting c2 would force every quad's 2nd
             * tri (v1,v3,v2) and every (v3,v1,v2) tri to colours (c1,c2,c2)/(c2,..),
             * a broken gouraud gradient that renders as hard facets on quad
             * faces. (c3 computed with c0-c2 above.) */

            /* Tiledef material from the cache (locals: the spec env path below
             * overwrites UV/tpage for reflective faces). */
            int tpx=F->tpx, tpy=F->tpy, clx=F->clx, cly=F->cly, bpp=F->bpp, abr=F->abr;
            /* MOD-TEXTURE faces on a heated moby: mirror the .s flame-heat
             * red-shift (.L80021FE0) on the modulating colours. The stock
             * metal chest's own face flavors get it via the env/highlight
             * branches below, which a replacement model's mod-textured
             * faces never enter - a flamed spyro3-chests vase stayed cold.
             * heat is the per-record moby snapshot (m_SpecularMetalColor
             * byte2); 0 for never-heated mobys -> no-op. */
            if (bpp == 32 && R->heat) {
                int heat = (int)R->heat;
                uint32_t* cc[4] = {&c0, &c1, &c2, &c3};
                for (int q = 0; q < 4; q++) {
                    uint32_t c = *cc[q];
                    int r=(int)(c&0xFF)+heat, g=(int)((c>>8)&0xFF)-heat, b=(int)((c>>16)&0xFF)-heat;
                    if (r>255) r=255; if (g<0) g=0; if (b<0) b=0;
                    *cc[q] = (uint32_t)(r | (g<<8) | (b<<16)) | (c & 0xFF000000u);
                }
            }
            /* Fog-band CLUT-row fog (the texel half of the LOD crossfade,
             * .s @8001FE04-FE08: s2 = (IR0>>9) << 22 is added to every textured
             * prim's UV word - word bits 22-30 ARE the CLUT y, so the row is
             * biased by IR0>>9 = 0..7 down an 8-row fogged-CLUT ladder; same
             * mechanism as the terrain CLUT-row texel fog). Without it the walk
             * samples the base (unfogged) row at every tier: distant HP mobys
             * get full-bright texels over faded vertex colours. Per-record and view-dependent,
             * so applied here at emit - never baked into the Tier-1 face cache. */
            int cly_fog = moby_face_fog_row(&fd, fcc);
            uint8_t tmix = moby_face_fog_mix(&fd, fcc);
            cly += cly_fog;
            int u[4]={F->u[0],F->u[1],F->u[2],F->u[3]}, v[4]={F->v[0],F->v[1],F->v[2],F->v[3]};
            /* SPECULAR per-face shading. STRIP faces (!fresh) with word0&0x4
             * get the special per-face treatment (highlight-enable @80021B64;
             * dispatcher .L80021DB4): METAL record -> env reflection (the
             * .L80021FE0 chain, per-vertex .L800222C0 atan2 + the per-moby
             * .L800226F8 TR-azimuth u-bias); non-metal -> diffuse highlight.
             * FRESH faces with &0x4 are BILLBOARD SPRITES (.L8002256C, handled
             * above with authored Tiledef). Every OTHER face keeps its baked
             * Tiledef texture + per-vertex colour. This per-face gate is
             * load-bearing: a metal moby's sombrero/eyes are regular textured
             * faces and must NOT be env-mapped -- only its at&4 strip faces
             * reflect. (Env-mapping every textured face of a metal moby would
             * replace the ordinary texture of its sombrero and eyes.) */
            proj_xy hlq[4];   /* hl decal copies (vz-nudged); see below */
            if (moby_game.spec_light && (s_cur_mat[0]=='S' || R->spec_solo) && F->hl) {
              /* The .s draws these SPECIAL faces (env reflection / diffuse
               * highlight) COPLANAR over the moby's base faces and wins by OT
               * paint order within the moby. The native scene is depth-tested
               * and material-BATCHED (draw order regroups by texture/blend), so
               * an exact-coplanar decal z-fights its base and mostly loses (the
               * armored gnorc's shield rendered its baked base texture instead
               * of the env reflection). Mirror the paint-order semantics in
               * depth: pull the decal 0.2% toward the camera (a few world units
               * at moby range - far below any real geometry separation). */
              hlq[0] = *p0; hlq[1] = *p1; hlq[2] = *p2;
              hlq[0].vz *= 0.998f; hlq[1].vz *= 0.998f; hlq[2].vz *= 0.998f;
              p0 = &hlq[0]; p1 = &hlq[1]; p2 = &hlq[2];
              if (p3) { hlq[3] = *p3; hlq[3].vz *= 0.998f; p3 = &hlq[3]; }
              if (R->metal) {
                /* ENV REFLECTION -- view-dependent spherical UV (byte-exact <=1
                 * texel vs prims); tpage/clut from the static metal Tiledef. */
                specular_env_uv(p0->ex,p0->ey,p0->ez, &u[0],&v[0]);
                specular_env_uv(p1->ex,p1->ey,p1->ez, &u[1],&v[1]);
                specular_env_uv(p2->ex,p2->ey,p2->ez, &u[2],&v[2]);
                if (p3) specular_env_uv(p3->ex,p3->ey,p3->ez, &u[3],&v[3]);
                /* SEAM UNWRAP: u is azimuth mod 128; pull u[1..3] within +-64 of
                 * u[0] (then keep >=0) so a seam face isn't interpolated backwards
                 * (mirrors the .s shifting a whole face by +-128). */
                int nvtx = p3 ? 4 : 3, umin = u[0];
                for (int q = 1; q < nvtx; q++) {
                    while (u[q] - u[0] >  64) u[q] -= 128;
                    while (u[q] - u[0] < -64) u[q] += 128;
                    if (u[q] < umin) umin = u[q];
                }
                if (umin < 0) for (int q = 0; q < nvtx; q++) u[q] += 128;
                textured = 1;   /* env faces sample the reflection texture */
                tpx=s_env_tpx; tpy=s_env_tpy; clx=s_env_clx; cly=s_env_cly; bpp=s_env_bpp;
                tmix = 0;   /* the reflection CLUT carries no fog row */
                /* FLAMED-METAL heat red-shift (.L80021FE0, gated heat!=0): the .s
                 * red-shifts the env face's modulating colour by a2=(ms>>16)&0xFF
                 * (R+=a2, G-=a2, B-=a2, clamp) before the env UV -- so a heated
                 * METAL object reddens, the same as the non-metal chest. cbase (the
                 * base) is already correct; only this shift was missing. No-op for
                 * cold metal (heat==0). */
                int heat=(int)((R->mscol>>16)&0xFFu);
                if (heat) {
                    uint32_t* cc[4]={&c0,&c1,&c2,&c3};
                    for (int q=0;q<4;q++){ uint32_t c=*cc[q];
                        int r=(int)(c&0xFF)+heat, g=(int)((c>>8)&0xFF)-heat, b=(int)((c>>16)&0xFF)-heat;
                        if(r>255)r=255; if(g<0)g=0; if(b<0)b=0;
                        *cc[q]=(uint32_t)(r|(g<<8)|(b<<16)); }
                }
              } else {
                /* DIFFUSE HIGHLIGHT (.L80021DB4) -- per-face flat colour that
                 * REPLACES the base. The .s builds it from the moby's
                 * m_SpecularMetalColor as the GTE back-colour (BK) plus a gray
                 * light contribution scaled by the spec coefficient (byte2) and the
                 * facing. BK carries the per-moby TINT, so a flamed metal chest
                 * (mscol -> red) reddens ("gets hot but doesn't break"); the cold
                 * chest's BK is gray -> silver. The gray *base* colour does not
                 * carry the tint. BK channels (the .s packing @80021E38): RBK=(ms>>10)&0xFF,
                 * GBK=(ms>>4)&0xFF, BBK=(ms<<2)&0xFF; spec = byte2 = (ms>>16)&0xFF.
                 * facing add: ~ -1.19 * spec * NL (gray), NL =
                 * view face-normal . spec-light (D_800770C8+0xC). */
                uint32_t ms=R->mscol;
                int RBK=(int)((ms>>10)&0xFFu), GBK=(int)((ms>>4)&0xFFu), BBK=(int)((ms<<2)&0xFFu);
                int spec=(int)((ms>>16)&0xFFu);
                float e1x=p1->ex-p0->ex, e1y=p1->ey-p0->ey, e1z=p1->ez-p0->ez;
                float e2x=p2->ex-p0->ex, e2y=p2->ey-p0->ey, e2z=p2->ez-p0->ez;
                float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
                float nl=sqrtf(nx*nx+ny*ny+nz*nz);
                if (nl > 0.001f) {
                    float NL=(nx*s_specL[0]+ny*s_specL[1]+nz*s_specL[2])/(nl*s_specLlen);
                    int add=(int)(-1.19f * (float)spec * NL);
                    int rr=RBK+add, gg=GBK+add, bb2=BBK+add;
                    if(rr<0)rr=0; if(rr>255)rr=255; if(gg<0)gg=0; if(gg>255)gg=255;
                    if(bb2<0)bb2=0; if(bb2>255)bb2=255;
                    c0=c1=c2=c3=(uint32_t)(rr|(gg<<8)|(bb2<<16));   /* flat per face */
                }
              }
            }
            /* The game's own per-face special material, where it has one that
             * the record selects rather than the face (the treatment reads the
             * face's normal and the record's material word). Like the branch
             * above it, the draw lays these faces coplanar over their base
             * faces and wins by ordering-table position; under a true Z-buffer
             * the decal needs the same pull toward the camera to win. */
            /* 4 tags a face whose flag selects the draw's second dispatcher,
             * which this walk does not carry: the face keeps the ordinary
             * emit, and the tag lets a material or colour compare separate
             * those faces from the ones the walk computes. */
            s_emit_special = (uint8_t)(F->hl == 2u ? 4u : 0u);
            s_bit4_faces += (F->hl == 2u);
            special_census_face(F, R->mscol, s_cur_moby_key, p0, p1, p2);
            int special_tex = 0;
            if (moby_game.special_face && F->hl) {
                uint32_t cc[4] = { c0, c1, c2, c3 };
                int tex2 = textured;
                moby_face_tex_t stex; memset(&stex, 0, sizeof stex);
                const proj_xy* pv4[4] = { p0, p1, p2, p3 ? p3 : p2 };
                float ev[4][3];
                for (int q = 0; q < 4; q++) {
                    ev[q][0] = pv4[q]->ex; ev[q][1] = pv4[q]->ey; ev[q][2] = pv4[q]->ez;
                }
                int applied = moby_game.special_face(cpu, R->mscol, R->M, R->moby,
                                                     R->addr, F, cc, ev, &tex2, &stex);
                if (applied && stex.set) {
                    /* The treatment's substitute page and CLUT, in place of the
                     * face's own. The draw biases the substitute's CLUT row by
                     * the same per-record fog step it adds to an ordinary face's
                     * texture word, so the substitute takes it too. */
                    tpx = stex.tpx; tpy = stex.tpy;
                    clx = stex.clx; cly = stex.cly + cly_fog;
                    bpp = stex.bpp; abr = stex.abr;
                    special_tex = 1;
                    if (stex.uv_set)
                        for (int q = 0; q < 4; q++) { u[q] = stex.u[q]; v[q] = stex.v[q]; }
                }
                if (applied) {
                    s_special_faces[applied - 1]++;
                    s_emit_special = (uint8_t)applied;
                    c0 = cc[0]; c1 = cc[1]; c2 = cc[2]; c3 = cc[3];
                    textured = tex2;
                    hlq[0] = *p0; hlq[1] = *p1; hlq[2] = *p2;
                    hlq[0].vz *= 0.998f; hlq[1].vz *= 0.998f; hlq[2].vz *= 0.998f;
                    p0 = &hlq[0]; p1 = &hlq[1]; p2 = &hlq[2];
                    if (p3) { hlq[3] = *p3; hlq[3].vz *= 0.998f; p3 = &hlq[3]; }
                }
            }

            /* Semi-transparency (ABE). The .s moby renderer (r_moby.s F798 emit
             * paths .L80020530 / .L8002066C) builds each GPU command word as
             *   code | RGB | ((w1 & 1) << 25)
             * i.e. bit 25 (the GPU ABE / semi-transparency-enable bit) comes from
             * bit0 of the per-face colour-index word w1 (fp+4) - deliberately the
             * bit the &0x7FC colour-index masks drop. When ABE is set the blend
             * MODE is the texpage ABR field (bits 5-6); untextured ABE faces have
             * no texpage of their own so they take the GPU's default ABR 0 (50%).
             * Mapping mirrors particle_native: 0=ABR0(B/2+F/2), 1=ABR1(add),
             * 2=ABR2(subtract), 3=ABR3(B+F/4 -> additive). */
            int blend = (F->blendbit && !special_tex) ? moby_abr_blend(abr) : 0;

            /* OPENPETE_MOBY_FACEDUMP=<moby address, hex>: every face of that
             * record as the walk emits it - topology, the two flag bits that
             * leave the ordinary emit, and the material words. Pairs with the
             * captured prims by the same index triple the emit keys on. */
            {
                static long s_fd = -1;
                if (s_fd < 0) { const char* e = getenv("OPENPETE_MOBY_FACEDUMP");
                    s_fd = e ? strtol(e, NULL, 16) : 0; }
                if (s_fd && (uint32_t)s_fd == s_cur_moby_key)
                    fprintf(stderr, "  [fdump vbl=%u] m=%08X mat=%08X i=%u,%u,%u,%u fresh=%d hl=%d "
                            "tex=%d tp=(%d,%d) clut=(%d,%d) bpp=%d abr=%d abe=%d\n",
                            openpete_frame, s_cur_moby_key, R->mscol, i0, i1, i2, i3,
                            (int)F->fresh, (int)F->hl, textured, tpx, tpy, clx, cly,
                            bpp, (int)abr, (int)F->blendbit);
            }

            uint64_t rt0 = rt_now();
            int ot_bk = moby_face_otkey(&s_proj[base], F, p0, p1, p2, p3);
            rt_add(RT_KEY, rt0, 1);
            if (emit_a) {
                s_emit_otk = moby_otk_tri(ot_bk, MOBY_OTK_TRI_A);
                s_emit_fkey = PACK_FKEY(i0,i1,i2);
                s_emit_tmix = tmix;
                emit_wtri(p0,p1,p2, c0,c1,c2, u[0],v[0],u[1],v[1],u[2],v[2],
                          textured,tpx,tpy,clx,cly,bpp,blend);
            }
            if (emit_b) {
                int32_t otk_b = moby_otk_second(ot_bk, emit_a);
                if (b_is_v3tri) {        /* tri-2nd (v3,v1,v2) */
                    s_emit_otk = moby_otk_tri(otk_b, MOBY_OTK_TRI_V3);
                    s_emit_fkey = PACK_FKEY(i3,i1,i2);
                    s_emit_tmix = tmix;
                    emit_wtri(p3,p1,p2, c3,c1,c2, u[3],v[3],u[1],v[1],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
                } else {                 /* quad 2nd half (v1,v3,v2) */
                    s_emit_otk = moby_otk_tri(otk_b, MOBY_OTK_TRI_QUAD_B);
                    s_emit_fkey = PACK_FKEY(i1,i3,i2);
                    s_emit_tmix = tmix;
                    emit_wtri(p1,p3,p2, c1,c3,c2, u[1],v[1],u[3],v[3],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
                }
            }
        }
        moby_coincident_pull(t_out, rec_tri0);
    }
}


/* Engagement census: walks that fanned out against walks that stayed serial. */
static long s_walk_par, s_walk_serial;

/** @brief Whether a walk instrument that needs a serial walk is on.
 *
 * True when OPENPETE_MOBY_CLASSDBG, OPENPETE_MOBY_FADEDBG or
 * OPENPETE_MOBY_CLERP_DBG is set; latched on the first call. Their censuses
 * and dumps are whole-walk statics with serial semantics, and several latch
 * mid-walk.
 * @return 1 when one is set, else 0. */
static int walk_dbg_env(void) {
    /* Any active walk instrument forces serial: their censuses and dumps are
     * whole-walk statics with serial semantics, and several latch mid-walk. */
    static int v = -1;
    if (v < 0)
        v = (getenv("OPENPETE_MOBY_CLASSDBG") || getenv("OPENPETE_MOBY_FADEDBG") ||
             getenv("OPENPETE_MOBY_CLERP_DBG")) ? 1 : 0;
    return v;
}

/** @brief Resolve every record's face sets and decide whether the walk may fan out.
 *
 * The face walk writes guest RAM: a keyframe with a patch list has its
 * high-poly faceblock patched in place, decoded and restored, which is how
 * effect models animate structure per keyframe. The patch, decode and
 * restore are synchronous so nothing observes the transient; per-record
 * parallelism would break that, since the buffer holding the originals is one
 * shared array and a torn restore writes wrong words back to the guest.
 *
 * Resolving every record here, in one pass under fcache_pass_begin, also
 * removes the two cache hazards: an eviction frees a face array a concurrent
 * reader may hold, and the 32-slot patched-decode ring inserts round-robin,
 * so a later insertion can clobber a pointer already handed out. The walk
 * fans out only when the prologue inserted into neither cache; otherwise it
 * stays serial for that present.
 *
 * The walk also stays serial for the first call (so lazy getenv latches
 * resolve on one thread), under OPENPETE_MOBY_WALK_SERIAL, below
 * OPENPETE_MOBY_FANOUT_MIN faces (default MN_FANOUT_MIN_FACES, where the
 * dispatch cost exceeds the saving), with one worker or fewer than two
 * records, and while a walk instrument is on (walk_dbg_env).
 * @param cpu The guest CPU state.
 * @param is_spec Nonzero for the specular pass, which skips mobys the regular
 *        pass already fed.
 * @return 1 when the walk must stay serial, 0 when it may fan out. */
static int walk_prologue(CPUState* cpu, int is_spec) {
    long fc_miss0 = s_fc_miss, fp_miss0 = s_fp_miss;
    long faces = 0;
    /* Every pointer resolved below is read by the walk after this returns, so
     * the whole loop is one resolve pass and nothing it hands out is evicted
     * inside it. */
    fcache_pass_begin();
    for (int r = 0; r < s_nrecs; r++) {
        rec_meta* R = &s_recs[r];
        s_pre_FC[r] = NULL; s_pre_FCp[r] = NULL; s_pre_clra[r] = 0.f;
        s_pre_rec[r].worker = 0; s_pre_rec[r].start = 0; s_pre_rec[r].count = 0;
        uint32_t mob = R->moby;
        /* The same skips the walk applies, so no record is decoded here that the
         * walk would not have decoded itself. */
        if (is_spec) {
            int dup = 0;
            for (int k = 0; k < s_reg_moby_n; k++) if (s_reg_moby[k] == mob) { dup = 1; break; }
            if (dup) continue;
        }
        if (mob && moby_run_once_hidden(cpu, mob, (unsigned)cpu->read_half(mob + game_layout.moby.class_off))) continue;
        if (R->faceblock < 0x80000000u || R->proj_n <= 0) continue;
        const fcache_t* FC = moby_faces_for_record(cpu, R);
        s_pre_FC[r] = FC;
        if (!FC) continue;
        faces += FC->nfaces;
        const fcache_t* FCp = moby_clerp_prev_faces(cpu, R, FC);
        if (FCp) { s_pre_FCp[r] = FCp; s_pre_clra[r] = R->clr_alpha; s_clerp_recs++; }
    }
    int nw = openpete_workpool_threads();
    if (nw > MN_MAX_WORKERS) nw = MN_MAX_WORKERS;
    /* A cache insertion this present may have evicted a pointer already stored
     * above, so the resolved set is only trustworthy when neither cache moved. */
    if (s_fc_miss != fc_miss0 || s_fp_miss != fp_miss0) return 1;
    /* The first walk runs serial so the body's own lazy getenv latches resolve
     * on one thread; every later walk finds them already set. */
    static int first = 1;
    if (first) { first = 0; return 1; }
    /* OPENPETE_MOBY_WALK_SERIAL=1 forces this walk serial while leaving the
     * worker pool up for every other caller. --extract-threads 1 cannot do
     * that: it degrades the pool itself, so it retires the terrain sector walk
     * in the same breath and no A/B built on it can say which fan-out a
     * measurement belongs to. */
    static int s_ser = -1;
    if (s_ser < 0) { const char* e = getenv("OPENPETE_MOBY_WALK_SERIAL");
                     s_ser = (e && *e && *e != '0'); }
    if (s_ser) return 1;
    if (tier_dbg_on()) { s_walk_faces += faces; s_walk_runs++; }
    /* Below a work bound the dispatch costs more than it saves. The pool's
     * floor is a semaphore wake plus a join that waits for every helper it
     * signalled - including one that finds no work left - so a run costs on the
     * order of 20 us however small it is, while the walk itself runs at ~0.1 us
     * per face (307 faces / 30.6 us and 615 / 66.4 us, two scenes a factor of
     * two apart). Break-even is therefore a few hundred faces, and below it the
     * walk measured slower parallel than serial.
     *
     * The bound needs no per-machine calibration, because being wrong about it
     * is wildly asymmetric: fanning out a scene that was too small costs a few
     * microseconds, while failing to fan out a heavy one costs more than a
     * millisecond - a whole-level vista walks ~17600 faces, 1852 us serial
     * against 444 parallel. Only the far side of the bound is expensive, so any
     * bound in this neighbourhood is safe wherever it runs. */
    /* Calibration override: re-derive the bound on other hardware without a
     * rebuild (sweep it against the walk cost the tier census reports). */
    static int s_min = -1;
    if (s_min < 0) { const char* e = getenv("OPENPETE_MOBY_FANOUT_MIN");
                     s_min = (e && *e) ? atoi(e) : MN_FANOUT_MIN_FACES; }
    if (faces < s_min) return 1;
    if (nw <= 1 || s_nrecs < 2 || walk_dbg_env()) return 1;
    return 0;
}

/* Per-worker emit slabs, allocated on first parallel use. A single worker can
 * legitimately draw every triangle of a scene, so each slab carries the full
 * global capacity. */
static mn_out s_wout[MN_MAX_WORKERS];

/** @brief Allocate the first nw workers' emit slabs on first use.
 *
 * A slab that already has its triangle array is taken as complete. Each new
 * slab holds MAX_NTRI entries of every parallel array.
 * @param nw The worker count.
 * @return 1 when every slab exists, 0 when an allocation failed. */
static int walk_slabs_ready(int nw) {
    for (int w = 0; w < nw; w++) {
        mn_out* O = &s_wout[w];
        if (O->tri) continue;
        O->tri    = (ntri_t*)  malloc(sizeof(ntri_t)  * MAX_NTRI);
        O->fkey   = (uint32_t*)malloc(sizeof(uint32_t)* MAX_NTRI);
        O->moby   = (uint32_t*)malloc(sizeof(uint32_t)* MAX_NTRI);
        O->cls    = (uint16_t*)malloc(sizeof(uint16_t)* MAX_NTRI);
        O->ui     = (uint8_t*) malloc(MAX_NTRI);
        O->front  = (uint8_t*) malloc(MAX_NTRI);
        O->mangle = (uint8_t*) malloc(MAX_NTRI);
        O->dofs   = (int8_t*)  malloc(MAX_NTRI);
        O->special= (uint8_t*) malloc(MAX_NTRI);
        O->otk    = (int32_t*) malloc(sizeof(int32_t) * MAX_NTRI);
        O->isxy   = (int16_t (*)[6])malloc(sizeof(int16_t[6]) * MAX_NTRI);
        O->cap = MAX_NTRI;
        if (!O->tri || !O->fkey || !O->moby || !O->cls || !O->ui || !O->front ||
            !O->mangle || !O->dofs || !O->special || !O->otk || !O->isxy) return 0;
    }
    return 1;
}

typedef struct { CPUState* cpu; int is_spec; } mn_job;

/** @brief Work-pool job: walk one record into the worker's slab and note its run.
 *
 * Points the thread's t_out and t_diag at the worker's slab and diagnostics,
 * runs walk_records for the record, and stores the worker, the slab offset
 * and the emitted count in s_pre_rec[item].
 * @param item The record index.
 * @param worker The pool worker, 0 for the calling thread.
 * @param arg The mn_job. */
static void walk_record_job(int item, int worker, void* arg) {
    mn_job* J = (mn_job*)arg;
    mn_out* O = &s_wout[worker];
    t_out  = O;
    t_diag = &s_wdiag[worker];
    int start = O->n;
    walk_records(J->cpu, J->is_spec, item, item + 1);
    s_pre_rec[item].worker = worker;
    s_pre_rec[item].start  = start;
    s_pre_rec[item].count  = O->n - start;
}

/** @brief Walk every record across the worker pool and merge the runs in record order.
 *
 * Falls back to a serial walk when a slab cannot be allocated. Otherwise
 * empties the slabs, runs walk_record_job per record, and copies each
 * record's run into the s_wtri arrays in record order, truncating at
 * MAX_NTRI, so the stream matches the serial walk. Sums the per-worker
 * diagnostics into worker 0's. With OPENPETE_MOBY_TIER_DBG on, the dispatch
 * and merge microseconds go to s_us_fanout and s_us_merge.
 * @param cpu The guest CPU state.
 * @param is_spec Nonzero for the specular pass. */
static void walk_parallel(CPUState* cpu, int is_spec) {
    int nw = openpete_workpool_threads();
    if (nw > MN_MAX_WORKERS) nw = MN_MAX_WORKERS;
    if (!walk_slabs_ready(nw)) { walk_records(cpu, is_spec, 0, s_nrecs); return; }
    for (int w = 0; w < nw; w++) s_wout[w].n = 0;
    mn_job J = { cpu, is_spec };
    uint64_t t_disp = tier_dbg_on() ? openpete_phase_now_us() : 0;
    openpete_workpool_run(s_nrecs, walk_record_job, &J);
    if (t_disp) s_us_fanout += (long)(openpete_phase_now_us() - t_disp);
    uint64_t t_mrg = tier_dbg_on() ? openpete_phase_now_us() : 0;
    t_out = &s_gout; t_diag = &s_wdiag[0];   /* worker 0 is this thread */
    /* Ordered merge: worker runs land in s_wtri in RECORD order, so the stream
     * and every consumer downstream of it match the serial walk. */
    s_gout.n = 0;
    for (int r = 0; r < s_nrecs; r++) {
        int n = s_pre_rec[r].count;
        if (n <= 0) continue;
        const mn_out* O = &s_wout[s_pre_rec[r].worker];
        int st = s_pre_rec[r].start;
        if (s_gout.n + n > MAX_NTRI) n = MAX_NTRI - s_gout.n;
        if (n <= 0) break;
        memcpy(&s_wtri[s_gout.n],        &O->tri[st],    (size_t)n * sizeof(ntri_t));
        memcpy(&s_wtri_fkey[s_gout.n],   &O->fkey[st],   (size_t)n * sizeof(uint32_t));
        memcpy(&s_wtri_moby[s_gout.n],   &O->moby[st],   (size_t)n * sizeof(uint32_t));
        memcpy(&s_wtri_cls[s_gout.n],    &O->cls[st],    (size_t)n * sizeof(uint16_t));
        memcpy(&s_wtri_ui[s_gout.n],     &O->ui[st],     (size_t)n);
        memcpy(&s_wtri_front[s_gout.n],  &O->front[st],  (size_t)n);
        memcpy(&s_wtri_mangle[s_gout.n], &O->mangle[st], (size_t)n);
        memcpy(&s_wtri_dofs[s_gout.n],   &O->dofs[st],   (size_t)n);
        memcpy(&s_wtri_special[s_gout.n],&O->special[st],(size_t)n);
        memcpy(&s_wtri_otk[s_gout.n],    &O->otk[st],    (size_t)n * sizeof(int32_t));
        memcpy(&s_wtri_isxy[s_gout.n],   &O->isxy[st],   (size_t)n * sizeof(int16_t[6]));
        s_gout.n += n;
    }
    if (t_mrg) s_us_merge += (long)(openpete_phase_now_us() - t_mrg);
    for (int w = 1; w < nw; w++) {   /* diagnostics roll-up */
        const mn_walkdiag* D = &s_wdiag[w];
        s_wdiag[0].faces += D->faces; s_wdiag[0].emitted += D->emitted;
        s_wdiag[0].oor += D->oor;     s_wdiag[0].skip += D->skip;
        s_wdiag[0].sprite += D->sprite; s_wdiag[0].strip += D->strip;
        s_wdiag[0].fresh_g += D->fresh_g; s_wdiag[0].fresh_t += D->fresh_t;
        s_wdiag[0].quad += D->quad;   s_wdiag[0].nearstraddle += D->nearstraddle;
        s_wdiag[0].align_lo += D->align_lo; s_wdiag[0].align_hi += D->align_hi;
        s_wdiag[0].desync += D->desync;
        s_wdiag[0].coinc_pairs += D->coinc_pairs; s_wdiag[0].coinc_pulls += D->coinc_pulls;
    }
}

/** @brief Walk every queued record's faces into s_wtri[], serially or across the worker pool.
 *
 * Refreshes the specular environment state (specular_env_load), empties s_gout
 * and the per-worker diagnostics, and runs walk_prologue, which resolves each
 * record's face cache and decides whether the walk may fan out. A serial walk
 * runs walk_records over all records; otherwise walk_parallel does, and merges
 * the worker runs in record order. Counts the choice into s_walk_serial or
 * s_walk_par. The pass is the specular one when s_cur_mat begins with "SP".
 * @param cpu The guest CPU state.
 * @return The triangle count, s_gout.n. */
static int build_walk_tris_inner(CPUState* cpu) {
    specular_env_load(cpu);   /* refresh metalMatrix + metal Tiledef (cheap; SPEC env-UV) */
    s_gout.n = 0;
    memset(s_wdiag, 0, sizeof(s_wdiag));
    int is_spec = (s_cur_mat && s_cur_mat[0]=='S' && s_cur_mat[1]=='P');
    int serial = walk_prologue(cpu, is_spec);
    if (serial) { s_walk_serial++; walk_records(cpu, is_spec, 0, s_nrecs); return s_gout.n; }
    s_walk_par++;
    walk_parallel(cpu, is_spec);
    return s_gout.n;
}

/* One triangle of a captured prim: screen vertices, colours, UVs and the
 * material words. */
typedef struct { float x[3], y[3]; uint32_t rgb[3]; int u[3], v[3]; int tex, clut, tpage; } ptri;
#define MAX_PTRI (MAX_PRIMS*2)
static ptri s_ptri[MAX_PTRI]; static int s_ptri_n;
/** @brief Split every captured prim of s_gte into triangles in s_ptri[].
 *
 * A prim with three or more vertices yields the triangle (0,1,2); a quad also
 * yields (1,3,2), its vertices being stored TL, TR, BL, BR. Each triangle keeps
 * the prim's screen positions, colours, UVs, texture flag, CLUT and texture
 * page. A prim with fewer than three vertices is skipped, and the list stops at
 * MAX_PTRI. */
static void build_prim_tris(void) {
    s_ptri_n = 0;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        if (P->nv < 3 || s_ptri_n >= MAX_PTRI) continue;
        ptri* a = &s_ptri[s_ptri_n++];
        for (int k=0;k<3;k++){a->x[k]=P->x[k];a->y[k]=P->y[k];a->rgb[k]=P->rgb[k];a->u[k]=P->u[k];a->v[k]=P->v[k];}
        a->tex=P->textured; a->clut=P->clut; a->tpage=P->tpage;
        if (P->nv==4 && s_ptri_n<MAX_PTRI) {            /* quad TL,TR,BL,BR -> (1,3,2) */
            ptri* b=&s_ptri[s_ptri_n++]; int idx[3]={1,3,2};
            for (int k=0;k<3;k++){int j=idx[k];b->x[k]=P->x[j];b->y[k]=P->y[j];b->rgb[k]=P->rgb[j];b->u[k]=P->u[j];b->v[k]=P->v[j];}
            b->tex=P->textured; b->clut=P->clut; b->tpage=P->tpage;
        }
    }
}
/** @brief The screen distance between a walk triangle and a captured prim triangle under their best vertex correspondence.
 *
 * The minimum, over the six vertex permutations, of the largest per-vertex
 * distance; the result does not depend on either triangle's vertex order.
 * @param w The walk triangle.
 * @param p The prim triangle.
 * @return The distance in screen px. */
static double tri_match_cost(const ntri_t* w, const ptri* p) {
    static const int perm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    double bestmax=1e18;
    for (int pi=0;pi<6;pi++){
        double mx=0;
        for (int k=0;k<3;k++){
            double dx=w->v[k].sx-p->x[perm[pi][k]], dy=w->v[k].sy-p->y[perm[pi][k]];
            double d=sqrt(dx*dx+dy*dy); if(d>mx)mx=d;
        }
        if(mx<bestmax)bestmax=mx;
    }
    return bestmax;
}
#define BOX_PASS_MAX 4
static const char* s_pb_mats[BOX_PASS_MAX]; static int s_pb_nmat;
static const char* s_wb_mats[BOX_PASS_MAX]; static int s_wb_nmat;
/** @brief Whether the current producer pass has already dumped, recording the pass when it has not.
 *
 * The pass is identified by the material name pointer s_cur_mat. Each distinct
 * pointer is remembered once, so a box dump prints once per pass (regular,
 * specular, shaded) instead of once overall. At most BOX_PASS_MAX names are
 * remembered; a pass past that reports not latched on every call.
 * @param seen The remembered material names, BOX_PASS_MAX slots.
 * @param n The count of names in `seen`, updated when a name is added.
 * @return 1 when s_cur_mat is already in `seen`, else 0. */
static int box_pass_latched(const char** seen, int* n) {
    for (int i = 0; i < *n; i++) if (seen[i] == s_cur_mat) return 1;
    if (*n < BOX_PASS_MAX) seen[(*n)++] = s_cur_mat;
    return 0;
}

/** @brief Match the walked triangles to the captured prims and print the topology and colour A/B to stderr.
 *
 * Builds the prim triangles (build_prim_tris), then assigns walk triangles to
 * prim triangles one to one: each pair whose centroids lie within 32 px and
 * whose tri_match_cost is at most 4 px is ranked by cost, and pairs are taken
 * in that order, each side once. Reports precision (walk triangles matched) and
 * recall (prim triangles covered), both over all triangles and over those whose
 * centroid lies in the 512 x 240 display rect, the mean match distance, the
 * nclip sign split of matched and unmatched triangles, per-vertex colour and UV
 * agreement, walk triangles that duplicate another walk triangle, the walk
 * shape counters of s_wdiag[0], and the per-channel colour error of the
 * special-material faces against their prim.
 *
 * OPENPETE_MOBY_PRIMBOX and OPENPETE_MOBY_WALKBOX dump the prims and the walk
 * triangles with a vertex in a screen box, once per pass (box_pass_latched).
 * The first call also prints a few on-screen misses and uncovered prims. Reads
 * s_gout, s_wtri and s_gte; the OPENPETE_MOBY_VERIFY path calls it after
 * build_walk_tris or build_shaded_walk_tris. */
static void compare_walk_vs_prims(void) {
    build_prim_tris();
    static int s_cmp_dumped = 0; int s_cmp_dump = !s_cmp_dumped; s_cmp_dumped = 1;
    { /* OPENPETE_MOBY_PRIMBOX="x0,y0,x1,y1": dump every captured .s prim with a
       * vertex in the PSX-screen box (per-vertex xy+rgb) - the position-matched
       * ground truth for the [fface] walk dump. One frame only. */
        static int s_pb = -1; static int px0,py0,px1,py1;
        if (s_pb < 0) { const char* e = getenv("OPENPETE_MOBY_PRIMBOX");
            s_pb = 0;
            if (e && sscanf(e, "%d,%d,%d,%d", &px0,&py0,&px1,&py1) == 4) s_pb = 1; }
        int s_pb_done = box_pass_latched(s_pb_mats, &s_pb_nmat);
        if (s_pb == 1 && !s_pb_done) {
            for (int pi = 0; pi < s_gte_n; pi++) {
                int in = 0;
                for (int k = 0; k < s_gte[pi].nv; k++)
                    if (s_gte[pi].x[k] >= px0 && s_gte[pi].x[k] <= px1 &&
                        s_gte[pi].y[k] >= py0 && s_gte[pi].y[k] <= py1) in = 1;
                if (!in) continue;
                fprintf(stderr, "  [pbox vbl=%u] code=%02X tex=%d clut=%04X tpage=%04X", openpete_frame,
                        s_gte[pi].code, s_gte[pi].textured, s_gte[pi].clut & 0xFFFFu, s_gte[pi].tpage & 0xFFFFu);
                for (int k = 0; k < s_gte[pi].nv; k++)
                    fprintf(stderr, " (%.0f,%.0f)%06X[%d,%d]", s_gte[pi].x[k], s_gte[pi].y[k],
                            s_gte[pi].rgb[k] & 0xFFFFFFu, s_gte[pi].u[k], s_gte[pi].v[k]);
                fprintf(stderr, "\n");
            }
        }
    }
    { /* OPENPETE_MOBY_WALKBOX="x0,y0,x1,y1": the native counterpart of
       * [pbox] - every walk triangle with a vertex in the same PSX-screen box,
       * with the material words the feeder emitted. Diffing a [wbox] line
       * against the [pbox] line at the same position names which word (colour,
       * texture page, CLUT, blend) the two sides disagree on. Latched per
       * pass, like [pbox]. */
        static int s_wb = -1; static int wx0,wy0,wx1,wy1;
        if (s_wb < 0) { const char* e = getenv("OPENPETE_MOBY_WALKBOX");
            s_wb = 0;
            if (e && sscanf(e, "%d,%d,%d,%d", &wx0,&wy0,&wx1,&wy1) == 4) s_wb = 1; }
        int wb_done = box_pass_latched(s_wb_mats, &s_wb_nmat);
        if (s_wb == 1 && !wb_done) {
            for (int wi = 0; wi < s_gout.n; wi++) {
                const ntri_t* w = &s_wtri[wi];
                int in = 0;
                for (int k = 0; k < 3; k++)
                    if (w->v[k].sx >= wx0 && w->v[k].sx <= wx1 &&
                        w->v[k].sy >= wy0 && w->v[k].sy <= wy1) in = 1;
                if (!in) continue;
                fprintf(stderr, "  [wbox vbl=%u] m=%08X k=%08X tex=%d tp=(%d,%d) clut=(%d,%d) bpp=%d blend=%d spec=%u",
                        openpete_frame, s_wtri_moby[wi], s_wtri_fkey[wi], w->textured,
                        w->tpage_x, w->tpage_y, w->clut_x, w->clut_y, w->bpp, w->blend,
                        (unsigned)s_wtri_special[wi]);
                for (int k = 0; k < 3; k++)
                    fprintf(stderr, " (%.0f,%.0f)%06X[%d,%d]{%.1f,%.1f,%.1f}",
                            w->v[k].sx, w->v[k].sy, w->v[k].rgb & 0xFFFFFFu,
                            w->v[k].u, w->v[k].v, w->v[k].ex, w->v[k].ey, w->v[k].ez);
                fprintf(stderr, "\n");
            }
        }
    }
    int wn=s_gout.n, pn=s_ptri_n;
    int prim_quad=0, prim_tri=0;   /* how many GPU quads vs tris the .s actually emitted */
    for(int g=0;g<s_gte_n;g++){ if(s_gte[g].nv==4)prim_quad++; else if(s_gte[g].nv==3)prim_tri++; }
    int wmatch=0, neg_m=0, pos_m=0, neg_u=0, pos_u=0;
    double wcost_sum=0.0;   /* mean of the matched tris' worst-vertex distance */
    int mt_p=0,mt_n=0,mf_p=0,mf_n=0;  /* matched nclip split: textured/flat (winding rule) */
    int colmatch=0, coln=0, uvmatch=0, uvn=0; double wluma_sum=0, pluma_sum=0;
    /* Per-channel colour error of the special-material faces alone, over the
     * vertex pairs the geometric match made: their colour is computed by the
     * feeder, not read from the model, so it is priced against the prim the
     * draw emitted rather than scored pass/fail with the rest. */
    long spc_n=0, spc_sum=0, spc_max=0;
    /* On-screen subset (centroid within the disp rect): the off-screen extras
     * the .s clip-rejects are scissored by panel 0 anyway, so the visually-
     * meaningful match is the on-screen one. */
    int won=0, wmatch_on=0, won_un=0;   /* on-screen walk tris / matched / unmatched-onscreen */
    #define ONSCR(cx,cy) ((cx)>=0.0&&(cx)<=512.0&&(cy)>=0.0&&(cy)<=240.0)
    char pmatched[MAX_PTRI]; if(pn>MAX_PTRI)pn=MAX_PTRI; memset(pmatched,0,pn);
    /* ONE-TO-ONE assignment, walk tri <-> prim tri. Taking each walk tri's own
     * nearest prim lets several walk tris claim one prim, which on a cluster of
     * sub-pixel triangles pairs faces that are not counterparts and then scores
     * their colours against each other. Rank every admissible pair by cost and
     * assign in that order, each side used once. */
    static int s_assign[MAX_NTRI];
    {
        typedef struct { float c; int w, p; } pair_t;
        static pair_t* pool; static int poolcap;
        int need = wn * 8; if (need < 1024) need = 1024;
        if (need > poolcap) { pool = (pair_t*)realloc(pool, sizeof(pair_t)*(size_t)need); poolcap = need; }
        int np = 0;
        for (int wi=0; wi<wn; wi++) {
            s_assign[wi] = -1;
            const ntri_t* w=&s_wtri[wi];
            double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
            for (int pi=0; pi<pn && np<poolcap; pi++) {
                double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
                double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
                if (fabs(pcx-wcx)>32.0||fabs(pcy-wcy)>32.0) continue;
                double c=tri_match_cost(w,&s_ptri[pi]);
                if (c<=4.0) { pool[np].c=(float)c; pool[np].w=wi; pool[np].p=pi; np++; }
            }
        }
        /* insertion-free ordering: the pool is small, sort by cost */
        for (int i=1;i<np;i++){ pair_t t=pool[i]; int j=i-1;
            while (j>=0 && pool[j].c>t.c) { pool[j+1]=pool[j]; j--; } pool[j+1]=t; }
        char ptaken[MAX_PTRI]; memset(ptaken,0,pn);
        for (int i=0;i<np;i++)
            if (s_assign[pool[i].w] < 0 && !ptaken[pool[i].p]) {
                s_assign[pool[i].w] = pool[i].p; ptaken[pool[i].p] = 1;
            }
    }
    for (int wi=0;wi<wn;wi++){
        ntri_t* w=&s_wtri[wi];
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0;pi<pn;pi++){
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>32.0||fabs(pcy-wcy)>32.0) continue;        /* centroid prune */
            double c=tri_match_cost(w,&s_ptri[pi]);
            if (c<best){best=c;bp=pi;}
        }
        double ncl=tri_nclip(w->v[0].sx,w->v[0].sy,w->v[1].sx,w->v[1].sy,w->v[2].sx,w->v[2].sy);
        int won_this = ONSCR(wcx,wcy);
        if (won_this) won++;
        int ap = s_assign[wi];
        if (ap >= 0) { bp = ap; best = tri_match_cost(w, &s_ptri[ap]); }
        if (ap >= 0){
            wmatch++; pmatched[bp]=1; wcost_sum += best;
            if (won_this) wmatch_on++;
            if(ncl<0)neg_m++;else pos_m++;
            if(w->textured){ if(ncl<0)mt_n++;else mt_p++; } else { if(ncl<0)mf_n++;else mf_p++; }
            /* Colour compare at POSITION-COINCIDENT vertices: the geometric match
             * is order-independent, so walk vertex 0 may correspond to any prim
             * corner - comparing rgb[0] vs rgb[0] scored a rotated correspondence
             * of the same gouraud face as a miss. Pair each walk vertex with the
             * nearest prim vertex (<=2.5px) and compare THERE; faces that still
             * differ carry genuinely wrong colours. */
            uint32_t wc=w->v[0].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[0]&0xFFFFFFu;
            {
                /* Corner pairing is only meaningful when the prim's vertices are
                 * mutually separated by more than the pairing radius - on a tiny
                 * face every walk vertex is "nearest" to every prim vertex and
                 * the verdict is noise. Skip those (counted as matches). */
                int unambiguous = 1;
                for (int pa = 0; pa < 3 && unambiguous; pa++)
                    for (int pb = pa + 1; pb < 3; pb++) {
                        double dx = s_ptri[bp].x[pa] - s_ptri[bp].x[pb];
                        double dy = s_ptri[bp].y[pa] - s_ptri[bp].y[pb];
                        if (dx*dx + dy*dy <= 3.0*3.0) { unambiguous = 0; break; }
                    }
                int all_ok = 1, paired = 0;
                if (unambiguous)
                for (int kv = 0; kv < 3; kv++) {
                    double bd = 1e9; int bk = -1;
                    for (int pk = 0; pk < 3; pk++) {
                        double dx = w->v[kv].sx - s_ptri[bp].x[pk];
                        double dy = w->v[kv].sy - s_ptri[bp].y[pk];
                        double dd = dx*dx + dy*dy;
                        if (dd < bd) { bd = dd; bk = pk; }
                    }
                    if (bd <= 2.5*2.5 && bk >= 0) {
                        paired++;
                        if (s_wtri_special[wi] && s_wtri_special[wi] < 4u) {
                            uint32_t wv = w->v[kv].rgb & 0xFFFFFFu;
                            uint32_t pv = s_ptri[bp].rgb[bk] & 0xFFFFFFu;
                            for (int ch = 0; ch < 3; ch++) {
                                long d = (long)((wv >> (ch*8)) & 0xFFu) - (long)((pv >> (ch*8)) & 0xFFu);
                                if (d < 0) d = -d;
                                spc_sum += d; spc_n++;
                                if (d > spc_max) spc_max = d;
                            }
                        }
                        if ((w->v[kv].rgb & 0xFFFFFFu) != (s_ptri[bp].rgb[bk] & 0xFFFFFFu)) {
                            all_ok = 0;
                            wc = w->v[kv].rgb & 0xFFFFFFu;
                            pc = s_ptri[bp].rgb[bk] & 0xFFFFFFu;
                        }
                    }
                }
                coln++;
                if (!unambiguous || (paired == 3 && all_ok)) colmatch++;
                else if (paired < 3) { colmatch++; /* geometric pairing failed - not colour evidence */ }
                else { /* colour-mismatch forensics: identity + values, cap via env */
                    static int ncd=0; static int cap=-1;
                    if (cap < 0) { const char* e = getenv("OPENPETE_MOBY_COLMISS_DBG"); cap = (e && *e) ? atoi(e) : 16; }
                    if (ncd++ < cap) fprintf(stderr,
                        "  [col-miss vbl=%u] m=%08X k=%08X w(%.0f,%.0f) tex=%d walk=%06X prim=%06X\n",
                        openpete_frame, s_wtri_moby[wi], s_wtri_fkey[wi],
                        w->v[0].sx, w->v[0].sy, w->textured, wc, pc);
                }
            }
            wluma_sum += (wc&0xFF)+((wc>>8)&0xFF)+((wc>>16)&0xFF);   /* brightness: is my colour darker than the .s's? */
            pluma_sum += (pc&0xFF)+((pc>>8)&0xFF)+((pc>>16)&0xFF);
            if (w->textured){ uvn++; if (w->v[0].u==s_ptri[bp].u[0] && w->v[0].v==s_ptri[bp].v[0]) uvmatch++; }
        } else {
            if(ncl<0)neg_u++;else pos_u++;
            if(won_this) won_un++;
            if (won_this && s_cmp_dump && bp>=0) {   /* show a few on-screen misses + nearest prim */
                static int nd=0;
                if (nd++ < 8) fprintf(stderr,
                    "  [miss] w(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) best=%.1f primTri(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) tex=%d\n",
                    w->v[0].sx,w->v[0].sy,w->v[1].sx,w->v[1].sy,w->v[2].sx,w->v[2].sy, best,
                    s_ptri[bp].x[0],s_ptri[bp].y[0],s_ptri[bp].x[1],s_ptri[bp].y[1],s_ptri[bp].x[2],s_ptri[bp].y[2], w->textured);
            }
        }
    }
    int pcov=0, pon=0, pcov_on=0, puncov_dump=0;
    for(int pi=0;pi<pn;pi++){
        pcov+=pmatched[pi];
        double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
        double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
        if(ONSCR(pcx,pcy)){
            pon++;
            if(pmatched[pi]) pcov_on++;
            else if(s_cmp_dump && puncov_dump++<12)   /* prims the .s drew that the walk MISSED */
                fprintf(stderr,"  [uncovered-prim] (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) tex=%d rgb=%06X\n",
                    s_ptri[pi].x[0],s_ptri[pi].y[0],s_ptri[pi].x[1],s_ptri[pi].y[1],
                    s_ptri[pi].x[2],s_ptri[pi].y[2], s_ptri[pi].tex, s_ptri[pi].rgb[0]&0xFFFFFFu);
        }
    }
    /* self-duplicate walk tris (order-independent vertex coincidence <=1px): the
     * "repeating tri" detector. A count-preserving topology substitution (emit a
     * wrong tri that still matches some prim's POINTS, miss the real one) shows up
     * here even while precision stays 100% -- the matcher is winding/dup-blind. */
    int wdup=0;
    for(int i=0;i<wn;i++) for(int j=0;j<i;j++){
        static const int pm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        double cost=1e18;
        for(int q=0;q<6;q++){ double mx=0; for(int k=0;k<3;k++){
            double dx=s_wtri[i].v[k].sx-s_wtri[j].v[pm[q][k]].sx;
            double dy=s_wtri[i].v[k].sy-s_wtri[j].v[pm[q][k]].sy;
            double d=sqrt(dx*dx+dy*dy); if(d>mx)mx=d; } if(mx<cost)cost=mx; }
        if(cost<=1.0){ wdup++; break; }
    }
    fprintf(stderr,
        "[moby-walk %s vbl=%u] recs=%d faces=%d walkTris=%d primTris=%d | "
        "ONSCREEN precision=%d/%d(%.1f%%) recall=%d/%d(%.1f%%) onscrUnmatched=%d | "
        "precision=%d/%d(%.1f%%) recall=%d/%d(%.1f%%) meanPx=%.2f | ",
        s_cur_mat, openpete_frame, s_nrecs, s_wdiag[0].faces, wn, pn,
        wmatch_on, won, won?100.0*wmatch_on/won:0.0, pcov_on, pon, pon?100.0*pcov_on/pon:0.0, won_un,
        wmatch, wn, wn?100.0*wmatch/wn:0.0, pcov, pn, pn?100.0*pcov/pn:0.0,
        wmatch?wcost_sum/wmatch:0.0);
    fprintf(stderr,
        "[moby-walk2 vbl=%u] "
        "shapes{freshG=%d freshT=%d strip=%d sprite=%d skip=%d quad=%d oor=%d nearstraddle=%d} "
        "align{lo=%d hi=%d desync=%d} | nclip matched{-=%d +=%d} unmatched{-=%d +=%d} "
        "matchedTex{-=%d +=%d} matchedFlat{-=%d +=%d} | col=%d/%d uv=%d/%d wdup=%d/%d primQ=%d primT=%d walkQ=%d luma=w%.0f/p%.0f "
        "| special_faces=%ld/%ld/%ld bit4_faces=%ld coincident=%d/%d\n",
        openpete_frame,
        s_wdiag[0].fresh_g, s_wdiag[0].fresh_t, s_wdiag[0].strip, s_wdiag[0].sprite,
        s_wdiag[0].skip, s_wdiag[0].quad, s_wdiag[0].oor, s_wdiag[0].nearstraddle,
        s_wdiag[0].align_lo, s_wdiag[0].align_hi, s_wdiag[0].desync,
        neg_m,pos_m,neg_u,pos_u, mt_n,mt_p,mf_n,mf_p, colmatch,coln, uvmatch,uvn, wdup,wn,
        prim_quad, prim_tri, s_wdiag[0].quad, coln?wluma_sum/coln:0.0, coln?pluma_sum/coln:0.0,
        s_special_faces[0], s_special_faces[1], s_special_faces[2], s_bit4_faces,
        s_wdiag[0].coinc_pairs, s_wdiag[0].coinc_pulls);
    fprintf(stderr, "[moby-walk-special vbl=%u] pairedChannels=%ld meanErr=%.2f maxErr=%ld\n",
            openpete_frame, spc_n, spc_n ? (double)spc_sum/(double)spc_n : 0.0, spc_max);
    #undef ONSCR
}

/** @brief Print the per-vertex colour A/B of the native shaded lighting against the captured shaded prims.
 *
 * A position-nearest-neighbour match is unreliable here: a level can pack
 * dozens of shaded mobys into a small region, so a vertex's nearest prim
 * point often belongs to a different moby. Each on-screen walk triangle is
 * therefore matched to a prim triangle by geometry (centroids within 16 px,
 * then tri_match_cost, accepted at 1 px or less), which keeps the comparison
 * inside one moby; the three colour pairs then take the best of the six
 * assignments by total channel error. The line tallies per shading mode
 * (gouraud, flat basic, flat metal) the exact per-vertex matches and the mean
 * per-channel error. */
static void shaded_color_ab(void) {
    build_prim_tris();
    if (s_ptri_n <= 0 || s_gout.n <= 0) return;
    static const int pm[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int n[3]={0,0,0}, exact[3]={0,0,0}; double cherr[3]={0,0,0};
    for (int wi=0; wi<s_gout.n; wi++) {
        ntri_t* w=&s_wtri[wi]; int md=s_wtri_mode[wi]; if(md>2)md=0;
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        if (!(wcx>=0&&wcx<=512&&wcy>=0&&wcy<=240)) continue;
        double best=1e18; int bp=-1;
        for (int pi=0; pi<s_ptri_n; pi++) {
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>16.0||fabs(pcy-wcy)>16.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if(c<best){best=c;bp=pi;}
        }
        if (best>1.0||bp<0) continue;   /* tight geom gate: kill cross-moby matches */
        /* best assignment of my 3 colours to the prim's 3 (min total channel err) */
        double bce=1e18; int bq=0;
        for (int q=0;q<6;q++){ double e=0; for(int k=0;k<3;k++){
            uint32_t wc=w->v[k].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[pm[q][k]]&0xFFFFFFu;
            e += abs((int)(wc&0xFF)-(int)(pc&0xFF))+abs((int)((wc>>8)&0xFF)-(int)((pc>>8)&0xFF))
               + abs((int)((wc>>16)&0xFF)-(int)((pc>>16)&0xFF)); }
            if(e<bce){bce=e;bq=q;} }
        for(int k=0;k<3;k++){
            uint32_t wc=w->v[k].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[pm[bq][k]]&0xFFFFFFu;
            n[md]++; if(wc==pc) exact[md]++;
            cherr[md] += abs((int)(wc&0xFF)-(int)(pc&0xFF))+abs((int)((wc>>8)&0xFF)-(int)((pc>>8)&0xFF))
                   + abs((int)((wc>>16)&0xFF)-(int)((pc>>16)&0xFF));
        }
    }
    int N=n[0]+n[1]+n[2], E=exact[0]+exact[1]+exact[2]; double CE=cherr[0]+cherr[1]+cherr[2];
    #define PMODE(i) n[i], n[i]?100.0*exact[i]/n[i]:0.0, n[i]?cherr[i]/(3.0*n[i]):0.0
    fprintf(stderr,"[shaded-color vbl=%u] verts=%d exact=%d(%.1f%%) meanChErr=%.2f | "
        "gouraud[v=%d %.0f%% e%.1f] flatBasic[v=%d %.0f%% e%.1f] flatMetal[v=%d %.0f%% e%.1f] nohl=%d\n",
        openpete_frame, N, E, N?100.0*E/N:0.0, N?CE/(3.0*N):0.0,
        PMODE(0), PMODE(1), PMODE(2), s_sh_nohl);
    #undef PMODE
}

/** @brief Overwrite each walk triangle's vertex colours, UVs and texture page with those of the nearest captured prim triangle.
 *
 * An instrument for the specular material: a walk triangle is matched to a
 * prim triangle by geometry (centroids within 32 px, then tri_match_cost,
 * accepted at 8 px or less), each vertex takes the colour and UV of the
 * prim's nearest vertex, and a textured prim's tpage and CLUT decode onto the
 * triangle. The walk's float geometry is kept. A walk triangle with no prim
 * nearby (a moby the game's distance cull dropped) keeps its own colour.
 * Only the OPENPETE_MOBY_VERIFY path on the specular pass calls it; the feed
 * derives colour and env-reflection UVs natively. */
static void recolor_walk_from_prims(void) {
    build_prim_tris();
    for (int wi = 0; wi < s_gout.n; wi++) {
        ntri_t* w = &s_wtri[wi];
        double wcx = (w->v[0].sx + w->v[1].sx + w->v[2].sx) / 3.0;
        double wcy = (w->v[0].sy + w->v[1].sy + w->v[2].sy) / 3.0;
        double best = 1e18; int bp = -1;
        for (int pi = 0; pi < s_ptri_n; pi++) {
            double pcx = (s_ptri[pi].x[0] + s_ptri[pi].x[1] + s_ptri[pi].x[2]) / 3.0;
            double pcy = (s_ptri[pi].y[0] + s_ptri[pi].y[1] + s_ptri[pi].y[2]) / 3.0;
            if (fabs(pcx - wcx) > 32.0 || fabs(pcy - wcy) > 32.0) continue;
            double c = tri_match_cost(w, &s_ptri[pi]);
            if (c < best) { best = c; bp = pi; }
        }
        if (bp < 0 || best > 8.0) continue;            /* no nearby prim -> keep base */
        ptri* P = &s_ptri[bp];
        for (int k = 0; k < 3; k++) {                  /* per-vertex: nearest prim vertex */
            double bd = 1e18; int bj = 0;
            for (int j = 0; j < 3; j++) {
                double dx = w->v[k].sx - P->x[j], dy = w->v[k].sy - P->y[j];
                double d = dx * dx + dy * dy; if (d < bd) { bd = d; bj = j; }
            }
            w->v[k].rgb = P->rgb[bj];                   /* env-metal colour */
            w->v[k].u = P->u[bj]; w->v[k].v = P->v[bj]; /* env-reflection UVs (the shine) */
        }
        /* texture page/CLUT: decode the prim's raw tpage/clut like the walk's Tiledef.
         * The shiny metal is an env-mapped texture; the per-face reflection picks the
         * tpage/UVs, which the static-Tiledef walk can't reproduce. */
        if (P->tex) {
            ngte_tpage t;
            ngte_decode_tpage((unsigned)P->tpage, (unsigned)P->clut, 0u, &t);
            w->textured = 1;
            w->tpage_x = t.tpx; w->tpage_y = t.tpy;
            w->bpp = t.bpp;
            w->clut_x = t.clx; w->clut_y = t.cly;
        }
    }
}

/* ---- specular env-reflection UV, native float --------------------------
 * The shiny-metal faces reflect the environment: the .s computes per-vertex UV
 * from refl = metalMatrix . (viewPos-TR) mapped to spherical coords (atan2 + the
 * D_8006D908/D_8006CBF8 tables), gated by m_SpecularMetalColor's high byte and
 * offset per-moby by the camera heading (.L800226F8/.L800222C0). metalMatrix
 * (D_800770C8+0x18) is the camera rotation with z negated (MetalBuildMatrix,
 * update-side), so refl is already CAMERA-relative -> the reflection tracks the
 * camera for free. The UV is re-derived in FLOAT as a clean spherical map; the
 * game's prim UV agrees with it in camera tracking but not exactly. */
static int   s_metalM[3][3];     /* D_800770C8+0x18, int16 1.0=4096 */
static int   s_camHead;          /* g_Camera+0x50 (camera heading word) */
/* s_specL[3]/s_specLlen forward-declared above build_walk_tris (highlight uses them) */
/** @brief Load the specular pass's per-tick inputs: the metal matrix, camera heading, specular light and metal env texture page.
 *
 * Reads the metal matrix at spec_light + 0x18 (the camera rotation with z
 * negated), the camera heading word, the specular light direction at
 * spec_light + 0xC..0x14 and its length (floored at 1), and decodes the metal
 * Tiledef (word 0 high half the CLUT, word 1 high half the tpage) into the
 * env texture fields. A game without a specular light or metal Tiledef
 * leaves them untouched.
 * @param cpu The guest CPU state, read only. */
static void specular_env_load(CPUState* cpu) {
    if (!moby_game.spec_light || !moby_game.metal_tiledef) return;   /* no specular material */
    ngte_read_matrix(cpu, moby_game.spec_light + 0x18u, s_metalM);
    s_camHead = (int)cpu->read_word(moby_game.cam_heading);
    s_specL[0] = (float)(int32_t)cpu->read_word(moby_game.spec_light + 0xCu);
    s_specL[1] = (float)(int32_t)cpu->read_word(moby_game.spec_light + 0x10u);
    s_specL[2] = (float)(int32_t)cpu->read_word(moby_game.spec_light + 0x14u);
    s_specLlen = sqrtf(s_specL[0]*s_specL[0] + s_specL[1]*s_specL[1] + s_specL[2]*s_specL[2]);
    if (s_specLlen < 1.0f) s_specLlen = 1.0f;
    /* metal env texture page/CLUT: D_800756F0 word0 hi=clut, word1 hi=tpage
     * (empirically clut=0x1C20 tpage=0x80AC; == the captured env prims). */
    unsigned cl = cpu->read_word(moby_game.metal_tiledef) >> 16;
    unsigned tp = cpu->read_word(moby_game.metal_tiledef + 4u) >> 16;
    ngte_tpage t;
    ngte_decode_tpage(tp, cl, 0u, &t);
    s_env_tpx = t.tpx; s_env_tpy = t.tpy;
    s_env_bpp = t.bpp;
    s_env_clx = t.clx; s_env_cly = t.cly;
}
/** @brief The environment-reflection vector: the metal matrix times a vertex's rotated offset.
 *
 * @param ex The rotated vertex offset X, (M v) / 4096.
 * @param ey The rotated vertex offset Y.
 * @param ez The rotated vertex offset Z.
 * @param r Receives the reflection vector, in the offset's units. */
static void specular_env_refl(float ex, float ey, float ez, float r[3]) {
    r[0] = (s_metalM[0][0]*ex + s_metalM[0][1]*ey + s_metalM[0][2]*ez) / 4096.0f;
    r[1] = (s_metalM[1][0]*ex + s_metalM[1][1]*ey + s_metalM[1][2]*ez) / 4096.0f;
    r[2] = (s_metalM[2][0]*ex + s_metalM[2][1]*ey + s_metalM[2][2]*ez) / 4096.0f;
}

#define PI_F 3.14159265358979f
/** @brief Wrap x into [0, 128).
 *
 * @param x The value, finite.
 * @return x modulo 128, non-negative. */
static float wrap128(float x) { x = fmodf(x, 128.0f); return x < 0 ? x + 128.0f : x; }
/** @brief The constant texel term of the environment-reflection U coordinate, latched on first use.
 *
 * specular_env_uv computes, from r = specular_env_refl(ex, ey, ez),
 *   u = (env_c2 - (128/2pi)*(atan2(rx,ry) + env_azsign*az_TR) + camHead/32) mod 128
 *   v = 64 + (128/pi)*asin(rz/|r|)
 * where az_TR is the record's view-space azimuth. The metal matrix
 * (MetalBuildMatrix, z-negated camera rotation) is not rebuilt every tick, so
 * the .s tracks fine camera yaw with the camHead term (g_Camera+0x50, an
 * 11-bit heading, 2048 per turn). This keeps the reflection world-anchored:
 * turning the camera slides the reflected environment across the metal.
 * OPENPETE_ENV_C2 overrides the default 14.5.
 * @return The term in texels. */
static float env_c2(void) {   /* calibration constant (temp env override) */
    static float c = -1e9f;
    if (c < -1e8f) { const char* e = getenv("OPENPETE_ENV_C2"); c = (e && *e) ? (float)atof(e) : 14.5f; }
    return c;
}
/** @brief The sign on the record azimuth in the environment-reflection U term, latched on first use.
 *
 * 1.0 by default; OPENPETE_ENV_AZSIGN overrides it. specular_env_uv multiplies
 * s_cur_env_az by it.
 * @return The factor. */
static float env_azsign(void) {
    static float c = -1e9f;
    if (c < -1e8f) { const char* e = getenv("OPENPETE_ENV_AZSIGN"); c = (e && *e) ? (float)atof(e) : 1.0f; }
    return c;
}
/** @brief The environment-reflection texel of a vertex, as the specular pass computes it.
 *
 * Reflects the offset (specular_env_refl), then u follows env_c2, the
 * reflection azimuth plus env_azsign times the record's view azimuth
 * (s_cur_env_az), and the camera heading (s_camHead / 32), modulo 128; v is
 * 64 plus the elevation scaled to 128 per half turn, 0 elevation when the
 * reflection is shorter than 0.001. The .s builds the same terms at
 * .L800226F8 from the octant-table atan of the record's view position.
 * @param ex The rotated vertex offset X.
 * @param ey The rotated vertex offset Y.
 * @param ez The rotated vertex offset Z.
 * @param pu Receives U, clamped to [0, 255].
 * @param pv Receives V, clamped to [0, 255]. */
static void specular_env_uv(float ex, float ey, float ez, int* pu, int* pv) {
    float r[3]; specular_env_refl(ex, ey, ez, r);
    float len = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    float az = atan2f(r[0], r[1]);
    float el = len > 0.001f ? asinf(r[2] / len) : 0.0f;
    /* u: per-moby bias mirrors the .s SPEC setup (.L800226F8): the record's
     * view-space position azimuth atan2(TR.z, TR.x) (octant-table atan in the
     * .s) + camHead>>3, folded into the per-vertex azimuth at (camHead/32 -
     * az_TR/2)*4 texel scale. The old fit (52.5 - az*k - camHead/16) had NO
     * TR term and the wrong camHead weight - exact only near the artisans
     * fit-point (edge mobys / other camHead values drifted, e.g. cliff town
     * shield sampling 75 texels off = dark noise). */
    int u = (int)lrintf(wrap128(env_c2() - (128.0f / (2.0f*PI_F)) * (az + env_azsign()*s_cur_env_az) + s_camHead / 32.0f));
    int v = (int)lrintf(64.0f + (128.0f / PI_F) * el);
    if (u < 0) u = 0; if (u > 255) u = 255;
    if (v < 0) v = 0; if (v > 255) v = 255;
    *pu = u; *pv = v;
}
/** @brief Print the metal faces' native environment UV beside their matched prim's UV.
 *
 * Refreshes the environment state and the captured prims, prints the metal
 * matrix, camera heading and record material words, then for up to 16
 * textured walk triangles matched to a captured prim (centroids within 32 px,
 * tri_match_cost at 8 px or less) prints each vertex's reflection vector, its
 * native (u, v), the nearest prim vertex's (u, v) and the difference, followed
 * by the mean and maximum errors. Prints to stderr; reads guest memory only.
 * @param cpu The guest CPU state, read only. */
static void specular_env_calibrate(CPUState* cpu) {
    specular_env_load(cpu);
    build_prim_tris();
    fprintf(stderr, "[spec-env vbl=%u] metalM=[%d %d %d|%d %d %d|%d %d %d] camHead=%d nrecs=%d "
        "D756F0=%08X %08X clutmix\n",
        openpete_frame, s_metalM[0][0],s_metalM[0][1],s_metalM[0][2],
        s_metalM[1][0],s_metalM[1][1],s_metalM[1][2], s_metalM[2][0],s_metalM[2][1],s_metalM[2][2],
        s_camHead, s_nrecs, cpu->read_word(moby_game.metal_tiledef), cpu->read_word(moby_game.metal_tiledef + 4u));
    for (int r = 0; r < s_nrecs; r++)
        fprintf(stderr, "  rec%d metal=%d mscol=%08X moby=%08X\n", r,
            s_recs[r].metal, s_recs[r].mscol, s_recs[r].moby);
    int dumped = 0, nv = 0; double sumu = 0, sumv = 0, sgnu = 0; int maxu = 0, maxv = 0;
    for (int wi = 0; wi < s_gout.n && dumped < 16; wi++) {
        ntri_t* w = &s_wtri[wi];
        if (!w->textured) continue;
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0; pi<s_ptri_n; pi++) {
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if (fabs(pcx-wcx)>32.0 || fabs(pcy-wcy)>32.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if (c<best){best=c;bp=pi;}
        }
        if (bp<0 || best>8.0) continue;
        ptri* P=&s_ptri[bp];
        for (int k=0;k<3;k++) {
            double bd=1e18; int bj=0;
            for (int j=0;j<3;j++){ double dx=w->v[k].sx-P->x[j], dy=w->v[k].sy-P->y[j];
                double d=dx*dx+dy*dy; if(d<bd){bd=d;bj=j;} }
            float r[3]; specular_env_refl(w->v[k].ex,w->v[k].ey,w->v[k].ez,r);
            int nu, nv2; specular_env_uv(w->v[k].ex,w->v[k].ey,w->v[k].ez, &nu, &nv2);
            int pu = P->u[bj], pv = P->v[bj];
            int du = nu - (pu & 127); if (du > 64) du -= 128; if (du < -64) du += 128;
            int dv = nv2 - pv;
            if (pu || pv) {  /* skip the (0,0) untextured-prim matches */
                nv++; sumu += fabs((double)du); sumv += fabs((double)dv); sgnu += du;
                if (abs(du) > maxu) maxu = abs(du); if (abs(dv) > maxv) maxv = abs(dv);
            }
            fprintf(stderr, "  refl(%.0f %.0f %.0f) -> uv(%d,%d) prim(%d,%d) d(%d,%d) clut=%04X\n",
                r[0],r[1],r[2], nu,nv2, pu,pv, du,dv, P->clut);
        }
        dumped++;
    }
    fprintf(stderr, "[spec-env vbl=%u] camHead=%d dumped %d faces, %d verts: meanErr u=%.1f v=%.1f "
        "signedU=%.2f maxErr u=%d v=%d\n",
        openpete_frame, s_camHead, dumped, nv, nv?sumu/nv:0, nv?sumv/nv:0, nv?sgnu/nv:0, maxu, maxv);
}

/** @brief Print the specular highlight correlation: each walk face's normal against the specular light, beside its matched prim colour.
 *
 * The draw gives a non-metal shiny face a per-face highlight (.L80021DB4:
 * the screen face normal, normalized, through CC against
 * m_SpecularMetalColor and the specular light) that replaces the per-vertex
 * base colour. For up to 24 walk triangles matched to a captured prim
 * (centroids within 32 px, tri_match_cost at 8 px or less) the line gives the
 * cosine of the view-space face normal with the light, the native base
 * colour, the prim colour and their luminance ratio. Runs on the
 * OPENPETE_MOBY_VERIFY specular path before recolor_walk_from_prims, so the
 * walk colours are still the native base.
 * @param cpu The guest CPU state, read only. */
static void specular_hl_calibrate(CPUState* cpu) {
    int lx=(int32_t)cpu->read_word(moby_game.spec_light+0xCu);   /* m_specularLightDirectionMoby */
    int ly=(int32_t)cpu->read_word(moby_game.spec_light+0x10u);
    int lz=(int32_t)cpu->read_word(moby_game.spec_light+0x14u);
    float Llen=sqrtf((float)(lx*lx+ly*ly+lz*lz)); if(Llen<1) Llen=1;
    build_prim_tris();
    fprintf(stderr, "[spec-hl vbl=%u] light=(%d,%d,%d)\n", openpete_frame, lx,ly,lz);
    int dumped=0;
    for (int wi=0; wi<s_gout.n && dumped<24; wi++) {
        ntri_t* w=&s_wtri[wi];
        double wcx=(w->v[0].sx+w->v[1].sx+w->v[2].sx)/3.0, wcy=(w->v[0].sy+w->v[1].sy+w->v[2].sy)/3.0;
        double best=1e18; int bp=-1;
        for (int pi=0;pi<s_ptri_n;pi++){
            double pcx=(s_ptri[pi].x[0]+s_ptri[pi].x[1]+s_ptri[pi].x[2])/3.0;
            double pcy=(s_ptri[pi].y[0]+s_ptri[pi].y[1]+s_ptri[pi].y[2])/3.0;
            if(fabs(pcx-wcx)>32.0||fabs(pcy-wcy)>32.0) continue;
            double c=tri_match_cost(w,&s_ptri[pi]); if(c<best){best=c;bp=pi;}
        }
        if(bp<0||best>8.0) continue;
        ptri* P=&s_ptri[bp];
        /* view-space face normal from the rotated vertex offsets */
        float e1x=w->v[1].ex-w->v[0].ex, e1y=w->v[1].ey-w->v[0].ey, e1z=w->v[1].ez-w->v[0].ez;
        float e2x=w->v[2].ex-w->v[0].ex, e2y=w->v[2].ey-w->v[0].ey, e2z=w->v[2].ez-w->v[0].ez;
        float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl<0.001f) nl=1;
        float dotp=(nx*lx+ny*ly+nz*lz)/(nl*Llen);   /* [-1,1] both signs */
        uint32_t b=w->v[0].rgb; int br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF;
        uint32_t pr0=P->rgb[0]; int pr=pr0&0xFF, pg=(pr0>>8)&0xFF, pb=(pr0>>16)&0xFF;
        fprintf(stderr, "  NL=%+.3f tex=%d | base(%d,%d,%d) L%d -> prim(%d,%d,%d) L%d  ratio=%.2f\n",
            dotp, w->textured, br,bg,bb,(br+bg+bb)/3, pr,pg,pb,(pr+pg+pb)/3,
            (br+bg+bb)?(double)(pr+pg+pb)/(br+bg+bb):0.0);
        dumped++;
    }
}

/* ---- same-moby paint rank ---------------------------------------------------
 * Under true depth two faces of one moby that cross in 3D show their
 * intersection contour, which moves with every animation tick; the game
 * paints each face whole in its ordering-table order and never shows it. The
 * rank select (native_render's rank image; OPENPETE_MOBY_RANK=0 turns it off) orders
 * such faces by the game's paint order, and every other face keeps true
 * depth: a face carries its rank only while it intersects another face of the
 * same moby (the contest flag).
 *
 * Intersection space: (sx, sy, K/vz). The perspective map from view space is
 * projective and bijective for vz > 0, so it preserves incidence (planes,
 * lines, intersections). Two faces sharing exactly two vertex positions
 * whose half-planes about the shared edge lie stacked (isect_stack_deg) are
 * contested: a decal face on its base, which the game paints after the base
 * on its key while its free vertex may sit behind the base's plane. Any
 * other pair sharing >= 2 vertex positions (a shared edge) is adjacent and
 * skipped; otherwise the pair intersects when the
 * overlap of the two plane-clipped segments has a screen length above
 * ISECT_TOL_PX (a shared single vertex yields a zero-length overlap at that
 * vertex and is rejected by the same bound). Coplanar pairs are not
 * contested. */
#define ISECT_K      1048576.0
#define ISECT_TOL_PX 0.5
#define ISECT_EPS    1e-6
static uint8_t s_wtri_contest[MAX_NTRI];
static int     s_nrank[MAX_NTRI];   /* position in the record's paint order, 0 = none */
/* The intersecting pairs of this walk (walk indices), for the calibration. */
#define ISECT_PAIRS_MAX 65536
static int s_ip_a[ISECT_PAIRS_MAX], s_ip_b[ISECT_PAIRS_MAX], s_ip_n;

/** @brief Whether OPENPETE_MOBY_ISECT_LOG enables the intersection log, latched on first use.
 *
 * The log prints per-VBL per-moby intersection counts, the select census and
 * the sub-tick carry's engagement. With the rank off it runs the intersection
 * census alone. On for a nonempty value not starting with '0'.
 * @return 1 when on, else 0. */
static int isect_log_on(void) {
    static int s = -1;
    if (s < 0) { const char* l = getenv("OPENPETE_MOBY_ISECT_LOG");
                 s = (l && *l && *l != '0'); }
    return s;
}
/* A face in intersection space: its vertices, bounding box and plane normal
 * n with nlen = |n|, computed once per face, and whether all of them are
 * finite. */
typedef struct { double p[3][3]; double lo[3], hi[3]; double n[3], nlen; int fin; } isect_tri;
/** @brief The cross product a x b.
 * @param a The first vector.
 * @param b The second vector.
 * @param o Receives the product; distinct from `a` and `b`. */
static void isect_cross(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1]; o[1] = a[2]*b[0] - a[0]*b[2]; o[2] = a[0]*b[1] - a[1]*b[0];
}
/** @brief The dot product of two 3-vectors.
 * @param a The first vector.
 * @param b The second vector.
 * @return The product. */
static double isect_dot(const double a[3], const double b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
#define ISECT_NAN 2
/** @brief The side class of a vertex against a plane, from its distance numerator g and the normal's length.
 *
 * The distance g/nlen, snapped to 0 within ISECT_EPS, classes as +1 or -1 by
 * its sign, 0 when snapped, and ISECT_NAN when it is not a number (on neither
 * side and not on the plane). Away from the snap bound the class follows from g
 * alone: a relative margin far above the rounding of the division decides it,
 * and the division runs only inside the margin.
 * @param g The distance numerator n.(p - q).
 * @param nlen The plane normal's length.
 * @return +1 or -1 beyond the snap bound, 0 within it, ISECT_NAN for a NaN
 *         distance. */
static int isect_side_of(double g, double nlen) {
    const double a = fabs(g), e = ISECT_EPS * nlen;
    const int beyond = a > e * (1.0 + 1e-6), within = a < e * (1.0 - 1e-6);
    if (beyond | within) return beyond * ((g > 0) - (g < 0));
    const double d = g / nlen;
    if (fabs(d) < ISECT_EPS) return 0;
    return d > 0 ? 1 : d < 0 ? -1 : ISECT_NAN;
}
/** @brief The side classes and distance numerators of a face's vertices against a plane.
 *
 * Each vertex k takes g[k] = n.(p_k - q) and its class from isect_side_of.
 * @param T The face.
 * @param n The plane normal.
 * @param nlen |n|.
 * @param q A point on the plane.
 * @param g Receives the three distance numerators.
 * @param s Receives the three side classes.
 * @return -1 when T lies strictly on one side, -2 when it lies in the plane,
 *         else 0. */
static int isect_side(const isect_tri* T, const double n[3], double nlen, const double q[3],
                      double g[3], int s[3]) {
    for (int k = 0; k < 3; k++) {
        double v[3] = { T->p[k][0]-q[0], T->p[k][1]-q[1], T->p[k][2]-q[2] };
        g[k] = isect_dot(n, v);
        s[k] = isect_side_of(g[k], nlen);
    }
    if ((s[0] == 1 && s[1] == 1 && s[2] == 1) || (s[0] == -1 && s[1] == -1 && s[2] == -1))
        return -1;
    if (s[0] == 0 && s[1] == 0 && s[2] == 0) return -2;
    return 0;
}
/** @brief The points of a face on a plane: its edge crossings and on-plane vertices.
 *
 * Both ends of a crossing edge lie beyond the snap bound, so their distances
 * are g/nlen unsnapped.
 * @param T The face.
 * @param g The vertices' distance numerators from isect_side.
 * @param s The vertices' side classes from isect_side.
 * @param nlen The plane normal's length.
 * @param pts Receives the points.
 * @return Their count, 0..3. */
static int isect_clip(const isect_tri* T, const double g[3], const int s[3], double nlen,
                      double pts[4][3]) {
    int m = 0;
    for (int k = 0; k < 3; k++) {
        int j = (k + 1) % 3;
        if (s[k] == 0) { memcpy(pts[m++], T->p[k], sizeof(double)*3); }
        if (s[k] * s[j] == -1) {
            double dk = g[k] / nlen, dj = g[j] / nlen;
            double t = dk / (dk - dj);
            for (int c = 0; c < 3; c++) pts[m][c] = T->p[k][c] + t*(T->p[j][c] - T->p[k][c]);
            m++;
        }
    }
    return m;
}
/** @brief The number of points isect_clip returns for the side classes s.
 *
 * @param s The three side classes.
 * @return The count, 0..3. */
static int isect_npts(const int s[3]) {
    int m = 0;
    for (int k = 0; k < 3; k++) m += (s[k] == 0) + (s[k] * s[(k + 1) % 3] == -1);
    return m;
}
/** @brief Whether A's side of B's plane alone rules the pair out.
 *
 * It does for a degenerate face, A strictly on one side, or A (finite
 * values) meeting the plane in at most one point, an overlap of zero length;
 * isect_pair then returns 0 or 2. The classes are counted without branching
 * on each vertex; a vertex inside the margin of isect_side_of takes the exact
 * classification. In a triangle every vertex pair is an edge, so pos * neg
 * edges cross the plane.
 * @param A The face tested against the plane.
 * @param B The face whose plane is used.
 * @return 1 when ruled out, else 0. */
static int isect_misses(const isect_tri* A, const isect_tri* B) {
    if (A->nlen < 1e-9 || B->nlen < 1e-9) return 1;
    const double e = ISECT_EPS * B->nlen, ehi = e * (1.0 + 1e-6), elo = e * (1.0 - 1e-6);
    int pos = 0, neg = 0, zer = 0, amb = 0;
    for (int k = 0; k < 3; k++) {
        double v[3] = { A->p[k][0]-B->p[0][0], A->p[k][1]-B->p[0][1], A->p[k][2]-B->p[0][2] };
        const double g = isect_dot(B->n, v), a = fabs(g);
        const int beyond = a > ehi, within = a < elo;
        pos += beyond & (g > 0); neg += beyond & (g < 0); zer += within; amb += !(beyond | within);
    }
    if (amb) {
        double g[3];
        int s[3];
        if (isect_side(A, B->n, B->nlen, B->p[0], g, s) == -1) return 1;
        return A->fin && B->fin && isect_npts(s) <= 1;
    }
    if (pos == 3 || neg == 3) return 1;
    return A->fin && B->fin && zer + pos * neg <= 1;
}
/** @brief The positions of points along a line direction and the indices of the first and last of them.
 *
 * @param pts The points.
 * @param m Their count.
 * @param D The line direction.
 * @param lD |D|.
 * @param t Receives each point's position D.p / lD.
 * @param first Receives the index of the smallest position.
 * @param last Receives the index of the largest. */
static void isect_extent(const double pts[4][3], int m, const double D[3], double lD,
                         double t[4], int* first, int* last) {
    int i0 = 0, i1 = 0;
    for (int k = 0; k < m; k++) {
        t[k] = isect_dot(D, pts[k]) / lD;
        if (t[k] < t[i0]) i0 = k;
        if (t[k] > t[i1]) i1 = k;
    }
    *first = i0; *last = i1;
}
/** @brief Classify a face pair in intersection space.
 *
 * Each face is clipped against the other's plane and the two segments are
 * projected on the planes' intersection line; the pair intersects when their
 * overlap's screen length exceeds ISECT_TOL_PX. A degenerate face, a face
 * strictly on one side, a single-point meet between finite faces, and an
 * empty overlap yield 0.
 * @param A The first face.
 * @param B The second face.
 * @return 1 intersect, 0 no, 2 coplanar or parallel planes. */
static int isect_pair(const isect_tri* A, const isect_tri* B) {
    if (A->nlen < 1e-9 || B->nlen < 1e-9) return 0;
    double ga[3], gb[3];
    int sa3[3], sb3[3];
    int sa = isect_side(A, B->n, B->nlen, B->p[0], ga, sa3);
    if (sa == -1) return 0;
    int sb = isect_side(B, A->n, A->nlen, A->p[0], gb, sb3);
    if (sb == -1) return 0;
    if (sa == -2 || sb == -2) return 2;
    double pa[4][3], pb[4][3];
    int ma = isect_clip(A, ga, sa3, B->nlen, pa), mb = isect_clip(B, gb, sb3, A->nlen, pb);
    if (ma < 1 || mb < 1) return 0;
    double D[3]; isect_cross(A->n, B->n, D);
    double lD = sqrt(isect_dot(D, D));
    if (lD < 1e-12) return 2;
    /* A face meeting the other's plane in one point (a shared vertex, or a
     * vertex touching it) bounds the overlap to that point: zero length. For
     * finite values the projection below reaches the same verdict. */
    if ((ma == 1 || mb == 1) && A->fin && B->fin) return 0;
    int ia0, ia1, ib0, ib1;
    double ta[4], tb[4];
    isect_extent(pa, ma, D, lD, ta, &ia0, &ia1);
    isect_extent(pb, mb, D, lD, tb, &ib0, &ib1);
    /* Overlap endpoints as 3D points: the later start and the earlier end. */
    const double* s = ta[ia0] > tb[ib0] ? pa[ia0] : pb[ib0];
    const double* e = ta[ia1] < tb[ib1] ? pa[ia1] : pb[ib1];
    double lo = ta[ia0] > tb[ib0] ? ta[ia0] : tb[ib0];
    double hi = ta[ia1] < tb[ib1] ? ta[ia1] : tb[ib1];
    if (hi <= lo) return 0;
    double dx = e[0]-s[0], dy = e[1]-s[1];
    return (dx*dx + dy*dy) > ISECT_TOL_PX*ISECT_TOL_PX;
}
/** @brief The vertices of one walk triangle that coincide with a vertex of another.
 *
 * A vertex coincides when a vertex of the other triangle has the same sx, sy
 * and tvz, compared exactly.
 * @param a The first triangle.
 * @param b The second triangle.
 * @param m Receives, per vertex of `a`, the index of its coincident vertex in
 *          `b`, or -1.
 * @return The number of coincident vertices, 0 to 3. */
static int isect_shared(const ntri_t* a, const ntri_t* b, int m[3]) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        m[i] = -1;
        for (int j = 0; j < 3 && m[i] < 0; j++)
            if (a->v[i].sx == b->v[j].sx && a->v[i].sy == b->v[j].sy &&
                a->v[i].tvz == b->v[j].tvz) m[i] = j;
        n += m[i] >= 0;
    }
    return n;
}
/* A pair sharing one edge is stacked (one face lies over the other, a decal
 * on its base) when its free vertices lie on the same side of the shared edge
 * on screen and the angle between the two half-planes about the edge is at
 * most ISECT_STACK_DEG in view space. A manifold neighbour's free vertex lies
 * on the opposite side; a fold seen at a silhouette opens a wider angle. On
 * the Spyro 1-3 mobys a decal lies within 3.5 degrees of its base and the
 * narrowest same-side fold opens 6.3 degrees. */
#define ISECT_STACK_DEG 5.0
/** @brief Whether the stacked-pair contest is on (OPENPETE_MOBY_RANK_STACKED, default on), latched on first use.
 * @return 1 when on, else 0. */
static int isect_stacked_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_STACKED"); s = !(e && *e == '0'); }
    return s;
}
/** @brief A walk vertex in view space scaled by the projection's H.
 *
 * Inverts the perspective divide sx = ofx + H x / z with z = tvz:
 * ((sx - ofx) tvz, (sy - ofy) tvz, H tvz), a uniform scale of the view-space
 * position, so angles between the vertices hold.
 * @param v The vertex.
 * @param o Receives the point. */
static void isect_view_of(const ntri_vert* v, double o[3]) {
    o[0] = ((double)v->sx - s_view.ofx) * v->tvz;
    o[1] = ((double)v->sy - s_view.ofy) * v->tvz;
    o[2] = s_view.H_eff * v->tvz;
}
/** @brief The screen-space cross product (e1 - e0) x (p - e0), whose sign is p's side of the line e0 e1.
 * @param e0 The line's first vertex.
 * @param e1 The line's second vertex.
 * @param p The vertex classified.
 * @return The signed doubled area of the triangle e0 e1 p. */
static double isect_side2(const ntri_vert* e0, const ntri_vert* e1, const ntri_vert* p) {
    return ((double)e1->sx - e0->sx) * ((double)p->sy - e0->sy) -
           ((double)e1->sy - e0->sy) * ((double)p->sx - e0->sx);
}
/** @brief The component of f - e0 perpendicular to the edge e0 e1 in view space.
 * @param e0 The edge's first point.
 * @param u The edge direction e1 - e0.
 * @param uu u.u, nonzero.
 * @param f The free vertex.
 * @param w Receives the perpendicular component. */
static void isect_edge_perp(const double e0[3], const double u[3], double uu, const double f[3],
                            double w[3]) {
    const double d[3] = { f[0] - e0[0], f[1] - e0[1], f[2] - e0[2] };
    const double t = isect_dot(d, u) / uu;
    for (int c = 0; c < 3; c++) w[c] = d[c] - t * u[c];
}
/** @brief The angle in degrees between two triangles' half-planes about their shared edge, in view space.
 *
 * 0 when both free vertices lie on one ray from the edge (stacked, coplanar),
 * 180 when the faces continue each other flat. A degenerate edge or face
 * returns 180.
 * @param e0 The shared edge's first vertex.
 * @param e1 Its second vertex.
 * @param fa The first face's free vertex.
 * @param fb The second face's free vertex.
 * @return The angle, 0 to 180. */
static double isect_fold_deg(const ntri_vert* e0, const ntri_vert* e1, const ntri_vert* fa,
                             const ntri_vert* fb) {
    double p0[3], p1[3], pa[3], pb[3], u[3], wa[3], wb[3], x[3];
    isect_view_of(e0, p0); isect_view_of(e1, p1); isect_view_of(fa, pa); isect_view_of(fb, pb);
    for (int c = 0; c < 3; c++) u[c] = p1[c] - p0[c];
    const double uu = isect_dot(u, u);
    if (!(uu > 0.0)) return 180.0;
    isect_edge_perp(p0, u, uu, pa, wa);
    isect_edge_perp(p0, u, uu, pb, wb);
    isect_cross(wa, wb, x);
    const double deg = atan2(sqrt(isect_dot(x, x)), isect_dot(wa, wb)) * (180.0 / M_PI);
    return isfinite(deg) && isect_dot(wa, wa) > 0.0 && isect_dot(wb, wb) > 0.0 ? deg : 180.0;
}
/** @brief The fold angle of a pair sharing exactly two vertices whose free vertices lie on one side of the shared edge on screen.
 *
 * @param a The first triangle.
 * @param b The second triangle.
 * @param m The coincidence map of `a` onto `b` from isect_shared, two entries
 *          set.
 * @return The angle from isect_fold_deg, or -1 when a vertex lies at or
 *         behind the camera or the free vertices lie on opposite sides (or
 *         on the line). */
static double isect_stack_deg(const ntri_t* a, const ntri_t* b, const int m[3]) {
    const int fa = m[0] < 0 ? 0 : m[1] < 0 ? 1 : 2;
    const int i0 = (fa + 1) % 3, i1 = (fa + 2) % 3;
    const int fb = 3 - m[i0] - m[i1];
    if (!(a->v[0].tvz > 0.0f && a->v[1].tvz > 0.0f && a->v[2].tvz > 0.0f && b->v[fb].tvz > 0.0f))
        return -1.0;
    const double sa = isect_side2(&a->v[i0], &a->v[i1], &a->v[fa]);
    const double sb = isect_side2(&a->v[i0], &a->v[i1], &b->v[fb]);
    if (!(sa * sb > 0.0)) return -1.0;
    return isect_fold_deg(&a->v[i0], &a->v[i1], &a->v[fa], &b->v[fb]);
}
/** @brief Whether the rank select can order a walk triangle.
 *
 * It can when the triangle is opaque (blend 0), belongs to a moby, is not a
 * UI face and is not mangled (s_wtri_mangle).
 * @param i The walk triangle index.
 * @return Nonzero when eligible. */
static int isect_eligible(int i) {
    return s_wtri[i].blend == 0 && s_wtri_moby[i] && s_wtri_ui[i] == 0 && !s_wtri_mangle[i];
}
/** @brief Build the intersection-space face of a walk triangle.
 *
 * The vertices are (sx, sy, ISECT_K / vz), with depth 0 where vz is not
 * positive. The face also gets its bounding box, its plane normal (p1 - p0) x
 * (p2 - p0) with the normal's length, and fin, set when every coordinate and
 * the length are finite.
 * @param i The walk triangle index.
 * @param t Receives the face. */
static void isect_tri_of(int i, isect_tri* t) {
    int fin = 1;
    for (int k = 0; k < 3; k++) {
        double w = s_wtri[i].v[k].vz;
        t->p[k][0] = s_wtri[i].v[k].sx; t->p[k][1] = s_wtri[i].v[k].sy;
        t->p[k][2] = w > 0.0 ? ISECT_K / w : 0.0;
        fin &= isfinite(t->p[k][0]) && isfinite(t->p[k][1]) && isfinite(t->p[k][2]);
    }
    double e[3], f[3];
    for (int c = 0; c < 3; c++) {
        t->lo[c] = fmin(t->p[0][c], fmin(t->p[1][c], t->p[2][c]));
        t->hi[c] = fmax(t->p[0][c], fmax(t->p[1][c], t->p[2][c]));
        e[c] = t->p[1][c]-t->p[0][c]; f[c] = t->p[2][c]-t->p[0][c];
    }
    isect_cross(e, f, t->n);
    t->nlen = sqrt(isect_dot(t->n, t->n));
    t->fin = fin && isfinite(t->nlen);
}
/** @brief Whether the census feeds an instrument, which reads every intersecting pair and every face's rank.
 *
 * The instruments are the per-moby log and select census
 * (OPENPETE_MOBY_ISECT_LOG) and the rank calibration.
 * @return Nonzero when one is on. */
static int isect_instrumented(void) { return isect_log_on() || moby_rank_cal_on(); }
typedef struct { long faces, contested, pairs, coplanar, shared, stacked; } isect_count;
typedef struct { const isect_tri* T; const int* idx; isect_count* c; int all; } isect_ctx;
/** @brief Set a contesting pair's flags and record it in the pair list while the list has room.
 * @param ia The first face's walk index.
 * @param ib The second face's walk index. */
static void isect_contest(int ia, int ib) {
    s_wtri_contest[ia] = 1; s_wtri_contest[ib] = 1;
    if (s_ip_n < ISECT_PAIRS_MAX) { s_ip_a[s_ip_n] = ia; s_ip_b[s_ip_n] = ib; s_ip_n++; }
}
/** @brief Print one same-side shared-edge pair as an [isect-stack] line.
 * @param ia The first face's walk index.
 * @param ib The second face's walk index.
 * @param deg The pair's fold angle.
 * @param hit Whether the angle is within ISECT_STACK_DEG. */
static void isect_log_stack(int ia, int ib, double deg, int hit) {
    const ntri_vert *a = s_wtri[ia].v, *b = s_wtri[ib].v;
    fprintf(stderr, "[isect-stack] vbl=%u mat=%s m=%08X cls=%u fkey=%08X,%08X tex=%d,%d "
                    "otk=%08X,%08X deg=%.3f stacked=%d a=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f "
                    "b=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
            openpete_frame, s_cur_mat, s_wtri_moby[ia], s_wtri_cls[ia], s_wtri_fkey[ia],
            s_wtri_fkey[ib], s_wtri[ia].textured, s_wtri[ib].textured, (uint32_t)s_wtri_otk[ia],
            (uint32_t)s_wtri_otk[ib], deg, hit, a[0].sx, a[0].sy, a[1].sx, a[1].sy, a[2].sx,
            a[2].sy, b[0].sx, b[0].sy, b[1].sx, b[1].sy, b[2].sx, b[2].sy);
}
/** @brief Test one pair of a moby's faces whose bounding boxes overlap and flag it when they intersect.
 *
 * Without an instrument, which counts every verdict, a pair is dropped first
 * when both faces contest already. A pair sharing exactly two vertex
 * positions that is stacked (isect_stack_deg within ISECT_STACK_DEG) contests
 * and counts as stacked. Without an instrument the remaining pairs are then
 * dropped when either face's side of the other's plane rules the pair out.
 * A pair sharing two or more vertex positions is counted adjacent and
 * skipped. An intersecting pair contests (isect_contest).
 * @param x The moby's faces, their walk indices and the counts.
 * @param a The first face's position in the moby's face list.
 * @param b The second face's position, after `a`. */
static void isect_test(const isect_ctx* x, int a, int b) {
    const int ia = x->idx[a], ib = x->idx[b];
    const isect_tri *A = &x->T[a], *B = &x->T[b];
    if (!x->all && s_wtri_contest[ia] && s_wtri_contest[ib]) return;
    int m[3];
    const int ns = isect_shared(&s_wtri[ia], &s_wtri[ib], m);
    const double deg = ns == 2 && isect_stacked_on() ? isect_stack_deg(&s_wtri[ia], &s_wtri[ib], m)
                                                     : -1.0;
    const int stacked = deg >= 0.0 && deg <= ISECT_STACK_DEG;
    if (deg >= 0.0 && isect_log_on() && openpete_frame != 0) isect_log_stack(ia, ib, deg, stacked);
    if (stacked) { x->c->stacked++; isect_contest(ia, ib); return; }
    if (!x->all && (isect_misses(A, B) || isect_misses(B, A))) return;
    if (ns >= 2) { x->c->shared++; return; }
    int r = isect_pair(A, B);
    x->c->coplanar += r == 2;
    if (r != 1) return;
    x->c->pairs++;
    isect_contest(ia, ib);
}
/* A face's bounding box in the sweep order. A bound that is not a number
 * (every vertex coordinate on that axis is NaN) is widened to infinity, which
 * like NaN never separates two boxes. */
typedef struct { double lo[3], hi[3]; int k; } isect_box;
/** @brief Fill a sweep box from a face's bounding box, widening a NaN bound to infinity.
 * @param t The face.
 * @param k The face's position in its moby's face list.
 * @param o Receives the box. */
static void isect_box_of(const isect_tri* t, int k, isect_box* o) {
    for (int c = 0; c < 3; c++) {
        o->lo[c] = isnan(t->lo[c]) ? -INFINITY : t->lo[c];
        o->hi[c] = isnan(t->hi[c]) ? INFINITY : t->hi[c];
    }
    o->k = k;
}
/** @brief The bounding boxes of a moby's faces in ascending order of x minimum.
 *
 * An insertion sort on a compact key: a moby's face count is small and the
 * sort runs per tick. A NaN x minimum sorts as minus infinity.
 * @param T The faces.
 * @param n Their count.
 * @param K Receives the n boxes in sweep order. */
static void isect_boxes_sorted(const isect_tri* T, int n, isect_box* K) {
    static struct { double x; int k; } X[MAX_NTRI];
    for (int a = 0; a < n; a++) {
        const double x = isnan(T[a].lo[0]) ? -INFINITY : T[a].lo[0];
        int j = a;
        for (; j > 0 && X[j - 1].x > x; j--) X[j] = X[j - 1];
        X[j].x = x; X[j].k = a;
    }
    for (int s = 0; s < n; s++) isect_box_of(&T[X[s].k], X[s].k, &K[s]);
}
/** @brief Test the pairs of one moby's eligible faces whose bounding boxes overlap.
 *
 * Sets the intersecting faces' contest flags and records the pairs. Sort and
 * sweep on x: the boxes sorted by x minimum, each against the
 * following boxes whose minimum does not pass its maximum, which enumerates
 * exactly the pairs overlapping in x; the pairs that also overlap in y and
 * depth are collected without a branch per pair, then tested. The pair test
 * takes the lower face-list position first, so its floating-point result does
 * not depend on the enumeration order.
 * @param T The faces in intersection space.
 * @param idx Each face's walk triangle index.
 * @param n The face count.
 * @param c Accumulates the moby's counts. */
static void isect_moby(const isect_tri* T, const int* idx, int n, isect_count* c) {
    static isect_box K[MAX_NTRI];
    static int hit[MAX_NTRI];
    isect_boxes_sorted(T, n, K);
    const isect_ctx x = { T, idx, c, isect_instrumented() };
    for (int s = 0; s < n; s++) {
        const isect_box* A = &K[s];
        int m = 0;
        for (int t = s + 1; t < n && K[t].lo[0] <= A->hi[0]; t++) {
            const isect_box* B = &K[t];
            hit[m] = t;
            m += (B->lo[1] <= A->hi[1]) & (A->lo[1] <= B->hi[1]) &
                 (B->lo[2] <= A->hi[2]) & (A->lo[2] <= B->hi[2]);
        }
        for (int j = 0; j < m; j++) {
            const int b = K[hit[j]].k;
            isect_test(&x, A->k < b ? A->k : b, A->k < b ? b : A->k);
        }
    }
    for (int a = 0; a < n; a++) c->contested += s_wtri_contest[idx[a]];
    c->faces += n;
}
/** @brief Print one moby's intersection counts as an [isect] line.
 *
 * The class and front flag come from the moby's first walk triangle.
 * @param mob The moby's PSX address.
 * @param i0 The index of the moby's first walk triangle.
 * @param c The moby's counts. */
static void isect_log_moby(uint32_t mob, int i0, const isect_count* c) {
    fprintf(stderr, "[isect] vbl=%u mat=%s m=%08X cls=%u front=%d faces=%ld contested=%ld "
                    "pairs=%ld coplanar=%ld sharededge=%ld stacked=%ld\n",
            openpete_frame, s_cur_mat, mob, s_wtri_cls[i0], s_wtri_front[i0], c->faces,
            c->contested, c->pairs, c->coplanar, c->shared, c->stacked);
}
/** @brief Fill s_wtri_contest[] and the intersecting-pair list for the whole walk.
 *
 * Groups the walk's contiguous triangles by moby and runs isect_moby over each
 * moby's eligible faces. With the intersection log on and the VBL nonzero it
 * prints one line per moby with faces and a summary line. */
static void isect_census(void) {
    static isect_tri T[MAX_NTRI];
    static int idx[MAX_NTRI];
    memset(s_wtri_contest, 0, (size_t)s_gout.n);
    s_ip_n = 0;
    int logit = isect_log_on() && openpete_frame != 0;
    isect_count tot = {0};
    for (int i0 = 0; i0 < s_gout.n; ) {
        uint32_t mob = s_wtri_moby[i0];
        int i1 = i0, n = 0;
        while (i1 < s_gout.n && s_wtri_moby[i1] == mob) i1++;
        for (int i = i0; i < i1; i++)
            if (mob && isect_eligible(i)) { isect_tri_of(i, &T[n]); idx[n++] = i; }
        isect_count c = {0};
        isect_moby(T, idx, n, &c);
        if (logit && n > 0) isect_log_moby(mob, i0, &c);
        tot.faces += c.faces; tot.contested += c.contested; tot.pairs += c.pairs;
        tot.coplanar += c.coplanar; tot.shared += c.shared; tot.stacked += c.stacked;
        i0 = i1;
    }
    if (logit)
        fprintf(stderr, "[isect-sum] vbl=%u mat=%s faces=%ld contested=%ld pairs=%ld coplanar=%ld "
                        "sharededge=%ld stacked=%ld\n", openpete_frame, s_cur_mat, tot.faces,
                tot.contested, tot.pairs, tot.coplanar, tot.shared, tot.stacked);
}

/* ---- native paint rank ------------------------------------------------------
 * The rank of each triangle is its prim's position in the record's paint
 * order as the draw's own ordering table produces it (moby_otk_face_bucket):
 * local bucket descending, then insertion order, which is the walk's emission
 * order. Both triangles of one prim share its position; triangles the table
 * does not receive rank 0. OPENPETE_MOBY_RANK_NEGCTL=1 reverses the order:
 * the negative control that proves a pixel A/B of the rank measures it. */
typedef struct { int bk, i; } nrank_e;
/** @brief qsort comparator over nrank_e: bucket descending, then walk index ascending. */
static int nrank_cmp(const void* a, const void* b) {
    const nrank_e *x = (const nrank_e*)a, *y = (const nrank_e*)b;
    if (x->bk != y->bk) return x->bk > y->bk ? -1 : 1;   /* far buckets paint first */
    return x->i < y->i ? -1 : x->i > y->i;
}
/** @brief Write s_nrank for one moby's walk triangles i0..i1-1 from their ordering-table keys.
 *
 * A triangle with a negative key or the second-triangle flag (MOBY_OTK_Q2)
 * stays out of the sort and starts at rank 0. The rest rank 1..n in paint
 * order, far bucket first, ties in walk order. A second-triangle face of a
 * quad prim then takes the rank of the triangle before it, so both halves of
 * a prim share one; the first triangle of the run is never copied.
 * @param i0 The first walk triangle of the moby.
 * @param i1 One past its last.
 * @param neg Nonzero reverses the rank order. */
static void native_rank_moby(int i0, int i1, int neg) {
    static nrank_e E[MAX_NTRI];
    int n = 0;
    for (int i = i0; i < i1; i++) {
        s_nrank[i] = 0;
        int32_t k = s_wtri_otk[i];
        if (k < 0 || (k & MOBY_OTK_Q2)) continue;
        E[n].bk = MOBY_OTK_BUCKET(k); E[n].i = i; n++;
    }
    qsort(E, (size_t)n, sizeof E[0], nrank_cmp);
    for (int k = 0; k < n; k++) s_nrank[E[k].i] = neg ? n - k : k + 1;
    for (int i = i0 + 1; i < i1; i++)
        if (s_wtri_otk[i] >= 0 && (s_wtri_otk[i] & MOBY_OTK_Q2)) s_nrank[i] = s_nrank[i - 1];
}
/** @brief Rank the faces of every moby with a contested face in the draw's paint order.
 *
 * The select reads a rank only on a contested face, so the faces of the other
 * mobys take rank 0 unsorted and the sub-tick carry stores none of them; an
 * instrument (isect_instrumented) ranks every moby. A moby is a contiguous run
 * of walk triangles with one moby pointer. OPENPETE_MOBY_RANK_NEGCTL=1
 * reverses the order. */
static void native_rank_build(void) {
    static int s_neg = -1;
    if (s_neg < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_NEGCTL"); s_neg = e && *e == '1'; }
    const int all = isect_instrumented();
    for (int i0 = 0; i0 < s_gout.n; ) {
        int i1 = i0, cont = 0;
        for (; i1 < s_gout.n && s_wtri_moby[i1] == s_wtri_moby[i0]; i1++) cont |= s_wtri_contest[i1];
        if (all || cont) native_rank_moby(i0, i1, s_neg);
        else memset(&s_nrank[i0], 0, (size_t)(i1 - i0) * sizeof s_nrank[0]);
        i0 = i1;
    }
}

/* ---- sub-tick carry of the paint verdict ------------------------------------
 * The scene build a canonical present shows stores each face's rank and
 * contest flag by (moby, fkey); an intermediate present reads them back
 * rather than keying and intersecting its interpolated pose, so the select
 * changes only where the game's own order changes, between two ticks, and a
 * present costs one lookup per face. Two tables are kept: the latest
 * canonical tick's (T) and the one before it (T-1). An intermediate present
 * shows a pose between T-1 (alpha 0) and T (alpha 1), and applies the verdict
 * of the nearer endpoint:
 * T-1's for alpha < 0.5, T's for alpha >= 0.5. The verdict therefore changes
 * once per tick interval, at its midpoint, and never leads the pose by more
 * than half a tick. A face the chosen endpoint did not rank (a mesh the
 * present holds from the previous clip, a face that tick culled, a T-1 table
 * that is not the tick adjacent to T) reads rank 0 and keeps true depth.
 * One canonical epoch can hold more than one scene build: a pause screen
 * feeds twice before its present, and runahead feeds the committed tick and
 * then the speculated ticks, whose last build is the one presented. Each
 * build empties the epoch's table before its first store, so the table holds
 * only the last build's verdict, the one the canonical present shows.
 * OPENPETE_MOBY_RANK_CARRY=0 recomputes on every present instead. */
#define RC_BITS 16
#define RC_SIZE (1 << RC_BITS)
/* An entry belongs to its table only while its generation is the table's, so
 * a new epoch empties a table by giving it a fresh generation. */
typedef struct { uint32_t moby, fkey; int32_t rank; uint32_t gen : 31, cont : 1; } rc_ent;
typedef struct {
    rc_ent   e[RC_SIZE];
    uint32_t gen;     /* generation of this table's live entries */
    uint32_t epoch;   /* canonical epoch the entries were stored under */
    unsigned scene;   /* native scene build (openpete_native_scene_frame) that stored them */
    int      n;
    int      valid;
} rc_tab;
static rc_tab   s_rc_tab[2];
static int      s_rc_cur;              /* index of the latest tick's table */
static uint32_t s_rc_gen;              /* last generation handed out */
static double   s_rc_alpha = 1.0;      /* the present's alpha (moby_native_set_verdict_alpha) */
static long     s_rc_hit, s_rc_miss;   /* engagement counters (sub-tick lookups) */
static int      s_rc_prev_used;        /* the last apply read the T-1 table */
static long     s_rc_rebuilds;         /* tables emptied by a second build within one epoch */
/** @brief Whether the sub-tick rank carry is on (OPENPETE_MOBY_RANK_CARRY).
 *
 * Read on the first call and latched.
 * @return 0 when the variable begins with '0', else 1. */
static int rank_carry_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_CARRY"); s = !(e && *e == '0'); }
    return s;
}
/* An unbracketed present shows the latest tick, so it takes alpha 1. */
void moby_native_set_verdict_alpha(double alpha, int bracketed) { s_rc_alpha = bracketed ? alpha : 1.0; }
/** @brief Hash a moby address and face key to a carry-table slot.
 *
 * @param m The moby's PSX address.
 * @param k The face key.
 * @return A slot in [0, RC_SIZE). */
static uint32_t rc_hash(uint32_t m, uint32_t k) {
    return ((m * 2654435761u) ^ (k * 2246822519u)) >> (32 - RC_BITS);
}
/** @brief Empty a carry table for the build that stores into it next.
 *
 * Hands out a fresh generation, so every entry of the table stops matching;
 * a generation wrap clears both tables first.
 * @param t The table.
 * @param ep The canonical epoch the build stores under.
 * @param scene The native scene build. */
static void rank_carry_empty(rc_tab* t, uint32_t ep, unsigned scene) {
    s_rc_gen = (s_rc_gen + 1) & 0x7FFFFFFFu;
    if (!s_rc_gen) {
        memset(s_rc_tab, 0, sizeof s_rc_tab);   /* wrap: both tables start empty */
        s_rc_gen = 1;
    }
    t->gen = s_rc_gen; t->n = 0; t->epoch = ep; t->scene = scene; t->valid = 1;
}
/** @brief The carry table a store from scene build scene under canonical epoch ep writes.
 *
 * The latest table while it is valid for ep and was stored by the same build,
 * so the material passes of one build accumulate. A later build under the
 * same epoch empties the latest table in place and leaves the T-1 table as it
 * is. The first store of a new epoch turns the latest table into the T-1
 * table and empties the other one for the new tick.
 * @param ep The canonical epoch.
 * @param scene The native scene build (openpete_native_scene_frame).
 * @return The table. */
static rc_tab* rank_carry_table_for(uint32_t ep, unsigned scene) {
    rc_tab* t = &s_rc_tab[s_rc_cur];
    if (t->valid && ep == t->epoch) {
        if (scene != t->scene) { rank_carry_empty(t, ep, scene); s_rc_rebuilds++; }
        return t;
    }
    s_rc_cur ^= 1;
    t = &s_rc_tab[s_rc_cur];
    rank_carry_empty(t, ep, scene);
    return t;
}
/** @brief Store this feed's face ranks in the carry table of its scene build.
 *
 * Runs for every feed that is not a sub-tick re-extract, runahead's
 * speculated builds included: the ranks come from the host walk of the build
 * the canonical present shows, not from guest scratch a later tick reuses.
 * The table is the one rank_carry_table_for gives the current canonical epoch
 * and native scene build. For each s_gout triangle with a positive s_nrank
 * and a nonzero moby it inserts or replaces the entry keyed by (moby, face
 * key) with the rank and the contest flag, probing linearly from rc_hash.
 * Once the table holds three quarters of RC_SIZE keys every remaining
 * triangle is skipped, replacements included. */
static void rank_carry_store(void) {
    rc_tab* t = rank_carry_table_for(spyro_native_canon_epoch_stamp(), openpete_native_scene_frame());
    for (int i = 0; i < s_gout.n; i++) {
        if (s_nrank[i] <= 0 || !s_wtri_moby[i] || t->n >= RC_SIZE - (RC_SIZE >> 2)) continue;
        uint32_t h = rc_hash(s_wtri_moby[i], s_wtri_fkey[i]);
        while (t->e[h].gen == t->gen &&
               !(t->e[h].moby == s_wtri_moby[i] && t->e[h].fkey == s_wtri_fkey[i]))
            h = (h + 1) & (RC_SIZE - 1);
        if (t->e[h].gen != t->gen) t->n++;
        t->e[h] = (rc_ent){ s_wtri_moby[i], s_wtri_fkey[i], s_nrank[i], t->gen, s_wtri_contest[i] };
    }
}
/** @brief Look up a face's carry entry in one table.
 *
 * Probes linearly from rc_hash and stops at the first slot whose generation
 * is not the table's, so entries of an earlier epoch never match.
 * @param t The table.
 * @param moby The moby's PSX address.
 * @param fkey The face key.
 * @return The entry, or NULL. */
static const rc_ent* rank_carry_find(const rc_tab* t, uint32_t moby, uint32_t fkey) {
    uint32_t h = rc_hash(moby, fkey);
    for (; t->e[h].gen == t->gen; h = (h + 1) & (RC_SIZE - 1))
        if (t->e[h].moby == moby && t->e[h].fkey == fkey) return &t->e[h];
    return NULL;
}
/** @brief The carry table whose verdict this present applies, or NULL when that endpoint has none.
 *
 * Alpha at or above 0.5 takes the latest table, below it the T-1 table. The
 * latest table counts only while its epoch is the last canonical present's
 * (spyro_native_canon_epoch_fresh), and the T-1 table only while its epoch is
 * the one directly before the latest. Records in s_rc_prev_used whether the
 * T-1 table was chosen.
 * @return The table, or NULL. */
static const rc_tab* rank_carry_endpoint(void) {
    const rc_tab* latest = &s_rc_tab[s_rc_cur];
    const rc_tab* prev   = &s_rc_tab[s_rc_cur ^ 1];
    s_rc_prev_used = 0;
    if (!latest->valid || !spyro_native_canon_epoch_fresh(latest->epoch)) return NULL;
    if (s_rc_alpha >= 0.5) return latest;
    s_rc_prev_used = 1;
    return (prev->valid && prev->epoch + 1u == latest->epoch) ? prev : NULL;
}
/** @brief Give this present's triangles the ranks and contest flags of its carry table.
 *
 * Each s_gout triangle takes the entry of its (moby, face key) from
 * rank_carry_endpoint's table into s_nrank and s_wtri_contest, and 0 for both
 * when there is no table, no moby, or no entry. Counts triangles with a
 * positive rank into s_rc_hit and the others into s_rc_miss, and empties the
 * intersection pair list (s_ip_n). */
static void rank_carry_apply(void) {
    const rc_tab* t = rank_carry_endpoint();
    for (int i = 0; i < s_gout.n; i++) {
        const rc_ent* e = (t && s_wtri_moby[i]) ? rank_carry_find(t, s_wtri_moby[i], s_wtri_fkey[i])
                                                 : NULL;
        s_nrank[i] = e ? e->rank : 0;
        s_wtri_contest[i] = e ? e->cont : 0;
        if (s_nrank[i] > 0) s_rc_hit++; else s_rc_miss++;
    }
    s_ip_n = 0;
}

/* Select census on the PSX pixel grid (pixel centres, either winding),
 * OPENPETE_MOBY_ISECT_LOG=1: replays the rank image's one-owner-per-pixel rule
 * (highest rank wins) over the ranked faces of this feed and counts, over
 * every ranked face's pixels, the discards the select applies (the owner is a
 * face of the same moby with a higher rank) against the ideal (some face of
 * the same moby with a higher rank covers the pixel). missed = ideal and not
 * applied: the owner is a face of another moby. */
static int s_isect_rk[MAX_NTRI];
#define ISG_X0 (-256)
#define ISG_Y0 (-256)
#define ISG_W 1024
#define ISG_H 768
typedef void (*isg_fn)(int face, int px, void* u);
/** @brief Call fn for each pixel centre a walk triangle covers, either winding, inside the census window.
 *
 * The scan box is the triangle's bounds clamped to the ISG_W x ISG_H window at
 * (ISG_X0, ISG_Y0). A pixel centre on an edge counts as covered. A triangle
 * of area below 1e-9 calls nothing.
 * @param i The walk triangle index into s_wtri.
 * @param fn Called with i, the pixel index (y - ISG_Y0) * ISG_W + (x - ISG_X0)
 *        and u.
 * @param u Passed to fn unchanged. */
static void isg_raster(int i, isg_fn fn, void* u) {
    const ntri_t* w = &s_wtri[i];
    double x0 = w->v[0].sx, y0 = w->v[0].sy, x1 = w->v[1].sx, y1 = w->v[1].sy,
           x2 = w->v[2].sx, y2 = w->v[2].sy;
    double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabs(area) < 1e-9) return;
    double sg = area > 0 ? 1.0 : -1.0;
    int bx0 = (int)floor(fmin(x0, fmin(x1, x2))), bx1 = (int)ceil(fmax(x0, fmax(x1, x2)));
    int by0 = (int)floor(fmin(y0, fmin(y1, y2))), by1 = (int)ceil(fmax(y0, fmax(y1, y2)));
    if (bx0 < ISG_X0) bx0 = ISG_X0;
    if (by0 < ISG_Y0) by0 = ISG_Y0;
    if (bx1 > ISG_X0 + ISG_W - 1) bx1 = ISG_X0 + ISG_W - 1;
    if (by1 > ISG_Y0 + ISG_H - 1) by1 = ISG_Y0 + ISG_H - 1;
    for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
        double px = x + 0.5, py = y + 0.5;
        double e0 = sg * ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0));
        double e1 = sg * ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1));
        double e2 = sg * ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2));
        if (e0 < 0 || e1 < 0 || e2 < 0) continue;
        fn(i, (y - ISG_Y0) * ISG_W + (x - ISG_X0), u);
    }
}
static int s_isg_own[ISG_W * ISG_H];   /* face index + 1 of the owner */
static int s_isg_grp[ISG_W * ISG_H];   /* max rank within the current moby */
/** @brief Raster callback: make a face the owner of a pixel when its rank exceeds the current owner's.
 *
 * The owner is stored as a face index plus 1 in s_isg_own, with 0 for none.
 * @param f The walk triangle index.
 * @param p The pixel index, (y - ISG_Y0) * ISG_W + (x - ISG_X0).
 * @param u Unused. */
static void isg_own(int f, int p, void* u) {
    (void)u; int o = s_isg_own[p];
    if (!o || s_isect_rk[o - 1] < s_isect_rk[f]) s_isg_own[p] = f + 1;
}
/** @brief Raster callback: clear the owner and the group rank of a pixel.
 * @param f The walk triangle index.
 * @param p The pixel index, (y - ISG_Y0) * ISG_W + (x - ISG_X0).
 * @param u Unused. */
static void isg_clr(int f, int p, void* u) { (void)f; (void)u; s_isg_grp[p] = 0; s_isg_own[p] = 0; }
/** @brief Raster callback: clear the group rank of a pixel.
 * @param f The walk triangle index.
 * @param p The pixel index, (y - ISG_Y0) * ISG_W + (x - ISG_X0).
 * @param u Unused. */
static void isg_clr_grp(int f, int p, void* u) { (void)f; (void)u; s_isg_grp[p] = 0; }
/** @brief Raster callback: raise the group rank of a pixel to a face's rank.
 * @param f The walk triangle index.
 * @param p The pixel index, (y - ISG_Y0) * ISG_W + (x - ISG_X0).
 * @param u Unused. */
static void isg_grp(int f, int p, void* u) {
    (void)u; if (s_isg_grp[p] < s_isect_rk[f]) s_isg_grp[p] = s_isect_rk[f];
}
typedef struct { long px, disc, ideal, missed; } isg_stat;
/** @brief Raster callback: count a face's pixel into the census as covered, discarded, ideal and missed.
 *
 * A pixel is discarded when its owner is a face of the same moby with a higher
 * rank, and ideal when the group rank exceeds the face's; missed is ideal and
 * not discarded.
 * @param f The walk triangle index.
 * @param p The pixel index, (y - ISG_Y0) * ISG_W + (x - ISG_X0).
 * @param u The isg_stat the counts accumulate into. */
static void isg_cnt(int f, int p, void* u) {
    isg_stat* st = (isg_stat*)u;
    st->px++;
    int o = s_isg_own[p] - 1;
    int applied = o >= 0 && s_wtri_moby[o] == s_wtri_moby[f] && s_isect_rk[o] > s_isect_rk[f];
    int ideal = s_isg_grp[p] > s_isect_rk[f];
    st->disc += applied;
    st->ideal += ideal;
    st->missed += ideal && !applied;
}
/** @brief Print the rank select's per-pixel discard census for this feed as an [isect-sel] line.
 *
 * The ranked faces are the eligible contested faces with a positive rank.
 * isg_raster covers each face over a 1024 x 768 pixel window at (-256, -256),
 * pixel centres, either winding. The census first records the highest-rank
 * owner of every pixel across all ranked faces (isg_own), then per moby records
 * the highest rank of that moby at each pixel (isg_grp) and counts, over each
 * face's pixels: px covered, disc where the owner is a face of the same moby
 * with a higher rank (the discard the select applies), ideal where some face of
 * the same moby with a higher rank covers the pixel, and missed where ideal
 * holds and disc does not. The pixel state is cleared as each moby finishes. */
static void isect_select_census(void) {
    static int ord[MAX_NTRI];
    int n = 0;
    for (int i = 0; i < s_gout.n; i++) {
        s_isect_rk[i] = (isect_eligible(i) && s_wtri_contest[i]) ? s_nrank[i] : 0;
        if (s_isect_rk[i] > 0) ord[n++] = i;
    }
    isg_stat st = {0};
    for (int k = 0; k < n; k++) isg_raster(ord[k], isg_clr, NULL);
    for (int k = 0; k < n; k++) isg_raster(ord[k], isg_own, NULL);
    for (int k0 = 0; k0 < n; ) {
        int k1 = k0;
        while (k1 < n && s_wtri_moby[ord[k1]] == s_wtri_moby[ord[k0]]) k1++;
        for (int k = k0; k < k1; k++) isg_raster(ord[k], isg_grp, NULL);
        for (int k = k0; k < k1; k++) isg_raster(ord[k], isg_cnt, &st);
        for (int k = k0; k < k1; k++) isg_raster(ord[k], isg_clr_grp, NULL);
        k0 = k1;
    }
    fprintf(stderr, "[isect-sel] vbl=%u mat=%s faces=%d px=%ld disc=%ld ideal=%ld missed=%ld\n",
            openpete_frame, s_cur_mat, n, st.px, st.disc, st.ideal, st.missed);
}

/** @brief Compare this feed's native rank against the game's own ordering table (OPENPETE_MOBY_RANK_CAL=1).
 *
 * An instrument: hands the walk's triangles, contest flags, ranks, paint keys
 * and intersection pairs to moby_rank_cal_run (moby_rank_cal.c). */
static void moby_rank_calibrate(void) {
    static uint8_t elig[MAX_NTRI];
    for (int i = 0; i < s_gout.n; i++) elig[i] = (uint8_t)isect_eligible(i);
    moby_rank_cal_in_t in = { s_gout.n, s_wtri_moby, (const int16_t (*)[6])s_wtri_isxy, s_nrank,
                              s_wtri_contest, elig, s_wtri_otk, s_wtri_fkey, s_ip_a, s_ip_b,
                              s_ip_n, s_cur_mat };
    moby_rank_cal_run(&in);
}

/** @brief Charge one rank stage's cycles and thread CPU time to slot.
 *
 * Adds the TSC cycles since t0 with count 0 (rt_add), and, when c0 is
 * nonzero, the thread CPU microseconds since c0 to s_rt_cpu_us[slot].
 * @param slot The stage, RT_ISECT or RT_RANK.
 * @param t0 The rt_now value at the stage start; 0 when the instrument is off.
 * @param c0 The rt_cpu_ns value at the stage start; 0 skips the CPU time. */
static void rank_stage_time(int slot, uint64_t t0, uint64_t c0) {
    rt_add(slot, t0, 0);
    if (c0) s_rt_cpu_us[slot] += (double)(rt_cpu_ns() - c0) / 1000.0;
}

/** @brief Produce the contest flags and native ranks of this feed.
 *
 * A canonical feed computes them (intersection census, then the rank build)
 * and stores them for the sub-tick carry; an intermediate present reads them
 * back from the carry while the carry is on and computes them otherwise.
 * Inert unless the native rank or the intersection log is on. Also emits the
 * rank-time, rank-carry and select census lines and runs the calibration
 * instrument when each is enabled. */
static void moby_rank_prepare(void) {
    if (!openpete_mobyrank_on() && !isect_log_on()) return;
    int carry = s_in_reextract && rank_carry_on();
    uint64_t t0 = rt_now(), c0 = rt_cpu_ns();
    if (carry) rank_carry_apply(); else isect_census();
    rank_stage_time(RT_ISECT, t0, c0);
    t0 = rt_now(); c0 = rt_cpu_ns();
    if (!carry) native_rank_build();
    if (!s_in_reextract) rank_carry_store();
    rank_stage_time(RT_RANK, t0, c0);
    rt_report(s_cur_mat, s_gout.n);
    if (isect_log_on()) {
        if (carry)
            fprintf(stderr, "[rank-carry] vbl=%u mat=%s tris=%d hit=%ld miss=%ld epoch=%u "
                            "alpha=%.3f endpoint=%s rebuilds=%ld\n",
                    openpete_frame, s_cur_mat, s_gout.n, s_rc_hit, s_rc_miss,
                    s_rc_tab[s_rc_cur].epoch, s_rc_alpha, s_rc_prev_used ? "prev" : "latest", s_rc_rebuilds);
        isect_select_census();
    }
    s_rc_hit = s_rc_miss = 0;
    if (moby_rank_cal_on()) moby_rank_calibrate();
}

/** @brief The rank a walk triangle carries into the select: its native paint rank while it contests another face of its moby, else 0.
 *
 * 0 also while the native rank is off (openpete_mobyrank_on) or the
 * triangle is not eligible for the contest (isect_eligible).
 * @param i The walk triangle's index.
 * @return The rank, or 0. */
static int moby_face_rank(int i) {
    return (openpete_mobyrank_on() && isect_eligible(i) && s_wtri_contest[i]) ? s_nrank[i] : 0;
}

/** @brief Log the walk triangles whose centroid falls in a PSX-screen box over a VBL range (OPENPETE_MOBY_TRIDBG).
 *
 * The variable is "x0,y0,x1,y1,fA,fB" and is parsed once. For each VBL in
 * [fA, fB] every triangle whose centroid is in the box prints one line with
 * its identity (moby, fkey), material, contest flag, rank, bucket word and
 * per-vertex screen position, depths, UV and colour, for an offline identity
 * diff across VBLs. */
static void moby_tri_debug(void) {
    static int s_td = -1; static int tx0, ty0, tx1, ty1, tfa, tfb;
    if (s_td < 0) {
        const char* e = getenv("OPENPETE_MOBY_TRIDBG");
        s_td = (e && sscanf(e, "%d,%d,%d,%d,%d,%d", &tx0, &ty0, &tx1, &ty1, &tfa, &tfb) == 6);
    }
    if (!s_td || (int)openpete_frame < tfa || (int)openpete_frame > tfb) return;
    for (int i = 0; i < s_gout.n; i++) {
        const ntri_t* w = &s_wtri[i];
        float cx = (w->v[0].sx + w->v[1].sx + w->v[2].sx) / 3.0f;
        float cy = (w->v[0].sy + w->v[1].sy + w->v[2].sy) / 3.0f;
        if (cx < tx0 || cx > tx1 || cy < ty0 || cy > ty1) continue;
        fprintf(stderr, "[mtri] vbl=%u i=%d m=%08X k=%08X tex=%d tp=%d,%d cl=%d,%d bl=%d fr=%d ui=%d "
                        "dofs=%d mg=%d ct=%d rk=%d bk=%d",
                openpete_frame, i, s_wtri_moby[i], s_wtri_fkey[i], w->textured,
                w->tpage_x, w->tpage_y, w->clut_x, w->clut_y, w->blend,
                s_wtri_front[i], s_wtri_ui[i], s_wtri_dofs[i], s_wtri_mangle[i],
                (int)s_wtri_contest[i], s_nrank[i], s_wtri_otk[i]);
        for (int k = 0; k < 3; k++)
            fprintf(stderr, " v%d=%.2f,%.2f,%.1f,%.1f,%d,%d,%06X", k,
                    w->v[k].sx, w->v[k].sy, w->v[k].vz, w->v[k].tvz,
                    w->v[k].u, w->v[k].v, w->v[k].rgb & 0xFFFFFF);
        fputc('\n', stderr);
    }
}

/** @brief A walk triangle's vertex alpha byte, in bits 24-31.
 *
 * 0xFF, or on a textured face carrying a second fogged-CLUT row, that row and
 * its weight (ntri_t.tmix, decoded from openpete_scene_vert.rgba).
 * @param w The walk triangle.
 * @return The alpha byte shifted to bits 24-31. */
static uint32_t moby_tri_alpha(const ntri_t* w) {
    return (w->textured && w->tmix) ? ((uint32_t)w->tmix << 24) : 0xFF000000u;
}

/* The scene the curated prop rows with a scene match against: the word at
 * game_layout.moby.prop_scene_addr while the scene state holds, else -1.
 * Latched once per canonical regular pass (moby_prop_scene_latch); the
 * sub-tick presents between two passes keep it. */
static int32_t s_prop_scene = -1;
/** @brief Latch the scene the curated prop rows match against.
 * @param cpu The guest CPU state, read only. */
static void moby_prop_scene_latch(CPUState* cpu) {
    const uint32_t a = game_layout.moby.prop_scene_addr, st = game_layout.moby.prop_state_addr;
    s_prop_scene = (a && st && cpu->read_word(st) == game_layout.moby.prop_state_scene)
                 ? (int32_t)cpu->read_word(a) : -1;
}
/** @brief Report whether a moby class is a curated embedded prop in the latched scene.
 *
 * Embedded props are static scenery authored sunk into the ground (the
 * k_lp_artfix seam pattern). Near-band terrain yields to them within the
 * embed bound and their own faces never yield to a terrain key, so they
 * render whole from every angle instead of the original's view-dependent
 * piece-key roulette. The list is curated per game (game_layout.moby.props),
 * because no geometric quantity separates an embedded prop from an actor
 * standing just behind a crest. OPENPETE_MOBY_PROP_LEGACY=1 matches classes
 * 250 and 332 in every game instead.
 * @param cls The moby's m_Class.
 * @return 1 for a prop class, 0 otherwise. */
static int moby_class_is_prop(uint16_t cls) {
    static int s_legacy = -1;
    if (s_legacy < 0) { const char* e = getenv("OPENPETE_MOBY_PROP_LEGACY"); s_legacy = e && *e == '1'; }
    if (s_legacy) return cls == 250u || cls == 332u;
    for (int k = 0; k < game_layout.moby.prop_n; k++) {
        if (game_layout.moby.props[k].cls != cls) continue;
        const int32_t sc = game_layout.moby.props[k].scene;
        if (sc < 0 || sc == s_prop_scene) return 1;
    }
    return 0;
}

/** @brief Return the rank sentinel an unranked opaque actor face carries into the key contest.
 *
 * The contest reads the sentinel as the class whose arms to run: an actor
 * face never yields to a prop key, and a prop face also never yields to a
 * terrain key.
 * @param prop Nonzero when the face belongs to a curated embedded prop.
 * @return OPENPETE_SCENE_RANK_PROP for a prop face, else
 *         OPENPETE_SCENE_RANK_ACTOR. */
static float moby_actor_rank_marker(int prop) {
    return prop ? OPENPETE_SCENE_RANK_PROP : OPENPETE_SCENE_RANK_ACTOR;
}

/** @brief Return the scene identity of the moby record at PSX address @p moby.
 *
 * The identity is the record's word index in the 2 MiB of PSX RAM plus one,
 * so two records live in the same present never share it and 0 stays free
 * for "no moby". It fits 19 bits plus the offset, below 2^24, so it travels
 * exactly through a float vertex channel and three 8-bit colour channels.
 * @param moby PSX address of the moby record, 0 for none.
 * @return The identity, 0 when @p moby is 0. */
static uint32_t moby_scene_identity(uint32_t moby)
{
    return moby ? ((moby & 0x1FFFFCu) >> 2) + 1u : 0u;
}

/** @brief Append the walk's moby triangles to the native scene, panel 0.
 *
 * Does nothing unless the moby render path is selected. Appends to the scene
 * already open (push_scene_moby opens the one of the current VBL counter),
 * which composes with the cyclorama: the cyclorama draws first and owns the
 * background fill, and the mobys append on top.
 * Each vertex carries its own view-space Z and the pre-bias perspective W, so
 * the Z-buffer sorts opaque faces independent of submission order. Each
 * triangle is stamped with its scene category from the walk's UI tag and
 * front flag (HUD3D, HUD, actor-front, actor), its ordering-table key for
 * opaque unmangled actor faces while key resolve is on, its rank while it contests a face of its own
 * moby, its material slot (resolved per moby at emit time, since slot ids are
 * interned per feed cycle) and whether a reveal added it. Writes host scene
 * state only. */
static void push_scene_moby_tris(void) {
    if (!render_path_enabled(RP_MOBY)) return;  /* --render-paths: mobys not selected */
    static int s_logged = 0;
    if (!s_logged && s_gout.n > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-native] feeding panel 0: %d moby tris from gamestate "
                "(independent face-stream walk) frame %u\n", s_gout.n, openpete_frame);
    }
    /* mobys cover shadows (ACTOR); HUD/UI mobys overlay on top. Stamped per-tri
     * from the walk's UI tag (0=ACTOR, 1=HUD flat overlay, 2=HUD3D depth-sorted
     * icon): the in-game 3D HUD icons (treasure/dragon/lives, the static g_Hud
     * range) go to HUD3D so their own faces Z-sort (depth-off paints them
     * inside-out); the flat near-OT text/2D billboards stay HUD. */
    moby_rank_prepare();
    moby_tri_debug();
    uint32_t rord_moby[256]; int rord_n = 0;  /* this walk's ranked-moby ordinals */
    /* Per-moby memo for the material-slot resolve below (faces of one moby
     * are contiguous in this stream). */
    uint32_t mat_last_moby = 0; uint16_t mat_last_slot = 0; int mat_have = 0;
    for (int i = 0; i < s_gout.n; i++) {
        int cat = s_wtri_ui[i] == 2 ? OPENPETE_SCENE_HUD3D
                : s_wtri_ui[i] == 1 ? OPENPETE_SCENE_HUD
                : s_wtri_front[i]   ? OPENPETE_SCENE_ACTOR_FRONT
                                    : OPENPETE_SCENE_ACTOR;
        openpete_scene_tri t = {0};
        for (int k = 0; k < 3; k++) {
            t.v[k].sx   = s_wtri[i].v[k].sx;
            t.v[k].sy   = s_wtri[i].v[k].sy;
            t.v[k].z    = openpete_native_scene_viewz(openpete_worldvz_full(s_wtri[i].v[k].vz));
            /* Perspective W: the vertex's unshifted view-Z (equal to z where
             * no OT pull moved it). */
            t.v[k].pvz  = openpete_native_scene_viewz(openpete_worldvz_full(s_wtri[i].v[k].tvz));
            t.v[k].u    = (float)s_wtri[i].v[k].u;
            t.v[k].v    = (float)s_wtri[i].v[k].v;
            t.v[k].rgba = (s_wtri[i].v[k].rgb & 0xFFFFFFu) | moby_tri_alpha(&s_wtri[i]);
        }
        t.textured = s_wtri[i].textured;
        t.tpage_x  = s_wtri[i].tpage_x; t.tpage_y = s_wtri[i].tpage_y;
        t.clut_x   = s_wtri[i].clut_x;  t.clut_y  = s_wtri[i].clut_y;
        t.bpp      = s_wtri[i].bpp;
        t.blend    = s_wtri[i].blend;
        t.mid      = moby_scene_identity(s_wtri_moby[i]);
        /* og OT key channel, moby twin of the terrain emit: the .s inserts
         * each moby face at its average-Z slot minus the authored
         * m_DepthOffset (the angle word's top byte, which the cull packs into record
         * w0 bits 16-23 / the shaded walk's rot>>24) - an ordering KEY,
         * never stored depth. The face's ABSOLUTE key rasters into the
         * MOBY key image (set 1, split from the terrain keys), where the
         * contest scopes the yield by the covering face's own TRUE view-Z
         * (MOBY_TRUESCOPE, psx_scene_key.frag): keyed terrain within the
         * moby's own authored pull yields per-pixel (a pedestal disc set
         * into sloped ground paints complete), while og's pull+push
         * compound - which paints a chest through the wall lip genuinely
         * in front of it - is not mirrored: a wall in front keeps
         * occluding by TRUE depth. The moby face contests nearer keys
         * except those its own moby owns (op_akey_select).
         * Blend/FRONT/HUD classes
         * stay unkeyed - and so do MANGLED faces (near-plane fixup /
         * coordinate saturation): their screen footprint is not the true
         * projection, so a flat key stamped across one discards terrain
         * far outside the moby's real silhouette (a right-at-the-camera
         * moby flooded a whole wall away). A genuinely-near moby needs no
         * key anyway - it is the viewer-side surface and TRUE depth
         * already wins its own pixels. */
        {
          if (moby_face_key_on() && cat == OPENPETE_SCENE_ACTOR && t.blend == 0 &&
              !s_wtri_mangle[i] &&
              s_wtri_moby[i] && openpete_native_key_resolve_on()) {
              int dofs = (int)s_wtri_dofs[i];
              double mean = ((double)s_wtri[i].v[0].tvz + (double)s_wtri[i].v[1].tvz
                           + (double)s_wtri[i].v[2].tvz) / 3.0;
              double key = mean - OPENPETE_OT_SLOT_WORLD * (double)dofs;
              if (key < 1.0) key = 1.0;      /* og floors the slot at 0 */
              t.koff = (float)(key + 0.5);
              /* Embedded-PROP classes: rendered whole from every angle. */
              t.prop = moby_class_is_prop(s_wtri_cls[i]);
          } }
        /* Rank: the face's paint position while it intersects another face
         * of its moby (moby_face_rank). Mangled faces (near fixup / coordinate
         * saturation) are never ranked, like they are never keyed: their
         * raster is not the true projection, so a rank stamped across one
         * would discard content far outside the real silhouette. */
        int rank = moby_face_rank(i);
        if (rank > 0) {
            int oi;
            for (oi = 0; oi < rord_n; oi++)
                if (rord_moby[oi] == s_wtri_moby[i]) break;
            if (oi == rord_n && rord_n < 256) rord_moby[rord_n++] = s_wtri_moby[i];
            if (oi < 255) { t.rank = (float)rank; t.mordinal = oi + 1; }
        } else if (cat == OPENPETE_SCENE_ACTOR && t.blend == 0) {
            /* Actor-fragment marker (still "unranked" to every rank
             * consumer): the key contest's prop-embed rule fires on
             * TERRAIN fragments only, so a statue standing on its disc
             * never yields the pixel to it (psx_scene_key.frag). A prop
             * face carries the prop marker, which also skips the contest's
             * terrain-key arm: terrain that yields to the prop is not
             * yielded to in turn, which would leave the pixel to neither. */
            t.rank = moby_actor_rank_marker(t.prop);
        }
        /* Per-instance material slot, resolved HERE rather than banked at
         * extraction. Slot ids are interned per feed cycle and this loop
         * runs AFTER scene_begin_frame() has reset that table, so a slot
         * captured during the walk names nothing by the time it would be
         * stamped - and because ids are handed out in first-seen order, a
         * stale one can even name another instance's block. Re-resolving
         * from the banked IDENTITY (moby ptr + class) is the same rule the
         * residency ladder applies everywhere else: bank identity, never
         * outcomes. Memoized on the moby ptr - the walk emits a moby's
         * faces contiguously, so this is one resolve per moby, not per
         * face. */
        uint16_t matslot = 0;
        if (openpete_mods_material_has_instance()) {
            if (mat_have && s_wtri_moby[i] == mat_last_moby) {
                matslot = mat_last_slot;
            } else {
                matslot = (uint16_t)openpete_mods_material_moby_slot(
                    NULL, s_wtri_cls[i], s_wtri_moby[i]);
                mat_last_moby = s_wtri_moby[i];
                mat_last_slot = matslot;
                mat_have = 1;
            }
        }
        openpete_native_scene_set_tag(
            OPENPETE_TAG_MOBY_SLOT(s_wtri_cls[i], matslot));
        /* Origin: a record the extended-LOD reveal appended is host-added
         * geometry, and the scene's content and world-frame verdicts are the
         * game's own to make. Memoized on the moby ptr like the material slot
         * above - the walk emits a moby's faces contiguously. */
        openpete_native_scene_set_host_added(moby_walk_tri_host_added(s_wtri_moby[i]));
        openpete_native_scene_add_tri(&t, cat, 0);
    }
    openpete_native_scene_set_tag(0);   /* leave the tagged emit loop clean */
    openpete_native_scene_set_host_added(0);
}

/** @brief Open the scene of the current VBL counter and append the walk's moby triangles to it (push_scene_moby_tris). */
static void push_scene_moby(void) {
    if (!render_path_enabled(RP_MOBY)) return;  /* --render-paths: mobys not selected */
    openpete_native_scene_begin_frame(openpete_frame);
    push_scene_moby_tris();
}

/* ---- projection A/B: native decode+project vs the emitted prim verts -- */
/** @brief Print the projection A/B of the native decode and projection against the captured primitive vertices.
 *
 * The prims carry the authoritative final SXY, so a close match proves the
 * decode and the RTPS+TR projection reproduce the GTE. Each s_gte vertex is
 * matched to its nearest s_proj vertex, saturated ones included; a match to a
 * saturated vertex counts as divergent (near-Z or IR clamp) and leaves the
 * clean denominator. A clean match within 4 px counts, split textured and
 * flat, with the floor match, the nearest-neighbour ambiguity (second-nearest
 * within 1 px) and the worst distance. The reverse check counts on-screen
 * unsaturated s_proj vertices with no prim vertex within 2 px, which
 * separates wrong decoded vertices from slot or topology misses. With `nrec`
 * <= 8 it also prints per record the native vertex count and the matches it
 * received. Reads s_proj, so project_all_records must have run.
 * @param nrec The pass's record count. */
static void compare_and_report(int nrec) {
    if (!s_proj_n) { fprintf(stderr, "[moby-verify vbl=%u] no records\n", openpete_frame); return; }

    int gv_n = 0, near_n = 0, floor_ok = 0, ambig = 0, divergent_n = 0;
    int tex_n = 0, tex_m = 0, flat_n = 0, flat_m = 0;  /* CLEAN (non-divergent) match split */
    double sum = 0, mx = 0, worst = -1, max_tiejump = 0;
    float wgx=0,wgy=0; int worst_code = 0;
    int rmatch[16] = {0}, rnat[16] = {0};   /* per-record diag: which moby mis-projects */
    for (int i = 0; i < s_proj_n; i++) if ((unsigned)s_proj[i].rec < 16) rnat[s_proj[i].rec]++;
    for (int g = 0; g < s_gte_n; g++) {
        prim_t* P = &s_gte[g];
        int it = P->textured;
        for (int k = 0; k < P->nv; k++) {
            float gx = P->x[k], gy = P->y[k];
            gv_n++;
            /* Nearest native vert INCLUDING saturated ones, so a prim vert maps
             * to its true native vert even when the GTE divide/IR clamp made the
             * two diverge; those are binned out as expected divergence rather
             * than scored as decode failures.
             * Near mobys (small view-Z, H>=2*SZ3) hit this a lot. */
            double best = 1e18, best2 = 1e18; int bn_sat = 1, bn_rec = 0; float bnx=0, bny=0, bnx2=0, bny2=0;
            for (int i = 0; i < s_proj_n; i++) {
                double dx = s_proj[i].sx - gx, dy = s_proj[i].sy - gy;
                double d = dx*dx + dy*dy;
                if (d < best) { best2 = best; bnx2 = bnx; bny2 = bny;
                                best = d; bnx = s_proj[i].sx; bny = s_proj[i].sy; bn_sat = s_proj[i].sat; bn_rec = s_proj[i].rec; }
                else if (d < best2) { best2 = d; bnx2 = s_proj[i].sx; bny2 = s_proj[i].sy; }
            }
            double dist = sqrt(best);
            if (bn_sat) { divergent_n++; continue; }   /* near-Z / IR-clamp divergence: expected */
            if (it) tex_n++; else flat_n++;            /* clean denominator */
            if (dist <= 4.0) {
                near_n++; sum += dist; if (dist > mx) mx = dist;
                if (it) tex_m++; else flat_m++;
                if ((unsigned)bn_rec < 16) rmatch[bn_rec]++;
                if (fabs(floor((double)bnx) - gx) <= 1.0 && fabs(floor((double)bny) - gy) <= 1.0)
                    floor_ok++;
                /* NN-ambiguity (jitter proxy): 2nd-nearest within 1px of nearest
                 * -> the nearest match can flip between passes. */
                if (best2 < 1e17 && sqrt(best2) - dist < 1.0) {
                    ambig++;
                    double jx = bnx2 - bnx, jy = bny2 - bny, jump = sqrt(jx*jx + jy*jy);
                    if (jump > max_tiejump) max_tiejump = jump;
                }
            }
            if (dist > worst && dist < 1e17) { worst = dist; wgx=gx; wgy=gy; worst_code = P->code; }
        }
    }
    int n_tex = 0, n_flat = 0;   /* per-frame split: confirms the non-textured (G3/G4) decode is exercised */
    for (int g = 0; g < s_gte_n; g++)
        if (s_gte[g].nv >= 3) { if (s_gte[g].textured) n_tex++; else n_flat++; }
    int clean = tex_n + flat_n;   /* prim verts whose nearest native vert is non-divergent */
    /* REVERSE check (decode-error vs slot-order discriminator): how many native
     * s_proj verts have NO prim vertex within 2px? Forward NN is order-blind and
     * hides wrong decoded verts; if this is ~0 every native vert is a real one
     * (so walk misses are pure slot/topology), else the decode emits bad verts. */
    int sp_unmatched = 0, sp_checked = 0;
    for (int i = 0; i < s_proj_n; i++) {
        if (s_proj[i].sat) continue;
        if (s_proj[i].sx < 0 || s_proj[i].sx > 512 || s_proj[i].sy < 0 || s_proj[i].sy > 240) continue;
        sp_checked++;
        double best = 1e18;
        for (int g = 0; g < s_gte_n; g++) for (int k = 0; k < s_gte[g].nv; k++) {
            double dx = s_gte[g].x[k]-s_proj[i].sx, dy = s_gte[g].y[k]-s_proj[i].sy;
            double d = dx*dx+dy*dy; if (d < best) best = d;
        }
        if (best > 4.0) sp_unmatched++;   /* >2px (squared 4) -> a native vert no prim has */
    }
    fprintf(stderr, "[moby-rev vbl=%u] onscreen native verts with NO prim within 2px: %d/%d "
            "(>0 => decode emits wrong verts; ~0 => misses are slot/topology)\n",
            openpete_frame, sp_unmatched, sp_checked);
    fprintf(stderr,
        "[moby-verify vbl=%u] recs=%d blend=%d primVerts=%d divergent(satNN)=%d | clean-match(<=4px)=%d/%d(%.1f%%) "
        "mean=%.3fpx | texMatch=%d/%d(%.1f%%) flatMatch=%d/%d(%.1f%%) | "
        "NN-amb=%d(%.1f%%) maxTieJump=%.1fpx | worst gte(%.0f,%.0f) d=%.1f code=%02X | "
        "prims=%d(tex=%d flat=%d)\n",
        openpete_frame, nrec, s_max_blend, gv_n, divergent_n, near_n, clean, clean?100.0*near_n/clean:0.0,
        near_n?sum/near_n:0.0,
        tex_m, tex_n, tex_n?100.0*tex_m/tex_n:0.0,
        flat_m, flat_n, flat_n?100.0*flat_m/flat_n:0.0,
        ambig, near_n?100.0*ambig/near_n:0.0, max_tiejump,
        wgx,wgy, worst, worst_code, s_gte_n, n_tex, n_flat);
    /* Per-record: native vert count + how many prim verts NN-matched to it.
     * A record with many native verts but ~0 matches is mis-projected (the
     * residual). w0 = the record flags word (LP/HP + per-axis sign). */
    if (nrec <= 8) {
        fprintf(stderr, "[moby-recs vbl=%u]", openpete_frame);
        for (int r = 0; r < nrec && r < 16; r++)
            fprintf(stderr, " rec%d{nat=%d hit=%d}", r, rnat[r], rmatch[r]);
        fprintf(stderr, "\n");
    }
}

/* ---- dispatch hook -------------------------------------------------- */
/* The probe's reporting window, in VBLs: `openpete_frame`, the VSync-yield
 * counter `--screenshot-on-frame` also keys on, which is the clock every
 * `vbl=` label in this file prints. OPENPETE_MOBY_VERIFY carries the window
 * as `<lo>` or `<lo>-<hi>`, so the VBLs asked for are the VBLs reported. The
 * values `0` and `1` keep their boolean meaning - `1` selects the default
 * window plus an every-200-VBL sample, so one boot reports on each level the
 * run loads - and OPENPETE_MOBY_VERIFY_FRAME moves that default window's
 * start. */
static unsigned s_verify_lo = 2000u, s_verify_hi = 2012u;
static int      s_verify_sparse = 1;
/** @brief Whether OPENPETE_MOBY_VERIFY turns the verify probe on, parsed once together with its VBL window.
 *
 * Unset, empty or starting with 0 is off. A value `1` or one that does not
 * start with a number selects the default window of 13 VBLs starting at
 * OPENPETE_MOBY_VERIFY_FRAME (default 2000) plus the every-200-VBL sample.
 * `<lo>` or `<lo>-<hi>` selects that window alone; a `<hi>` below `<lo>`
 * becomes `<lo>`.
 * @return Nonzero when the probe is on. */
static int moby_verify_enabled(void) {
    static int s = -1;
    if (s >= 0) return s;
    const char* v = getenv("OPENPETE_MOBY_VERIFY");
    if (!(v && v[0] && v[0] != '0')) { s = 0; return s; }
    s = 1;
    char* end = NULL;
    unsigned long lo = strtoul(v, &end, 10);
    if (end != v && !(lo == 1ul && *end == '\0')) {
        s_verify_lo = (unsigned)lo;
        s_verify_hi = (*end == '-') ? (unsigned)strtoul(end + 1, NULL, 10) : s_verify_lo;
        if (s_verify_hi < s_verify_lo) s_verify_hi = s_verify_lo;
        s_verify_sparse = 0;
    } else {
        const char* f = getenv("OPENPETE_MOBY_VERIFY_FRAME");
        s_verify_lo = f ? (unsigned)strtoul(f, NULL, 10) : 2000u;
        s_verify_hi = s_verify_lo + 12u;
        s_verify_sparse = 1;
    }
    return s;
}
/* This VBL is inside the probe's window. */
int moby_verify_vbl(void) {
    if (!moby_verify_enabled()) return 0;
    if (openpete_frame >= s_verify_lo && openpete_frame <= s_verify_hi) return 1;
    return s_verify_sparse && openpete_frame > s_verify_hi && openpete_frame % 200u == 0;
}

/* ---- overlay pass: fed at the canonical present -------------------------
 * The menu screens that draw a one-moby overlay pass (game_layout.moby.
 * overlay_moby) run it at the end of their Draw, after the state draw and its
 * VSync waits and just before DrawOTag; the menu update then moves that moby,
 * and the 2D of the next Draw lands in the scene the pass opened before that
 * scene is presented. The ordering table the guest presents beside that 2D is
 * the next Draw's, whose overlay pass draws the moby's updated state. So the
 * pass opens its scene and feeds nothing; at the canonical present
 * (moby_native_overlay_commit) the moby's record is rebuilt from its state
 * through the cull's rule (moby_game.reveal_record), replaces the pass's
 * record in the host record copy, and is walked into the same scene in front
 * of its 2D. The canonical present and the sub-tick re-extract, which replays
 * that copy against the tick-end snapshot pair, then show one pose. Where a
 * later regular pass of the same Draw recaptured the copy, the rebuild runs
 * in a borrowed copy and the re-extract replays the kept record
 * (moby_ovl_reextract). */
#define OVL_REC_WORDS 32u
static struct {
    CPUState* cpu;
    unsigned  frame;               /* the VBL whose scene the overlay pass opened */
    uint32_t  moby;                /* the overlay moby the pass drew */
    uint32_t  base;                /* the record copy's guest base at the pass */
    uint32_t  rec[OVL_REC_WORDS];  /* the pass's one record */
    unsigned  epoch;               /* the canonical epoch the pass ran in */
    int       pending;
} s_ovl_feed;
/* The rebuilt overlay record of a commit that borrowed the record copy, for
 * the sub-tick re-extract: the copy it replays holds the later cull's
 * records, so the overlay record is replayed in a borrowed copy of its own
 * (moby_ovl_reextract). */
static struct {
    int      valid;
    unsigned epoch;
    uint32_t base;
    uint32_t rec[OVL_REC_WORDS];
} s_ovl_rex;
/* Commit outcomes: rebuilt; fed the pass's own record (the overlay word
 * moved or the builder declined); held feeds dropped because their scene
 * closed before a canonical present; and commits that found the record copy
 * replaced by a later cull and borrowed it. */
static long s_ovl_rebuilt, s_ovl_kept, s_ovl_orphan, s_ovl_borrowed;

/* The record copy a later cull left, set aside while a commit walks the
 * overlay record in its place (ovl_copy_borrow / ovl_copy_return). */
static struct {
    uint32_t words, queued, base;
    int      ended, appended, append_n;
    uint32_t append_moby[REC_APPEND_MOBYS];
    uint32_t snap[REC_SNAP_WORDS];
} s_ovl_lent;

/* OPENPETE_OVERLAY_REBUILD_AB=1: the rebuilt record and its walk, kept until
 * the next overlay pass, whose cull record and walk must equal them. */
#define OVL_AB_TRIS  2048
static struct {
    int           armed;
    uint32_t      moby;
    unsigned      frame;
    moby_record_t rec;
    int           n;
    ntri_t        t[OVL_AB_TRIS];
    uint8_t       ui[OVL_AB_TRIS];
    long          cmp, rec_eq, verdict_eq, tri_eq;
    double        worst_xy;
} s_ovl_ab;

/** @brief Whether OPENPETE_OVERLAY_REBUILD_AB is set, read once.
 *
 * @return 1 when the overlay rebuild A/B is on, else 0. */
static int ovl_ab_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_OVERLAY_REBUILD_AB"); on = (e && *e && *e != '0'); }
    return on;
}

/** @brief Print the overlay rebuild A/B totals at exit. */
static void ovl_ab_report(void) {
    fprintf(stderr, "[ovl-ab] total compared=%ld record_equal=%ld verdict_equal=%ld walk_equal=%ld "
                    "worst_xy=%.4f rebuilt=%ld kept=%ld dropped=%ld borrowed=%ld\n",
            s_ovl_ab.cmp, s_ovl_ab.rec_eq, s_ovl_ab.verdict_eq, s_ovl_ab.tri_eq, s_ovl_ab.worst_xy,
            s_ovl_rebuilt, s_ovl_kept, s_ovl_orphan, s_ovl_borrowed);
}

/** @brief Keep the rebuilt overlay record and its walk for the next overlay pass to compare against.
 *
 * @param cpu The guest CPU state, read only.
 * @param moby The overlay Moby address. */
static void ovl_ab_store(CPUState* cpu, uint32_t moby) {
    static int reg = 0;
    if (!reg) { reg = 1; atexit(ovl_ab_report); }
    if (!moby_game.read_record(cpu, s_rec_snap_base, &s_ovl_ab.rec)) return;
    s_ovl_ab.n = s_gout.n < OVL_AB_TRIS ? s_gout.n : OVL_AB_TRIS;
    memcpy(s_ovl_ab.t, s_wtri, (size_t)s_ovl_ab.n * sizeof(ntri_t));
    memcpy(s_ovl_ab.ui, s_wtri_ui, (size_t)s_ovl_ab.n);
    s_ovl_ab.moby  = moby;
    s_ovl_ab.frame = openpete_frame;
    s_ovl_ab.armed = 1;
}

/** @brief The first field the walk reads that differs between two overlay records, or NULL when none does.
 *
 * Compares the keyframe-blend byte, the LOD verdict, header and keyframe A,
 * keyframe B and its header while the blend is nonzero (the cull leaves them
 * stale at blend 0, where the walk does not read them), the moby, the
 * material word, the translation and the matrix. The draw-verdict byte
 * (flags byte 0) is reported apart.
 * @param a The rebuilt record.
 * @param b The cull's record.
 * @return The field name, or NULL. */
static const char* ovl_ab_record_diff(const moby_record_t* a, const moby_record_t* b) {
    const uint32_t bl = (a->w0 >> 8) & 0xFFu;
    if (bl != ((b->w0 >> 8) & 0xFFu)) return "blend";
    if (a->tier != b->tier) return "tier";
    if (a->header != b->header) return "header";
    if (a->frame_a != b->frame_a) return "frame_a";
    if (bl && (a->header_b != b->header_b || a->frame_b != b->frame_b)) return "frame_b";
    if (a->moby != b->moby) return "moby";
    if (a->material != b->material) return "material";
    if (memcmp(a->TR_raw, b->TR_raw, sizeof a->TR_raw)) return "TR";
    if (memcmp(a->M, b->M, sizeof a->M)) return "M";
    return NULL;
}

/** @brief Compare this overlay pass's cull record and walk with the record rebuilt at the previous canonical present.
 *
 * The record fields the walk reads must be equal (ovl_ab_record_diff); each
 * triangle's material, texel and colour must be equal and its screen
 * position within 1/64 px. One line per pass.
 * @param cpu The guest CPU state, read only.
 * @param moby The overlay Moby address this pass drew. */
static void ovl_ab_compare(CPUState* cpu, uint32_t moby) {
    if (!s_ovl_ab.armed) return;
    s_ovl_ab.armed = 0;
    moby_record_t R;
    if (moby != s_ovl_ab.moby || s_rec_snap_queued != 1u || s_rec_snap_appended ||
        !moby_game.read_record(cpu, s_rec_snap_base, &R)) return;
    const char* rdiff = ovl_ab_record_diff(&s_ovl_ab.rec, &R);
    const int verdict_eq = (s_ovl_ab.rec.w0 & 0xFFu) == (R.w0 & 0xFFu);
    int mism = 0; double wxy = 0.0;
    const int n = s_gout.n;
    if (n != s_ovl_ab.n) mism = -1;
    for (int i = 0; mism >= 0 && i < n; i++) {
        const ntri_t* a = &s_ovl_ab.t[i]; const ntri_t* b = &s_wtri[i];
        int bad = a->textured != b->textured || a->tpage_x != b->tpage_x || a->tpage_y != b->tpage_y ||
                  a->clut_x != b->clut_x || a->clut_y != b->clut_y || a->bpp != b->bpp ||
                  a->blend != b->blend || s_ovl_ab.ui[i] != s_wtri_ui[i];
        for (int k = 0; k < 3; k++) {
            double dx = fabs((double)a->v[k].sx - b->v[k].sx), dy = fabs((double)a->v[k].sy - b->v[k].sy);
            if (dx > wxy) wxy = dx;
            if (dy > wxy) wxy = dy;
            bad |= a->v[k].u != b->v[k].u || a->v[k].v != b->v[k].v || a->v[k].rgb != b->v[k].rgb ||
                   dx > 1.0 / 64.0 || dy > 1.0 / 64.0;
        }
        mism += bad;
    }
    s_ovl_ab.cmp++;
    if (!rdiff) s_ovl_ab.rec_eq++;
    if (verdict_eq) s_ovl_ab.verdict_eq++;
    if (mism == 0) s_ovl_ab.tri_eq++;
    if (wxy > s_ovl_ab.worst_xy) s_ovl_ab.worst_xy = wxy;
    fprintf(stderr, "[ovl-ab vbl=%u built=%u] moby=%08X record=%s verdict=%02X/%02X blend=%02X "
                    "tris %d/%d mismatched=%d max_dxy=%.4f\n",
            openpete_frame, s_ovl_ab.frame, moby, rdiff ? rdiff : "equal",
            s_ovl_ab.rec.w0 & 0xFFu, R.w0 & 0xFFu, (R.w0 >> 8) & 0xFFu, s_ovl_ab.n, n, mism, wxy);
}

/** @brief Whether the host record copy holds exactly the cull's one record and it names a given moby.
 *
 * @param mob The Moby address.
 * @return 1 when it does, else 0. */
static int moby_ovl_single_record(uint32_t mob) {
    const uint32_t sw = game_layout.moby.rec_stride / 4u;
    return s_rec_snap_words && s_rec_snap_ended && s_rec_snap_queued == 1u && !s_rec_snap_appended &&
           sw != 0u && sw <= OVL_REC_WORDS && s_rec_snap[game_layout.moby.rec_moby_off / 4u] == mob;
}

/** @brief Hold an overlay pass's scene feed for the canonical present instead of pushing it now.
 *
 * Holds only a pass whose record copy is the overlay moby's one record
 * (moby_ovl_single_record), on a game with a record builder
 * (moby_game.reveal_record); any other pass keeps the draw-time feed. Opens
 * the pass's scene as push_scene_moby would and records the scene, the CPU
 * and the overlay moby for moby_native_overlay_commit.
 * @param cpu The guest CPU state, read only.
 * @return 1 when the feed is held, 0 when the caller pushes it. */
static int moby_ovl_defer(CPUState* cpu) {
    if (!moby_game.reveal_record) return 0;
    const uint32_t mob = cpu->read_word(game_layout.moby.overlay_moby);
    if (ovl_ab_on()) ovl_ab_compare(cpu, mob);
    if (!moby_ovl_single_record(mob)) return 0;
    if (s_ovl_feed.pending) s_ovl_orphan++;
    if (render_path_enabled(RP_MOBY)) openpete_native_scene_begin_frame(openpete_frame);
    memcpy(s_ovl_feed.rec, s_rec_snap, game_layout.moby.rec_stride);
    s_ovl_feed.base = s_rec_snap_base;
    s_ovl_feed.epoch = spyro_native_canon_epoch_stamp();
    s_ovl_feed.cpu = cpu;
    s_ovl_feed.frame = openpete_frame;
    s_ovl_feed.moby = mob;
    s_ovl_feed.pending = 1;
    return 1;
}

/** @brief Replace the host record copy's single overlay record with one rebuilt from the moby's state.
 *
 * Runs only while the copy holds exactly the cull's one record, naming the
 * overlay moby. The builder appends after an emptied copy; the result is then
 * re-labelled as the cull's own record (queued, not appended), since it
 * stands in for the record the next overlay pass's cull writes, and keeps the
 * pass's draw-verdict byte (record byte 0), which the builder sets to the
 * near verdict and the overlay cull sets to its own. A declined build puts
 * the pass's record back.
 * @param cpu The guest CPU state, read only.
 * @param mob The overlay Moby address.
 * @return 1 when the copy holds the rebuilt record, 0 when it holds the pass's. */
static int moby_ovl_rebuild_record(CPUState* cpu, uint32_t mob) {
    if (!moby_ovl_single_record(mob)) return 0;
    const uint32_t sw = game_layout.moby.rec_stride / 4u;
    uint32_t keep[OVL_REC_WORDS];
    memcpy(keep, s_rec_snap, sw * 4u);
    s_rec_snap_queued = 0u;
    moby_rec_append_reset();
    moby_reveal_reason_t why = MOBY_REVEAL_OK;
    const int ok = moby_game.reveal_record(cpu, mob, MOBY_REVEAL_ANIMATED, &why) &&
                   s_rec_snap_appended == 1;
    if (!ok) memcpy(s_rec_snap, keep, sw * 4u);
    else     s_rec_snap[0] = (s_rec_snap[0] & ~0xFFu) | (keep[0] & 0xFFu);
    s_rec_snap[sw] = 0u;
    s_rec_snap_words = sw + 1u;
    s_rec_snap_queued = 1u;
    s_rec_snap_appended = 0;
    s_rec_append_moby_n = 0;
    s_rec_append_gen++;
    return ok;
}

/** @brief Set aside the record copy a later cull left and install one overlay record in its place.
 *
 * The copy is restored by ovl_copy_return.
 * @param rec The record's words, game_layout.moby.rec_stride bytes.
 * @param base The guest base the record is read at. */
static void ovl_copy_borrow(const uint32_t* rec, uint32_t base) {
    const uint32_t sw = game_layout.moby.rec_stride / 4u;
    s_ovl_lent.words = s_rec_snap_words;  s_ovl_lent.queued = s_rec_snap_queued;
    s_ovl_lent.base = s_rec_snap_base;    s_ovl_lent.ended = s_rec_snap_ended;
    s_ovl_lent.appended = s_rec_snap_appended;
    s_ovl_lent.append_n = s_rec_append_moby_n;
    memcpy(s_ovl_lent.append_moby, s_rec_append_moby, sizeof s_rec_append_moby);
    memcpy(s_ovl_lent.snap, s_rec_snap, (size_t)s_rec_snap_words * 4u);
    memcpy(s_rec_snap, rec, sw * 4u);
    s_rec_snap[sw] = 0u;
    s_rec_snap_words = sw + 1u;  s_rec_snap_queued = 1u;
    s_rec_snap_base = base;
    s_rec_snap_ended = 1;  s_rec_snap_appended = 0;  s_rec_append_moby_n = 0;
    s_rec_append_gen++;
}

/** @brief Restore the record copy ovl_copy_borrow set aside. */
static void ovl_copy_return(void) {
    s_rec_snap_words = s_ovl_lent.words;  s_rec_snap_queued = s_ovl_lent.queued;
    s_rec_snap_base = s_ovl_lent.base;    s_rec_snap_ended = s_ovl_lent.ended;
    s_rec_snap_appended = s_ovl_lent.appended;
    s_rec_append_moby_n = s_ovl_lent.append_n;
    memcpy(s_rec_append_moby, s_ovl_lent.append_moby, sizeof s_rec_append_moby);
    memcpy(s_rec_snap, s_ovl_lent.snap, (size_t)s_ovl_lent.words * 4u);
    s_rec_append_gen++;
}

void moby_native_overlay_commit(void) {
    if (!s_ovl_feed.pending) return;
    s_ovl_feed.pending = 0;
    s_ovl_rex.valid = 0;
    if (openpete_native_scene_frame() != s_ovl_feed.frame) { s_ovl_orphan++; return; }
    /* A later cull (a regular pass after the overlay pass in the same Draw)
     * replaced the copy: walk the overlay record in a borrowed copy. */
    const int borrow = !moby_ovl_single_record(s_ovl_feed.moby) || s_rec_snap_base != s_ovl_feed.base;
    if (borrow) { s_ovl_borrowed++; ovl_copy_borrow(s_ovl_feed.rec, s_ovl_feed.base); }
    CPUState* cpu = s_ovl_feed.cpu;
    const uint32_t mob = cpu->read_word(game_layout.moby.overlay_moby);
    moby_view_build(cpu);
    const int rebuilt = mob == s_ovl_feed.moby && moby_ovl_rebuild_record(cpu, mob);
    if (rebuilt) s_ovl_rebuilt++; else s_ovl_kept++;
    s_cur_mat = "REG";
    s_rec_base = s_rec_snap_base;
    s_pass_overlay = 1;
    project_all_records(cpu);
    build_walk_tris(cpu);
    if (rebuilt && ovl_ab_on()) ovl_ab_store(cpu, mob);
    /* The held scene is still the open one; the next Draw's 2D is in it, so
     * the triangles go in front of that 2D, where the pass would have fed them. */
    openpete_native_scene_front_begin();
    push_scene_moby_tris();
    openpete_native_scene_front_end();
    s_pass_overlay = 0;
    if (borrow) {
        memcpy(s_ovl_rex.rec, s_rec_snap, game_layout.moby.rec_stride);
        s_ovl_rex.base  = s_rec_snap_base;
        s_ovl_rex.epoch = s_ovl_feed.epoch;
        s_ovl_rex.valid = 1;
        ovl_copy_return();
    }
}

/** @brief One canonical regular or specular moby pass: run the game's draw, then decode, feed and verify its records natively.
 *
 * Both passes loop the same cull-record queue with the same vertex decode,
 * RTPS projection and face walk; only the per-face highlight colour differs.
 * The pass captures the queue base (s_rec_base_canon) for the sub-tick
 * replay, runs the game's draw once, records the polybuf interval it wrote
 * (RP_MOBY), captures its prims into s_gte, and projects every record
 * natively. The regular pass snapshots its moby set so the specular pass
 * feeds only the mobys regular did not: re-feeding a shared moby from the
 * specular pass would paint a second pose, since the specular cull has
 * advanced its keyframe and blend state. With the scene feed on, the regular
 * pass appends the reveal records, the walk builds and pushes the triangles,
 * and the canonical capture takes the colour scratch, the records and the
 * camera for the sub-tick spine; the regular pass invalidates the specular
 * capture, which the specular pass revalidates only when the game runs it.
 * The path is marked drawn when the regular pass appends reveal records, and
 * when walk triangles exist on a tick whose game interval is empty. With
 * OPENPETE_MOBY_VERIFY the pass compares the native decode and walk with the
 * captured prims inside the probe window. After the game's draw the pass only
 * reads guest state, except the keyframe faceblock patch mirror, which writes
 * and restores within one decode.
 * @param cpu The guest CPU state.
 * @param draw_addr The game's draw entry for this material.
 * @param mat The material tag, "REG" or "SPEC". */
static void moby_native_pass(CPUState* cpu, uint32_t draw_addr, const char* mat) {
    int verify = moby_verify_enabled();
    int feed   = openpete_native_scene_enabled();
    s_cur_mat = mat;
    s_rec_base = moby_rec_base(cpu);
    s_rec_base_canon = s_rec_base;
    (void)mat;
    moby_view_build(cpu);

    uint32_t start = cpu->read_word(game_layout.polybuf.cursor);
    { OP_PROF_ACCUM_SCOPE("moby.srend");
      moby_run_draw(cpu, draw_addr); }         /* run the real renderer */
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);

    render_path_record(RP_MOBY, start, end);  /* polybuf range for the OT filter */

    /* Verify aid: the pass's polybuf window - pairs with the full-OT dump
     * (OPENPETE_OT_DUMP_FRAMES prim addresses) to attribute un-fed prims to
     * their producer by allocation range. */
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-window %s vbl=%u] polybuf [%08X,%08X) %u bytes\n",
                mat ? mat : "?", openpete_frame, start, end, end - start);

    { OP_PROF_ACCUM_SCOPE("moby.capture");
      capture_gte(cpu, start, end); }
    int nrec;
    { OP_PROF_ACCUM_SCOPE("moby.project");
      nrec = project_all_records(cpu); }      /* native float verts -> s_proj */
    if (tier_dbg_on() && !(mat && mat[0]=='S' && mat[1]=='P')) {   /* ~1/sec, REG+SPEC records pooled */
        static int s_tier_cnt = 0;
        if (++s_tier_cnt >= 30) {
            fprintf(stderr, "[moby-tier vbl=%u] near=%ld fog=%ld far=%ld farfade=%ld gpfskip=%ld lpskip=%ld shband=%ld | "
                    "zsf4-mirror ok=%ld bad=%ld | "
                    "proj=%ldus walk=%ldus rexwalk=%ldus dec=%ldus fld=%ldus rvdec=%ldus rvfld=%ldus rv=%ldus fan=%ldus mrg=%ldus faces=%ld/%ld calls=%ld fc=%ld/%ld fp=%ld/%ld clerp=%ld hold=%ld par=%ld/%ld fcovf=%ld/%ld\n",
                    openpete_frame, s_tier_near, s_tier_fog, s_tier_far, s_tier_farfade,
                    s_tier_gpfskip, s_tier_lpskip, s_shband_cull, s_tier_mirror_ok, s_tier_mirror_bad,
                    s_us_proj, s_us_walk, s_us_walk_rex, s_us_dec, s_us_fields,
                    s_us_rvdec, s_us_rvfields, s_us_reveal,
                    s_us_fanout, s_us_merge, s_walk_faces, s_walk_runs,
                    s_us_calls, s_fc_hit, s_fc_miss,
                    s_fp_hit, s_fp_miss, s_clerp_recs, s_hold_replayed,
                    s_walk_par, s_walk_par + s_walk_serial,
                    s_fc_ovf_used, s_fc_ovf_full);
            s_tier_near = s_tier_fog = s_tier_far = s_tier_farfade = s_tier_gpfskip = s_tier_lpskip = s_shband_cull = 0;
            s_tier_mirror_ok = s_tier_mirror_bad = 0; s_tier_cnt = 0;
            s_us_proj = s_us_walk = s_us_walk_rex = s_us_calls = 0;
            s_us_dec = s_us_fields = s_us_rvdec = s_us_rvfields = s_us_reveal = 0;
            s_us_fanout = s_us_merge = 0;
            s_walk_faces = s_walk_runs = 0;
            s_fc_hit = s_fc_miss = 0; s_hold_replayed = 0; s_clerp_recs = 0;
            s_fc_ovf_used = s_fc_ovf_full = 0;
            s_walk_par = s_walk_serial = 0;
        }
    }

    /* The .s runs REGULAR (func_8001F798) and SPECULAR (func_80020F34) over the
     * same +0x1600 record queue; SPECULAR re-draws the identical geometry to overlay
     * an additive per-face specular highlight (PSX can't bake it into one pass), so
     * the two .s passes OVERLAP at one pose. The native bakes the specular shading
     * per-face in build_walk_tris already, so re-feeding the same moby from the
     * SPECULAR pass is redundant - AND wrong-pose: the native re-projects AFTER
     * func_80020F34 (and the intervening specular cull) has advanced the per-record
     * keyframe/blend state, so the SPEC feed paints a SECOND, different animation
     * pose over the REGULAR one (the "death animation shows 2 frames at once"
     * doubling - e.g. a dying moby read blend=3072 in REG, blend=0 in SPEC). So
     * feed each moby ONCE: the REGULAR pass feeds its whole set and snapshots it
     * (s_reg_moby); the SPECULAR pass feeds ONLY mobys REGULAR did not (build_walk_tris
     * skips records whose moby is in s_reg_moby) - which keeps specular-ONLY mobys
     * (e.g. the rescue dragon the specular cull adds to a slot a regular moby held at
     * REGULAR-read time; also the disjoint specular queue under --no-lod). */
    int is_spec = (mat && mat[0]=='S' && mat[1]=='P');
    canon_drawn_harvest(nrec);  /* the drawn set for the sub-tick interp walks */
    if (is_spec) {              /* snapshot the SPEC moby set for the reveal skip */
        s_specq_n = 0;
        for (int r = 0; r < nrec && s_specq_n < MAX_RECORDS; r++)
            s_specq_moby[s_specq_n++] = s_recs[r].moby;
    }
    if (!is_spec) moby_build_validate(cpu);   /* OPENPETE_MOBY_BUILDDBG: host-builder A/B vs the .s records */
    if (!is_spec) {                           /* REGULAR runs first - snapshot its moby set */
        s_reg_moby_n = 0;
        for (int r = 0; r < nrec && s_reg_moby_n < MAX_RECORDS; r++)
            s_reg_moby[s_reg_moby_n++] = s_recs[r].moby;
    }
    if (feed) {
        if (!is_spec) {
            /* Host-built records are native-only - they never reach the .s
             * polybuf, so a present whose mobys came ENTIRELY from the reveal
             * records no RP_MOBY interval and reads as "canonical drew no
             * mobys". moby_reextract then skips every sub-tick pass and
             * the mobys exist on canonical presents alone: solid at
             * --render-fps 0, flickering-to-absent the moment presents outrun
             * ticks. That is the state past the canonical cull distance, where
             * the .s queue is empty by construction - so mark the path drawn on
             * the reveal's own yield. Can only fire when the .s REGULAR render
             * fn actually ran this tick (this is its hook), so a load screen -
             * where only the SHADED/text producer runs - still cannot mark it. */
            if (append_revealed_regular(cpu) > 0) render_path_mark_drawn(RP_MOBY);
        }
        { OP_PROF_ACCUM_SCOPE("moby.build");
          build_walk_tris(cpu); }             /* face-stream walk; SPEC dedups vs s_reg_moby; shading baked */
        /* The walk admits what its own cull mirror admits; on a tick where the
         * .s pass emitted nothing (an empty interval: the portal-entry tick
         * culls every record) but the walk still produced tris, the path
         * must read canon-drawn or the spine drops those tris on every
         * intermediate present while the canonical present shows them. */
        if (s_gout.n > 0 && end <= start) render_path_mark_drawn(RP_MOBY);
        { OP_PROF_ACCUM_SCOPE("moby.feed");
          /* the overlay pass feeds at the canonical present (moby_ovl_defer) */
          if (is_spec || !s_pass_overlay || !moby_ovl_defer(cpu))
              push_scene_moby(); }            /* recolor_walk_from_prims dropped: colour already native
                                               * (walk +0x1100/base), env-UV now native (build_walk_tris) */
        /* Sub-tick spine: snapshot the cull records + per-vertex fog colour
         * scratch (both live now) so intermediates can re-derive these mobys.
         * REGULAR feeds the shared bracket; the SPECULAR pass rebuilds the
         * +0x1600 queue with the specular set (incl. specular-ONLY mobys like
         * the rescue dragon that REGULAR didn't draw), so capture it separately
         * for the re-extract's SPEC pass - else those mobys vanish on sub-ticks. */
        /* colour row is [0x1100,0x1600); the old 0x800 over-capture spilled
         * into the records, which capture_recs covers in full anyway. */
        {
            /* Runahead: the whole capture/invalidate tail is canonical-only.
             * A speculative REGULAR pass clearing s_canon_spec_valid (with
             * the guarded spec capture unable to re-validate) skipped the
             * SPEC replay on every intermediate - specular mobys strobed. */
            if (spyro_native_canon_capture_open()) {
                if (!is_spec) {
                    spyro_native_canon_capture_range(cpu, 0x1100u, 0x500u);
                    spyro_native_canon_capture_recs(cpu);    /* full records (>13 overflow 0x1900) */
                    s_canon_overlay = s_pass_overlay;
                    spyro_native_canon_capture_camera(cpu, SPYRO_PROD_MOBYREG);
                    /* Invalidate the SPECULAR capture at the START of each canonical
                     * moby render (REGULAR runs first every frame). The game only calls
                     * func_80020F34 when there ARE specular mobys to draw; once the last
                     * one goes (e.g. the rescue-dragon CRYSTAL statue, cls 0xFA, whose
                     * moby flips to state 0xFD when the dragon breaks out), the SPEC pass
                     * stops firing and never re-captures - so without this clear the
                     * re-extract would replay the last frozen s_canon_spec FOREVER,
                     * leaving the crystal statue drawn on every sub-tick present after the
                     * dragon is freed. The SPEC pass below re-validates it iff it runs
                     * this frame; if it doesn't, the re-extract correctly skips SPEC. */
                    s_canon_spec_valid = 0;
                } else {
                    moby_native_capture_spec_scratch(cpu);
                }
            }
        }
    }

    if (verify) {
        static int s_dumped = 0, s_reports = 0;
        int target = (s_gte_n > 0 && s_reports < 120 && moby_verify_vbl());
        if (target && !s_dumped) { hex_dump(cpu, start, end); s_dumped = 1; }
        if (target) {
            compare_and_report(nrec);
            build_walk_tris(cpu);             /* independent walk + topology A/B */
            compare_walk_vs_prims();
            if (mat && mat[0] == 'S' && mat[1] == 'P') {   /* confirm specular recolor attaches the prim colour */
                specular_env_calibrate(cpu);  /* env-UV calibration dump (native refl vs prim UV) */
                specular_hl_calibrate(cpu);   /* highlight calibration (face-normal.light vs prim) */
                recolor_walk_from_prims();
                compare_walk_vs_prims();      /* the post-recolor col= should jump high */
            }
            s_reports++;
        }
        static unsigned s_codes_at = 0;
        if (s_gte_n > 0 && openpete_frame >= s_codes_at + 2000) {
            s_codes_at = openpete_frame; report_code_hist();
        }
    }
}

/** @brief Census of the mobys a wider view reveals, split by whether gameplay updates them (OPENPETE_MOBY_REVEALDBG).
 *
 * Walks the moby array, repeats the cull's behind-camera and render-distance
 * tests and its lateral cone test (r_moby.s 0x80022C04) at 4:3 and at the
 * view's widening F/s, and classifies each moby inside the wide cone and
 * outside the 4:3 one as live or frozen. Live is the update list's own gate,
 * which does not depend on the camera angle: m_WasDrawn set, an
 * always-update m_UpdateDistance sentinel (>= 0x80), or the player within
 * m_UpdateDistance * 1024 on the XY plane. A live revealed moby holds its
 * current gameplay pose; a frozen one holds a stale pose. Reads gamestate
 * only; prints one line, once, at the first call at or after VBL 1850.
 * @param cpu The guest CPU state, read only. */
static void moby_reveal_dbg(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_REVEALDBG"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    static int s_done = 0;
    if (s_done || openpete_frame < 1850u) return;
    s_done = 1;

    ngte_view v; ngte_view_build(cpu, &v);
    float F = v.F;                                   /* 1.0 at native 4:3 */
    int M[3][3]; memcpy(M, v.cam_M, sizeof M);       /* moby transform uses ProjMatrix */
    int camx = (int32_t)v.cam_ram[0];
    int camy = (int32_t)v.cam_ram[1];
    int camz = (int32_t)v.cam_ram[2];
    int spx = (int32_t)cpu->read_word(moby_game.player_pos + 0x00);
    int spy = (int32_t)cpu->read_word(moby_game.player_pos + 0x04);

    uint32_t m = moby_array_head(cpu);
    int alive=0, drawn_s=0, in43=0, inwide=0, revealed=0, rev_live=0, rev_frozen=0;
    int guard = 0;
    while (m && guard++ < 4096) {
        uint8_t st = cpu->read_byte(m + game_layout.moby.state_off);
        if (st == 0xFF) break;                       /* sentinel ends the array */
        if (st >= 0x80) { m += game_layout.moby.array_stride; continue; }     /* dead/inactive */
        alive++;
        uint8_t wasDrawn = cpu->read_byte(m + game_layout.moby.drawn_off);
        uint8_t updDist  = cpu->read_byte(m + moby_game.update_dist_off);
        uint8_t rRad     = cpu->read_byte(m + moby_game.render_radius_off);
        if (wasDrawn) drawn_s++;

        int mx = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u);
        int my = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u);
        int mz = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
        /* camera-relative, .s moby swizzle V=(dy,dz,dx)>>2 through ProjMatrix */
        double dx = (double)((mx - camx) >> 2);
        double dy = (double)((camy - my) >> 2);
        double dz = (double)((camz - mz) >> 2);
        double rx = (M[0][0]*dy + M[0][1]*dz + M[0][2]*dx) / 4096.0;
        double rz = (M[2][0]*dy + M[2][1]*dz + M[2][2]*dx) / 4096.0;
        double radius = (double)rRad * 256.0;        /* m_RenderRadius<<8 */
        if (rz + 128.0 <= 0.0) { m += game_layout.moby.array_stride; continue; }   /* behind camera */
        if (rz - radius >= 0.0) { m += game_layout.moby.array_stride; continue; }  /* beyond render distance */
        double arx = rx < 0 ? -rx : rx;
        /* X-cone (r_moby.s 0x80022C04): cull if 4*(|rx|-102) - 3*S*(rz+77) >= 0;
         * S=1 -> 4:3 (slope 0.75), S=F/s -> the view's horizontal half-FOV
         * (Hor+ widens by F, a wider FOV divides H by s). */
        int pass43   = (4.0*(arx-102.0) - 3.0*       (rz+77.0)) < 0.0;
        int passwide = (4.0*(arx-102.0) - 3.0*((double)F / v.s)*(rz+77.0)) < 0.0;
        if (pass43)   in43++;
        if (passwide) inwide++;
        if (passwide && !pass43) {
            revealed++;
            int live = wasDrawn != 0;                /* render grace = on update list */
            if (!live && updDist != 0) {             /* else the Spyro-anchored distance gate */
                if (updDist >= 0x80) live = 1;       /* always-update sentinel */
                else {
                    double sdx = (double)(mx - spx), sdy = (double)(my - spy);
                    double thr = (double)updDist * 1024.0;
                    if (sdx*sdx + sdy*sdy <= thr*thr) live = 1;
                }
            }
            if (live) rev_live++; else rev_frozen++;
        }
        m += game_layout.moby.array_stride;
    }
    fprintf(stderr,
        "[moby-reveal] frame=%u F=%.3f alive=%d drawn(.s m_WasDrawn)=%d  "
        "myCull{4:3=%d wide=%d}  REVEALED=%d (live=%d frozen=%d)\n",
        openpete_frame, F, alive, drawn_s, in43, inwide, revealed, rev_live, rev_frozen);
}

/** @brief The cull hook: run the game's cull, then copy its record list before any draw pass reuses the scratch it sits in.
 *
 * Snapshots the drawn-verdict bytes the cull will test (moby_precull_snapshot)
 * first. The cull is called exactly once, as the fall-through would have
 * called it, and nothing is written back.
 * @param cpu The guest CPU state.
 * @param addr The cull routine's guest address.
 * @return 1 when the hook ran the cull; 0 when no consumer is active
 *         (verify, native scene, render paths) or on re-entry, leaving the
 *         fall-through to run it. */
static int on_cull(CPUState* cpu, uint32_t addr) {
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_precull_snapshot(cpu);
    moby_run_draw(cpu, addr);
    moby_rec_snapshot(cpu);
    s_in = 0;
    return 1;
}

/** @brief The regular-pass hook: run the game's regular moby draw and feed its records to the native scene.
 *
 * Clears the per-moby fade snapshot first. Where the game reveals records
 * the cull skipped, builds the pass view, drops last tick's appended records
 * and appends this tick's (moby_reveal_candidates) before the pass walks the
 * copy. Reads the drawn-verdict bytes and writes none.
 * @param addr The hooked address, unused.
 * @return 1 when the hook ran the pass; 0 when no consumer is active
 *         (verify, native scene, render paths) or on re-entry, leaving the
 *         ordinary dispatch to run it. */
static int on_draw_regular(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    fade_snap_clear();                             /* refresh per-moby ZSF4 fade snapshot this canonical frame */
    moby_prop_scene_latch(cpu);
    s_pass_overlay = moby_overlay_pass_now(cpu);
    /* Extended LOD: the cull's distance box is what keeps a distant animated
     * moby out of its record list, so the records it rejected are appended to
     * the host copy of that list here and walked with the queued ones. The
     * view is built first because the appended record's rotation and centre
     * are view-space against the camera this pass projects through. */
    if (moby_game.reveal_record) {
        moby_view_build(cpu);
        /* Last tick's appended records go first: the cull hook refilled the
         * copy with the cull's own list, and a tick whose regular pass runs
         * without a cull hook behind it rebuilds the appends rather than
         * growing them. */
        moby_rec_append_reset();
        moby_reveal_candidates(cpu, MOBY_REVEAL_ANIMATED);
    }
    moby_native_pass(cpu, game_layout.moby.hook_draw_regular, "REG");
    moby_reveal_dbg(cpu);                          /* widescreen-reveal sizing (env-gated) */
    s_pass_overlay = 0;
    s_in = 0;
    return 1;
}
/** @brief The specular-pass hook: run the game's specular draw and feed the native scene.
 *
 * Runs moby_native_pass under material tag "SPEC" over the record queue the
 * regular pass walked.
 * @param cpu The guest CPU state.
 * @param addr The hooked address, unused.
 * @return 1 when the hook ran the pass; 0 when no consumer is active
 *         (verify, native scene, render paths) or on re-entry, leaving the
 *         ordinary dispatch to run it. */
static int on_draw_specular(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_native_pass(cpu, game_layout.moby.hook_draw_specular, "SPEC");  /* same geometry, same record queue */
    s_in = 0;
    return 1;
}

/** @brief Print the shaded material's data layout once, for reading the offsets the shaded decode uses off live data.
 *
 * The shaded draw (func_80022A2C) loops a flat Moby* list the descriptor
 * names rather than a record queue, decodes verts of three signed bytes each
 * (doubled), and rebuilds R per moby as ProjMatrix * Ry * Rx * Rz from
 * m_Rotation (block_80022DBC..80022F34). The dump prints the list's first 16 entries, the position,
 * class, rotation word, render radius and m_WasDrawn of the first two live
 * mobys with their model's first 12 words, the camera's projection matrix and
 * position, the histogram of the captured prim codes, and the first four
 * prims raw. It reads guest memory only.
 * @param cpu The guest CPU state, read only.
 * @param start The first polybuf address of the pass's output.
 * @param end One past the last. */
static void shaded_dump(CPUState* cpu, uint32_t start, uint32_t end) {
    fprintf(stderr, "[shaded-dump vbl=%u] prims [%08X,%08X) %u bytes\n",
            openpete_frame, start, end, end - start);
    /* +0x2400 Moby* list */
    fprintf(stderr, "  list@%08X:", SHADED_BASE);
    int nm = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)i*4);
        fprintf(stderr, " %08X", m);
        if (m == 0) break;
        nm++;
    }
    fprintf(stderr, "  (%d mobys)\n", nm);
    /* first 2 live mobys + their models */
    for (int i = 0; i < 2; i++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)i*4);
        if (m < 0x80000000u) break;
        int32_t px = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u);
        int32_t py = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u);
        int32_t pz = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
        uint32_t cls = cpu->read_half(m + game_layout.moby.class_off);
        uint32_t rot = cpu->read_word(m + game_layout.moby.angle_off + 0u);
        uint32_t rad = cpu->read_byte(m + moby_game.render_radius_off);
        uint32_t wasdrawn = cpu->read_byte(m + game_layout.moby.drawn_off);
        fprintf(stderr, "  moby%d @%08X pos=(%d,%d,%d) class=%u rot=%08X radius=%u wasDrawn=%u\n",
                i, m, px,py,pz, cls, rot, rad, wasdrawn);
        uint32_t model = cpu->read_word(game_layout.moby.model_table + cls*4);
        fprintf(stderr, "   model[%u]=%08X:", cls, model);
        if (model >= 0x80000000u) for (int w = 0; w < 12; w++)
            fprintf(stderr, " %08X", cpu->read_word(model + (uint32_t)w*4));
        fprintf(stderr, "\n");
    }
    /* camera ProjMatrix (+0x00..0x10) + position (+0x28..0x30) */
    fprintf(stderr, "  cam projM:");
    for (int w = 0; w < 5; w++) fprintf(stderr, " %08X", cpu->read_word(game_layout.cam.base + (uint32_t)w*4));
    fprintf(stderr, "  pos=(%d,%d,%d)\n",
            (int32_t)cpu->read_word(game_layout.cam.position + 0u), (int32_t)cpu->read_word(game_layout.cam.position + 4u),
            (int32_t)cpu->read_word(game_layout.cam.position + 8u));
    /* captured shaded prims (codes 0x20/28/22/2A expected) */
    int hist[256]; memset(hist, 0, sizeof(hist));
    for (int g = 0; g < s_gte_n; g++) hist[s_gte[g].code]++;
    fprintf(stderr, "  shaded prim codes:");
    for (int c = 0; c < 256; c++) if (hist[c]) fprintf(stderr, " %02X=%d", c, hist[c]);
    fprintf(stderr, " (total=%d)\n", s_gte_n);
    for (int g = 0; g < 4 && g < s_gte_n; g++) {
        prim_t* t = &s_gte[g];
        fprintf(stderr, "  [sprim %d] code=%02X len=%u :", g, t->code, t->len);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        fprintf(stderr, "\n");
    }
}

/* Sub-tick moby rotation interpolation state (set by moby_native_set_rot_interp,
 * declared in moby.h). s_rot_now is the present's
 * openpete_interp_present_now() timestamp, captured once when enabled so every moby composed this present uses
 * one consistent clock (each still gets its OWN per-object alpha from its distinct
 * prev/curr roll timestamps). 0 = inactive -> byte-exact path (canonical render,
 * or --render-fps off where the snapshot table is empty anyway). */
static op_time_t s_rot_now = 0;
static void moby_pose_snap_census(void);
void moby_native_set_rot_interp(int on) {
    if (!on && s_rot_now) moby_pose_snap_census();
    s_rot_now = on ? openpete_interp_present_now() : 0;
}

/* The camera the replayed cull records were composed against (the interp
 * ring's curr snapshot), installed per present by the orchestrator and cleared
 * after it. Read only by the record-replay branch of reextract_record_pass. */
static int     s_tickcam_have;
static int     s_tickcam_M[3][3];
static int32_t s_tickcam_pos[3];

void moby_native_set_tick_camera(const int16_t mat[9], const int32_t pos[3]) {
    if (!mat || !pos) { s_tickcam_have = 0; return; }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) s_tickcam_M[r][c] = mat[r * 3 + c];
    for (int i = 0; i < 3; i++) s_tickcam_pos[i] = pos[i];
    s_tickcam_have = 1;
}

/** @brief Whether the previous epoch's record copy can stand in for the current one.
 *
 * It must be the canonical epoch immediately before a fresh current copy's,
 * hold records, and carry the camera its cull composed them against.
 * @return 1 when usable, else 0. */
static int rec_prev_usable(void) {
    return s_rec_snap_words && s_rec_prev.words && s_rec_prev.cam_ok &&
           cut_held_set_pick(s_rec_snap_epoch, s_rec_prev.epoch) == CUT_HELD_PREV;
}

int moby_native_canon_ref_ns(op_time_t* out) {
    if (s_rec_ref_prev || !s_rec_snap_words || !spyro_native_canon_epoch_fresh(s_rec_snap_epoch))
        return 0;
    if (out) *out = s_rec_snap_ref_ns;
    return 1;
}

/* A held cut asked for the previous queue (moby_native_replay_prev_queue on),
 * whether or not it was usable; read by the cut census. */
static int s_hold_asked;

int moby_native_replay_prev_queue(int on) {
    static int      active;
    static uint32_t saved_canon;
    s_hold_asked = on ? 1 : 0;
    if (on) {
        if (active) return 1;
        if (!rec_prev_usable()) return 0;
        /* Every object state the previous queue's cull composed is stamped at
         * or before the stamp the current copy took. */
        s_rec_ref_ns = s_rec_snap_ref_ns;
        rec_snap_swap();
        saved_canon = s_rec_base_canon;
        s_rec_base_canon = s_rec_snap_base;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) s_rec_cam_M[r][c] = s_rec_snap_cam_M[r * 3 + c];
        for (int i = 0; i < 3; i++) s_rec_cam_pos[i] = s_rec_snap_cam_pos[i];
        s_rec_ref_prev = 1;
        active = 1;
        return 1;
    }
    if (active) {
        rec_snap_swap();
        s_rec_base_canon = saved_canon;
        s_rec_ref_prev = 0;
        active = 0;
    }
    return 0;
}

/** @brief On a held cut, print which epoch's animated queue the replay walked, when the cut census is armed.
 *
 * @param recs The records the regular pass replayed.
 * @param tris The triangles they produced. */
static void rec_held_queue_census(int recs, int tris) {
    if (!s_hold_asked || !cut_census_on()) return;
    fprintf(stderr, "[cut-census vbl=%u] held-queue kind=reg set=%s epoch=%u canon=%u recs=%d tris=%d\n",
            openpete_frame, s_rec_ref_prev ? "prev" : "curr", s_rec_snap_epoch,
            spyro_native_canon_epoch_stamp(), recs, tris);
}

int moby_native_prev_queue_mobys(uint32_t* out, int max, op_time_t* ref_ns) {
    if (!rec_prev_usable() || !out || max <= 0) return -1;
    if (ref_ns) *ref_ns = s_rec_snap_ref_ns;
    const uint32_t stride = game_layout.moby.rec_stride;
    const uint32_t sw = stride / 4u;
    const uint32_t n = s_rec_prev.queued + (uint32_t)s_rec_prev.appended;
    const uint32_t moff = game_layout.moby.rec_moby_off / 4u;
    int k = 0;
    for (uint32_t r = 0; r < n && k < max && (r + 1u) * sw <= s_rec_prev.words; r++) {
        if (s_rec_prev.snap[r * sw] == 0u) break;             /* the cull's terminator */
        const uint32_t m = s_rec_prev.snap[r * sw + moff];
        if (m >= 0x80000000u) out[k++] = m;
    }
    /* The second kind's queue of the same cull, when its copy of that epoch
     * is held: a held cut replays it too. */
    if (moby_game.simple_prev_mobys && k < max) {
        const int ks = moby_game.simple_prev_mobys(s_rec_prev.epoch, out + k, max - k);
        if (ks > 0) k += ks;
    }
    return k;
}

int moby_native_prev_queue_replay(uint32_t* epoch, int32_t cam_pos[3]) {
    if (!s_rec_ref_prev) return 0;
    if (epoch) *epoch = s_rec_snap_epoch;
    if (cam_pos) for (int i = 0; i < 3; i++) cam_pos[i] = s_rec_cam_pos[i];
    return 1;
}

/** @brief The object state a replayed record was composed from: the snapshot pair's curr, or under a previous-queue replay the state as of that queue's cull.
 *
 * @param o The object's snapshot pair.
 * @return The state bytes, or NULL when the record shows a state the pair no
 *         longer holds (it rolled after that cull and has no prev). */
static const uint8_t* rec_ref_state(const openpete_interp_obj_t* o) {
    if (!s_rec_ref_prev || o->curr_ns <= s_rec_ref_ns) return o->curr;
    return o->have_prev ? o->prev : NULL;
}

int moby_native_tick_camera_pos(int32_t out[3]) {
    if (!s_tickcam_have) return 0;
    for (int i = 0; i < 3; i++) out[i] = s_tickcam_pos[i];
    return 1;
}

double moby_native_view_widen(void) {
    return (double)s_view.F / s_view.s;
}

/** @brief Inverse of an integer 3x3 matrix by cofactors, in double.
 *
 * @param M The matrix.
 * @param out Receives the inverse; unwritten on a singular matrix.
 * @return 1, or 0 when |det| <= 1e-9. */
static int mat3_inverse(const int M[3][3], double out[3][3]) {
    double a = M[0][0], b = M[0][1], c = M[0][2];
    double d = M[1][0], e = M[1][1], f = M[1][2];
    double g = M[2][0], h = M[2][1], i = M[2][2];
    double c00 = e*i - f*h, c01 = f*g - d*i, c02 = d*h - e*g;
    double det = a*c00 + b*c01 + c*c02;
    if (!(det > 1e-9 || det < -1e-9)) return 0;
    double id = 1.0 / det;
    out[0][0] = c00*id;          out[0][1] = (c*h - b*i)*id; out[0][2] = (b*f - c*e)*id;
    out[1][0] = c01*id;          out[1][1] = (a*i - c*g)*id; out[1][2] = (c*d - a*f)*id;
    out[2][0] = c02*id;          out[2][1] = (b*g - a*h)*id; out[2][2] = (a*e - b*d)*id;
    return 1;
}

/* Facing-reversal hold threshold, in byte angle units (256 = one turn): a
 * record whose euler steps this far on any axis in one tick, while its
 * keyframe pair changes, has no pose between its two snapshots, so the
 * sub-tick interval holds instead of interpolating. 0 disables the hold. */
#define MOBY_MIRROR_HOLD_DEFAULT 0x40

/** @brief The facing-reversal hold threshold in byte angle units, read once.
 *
 * OPENPETE_MOBY_MIRROR_HOLD parsed with strtol base 0 when set and nonempty,
 * else MOBY_MIRROR_HOLD_DEFAULT. moby_mirror_hold treats 0 and any negative
 * value as the hold disabled.
 * @return The threshold. */
static int moby_mirror_thresh(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_MIRROR_HOLD");
                 s = (e && *e) ? (int)strtol(e, NULL, 0) : MOBY_MIRROR_HOLD_DEFAULT; }
    return s;
}

/** @brief The largest shortest-arc step of a moby's three euler bytes across its interp pair, in byte angle units (256 per turn).
 *
 * @param o The interp object; its prev and curr blobs are read at game_layout.moby.angle_off.
 * @return The step, 0..128. */
static int moby_euler_step_max(const openpete_interp_obj_t* o) {
    const uint32_t ao = game_layout.moby.angle_off;
    int worst = 0;
    for (unsigned a = 0; a < 3u; a++) {
        int d = (int)o->curr[ao + a] - (int)o->prev[ao + a];
        if (d > 128) d -= 256; else if (d < -128) d += 256;
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    return worst;
}

/** @brief The interval between a record's two interp snapshots as the two sub-tick channels see it.
 *
 * *step is the euler sweep's largest axis step and *kf_changed says whether
 * the shape lerp spans a different pair of keyframe streams (the pose the
 * cull would draw from each snapshot's animation state bytes: its header and
 * keyframe A, and keyframe B's while both snapshots blend). The blend byte
 * alone moving does not count: that is one keyframe pair at two weights,
 * which is ordinary animation, continuous by construction.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param step Receives the largest euler axis step.
 * @param kf_changed Receives 1 when the keyframe pair changed, else 0.
 * @return 0 when the record has no usable pair (no previous snapshot, no
 *         distinct states, two states holding different classes, or a model
 *         or pose that does not resolve), leaving both outputs untouched;
 *         else 1. */
static int moby_turn_interval(CPUState* cpu, uint32_t moby, int* step, int* kf_changed) {
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + game_layout.moby.class_off, 2);
    memcpy(&ccls, o->curr + game_layout.moby.class_off, 2);
    if (pcls != ccls) return 0;
    uint32_t model = cpu->read_word(game_layout.moby.model_table + (uint32_t)ccls * 4u);
    if (model < 0x80000000u) return 0;
    moby_pose_t pp, pc;
    if (!moby_pose_derive(cpu, model, o->prev + game_layout.moby.anim_state_off, &pp)) return 0;
    if (!moby_pose_derive(cpu, model, o->curr + game_layout.moby.anim_state_off, &pc)) return 0;
    *step = moby_euler_step_max(o);
    *kf_changed = (pp.header != pc.header || pp.frame_a != pc.frame_a ||
                   (pp.blend != 0u && pc.blend != 0u &&
                    (pp.header_b != pc.header_b || pp.frame_b != pc.frame_b)));
    return 1;
}

/** @brief Whether a hold has run one tick period past the current snapshot's stamp.
 *
 * A hold shows the pose of the tick before the current snapshot. The pair's
 * previous snapshot is the last distinct state, which for a moby static
 * before the change is many ticks old, so the pair's own alpha would stretch
 * the hold over the static span; the tick before the current one ends once
 * the present clock (s_rot_now) is one tick period past the current stamp.
 * @param o The interp object.
 * @return Nonzero once the hold has expired. */
static int moby_hold_expired(const openpete_interp_obj_t* o) {
    return s_rot_now - o->curr_ns >= g_openpete_tick_period_ns;
}

/** @brief A pair's alpha at this present on the per-object clock.
 *
 * The present clock (s_rot_now) past the current stamp over the pair's
 * distinct interval, clamped to [0, 1]. On a present that holds a camera cut,
 * an object that did not roll at the cut's tick takes 1, its current state
 * being its state at tick N-1 (cut_held_rolled); one that rolled is held by
 * moby_jump_held. The caller has checked curr_ns > prev_ns.
 * @param o The interp object.
 * @return The alpha. */
static double moby_pair_alpha(const openpete_interp_obj_t* o) {
    if (cut_held_rolled(o) == 0) return 1.0;
    double t = (double)(s_rot_now - o->curr_ns) / (double)(o->curr_ns - o->prev_ns);
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

/** @brief Whether moby_jump_held may hold a record: OPENPETE_MOBY_JUMP_HOLD, default on, read once.
 *
 * 0 interpolates across a jump, for a same-binary A/B.
 * @return Nonzero when the hold applies. */
static int moby_jump_hold_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_JUMP_HOLD"); s = (e && *e) ? (*e != '0') : 1; }
    return s;
}

/** @brief Whether the sub-tick view holds a replayed record whole at its previous tick on this present, for a held cut or a jump.
 *
 * A present that holds a camera cut shows tick N-1 on every record, so a
 * record that rolled at the cut's tick holds (kind CUT_HELD) and one that did
 * not shows its current state (cut_held_rolled). Otherwise a jump
 * (cut_obj_jump) holds: it is not a motion, so no state lies between the two
 * ticks: the interval shows tick N-1 on every channel the replay
 * interpolates (position, euler angles, keyframe shape, GPF scale) and tick N
 * at alpha 1. The shared guard (cut_obj_hold) takes the verdict once per
 * present, on the moby pass (s_rot_now set); a later reader on the same
 * present, such as the shadow pass, reads that verdict, and a reader outside
 * an open present or before the moby pass reads no hold. The alpha is the
 * present clock past the current stamp over the pair's interval capped at one
 * tick, the clock moby_record_pair sweeps. A game whose sub-tick view stages
 * its moby records (host_reveal) holds them in its own walk, so every channel
 * here already reads tick N-1, and this returns 0.
 * @param moby The moby's PSX address.
 * @param o The moby's interp pair; NULL yields 0.
 * @return 1 when the record holds, else 0. */
static int moby_jump_held(uint32_t moby, const openpete_interp_obj_t* o) {
    if (moby_game.host_reveal || !o || !o->have_prev || o->curr_ns <= o->prev_ns) return 0;
    const uint32_t po = game_layout.moby.pos_off;
    const int rolled = cut_held_rolled(o);
    if (rolled == 0) return 0;
    if (rolled < 0 && !cut_obj_jump(moby, o->prev, o->curr)) return 0;
    const int v = cut_obj_held(moby);
    if (v >= 0) return v;
    if (!s_rot_now || (rolled < 0 && !moby_jump_hold_enabled())) return 0;
    double period = (double)(o->curr_ns - o->prev_ns);
    if (period > (double)g_openpete_tick_period_ns) period = (double)g_openpete_tick_period_ns;
    const double a = (double)(s_rot_now - o->curr_ns) / period;
    return cut_obj_hold(rolled > 0 ? CUT_HELD : CUT_TELEPORT, moby, o, po, a, NULL);
}

/** @brief The facing-reversal verdict: whether a record holds its previous shape and rotation through this sub-tick interval.
 *
 * One verdict per record per present, read by both sub-tick channels, the
 * per-vertex model-space shape lerp and the euler sweep. A moby that turns a
 * half circle in one tick does it through two channels that cancel at the
 * tick instant and nowhere between: m_Rotation steps the half circle while
 * the keyframe pair jumps to the mirrored pose. Interpolated independently,
 * the model-space lerp between a pose and its mirror has in-plane extent
 * proportional to |1 - 2a|, collapsing onto the flip's invariant axis at
 * alpha 0.5, and the euler sweep turns that line to face the camera. No pose
 * lies between the two, so the interval holds the previous shape and
 * rotation and reaches the current tick's at alpha 1 or one tick period past
 * the current stamp (moby_hold_expired), whichever comes first.
 *
 * The verdict holds when the largest euler step reaches the threshold
 * (OPENPETE_MOBY_MIRROR_HOLD, default MOBY_MIRROR_HOLD_DEFAULT; 0 disables)
 * and the keyframe pair changed (moby_turn_interval). It is a pure function
 * of the interp pair and the model's tables, so the two readers cannot
 * disagree; a one-entry memo on (moby, present clock) spares the second
 * reader a second pose derive. 0 outside a sub-tick present (s_rot_now 0).
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @return 1 when the record holds, else 0. */
static int moby_mirror_hold(CPUState* cpu, uint32_t moby) {
    int thr = moby_mirror_thresh();
    if (thr <= 0 || !s_rot_now) return 0;
    static uint32_t s_moby = 0;
    static op_time_t s_at  = 0;
    static int       s_hold = 0;
    if (s_moby == moby && s_at == s_rot_now) return s_hold;
    int step = 0, kf = 0, hold = 0;
    if (moby_turn_interval(cpu, moby, &step, &kf)) hold = (step >= thr && kf);
    s_moby = moby; s_at = s_rot_now; s_hold = hold;
    return hold;
}

/** @brief Shortest-arc interpolation between two byte angles (256 per turn).
 *
 * The difference is wrapped to -128..128 before scaling; the result is not
 * wrapped, so it may leave 0..256.
 * @param prev_b The angle at t = 0.
 * @param curr_b The angle at t = 1.
 * @param t The phase.
 * @return The angle in byte units. */
static double lerp_byte_angle(int prev_b, int curr_b, double t) {
    int d = curr_b - prev_b;
    if (d > 128) d -= 256; else if (d < -128) d += 256;
    return (double)prev_b + (double)d * t;
}

int moby_record_euler_at_present(CPUState* cpu, uint32_t moby, ngte_euler_sc* e) {
    if (!s_rot_now) return 0;
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + game_layout.moby.class_off, 2);
    memcpy(&ccls, o->curr + game_layout.moby.class_off, 2);
    if (pcls != ccls) return 0;
    double t = moby_jump_held(moby, o) ? 0.0 : moby_pair_alpha(o);
    /* A facing reversal steps instead of sweeping: the previous tick's angles
     * through the whole interval, this tick's at alpha 1, the same schedule
     * the shape channel takes for the same verdict. */
    if (t < 1.0 && moby_mirror_hold(cpu, moby)) t = moby_hold_expired(o) ? 1.0 : 0.0;
    const uint32_t ao = game_layout.moby.angle_off;
    double fZ = lerp_byte_angle(o->prev[ao + 0u], o->curr[ao + 0u], t);
    double fX = lerp_byte_angle(o->prev[ao + 1u], o->curr[ao + 1u], t);
    double fY = lerp_byte_angle(o->prev[ao + 2u], o->curr[ao + 2u], t);
    ngte_sincos_interp(cpu, fY, &e->sy, &e->cy);
    ngte_sincos_interp(cpu, fX, &e->sx, &e->cx);
    ngte_sincos_interp(cpu, fZ, &e->sz, &e->cz);
    return 1;
}

/* One game-logic tick in WALL time (nominally NTSC 37609/1125000 s = 33.4302 ms;
 * run.c rescales the global by 1/tick_scale so --tick-scale slow-motion keeps
 * every gap-vs-tick comparison honest). The interp denominator cap; same
 * global as TICK_PERIOD_NS in reextract.c. (The measured
 * per-object gap the alpha actually divides by tracks the real VBL cadence;
 * this only caps it.) */
#define MOBY_TICK_PERIOD_NS g_openpete_tick_period_ns

/** @brief Whether moby_interp_pair_stale applies: OPENPETE_MOBY_SNAP_STALE, default on, read once.
 *
 * 0 interpolates across a stale pair, for a same-window A/B.
 * @return Nonzero when the stale test applies. */
static int moby_interp_stale_snap_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_STALE"); s = (e && *e) ? (*e != '0') : 1; }
    return s;
}

/** @brief Whether an interp pair is stale: its distinct-state gap exceeds 1.5 tick periods.
 *
 * The ring rolls prev from curr only on a change, so a gap that long means
 * prev straddles a static stretch and the pair is a discontinuity, not
 * motion. Two readers snap on it: the billboard position lerp, reached by the
 * mobys that miss the primary sub-tick position path
 * (openpete_native_interp_pos), such as HUD-pool text, tally and icon mobys;
 * and moby_record_pair, which gates every record-replay term built on a pair.
 * A HUD text glyph keys by (class, slot), which is not stable across a HUD
 * relayout: after a submenu switch a slot's prev can be the glyph that last
 * held it about 20 ticks earlier, and a lerp over that gap moves the letter
 * across the screen. A moving billboard (a flying tally gem, a sliding HUD
 * icon) rolls every tick, is not stale, and keeps interpolating.
 *
 * The rotation (shaded_compose_euler) and keyframe-shape
 * (moby_prev_keyframe) lerps do not consult it: slowly rotating or animating
 * mobys (gems, idle dragons) change less than one byte per tick and roll
 * their record only every few ticks, and snapping them would restore the
 * 1.4-degree steps. OPENPETE_MOBY_SNAP_STALE=0 disables the test.
 * @param prev_ns The previous snapshot's stamp.
 * @param curr_ns The current snapshot's stamp.
 * @return 1 when stale, else 0. */
static int moby_interp_pair_stale(op_time_t prev_ns, op_time_t curr_ns) {
    if (!moby_interp_stale_snap_enabled()) return 0;
    return (curr_ns - prev_ns) > MOBY_TICK_PERIOD_NS + MOBY_TICK_PERIOD_NS / 2;
}

/** @brief Whether OPENPETE_MOBY_IDLE enables the host-side idle wobble of frozen collectables, read once.
 *
 * Mobys outside the canonical update gates freeze in RAM, so the reveal and
 * extended paths draw them as statues. With the knob on, shaded_compose_euler
 * substitutes the rotation the frozen moby's own idle math would produce,
 * computed host-side, with no PSX RAM write; m_WasDrawn keeps its one writer,
 * the .s cull.
 *
 * Scope: the collectable wobble of gems (cls 83..87) and the life statue
 * (cls 14). The level updater's collectable case skips its whole state
 * machine when m_WasDrawn == 0, OctDistance(moby, g_Spyro) >= 0x2001,
 * m_Substate == 1 and props->m_SpawnState == 0 (level_10 .s 8007F9B8), and
 * otherwise, while m_Substate < 3, writes rot.x (+0x44) = ((s16)cos[ticks *
 * 2] >> 7) + props->m_RotY (+0x11), rot.y (+0x45) = ((s16)sin[ticks * 2] >>
 * 7) + props->m_RotZ (+0x12), and props->m_RotationTicks (+0x13) +=
 * 2 * g_DeltaTime (800802F4..80080360; cls 14 shifts by 9 and leaves +0x46).
 * The table is game_layout.sin_table (cos at +0x80), the one
 * ngte_sincos_interp blends. The life orb (cls 15) accumulates its heading
 * (z += 8, y -= 6 per tick) and is excluded, since a wake would snap the
 * whole offset.
 * @return Nonzero when the wobble is enabled. */
static int moby_idle_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_IDLE"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s;
}

/** @brief Whether a moby is on the game's update list, mirroring func_80051FEC (moby_lists.s:35-96).
 *
 * A moby with m_WasDrawn set is included. Otherwise the signed
 * m_UpdateDistance byte ud decides: 0 never updates; a value whose << 10 is
 * negative always does; else the moby must lie strictly inside the 3-axis box
 * |d| < ud * 1024 around g_Spyro and pass the horizontal squared-distance test
 * on (dx >> 3, dy >> 3) against (ud * 1024) >> 3. A moby outside the list gets
 * no update, so its whole state machine, the gem settle and wobble included,
 * is frozen in RAM.
 * @param cpu The guest CPU state, read only.
 * @param m The moby's PSX address.
 * @return 1 on the list, else 0. */
static int gem_in_update_list(CPUState* cpu, uint32_t m) {
    if (cpu->read_byte(m + game_layout.moby.drawn_off) != 0) return 1;      /* m_WasDrawn grace auto-include */
    int32_t ud = (int8_t)cpu->read_byte(m + moby_game.update_dist_off);    /* SIGNED (0xFF = -1 = always) */
    if (ud == 0) return 0;                            /* never update */
    int32_t thr = ud << 10;
    if (thr < 0) return 1;                            /* ud >= 0x80: always update */
    int32_t dx = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u) - (int32_t)cpu->read_word(moby_game.player_pos + 0x0);
    if (dx + thr <= 0 || dx - thr >= 0) return 0;
    int32_t dy = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u) - (int32_t)cpu->read_word(moby_game.player_pos + 0x4);
    if (dy + thr <= 0 || dy - thr >= 0) return 0;
    int32_t dz = (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u) - (int32_t)cpu->read_word(moby_game.player_pos + 0x8);
    if (dz + thr <= 0 || dz - thr >= 0) return 0;
    int32_t hx = dx >> 3, hy = dy >> 3, ht = thr >> 3;
    return (ht * ht - hx * hx - hy * hy) > 0;
}

/** @brief Whether the game's collectable wobble is frozen for a moby this tick.
 *
 * A pure function of the game state the .s itself reads, never of host
 * render state. A moby in m_Substate 3 or above is not frozen. A moby off the
 * update list (gem_in_update_list) is frozen, which covers far gems resting
 * unsettled in m_Substate 0 since level load (Beast Makers). On the list, the
 * collectable case skips its state machine (level_10 .s 8007F9B8) only when
 * m_WasDrawn is 0, the OctDistance to g_Spyro (math.s, XY only:
 * max + min * 3/8) is at least 0x2001, m_Substate is 1 and the props spawn
 * state byte (+0x0E) is 0.
 * @param cpu The guest CPU state, read only.
 * @param m The moby's PSX address.
 * @param props The moby's props record.
 * @return 1 when frozen, else 0. */
static int gem_idle_frozen(CPUState* cpu, uint32_t m, uint32_t props) {
    if (cpu->read_byte(m + moby_game.substate_off) >= 3) return 0;      /* non-resting states: leave canonical */
    if (!gem_in_update_list(cpu, m)) return 1;        /* no update at all -> frozen */
    if (cpu->read_byte(m + game_layout.moby.drawn_off) != 0) return 0;      /* drawn last frame -> handler runs */
    int32_t dx = (int32_t)cpu->read_word(moby_game.player_pos + 0x0) - (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u);
    int32_t dy = (int32_t)cpu->read_word(moby_game.player_pos + 0x4) - (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t oct = (dx < dy) ? ((dx * 3) >> 3) + dy : dx + ((dy * 3) >> 3);
    if (oct < 0x2001) return 0;                       /* near Spyro -> handler runs */
    if (cpu->read_byte(m + moby_game.substate_off) != 1) return 0;      /* updating + not settled -> settle logic runs */
    if (cpu->read_byte(props + 0x0E) != 0) return 0;  /* updating + still spawning -> logic runs */
    return 1;
}

/* Host wobble phase per frozen gem: seeded from the frozen RAM phase byte (so
 * the outward crossing is continuous), advanced +1 byte-angle per VBL (the
 * canonical +2 per 29.913 Hz tick). Entry lifetime == one frozen stretch: any
 * not-frozen verdict drops it, so a re-freeze re-seeds from RAM. cls+base
 * mismatch re-seeds too (self-heals slot reuse across level loads). */
typedef struct { uint32_t moby; uint16_t cls; uint8_t base_ticks; uint32_t t0_frame; } gem_idle_ent;
#define GEM_IDLE_SLOTS 1024                /* > max gems per level (~400), power of 2 */
static gem_idle_ent s_gem_idle[GEM_IDLE_SLOTS];
static long s_idle_active, s_idle_seeded;  /* census counters (OPENPETE_MOBY_TIER_DBG) */
static long s_idle_live;                   /* collectables whose gate said "canonical animates" */

/** @brief The wobble-table slot for a moby: its entry, or the first free slot of a 16-slot probe.
 *
 * Probing starts at (m >> 3) modulo GEM_IDLE_SLOTS. When no probed slot names
 * the moby or is free, the moby's home slot is returned, which then evicts the
 * entry it holds; two mobys sharing a home slot re-seed each other on every
 * call and render frozen.
 * @param m The moby's PSX address.
 * @return A slot, never NULL. */
static gem_idle_ent* gem_idle_slot(uint32_t m) {
    uint32_t h = (m >> 3) & (GEM_IDLE_SLOTS - 1u);
    for (uint32_t i = 0; i < 16u; i++) {
        gem_idle_ent* e = &s_gem_idle[(h + i) & (GEM_IDLE_SLOTS - 1u)];
        if (e->moby == m || e->moby == 0) return e;
    }
    return &s_gem_idle[h];                 /* probe cap: two gems share the home slot
                                            * and keep re-seeding = they render frozen
                                            * (feature no-op), never wrongly posed */
}

/** @brief The host wobble phase of a frozen collectable, in byte-angle units, at this VBL.
 *
 * The moby's table entry is seeded when it names another moby or class, or
 * holds another base m_RotationTicks (props + 0x13): the seed is that byte and
 * the current VBL. The phase is (base + VBLs since the seed) mod 256, one
 * byte-angle per VBL where the game advances two per tick. Each seed counts
 * into s_idle_seeded.
 * @param cpu The guest CPU state, read only.
 * @param m The moby's PSX address.
 * @param cls The moby's class, stored in the entry.
 * @param props The moby's props record address.
 * @param t0_out Receives the seed phase, the frozen RAM byte.
 * @return The phase, in [0, 256). */
static double gem_idle_phase(CPUState* cpu, uint32_t m, uint32_t cls, uint32_t props, uint8_t* t0_out) {
    uint8_t ticks = cpu->read_byte(props + 0x13);      /* m_RotationTicks, frozen */
    gem_idle_ent* e = gem_idle_slot(m);
    if (e->moby != m || e->cls != (uint16_t)cls || e->base_ticks != ticks) {
        e->moby = m; e->cls = (uint16_t)cls; e->base_ticks = ticks; e->t0_frame = openpete_frame;
        s_idle_seeded++;
    }
    *t0_out = e->base_ticks;
    return fmod((double)e->base_ticks + (double)(openpete_frame - e->t0_frame), 256.0);
}

/** @brief Compose a moby's euler rotation onto a matrix as start * Ry * Rx * Rz.
 *
 * Mirrors func_80022A2C's matrix build (block_80022DBC..80022F34). The
 * angles come from, in order: the idle wobble of a frozen collectable (a
 * non-billboard moby of class 14 or 83 to 87 that gem_idle_frozen accepts),
 * computed as a delta around the RAM angles; the present's interpolated
 * angles (moby_record_euler_at_present); the canonical table lookup of the
 * m_Rotation word at angle_off (Y = (rot >> 15) & 0x1FE, X = (rot & 0xFF00)
 * >> 7, Z = (rot & 0xFF) << 1 as table indices). The sin and cos table values
 * enter unmodified.
 * @param cpu The guest CPU state, read only.
 * @param start The matrix to rotate, 12-bit fixed point.
 * @param moby The moby's PSX address, which keys its prev and curr snapshots.
 * @param out Receives the composed matrix. */
static void shaded_compose_euler(CPUState* cpu, const int start[3][3], uint32_t moby, int out[3][3]) {
    ngte_euler_sc e;
    int composed = 0;
    /* Idle-anim override: a FROZEN collectable renders the pose its own math
     * would produce had it kept ticking. Billboards excluded (the tally/HUD
     * gems carry a different props layout than MobyCollectableProps). */
    if (moby_idle_on() && !(cpu->read_byte(moby + moby_game.render_radius_off) & 0x80)) {
        uint32_t cls = cpu->read_half(moby + game_layout.moby.class_off);
        if (cls == 14u || (cls >= 83u && cls <= 87u)) {
            uint32_t props = cpu->read_word(moby + moby_game.props_off);
            if (props >= 0x80000000u) {
                if (gem_idle_frozen(cpu, moby, props)) {
                    uint8_t t0 = 0;
                    double ph = gem_idle_phase(cpu, moby, cls, props, &t0);
                    int si, ci, s0, c0;
                    ngte_sincos_interp(cpu, ph, &si, &ci);
                    ngte_sincos_interp(cpu, (double)t0, &s0, &c0);
                    /* float mirror of ((s16)lut>>shift): /128 gems, /512 statue.
                     * Wobble as a DELTA around the frozen RAM pose: at ph==t0
                     * this IS the RAM euler (seamless seed), and for a SETTLED
                     * gem it equals the canonical formula (RAM +0x44 =
                     * cos[t0]>>7 + m_RotY). An UNSETTLED far gem (substate 0 -
                     * never updated since load, base m_RotY/RotZ never written)
                     * wobbles around its placed pose instead of snapping to
                     * uninitialized props bytes (beast-makers). */
                    double div = (cls == 14u) ? 512.0 : 128.0;
                    double fZ = (double)cpu->read_byte(moby + game_layout.moby.angle_off + 0u) + ((double)ci - (double)c0) / div;
                    double fX = (double)cpu->read_byte(moby + game_layout.moby.angle_off + 1u) + ((double)si - (double)s0) / div;
                    double fY = (double)cpu->read_byte(moby + game_layout.moby.angle_off + 2u);   /* untouched by the handler */
                    ngte_sincos_interp(cpu, fY, &e.sy, &e.cy);
                    ngte_sincos_interp(cpu, fX, &e.sx, &e.cx);
                    ngte_sincos_interp(cpu, fZ, &e.sz, &e.cz);
                    composed = 1; s_idle_active++;
                    if (moby_trace_target() == moby)
                        fprintf(stderr, "[moby-idle vbl=%u] %08X ph=%.1f base=%u eul=(%.1f,%.1f,%.1f)\n",
                                openpete_frame, moby, ph,
                                (unsigned)cpu->read_byte(props + 0x13), fZ, fX, fY);
                } else {
                    gem_idle_ent* e = gem_idle_slot(moby);
                    if (e->moby == moby) e->moby = 0;   /* drop: re-freeze re-seeds from RAM */
                    if (moby_trace_target() == moby) {
                        int32_t dx = (int32_t)cpu->read_word(moby_game.player_pos + 0x0) - (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u);
                        int32_t dy = (int32_t)cpu->read_word(moby_game.player_pos + 0x4) - (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u);
                        if (dx < 0) dx = -dx;
                        if (dy < 0) dy = -dy;
                        fprintf(stderr, "[moby-idle vbl=%u] %08X live: wd=%u oct=%d sub=%u spawn=%u ticks=%u\n",
                                openpete_frame, moby, cpu->read_byte(moby + game_layout.moby.drawn_off),
                                (dx < dy) ? ((dx * 3) >> 3) + dy : dx + ((dy * 3) >> 3),
                                cpu->read_byte(moby + moby_game.substate_off), cpu->read_byte(props + 0x0E),
                                (unsigned)cpu->read_byte(props + 0x13));
                    }
                    s_idle_live++;
                }
            }
        }
    }
    /* A host-composed euler above owns the whole rotation; otherwise take the
     * present's interpolated angles, and the canonical byte-exact table lookup
     * when there is no usable pair. */
    if (!composed && !moby_record_euler_at_present(cpu, moby, &e))
        ngte_euler_sincos(cpu, cpu->read_word(moby + game_layout.moby.angle_off + 0u), &e);
    ngte_compose_euler(start, &e, out);
}

/** @brief Compare the shaded mobys' native projection with the shaded pass's prims.
 *
 * Decodes every moby of the Moby* list (3 signed bytes per vertex, doubled:
 * byte 0 Z, byte 1 X, byte 2 Y, .L80023010, from model + 4 with the count at
 * model + 0), projects with the canonical view, then set-matches the
 * projected vertices against the captured shaded prim vertices. A 3D moby
 * uses R = ProjMatrix * Ry (rotation axis 0 only) and TR = ProjMatrix *
 * (pos - cam); a billboard (bit 7 of the render radius) uses the fixed
 * matrix, TR = (0, 0, z >> 1) and its position as the screen offset. Keeps
 * at most 8192 vertices. Prints the counts, the centroid offset and the share
 * of unsaturated prim vertices within 4 px. Reads guest memory only.
 * @param cpu The guest CPU state, read only. */
static void shaded_verify(CPUState* cpu) {
    /* The +0x2400 list holds MULTIPLE live shaded mobys -- project EVERY one
     * (each with its own R = ProjMatrix . Ry(m_Rotation axis0) and TR =
     * ProjMatrix . (pos - cam)) and accumulate, then A/B vs all shaded prims.
     * (axis0 = Y confirmed; axis1/2 euler TODO for fuller rotations.) */
    /* Compares against the game's own prims: the canonical projection. */
    ngte_view vc; ngte_view_canon(&s_view_shd, &vc);
    int PM[3][3]; memcpy(PM, vc.cam_M, sizeof PM);
    int32_t cx=(int32_t)vc.cam_ram[0], cy=(int32_t)vc.cam_ram[1], cz=(int32_t)vc.cam_ram[2];
    double ofx=vc.ofx, ofy=vc.ofy;
    static proj_xy sv[8192]; int n=0; int nmoby=0;
    for (int r=0; r<256; r++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)r*4u);
        if (m < 0x80000000u) break;
        nmoby++;
        uint32_t cls = cpu->read_half(m + game_layout.moby.class_off);
        uint32_t model = cpu->read_word(game_layout.moby.model_table + cls*4u); if (model<0x80000000u) model|=0x80000000u;
        uint32_t vbase = cpu->read_word(model + 0x4u);      if (vbase<0x80000000u) vbase|=0x80000000u;
        int count = cpu->read_byte(model + 0x0u) & 0xFF;
        int32_t px=(int32_t)cpu->read_word(m+0xC), py=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u), pz=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
        /* billboard/2D vs 3D path: flag = bit7 of m_RenderRadius (asm
         * `lhu a0,0x50; sll a0,24; bltz -> .L80022D1C`). */
        int billboard = (cpu->read_byte(m + moby_game.render_radius_off) & 0x80) != 0;
        int M[3][3]; double TR[3]; double mofx, mofy;
        if (billboard) {                              /* .L80022D1C */
            M[0][0]=4096;M[0][1]=0;M[0][2]=0; M[1][0]=0;M[1][1]=2560;M[1][2]=0; M[2][0]=0;M[2][1]=0;M[2][2]=4096;
            TR[0]=0; TR[1]=0; TR[2]=(double)(pz>>1);  /* OFX/OFY = pos.x/y; TRZ = pos.z>>1 */
            mofx=(double)px; mofy=(double)py;
        } else {                                      /* 3D: R = ProjMatrix . Ry(axis0), TR = PM.(pos-cam) */
            uint32_t rot = cpu->read_word(m + game_layout.moby.angle_off + 0u);
            uint32_t idx0 = (rot >> 15) & 0x1FEu;
            int s0 = (int16_t)cpu->read_half(game_layout.sin_table + idx0);
            int c0 = (int16_t)cpu->read_half(game_layout.sin_table + 0x80u + idx0);
            int Ry[3][3] = {{c0,0,s0},{0,4096,0},{-s0,0,c0}};
            for (int i=0;i<3;i++) for (int j=0;j<3;j++)
                M[i][j] = (PM[i][0]*Ry[0][j] + PM[i][1]*Ry[1][j] + PM[i][2]*Ry[2][j]) >> 12;
            int32_t dx=px-cx, dy=py-cy, dz=pz-cz;
            for (int i=0;i<3;i++) TR[i] = (double)((PM[i][0]*dx + PM[i][1]*dy + PM[i][2]*dz) >> 12);
            mofx=ofx; mofy=ofy;
        }
        for (int i=0;i<count && n<8192;i++) {
            uint32_t p = vbase + (uint32_t)i*3u;
            uint32_t w = cpu->read_byte(p) | (cpu->read_byte(p+1)<<8) | (cpu->read_byte(p+2)<<16);
            int VZ=((int32_t)(w<<24))>>23, VX=((int32_t)(w<<16))>>23, VY=((int32_t)(w<<8))>>23;
            double sx,sy; int sat;
            project(M,TR,(double)VX,(double)VY,(double)VZ, mofx,mofy,&vc, &sx,&sy,&sat);
            sv[n].sx=(float)sx; sv[n].sy=(float)sy; sv[n].sat=sat; n++;
        }
    }
    /* set-match shaded prim verts vs sv + centroid offset (to characterize any
     * constant projection-center error, e.g. a 3D-vs-billboard OFX mismatch). */
    int gv=0,near=0,div=0; double sum=0,mx=0;
    double mycx=0,mycy=0, pcx=0,pcy=0; int pcn=0;
    for (int i=0;i<n;i++){ mycx+=sv[i].sx; mycy+=sv[i].sy; } if(n){mycx/=n;mycy/=n;}
    for (int g=0; g<s_gte_n; g++) for (int k=0;k<s_gte[g].nv;k++) {
        float gx=s_gte[g].x[k], gy=s_gte[g].y[k]; gv++; pcx+=gx; pcy+=gy; pcn++;
        double best=1e18; int bs=1;
        for (int i=0;i<n;i++){ double dx=sv[i].sx-gx,dy=sv[i].sy-gy,d=dx*dx+dy*dy; if(d<best){best=d;bs=sv[i].sat;} }
        double dist=sqrt(best);
        if (bs){div++;continue;}
        if (dist<=4.0){near++;sum+=dist;if(dist>mx)mx=dist;}
    }
    if(pcn){pcx/=pcn;pcy/=pcn;}
    int clean=gv-div;
    fprintf(stderr,"[shaded-verify vbl=%u] mobys=%d nativeVerts=%d | "
            "myCentroid=(%.0f,%.0f) primCentroid=(%.0f,%.0f) offset=(%.0f,%.0f) | "
            "primVerts=%d match<=4px=%d/%d(%.1f%%) mean=%.2fpx\n",
            openpete_frame, nmoby, n, mycx,mycy, pcx,pcy, mycx-pcx, mycy-pcy,
            gv, near,clean, clean?100.0*near/clean:0.0, near?sum/near:0.0);
}

/* ---- shaded directional lighting (#2b) - faithful integer GTE mirror -------
 * Reproduces func_80022A2C's per-normal lighting (gouraud block_80023148 /
 * flat block_80023574) bit-for-bit so the native colour equals the .s
 * renderer's, instead of NN-sampling the .s's emitted prims. Op chain:
 *   IR = normal (3 signed bytes)
 *   MVMVA(mx=RT, v=IR, sf=1, lm=0)   -> view-space normal  (gte_mvmva)
 *   IR0 = (base>>23)&0x1E ; GPF(sf=0,lm=0): IR = IR0*IR     (gte_gpf)
 *   BK  = base colour channels<<? ; LC = light dir broadcast
 *   CC: light_color (BK + LC·IR) then color_output (white·IR) -> RGB
 * Light dir is read fresh per frame (s_shL); base colour per moby. */
static int s_shL[3];   /* int16-truncated directional-light dir (D_800770C8+0/4/8) */

/** @brief Clamp a value to the GTE IR range.
 *
 * @param v The value.
 * @param lm Nonzero raises the floor from -0x8000 to 0.
 * @return v clamped to [floor, 0x7FFF]. */
static int32_t sat_ir_lm(int64_t v, int lm) {   /* mirrors GTEState::saturate_ir */
    int32_t mn = lm ? 0 : -0x8000;
    if (v < mn) return mn;
    if (v > 0x7FFF) return 0x7FFF;
    return (int32_t)v;
}
/** @brief Clamp a value to a colour byte.
 *
 * @param v The value.
 * @return v clamped to [0, 255]. */
static int32_t sat_col8(int64_t v) {            /* mirrors GTEState::saturate_color */
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int32_t)v;
}

/** @brief The shaded draw's directional lighting of one normal, as the integer GTE chain computes it.
 *
 * MVMVA of the normal by RT, GPF by IR0 = (base >> 23) & 0x1E, then CC with
 * the base colour as BK and the light direction (s_shL) broadcast as LC, then
 * the white colour output. With highlight set the gouraud specular boost
 * (block_800231C4..80023250) follows: a packed result below the base's low
 * 24 bits returns those bits; otherwise, with hn = base >> 28 nonzero and
 * d = IR1 - (hn << 7) - the red BK term positive, each channel's IR gains
 * 2d and is shifted right 4, saturating to 0xFF from 4096.
 * @param RT The model-to-view rotation this moby's verts project through.
 * @param nx The normal X, a signed byte.
 * @param ny The normal Y.
 * @param nz The normal Z.
 * @param base The per-moby base-colour word.
 * @param highlight Nonzero on the gouraud path; the flat path omits the boost.
 * @return The colour as 0x00BBGGRR, red in the low byte, as s_gte and s_ptri
 *         carry rgb. */
static uint32_t shaded_lit_color(const int RT[3][3], int nx, int ny, int nz,
                                 uint32_t base, int highlight) {
    int64_t m1 = ((int64_t)RT[0][0]*nx + (int64_t)RT[0][1]*ny + (int64_t)RT[0][2]*nz) >> 12;
    int64_t m2 = ((int64_t)RT[1][0]*nx + (int64_t)RT[1][1]*ny + (int64_t)RT[1][2]*nz) >> 12;
    int64_t m3 = ((int64_t)RT[2][0]*nx + (int64_t)RT[2][1]*ny + (int64_t)RT[2][2]*nz) >> 12;
    int32_t ir1 = sat_ir_lm(m1,0), ir2 = sat_ir_lm(m2,0), ir3 = sat_ir_lm(m3,0);
    int32_t ir0 = (int32_t)(((uint32_t)base >> 23) & 0x1Eu);
    ir1 = sat_ir_lm((int64_t)ir0*ir1, 0);
    ir2 = sat_ir_lm((int64_t)ir0*ir2, 0);
    ir3 = sat_ir_lm((int64_t)ir0*ir3, 0);
    int32_t rbk = (int32_t)(((uint32_t)base << 4) & 0xFF0u);
    int32_t gbk = (int32_t)(((uint32_t)base >> 4) & 0xFF0u);
    int32_t bbk = (int32_t)(((uint32_t)base >> 12) & 0xFF0u);
    /* CC step 1 - light_color: LC matrix = light dir broadcast to all 3 rows */
    int64_t lc = (int64_t)s_shL[0]*ir1 + (int64_t)s_shL[1]*ir2 + (int64_t)s_shL[2]*ir3;
    ir1 = sat_ir_lm((((int64_t)rbk << 12) + lc) >> 12, 1);
    ir2 = sat_ir_lm((((int64_t)gbk << 12) + lc) >> 12, 1);
    ir3 = sat_ir_lm((((int64_t)bbk << 12) + lc) >> 12, 1);
    /* CC step 2 - color_output: RGBC = white (0xFFFFFF). MAC = (255*IR<<4)>>12 */
    int32_t M1 = (int32_t)(((int64_t)255*ir1) >> 8);
    int32_t M2 = (int32_t)(((int64_t)255*ir2) >> 8);
    int32_t M3 = (int32_t)(((int64_t)255*ir3) >> 8);
    int32_t oir1 = sat_ir_lm(M1,1), oir2 = sat_ir_lm(M2,1), oir3 = sat_ir_lm(M3,1);
    uint32_t a0 = (uint32_t)sat_col8(M1>>4) | ((uint32_t)sat_col8(M2>>4)<<8) | ((uint32_t)sat_col8(M3>>4)<<16);
    if (!highlight) return a0;
    /* gouraud spec boost */
    int32_t s1 = (int32_t)(base & 0xFFFFFFu);
    if ((int32_t)a0 - s1 < 0) return (uint32_t)s1;
    int32_t hn = (int32_t)((uint32_t)base >> 28);
    if (hn == 0) return a0;
    int32_t d = oir1 - (hn << 7) - rbk;
    if (d <= 0) return a0;
    d <<= 1;
    int32_t ch[3] = { oir1 + d, oir2 + d, oir3 + d };
    for (int k = 0; k < 3; k++) ch[k] = (ch[k] < 4096) ? (ch[k] >> 4) : 0xFF;
    return (uint32_t)ch[0] | ((uint32_t)ch[1]<<8) | ((uint32_t)ch[2]<<16);
}


/** @brief The shaded metal sub-mode colour of one face (word0 bit1, block_80023720).
 *
 * The face normal rotates to view space (RT N) and the reflection intensity
 * is the light direction (s_shL, broadcast across the LCM rows) dotted with
 * it, >> 8. The colour is the metal table entry's word 0 as base tint plus
 * intensity times word 1 >> 12, plus a boost of (intensity - 0x5C0) << 3 on
 * every channel when positive, clamped from 12 bits to 8. A back-facing
 * face (MAC0 < 0 at r_moby.s 4895-4899) takes the intensity >> 10 and the
 * whole word 0 shifted right by one before its channels are extracted, so
 * a channel's low bit enters the channel below it.
 * @param RT The model-to-view rotation.
 * @param nx The face normal X, a signed byte.
 * @param ny The face normal Y.
 * @param nz The face normal Z.
 * @param w0 The metal table entry's base-tint word, unmasked.
 * @param w1 The metal table entry's reflected-tint word.
 * @param back Nonzero for a face the draw takes as back-facing.
 * @return The colour as 0x00BBGGRR. */
static uint32_t shaded_metal_color(const int RT[3][3], int nx, int ny, int nz,
                                   uint32_t w0, uint32_t w1, int back) {
    int64_t m1=((int64_t)RT[0][0]*nx+(int64_t)RT[0][1]*ny+(int64_t)RT[0][2]*nz)>>12;
    int64_t m2=((int64_t)RT[1][0]*nx+(int64_t)RT[1][1]*ny+(int64_t)RT[1][2]*nz)>>12;
    int64_t m3=((int64_t)RT[2][0]*nx+(int64_t)RT[2][1]*ny+(int64_t)RT[2][2]*nz)>>12;
    int32_t ir1=sat_ir_lm(m1,0), ir2=sat_ir_lm(m2,0), ir3=sat_ir_lm(m3,0);
    int64_t refl=(int64_t)s_shL[0]*ir1+(int64_t)s_shL[1]*ir2+(int64_t)s_shL[2]*ir3;
    int32_t intensity=(int32_t)(refl>>8);
    if (back) { intensity >>= 2; w0 >>= 1; }
    int16_t ir0=(int16_t)intensity;
    int r1=(int)((w1<<4)&0xFF0u), g1=(int)((w1>>4)&0xFF0u), b1=(int)((w1>>12)&0xFF0u);
    int r0=(int)((w0<<4)&0xFF0u), g0=(int)((w0>>4)&0xFF0u), b0=(int)((w0>>12)&0xFF0u);
    int R=r0+(((int)ir0*r1)>>12), G=g0+(((int)ir0*g1)>>12), B=b0+(((int)ir0*b1)>>12);
    int bo=intensity-0x5C0; if(bo>0){ bo<<=3; R+=bo; G+=bo; B+=bo; }
    R = R<0?0:(R>=0xFFF?0xFF:(R>>4));
    G = G<0?0:(G>=0xFFF?0xFF:(G>>4));
    B = B<0?0:(B>=0xFFF?0xFF:(B>>4));
    return (uint32_t)R | ((uint32_t)G<<8) | ((uint32_t)B<<16);
}

/* Per-shaded-moby metadata recorded alongside the projected verts, so the
 * native face-stream walk (build_shaded_walk_tris) can index each moby's verts
 * in out[] and read its face stream. RT/base/model carry the lighting inputs. */
typedef struct { int sv_off, vcount; uint32_t faces; int fcount; int cls;
                 uint32_t model, base, moby; int RT[3][3]; int near_ot;
                 int vzspread; float pvzspread;
                 uint32_t ctab; int ctab_len;   /* static baked colour table [model+8] (bytes); 0 len = lit */
                 moby_otk_rec_t otk;            /* the pass's paint key for this moby */
               } shaded_rec;   /* vzspread = model-space Z extent (max-min);
                 * a large extent means a real 3D model that self-occludes -> needs the HUD3D
                 * depth pass, vs a thin text/decal overlay that renders fine depth-off.
                 * pvzspread = PROJECTED per-vertex view-Z extent (raw project() mac3,
                 * pre-vz_scale - same 4096-scale unit as model VZ). Catches what the
                 * static model measure can't: a thin model the game ROTATES (the
                 * inventory screen's spinning letter/dragon-icon mobys) swings its
                 * large X extent into view-Z, so it self-occludes despite a small
                 * model-space Z. Either spread ≥ threshold -> HUD3D. */
static shaded_rec s_shrecs[256]; static int s_nshrecs;

/** @brief The integer translation the shaded pass loads for a 3D moby (r_moby.s 4015-4154).
 *
 * IR = ((camY - Y) >> 2, (camZ - Z) >> 2, (X - camX) >> 2) through the camera
 * matrix (MVMVA sf=1), doubled; a nonzero draw-scale byte then runs GPF sf=0
 * on the int16 of each component and shifts right 5.
 * @param PM The camera matrix, 4096 = 1.0.
 * @param cam The camera position.
 * @param pos The moby position.
 * @param p57 The moby's draw-scale byte.
 * @param tr Receives the translation. */
static void shaded_int_tr(const int PM[3][3], const int32_t cam[3], const int32_t pos[3],
                          int p57, int tr[3]) {
    int32_t ir[3] = { (cam[1] - pos[1]) >> 2, (cam[2] - pos[2]) >> 2, (pos[0] - cam[0]) >> 2 };
    for (int k = 0; k < 3; k++) {
        int64_t mac = ((int64_t)PM[k][0] * ir[0] + (int64_t)PM[k][1] * ir[1]
                     + (int64_t)PM[k][2] * ir[2]) >> 12;
        int t = (int)mac * 2;
        tr[k] = p57 ? (p57 * (int)(int16_t)t) >> 5 : t;
    }
}

/* The shaded pass's paint key for one moby: the row, and the integer
 * translation and screen offset its RTPS loads. A billboard loads
 * TR = (0, 0, Z >> 1) and its screen position as the offset (r_moby.s
 * 4155-4166); a 3D moby loads shaded_int_tr. */
typedef struct { moby_otk_rec_t rec; int tr[3]; double ofx, ofy; } shaded_otk_t;
/** @brief Build one shaded moby's paint key, integer translation and screen offset.
 *
 * A billboard takes TR = (0, 0, pos z >> 1) and its position as the screen
 * offset; a 3D moby takes shaded_int_tr from its draw-scale byte and keeps the
 * given offset. The row is moby_game.shaded_otkey_row while the key is wanted,
 * else MOBY_OTK_NONE.
 * @param cpu The guest CPU state, read only.
 * @param m The moby's PSX address.
 * @param billboard Nonzero for a billboard moby.
 * @param PM The view rotation.
 * @param cam The camera position.
 * @param pos The moby position.
 * @param ofx The view's screen offset X for a 3D moby.
 * @param ofy The view's screen offset Y.
 * @param o Receives the record, translation and offset. */
static void shaded_otk_record(CPUState* cpu, uint32_t m, int billboard, const int PM[3][3],
                              const int32_t cam[3], const int32_t pos[3], double ofx,
                              double ofy, shaded_otk_t* o) {
    o->tr[0] = o->tr[1] = o->tr[2] = 0;
    o->ofx = ofx; o->ofy = ofy;
    if (billboard) {
        o->tr[2] = pos[2] >> 1;
        o->ofx = pos[0]; o->ofy = pos[1];
    } else {
        shaded_int_tr(PM, cam, pos, cpu->read_byte(m + game_layout.moby.size_off), o->tr);
    }
    moby_otk_record(moby_otkey_wanted() ? moby_game.shaded_otkey_row : MOBY_OTK_NONE, 0,
                    o->tr[2], &o->rec);
}

/** @brief Project every drawn shaded moby's verts, billboard or 3D, into out, and record each moby's metadata in s_shrecs.
 *
 * Walks the shaded Moby* list and skips what the shaded draw skips: a
 * render-radius byte of 0 (a dormant moby), and, for a 3D moby outside
 * extended LOD, a position outside the draw's three-axis distance band.
 * s_shrecs carries each moby's vertex base and count, face stream, lighting
 * inputs, static colour table and paint key for build_shaded_walk_tris.
 * Refreshes the directional light (s_shL) first.
 * @param cpu The guest CPU state, read only.
 * @param out Receives the projected vertices.
 * @param cap The capacity of out.
 * @return The number of vertices written. */
static int project_all_shaded(CPUState* cpu, proj_xy* out, int cap) {
    const ngte_view* v = &s_view_shd;
    int PM[3][3]; memcpy(PM, v->cam_M, sizeof PM);
    int32_t cx=(int32_t)v->cam_ram[0], cy=(int32_t)v->cam_ram[1], cz=(int32_t)v->cam_ram[2];
    /* Sub-tick floor fix: fractional interpolated camera (un-rounded) for the
     * camera-relative delta below; the floored RAM at 30 Hz. */
    double cfx=v->cam[0], cfy=v->cam[1], cfz=v->cam[2];
    double ofx=v->ofx, ofy=v->ofy;
    /* directional light dir (int16-truncated on ctc2 into the LCM), refreshed
     * each frame from D_800770C8+0/+4/+8 (block_80022AD0). */
    s_shL[0]=(int16_t)cpu->read_word(moby_game.spec_light+0); s_shL[1]=(int16_t)cpu->read_word(moby_game.spec_light+4); s_shL[2]=(int16_t)cpu->read_word(moby_game.spec_light+8);
    int n=0; s_nshrecs = 0;
    for (int r=0; r<256; r++) {
        uint32_t m = cpu->read_word(SHADED_BASE + (uint32_t)r*4u);
        if (m < 0x80000000u) break;
        /* Dormant/uninit-moby skip. SHADED_BASE is a pre-built list that the .s
         * shaded renderer (func_80022A2C) walks; for each moby it reads
         * m_RenderRadius (+0x50) as the cull half-width `a0` and keeps it only if
         * -a0 < (mx-camx)>>2 < a0. m_RenderRadius==0 makes that band degenerate
         * (every position fails one side), so the game ALWAYS skips a radius-0 moby
         * - the mechanism behind in-spawner objects that are dormant until released
         * (e.g. cliff-town's cauldron gems, which sit at m_RenderRadius 0 until you
         * flame the cauldron). The list still contains them, so replicate the skip
         * here, else they render as ghost gems beside their spawner. (m_RenderRadius
         * & 0x80 is the billboard/HUD flag, handled below - nonzero, so it passes.) */
        if (cpu->read_byte(m + moby_game.render_radius_off) == 0) continue;
        /* .s in-walk 3-axis distance-band cull (block_80022B40..80022B90) -
         * NON-BILLBOARD mobys only (the lhu@80022B20 halfword's bit7 <<24 sign
         * branches billboards past it). eff = (m_RenderRadius & 0x7F) << 8,
         * plus 0x200 when the SAME lhu's high byte (m_WasDrawn, +0x51) has
         * bit0 set - a one-tick hysteresis so boundary mobys don't flicker.
         * The moby is SKIPPED unless (pos−cam)>>2 is STRICTLY inside ±eff on
         * ALL THREE axes (camera = the canonical g_Camera+0x28.. words the
         * walk stashes at scratch+0x3E0 - the same cx/cy/cz above; axis signs
         * and >>2 rounding mirrored exactly). This in-WALK test - not the
         * queue - is what hides distant gems/props on PSX (the SHADED_BASE
         * list still contains them); without it native drew every listed
         * moby at any distance (the distant-gem leak).
         * Extended LOD drops the band outright (the reveal builder's twin -
         * saturating to 0x7F still hid every gem once the camera cleared
         * ~130 K world units). The m_Distance==0 dormant sentinel never
         * reaches here (skipped above), so dropping the test cannot resurrect
         * a dormant gem; billboards already bypass it. */
        {
            uint8_t r50 = cpu->read_byte(m + moby_game.render_radius_off);
            if (!(r50 & 0x80) && !openpete_no_lod) {
                int eff = (int)(r50 & 0x7F);
                int32_t a0 = ((int32_t)eff << 8)
                           + ((cpu->read_byte(m + game_layout.moby.drawn_off) & 1) ? 0x200 : 0);
                int32_t t;
                t = ((int32_t)cpu->read_word(m + game_layout.moby.pos_off + 0u) - cx) >> 2;
                if (t + a0 <= 0 || t - a0 >= 0) { s_shband_cull++; continue; }
                t = (cy - (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u)) >> 2;
                if (t + a0 <= 0 || t - a0 >= 0) { s_shband_cull++; continue; }
                t = (cz - (int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u)) >> 2;
                if (t + a0 <= 0 || t - a0 >= 0) { s_shband_cull++; continue; }
            }
        }
        uint32_t cls = cpu->read_half(m + game_layout.moby.class_off);
        uint32_t mraw  = cpu->read_word(game_layout.moby.model_table + cls*4u); /* RAW entry - bit31 is a colour-mode switch */
        uint32_t model = mraw;                              if (model<0x80000000u) model|=0x80000000u;
        uint32_t vraw  = cpu->read_word(model + 0x4u);
        uint32_t vbase = vraw;                              if (vbase<0x80000000u) vbase|=0x80000000u;
        int count = cpu->read_byte(model + 0x0u) & 0xFF;
        uint32_t faces = cpu->read_word(model + 0xCu); if (faces<0x80000000u) faces|=0x80000000u;  /* shaded face stream */
        int fcount = cpu->read_byte(model + 0x1u) & 0xFF;   /* face count: base model+0xC, count model+0x1, stride 8 */
        /* STATIC-GOURAUD colour mode (.s block_800230EC..80023268):
         * after the vertex loop the .s compares (vbase<<1>>1) against
         * raw_g_Models_entry + 0x10. Equal (entry stored WITHOUT bit31 and verts
         * inline at model+0x10) -> it SKIPS the per-vertex lighting pre-pass and
         * takes s4 = [model+8] - the model's BAKED colour table - in the branch
         * delay slot at 0x800230FC. Mode-0 face colour fields are then BYTE
         * OFFSETS into that table (0x00BBGGRR words, emitted verbatim). bit31
         * SET in the entry makes the compare fail -> lit-gouraud (+0x2A00 path).
         * (Lit-gouraud on a static-gouraud model paints it uniform gray.) */
        uint32_t ctab = 0; int ctab_len = 0;
        if (!(mraw & 0x80000000u) && (vraw & 0x7FFFFFFFu) == ((mraw & 0x7FFFFFFFu) + 0x10u)) {
            uint32_t craw = cpu->read_word(model + 0x8u);
            ctab = craw; if (ctab < 0x80000000u) ctab |= 0x80000000u;
            /* table spans [model+8 ptr, faces ptr) - bound the face-field offsets */
            ctab_len = (faces > ctab && faces - ctab < 0x2000u) ? (int)(faces - ctab) : 0x800;
        }
        int sv_off = n;
        int32_t px=(int32_t)cpu->read_word(m+0xC), py=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u), pz=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
        int billboard = (cpu->read_byte(m + moby_game.render_radius_off) & 0x80) != 0;
        uint32_t rot = cpu->read_word(m + game_layout.moby.angle_off + 0u);   /* m_Rotation (3 euler bytes) */
        /* OT-bias overlay flag: the .s billboard path (block_80022D90) computes the
         * OT index as (pz>>7) - ((int32_t)m_0x44 >> 24) clamped >= 0. A large bias
         * byte forces the index to 0 - the NEAR OT slot - so the moby draws OVER the
         * world/terrain (but still UNDER Spyro, which renders in a later pass). This
         * is how the game keeps HUD icons, the "DEMO MODE" banner, and the level-
         * intro text on top of the scene. Mirror it: such mobys go to the HUD/overlay
         * layer instead of being world-depth-sorted (else nearer terrain occludes
         * them). Only billboards carry this; 3D shaded mobys keep their own depth. */
        int near_ot = billboard && (((pz >> 7) - ((int32_t)rot >> 24)) <= 0);
        /* Per-record view: a billboard anchors at a game-authored screen
         * coordinate (mofx/mofy = its screen position) and a HUD-range moby
         * is drawn where the game put it, so both project canonical; a 3D
         * actor projects through the pass view. */
        int hud_rec = openpete_ui_moby_is_addr_static(m) || openpete_ui_moby_is_addr_ui(m);
        const ngte_view* rv = (billboard || hud_rec) ? &s_view_shd_canon : v;
        if (billboard && !hud_rec && !near_ot) NGTE_CENSUS_HIT(bb_untagged);
        int M[3][3]; double TR[3]; double mofx, mofy;
        double vz_scale = 1.0;   /* depth -> full-world (set for the 3D path below) */
        if (billboard) {
            /* base = [4096 0 0/0 2560 0/0 0 4096] composed with the moby euler
             * (block_80022D64 base, then the Ry/Rx/Rz MVMVA chain). */
            int base[3][3]={{4096,0,0},{0,2560,0},{0,0,4096}};
            shaded_compose_euler(cpu, base, m, M);
            /* Billboard position interp: px/py here are SCREEN
             * coords the game animates per tick (pz = depth) - fed raw, a
             * moving billboard (the level-transition tally gems flying to the
             * chest) stepped at 30 Hz regardless of --render-fps. Prefer the
             * spine's fractional position; else lerp the interp-objs prev/curr
             * at the rotation-interp clock (same guards as the moby lerp:
             * class unchanged + teleport snap). */
            double bfx=(double)px, bfy=(double)py, bfz=(double)pz;
            { double mf[3];
              if (openpete_native_interp_pos(m, mf)) { bfx=mf[0]; bfy=mf[1]; bfz=mf[2]; }
              else if (s_rot_now) {
                const openpete_interp_obj_t* o = moby_interp_find(m);
                if (o && o->have_prev && o->prev_ns != 0 && o->curr_ns > o->prev_ns) {
                    uint16_t pcls2, ccls2;
                    memcpy(&pcls2, o->prev + 0x36u, 2); memcpy(&ccls2, o->curr + 0x36u, 2);
                    int32_t pp[3], cp[3];
                    memcpy(pp, o->prev + 0x0Cu, 12); memcpy(cp, o->curr + 0x0Cu, 12);
                    double ddx=(double)cp[0]-pp[0], ddy=(double)cp[1]-pp[1], ddz=(double)cp[2]-pp[2];
                    /* px/py are SCREEN px - a 5000 guard never fires. 64 px/tick
                     * is ~4x the tally gem's flight speed; a same-class slot shift
                     * (gem inheriting another gem's slot) jumps farther and snaps.
                     * pz is DEPTH (the billboard's perspective scale) on a
                     * different unit: the credits letters fly in at ~375/tick, so
                     * it stays out of the guard. */
                    /* stale prev = a relaid-out submenu glyph paired with the old
                     * glyph that last held its pool slot -> snap (leave bf at the
                     * canonical curr) so the letter appears in place instead of
                     * swimming from the previous word's position. */
                    if (pcls2 == ccls2 && ddx*ddx + ddy*ddy <= 64.0*64.0 &&
                        !moby_interp_pair_stale(o->prev_ns, o->curr_ns)) {
                        double t = moby_pair_alpha(o);
                        bfx = (double)pp[0] + ddx * t;
                        bfy = (double)pp[1] + ddy * t;
                        bfz = (double)pp[2] + ddz * t;
                    }
                }
              } }
            TR[0]=0; TR[1]=0; TR[2]=bfz/2.0; mofx=bfx; mofy=bfy;
            /* OPENPETE_SHADED_POSTRACE also traces the billboard branch. */
            {
                static int s_bt0 = -2;
                if (s_bt0 == -2) { const char* e = getenv("OPENPETE_SHADED_POSTRACE"); s_bt0 = (e && *e) ? atoi(e) : -1; }
                if (s_bt0 >= 0 && (int)openpete_frame >= s_bt0 && (int)openpete_frame < s_bt0 + 6)
                    fprintf(stderr, "[bb-pos vbl=%u] moby=%08x key=%08x cls=%03x bf=(%.2f,%.2f,%.2f) raw=(%d,%d,%d)\n",
                            openpete_frame, m, moby_interp_key(m), cls, bfx, bfy, bfz, px, py, pz);
            }
            /* Like the 3D path, TR[2] is HALF the true view-Z (the .s sets the GTE
             * depth to pz>>1, but orders the prim in the OT by pz>>7 - i.e. the FULL
             * pz). Scale the fed depth back to full-world so billboards (e.g. the
             * level-intro "THE ADVENTURE BEGINS" text, and any sprite moby) sort vs
             * terrain / Spyro / 3D mobys on the same scale. Without this they sit at
             * half-depth - artificially near - and a billboard wrongly occludes
             * actors that are actually in front of it (Spyro drew BEHIND the text). */
            vz_scale = 2.0;
        } else {
            /* model->view = PM (camera ProjMatrix) composed with the full moby
             * euler rotation (block_80022CE4 PM start, then the same chain). */
            shaded_compose_euler(cpu, PM, m, M);
            /* TR (moby centre in view space) = the .s's swizzled centre MVMVA
             * (block_80022B40..80022D18): IR=((cam.y-my)>>2,(cam.z-mz)>>2,(mx-cam.x)>>2)
             * loaded as (IR1,IR2,IR3); MAC=(PM·IR)>>12; TR=MAC<<1; then an optional
             * GPF perspective scale by m_DrawScale (moby+0x57). NOT the naive
             * PM·(pos-cam) - that swizzle/scale mismatch sinks 3D mobys (gems)
             * below ground. */
            /* Delta as ÷4.0 (not >>2) so shaded mobys (gems/props) track the smooth
             * sub-tick camera instead of stepping in 4-world-unit chunks - same fix
             * as the regular path + terrain. TR is fractional; scale unchanged.
             * Floor fix: also prefer the FRACTIONAL interpolated moby position so the
             * moby's own motion is continuous (not just the camera component). */
            double mfx=(double)px, mfy=(double)py, mfz=(double)pz;
            int fpos_hit;
            { double mf[3]; fpos_hit = openpete_native_interp_pos(m, mf); if (fpos_hit) { mfx=mf[0]; mfy=mf[1]; mfz=mf[2]; } }
            /* OPENPETE_SHADED_INTERP_MISS: shaded mobys with NO fractional
             * position (not in the spine's lerp walk -> they step at 30 Hz). */
            {
                static int s_smiss = -1;
                if (s_smiss < 0) { const char* e = getenv("OPENPETE_SHADED_INTERP_MISS"); s_smiss = (e && *e && *e != '0'); }
                if (s_smiss && !fpos_hit) {
                    static uint64_t t0 = 0; static int nn2 = 0;
                    uint64_t nowp = openpete_phase_now_us();
                    if (nowp - t0 >= 1000000u) { t0 = nowp; nn2 = 0; }
                    if (nn2 < 12) { nn2++;
                        fprintf(stderr, "[shaded-interp-MISS vbl=%u] moby=%08x cls=%03x pos=(%d,%d,%d)\n",
                                openpete_frame, m, cls, px, py, pz); }
                }
            }
            /* OPENPETE_SHADED_POSTRACE=frameLo: per-PRESENT fractional position of
             * every cls-0x53 gem for ~40 frames from frameLo - does the fed
             * position CHANGE between the presents of one tick? */
            {
                static int s_ptr0 = -2;
                if (s_ptr0 == -2) { const char* e = getenv("OPENPETE_SHADED_POSTRACE"); s_ptr0 = (e && *e) ? atoi(e) : -1; }
                if (s_ptr0 >= 0 &&
                    (int)openpete_frame >= s_ptr0 && (int)openpete_frame < s_ptr0 + 6)
                    fprintf(stderr, "[gem-pos vbl=%u] moby=%08x hit=%d fp=(%.2f,%.2f,%.2f) raw=(%d,%d,%d)\n",
                            openpete_frame, m, fpos_hit, mfx, mfy, mfz, px, py, pz);
            }
            double Dx=(mfx-cfx)/4.0, Dy=(cfy-mfy)/4.0, Dz=(cfz-mfz)/4.0;
            double t1=(PM[0][0]*Dy+PM[0][1]*Dz+PM[0][2]*Dx)/4096.0;
            double t2=(PM[1][0]*Dy+PM[1][1]*Dz+PM[1][2]*Dx)/4096.0;
            double t3=(PM[2][0]*Dy+PM[2][1]*Dz+PM[2][2]*Dx)/4096.0;
            TR[0]=t1*2.0; TR[1]=t2*2.0; TR[2]=t3*2.0;
            uint8_t p57=cpu->read_byte(m + game_layout.moby.size_off);
            /* Draw-scale (p57): apply to the FRACTIONAL TR directly - no int16 round.
             * (An int16 truncation here would re-quantize the fractional TR
             * back to a ~1-unit grid for draw-scaled shaded mobys, undoing the ÷4.0
             * delta. The GTE's IR16 saturation only
             * matters for off-screen/huge TR, which project()'s sat flag culls.) */
            if (p57){ for(int i=0;i<3;i++){ TR[i]=(double)p57 * TR[i] / 32.0; } }
            mofx=ofx; mofy=ofy;
            /* TR here = trueViewZ/2 (D=(pos-cam)>>2 then MAC<<1), or x p57/32 with
             * draw-scale: scale the fed depth back to full-world so shaded mobys
             * (gems) sort vs terrain/Spyro. p57=0 -> x2;
             * p57!=0 -> x64/p57. Sign-preserving (near cull intact); depth-only. */
            vz_scale = p57 ? 64.0 / (double)p57 : 2.0;
        }
        shaded_otk_t sk;
        {
            const int32_t cam3[3] = { cx, cy, cz }, pos3[3] = { px, py, pz };
            shaded_otk_record(cpu, m, billboard, PM, cam3, pos3, ofx, ofy, &sk);
        }
        int vzmin=1<<30, vzmax=-(1<<30);   /* model-space Z extent (HUD3D 3D-vs-flat test) */
        double rec_minfvz = 1e30;          /* nearest in-front vertex after pulls (near-floor lift) */
        double pvzmin=1e30, pvzmax=-1e30;  /* projected view-Z extent (rotation-aware half) */
        /* .s OT bias, SHADED path: the walk subtracts the SIGNED m_Rotation>>24
         * byte from the moby's OT index (0x80022D9C: t9 -= s4>>24; OT slots are
         * 128 world units). Like the regular walk's m_DepthOffset, that bias is
         * an OT ORDERING KEY, not geometry - it never enters stored depth
         * (world-scale pulls punched mobys through genuinely-occluding
         * terrain). Coincident-surface intent (the portal-name text glyphs
         * sitting ON the arch stone, beast-makers-wild-flight-text f3310)
         * survives as the sub-slot tie pull below. Billboards handle their
         * bias via near_ot above. */
        static int s_otbias_on = -1;   /* OPENPETE_SHADED_OTBIAS=0 disables the tie pull (A/B) */
        if (s_otbias_on < 0) { const char* e = getenv("OPENPETE_SHADED_OTBIAS"); s_otbias_on = (e && *e) ? (*e != '0') : 1; }
        /* Tie-pull gate (shaded twin of moby_decode_project's): applied only
         * when the moby centre is in front of the camera, so behind-camera
         * records keep raw signed depths for the vz<=0 near cull. */
        double ot_centre = 0.0;
        if (!billboard && s_otbias_on) {
            double centre_full = TR[2] * vz_scale;
            if (centre_full > 0.0)
                ot_centre = centre_full;
        }
        for (int i=0;i<count && n<cap;i++) {
            uint32_t p=vbase+(uint32_t)i*3u;
            uint32_t w=cpu->read_byte(p)|(cpu->read_byte(p+1)<<8)|(cpu->read_byte(p+2)<<16);
            int VZ=((int32_t)(w<<24))>>23, VX=((int32_t)(w<<16))>>23, VY=((int32_t)(w<<8))>>23;
            if (VZ<vzmin) vzmin=VZ; if (VZ>vzmax) vzmax=VZ;
            double sx,sy; int sat;
            /* TRUE per-vertex view-Z (project returns mac3). Stamping the
             * moby-constant TR[2] on every vertex would collapse within-moby
             * depth so the Z-buffer couldn't separate a moby's faces
             * (interior/back faces would punch through the front). Per-vertex Z is
             * what the real Z-buffer needs. */
            double vzz = project(M,TR,(double)VX,(double)VY,(double)VZ, mofx,mofy,rv, &sx,&sy,&sat);
            int unr = moby_gte_near_fixup(M, TR, (double)VX,(double)VY,(double)VZ, vzz,
                                          mofx, mofy, rv, &sx, &sy);
            if (vzz > 0.0) { if (vzz<pvzmin) pvzmin=vzz; if (vzz>pvzmax) pvzmax=vzz; }
            double fvz = vzz*vz_scale;
            /* Fed depth stays TRUE per-vertex (see the regular walk's twin):
             * the game's OT is per-face, so per-pixel Z over true depth is
             * the faithful mirror; only the sub-slot tie pull shifts depth. */
            /* One-slot tie pull - see the regular walk's twin above. The
             * pedestal discs (dofs 0) rely on it for the ground they touch. */
            if (ot_centre > 0.0) fvz -= OPENPETE_MOBY_TIE_PULL_VZ;
            if (vzz > 0.0 && fvz < rec_minfvz) rec_minfvz = fvz;
            out[n].sx=(float)sx; out[n].sy=(float)sy; out[n].vz=(float)fvz; out[n].sat=sat; out[n].unr=unr;
            out[n].tvz=(float)(vzz*vz_scale);            /* pre-bias: the perspective W */
            out[n].ex=out[n].ey=out[n].ez=0.0f;   /* env-UV unused on shaded faces */
            moby_otk_vertex(&sk.rec, sk.tr, M, VX, VY, VZ, sk.ofx, sk.ofy, (int)rv->H_canon,
                            &out[n]);
            n++;
        }
        /* Near-floor lift, per moby (see moby_decode_project's twin): uniform,
         * never a per-vertex flatten. Positive-vz verts only set the floor. */
        if (rec_minfvz < 1.0 && rec_minfvz > -1e29) {
            float lift = (float)(1.0 - rec_minfvz);
            for (int vi = sv_off; vi < n; vi++) out[vi].vz += lift;
        }
        if (s_nshrecs < 256) {
            uint32_t scolor = cpu->read_word(m + moby_game.spec_colour_off);   /* m_SpecularMetalColor */
            uint32_t base = cpu->read_word(moby_game.shade_coltab + (uint32_t)((int32_t)scolor >> 22));
            shaded_rec* R = &s_shrecs[s_nshrecs++];
            R->sv_off=sv_off; R->vcount=n-sv_off; R->faces=faces; R->fcount=fcount; R->cls=(int)cls;
            R->model=model; R->base=base; R->moby=m; R->near_ot=near_ot;
            R->ctab=ctab; R->ctab_len=ctab_len; R->otk=sk.rec;
            R->vzspread = (vzmax>vzmin) ? (vzmax-vzmin) : 0;
            R->pvzspread = (pvzmax>pvzmin) ? (float)(pvzmax-pvzmin) : 0.0f;
            for (int i=0;i<3;i++) for (int j=0;j<3;j++) R->RT[i][j]=M[i][j];
        }
    }
    /* Idle-anim census (~1/s, under OPENPETE_MOBY_TIER_DBG): overridden-gem
     * composes + fresh seeds since the last print. active counts euler composes
     * (canonical + per-present re-extract), so it scales with render fps -
     * nonzero = the override runs. */
    if (moby_idle_on() && tier_dbg_on() && (s_idle_active || s_idle_seeded || s_idle_live)) {
        static unsigned next_idle;
        if (openpete_frame >= next_idle) {
            next_idle = openpete_frame + 60u;
            fprintf(stderr, "[moby-idle vbl=%u] active=%ld seeded=%ld live=%ld\n",
                    openpete_frame, s_idle_active, s_idle_seeded, s_idle_live);
            s_idle_active = 0; s_idle_seeded = 0; s_idle_live = 0;
        }
    }
    return n;
}

/** @brief The bucket word of one shaded face, or -1 when the pass gives it none.
 *
 * The pass's row through moby_otk_face_bucket, with the game's face
 * treatment (moby_game.shaded_otkey_face) applied to the face words; -1 when
 * the paint key has no row or a quad's fourth vertex lies past vc. i2 == i3
 * names a triangle.
 * @param rec The moby's paint key.
 * @param base The moby's first projected vertex.
 * @param i0 The first vertex slot.
 * @param i1 The second vertex slot.
 * @param i2 The third vertex slot.
 * @param i3 The fourth vertex slot, equal to i2 on a triangle.
 * @param vc The moby's vertex count.
 * @param w0 The face's first word.
 * @param w1 The face's second word.
 * @return The bucket word, or -1. */
static int shaded_face_otkey(const moby_otk_rec_t* rec, const proj_xy* base, int i0, int i1,
                             int i2, int i3, int vc, uint32_t w0, uint32_t w1) {
    if (rec->row == MOBY_OTK_NONE || (i2 != i3 && i3 >= vc)) return -1;
    moby_otk_face_t f = { { &base[i0].iv, &base[i1].iv, &base[i2].iv,
                            i2 == i3 ? NULL : &base[i3].iv }, 0, INT32_MIN, 0, 0 };
    moby_game.shaded_otkey_face(w0, w1, &f);
    return moby_otk_face_bucket(rec, &f);
}

/** @brief Whether the shaded draw keeps one face, and whether it takes it as back-facing.
 *
 * Outside the metal near band a triangle is kept on NCLIP(v0,v1,v2) > 0 and
 * a quad on that or NCLIP(v3,v1,v2) < 0; a metal quad kept on the second
 * test is back-facing, since the draw's MAC0 then holds the negative second
 * NCLIP (r_moby.s 4853-4861). Inside the band a metal face is kept at either
 * facing and is back-facing when NCLIP(v0,v1,v2) < 0 (r_moby.s 4844-4851).
 * @param metal Nonzero for a metal face (word 0 bits 0 and 1).
 * @param in_band Nonzero when the moby's TRZ is below 0x800.
 * @param tri Nonzero for a triangle.
 * @param n1 The float NCLIP of (v0,v1,v2).
 * @param n2 The float NCLIP of (v3,v1,v2), unused on a triangle.
 * @param back Receives 1 for a back-facing metal face, else 0.
 * @return 1 when the face is kept, 0 when the draw drops it. */
static int shaded_face_keep(int metal, int in_band, int tri, double n1, double n2, int* back) {
    *back = 0;
    if (metal && in_band) { *back = n1 < 0.0; return 1; }
    if (n1 > 0.0) return 1;
    if (tri || !(n2 < 0.0)) return 0;
    *back = metal;
    return 1;
}

/** @brief The four vertex colours of one shaded face by its shading mode.
 *
 * Word 0 bit 0 selects flat shading, lit once from word 1's face normal: with
 * bit 1 set the metal tint (shaded_metal_color), otherwise the directional
 * light on the flat table base. Gouraud reads word 1's colour fields
 * (>> {21,14,7,0} & 0x1FC): byte offsets into the baked table of a
 * static-gouraud record, else slots of the lit per-vertex colours. A field
 * past its table gives 0x808080.
 * @param cpu The guest CPU state, read only.
 * @param R The moby's shaded record.
 * @param w0 The face's first word.
 * @param w1 The face's second word.
 * @param pcol The lit per-vertex colours.
 * @param pcn The number of entries in pcol.
 * @param mw0 The metal table entry's base-tint word.
 * @param mw1 The metal table entry's reflected-tint word.
 * @param back Nonzero for a back-facing metal face.
 * @param c0 Receives the first vertex colour.
 * @param c1 Receives the second vertex colour.
 * @param c2 Receives the third vertex colour.
 * @param c3 Receives the fourth vertex colour. */
static void shaded_face_colors(CPUState* cpu, const shaded_rec* R, uint32_t w0, uint32_t w1,
                               const uint32_t* pcol, int pcn, uint32_t mw0, uint32_t mw1,
                               int back, uint32_t* c0, uint32_t* c1, uint32_t* c2,
                               uint32_t* c3) {
    uint32_t* c[4] = { c0, c1, c2, c3 };
    if (w0 & 1u) {
        int fnz = (int8_t)(w1 >> 24), fnx = (int8_t)(w1 >> 16), fny = (int8_t)(w1 >> 8);
        uint32_t v = (w0 & 2u) ? shaded_metal_color(R->RT, fnx, fny, fnz, mw0, mw1, back)
                               : shaded_lit_color(R->RT, fnx, fny, fnz, R->base, 0);
        for (int k = 0; k < 4; k++) *c[k] = v;
        return;
    }
    for (int k = 0; k < 4; k++) {
        uint32_t o = (w1 >> (21 - 7 * k)) & 0x1FCu;
        if (R->ctab) *c[k] = o < (uint32_t)R->ctab_len ? cpu->read_word(R->ctab + o) & 0xFFFFFFu : 0x808080u;
        else *c[k] = (int)(o >> 2) < pcn ? pcol[o >> 2] : 0x808080u;
    }
}

/** @brief OPENPETE_SHADED_FACEDBG="cls,vbl": per-face NCLIP forensics for one class over two VBLs.
 *
 * Prints the float NCLIPs against the GTE-style integer NCLIP on truncated
 * SXY, to catch sign flips on near-edge-on faces.
 * @param R The moby's shaded record.
 * @param f The face's stream index.
 * @param i0 The first vertex slot.
 * @param i1 The second vertex slot.
 * @param i2 The third vertex slot.
 * @param i3 The fourth vertex slot.
 * @param tri Nonzero for a triangle.
 * @param p0 The first projected vertex.
 * @param p1 The second projected vertex.
 * @param p2 The third projected vertex.
 * @param ncl1 The float NCLIP of (v0,v1,v2).
 * @param ncl2 The float NCLIP of (v3,v1,v2), 0 on a triangle.
 * @param emitted The triangles the face emitted. */
static void shaded_face_dbg(const shaded_rec* R, int f, int i0, int i1, int i2, int i3, int tri,
                            const proj_xy* p0, const proj_xy* p1, const proj_xy* p2,
                            double ncl1, double ncl2, int emitted) {
    static long s_fd_cls = -2, s_fd_f0 = -1;
    if (s_fd_cls == -2) {
        const char* e = getenv("OPENPETE_SHADED_FACEDBG");
        s_fd_cls = -1;
        if (e && *e) { char* c = NULL; s_fd_cls = strtol(e, &c, 0);
                       if (c && *c == ',') s_fd_f0 = strtol(c+1, NULL, 0); }
    }
    if (s_fd_cls < 0 || (int)s_cur_moby_cls != (int)s_fd_cls ||
        (int)openpete_frame < s_fd_f0 || (int)openpete_frame >= s_fd_f0 + 2) return;
    long i0i = (long)((int)p1->sx - (int)p0->sx) * ((int)p2->sy - (int)p0->sy)
             - (long)((int)p2->sx - (int)p0->sx) * ((int)p1->sy - (int)p0->sy);
    fprintf(stderr, "[shface vbl=%u] moby=%08x cls=%03x face=%d idx=%d,%d,%d,%d "
            "p0=(%.2f,%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f)%s ncl1=%.3f ncl2=%.3f int1=%ld emit=%d\n",
            openpete_frame, R->moby, s_cur_moby_cls, f, i0, i1, i2, i3,
            (double)p0->sx, (double)p0->sy, (double)p1->sx, (double)p1->sy,
            (double)p2->sx, (double)p2->sy, tri ? " TRI" : " QUAD",
            ncl1, ncl2, i0i, emitted);
}

/* One in-band metal triangle the shaded walk holds for the moby's composite:
 * its projected vertices, flat colour, paint bucket and face identity. */
typedef struct {
    const proj_xy* v[3];
    uint32_t col;
    int32_t  otk;
    uint32_t fkey;
} metal_tri;

/* The in-band metal triangles of the moby being walked: the back faces as
 * drawn, and the front faces held until the moby's ABR is known. */
#define METAL_TRI_MAX 512
static metal_tri s_metal_back[METAL_TRI_MAX], s_metal_front[METAL_TRI_MAX];
static int s_metal_back_n, s_metal_front_n;

/** @brief One channel of a semi-transparent face over the pixel beneath it, by ABR.
 *
 * ABR 0 is (B + F) / 2, 1 is B + F, 2 is B - F, 3 is B + F / 4, clamped to
 * 0..255.
 * @param abr The ABR in force.
 * @param b The channel beneath.
 * @param f The face's channel.
 * @return The composed channel. */
static int metal_abr_channel(int abr, int b, int f) {
    int v = abr == 0 ? (b + f) >> 1 : abr == 1 ? b + f : abr == 2 ? b - f : b + (f >> 2);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/** @brief A face colour composed over another by ABR, both 0x00BBGGRR.
 *
 * @param abr The ABR in force.
 * @param b The colour beneath.
 * @param f The face colour.
 * @return The composed colour. */
static uint32_t metal_abr_compose(int abr, uint32_t b, uint32_t f) {
    uint32_t out = 0u;
    for (int k = 0; k < 24; k += 8)
        out |= (uint32_t)metal_abr_channel(abr, (int)((b >> k) & 0xFFu), (int)((f >> k) & 0xFFu)) << k;
    return out;
}

/* A screen-space clip polygon vertex. */
typedef struct { double x, y; } mclip_pt;
#define MCLIP_MAX 12

/** @brief Whether point a orders before point b, x first, then y. */
static int mclip_before(mclip_pt a, mclip_pt b) {
    return a.x < b.x || (a.x == b.x && a.y < b.y);
}

/** @brief Twice the signed area of (a, b, p), with (a, b) taken in canonical order.
 *
 * Canonical order makes the value for an edge shared by two triangles the
 * same number in both, up to sign.
 * @param a One end of the line.
 * @param b The other end.
 * @param p The point.
 * @param flip Receives -1 when the canonical order swapped a and b, else 1.
 * @return The cross product. */
static double mclip_side(mclip_pt a, mclip_pt b, mclip_pt p, int* flip) {
    *flip = 1;
    if (mclip_before(b, a)) { mclip_pt t = a; a = b; b = t; *flip = -1; }
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

/** @brief Clip a convex polygon to the inside of one edge of a triangle.
 *
 * The crossing point of a polygon edge is taken with that edge's ends in
 * canonical order, so two polygons sharing the edge get bit-identical
 * points and their pieces meet without a crack.
 * @param in The polygon.
 * @param n Its vertex count.
 * @param a The edge's first end.
 * @param b The edge's second end.
 * @param sgn 1 when the inside is the positive side of (a, b), else -1.
 * @param out Receives the clipped polygon.
 * @return The clipped vertex count. */
static int mclip_edge(const mclip_pt* in, int n, mclip_pt a, mclip_pt b, int sgn, mclip_pt* out) {
    int m = 0;
    for (int i = 0; i < n && m < MCLIP_MAX - 1; i++) {
        mclip_pt p = in[i], q = in[(i + 1) % n];
        int fp, fq;
        double dp = mclip_side(a, b, p, &fp) * fp * sgn, dq = mclip_side(a, b, q, &fq) * fq * sgn;
        if (dp >= 0.0) out[m++] = p;
        if ((dp >= 0.0) != (dq >= 0.0)) {
            mclip_pt s0 = p, s1 = q; double d0 = dp, d1 = dq;
            if (mclip_before(q, p)) { s0 = q; s1 = p; d0 = dq; d1 = dp; }
            double t = d0 / (d0 - d1);
            out[m++] = (mclip_pt){ s0.x + t * (s1.x - s0.x), s0.y + t * (s1.y - s0.y) };
        }
    }
    return m;
}

/** @brief The part of a screen triangle inside another, as a convex polygon.
 *
 * @param f The clipped triangle.
 * @param b The clipping triangle.
 * @param out Receives the polygon.
 * @return Its vertex count, below 3 when the triangles do not overlap. */
static int mclip_tri(const mclip_pt f[3], const mclip_pt b[3], mclip_pt* out) {
    mclip_pt buf[2][MCLIP_MAX];
    int n = 3;
    for (int i = 0; i < 3; i++) buf[0][i] = f[i];
    int cur = 0;
    double area = (b[1].x - b[0].x) * (b[2].y - b[0].y) - (b[1].y - b[0].y) * (b[2].x - b[0].x);
    int sgn = area > 0.0 ? 1 : -1;
    for (int e = 0; e < 3 && n >= 3; e++) {
        n = mclip_edge(buf[cur], n, b[e], b[(e + 1) % 3], sgn, buf[cur ^ 1]);
        cur ^= 1;
    }
    for (int i = 0; i < n; i++) out[i] = buf[cur][i];
    return n;
}

/** @brief Twice the unsigned area of a polygon. */
static double mclip_area(const mclip_pt* p, int n) {
    double a = 0.0;
    for (int i = 0; i < n; i++)
        a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return fabs(a);
}

/** @brief A vertex on a front triangle's plane at a screen point.
 *
 * Screen barycentrics weight 1/vz and 1/tvz (perspective-correct depth) and
 * the remaining fields linearly; the integer SXY is the rounded point.
 * @param t The triangle.
 * @param p The screen point.
 * @return The vertex. */
static proj_xy metal_plane_vertex(const metal_tri* t, mclip_pt p) {
    const proj_xy *a = t->v[0], *b = t->v[1], *c = t->v[2];
    double den = (double)(b->sx - a->sx) * (c->sy - a->sy) - (double)(c->sx - a->sx) * (b->sy - a->sy);
    double wb = ((p.x - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (p.y - a->sy)) / den;
    double wc = ((b->sx - a->sx) * (p.y - a->sy) - (p.x - a->sx) * (b->sy - a->sy)) / den;
    double wa = 1.0 - wb - wc;
    proj_xy o = *a;
    o.sx = (float)p.x; o.sy = (float)p.y;
    o.vz  = (float)(1.0 / (wa / a->vz + wb / b->vz + wc / c->vz));
    o.tvz = (float)(1.0 / (wa / a->tvz + wb / b->tvz + wc / c->tvz));
    o.ex = (float)(wa * a->ex + wb * b->ex + wc * c->ex);
    o.ey = (float)(wa * a->ey + wb * b->ey + wc * c->ey);
    o.ez = (float)(wa * a->ez + wb * b->ez + wc * c->ez);
    o.sat = a->sat | b->sat | c->sat; o.unr = a->unr | b->unr | c->unr;
    o.iv.sx = (int16_t)lrint(p.x); o.iv.sy = (int16_t)lrint(p.y);
    o.iv.sz = (int32_t)lrint(o.vz);
    return o;
}

/** @brief Hold one in-band metal triangle for the moby's composite.
 *
 * A back triangle joins the back list, which the front triangles compose
 * over; a front triangle joins the front list, emitted after the face walk.
 * A full list drops the triangle.
 * @param back Nonzero for a back-facing triangle.
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @param col The flat colour.
 * @param otk The paint bucket word.
 * @param fkey The face identity. */
static void metal_hold(int back, const proj_xy* a, const proj_xy* b, const proj_xy* c,
                       uint32_t col, int32_t otk, uint32_t fkey) {
    metal_tri* L = back ? s_metal_back : s_metal_front;
    int* n = back ? &s_metal_back_n : &s_metal_front_n;
    if (*n >= METAL_TRI_MAX) return;
    L[(*n)++] = (metal_tri){ { a, b, c }, col, otk, fkey };
}

/** @brief Emit one in-band metal front triangle composed over the moby's back faces.
 *
 * The draw paints a metal moby's back faces opaque, their buckets pushed
 * 0x200 further back, before its front faces, and a front face then blends
 * onto them at the moby's ABR (r_moby.s 4895-4899, 4952-4961). Where the
 * back triangles' screen pieces under the front triangle tile it (their
 * areas sum to its own within 0.1%), each piece is emitted opaque on the
 * front plane in the colour the blend produces over that back face, so the
 * result needs nothing behind it to survive depth or the paint-key contest.
 * Otherwise, or on a mangled vertex, the triangle is emitted semi-transparent
 * as it stands.
 * @param t The front triangle.
 * @param abr The ABR in force.
 * @param blend_class The blend code the fallback triangle carries.
 * @return The triangles emitted. */
static int metal_front_emit(const metal_tri* t, int abr, int blend_class) {
    int n0 = s_gout.n;
    mclip_pt f[3];
    int mangled = 0;
    for (int k = 0; k < 3; k++) {
        f[k] = (mclip_pt){ t->v[k]->sx, t->v[k]->sy };
        mangled |= t->v[k]->sat | t->v[k]->unr;
    }
    double fa = mclip_area(f, 3), sum = 0.0;
    static mclip_pt pc[METAL_TRI_MAX][MCLIP_MAX];
    static int pn[METAL_TRI_MAX];
    for (int j = 0; j < s_metal_back_n && !mangled; j++) {
        mclip_pt b[3];
        for (int k = 0; k < 3; k++) b[k] = (mclip_pt){ s_metal_back[j].v[k]->sx, s_metal_back[j].v[k]->sy };
        pn[j] = mclip_tri(f, b, pc[j]);
        if (pn[j] >= 3) sum += mclip_area(pc[j], pn[j]);
    }
    s_emit_fkey = t->fkey;
    if (mangled || fa <= 0.0 || fabs(sum - fa) > fa * 1e-3) {
        s_metal_cnt[8]++;
        s_emit_otk = t->otk;
        emit_wtri((proj_xy*)t->v[0], (proj_xy*)t->v[1], (proj_xy*)t->v[2],
                  t->col, t->col, t->col, 0,0,0,0,0,0, 0,0,0,0,0,16, blend_class);
        return s_gout.n - n0;
    }
    s_metal_cnt[7]++;
    for (int j = 0; j < s_metal_back_n && s_gout.n < MAX_NTRI; j++) {
        if (pn[j] < 3) continue;
        uint32_t c = metal_abr_compose(abr, s_metal_back[j].col, t->col);
        proj_xy v[MCLIP_MAX];
        for (int k = 0; k < pn[j]; k++) v[k] = metal_plane_vertex(t, pc[j][k]);
        for (int k = 1; k + 1 < pn[j] && s_gout.n < MAX_NTRI; k++) {
            s_emit_otk = t->otk;
            emit_wtri(&v[0], &v[k], &v[k + 1], c, c, c, 0,0,0,0,0,0, 0,0,0,0,0,16, 0);
        }
    }
    s_emit_otk = -1;
    return s_gout.n - n0;
}

/** @brief Emit one kept shaded face, or hold it for the moby's metal composite.
 *
 * A metal face in the near band joins the moby's metal lists (metal_hold);
 * its front side is emitted later by metal_moby_finish. Every other face
 * emits (v0,v1,v2) and, on a quad, (v1,v3,v2) with the second half's
 * bucket word, the face identity carrying bit 0 on that half.
 * @param p0 The first vertex.
 * @param p1 The second vertex.
 * @param p2 The third vertex.
 * @param p3 The fourth vertex, p2 on a triangle.
 * @param tri Nonzero for a triangle.
 * @param c The four vertex colours.
 * @param ot_bk The face's bucket word.
 * @param fkey The face identity.
 * @param metal_band Nonzero for a metal face of a moby in the near band.
 * @param back Nonzero for a back-facing face.
 * @param blend The blend code. */
static void shaded_face_emit(proj_xy* p0, proj_xy* p1, proj_xy* p2, proj_xy* p3, int tri,
                             const uint32_t c[4], int ot_bk, uint32_t fkey, int metal_band,
                             int back, int blend) {
    if (metal_band) {
        metal_hold(back, p0, p1, p2, c[0], ot_bk, fkey);
        if (!tri) metal_hold(back, p1, p3, p2, c[0], moby_otk_second(ot_bk, 1), fkey | 1u);
        if (!back) return;
    }
    s_emit_fkey = fkey;
    s_emit_otk = ot_bk;
    emit_wtri(p0,p1,p2, c[0],c[1],c[2], 0,0,0,0,0,0, 0,0,0,0,0,16, blend);
    if (!tri && s_gout.n < MAX_NTRI) {
        s_emit_fkey = fkey | 1u;
        s_emit_otk = moby_otk_second(ot_bk, 1);
        emit_wtri(p1,p3,p2, c[1],c[3],c[2], 0,0,0,0,0,0, 0,0,0,0,0,16, blend);
    }
    s_emit_otk = -1;
}

/** @brief Count one kept metal face into the shaded metal census.
 *
 * @param drawn Nonzero when the face emitted a triangle or is held to blend.
 * @param back Nonzero for a back-facing face.
 * @param in_band Nonzero when the moby is in the near band.
 * @param blended Nonzero for a semi-transparent face. */
static void metal_count_face(int drawn, int back, int in_band, int blended) {
    if (!drawn) return;
    s_metal_cnt[back ? 1 : 0]++;
    s_metal_cnt[2] += in_band;
    s_metal_cnt[3] += blended;
}

/** @brief Close one moby of the shaded walk: emit its held metal fronts and apply its DR_TPAGE ABR.
 *
 * After the moby's faces the draw emits one DR_TPAGE whose ABR is the s7
 * flags less one and links it into the chain ahead of them (r_moby.s
 * 5043-5073); the walk holds no textured face, so that ABR is in force for
 * every semi-transparent face of the moby. The held front triangles emit
 * through metal_front_emit at that ABR, then every semi-transparent
 * triangle of the moby takes the ABR's blend code.
 * @param rt0 The moby's first triangle in s_wtri.
 * @param abe_flags The moby's s7 flags: 1 flat ABE, 2 metal. */
static void metal_moby_finish(int rt0, unsigned abe_flags) {
    for (int k = 0; k < s_metal_front_n && s_gout.n < MAX_NTRI; k++) {
        int e0 = s_gout.n;
        metal_front_emit(&s_metal_front[k], (int)abe_flags - 1, 1);
        for (int t = e0; t < s_gout.n; t++) s_wtri_mode[t] = 2;
    }
    if (!abe_flags) return;
    int bl = moby_abr_blend((int)abe_flags - 1);
    for (int t = rt0; t < s_gout.n; t++) if (s_wtri[t].blend) s_wtri[t].blend = bl;
    if (s_gout.n > rt0) s_metal_cnt[4 + abe_flags - 1]++;
}

/** @brief Build s_wtri[] from the shaded pass's moby records and return the triangle count.
 *
 * project_all_shaded fills the projected vertices and s_shrecs[]; each record's
 * face stream (func_80022A2C .L800232A8) is then walked. Word 0 of a face packs
 * four vertex slots, (w0 >> {21,14,7,0}) & 0x1FC with >> 2 indexing the
 * record's projected vertices, and a triangle has slot 2 equal to slot 3. A
 * face with a slot at or past the vertex count is skipped. A kept quad emits
 * (v0,v1,v2) and (v1,v3,v2); the facing tests run on float NCLIP.
 *
 * Bit 0 of word 0 selects the shading mode and shaded_face_colors the
 * colours. shaded_face_keep decides the facing: a metal face (bits 0 and 1)
 * of a moby whose TRZ is below 0x800 is kept at either facing, the front
 * face semi-transparent and the back face opaque with the back-face colour;
 * the front faces are emitted after the walk, composed over the back faces
 * (metal_front_emit). Elsewhere a metal face is opaque and front-only. A flat-basic face of a
 * moby whose m_SpecularMetalColor >> 22 is 0 is semi-transparent. The
 * semi-transparent faces of one moby take the ABR of the DR_TPAGE the draw
 * emits for it: flags less one, flag 1 for any kept flat ABE face and flag
 * 2 for any kept metal face. OPENPETE_SHADED_NOHL drops the highlight term
 * of the gouraud colours.
 *
 * Per record it sets the HUD tag s_cur_ui (0 actor, 1 flat HUD, 2 depth-tested
 * HUD), the cutscene-front flag, class and OT bias. Each face's paint bucket
 * comes from shaded_face_otkey, and its identity key is 0x80000000 | (face <<
 * 1), bit 0 marking a quad's second half. Stops at MAX_NTRI. Reads no guest
 * prim output.
 * @param cpu The guest CPU state, read only.
 * @return The triangle count, s_gout.n. */
static int build_shaded_walk_tris(CPUState* cpu) {
    static proj_xy sv[8192]; project_all_shaded(cpu, sv, 8192);   /* fills sv[] + s_shrecs[] */
    if (s_sh_nohl < 0) { const char* e=getenv("OPENPETE_SHADED_NOHL"); s_sh_nohl = (e&&*e!='0'); }
    s_shmode[0]=s_shmode[1]=s_shmode[2]=0;
    memset(s_metal_cnt, 0, sizeof s_metal_cnt);
    s_gout.n = 0;
    for (int r = 0; r < s_nshrecs; r++) {
        shaded_rec* R = &s_shrecs[r];
        /* HUD/overlay tag (0=ACTOR, 1=HUD flat, 2=HUD3D depth-sorted). Two kinds of
         * moby reach the HUD layer: the static g_Hud icons (gem/dragon/Spyro-head/
         * key/digits) and the .s OT-bias near-OT billboards (level-intro/RESCUED
         * text, "DEMO MODE", and 3D transition props like the "TREASURE FOUND"
         * chest). The ones that are real 3D MODELS self-occlude and need their own
         * Z-buffer (HUD3D, depth-isolated, on top) or they render inside-out; thin
         * text/decal overlays (small model-space Z extent) are convex enough for
         * painter's order and stay flat HUD (drawn UNDER the player - keeps the
         * level-intro/RESCUED text behind Spyro). Static icons are always 3D -> HUD3D;
         * a near-OT/UI billboard is HUD3D only if its model Z-extent is large.
         * ROTATING thin glyphs (the inventory screen spins its letter/dragon-icon
         * mobys, swinging their width into view-Z - pvzspread measures it) are
         * NOT promoted here: promotion would move them over the player (DEMO
         * MODE letters wave at pvzspread 41-95 in ordinary attract gameplay,
         * where Spyro must cover them - measured via OPENPETE_SHADED_HUDDBG,
         * which falsified a spread-threshold promotion). Their self-occlusion is
         * instead fixed INSIDE the flat HUD layer: its draw sorts tris stably
         * back-to-front by the per-tri z (native_render.c draw_hud_flat_layer),
         * the depth-off analogue of the PSX OT bucket order. */
        int model_3d = (R->vzspread >= 96);   /* >~3× a text glyph's thickness */
        { /* OPENPETE_SHADED_HUDDBG=frameLo: per-record HUD routing forensics. */
            static int s_hd0 = -2;
            if (s_hd0 == -2) { const char* e = getenv("OPENPETE_SHADED_HUDDBG"); s_hd0 = (e && *e) ? atoi(e) : -1; }
            if (s_hd0 >= 0 && (int)openpete_frame >= s_hd0 && (int)openpete_frame < s_hd0 + 4)
                fprintf(stderr, "[hud-route vbl=%u] moby=%08x cls=%03x near_ot=%d static=%d ui=%d "
                        "vzspread=%d pvzspread=%.1f -> %s\n",
                        openpete_frame, R->moby, R->cls, R->near_ot,
                        openpete_ui_moby_is_addr_static(R->moby), openpete_ui_moby_is_addr_ui(R->moby),
                        R->vzspread, (double)R->pvzspread,
                        openpete_ui_moby_is_addr_static(R->moby) ? "HUD3D(static)"
                        : !(openpete_ui_moby_is_addr_ui(R->moby) || R->near_ot) ? "ACTOR"
                        : model_3d ? "HUD3D" : "HUDflat");
        }
        s_cur_moby_key = R->moby; s_emit_fkey = 0u;   /* face identity (shaded tris carry no topology key) */
        s_cur_moby_cls = R->moby ? (uint16_t)cpu->read_half(R->moby + game_layout.moby.class_off) : 0;
        s_cur_moby_dofs = R->moby ? (int8_t)cpu->read_byte(R->moby + game_layout.moby.angle_off + 3u) : 0; /* og OT bias (rot>>24) */
        s_cur_ui = openpete_ui_moby_is_addr_static(R->moby) ? 2
                 : (openpete_ui_moby_is_addr_ui(R->moby) || R->near_ot) ? (model_3d ? 2 : 1)
                 : 0;
        s_cur_front = s_cur_ui == 0 && moby_is_cutscene_actor(cpu, R->moby);
        proj_xy* base = sv + R->sv_off;
        int vc = R->vcount;
        /* Per-vertex gouraud lit colours from the inline normal stream
         * (model+0x10, 3 signed bytes/vertex: byte0=Nz, byte1=Nx, byte2=Ny -
         * same byte layout as the vertex positions). Computed for every moby;
         * only read for gouraud faces (flat faces light per-face below).
         * STATIC-GOURAUD models (R->ctab set) skip this: their mode-0 colour
         * fields are byte offsets into the baked table, read in the face loop. */
        static uint32_t pcol[256];
        int pcn = vc < 256 ? vc : 256;
        if (!R->ctab)
            for (int i = 0; i < pcn; i++) {
                uint32_t p = R->model + 0x10u + (uint32_t)i*3u;
                int nz=(int8_t)cpu->read_byte(p), nx=(int8_t)cpu->read_byte(p+1), ny=(int8_t)cpu->read_byte(p+2);
                pcol[i] = shaded_lit_color(R->RT, nx, ny, nz, R->base, s_sh_nohl?0:1);
            }
        /* METAL sub-mode base (word0 bit1): the gem/shiny tint comes from the
         * metal table D_8006E44C[scolor>>21] (8B: word0=base, word1=reflected),
         * NOT the flat gray table - see shaded_metal_color. Read once per moby. */
        uint32_t mscolor = cpu->read_word(R->moby + moby_game.spec_colour_off);
        uint32_t mtab = moby_game.metal_coltab + (uint32_t)((int32_t)mscolor >> 21);
        uint32_t metal_w0 = cpu->read_word(mtab);
        uint32_t metal_w1 = cpu->read_word(mtab + 4u) & 0xFFFFFFu;
        /* flat-basic ABE (func_80022A2C block_8002363C): a flat-shaded moby whose flat
         * colour-table index (m_SpecularMetalColor>>22) is 0 emits its faces
         * semi-transparent (the .s adds 0x02000000 = the GPU ABE bit, r_moby.s 4775-4779). */
        int flat_abe = (((int32_t)mscolor) >> 22) == 0;
        /* Metal near band (r_moby.s 4844-4847): TRZ - 0x800 < 0. */
        int in_band = R->otk.trz < 0x800;
        unsigned abe_flags = 0u;   /* the draw's s7 low bits: 1 flat ABE, 2 metal */
        int rt0 = s_gout.n;
        s_metal_back_n = s_metal_front_n = 0;
        uint32_t fp = R->faces;
        for (int f = 0; f < R->fcount && s_gout.n < MAX_NTRI; f++, fp += 8u) {
            uint32_t w0 = cpu->read_word(fp), w1 = cpu->read_word(fp + 4u);
            if (w0 & 1u) { if (w0 & 2u) s_shmode[2]++; else s_shmode[1]++; } else s_shmode[0]++;
            int i0 = (int)((w0 >> 21) & 0x1FCu) >> 2;
            int i1 = (int)((w0 >> 14) & 0x1FCu) >> 2;
            int i2 = (int)((w0 >>  7) & 0x1FCu) >> 2;
            int i3 = (int)( w0        & 0x1FCu) >> 2;
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
            int tri = (i2 == i3);
            if (!tri && i3 >= vc) continue;
            proj_xy* p0 = &base[i0]; proj_xy* p1 = &base[i1]; proj_xy* p2 = &base[i2];
            proj_xy* p3 = tri ? p2 : &base[i3];
            double ncl1 = tri_nclip(p0->sx,p0->sy,p1->sx,p1->sy,p2->sx,p2->sy);  /* float backface cull (see build_walk_tris) */
            double ncl2 = tri ? 0.0 : tri_nclip(p3->sx,p3->sy,p1->sx,p1->sy,p2->sx,p2->sy);
            uint8_t mode = (w0 & 1u) ? ((w0 & 2u) ? 2 : 1) : 0;
            int back = 0;
            if (!shaded_face_keep(mode == 2, in_band, tri, ncl1, ncl2, &back)) continue;
            int sh_blend = 0;
            if (mode == 2) { abe_flags |= 2u; sh_blend = in_band && !back; }
            else if (mode == 1 && flat_abe) { abe_flags |= 1u; sh_blend = 1; }
            uint32_t c0,c1,c2,c3;
            shaded_face_colors(cpu, R, w0, w1, pcol, pcn, metal_w0, metal_w1, back, &c0, &c1, &c2, &c3);
            int mt0 = s_gout.n;
            uint64_t rt0k = rt_now();
            int ot_bk = shaded_face_otkey(&R->otk, base, i0, i1, i2, i3, vc, w0, w1);
            rt_add(RT_KEY, rt0k, 1);
            /* Face identity for the paint-verdict carry and the rank
             * calibration: the face's stream index, bit 0 the quad's half. */
            const uint32_t cq[4] = { c0, c1, c2, c3 };
            shaded_face_emit(p0, p1, p2, p3, tri, cq, ot_bk, 0x80000000u | ((uint32_t)f << 1),
                             mode == 2 && in_band, back, sh_blend);
            shaded_face_dbg(R, f, i0, i1, i2, i3, tri, p0, p1, p2, ncl1, ncl2, s_gout.n - mt0);
            if (mode == 2) metal_count_face(s_gout.n > mt0 || sh_blend, back, in_band, sh_blend);
            for (int t = mt0; t < s_gout.n; t++) s_wtri_mode[t] = mode;
        }
        metal_moby_finish(rt0, abe_flags);
    }
    if (moby_verify_vbl() && s_metal_cnt[0] + s_metal_cnt[1])
        fprintf(stderr, "[moby-shaded-metal vbl=%u] front=%ld back=%ld in_band=%ld blended=%ld "
                        "abr0=%ld abr1=%ld abr2=%ld composed=%ld left_blended=%ld\n", openpete_frame,
                s_metal_cnt[0], s_metal_cnt[1], s_metal_cnt[2], s_metal_cnt[3], s_metal_cnt[4],
                s_metal_cnt[5], s_metal_cnt[6], s_metal_cnt[7], s_metal_cnt[8]);
    return s_gout.n;
}

/** @brief Print the gouraud per-vertex lighting check against the colours the shaded draw wrote to its scratch window.
 *
 * The draw writes one lit colour word per vertex to the scratch window the
 * descriptor names (moby_game.shaded_colour), for the last gouraud moby it
 * processed. The check recomputes the first eight vertex colours of each
 * shaded record with shaded_lit_color, takes the record whose colours lie
 * nearest the scratch as the one it holds, and compares up to 64 vertices
 * element-wise, with no triangle match and so no cross-moby confound. The
 * line also counts the shaded mobys by which euler axes they rotate on.
 * Called from the OPENPETE_MOBY_VERIFY path.
 * @param cpu The guest CPU state, read only. */
static void shaded_pcol_scratch_check(CPUState* cpu) {
    uint32_t SCR = moby_game.shaded_colour;
    int brec=-1; double bscore=1e18;
    for (int r=0; r<s_nshrecs; r++) {            /* find the rec the scratchpad holds */
        shaded_rec* R=&s_shrecs[r]; if (R->vcount<3) continue;
        double e=0; int nn = R->vcount<8?R->vcount:8;
        for (int i=0;i<nn;i++){
            uint32_t p=R->model+0x10u+(uint32_t)i*3u;
            int nz=(int8_t)cpu->read_byte(p), nx=(int8_t)cpu->read_byte(p+1), ny=(int8_t)cpu->read_byte(p+2);
            uint32_t mine=shaded_lit_color(R->RT,nx,ny,nz,R->base,s_sh_nohl?0:1);
            uint32_t sc=cpu->read_word(SCR+(uint32_t)i*4u)&0xFFFFFFu;
            e += abs((int)(mine&0xFF)-(int)(sc&0xFF))+abs((int)((mine>>8)&0xFF)-(int)((sc>>8)&0xFF))
               + abs((int)((mine>>16)&0xFF)-(int)((sc>>16)&0xFF));
        }
        e/=nn; if(e<bscore){bscore=e;brec=r;}
    }
    if (brec<0) return;
    shaded_rec* R=&s_shrecs[brec];
    uint32_t rot=cpu->read_word(R->moby + game_layout.moby.angle_off + 0u);
    int nn=R->vcount<64?R->vcount:64, exact=0; double err=0;
    for (int i=0;i<nn;i++){
        uint32_t p=R->model+0x10u+(uint32_t)i*3u;
        int nz=(int8_t)cpu->read_byte(p),nx=(int8_t)cpu->read_byte(p+1),ny=(int8_t)cpu->read_byte(p+2);
        uint32_t mine=shaded_lit_color(R->RT,nx,ny,nz,R->base,s_sh_nohl?0:1)&0xFFFFFFu;
        uint32_t s=cpu->read_word(SCR+(uint32_t)i*4u)&0xFFFFFFu;
        if(mine==s)exact++;
        err += abs((int)(mine&0xFF)-(int)(s&0xFF))+abs((int)((mine>>8)&0xFF)-(int)((s>>8)&0xFF))
             + abs((int)((mine>>16)&0xFF)-(int)((s>>16)&0xFF));
    }
    int nyonly=0, nx=0, nz=0;   /* euler census across all shaded mobys this frame */
    for (int r=0;r<s_nshrecs;r++){ uint32_t rr=cpu->read_word(s_shrecs[r].moby + game_layout.moby.angle_off + 0u);
        int X=(rr&0xFF00u)>>7, Z=(rr&0xFFu)<<1; if(X)nx++; if(Z)nz++; if(!X&&!Z)nyonly++; }
    fprintf(stderr,"[shaded-pcol vbl=%u] rec=%d cls=0x%X vcount=%d euler{Y=%u X=%u Z=%u} R->RT exact=%d/%d meanChErr=%.2f nohl=%d | census{Yonly=%d Xrot=%d Zrot=%d of %d}\n",
        openpete_frame, brec, R->cls, R->vcount,
        (rot>>15)&0x1FEu, (rot&0xFF00u)>>7, (rot&0xFFu)<<1, exact, nn, err/(3.0*nn), s_sh_nohl,
        nyonly, nx, nz, s_nshrecs);
}

/** @brief Arm the scene's actor-front pass while the game's cutscene renderer owns the draw.
 *
 * Cutscene actors draw in front of the terrain they interpenetrate, as the
 * PSX's coarse OT bucketing painted them. The gate is the game's own
 * (moby_cutscene_owns_draw): while its cutscene renderer owns the draw it
 * renders only those actors. A game with no such renderer answers 0 and the
 * pass never arms. The native renderer's front pass draws only the triangles
 * tagged OPENPETE_SCENE_ACTOR_FRONT and replaces the stored depth at their
 * own pixels, so every other moby, the distant ones extended LOD admits
 * included, keeps its terrain occlusion. OPENPETE_ACTOR_FRONT=0 disables it.
 * Called at both feed sites (canonical and sub-tick re-extract) after the
 * scene's per-present reset.
 * @param cpu The guest CPU state, read only. */
static void moby_native_set_actor_front(CPUState* cpu) {
    static int s_en = -1;
    if (s_en < 0) { const char* e = getenv("OPENPETE_ACTOR_FRONT"); s_en = (e && *e) ? (*e != '0') : 1; }
    openpete_native_scene_set_actor_front(s_en && moby_cutscene_owns_draw(cpu));
}

/** @brief Run the game's shaded moby draw from its own entry (game_layout.moby.hook_draw_shaded), not the hooked address.
 *
 * @param cpu The guest CPU state.
 * @param addr The hooked address, unused. */
static void moby_run_draw_shaded(CPUState* cpu, uint32_t addr) {
    (void)addr;
    moby_run_draw(cpu, game_layout.moby.hook_draw_shaded);
}

static hook_bracket_state s_shaded_hb;
static const hook_bracket_desc k_shaded_bracket = {
    .path = RP_MOBY, .prof_tag = "moby.srend",
    .run = moby_run_draw_shaded, .verify_enabled = moby_verify_enabled,
    .st = &s_shaded_hb,
};

/** @brief The shaded-pass hook: run the game's shaded moby draw, then feed its gems, props, HUD icons and text natively.
 *
 * Under hook_bracket_run. With the native scene live it builds the shaded
 * walk, marks the moby path drawn when the walk made triangles, pushes the
 * scene, snapshots the shaded list for the sub-tick spine when a capture is
 * open, and sets the HUD and actor-front flags. With verify on it compares
 * the walk against the pass's own prims for the first few reports.
 * @param addr The hooked address, passed to the bracket.
 * @return 1 when the hook ran the pass; 0 when the bracket declined. */
static int on_draw_shaded(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_shaded_bracket, &startp, &endp, &feed, &verify))
        return 0;
    moby_view_shd_build(cpu);

    if (feed) {                                 /* feed shaded mobys to the scene - fully native */
        s_cur_mat = "SHD";
        int shd_n = build_shaded_walk_tris(cpu); /* native topology (#2a) + native lighting (#2b) */
        /* Twin of the REG reveal mark: past the canonical band the .s shaded
         * walk emits nothing (its own 3-axis cull, which extended LOD drops on
         * the native mirror), so the interval above is empty and the gems/props
         * this feeder just built would live on canonical presents only. */
        if (shd_n > 0) render_path_mark_drawn(RP_MOBY);
        push_scene_moby();                      /* reads zero .s prim output */
        /* Sub-tick spine: snapshot the shaded Moby* list (live now) so
         * intermediates can re-derive the gems/props. */
        if (spyro_native_canon_capture_open()) {
            spyro_native_canon_capture_range(cpu, 0x2400u, 0x800u);
            spyro_native_canon_capture_camera(cpu, SPYRO_PROD_SHADED);
        }
        /* Flat HUD and text draw OVER the player by default; the game states
         * where they draw under it are moby_game's to name. Set after push
         * (begin_frame, which resets the flag, has run); the moby renderer
         * runs every 3D frame. */
        openpete_native_scene_set_hud_under_player(
            moby_game.hud_under_player ? moby_game.hud_under_player(cpu) : 0);
        moby_native_set_actor_front(cpu);
    }

    if (verify) {
        static int s_dumped = 0, s_sreports = 0;
        if (moby_verify_vbl() && endp > startp && s_sreports < 12) {
            s_cur_mat = "SHD";
            capture_gte(cpu, startp, endp);
            if (!s_dumped) { shaded_dump(cpu, startp, endp); s_dumped = 1; }
            shaded_verify(cpu);                 /* vertex A/B (100% geometry match) */
            build_shaded_walk_tris(cpu);        /* native topology (#2a) + lighting (#2b) -> s_wtri */
            compare_walk_vs_prims();            /* triangle A/B vs the shaded prims (~90% onscreen) */
            shaded_color_ab();                  /* position-keyed colour A/B (#2b lighting) */
            shaded_pcol_scratch_check(cpu);     /* direct pcol vs .s +0x2A00 scratchpad */
            s_sreports++;
        }
    }
    return hook_bracket_end(&k_shaded_bracket);
}

/* The second model kind's draw pass. The provider owns the guest dispatch: its
 * pass captures the cull's own record queue around the call, because that
 * queue lives in the tick's draw scratch that later passes reuse. This frame
 * supplies the pass view, the triangle buffer and the scene push, so the kind
 * lands beside the animated one. Returning 0 leaves the game's draw to the
 * ordinary dispatch. */
/* The provider owns the guest dispatch; its triangle count decides the
 * mark-drawn below. */
static int s_simple_ntri;
/* The canonical epoch the second-kind queue was captured in. The queue is a
 * host copy the pass overwrites only when the game's second-kind draw runs;
 * on a tick where that draw does not run (a menu or map state owns the frame)
 * it still holds the last gameplay tick's records, which project to nothing
 * the canonical present drew. The sub-tick replay decodes that copy, so it
 * runs only while this stamp is the committed epoch. */
static uint32_t s_canon_simple_epoch = 0;
static int      s_canon_simple_valid = 0;
int moby_native_simple_fresh(void) {
    if (!moby_game.simple_replay) return 0;
    if (s_rec_ref_prev) return 1;   /* held: the replay picks tick N-1's copy */
    return s_canon_simple_valid && spyro_native_canon_epoch_fresh(s_canon_simple_epoch);
}
/** @brief Run the provider's second-kind draw pass and stamp the canonical epoch its queue belongs to.
 *
 * The queue's validity flag and epoch stamp are set only on a
 * non-speculative run. The pass's triangle count goes to s_simple_ntri.
 * @param addr The hooked address, unused. */
static void moby_run_draw_simple(CPUState* cpu, uint32_t addr) {
    (void)addr;
    s_simple_ntri = moby_game.simple_pass(cpu);
    extern int openpete_speculative;
    if (!openpete_speculative) {
        s_canon_simple_valid = 1;
        s_canon_simple_epoch = spyro_native_canon_epoch_stamp();
    }
}
/** @brief Bracket pre-hook of the second-kind pass: name the material, build the pass view and empty the walk output.
 *
 * @param feed Unused.
 * @param verify Unused. */
static void moby_simple_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    s_cur_mat = "SIM";
    moby_view_build(cpu);
    s_gout.n = 0;
}

static hook_bracket_state s_simple_hb;
static const hook_bracket_desc k_simple_bracket = {
    .path = RP_MOBY, .prof_tag = "moby.srend",
    .run = moby_run_draw_simple, .verify_enabled = moby_verify_enabled,
    .pre = moby_simple_pre, .st = &s_simple_hb,
};

/** @brief The second-kind hook: run the game's second model-kind draw and feed the scene.
 *
 * Brackets moby_run_draw_simple with k_simple_bracket. On the feed path, a
 * pass that emitted triangles into an empty polybuf interval still marks
 * RP_MOBY drawn, then the triangles are pushed to the scene. On the verify
 * path, up to 120 reports capture the pass's prims and compare the walk
 * against them.
 * @param cpu The guest CPU state.
 * @param addr The hooked address.
 * @return 0 when the game has no second-kind pass or the bracket declines,
 *         leaving the ordinary dispatch to run it; 1 after the pass ran. */
static int on_draw_simple(CPUState* cpu, uint32_t addr) {
    if (!moby_game.simple_pass) return 0;
    uint32_t start, end; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_simple_bracket, &start, &end, &feed, &verify))
        return 0;
    const int ntri = s_simple_ntri;
    if (feed) {
        /* A tick whose second-kind pass emitted nothing to the polybuf leaves
         * an empty interval; the path must still read canon-drawn or the
         * sub-tick spine drops these triangles on every intermediate present. */
        if (ntri > 0 && end <= start) render_path_mark_drawn(RP_MOBY);
        push_scene_moby();
    }
    if (verify) {
        static int s_reports = 0;
        if (s_gout.n > 0 && s_reports < 120 && moby_verify_vbl()) {
            capture_gte(cpu, start, end);     /* the pass's own prims: the bracket */
            compare_walk_vs_prims();
            s_reports++;
        }
    }
    return hook_bracket_end(&k_simple_bracket);
}

/* What the sub-tick pass may do with a clip switch, shared by the pass and the
 * census so both read one rule. CROSS: the two clips deform one shared mesh
 * (equal vertex count), so vertex i is the same physical vertex in both and a
 * model-space lerp prev->curr stays on-model. A cutscene path-follower's
 * switch is CROSS too: the Spyro 1 sampler func_8002BFE0 steps m_Animation
 * every 60 waypoints and blends keyframe 59 of one clip into keyframe 0 of the
 * next, so the switch continues the motion; a placement it did not
 * interpolate is a jump (cut_obj_jump), whose interval shows tick N-1's
 * record whole and so carries no switch.
 * COUNTSW: the clips have different vertex counts, so no vertex correspondence
 * exists between them. */
typedef enum { MOBY_CLIP_CROSS = 0, MOBY_CLIP_COUNTSW } moby_clip_t;

/* moby_prev_keyframe's verdict; also the prevkf field of the [moby-anim]
 * trace, which the sub-tick harness bins on. */
enum { MOBY_PREV_SNAP = 0, MOBY_PREV_LERP = 1, MOBY_PREV_HOLD = 2 };

/** @brief Classify a clip switch as cross-fadable or a vertex-count change.
 *
 * Equal vertex counts (anim_vcount_hi of each header) are CROSS and unequal
 * ones COUNTSW.
 * @param cpu The guest CPU state, read only.
 * @param header_prev The previous pose's AnimationHeader.
 * @param header_curr The current pose's AnimationHeader.
 * @param count_p Receives the previous header's vertex count.
 * @param count_c Receives the current header's vertex count.
 * @return The kind. */
static moby_clip_t moby_clip_switch_kind(CPUState* cpu,
                                         uint32_t header_prev, uint32_t header_curr,
                                         int* count_p, int* count_c) {
    *count_c = cpu->read_byte(header_curr + game_layout.moby.anim_vcount_hi) & 0xFFu;
    *count_p = cpu->read_byte(header_prev + game_layout.moby.anim_vcount_hi) & 0xFFu;
    return (*count_p == *count_c) ? MOBY_CLIP_CROSS : MOBY_CLIP_COUNTSW;
}

/** @brief OPENPETE_MOBY_ANIMSW_CENSUS=1: log each queued record whose animation clip changed since the previous tick.
 *
 * Runs on the canonical pass, so one replay censuses every tick at replay
 * speed; the sub-tick pass evaluates the same rule (moby_clip_switch_kind)
 * once per present. A switch line carries the kind, the two clips' vertex
 * counts, span (the pair's interval in tick periods, above 1 after a static
 * stretch) and intact (fade_header_intact of the previous header against the
 * previous canonical table). A facing reversal of MOBY_MIRROR_HOLD_DEFAULT or
 * more, whatever OPENPETE_MOBY_MIRROR_HOLD is set to, logs a mirror line,
 * counted apart by whether the keyframe pair changed with the turn. Every 400
 * VBLs a stage line prints the engagement tally.
 * @param cpu The guest CPU state, read only.
 * @param moby The record's moby; an address below KSEG0 is ignored. */
static void moby_animsw_census(CPUState* cpu, uint32_t moby) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_ANIMSW_CENSUS");
                    s_on = (e && *e && *e != '0'); }
    if (!s_on || moby < 0x80000000u) return;
    /* Engagement tally: a census that reports no switch is only evidence once
     * the stages it passed through are visible. */
    static long n_rec = 0, n_obj = 0, n_prev = 0, n_cls = 0, n_pose = 0, n_sw = 0;
    /* Facing reversals, priced at the hold's default threshold whatever
     * OPENPETE_MOBY_MIRROR_HOLD is set to, split by whether the keyframe pair
     * changed with the turn: only the changed half has no pose between its two
     * snapshots, and the unchanged half is the population a threshold-only
     * rule would hold as well. */
    static long n_mirror_kf = 0, n_mirror_nokf = 0;
    static unsigned at = 0;
    n_rec++;
    if (openpete_frame >= at + 400u) {
        at = openpete_frame;
        fprintf(stderr, "[moby-animsw-census vbl=%u] stage recs=%ld obj=%ld prev=%ld "
                        "sameclass=%ld pose=%ld switches=%ld mirror=%ld mirror_nokf=%ld\n",
                openpete_frame, n_rec, n_obj, n_prev, n_cls, n_pose, n_sw,
                n_mirror_kf, n_mirror_nokf);
    }
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o) return;
    n_obj++;
    if (!o->have_prev) return;
    n_prev++;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + 0x36, 2); memcpy(&ccls, o->curr + 0x36, 2);
    if (pcls != ccls) return;
    n_cls++;
    uint32_t model = cpu->read_word(game_layout.moby.model_table + (uint32_t)ccls * 4u);
    if (model < 0x80000000u) return;
    moby_pose_t pp, pc;
    if (!moby_pose_derive(cpu, model, o->prev + game_layout.moby.anim_state_off, &pp) ||
        !moby_pose_derive(cpu, model, o->curr + game_layout.moby.anim_state_off, &pc)) return;
    n_pose++;
    {
        int step = 0, kf = 0;
        if (moby_turn_interval(cpu, moby, &step, &kf) && step >= MOBY_MIRROR_HOLD_DEFAULT) {
            if (kf) n_mirror_kf++; else n_mirror_nokf++;
            fprintf(stderr, "[moby-animsw-census vbl=%u] m=%08x cls=%03x mirror step=%d kf=%d "
                            "hdr=%08x->%08x fa=%08x->%08x\n",
                    openpete_frame, moby, (unsigned)ccls, step, kf,
                    pp.header, pc.header, pp.frame_a, pc.frame_a);
        }
    }
    if (pp.header == pc.header) return;
    n_sw++;
    int np = 0, nc = 0;
    moby_clip_t k = moby_clip_switch_kind(cpu, pp.header, pc.header, &np, &nc);
    static const char* const kind[] = { "cross", "countsw" };
    /* span: the pair's interval in tick periods (above 1 after a static
     * stretch); intact: the previous header against the signature the
     * previous canonical pass drew it with (1 same, 0 changed, -1 no entry). */
    int intact = fade_header_intact(cpu, s_fade_prev, s_fade_prev_n, moby, pp.header);
    fprintf(stderr, "[moby-animsw-census vbl=%u] m=%08x cls=%03x kind=%s anim=%u->%u "
                    "hdr=%08x->%08x n=%d->%d span=%.2f intact=%d\n",
            openpete_frame, moby, (unsigned)ccls, kind[k],
            (unsigned)o->prev[game_layout.moby.anim_state_off],
            (unsigned)o->curr[game_layout.moby.anim_state_off],
            pp.header, pc.header, np, nc,
            (double)(o->curr_ns - o->prev_ns) / (double)g_openpete_tick_period_ns, intact);
}

/** @brief The sub-tick verdict for a pair whose previous pose sits in another clip.
 *
 * The previous pose is read from its header at this present; a header the
 * game no longer draws may already hold another clip, and with its content
 * gone there is no previous pose to show, so unless fade_header_intact finds
 * it unchanged in this tick's table the record snaps to the current pose.
 * Otherwise moby_clip_switch_kind decides: a vertex-count change holds, a
 * shared mesh lerps. The refusal census
 * (s_prevhdr_sig, s_prevhdr_unk) counts presents below alpha 1 only.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param header_prev The previous pose's header.
 * @param header_curr The current pose's header.
 * @param t The pair's alpha at this present.
 * @return MOBY_PREV_SNAP, MOBY_PREV_HOLD or MOBY_PREV_LERP. */
static int moby_clip_switch_verdict(CPUState* cpu, uint32_t moby, uint32_t header_prev,
                                    uint32_t header_curr, double t) {
    int intact = fade_header_intact(cpu, s_fade_snap, s_fade_snap_n, moby, header_prev);
    s_prevhdr_sig += (intact == 0 && t < 1.0);
    s_prevhdr_unk += (intact < 0 && t < 1.0);
    if (intact != 1) return MOBY_PREV_SNAP;
    int count_p = 0, count_c = 0;
    switch (moby_clip_switch_kind(cpu, header_prev, header_curr, &count_p, &count_c)) {
    case MOBY_CLIP_COUNTSW: s_animsw_snap++;  return MOBY_PREV_HOLD;
    default:                s_animsw_cross++; return MOBY_PREV_LERP;
    }
}

/** @brief Schedule a hold verdict: below alpha 1 a hold lasts one tick period past the current stamp, then snaps to the current pose.
 *
 * Any other verdict, and any verdict at alpha 1, passes through. Held
 * presents count into s_hold_shown and expired ones into s_hold_period.
 * @param o The interp object.
 * @param verdict The verdict so far.
 * @param t The pair's alpha at this present.
 * @return The scheduled verdict. */
static int moby_hold_schedule(const openpete_interp_obj_t* o, int verdict, double t) {
    if (verdict != MOBY_PREV_HOLD || t >= 1.0) return verdict;
    if (moby_hold_expired(o)) { s_hold_period++; return MOBY_PREV_SNAP; }
    s_hold_shown++;
    return MOBY_PREV_HOLD;
}

/** @brief The previous rendered keyframe pose of a moby, and how the sub-tick shape lerp may use it.
 *
 * The pose is the one the cull drew from the previous snapshot's animation
 * state bytes (moby_pose_derive), so a tick that blends into another
 * animation gives that blend. The alpha comes from the same per-object clock
 * (s_rot_now) as the rotation interp, so shape, rotation and position share
 * one timeline.
 *
 * MOBY_PREV_SNAP: show the current pose from alpha 0. It is returned with
 * *prev and *alpha not written for interp off, a game that cannot name the
 * previous keyframe, no distinct prev snapshot, a slot that changed class, or
 * a model or pose that does not derive; with both written for a clip switch
 * whose previous header no longer holds the content a canonical pass drew it
 * with (fade_header_intact), or a hold past its tick period
 * (moby_hold_expired). MOBY_PREV_LERP: a lerp between the two poses,
 * including a clip switch between clips of equal vertex count, which deform
 * one shared mesh. MOBY_PREV_HOLD: show the previous
 * pose whole until alpha 1, for a clip switch between clips of different
 * vertex count and for a facing reversal (moby_mirror_hold). *prev carries
 * the vertices a canonical pass held for it (pose_hold_find); across a clip
 * switch a pose without them snaps, since the clip the moby left may hold
 * other data.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param header_curr The current pose's AnimationHeader.
 * @param prev Receives the previous pose.
 * @param alpha Receives the pair's phase.
 * @return MOBY_PREV_SNAP, MOBY_PREV_LERP or MOBY_PREV_HOLD. */
static int moby_prev_keyframe(CPUState* cpu, uint32_t moby, uint32_t header_curr,
                              moby_pose_t* prev, double* alpha) {
    if (!s_rot_now) return 0;                                  /* interp off -> snap */
    /* The prev pose is the decoder's second stream input, so it has to be
     * expressible in the form this game's record uses for a keyframe: a
     * pointer into the model's frame array where the game's provider rebuilds
     * records from gamestate, or the keyframe's leading stream word where the
     * cull copies that word into the record (rec_frame_is_word). A game with
     * neither form has no way to name the prev keyframe, so the sub-tick pass
     * replays the record the cull wrote and holds the pose. */
    if (!moby_game.host_reveal && !game_layout.moby.rec_frame_is_word) return 0;
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    /* Occupant guard: prev snapshot must be the SAME class as the live moby -
     * a reused level slot would crossfade another object's anim/frame state
     * (the topology count check below only catches different mesh sizes). */
    {
        uint16_t pcls4, ccls4;
        memcpy(&pcls4, o->prev + 0x36, 2); memcpy(&ccls4, o->curr + 0x36, 2);
        if (pcls4 != ccls4) return 0;
    }
    uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model < 0x80000000u) return 0;
    if (!moby_pose_derive(cpu, model, o->prev + game_layout.moby.anim_state_off, prev)) return 0;
    double t = moby_pair_alpha(o);
    *alpha = t;
    pose_hold_find(moby, prev);
    /* A jump has no pose between its two ticks either: the whole record,
     * shape included, shows tick N-1 until alpha 1. */
    if (moby_jump_held(moby, o)) return MOBY_PREV_HOLD;
    int verdict = (prev->header != header_curr)
                ? moby_clip_switch_verdict(cpu, moby, prev->header, header_curr, t)
                : MOBY_PREV_LERP;
    /* The clip the moby left may hold other data by now: across a switch the
     * previous pose is shown only from the vertices a canonical pass held. */
    if (verdict != MOBY_PREV_SNAP && prev->header != header_curr && pose_hold_on() && !prev->held) {
        s_animsw_noheld++;
        verdict = MOBY_PREV_SNAP;
    }
    s_animsw_heldx += (verdict != MOBY_PREV_SNAP && prev->header != header_curr && prev->held);
    /* A facing reversal has no pose between its two keyframe pairs either, so
     * it takes the same hold, clip switch or not. */
    if (verdict == MOBY_PREV_LERP && moby_mirror_hold(cpu, moby)) {
        s_animsw_mirror++;
        verdict = MOBY_PREV_HOLD;
    }
    verdict = moby_hold_schedule(o, verdict, t);
    if (verdict != MOBY_PREV_SNAP && prev->header != header_curr &&
        cpu->read_byte(prev->header + game_layout.moby.anim_scale_off) !=
        cpu->read_byte(header_curr + game_layout.moby.anim_scale_off)) {
        if (verdict == MOBY_PREV_LERP) s_animsw_scale_lerp++; else s_animsw_scale_hold++;
    }
    return verdict;
}

/** @brief The GPF scale byte a sub-tick present decodes a record at: the byte lerped from the previous tick's to this tick's.
 *
 * The byte is the cull's copy of m_ScaleOverride (moby+0x57, folded into w0
 * @8001F4A8/F4C4), animated per tick by effect classes (the Gnasty-blast
 * dome). It lerps on the pose crossfade's clock (moby_pair_alpha), so scale
 * stays in phase with the shape, and reads tick N-1's byte while the record
 * holds a jump (moby_jump_held). A previous byte of 0 takes the .s's non-GPF
 * (true-distance) branch, a discontinuity, so it does not lerp. With
 * OPENPETE_MOBY_GPF_DBG=1 one line per call prints the ramp, which must be
 * monotone across the presents of one tick.
 * @param moby The moby's PSX address.
 * @param gnow This tick's byte, from the live record's w0.
 * @param frac The value to keep when no lerp applies.
 * @return The lerped byte, or frac outside a sub-tick present, without a pair,
 *         or after a previous byte of 0. */
static double moby_gpf_subtick(uint32_t moby, int gnow, double frac) {
    if (!s_rot_now) return frac;
    const openpete_interp_obj_t* og = moby_interp_find(moby);
    if (!og || !og->have_prev || !og->prev_ns || og->curr_ns <= og->prev_ns) return frac;
    const int gprev = (int)og->prev[game_layout.moby.size_off];
    if (gprev > 0) {
        const double t = moby_jump_held(moby, og) ? 0.0 : moby_pair_alpha(og);
        frac = (double)gprev + ((double)gnow - (double)gprev) * t;
    }
    static int s_gd = -1;
    if (s_gd < 0) { const char* e = getenv("OPENPETE_MOBY_GPF_DBG"); s_gd = (e && *e && *e != '0'); }
    if (s_gd) fprintf(stderr, "[gpf] vbl=%u m=%08x prev=%d now=%d frac=%.3f\n",
                      openpete_frame, moby, gprev, gnow, frac);
    return frac;
}

/* ---- moby occlusion cull (native-side, display-only) ------------------------
 *
 * Skips the sub-tick re-extraction (decode + project + walk + draw) of a moby
 * whose whole screen rect is provably behind nearer WORLD-opaque depth - the
 * depth test would erase every pixel of it anyway (the game's canonical cull
 * is a view-cone + distance test with no occlusion knowledge, so enclosed
 * areas queue every moby behind the walls). Reads the occlusion grid via
 * openpete_native_occl_query; the CANONICAL cull pass and its gameplay-
 * readable writes are untouched (determinism-safe by construction).
 *
 * Per-moby cache: the last extraction's projected bounds - PSX-screen rect +
 * nearest FULL-WORLD view-Z, straight off s_proj (so no per-class scale
 * reverse-engineering). A hit older than 2 ticks is ignored (the moby
 * re-extracts, refreshing the cache - so a permanently-hidden moby still
 * pays full extraction every ≤2 ticks, ~15 Hz instead of every present).
 * Staleness within those 2 ticks is absorbed conservatively: the rect
 * inflates with the camera's measured pan speed (set_occl_pan) + a generous
 * own-motion allowance, and the view-Z gets a per-tick translation slack on
 * top of the grid's own OPENPETE_MOBY_OCCL_SLACK. HUD/UI-routed mobys are
 * exempt (drawn depth-off - never depth-occluded). Direct-mapped cache: a
 * collision only evicts (-> that moby extracts normally), never lies. */
typedef struct { uint32_t moby; float x0, y0, x1, y1, vz_min; unsigned tick;
                 float mot, mot_vz;      /* measured screen / view-Z motion per VBL (px, world units) */
                 /* Anchor for the extraction-free bounds refresh below: the
                  * record's origin projection, view-space TR.z, model radius
                  * and class at the last REAL extraction. Written only there,
                  * so a refresh re-derives from a fixed reference and cannot
                  * drift however long a moby stays hidden. */
                 float asx, asy, atrz, arad; uint16_t acls;
                 /* Verdict held for one canonical epoch: the moby it was
                  * decided for (the slot is direct-mapped, so it need not be
                  * the moby whose bounds the entry carries), the epoch stamp
                  * it was decided in, and the verdict itself. */
                 uint32_t vmoby; uint32_t vepoch; uint8_t vculled; } occl_rect_t;
#define OCCL_HASH_N 1024                       /* power of two; ~500 live mobys max */
static occl_rect_t s_occl_cache[OCCL_HASH_N];
static float s_occl_pan_px = 0.0f;             /* px/tick pan allowance (orchestrator) */
/* s_occl_culled / s_occl_seen: census (records, re-extract + reveal passes) -
 * declared above append_revealed_regular, which is also a cull site. */

void moby_native_set_occl_pan(float px_per_tick) { s_occl_pan_px = px_per_tick; }

/** @brief The occlusion cache slot of a moby: a multiplicative hash of the address, direct mapped.
 *
 * @param moby The Moby address.
 * @return An index below OCCL_HASH_N. */
static unsigned occl_slot(uint32_t moby) {
    return ((moby >> 3) * 2654435761u >> 16) & (OCCL_HASH_N - 1);
}

static long s_occl_f[4];   /* feeder-side pre-query outcomes: ui-exempt / no-cache / stale-cache / refreshed */

/* Per-leg cull census (OPENPETE_REEXTRACT_FEEDDBG), index 1 = the canonical
 * re-extract leg, 0 = an intermediate one. Both legs of one canonical epoch
 * see the same gameplay state, so their culled counts must agree; a
 * difference means the verdict was decided per present. */
static long s_occl_leg_culled[2], s_occl_leg_seen[2];
extern int g_openpete_canon_lag_leg;

static int hold_project_origin(CPUState* cpu, const double tr[3], double* sx, double* sy);

/* ---- extraction-free bounds refresh ---------------------------------------
 * A culled record never reaches occl_cache_update, so its bounds age out and
 * the age test forces a full extraction - decode, project and walk - purely to
 * re-derive a screen rect whose only consumer is the cull that then skips it
 * again. That cycle capped the cull's value at one skipped present in three.
 *
 * The rect can be rebuilt without any of it: project the record ORIGIN through
 * the current camera (the TR-only slice - no euler compose, no decode) and
 * carry the anchor's rect around it, rescaled by the view-Z ratio, unioned with
 * the model's own radius box. The radius bounds every pose and rotation of the
 * model, so the union contains the true projected extent whatever the moby has
 * animated into since. Both bounds are taken conservatively - an over-large
 * rect or an over-near view-Z can only fail to cull, never cull something
 * visible.
 *
 * Nothing is written: repeated refreshes re-derive from the same anchor, so
 * they cannot drift, and the measured-motion fields (which compare successive
 * extractions) never see a rect built by this second method. Two guards stand
 * in for the staleness bound that is being removed - m_Class is re-checked
 * because moby slots are recycled and a reused address would otherwise inherit
 * the previous occupant's radius, and the anchor expires outright after
 * OCCL_ANCHOR_MAX_VBL so no rect survives a scene indefinitely. */
#define OCCL_ANCHOR_MAX_VBL 64u

/** @brief Rebuild a cached record's screen rect and nearest view-Z from the current camera, without decoding.
 *
 * Refuses (0) an entry with no usable anchor (radius, TR.z or view-Z not
 * positive), an anchor older than OCCL_ANCHOR_MAX_VBL VBLs, a moby whose
 * class changed since the anchor, and an origin nearer than
 * hold_project_origin accepts. Writes nothing to the cache or guest memory.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param e The cache entry holding the anchor.
 * @param x0 Receives the rect's left edge, PSX screen px.
 * @param y0 Receives the top edge.
 * @param x1 Receives the right edge.
 * @param y1 Receives the bottom edge.
 * @param vz Receives the nearest view-Z, biased toward the camera.
 * @return 1 with the outputs written, else 0. */
static int occl_refresh_bounds(CPUState* cpu, uint32_t moby, const occl_rect_t* e,
                               float* x0, float* y0, float* x1, float* y1, float* vz) {
    if (e->arad <= 0.0f || e->atrz <= 1.0f || e->vz_min <= 0.0f) return 0;
    if ((unsigned)(openpete_frame - e->tick) > OCCL_ANCHOR_MAX_VBL) return 0;
    if ((uint16_t)cpu->read_half(moby + game_layout.moby.class_off) != e->acls) return 0;   /* slot recycled */
    double tr[3], sx, sy;
    build_moby_record_tr(cpu, moby, tr);
    if (!hold_project_origin(cpu, tr, &sx, &sy)) return 0;   /* too near to project: extract */
    double H     = s_view.H_eff;
    double scale = (double)e->atrz / tr[2];                  /* screen px per anchor px */
    double hr    = (double)e->arad * H / tr[2];              /* radius box half-extent */
    double ax0 = sx + ((double)e->x0 - e->asx) * scale, ax1 = sx + ((double)e->x1 - e->asx) * scale;
    double ay0 = sy + ((double)e->y0 - e->asy) * scale, ay1 = sy + ((double)e->y1 - e->asy) * scale;
    *x0 = (float)(ax0 < sx - hr ? ax0 : sx - hr);
    *x1 = (float)(ax1 > sx + hr ? ax1 : sx + hr);
    *y0 = (float)(ay0 < sy - hr ? ay0 : sy - hr);
    *y1 = (float)(ay1 > sy + hr ? ay1 : sy + hr);
    /* Nearest view-Z. k is world view-Z per TR unit taken from the anchor's
     * NEAREST vertex, so it under-estimates the origin's own ratio - nearer,
     * conservative - and a full radius comes off again to bound any rotation.
     * The anchor rect rescaled is kept as the alternative and the nearer of the
     * two wins. */
    double k   = (double)e->vz_min / (double)e->atrz;
    double vzr = k * (tr[2] - (double)e->arad);
    double vza = (double)e->vz_min / scale;
    *vz = (float)(vzr < vza ? vzr : vza);
    return 1;
}

/** @brief The occlusion cull verdict for one record, before any decode work.
 *
 * A HUD or UI moby is never occluded, and a moby with no cached bounds is
 * extracted, which refreshes them. Bounds older than two VBLs are rebuilt
 * around the moby's current origin (occl_refresh_bounds), which owes no
 * own-motion allowance; without a usable anchor the moby is extracted. The
 * query rect inflates by 8 px plus the bounds' age times the camera pan and
 * the moby's own-motion allowance, and the depth by a margin scaled the same
 * way.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @return 1 when the moby is hidden behind nearer world depth, else 0. */
static int occl_record_decide(CPUState* cpu, uint32_t moby) {
    if (moby_is_hud(moby)) { s_occl_f[0]++; return 0; }   /* HUD/UI: drawn over the scene, never occluded */
    const occl_rect_t* e = &s_occl_cache[occl_slot(moby)];
    if (e->moby != moby) { s_occl_f[1]++; return 0; }   /* no cached bounds -> extract (refreshes) */
    unsigned age = openpete_frame - e->tick;
    float rx0 = e->x0, ry0 = e->y0, rx1 = e->x1, ry1 = e->y1, rvz = e->vz_min;
    float own;
    if (age > 2u) {
        if (!occl_refresh_bounds(cpu, moby, e, &rx0, &ry0, &rx1, &ry1, &rvz))
            { s_occl_f[2]++; return 0; }            /* no usable anchor -> extract (refreshes) */
        s_occl_f[3]++;
        /* The refreshed rect is built around the moby's CURRENT origin and hard-
         * bounded by its model radius, so no own-motion allowance is owed - only
         * the camera's pan across this present. */
        age = 0u;
        own = 0.0f;
    } else {
        /* Own-motion allowance ≈ 256 world units/tick at an overestimated H≈400:
         * px = 400·256/vz, scaled by the view's H scale (the pan allowance is
         * measured at the canonical H and scaled the same way). Camera pan
         * rides s_occl_pan_px (measured per tick).
         * Own-motion allowance: the 256-u/tick model floor OR the moby's MEASURED
         * screen motion ×1.5, whichever is larger - fast movers (birds ~750 u/tick)
         * beat the model by 3× and could stay culled ≤3 presents while emerging
         * from cover. */
        float vzm = e->vz_min < 512.0f ? 512.0f : e->vz_min;
        own = 102400.0f * (float)s_view.s / vzm;
        if (e->mot * 1.5f > own) own = e->mot * 1.5f;
    }
    float aged = (float)age + 1.0f;
    float inf = 8.0f + aged * (s_occl_pan_px * (float)s_view.s + own);
    float vzs = 600.0f;
    if (e->mot_vz * 1.5f > vzs) vzs = e->mot_vz * 1.5f;
    openpete_occl_camera qcam; ngte_occl_camera(&s_view, &qcam);
    int culled = openpete_native_occl_query(rx0 - inf, ry0 - inf,
                                            rx1 + inf, ry1 + inf,
                                            rvz - vzs * aged, &qcam);
    return culled;
}

/** @brief The occlusion cull verdict for one record, decided once per canonical epoch and held for every later present of it.
 *
 * Both the rect inflation and the depth margin scale with the cached bounds'
 * age, and a record the first present of the epoch extracts is written back
 * at age 0 by occl_cache_update: without the hold the next present of the
 * same tick queries tighter bounds and culls what the previous present drew,
 * which alternates the record in and out at the present rate. The canonical
 * leg always re-decides; it opens each epoch's span, so the held verdict is
 * the one it reached, and a run with a single present per tick decides every
 * record fresh. Counts each verdict into the per-leg census.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @return 1 when culled, else 0. */
static int occl_record_culled(CPUState* cpu, uint32_t moby) {
    occl_rect_t* e = &s_occl_cache[occl_slot(moby)];
    unsigned epoch = spyro_native_canon_epoch_stamp();
    int leg = g_openpete_canon_lag_leg ? 1 : 0;
    int culled;
    if (!leg && e->vmoby == moby && e->vepoch == epoch) {
        culled = (int)e->vculled;
    } else {
        culled = occl_record_decide(cpu, moby);
        e->vmoby = moby; e->vepoch = epoch; e->vculled = (uint8_t)(culled != 0);
    }
    s_occl_leg_seen[leg]++;
    if (culled) s_occl_leg_culled[leg]++;
    return culled;
}

/** @brief Print one line per re-extract leg with that leg's occlusion verdict counts since its previous line (OPENPETE_REEXTRACT_FEEDDBG). */
static void occl_leg_census_report(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_REEXTRACT_FEEDDBG"); on = (e && *e && *e != '0'); }
    if (!on) return;
    int leg = g_openpete_canon_lag_leg ? 1 : 0;
    fprintf(stderr, "[moby-occl-leg vbl=%u epoch=%u leg=%s] culled=%ld seen=%ld\n",
            openpete_frame, spyro_native_canon_epoch_stamp(), leg ? "CANON" : "inter",
            s_occl_leg_culled[leg], s_occl_leg_seen[leg]);
    s_occl_leg_culled[leg] = 0; s_occl_leg_seen[leg] = 0;
}

/** @brief Whether the occlusion-verify audit mode (OPENPETE_MOBY_OCCL_VERIFY=1) is on, read once.
 *
 * When the cull says hidden, the pass extracts the moby anyway and re-runs
 * the query on the bounds that extraction just produced. A fresh verdict of
 * visible means the cached bounds would have culled a moby the depth test
 * would not fully erase: a would-be single-present flicker. The census prints
 * once per second inside the OPENPETE_MOBY_OCCL_DBG summary, so it appears only
 * while that variable is also set. The moby still
 * draws in verify mode (the verdict is overridden), so a live run with verify
 * on is also the zero-flicker reference.
 *
 * A would-be-culled record does not write its cache entry here, as it would
 * not in a production run. Refreshing it would keep every entry young, the
 * extraction-free refresh path would never engage, and the audit would report
 * on a state the cull never reaches, so the fresh bounds are taken straight
 * off the projection instead of through the cache.
 * @return Nonzero when the mode is on. */
static int occl_verify_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_OCCL_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static long s_ver_would = 0;   /* stale-cache verdicts said CULL */
static long s_ver_wrong = 0;   /* ...of which fresh bounds said VISIBLE */

static void occl_proj_bounds(const proj_xy* p, int n, float* px0, float* py0,
                             float* px1, float* py1, float* pvz);

/** @brief Query one explicit set of bounds with the age-0 inflation of occl_record_decide.
 *
 * @param x0 The rect's left edge, PSX screen px.
 * @param y0 The rect's top edge.
 * @param x1 The rect's right edge.
 * @param y1 The rect's bottom edge.
 * @param vzmin The nearest view-Z.
 * @param mot The moby's measured screen motion per tick.
 * @param mot_vz The moby's measured view-Z motion per tick.
 * @return Nonzero when the query culls. */
static int occl_query_at(float x0, float y0, float x1, float y1, float vzmin,
                         float mot, float mot_vz) {
    float vzm = vzmin < 512.0f ? 512.0f : vzmin;
    float own = 102400.0f * (float)s_view.s / vzm;
    if (mot * 1.5f > own) own = mot * 1.5f;
    float inf = 8.0f + (s_occl_pan_px * (float)s_view.s + own);
    float vzs = 600.0f;
    if (mot_vz * 1.5f > vzs) vzs = mot_vz * 1.5f;
    openpete_occl_camera qcam; ngte_occl_camera(&s_view, &qcam);
    return openpete_native_occl_query(x0 - inf, y0 - inf, x1 + inf, y1 + inf,
                                      vzmin - vzs, &qcam);
}
static occl_rect_t s_ver_snap;   /* the entry the cull judged, for the report */
/** @brief Audit-mode entry for a record the cull judged hidden: count it and keep the cache entry that judged it.
 *
 * @param moby The Moby address.
 * @return 1 when the audit is on and the caller must extract the moby anyway,
 *         else 0. */
static int occl_verify_begin(uint32_t moby) {
    if (!occl_verify_on()) return 0;
    s_ver_would++;
    s_ver_snap = s_occl_cache[occl_slot(moby)];
    return 1;
}
/** @brief Audit-mode exit: re-query the fresh bounds of an extracted record the cull judged hidden and report a visible verdict.
 *
 * A fresh rect outside the visible extent is reported as zeropx, any other
 * as a wrong verdict and counted in s_ver_wrong. Nothing is reported when
 * the fresh query culls too.
 * @param moby The Moby address.
 * @param p The record's projected verts.
 * @param n Their count; 0 or less returns without a report. */
static void occl_verify_end(uint32_t moby, const proj_xy* p, int n) {
    if (n <= 0) return;                 /* no verts projected -> nothing drawable */
    float x0, y0, x1, y1, vz;
    occl_proj_bounds(p, n, &x0, &y0, &x1, &y1, &vz);
    if (occl_query_at(x0, y0, x1, y1, vz, s_ver_snap.mot, s_ver_snap.mot_vz)) return;
    /* Classify: a fresh rect fully outside the visible extent produces ZERO
     * pixels regardless of the verdict - the conservative inflation merely
     * crossed the boundary. Only on-screen fresh rects are real events. */
    float xlo = s_view.xlo, xhi = s_view.xhi;
    int zeropx = (x1 < xlo || x0 > xhi || y1 < 0.0f || y0 > 224.0f);
    if (!zeropx) s_ver_wrong++;
    fprintf(stderr, "[occl-VERIFY-%s vbl=%u] moby=%08x cached rect=(%.0f,%.0f)-(%.0f,%.0f) vz=%.0f age=%u "
                    "fresh rect=(%.0f,%.0f)-(%.0f,%.0f) vz=%.0f mot=%.1f scan_max=%.0f\n",
            zeropx ? "zeropx" : "WRONG", openpete_frame, moby,
            s_ver_snap.x0, s_ver_snap.y0, s_ver_snap.x1, s_ver_snap.y1, s_ver_snap.vz_min,
            openpete_frame - s_ver_snap.tick,
            x0, y0, x1, y1, vz, s_ver_snap.mot,
            openpete_native_occl_last_scan_max());
}

/** @brief The screen rect and nearest view-Z of a record's projected verts, in the units the occlusion query takes.
 *
 * s_proj carries PSX screen px and the fed view-Z. The bounds cover all n
 * verts, a superset of the drawn subset, so the rect is conservative.
 * @param p The projected verts.
 * @param n Their count, at least 1.
 * @param px0 Receives the left edge.
 * @param py0 Receives the top edge.
 * @param px1 Receives the right edge.
 * @param py1 Receives the bottom edge.
 * @param pvz Receives the nearest view-Z. */
static void occl_proj_bounds(const proj_xy* p, int n, float* px0, float* py0,
                             float* px1, float* py1, float* pvz) {
    float x0 = p[0].sx, x1 = p[0].sx, y0 = p[0].sy, y1 = p[0].sy, vzm = p[0].vz;
    for (int j = 1; j < n; j++) {
        if (p[j].sx < x0) x0 = p[j].sx; else if (p[j].sx > x1) x1 = p[j].sx;
        if (p[j].sy < y0) y0 = p[j].sy; else if (p[j].sy > y1) y1 = p[j].sy;
        if (p[j].vz < vzm) vzm = p[j].vz;
    }
    *px0 = x0; *py0 = y0; *px1 = x1; *py1 = y1; *pvz = vzm;
}

/** @brief Store a record's projected bounds in its cache slot, with the measured motion and the refresh anchor.
 *
 * Motion is the rect-centre step and the view-Z step per VBL against the same
 * moby's previous entry, 0 on a first sighting or slot collision. The anchor
 * (origin projection, TR.z, radius, class) is rewritten here only; it is
 * left unusable (radius 0) when the header is below KSEG0, tr is NULL, or
 * TR.z is too near to project.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param p The record's projected verts.
 * @param n Their count; 0 or less changes nothing.
 * @param tr The record's view-space translation, or NULL.
 * @param header The AnimationHeader whose radius byte bounds the model. */
static void occl_cache_update(CPUState* cpu, uint32_t moby, const proj_xy* p, int n,
                              const double tr[3], uint32_t header) {
    if (n <= 0) return;
    float x0, y0, x1, y1, vzm;
    occl_proj_bounds(p, n, &x0, &y0, &x1, &y1, &vzm);
    occl_rect_t* e = &s_occl_cache[occl_slot(moby)];
    /* Measured motion (px + view-Z per VBL) from the previous bounds of the
     * SAME moby: rect-centre delta over the refresh interval. Slot collision
     * or first sighting -> 0 (the model floor covers it). */
    if (e->moby == moby && openpete_frame != e->tick) {
        float dt = (float)(openpete_frame - e->tick);
        float dx = 0.5f * ((x0 + x1) - (e->x0 + e->x1));
        float dy = 0.5f * ((y0 + y1) - (e->y0 + e->y1));
        float d  = sqrtf(dx * dx + dy * dy) / dt;
        float dv = (vzm > e->vz_min ? vzm - e->vz_min : e->vz_min - vzm) / dt;
        e->mot    = d;
        e->mot_vz = dv;
    } else if (e->moby != moby) {
        e->mot = 0.0f; e->mot_vz = 0.0f;
    }
    e->moby = moby; e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
    e->vz_min = vzm; e->tick = openpete_frame;
    /* Anchor for the extraction-free refresh: this pose's origin projection and
     * TR.z, plus the model radius (AnimationHeader byte[7] << 4, the same
     * bounding radius the .s cone cull biases by @8001F2A0) and the class the
     * radius belongs to. A record whose origin will not project (too near) still
     * refreshes bounds by extracting, so the anchor is simply left unusable. */
    e->arad = 0.0f;
    if (header >= 0x80000000u && tr && tr[2] > 1.0) {
        double asx, asy;
        if (hold_project_origin(cpu, tr, &asx, &asy)) {
            e->asx  = (float)asx; e->asy = (float)asy;
            e->atrz = (float)tr[2];
            e->arad = (float)((int)cpu->read_byte(header + game_layout.moby.anim_radius) << 4);
            e->acls = (uint16_t)cpu->read_half(moby + game_layout.moby.class_off);
        }
    }
}

/* ---- Tier-2 small-moby extraction hold ------------------------------------
 * A moby only a few px on screen moves sub-pixel between the presents of one
 * sim tick: its per-present decode+project+walk buys nothing visible. On the
 * FIRST present of each canonical epoch it extracts fully and its built tris
 * are captured; every later present of that epoch REPLAYS the capture,
 * translated by the delta of the moby ORIGIN's projection (one TR projection
 * through the current interpolated camera vs the captured one) with view-Z
 * rescaled by the TR.z ratio. Translation error across the moby is bounded by
 * its screen size (< the hold threshold) - imperceptible at speck scale, and
 * the pose lerp it skips is equally sub-pixel there. Gates: screen extent
 * from the occl cache (< OPENPETE_MOBY_HOLD_PX, default 10, 0 = off), not
 * HUD/UI (special projection), origin safely in front of the camera, same
 * canonical epoch (a new tick always re-extracts - gameplay state only
 * changes at ticks, so no discontinuity can hide inside an epoch).
 * Display-only. */
#define HOLD_TRI_MAX  32768
#define HOLD_MOBY_MAX 512
static ntri_t   s_hold_tri[HOLD_TRI_MAX];
static uint32_t s_hold_fkey[HOLD_TRI_MAX];
static uint8_t  s_hold_mode[HOLD_TRI_MAX];
/* Per-face verdicts of the capturing walk. They are properties of the face,
 * not of the present, so a replay restores them rather than recomputing or
 * clearing them: mangle gates the og OT key and the rank snapshot, dofs is the
 * authored OT bias inside that key, special names the material that produced
 * the face's colours. */
static uint8_t  s_hold_mangle[HOLD_TRI_MAX];
static int8_t   s_hold_dofs[HOLD_TRI_MAX];
static uint8_t  s_hold_special[HOLD_TRI_MAX];
typedef struct { uint32_t moby; unsigned epoch; int off, n, spec;
                 double sx, sy, trz;              /* captured origin projection + view-Z */
                 double H;                        /* projection distance the tris were built with */
                 uint16_t cls;                    /* m_Class at capture (provenance tag) */
                 uint8_t front; } hold_t;         /* per-moby emit flags at capture */
static hold_t   s_hold[HOLD_MOBY_MAX];
static int      s_hold_n, s_hold_tri_n;
static unsigned s_hold_epoch;

/** @brief The Tier-2 hold's screen-extent bound in px, 0 when the hold is off, latched on first use.
 *
 * OPENPETE_MOBY_HOLD_PX sets it, default 10; a negative value reads as 0.
 * @return The bound. */
static int hold_px_max(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_HOLD_PX"); v = e ? atoi(e) : 10; if (v < 0) v = 0; }
    return v;
}
/** @brief The hold entry captured for a moby and pass this epoch, or NULL.
 *
 * The table is emptied when the canonical epoch changes, so every entry belongs
 * to the current epoch.
 * @param moby The moby's PSX address.
 * @param spec 1 for the specular pass's capture, 0 for the regular pass's.
 * @return The entry, or NULL. */
static hold_t* hold_find(uint32_t moby, int spec) {
    for (int i = 0; i < s_hold_n; i++)
        if (s_hold[i].moby == moby && s_hold[i].spec == spec) return &s_hold[i];
    return NULL;
}
/** @brief Project a record's origin (v = 0) to the screen: sx = OFX + TRx·H/TRz.
 *
 * H is the view's effective projection distance (s_view.H_eff). The m_Scale
 * shift cancels in the x/z ratio, so the raw translation is exact here.
 * @param cpu Unused.
 * @param tr The record's view-space translation.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y.
 * @return 1, or 0 with *sx and *sy unwritten when TRz is below 64 (too near
 *         or behind). */
static int hold_project_origin(CPUState* cpu, const double tr[3], double* sx, double* sy) {
    if (tr[2] < 64.0) return 0;                    /* too near / behind: no hold */
    (void)cpu;
    double ofx = s_view.ofx;
    double ofy = s_view.ofy;
    double H   = s_view.H_eff;
    *sx = ofx + tr[0] * H / tr[2];
    *sy = ofy + tr[1] * H / tr[2];
    return 1;
}

/** @brief The object's prev/curr snapshot pair, with the present's phase past the curr stamp.
 *
 * The phase is the present clock past the curr stamp over the roll interval,
 * capped at one tick so a pair whose two distinct states are further apart
 * sweeps the whole delta over the tick and still lands exactly on curr at
 * t = 1, and clamped to [0, 1]. On a held camera cut it is 1 for an object
 * that did not roll at the cut's tick (cut_held_rolled), and 0 for a record
 * moby_jump_held holds.
 * @param moby The Moby address.
 * @param t Receives the phase when a pair is returned.
 * @return The interp object, or NULL when no term built on the pair may be
 *         applied: no pair, a slot that changed class between the two states,
 *         or a stale pair, where a multi-tick gap is a discontinuity rather
 *         than motion and the record's own pose is what to keep. */
static const openpete_interp_obj_t* moby_record_pair(uint32_t moby, double* t) {
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return NULL;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + game_layout.moby.class_off, 2);
    memcpy(&ccls, o->curr + game_layout.moby.class_off, 2);
    if (pcls != ccls) return NULL;
    if (moby_interp_pair_stale(o->prev_ns, o->curr_ns)) return NULL;
    double period = (double)(o->curr_ns - o->prev_ns);
    if (period > (double)MOBY_TICK_PERIOD_NS) period = (double)MOBY_TICK_PERIOD_NS;
    double ph = (double)(openpete_interp_present_now() - o->curr_ns) / period;
    *t = ph < 0.0 ? 0.0 : (ph > 1.0 ? 1.0 : ph);
    if (cut_held_rolled(o) == 0) *t = 1.0;
    if (moby_jump_held(moby, o)) *t = 0.0;
    return o;
}

int moby_record_world_offset(uint32_t moby, double out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    double t = 0.0;
    const openpete_interp_obj_t* o = moby_record_pair(moby, &t);
    if (!o) return 0;
    int32_t pv[3], cv[3];
    memcpy(pv, o->prev + game_layout.moby.pos_off, 12);
    memcpy(cv, o->curr + game_layout.moby.pos_off, 12);
    for (int i = 0; i < 3; i++)
        out[i] = ((double)pv[i] - (double)cv[i]) * (1.0 - t);
    return 1;
}

int moby_record_halfword_offset(uint32_t moby, uint32_t off, double* out) {
    *out = 0.0;
    double t = 0.0;
    const openpete_interp_obj_t* o = moby_record_pair(moby, &t);
    if (!o || off + 2u > (uint32_t)o->blob_len) return 0;
    uint16_t pv, cv;
    memcpy(&pv, o->prev + off, 2);
    memcpy(&cv, o->curr + off, 2);
    if (pv == 0 || cv == 0) return 0;
    *out = ((double)pv - (double)cv) * (1.0 - t);
    return 1;
}

/* Records whose translation moby_record_centre_final replaced, per pass:
 * replaced counts every replacement, sized the ones carrying a nonzero size
 * byte, wrapped the sized ones left on the record's translation. */
static struct { long replaced, sized, wrapped; } s_ctr_census;

/** @brief OPENPETE_MOBY_CENSUS: log one line for a pass that replaced a record centre, then reset the counts.
 *
 * The line carries s_ctr_census: the centres moby_record_centre_final
 * replaced, those with a nonzero size byte, and the sized ones left on the
 * record's translation (wrapped). The counts reset whether or not the line
 * prints.
 * @param pass The pass name; NULL prints "rex". */
static void moby_centre_census(const char* pass) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_CENSUS"); s = (e && *e && *e != '0'); }
    if (s && (s_ctr_census.replaced || s_ctr_census.wrapped))
        fprintf(stderr, "[moby-centre-census vbl=%u] pass=%s replaced=%ld sized=%ld wrapped=%ld\n",
                openpete_frame, pass ? pass : "rex", s_ctr_census.replaced, s_ctr_census.sized,
                s_ctr_census.wrapped);
    memset(&s_ctr_census, 0, sizeof s_ctr_census);
}

/** @brief The translation a rec_tr_final cull stores, at full precision.
 *
 * The cull takes the centre on the quarter-world grid (build_moby_record_tr's
 * derivation, truncated), shifts it left 2 and right by the animation
 * header's +2 low byte, and, when the moby's size byte (+0x4F) is nonzero,
 * runs it through GPF with IR0 = that byte and shifts right 5 (Spyro 3
 * func_80030478 0x80030B0C-0x80030C8C, Spyro 2 func_80043858
 * 0x80043EEC-0x8004406C, in the recompiled C). The stored
 * value is therefore k times the centre, k = 4 / 2^shift times size / 32 where
 * the byte is set, carried by the record in integers on the 4k-world-unit
 * grid. This returns k times the untruncated centre at this present's camera
 * and the moby's position at this present, so a still moby under a moving
 * camera does not carry a different grid residue on each tick, and the
 * canonical present and the alpha-1 sub-tick present read one number. The
 * size byte is interpolated between the two ticks where both are nonzero.
 *
 * GPF reads the shifted centre from IR1-3, which the mtc2 truncates to
 * int16: a sized centre past that range is stored wrapped, which no float
 * form reproduces, so those records keep their own translation.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param header The record's AnimationHeader, for the shift.
 * @param tr Receives the translation when one is returned.
 * @return 1 when tr was written; 0 on a game whose cull does not store the
 *         final translation, when moby or header is not an address, or for a
 *         wrapped sized centre. */
static int moby_record_centre_final(CPUState* cpu, uint32_t moby, uint32_t header, double tr[3]) {
    if (!game_layout.moby.rec_tr_final || moby < 0x80000000u || header < 0x80000000u) return 0;
    int sh = (int)(cpu->read_byte(header + game_layout.moby.anim_scale_off) & 0xFFu);
    if (sh > 30) sh = 30;
    const double k = 4.0 / (double)(1u << sh);
    double mw[3], c[3];
    moby_record_world_offset(moby, mw);
    moby_centre_at(cpu, moby, mw, c);
    double size = (double)(cpu->read_byte(moby + game_layout.moby.size_off) & 0xFFu);
    if (size != 0.0) {
        for (int i = 0; i < 3; i++)
            if (fabs(c[i] * k) >= 32768.0) { s_ctr_census.wrapped++; return 0; }
        double ds = 0.0, t = 0.0;
        const openpete_interp_obj_t* o = moby_record_pair(moby, &t);
        if (o && game_layout.moby.size_off < (uint32_t)o->blob_len &&
            o->prev[game_layout.moby.size_off] && o->curr[game_layout.moby.size_off])
            ds = ((double)o->prev[game_layout.moby.size_off]
                - (double)o->curr[game_layout.moby.size_off]) * (1.0 - t);
        size += ds;
        s_ctr_census.sized++;
    }
    const double ks = size != 0.0 ? k * size / 32.0 : k;
    for (int i = 0; i < 3; i++) tr[i] = c[i] * ks;
    s_ctr_census.replaced++;
    return 1;
}

/** @brief The camera the replayed cull records were composed against: the installed tick camera, or under a previous-queue replay the camera that queue's cull used.
 *
 * @param M Receives the camera matrix.
 * @param pos Receives the camera position.
 * @return 0 when no tick camera is installed (a canonical decode), else 1. */
static int record_replay_camera(const int (**M)[3], const int32_t** pos) {
    if (!s_tickcam_have) return 0;
    *M   = s_rec_ref_prev ? (const int (*)[3])s_rec_cam_M : (const int (*)[3])s_tickcam_M;
    *pos = s_rec_ref_prev ? s_rec_cam_pos : s_tickcam_pos;
    return 1;
}

void moby_replay_world_offset(uint32_t moby, double out[3]) {
    moby_record_world_offset(moby, out);
    if (!s_rec_ref_prev) return;
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    const uint8_t* ref = o ? rec_ref_state(o) : NULL;
    if (!ref || ref == o->curr) return;
    int32_t rv[3], cv[3];
    memcpy(rv, ref + game_layout.moby.pos_off, 12);
    memcpy(cv, o->curr + game_layout.moby.pos_off, 12);
    for (int i = 0; i < 3; i++) out[i] += (double)cv[i] - (double)rv[i];
}

/** @brief Re-base one replayed cull record from the camera the tick composed it against onto the camera this present stages.
 *
 * A cull that stores view-space records hands the replay a matrix M = Rt*Rm,
 * the camera rotation times the moby's own (mobydraw.s:355-359), and a
 * translation TR = S*(Rt*d)/4096, where d is the camera-relative moby position
 * in quarter-world units in the GTE's axis order - ((camY-mobyY)/4,
 * (camZ-mobyZ)/4, (mobyX-camX)/4), mobydraw.s:79-94, transformed and left in
 * the GTE translation at mobydraw.s:125-130,189 - and S is the per-moby size
 * rescale the cull folds in before it stores that translation
 * (mobydraw.s:450,485,537,546-550,572). Replayed as stored, the record is
 * fixed in screen space for the whole tick: every other feeder re-reads the
 * staged camera, so the moby holds still while the world moves under it and
 * steps a full tick at each boundary.
 *
 * The record also carries the moby's own world position at the tick, so a moby
 * that moves through the world steps once per tick even after the camera term
 * below turns it onto the staged camera.
 *
 * With Rp, pp the staged camera and Rt, pt the tick one, and N = Rp*Rt^-1,
 *
 *     M'  = N*M
 *     TR' = N*TR + S*(Rp*e)/4096
 *
 * where e is (pp - pt) minus the moby's own displacement over the same
 * interval, both in d's units and axis order. The inverse is explicit
 * and in double: this matrix carries the projection's aspect and FOV scaling,
 * so it is not orthonormal and its transpose is not its inverse. S is read
 * back from the record's own inputs (the animation header's scale byte is the
 * shift the cull applied; the record's low byte is its GPF factor only on a
 * game whose cull left the translation unscaled; on a game whose cull stored
 * the final translation the moby's nonzero size byte over 32 is its rescale)
 * because only the camera-motion term needs it; the N*TR term carries S
 * already.
 *
 * A present whose staged camera words equal the tick's (the canonical present,
 * or alpha 1) for a moby with no own-motion offset returns with the record
 * untouched, so that leg stays bit-exact; so does a present with no tick
 * camera installed, a record whose header is not an address, or a singular
 * tick matrix. Host-side only: the staged camera is a sub-tick view copy and
 * no guest word is written.
 * @param cpu The guest CPU state, read only.
 * @param f The record's fields, rebased in place.
 * @param moby The Moby address, for its own-motion offset and size byte. */
static void record_rebase_to_present(CPUState* cpu, moby_fields* f, uint32_t moby) {
    const int (*TM)[3];
    const int32_t* TP;
    if (!record_replay_camera(&TM, &TP) || f->header < 0x80000000u) return;

    int Rp[3][3];
    ngte_read_matrix(cpu, game_layout.cam.proj_matrix, Rp);
    int32_t pp[3];
    for (int i = 0; i < 3; i++)
        pp[i] = (int32_t)cpu->read_word(game_layout.cam.position + (uint32_t)i * 4u);
    double mw[3];
    moby_replay_world_offset(moby, mw);
    if (memcmp(Rp, TM, sizeof Rp) == 0 &&
        memcmp(pp, TP, sizeof pp) == 0 &&
        mw[0] == 0.0 && mw[1] == 0.0 && mw[2] == 0.0) return;

    double Ti[3][3];
    if (!mat3_inverse(TM, Ti)) return;

    /* N = Rp * Rt^-1, both in 4096 units on the left and 1/4096 on the right,
     * so N is the plain camera-to-camera change of basis. */
    double N[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            N[r][c] = ((double)Rp[r][0]*Ti[0][c] + (double)Rp[r][1]*Ti[1][c]
                     + (double)Rp[r][2]*Ti[2][c]);

    /* The camera-position delta in the record's units and axis order, plus the
     * moby's own displacement over the same interval in the same units. d is
     * (cam - moby) on the first two components and (moby - cam) on the third
     * (mobydraw.s:79-94), so the moby's motion enters with the opposite sign
     * per component and the two displacements share one transform. */
    double e[3] = {
        ((double)pp[1] - (double)TP[1]) / 4.0 - mw[1] / 4.0,
        ((double)pp[2] - (double)TP[2]) / 4.0 - mw[2] / 4.0,
       -((double)pp[0] - (double)TP[0]) / 4.0 + mw[0] / 4.0,
    };
    int msc = (int)(cpu->read_byte(f->header + game_layout.moby.anim_scale_off) & 0xFFu);
    if (msc < 0) msc = 0; else if (msc > 30) msc = 30;
    double S = 4.0 / (double)(1u << msc);
    /* The draw-side GPF rescale, and so the record low byte that carries its
     * factor, exists only where the cull stored the unscaled translation; a
     * cull that stored the final one folded its own rescale in already and
     * that byte means something else, which is how the decode reads it too. */
    int gpf = game_layout.moby.rec_tr_final ? 0 : (int)(f->w0 & 0xFFu);
    if (gpf) S *= (double)gpf / 32.0;
    /* That cull's own rescale is the moby's size byte over 32 where the byte
     * is nonzero (moby_record_centre_final). */
    int size = game_layout.moby.rec_tr_final
             ? (int)(cpu->read_byte(moby + game_layout.moby.size_off) & 0xFFu) : 0;
    if (size) S *= (double)size / 32.0;

    double TR[3], M[3][3];
    for (int r = 0; r < 3; r++) {
        double t = 0.0, d = 0.0;
        for (int c = 0; c < 3; c++) {
            t += N[r][c] * f->TR_frac[c];
            d += (double)Rp[r][c] * e[c];
            M[r][c] = N[r][0]*(double)f->M[0][c] + N[r][1]*(double)f->M[1][c]
                    + N[r][2]*(double)f->M[2][c];
        }
        TR[r] = t + S * d / 4096.0;
    }
    for (int r = 0; r < 3; r++) {
        f->TR_frac[r] = TR[r];
        f->TR_raw[r]  = (int32_t)lround(TR[r]);
        for (int c = 0; c < 3; c++) f->M[r][c] = (int)lround(M[r][c]);
    }
}

/** @brief The euler composition Ry·Rx·Rz in unit doubles from table sin/cos pairs.
 *
 * The order and axis signs are those of ngte_compose_euler. `inv` builds the
 * inverse, Rz⁻¹·Rx⁻¹·Ry⁻¹, each axis inverted exactly over its own c² + s²,
 * since the table's pairs are not unit length.
 * @param e The three axes' sin/cos pairs, 4096 = 1.
 * @param inv Nonzero for the inverse.
 * @param out Receives the matrix. */
static void euler_mat_d(const ngte_euler_sc* e, int inv, double out[3][3]) {
    const double ny = (double)e->cy * e->cy + (double)e->sy * e->sy;
    const double nx = (double)e->cx * e->cx + (double)e->sx * e->sx;
    const double nz = (double)e->cz * e->cz + (double)e->sz * e->sz;
    const double sg = inv ? -1.0 : 1.0;
    const double ky = inv ? 4096.0 / ny : 1.0 / 4096.0, kx = inv ? 4096.0 / nx : 1.0 / 4096.0,
                 kz = inv ? 4096.0 / nz : 1.0 / 4096.0;
    const double cy = e->cy * ky, sy = sg * e->sy * ky, cx = e->cx * kx, sx = sg * e->sx * kx,
                 cz = e->cz * kz, sz = sg * e->sz * kz;
    const double Ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
    const double Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const double Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    const double (*A)[3] = inv ? Rz : Ry, (*C)[3] = inv ? Ry : Rz;
    double t[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            t[r][c] = A[r][0] * Rx[0][c] + A[r][1] * Rx[1][c] + A[r][2] * Rx[2][c];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r][c] = t[r][0] * C[0][c] + t[r][1] * C[1][c] + t[r][2] * C[2][c];
}

/* Outcome of record_rotation_to_present, one census bin each. */
enum { REC_ROT_NOPAIR = 0, REC_ROT_SAME, REC_ROT_SWAPPED, REC_ROT_OTHER_FORM, REC_ROT_N };

/** @brief Turn a replayed cull record's rotation from the tick's euler angles to the ones this present displays.
 *
 * A replayed cull record carries the rotation the tick composed:
 * M = camera * Ry * Rx * Rz from the moby's three angle bytes, each factor one
 * MVMVA pair and stored at record +0x28 (Spyro 3 mobydraw.s:230-359, Spyro 2
 * mobydraw.s:232-361), with no scale in the matrix: the moby's scale rides
 * the translation and the vertex decode. The rotation this present displays
 * is E(present) from moby_record_euler_at_present, so the matrix becomes
 * M * E(curr)^-1 * E(present); translation and scale are untouched. Where
 * E(present) equals E(curr) (every alpha-1 present, a moby that did not
 * turn), M is kept bit-exact.
 *
 * A moby whose colour word has bit 30 set takes the other composition
 * (Spyro 3 mobydraw.s:227-228 to .L80030DBC): the same axis order from angles
 * carrying a 4-bit fraction out of that word, which the byte-angle pair does
 * not describe, so it keeps the tick's rotation.
 * @param cpu The guest CPU state, read only.
 * @param f The record fields, whose M is rewritten in place.
 * @param moby The Moby address.
 * @return The outcome's census bin (REC_ROT_*). */
static int record_rotation_to_present(CPUState* cpu, moby_fields* f, uint32_t moby) {
    if (game_layout.moby.colour_off &&
        (cpu->read_word(moby + game_layout.moby.colour_off) & 0x40000000u))
        return REC_ROT_OTHER_FORM;
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    if (!o) return REC_ROT_NOPAIR;
    const uint8_t* ref = rec_ref_state(o);
    if (!ref) return REC_ROT_NOPAIR;
    ngte_euler_sc ep, ec;
    if (!moby_record_euler_at_present(cpu, moby, &ep)) return REC_ROT_NOPAIR;
    const uint32_t ao = game_layout.moby.angle_off;
    ngte_euler_sincos(cpu, (uint32_t)ref[ao] | ((uint32_t)ref[ao + 1u] << 8)
                           | ((uint32_t)ref[ao + 2u] << 16), &ec);
    if (memcmp(&ep, &ec, sizeof ep) == 0) return REC_ROT_SAME;
    double Ei[3][3], Ep[3][3], Q[3][3];
    euler_mat_d(&ec, 1, Ei);
    euler_mat_d(&ep, 0, Ep);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            Q[r][c] = Ei[r][0] * Ep[0][c] + Ei[r][1] * Ep[1][c] + Ei[r][2] * Ep[2][c];
    int M[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            M[r][c] = (int)lround((double)f->M[r][0] * Q[0][c] + (double)f->M[r][1] * Q[1][c]
                                  + (double)f->M[r][2] * Q[2][c]);
    memcpy(f->M, M, sizeof M);
    return REC_ROT_SWAPPED;
}

/** @brief OPENPETE_MOBY_CENSUS: log one line per record-replay pass that replayed a record, with the bins of record_rotation_to_present.
 *
 * @param pass The pass name.
 * @param bins The rotation outcome counts, indexed by REC_ROT_*. */
static void moby_rot_census(const char* pass, const long bins[REC_ROT_N]) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_CENSUS"); s = (e && *e && *e != '0'); }
    if (!s || bins[REC_ROT_NOPAIR] + bins[REC_ROT_SAME] + bins[REC_ROT_SWAPPED]
              + bins[REC_ROT_OTHER_FORM] == 0) return;
    fprintf(stderr, "[moby-rot-census vbl=%u] pass=%s swapped=%ld same=%ld nopair=%ld other_form=%ld\n",
            openpete_frame, pass, bins[REC_ROT_SWAPPED], bins[REC_ROT_SAME],
            bins[REC_ROT_NOPAIR], bins[REC_ROT_OTHER_FORM]);
}

/** @brief Overwrite a record's pose fields from a derived pose, keeping w0's other bytes.
 *
 * The header pair, keyframes A and B, and the blend byte (w0 bits 8-15) take
 * the pose's values; the m_DepthOffset OT pull in bits 16-23 and the GPF
 * scale in the low byte belong to the record and stay.
 * @param f The record fields.
 * @param p The pose. */
static void moby_fields_set_pose(moby_fields* f, const moby_pose_t* p) {
    f->header   = p->header;
    f->header_b = p->header_b;
    f->frameA   = p->frame_a;
    f->frameB   = p->frame_b;
    f->w0       = (f->w0 & ~0xFF00u) | ((p->blend & 0xFFu) << 8);
}

/** @brief On a hold verdict below alpha 1, give a record the previous tick's pose whole.
 *
 * The record's moby switched into a clip with a different vertex count. No
 * vertex correspondence exists between the two meshes, so there is no pose
 * between them: the record shows the previous tick's pose with its own
 * header, vertex count, face list and keyframe patch list, and reaches this
 * tick's pose at alpha 1, the instant every other channel reaches its
 * canonical value. Translation and rotation keep interpolating: the keyframe
 * stream is model-space only, and the world position is the moby's own,
 * which the guest steps per tick independently of the clip.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's PSX address.
 * @param f The record fields, rewritten on a hold.
 * @param prev The previous pose.
 * @param verdict The moby_prev_keyframe verdict.
 * @param alpha The pair's alpha at this present.
 * @return On a hold, the model table entry of the moby's class, whose table resolves the held pose's keyframe patch list on a game that hangs the list off the cull record; 0 when the record keeps its pose. */
static uint32_t moby_fields_hold_prev(CPUState* cpu, uint32_t moby, moby_fields* f,
                                      const moby_pose_t* prev, int verdict, double alpha) {
    if (verdict != MOBY_PREV_HOLD || alpha >= 1.0) return 0u;
    moby_fields_set_pose(f, prev);
    uint32_t cls = cpu->read_half(moby + game_layout.moby.class_off);
    return cpu->read_word(game_layout.moby.model_table + cls * 4u);
}

/** @brief The previous pose the sub-tick decode reads, and its alpha.
 *
 * A lerp reads the previous pose at the pair's alpha. A hold whose previous
 * pose carries held vertices reads them as the alpha 0 end of the lerp, so
 * the whole held shape comes from what a canonical pass decoded; a hold
 * without them decodes its rewritten record alone. Any other verdict reads
 * no previous pose.
 * @param prev The previous pose from moby_prev_keyframe.
 * @param verdict The moby_prev_keyframe verdict.
 * @param model_held moby_fields_hold_prev's result, nonzero when the record holds.
 * @param copy Receives the pose a hold reads.
 * @param alpha The pair's alpha in, the decode's alpha out.
 * @return The pose, or NULL. */
static const moby_pose_t* moby_decode_prev(const moby_pose_t* prev, int verdict,
                                           uint32_t model_held, moby_pose_t* copy,
                                           double* alpha) {
    if (verdict == MOBY_PREV_LERP) return prev;
    if (!model_held || !prev->held) return NULL;
    *copy = *prev;
    *alpha = 0.0;
    return copy;
}

/* Per present, the host-built records moby_fields_pose_from_snapshot
 * rewrote (applied), those whose pose the rewrite changed (differ), and those
 * it left because the sub-tick view holds the moby at its previous tick
 * (held_skip). */
static struct { long applied, differ, held_skip; } s_pose_snap;

/** @brief OPENPETE_MOBY_CENSUS: emit one `pose_snap` line per sub-tick present, then reset its counts.
 *
 * The line carries the counts moby_fields_pose_from_snapshot accumulated. */
static void moby_pose_snap_census(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_CENSUS"); s = (e && *e && *e != '0'); }
    if (s)
        fprintf(stderr, "[moby-census vbl=%u] pose_snap applied=%ld differ=%ld held_skip=%ld\n",
                openpete_frame, s_pose_snap.applied, s_pose_snap.differ, s_pose_snap.held_skip);
    memset(&s_pose_snap, 0, sizeof s_pose_snap);
}

/** @brief The pose the snapshot pair's curr anim-state bytes derive, for a moby whose live class matches that state's.
 *
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's guest address.
 * @param oc Receives the moby's snapshot pair when one exists.
 * @param pose Receives the pose.
 * @return 1 when the pose was derived, else 0. */
static int moby_snapshot_curr_pose(CPUState* cpu, uint32_t moby,
                                   const openpete_interp_obj_t** oc, moby_pose_t* pose) {
    const openpete_interp_obj_t* o = moby_interp_find(moby);
    *oc = o;
    if (!o) return 0;
    uint16_t ccls = 0;
    memcpy(&ccls, o->curr + 0x36, 2);
    if (ccls != (uint16_t)cpu->read_half(moby + game_layout.moby.class_off)) return 0;
    uint32_t model = cpu->read_word(game_layout.moby.model_table + (uint32_t)ccls * 4u);
    return model >= 0x80000000u &&
           moby_pose_derive(cpu, model, o->curr + game_layout.moby.anim_state_off, pose);
}

/** @brief On a sub-tick present, derive a host-built record's pose from the interp-objs snapshot.
 *
 * The pose is the one derived from the snapshot's curr anim-state bytes: the
 * tick the canonical present showed, which under --runahead is the led tick
 * while guest RAM holds the committed one. Without runahead the snapshot's curr
 * is the committed tick and the pose is unchanged. A moby the view holds at its
 * previous tick keeps the pose read through the view, as every other channel of
 * its record does; a class change between the view and the snapshot keeps it as
 * well.
 * @param cpu  guest state, the sub-tick view staged
 * @param moby the moby's guest address
 * @param f    the record fields built through the view, rewritten in place */
static void moby_fields_pose_from_snapshot(CPUState* cpu, uint32_t moby, moby_fields* f) {
    if (!s_rot_now) return;
    if (moby_game.moby_held && moby_game.moby_held(moby)) { s_pose_snap.held_skip++; return; }
    const openpete_interp_obj_t* oc = NULL;
    moby_pose_t pose;
    if (!moby_snapshot_curr_pose(cpu, moby, &oc, &pose)) return;
    const moby_fields before = *f;
    /* keep the snapshot's m_DepthOffset in bits 16-23 (OT pull) */
    f->w0 = (uint32_t)oc->curr[game_layout.moby.angle_off + 3u] << 16;
    moby_fields_set_pose(f, &pose);
    s_pose_snap.applied++;
    if (f->header != before.header || f->header_b != before.header_b ||
        f->frameA != before.frameA || f->frameB != before.frameB || f->w0 != before.w0)
        s_pose_snap.differ++;
}

/** @brief During a previous-queue replay, give a record the pose the current tick's cull wrote for the same moby.
 *
 * The current copy sits in the previous slot while the replay runs. The
 * header pair, keyframes and blend byte come from that record's own words; a
 * moby the current cull did not queue takes the pose its snapshot's curr
 * anim-state bytes derive, and keeps its record's pose when that fails.
 * @param cpu The guest CPU state, read only.
 * @param moby The moby's guest address.
 * @param f The record fields, whose pose is rewritten in place. */
static void rec_pose_from_current_queue(CPUState* cpu, uint32_t moby, moby_fields* f) {
    const uint32_t sw = game_layout.moby.rec_stride / 4u;
    const uint32_t n = s_rec_prev.queued + (uint32_t)s_rec_prev.appended;
    for (uint32_t r = 0; sw && r < n && (r + 1u) * sw <= s_rec_prev.words; r++) {
        const uint32_t* w = &s_rec_prev.snap[r * sw];
        if (w[0] == 0u) break;                                /* the cull's terminator */
        if (w[game_layout.moby.rec_moby_off / 4u] != moby) continue;
        moby_pose_t p = {0};
        p.header   = w[game_layout.moby.rec_header_off / 4u];
        p.header_b = game_layout.moby.rec_header_b_off ? w[game_layout.moby.rec_header_b_off / 4u] : p.header;
        p.frame_a  = w[game_layout.moby.rec_frame_a_off / 4u];
        p.frame_b  = w[game_layout.moby.rec_frame_b_off / 4u];
        p.blend    = (w[0] >> 8) & 0xFFu;
        moby_fields_set_pose(f, &p);
        return;
    }
    const openpete_interp_obj_t* oc = NULL;
    moby_pose_t pose;
    if (moby_snapshot_curr_pose(cpu, moby, &oc, &pose)) moby_fields_set_pose(f, &pose);
}

/** @brief Build a record's fields by replaying the record the cull wrote, on a game with no host record builder.
 *
 * The record is read exactly as the canonical pass read it, so the pose is
 * the one the tick drew, identical to canonical at alpha 1. Its matrix and
 * translation are view-space against the camera the tick composed them with,
 * so record_rebase_to_present turns them onto the camera this present stages;
 * without that the record would be the one thing in the scene that does not
 * follow the interpolated view. record_rotation_to_present then turns the
 * rotation to this present's euler angles. The rebased translation still
 * carries the tick's grid residue turned onto this camera, so the centre at
 * this present replaces it where the game stores the scaled centre
 * (moby_record_centre_final); TR_raw keeps the rebased integer for the paint
 * key.
 * @param cpu The guest CPU state, read only.
 * @param rec The record's guest address.
 * @param moby The Moby address.
 * @param f Receives the fields.
 * @param rot_bins Counts the rotation outcome into its bin.
 * @return 0 when the record does not read, else 1. */
static int record_replay_fields(CPUState* cpu, uint32_t rec, uint32_t moby, moby_fields* f,
                                long rot_bins[REC_ROT_N]) {
    moby_record_t LR;
    if (!moby_game.read_record(cpu, rec, &LR)) return 0;
    memset(f, 0, sizeof *f);
    f->header  = LR.header;  f->header_b = LR.header_b;
    f->frameA  = LR.frame_a; f->frameB   = LR.frame_b;
    f->w0      = LR.w0;
    f->mscol   = LR.material;
    f->metal   = (int)((LR.material >> 24) & 0xFFu);
    memcpy(f->M, LR.M, sizeof f->M);
    for (int k = 0; k < 3; k++) {
        f->TR_raw[k]  = LR.TR_raw[k];
        f->TR_frac[k] = (double)LR.TR_raw[k];
    }
    /* A record composed from an object's prev state carries prev's keyframes;
     * the shape lerp runs from the pair's prev pose to the record's, so the
     * record takes curr's pose to span the same interval as every other. */
    if (s_rec_ref_prev) {
        const openpete_interp_obj_t* o = moby_interp_find(moby);
        if (o && rec_ref_state(o) != o->curr) rec_pose_from_current_queue(cpu, moby, f);
    }
    record_rebase_to_present(cpu, f, moby);
    rot_bins[record_rotation_to_present(cpu, f, moby)]++;
    double ctr[3];
    if (moby_record_centre_final(cpu, moby, f->header, ctr))
        memcpy(f->TR_frac, ctr, sizeof ctr);
    return 1;
}

/** @brief One sub-tick record pass over the cull queue for material `mat` ("REG" or "SP").
 *
 * The pass rebuilds each record's fields from gamestate under the present's
 * camera (or replays the captured record on a game without host reveal),
 * applies the game's draw-time row scale, lerps the shape between the tick
 * pair, and walks the result into the scene. Returns the records queued. */
static int reextract_record_pass(CPUState* cpu, const char* mat) {
    int is_spec = (mat && mat[0] == 'S' && mat[1] == 'P');
    long rot_bins[REC_ROT_N] = {0};
    s_walk_in_rex = 1;
    s_pass_overlay = !is_spec && s_canon_overlay;
    static uint32_t mobys[MAX_RECORDS], recs[MAX_RECORDS]; int nm = 0;
    /* Tier-2 hold pool is per canonical epoch: a new tick invalidates every
     * hold (its first present re-extracts everything and re-captures). */
    unsigned epoch = spyro_native_canon_epoch_stamp();
    if (epoch != s_hold_epoch) { s_hold_epoch = epoch; s_hold_n = 0; s_hold_tri_n = 0; }
    static struct { const hold_t* h; double dx, dy, vzs; } replays[MAX_RECORDS];
    int nreplay = 0;
    static uint32_t replay_moby[MAX_RECORDS]; int nreplay_moby = 0;
    s_rec_base = game_layout.moby.rec_base ? game_layout.moby.rec_base : s_rec_base_canon;
    if (!s_rec_base) { s_walk_in_rex = 0; s_pass_overlay = 0; return 0; }
    for (int r = 0; r < MAX_RECORDS; r++) {
        uint32_t rec = s_rec_base + (uint32_t)r * REC_STRIDE;
        moby_record_t R;
        if (!moby_game.read_record(cpu, rec, &R)) break;   /* queue terminator */
        if (R.moby >= 0x80000000u && nm < MAX_RECORDS) { recs[nm] = rec; mobys[nm] = R.moby; nm++; }
    }
    s_cur_mat = mat;
    s_proj_n = 0; s_nrecs = 0;
    static uint32_t culled_moby[MAX_RECORDS]; int culled_n = 0;
    for (int i = 0; i < nm; i++) {
        /* Occlusion cull: skip the whole record (decode + project + walk +
         * draws) when the moby is provably hidden behind nearer WORLD depth.
         * Display-only skip; the record itself (canonical gameplay state) is
         * untouched. Culled REG mobys still join the SPEC dedup set below -
         * their specular twin is equally hidden. */
        /* OPENPETE_MOBY_INTERP_MISS: name the record mobys with NO interp-objs
         * snapshot (they present at 30 Hz - the capture walk never saw them).
         * Rate-limited; prints the moby addr + class + position. */
        {
            static int s_imiss = -1;
            if (s_imiss < 0) { const char* e = getenv("OPENPETE_MOBY_INTERP_MISS"); s_imiss = (e && *e && *e != '0'); }
            if (s_imiss) {
                static uint64_t t0 = 0; static int n = 0;
                uint64_t nowp = openpete_phase_now_us();
                if (nowp - t0 >= 1000000u) { t0 = nowp; n = 0; }
                const openpete_interp_obj_t* io = openpete_interp_objs_find(mobys[i]);
                if ((!io || !io->have_prev) && n < 16) {
                    n++;
                    fprintf(stderr, "[moby-interp-%s vbl=%u] moby=%08x cls=%04x pos=(%d,%d,%d)\n",
                            io ? "noprev" : "MISS", openpete_frame, mobys[i],
                            (unsigned)(cpu->read_half(mobys[i] + game_layout.moby.class_off)),
                            (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 0u),
                            (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 4u),
                            (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 8u));
                }
            }
        }
        s_occl_seen++;
        int ver_check = 0;              /* verify mode: audit this moby post-extract */
        if (occl_record_culled(cpu, mobys[i])) {
            ver_check = occl_verify_begin(mobys[i]);   /* extract anyway and audit */
            if (!ver_check) {
                s_occl_culled++;
                if (!is_spec && culled_n < MAX_RECORDS) culled_moby[culled_n++] = mobys[i];
                continue;
            }
        }
        /* Tier-2 hold: a speck-sized moby captured earlier THIS epoch replays
         * its built tris translated by the origin-projection delta - skipping
         * the record rebuild AND decode+project+walk. TR comes from the slim
         * TR-only slice (no euler compose). See the hold block above
         * occl_cache_update. */
        if (hold_px_max() > 0) {
            const hold_t* h = hold_find(mobys[i], is_spec);
            if (h && h->n > 0 && h->trz > 1.0 && h->H == s_view.H_eff && nreplay < MAX_RECORDS) {
                double tr[3], sx, sy;
                build_moby_record_tr(cpu, mobys[i], tr);
                if (hold_project_origin(cpu, tr, &sx, &sy)) {
                    replays[nreplay].h  = h;
                    replays[nreplay].dx = sx - h->sx;
                    replays[nreplay].dy = sy - h->sy;
                    replays[nreplay].vzs = tr[2] / h->trz;
                    /* OPENPETE_HOLD_PROBE=1: one line per Tier-2 replay with the
                     * capture and present view-Z references and the rescale they
                     * produce. A static moby under a static camera must read
                     * vzs = 1: any other constant is a unit mismatch between the
                     * two references, which shifts the held moby's depth on every
                     * intermediate present. */
                    { static int s_hp = -1;
                      if (s_hp < 0) { const char* e = getenv("OPENPETE_HOLD_PROBE"); s_hp = (e && *e && *e != '0'); }
                      if (s_hp) fprintf(stderr, "[hold-probe vbl=%u] m=%08x trz_now=%.3f trz_cap=%.3f vzs=%.5f dx=%.3f dy=%.3f n=%d\n",
                                        openpete_frame, mobys[i], tr[2], h->trz, tr[2] / h->trz,
                                        sx - h->sx, sy - h->sy, h->n); }
                    nreplay++;
                    if (!is_spec && nreplay_moby < MAX_RECORDS) replay_moby[nreplay_moby++] = mobys[i];
                    continue;
                }
            }
        }
        moby_fields f;
        double gpf_frac = -1.0;
        if (!moby_game.host_reveal) {
            if (!record_replay_fields(cpu, recs[i], mobys[i], &f, rot_bins)) continue;
        } else {
        { int bok;
          uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          bok = build_moby_record_fields(cpu, mobys[i], &f);
          if (t0) s_us_fields += (long)(openpete_phase_now_us() - t0);
          if (!bok) continue; }   /* shaded -> handled separately */
        moby_fields_pose_from_snapshot(cpu, mobys[i], &f);
        /* .s far-skip parity: the host-rebuilt w0 has no low byte (the GPF-TR
         * scale), so moby_decode_project's mirror of .L8001F888 can't fire on
         * sub-ticks. Re-apply the canonical verdict from the LIVE record's w0 +
         * TR.z (frozen per tick - no sub-tick boundary flicker), else a GPF-far
         * moby the game popped out draws on intermediates only. */
        {
            moby_record_t LR; int lrok = moby_game.read_record(cpu, recs[i], &LR);
            uint32_t rw0 = lrok ? LR.w0 : 0u;
            if ((rw0 & 0xFFu) != 0u && f.header >= 0x80000000u) {
                int msc  = (int)(cpu->read_byte(f.header + game_layout.moby.anim_scale_off) & 0xFFu);
                int ir3  = (int)(int16_t)((LR.TR_raw[2] << 2) >> msc);
                if ((((int)(rw0 & 0xFFu) * ir3) >> 5) > 63000) continue;
                /* thread the live GPF byte into the decode so the sub-tick pose
                 * projects at the same GPF-rescaled translation */
                f.w0 = (f.w0 & ~0xFFu) | (rw0 & 0xFFu);
                /* Frozen per tick, the on-screen scale steps at the tick rate
                 * under --render-fps: the sub-tick decode takes the lerped byte. */
                gpf_frac = moby_gpf_subtick(mobys[i], (int)(rw0 & 0xFFu), gpf_frac);
            }
        }
        if (!moby_draw_row_scale(cpu, recs[i], s_cur_mat, f.M))
            moby_ratio_row_scale(cpu, recs[i], mobys[i], f.M);
        }
        if (s_nrecs >= MAX_RECORDS || s_proj_n >= MAX_PROJ) break;
        int off = s_proj_n;
        /* Two-pass shape lerp: f is the curr REAL pose (no synthesized blend - the
         * spine no longer writes m_FrameProgress); add the prev REAL pose and lerp
         * the two in model space. Crossfades within a clip AND across a clip switch
         * when the two animations share vertex topology; snaps to curr (no prev
         * pose, prev zeroed for the trace and the record) on spawn or a
         * topology-changing clip switch. */
        moby_pose_t prev = {0}; double balpha = 0.0;
        int prevkf = moby_prev_keyframe(cpu, mobys[i], f.header, &prev, &balpha);
        uint32_t model_held =
            moby_fields_hold_prev(cpu, mobys[i], &f, &prev, prevkf, balpha);
        /* The held pose is the whole shape, not one end of a lerp: it decodes
         * alone, or from the vertices a canonical pass kept for it, as the
         * alpha 0 end of the lerp. prevkf keeps the verdict for the trace. */
        moby_pose_t dpose; double dalpha = balpha;
        const moby_pose_t* dprev = moby_decode_prev(&prev, prevkf, model_held, &dpose, &dalpha);
        if (prevkf != MOBY_PREV_LERP) memset(&prev, 0, sizeof prev);
        /* OPENPETE_MOBY_POSTRACE_CLS=<class> (0xFFFF = every class) also
         * traces the anim channel here. This block sits in the sub-tick
         * re-extract pass, so a run that presents only canonical ticks prints
         * nothing however the variable is set. */
        {
            static long s_atcls = -2;
            if (s_atcls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_atcls = e ? strtol(e, NULL, 0) : -1; }
            uint16_t tcls2 = (uint16_t)cpu->read_half(mobys[i] + game_layout.moby.class_off);
            if (s_atcls >= 0 && (s_atcls == 0xFFFF || tcls2 == (uint16_t)s_atcls)) {
                uint32_t mrot = cpu->read_word(mobys[i] + game_layout.moby.angle_off + 0u);
                fprintf(stderr, "[moby-anim vbl=%u] m=%08x cls=%03x prevkf=%d balpha=%.3f curr(frm=%u nfrm=%u prog=%u anim=%u nid=%u) fa=%08x fb=%08x fap=%08x fbp=%08x w0=%04x w0p=%04x hdr=%08x hdrp=%08x rot=%06x\n",
                        openpete_feed_key_now(), mobys[i], tcls2,
                        prevkf, balpha,
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 2u), cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 3u),
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 4u), cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 0u),
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 1u),
                        f.frameA, f.frameB, prev.frame_a, prev.frame_b, f.w0 & 0xFFFFu, (prev.blend << 8) & 0xFFFFu,
                        f.header, prev.header,
                        mrot & 0xFFFFFFu);
            }
        }
        /* LOD tier from the last canonical pass (sub-tick reuse - the tier is a
         * per-tick verdict, held across intermediates). Miss default 1024 = the
         * near/HP tier, today's behaviour for a moby with no canonical record.
         * Read BEFORE the decode: the tier-adjusted vertex-count read
         * ([hdr+8+(ZSF4>>31)], 0 = skip) hides far no-LP-mesh mobys (gems). */
        int zsf4 = fade_snap_get(mobys[i], 1024);
        int got;
        { uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          got = moby_decode_project(cpu, f.header, f.header_b, f.frameA, f.frameB, f.w0,
                                    dprev, dalpha, gpf_frac,
                                    f.TR_raw, f.TR_frac, f.M, zsf4, moby_record_view(mobys[i]),
                                    s_proj + s_proj_n, MAX_PROJ - s_proj_n);
          if (t0) s_us_dec += (long)(openpete_phase_now_us() - t0); }
        if (got <= 0) continue;
        /* OPENPETE_MOBY_POSTRACE_CLS: final SUBMITTED screen-space centroid -
         * the render-side truth, one step before raster (jitter forensics). */
        {
            static long s_sccls = -2;
            if (s_sccls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_sccls = e ? strtol(e, NULL, 0) : -1; }
            uint16_t sccls = (uint16_t)cpu->read_half(mobys[i] + game_layout.moby.class_off);
            if (s_sccls >= 0 && (s_sccls == 0xFFFF || sccls == (uint16_t)s_sccls)) {
                extern int g_openpete_canon_lag_leg;
                double mx = 0, my = 0, mz = 0;
                for (int j = 0; j < got; j++) { mx += s_proj[s_proj_n+j].sx; my += s_proj[s_proj_n+j].sy; mz += s_proj[s_proj_n+j].vz; }
                double cf[3]={0,0,0}, mf[3]={0,0,0};
                int ch = openpete_native_interp_pos(game_layout.cam.base, cf);
                int mh2 = openpete_native_interp_pos(mobys[i], mf);
                /* The moby's world position at the tick and the own-motion
                 * offset the replay path applied to it this present: a record
                 * whose wpos changes tick to tick is the class the offset
                 * exists for, and dmob is how much of that tick step this
                 * present's phase carries. */
                double dmob[3]; moby_record_world_offset(mobys[i], dmob);
                fprintf(stderr, "[moby-world vbl=%u] m=%08x rot=%08X kf=(%08x,%08x,%02x) wpos=(%d,%d,%d) dmob=(%.2f,%.2f,%.2f)\n",
                        openpete_feed_key_now(), mobys[i],
                        cpu->read_word(mobys[i] + game_layout.moby.angle_off + 0u),
                        f.frameA, f.frameB, (unsigned)((f.w0 >> 8) & 0xFFu),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 0u),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 4u),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 8u),
                        dmob[0], dmob[1], dmob[2]);
                fprintf(stderr, "[moby-screen vbl=%u] leg=%s now=%.3f m=%08x cls=%03x n=%d c=(%.2f,%.2f) vz=%.1f fpos=%d/%d cam=(%.1f,%.1f,%.1f) mob=(%.1f,%.1f,%.1f) TRf=(%.2f,%.2f,%.2f)\n",
                        openpete_feed_key_now(), g_openpete_canon_lag_leg ? "CANON" : "inter",
                        (double)op_now() / 1e6, /* clock-guard: wall */ /* prints both */
                        mobys[i], sccls, got,
                        mx / got, my / got, mz / got,
                        ch, mh2, cf[0], cf[1], cf[2], mf[0], mf[1], mf[2],
                        f.TR_frac[0], f.TR_frac[1], f.TR_frac[2]);
            }
        }
        if (ver_check) occl_verify_end(mobys[i], s_proj + off, got);
        else occl_cache_update(cpu, mobys[i], s_proj + off, got, f.TR_frac, f.header);
        for (int j = 0; j < got; j++) s_proj[off + j].rec = s_nrecs;
        /* Carry the live record's address: a game whose keyframe patch list
         * hangs off the record needs it to find one, and a zero here would send
         * the provider reading at address 0. Host-built records have none and
         * the reveal path passes 0 for that reason. */
        s_recs[s_nrecs] = (rec_meta){ recs[i], off, got, f.header, f.header_b,
            moby_tier_faceblock(cpu, f.header, zsf4),
            f.w0, f.metal, f.mscol, mobys[i], zsf4,
            atan2f((float)f.TR_raw[2], (float)f.TR_raw[0]), f.frameA,
            prev.frame_a, prev.header, (float)balpha };
        s_recs[s_nrecs].model_held = model_held;
        memcpy(s_recs[s_nrecs].M, f.M, sizeof s_recs[s_nrecs].M);
        s_recs[s_nrecs].heat = (uint8_t)moby_heat_scalar(f.mscol);
        /* The colour fade follows this present's centre and the authored term
         * between the two ticks; the tier above stays the tick's. */
        s_recs[s_nrecs].zf_on = (uint8_t)fade_snap_present_z(mobys[i], zsf4, f.TR_frac[2],
                                                             s_rc_alpha, &s_recs[s_nrecs].zf);
        s_proj_n += got; s_nrecs++;
    }
    if (!is_spec) {                            /* REGULAR snapshots its moby set for the SPEC dedup */
        s_reg_moby_n = 0;
        for (int r = 0; r < s_nrecs && s_reg_moby_n < MAX_RECORDS; r++)
            s_reg_moby[s_reg_moby_n++] = s_recs[r].moby;
        /* Culled REG mobys count as "handled" too - without this, a moby in
         * BOTH queues would slip past the dedup and draw via SPEC anyway. */
        for (int c = 0; c < culled_n && s_reg_moby_n < MAX_RECORDS; c++)
            s_reg_moby[s_reg_moby_n++] = culled_moby[c];
        /* Hold-replayed REG mobys likewise (they render, just from the hold). */
        for (int c = 0; c < nreplay_moby && s_reg_moby_n < MAX_RECORDS; c++)
            s_reg_moby[s_reg_moby_n++] = replay_moby[c];
        /* Widescreen reveal on SUB-TICKS too: the render set above is the .s
         * cull-record queue, which never contains the off-4:3 mobys the 4:3
         * cone cull skipped - the canonical feed host-builds those via
         * append_revealed_regular, so without this they existed ONLY on
         * canonical presents and flickered at high FPS in wide aspects.
         * Same builder, re-projected through the
         * interpolated g_Camera; dedups vs s_recs (the queue set - an
         * occl-culled off-4:3 queue moby may re-enter here, which is merely
         * redundant draw work, never a wrong pixel). Inert at 4:3. */
        append_revealed_regular(cpu);
    }
    build_walk_tris(cpu);                       /* SPEC: build_walk_tris dedups vs s_reg_moby */
    /* Tier-2 hold capture: any freshly-extracted speck-sized moby (extent from
     * its just-refreshed occl rect) banks its built tris. Runs whenever a small
     * moby extracted (not just the epoch's first present): dedup via hold_find. */
    if (hold_px_max() > 0) {
        int hpx = hold_px_max();
        for (int r = 0; r < s_nrecs && s_hold_n < HOLD_MOBY_MAX; r++) {
            uint32_t mob = s_recs[r].moby;
            if (moby_is_hud(mob)) continue;
            const occl_rect_t* e = &s_occl_cache[occl_slot(mob)];
            if (e->moby != mob) continue;
            if ((e->x1 - e->x0) >= (float)hpx || (e->y1 - e->y0) >= (float)hpx) continue;
            if (hold_find(mob, is_spec)) continue;      /* already banked this epoch */
            /* The capture reference comes from build_moby_record_tr, the same
             * derivation the replay re-measures against, so the replay's view-Z
             * rescale is the ratio of one quantity to itself and is exactly 1
             * for a moby that has not moved. The record's own TR is not
             * interchangeable with it: on a game whose cull stores view-space
             * records, that TR carries the cull's unit scaling (a factor of 4
             * on the quarter-world translation, times the per-moby size
             * rescale), so a ratio taken across the two derivations pulls every
             * held moby that many times nearer and punches it through whatever
             * stands in front of it. */
            double htr[3], sx, sy;
            build_moby_record_tr(cpu, mob, htr);
            if (!hold_project_origin(cpu, htr, &sx, &sy)) continue;
            int off = s_hold_tri_n, n = 0;
            uint16_t hcls = 0; uint8_t hfront = 0;
            for (int wi = 0; wi < s_gout.n && s_hold_tri_n < HOLD_TRI_MAX; wi++) {
                if (s_wtri_moby[wi] != mob) continue;
                if (n == 0) { hcls = s_wtri_cls[wi]; hfront = s_wtri_front[wi]; }
                s_hold_tri[s_hold_tri_n]  = s_wtri[wi];
                s_hold_fkey[s_hold_tri_n] = s_wtri_fkey[wi];
                s_hold_mode[s_hold_tri_n] = s_wtri_mode[wi];
                s_hold_mangle[s_hold_tri_n]  = s_wtri_mangle[wi];
                s_hold_dofs[s_hold_tri_n]    = s_wtri_dofs[wi];
                s_hold_special[s_hold_tri_n] = s_wtri_special[wi];
                s_hold_tri_n++; n++;
            }
            hold_t* h = &s_hold[s_hold_n++];
            h->moby = mob; h->epoch = s_hold_epoch; h->off = off; h->n = n;
            h->spec = is_spec; h->sx = sx; h->sy = sy; h->trz = htr[2];
            h->H = s_view.H_eff;
            h->cls = hcls; h->front = hfront;
        }
    }
    /* Tier-2 hold replay: append the banked tris, translated by the origin
     * delta, view-Z rescaled. */
    for (int k = 0; k < nreplay; k++) {
        const hold_t* h = replays[k].h;
        float dx = (float)replays[k].dx, dy = (float)replays[k].dy;
        float vzs = (float)replays[k].vzs;
        /* cls/front are re-stamped per replayed tri below: without
         * them the replayed tris kept whatever stale values these arrays
         * held at their indices, so a held moby lost its mod material on
         * every intermediate present (distant decor flickering stock-bright
         * at high FPS). The material SLOT is no longer among them - it is
         * resolved from (moby, class) at stamp time in push_scene_moby,
         * after the per-cycle intern table has been reset, which is the
         * only point where a slot id is meaningful. */
        s_hold_replayed++;
        for (int t = 0; t < h->n && s_gout.n < MAX_NTRI; t++) {
            s_wtri_ui[s_gout.n]      = 0;
            s_wtri_front[s_gout.n]   = h->front;
            s_wtri_cls[s_gout.n]     = h->cls;
            s_wtri_moby[s_gout.n]    = h->moby;
            s_wtri_fkey[s_gout.n]    = s_hold_fkey[h->off + t];
            s_wtri_mode[s_gout.n]    = s_hold_mode[h->off + t];
            s_wtri_mangle[s_gout.n]  = s_hold_mangle[h->off + t];
            s_wtri_dofs[s_gout.n]    = s_hold_dofs[h->off + t];
            s_wtri_special[s_gout.n] = s_hold_special[h->off + t];
            ntri_t* o = &s_wtri[s_gout.n];
            *o = s_hold_tri[h->off + t];
            for (int vv = 0; vv < 3; vv++) { o->v[vv].sx += dx; o->v[vv].sy += dy; o->v[vv].vz *= vzs; }
            s_gout.n++;
        }
    }
    push_scene_moby();
    moby_rot_census(mat, rot_bins);
    moby_centre_census(mat);
    s_walk_in_rex = 0;
    s_pass_overlay = 0;
    return nm;
}

/** @brief Replay the overlay record a borrowing commit kept, as an overlay pass of the sub-tick re-extract.
 *
 * Runs when the tick's regular-pass capture is a later cull's (s_canon_overlay
 * 0) and the commit of the same canonical epoch rebuilt the overlay record in
 * a borrowed copy (s_ovl_rex): the record is installed in a borrowed copy
 * again, replayed by reextract_record_pass as the overlay pass, and the copy
 * restored.
 * @param cpu The sub-tick view's CPU state, read only. */
static void moby_ovl_reextract(CPUState* cpu) {
    if (!s_ovl_rex.valid || s_canon_overlay || !spyro_native_canon_epoch_fresh(s_ovl_rex.epoch)) return;
    const uint32_t base_canon = s_rec_base_canon;
    ovl_copy_borrow(s_ovl_rex.rec, s_ovl_rex.base);
    s_rec_base_canon = s_ovl_rex.base;
    s_canon_overlay = 1;
    reextract_record_pass(cpu, "REG");
    s_canon_overlay = 0;
    s_rec_base_canon = base_canon;
    ovl_copy_return();
}

/** @brief Whether OPENPETE_LOAD_CENSUS is set to a nonzero value, read once.
 *
 * Makes moby_reextract_body log a skipped pass and its per-present feed line.
 * @return Nonzero when on. */
static int moby_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}

static void moby_reextract_body(CPUState* cpu);
void moby_reextract(CPUState* cpu) {
    s_in_reextract = 1;
    moby_reextract_body(cpu);
    s_in_reextract = 0;
}
/** @brief Replay each canonical moby pass still fresh under this present's cameras, then push the scene.
 *
 * Does nothing without the native scene or when the moby path was not
 * canon-drawn. The regular, specular, second-kind and shaded replays each run
 * only while their own producer ran in the last canonical present; the
 * specular replay swaps its captured scratch in through the sub-tick view and
 * restores the regular scratch after. Also sets the HUD and actor-front
 * flags and prints the occlusion, feed and clip-switch census lines their
 * OPENPETE_* variables enable.
 * @param cpu The guest CPU state. */
static void moby_reextract_body(CPUState* cpu) {
    /* Only replay mobys the canonical frame actually drew. During a level
     * transition the moby render path may not fire (or only for the collected-gem
     * mobys); gating on the canon-drawn mask reproduces exactly that set instead
     * of replaying the previous level's mobys from stale cull records. The
     * gate also holds the native scene on and the path selected. */
    if (!hook_reextract_ready(RP_MOBY)) {
        if (moby_census_on() && openpete_native_scene_enabled())
            fprintf(stderr, "[census vbl=%u MOBY-RE] SKIP (not canon-drawn or "
                            "deselected)\n", openpete_frame);
        return;
    }

    /* Per-producer freshness (canonical-render epoch): RP_MOBY is shared by
     * THREE canonical producers - REGULAR (func_8001F798), SPECULAR
     * (func_80020F34) and SHADED/HUD/text (func_80022A2C) - and a pause menu or
     * portal-load screen runs ONLY the SHADED/text one. Each re-extract pass
     * replays its capture only if ITS producer ran during the last canonical
     * present; otherwise the pass is skipped (the canonical present didn't draw
     * that content, so neither may the sub-ticks). Without this, the load
     * screen's text kept RP_MOBY canon-drawn and the stale REG/SPEC queues from
     * the last gameplay tick painted the previous level's mobys over the frozen
     * backdrop - the "moby soup" during level transitions (--render-fps only). */
    int reg_fresh  = spyro_native_canon_mobyreg_fresh();
    int spec_fresh = s_canon_spec_valid && spyro_native_canon_epoch_fresh(s_canon_spec_epoch);
    int shd_fresh  = spyro_native_canon_shaded_fresh();

    int nm = 0, reg_walk = 0;
    /* Each pass re-derives from the camera its producer drew under (held
     * verbatim when the tick ended under a different one, else the
     * interpolated bracket). */
    spyro_native_prod_camera_enter(SPYRO_PROD_MOBYREG);
    moby_view_build(cpu);
    if (reg_fresh) {
        nm = reextract_record_pass(cpu, "REG");
        reg_walk = s_gout.n;
    }
    rec_held_queue_census(nm, reg_walk);
    openpete_phase_set_count(OPPC_MOBYS, nm);   /* perf census: records this present */

    /* SPEC pass: replay the captured specular queue so specular-ONLY mobys (the
     * rescue dragon) render on sub-ticks too. Swap the REGULAR scratch (the
     * orchestrator's bracket restored it) for the captured SPEC scratch, run a
     * dedup'd record pass, then restore the REGULAR scratch. */
    int spec_walk = 0;
    if (spec_fresh) {
        /* The window is wider than any one staged span (it covers both the
         * colour scratch and the cull records), so it is swapped span by span
         * through the view rather than through one host pointer: a memcpy from
         * the window's first resolved address runs past that span's end into
         * the next staged region, and off a sub-tick present it would reach
         * gameplay memory. */
        uint32_t spec_base = moby_game.scratch + SPEC_SCR_LO;
        static uint8_t saved_reg[SPEC_SCR_MAX];
        openpete_subtick_view_peek(spec_base, saved_reg, SPEC_SCR_LEN);  /* REGULAR (from bracket) */
        openpete_subtick_view_poke(spec_base, s_canon_spec, SPEC_SCR_LEN); /* specular queue + colour */
        reextract_record_pass(cpu, "SP");
        spec_walk = s_gout.n;
        openpete_subtick_view_poke(spec_base, saved_reg, SPEC_SCR_LEN);   /* restore REGULAR */
    }
    /* The second model kind: the queue its canonical pass captured, decoded
     * again under this present's camera. Its producer is its own draw hook,
     * which a menu or map state leaves unrun while the regular pass keeps
     * running, so the replay takes that pass's own epoch stamp rather than the
     * regular pass's freshness. */
    int sim_fresh = moby_native_simple_fresh();
    int sim_walk = 0;
    if (sim_fresh) {
        s_cur_mat = "SIM";
        moby_view_build(cpu);
        s_gout.n = 0;
        moby_game.simple_replay(cpu);
        sim_walk = s_gout.n;
        push_scene_moby();
    }
    if (reg_fresh) moby_ovl_reextract(cpu);
    spyro_native_prod_camera_leave();

    /* Shaded mobys (gems/props/HUD icons/text) - already re-derive from g_Camera. */
    if (shd_fresh) {
        s_cur_mat = "SHD";
        spyro_native_prod_camera_enter(SPYRO_PROD_SHADED);
        moby_view_shd_build(cpu);
        build_shaded_walk_tris(cpu);
        spyro_native_prod_camera_leave();
        push_scene_moby();
    }
    openpete_native_scene_set_hud_under_player(
        moby_game.hud_under_player ? moby_game.hud_under_player(cpu) : 0);
    moby_native_set_actor_front(cpu);

    /* Occlusion census: per-present culled count for the phase counters + an
     * OPENPETE_MOBY_OCCL_DBG once-per-second summary line. */
    {
        static long prev_culled = 0;
        openpete_phase_set_count(OPPC_CULLED, (int)(s_occl_culled - prev_culled));
        prev_culled = s_occl_culled;
        static int occl_dbg = -1;
        if (occl_dbg < 0) { const char* e = getenv("OPENPETE_MOBY_OCCL_DBG"); occl_dbg = (e && *e && *e != '0'); }
        if (occl_dbg) {
            static uint64_t win0 = 0; static long wc = 0, ws = 0;
            uint64_t now = openpete_phase_now_us();
            if (win0 == 0) { win0 = now; wc = s_occl_culled; ws = s_occl_seen; }
            if (now - win0 >= 1000000u) {
                long dc = s_occl_culled - wc, dsn = s_occl_seen - ws;
                static long pq[OPENPETE_OCCL_Q_COUNT], pf[4];
                long q[OPENPETE_OCCL_Q_COUNT];
                openpete_native_occl_qstats(q);
                fprintf(stderr, "[moby-occl vbl=%u] culled %ld / %ld records (%.0f%%) | "
                        "pre: ui=%ld nocache=%ld stalecache=%ld refreshed=%ld | "
                        "query: nogrid=%ld susp=%ld stale=%ld near=%ld edge=%ld vis=%ld cull=%ld "
                        "offscr=%ld moved=%ld\n",
                        openpete_frame, dc, dsn, dsn > 0 ? 100.0 * (double)dc / (double)dsn : 0.0,
                        s_occl_f[0]-pf[0], s_occl_f[1]-pf[1], s_occl_f[2]-pf[2], s_occl_f[3]-pf[3],
                        q[0]-pq[0], q[1]-pq[1], q[2]-pq[2], q[3]-pq[3], q[4]-pq[4], q[5]-pq[5],
                        q[6]-pq[6], q[7]-pq[7], q[8]-pq[8]);
                if (occl_verify_on())
                    fprintf(stderr, "[moby-occl-verify vbl=%u] would_cull=%ld wrong=%ld (%.3f%%)\n",
                            openpete_frame, s_ver_would, s_ver_wrong,
                            s_ver_would > 0 ? 100.0 * (double)s_ver_wrong / (double)s_ver_would : 0.0);
                for (int k = 0; k < OPENPETE_OCCL_Q_COUNT; k++) pq[k] = q[k];
                for (int k = 0; k < 4; k++) pf[k] = s_occl_f[k];
                win0 = now; wc = s_occl_culled; ws = s_occl_seen;
            }
        }
    }

    static int feeddbg = -1;
    if (feeddbg < 0) { const char* e = getenv("OPENPETE_REEXTRACT_FEEDDBG");
                       feeddbg = (e && *e && *e != '0') || moby_census_on(); }
    if (feeddbg) {
        static unsigned last = 0xFFFFFFFFu;
        if (openpete_frame != last) { last = openpete_frame;
            fprintf(stderr, "[reextract-moby vbl=%u] fresh reg=%d spec=%d shd=%d | recs_in=%d reg_walk=%d spec_walk=%d sim_walk=%d shd_walk=%d\n",
                    openpete_frame, reg_fresh, spec_fresh, shd_fresh,
                    nm, reg_walk, spec_walk, sim_walk, shd_fresh ? s_gout.n : 0); }
    }
    occl_leg_census_report();
    static int animswdbg = -1;
    if (animswdbg < 0) { const char* e = getenv("OPENPETE_MOBY_ANIMSW_DBG"); animswdbg = (e && *e && *e != '0'); }
    if (animswdbg) {
        static long last_cross = -1, last_snap = -1;
        static long last_guard = -1;
        long guard = s_hold_shown + s_hold_period + s_prevhdr_sig + s_prevhdr_unk + s_animsw_heldx +
                     s_animsw_noheld + s_animsw_scale_lerp + s_animsw_scale_hold + s_prev_rescaled;
        if (s_animsw_cross != last_cross || s_animsw_snap != last_snap || guard != last_guard) {
            last_cross = s_animsw_cross; last_snap = s_animsw_snap; last_guard = guard;
            fprintf(stderr, "[moby-animsw vbl=%u] clip-switch crossfaded=%ld snapped(count-mismatch)=%ld "
                            "held(facing-reversal)=%ld hold-shown=%ld hold-period-ended=%ld "
                            "prevhdr-refused(sig)=%ld prevhdr-refused(unknown)=%ld "
                            "held-shown=%ld held-refused=%ld scale-differ(lerp)=%ld "
                            "scale-differ(hold)=%ld prev-rescaled=%ld\n",
                    openpete_frame, s_animsw_cross, s_animsw_snap, s_animsw_mirror,
                    s_hold_shown, s_hold_period, s_prevhdr_sig, s_prevhdr_unk, s_animsw_heldx,
                    s_animsw_noheld, s_animsw_scale_lerp, s_animsw_scale_hold, s_prev_rescaled);
        }
    }
}

/* One entry per draw pass the descriptor names, filled at apply_options. The
 * registry stops at the first zero address, so the table is COMPACTED: a
 * pass the layout leaves at 0 contributes no entry instead of truncating the
 * ones after it. A game that names no pass registers nothing and never enters
 * this feeder. */
static feature_dispatch_entry_t moby_dispatch[6];

/** @brief The feature module's option hook: fill moby_dispatch with the cull and draw hooks the game layout allows.
 *
 * Entries for a pass whose layout address is 0 are left out, so the table is
 * compacted. Without all four decoder slots (vstream_init, vstream_next,
 * decode_faces, read_record) only the second-kind pass registers, and only
 * when moby_game.simple_pass is set; a note prints to stderr when
 * OPENPETE_MOBY_CENSUS is set. The table ends on an address of 0.
 * @param opts Unused. */
static void moby_apply_options(const openpete_options_t* opts) {
    (void)opts;
    /* No decoder slots means the geometry would be read with another game's
     * bit layout. Refuse every pass rather than emit wrong triangles, and say
     * why once. */
    int have = moby_game.vstream_init && moby_game.vstream_next &&
               moby_game.decode_faces && moby_game.read_record;
    if (!have && getenv("OPENPETE_MOBY_CENSUS"))
        fprintf(stderr, "[moby-native] feeder inert: no moby_game decoder slots\n");
    const struct { uint32_t addr; feature_dispatch_fn fn; } passes[] = {
        /* The cull runs first and owns the record list every draw pass reads. */
        { game_layout.moby.hook_cull,          on_cull          },
        { game_layout.moby.hook_draw_regular,  on_draw_regular  },
        { game_layout.moby.hook_draw_specular, on_draw_specular },
        { game_layout.moby.hook_draw_shaded,   on_draw_shaded   },
        /* The second kind reads none of the animated stream decoders, so it
         * registers on its own provider slot. */
        { moby_game.simple_pass ? game_layout.moby.hook_draw_simple : 0u, on_draw_simple },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof passes / sizeof passes[0]; i++)
        if (passes[i].addr && (have || passes[i].fn == on_draw_simple)) {
            moby_dispatch[n].addr = passes[i].addr;
            moby_dispatch[n].fn   = passes[i].fn;
            n++;
        }
    moby_dispatch[n].addr = 0u;
    moby_dispatch[n].fn   = NULL;
}

const feature_module_t moby_module = {
    .name           = "moby_native",
    .apply_options  = moby_apply_options,
    .dispatch_table = moby_dispatch,
};
