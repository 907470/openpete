// games/common/renderer/moby.c — the MOBY (3D actor) renderer, shared across
// games.
//
// Mobys are the animated, textured actors: enemies, gems, props, HUD icons.
// A game's cull pass writes one render record per drawn moby into the render
// scratch; one or more draw passes then walk those records, decode each
// model's packed vertex stream and per-face material stream, project through
// the GTE and emit. This module is the scene-extraction half: it reads the
// records, re-derives the same geometry in float (mirroring
// runner/src/gte.cpp RTPS, with a real TR translation), and produces
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
// Determinism: never writes PSX RAM. When active it calls the real render
// function exactly once (behaviour-identical to the normal dispatch
// fall-through) and only READS the records + polybuf afterwards.

#include "feature_registry.h"
#include "renderer/orchestrator.h"  /* the canonical-render epoch + producer-camera brackets */
#include "renderer/moby.h"          /* this feeder's exported entries */
#include "renderer/moby_game.h"     /* per-game decoders, classifications and addresses */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "openpete_psx.h"
#include "openpete_vstream.h"       /* frame-word field -> stream address (handles) */
#include "openpete_subtick_view.h"            /* openpete_psx_to_host_ptr (SPEC scratch bracket) */
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_log.h"           /* OPL_ERROR_F — ABR/blend FIXME runtime alarms */
#include "openpete_interp.h"        /* op_now() — sub-tick clock for rotation interp */
#include "renderer/moby_rank.h"              /* same-moby painter rank source (OT-chain read) */
#include "openpete_interp_objs.h"   /* per-object prev/curr snapshots (rotation interp) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "openpete_ui_moby.h"  /* HUD/UI moby classifier (overlay-on-top tagging) */
#include "openpete_mods.h"     /* per-instance material slot resolve (S8b) */
#include "openpete_workpool.h" /* data-parallel face walk (--extract-threads) */

#include <math.h>
#include <stdint.h>
#include <limits.h>   /* INT32_MAX/INT32_MIN: the removed extended-LOD cull ceiling */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"          /* game_layout.moby: the engine addresses this walk reads */
#include "openpete_recomp_module.h"  /* the game's own moby draws, run through the module dispatch */
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern unsigned openpete_frame;

/* Run one of the game's own moby draw passes through the recomp module. */
/* Run one of the game's own moby draw passes through the recomp module. */
static void moby_run_draw(CPUState* cpu, uint32_t addr) {
    openpete_recomp_module_get()->dispatch(cpu, addr);
}

/* The head of the moby array: an address the game stores the first Moby's
 * pointer at, or the array itself where the game keeps one at a fixed
 * address. */
static uint32_t moby_array_head(CPUState* cpu) {
    if (!game_layout.moby.array_head) return 0u;
    return game_layout.moby.array_direct ? game_layout.moby.array_head
                                         : cpu->read_word(game_layout.moby.array_head);
}

static const char* s_cur_mat = "REG";   /* material tag for the current pass (REG/SPEC) */
/* Clip-switch crossfade census (OPENPETE_MOBY_ANIMSW_DBG): how often a sub-tick moby
 * shape-lerp spans a clip switch (prev m_Animation != curr) — crossfaded when the two
 * clips share vertex count, snapped when they don't. Pure instrument, no behaviour. */
static long s_animsw_cross = 0, s_animsw_snap = 0;
static long s_shmode[3]; /* shaded face-mode census: [0]=gouraud [1]=flat-basic [2]=flat-metal(bit1) */
static int  s_sh_nohl = -1; /* OPENPETE_SHADED_NOHL=1 disables the gouraud spec boost (A/B) */

/* The cull-record queue the game's cull pass built, and one record's size.
 * Every material pass over the same model kind shares one queue. The base is a
 * fixed address, or the tick's own draw-scratch block below the polybuf end —
 * which moves with the draw buffer, so it is read per pass and never cached
 * across a tick. */
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
/* The record queue the last canonical pass read, for an orchestrator that has
 * to stage those bytes for a sub-tick present: a queue carved out of the
 * polybuf runs from its base to the polybuf end, and the draw passes that
 * follow the moby draw reuse that memory before the present happens. */
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
 * draw's own output — packed screen XY and colour words — by the time this
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
static uint32_t s_rec_snap[REC_SNAP_WORDS];
static uint32_t s_rec_snap_base;    /* guest address the copy starts at */
static uint32_t s_rec_snap_words;   /* 0 = no copy; reads pass through */
/* The records the cull itself wrote, and whether the copy reached the cull's
 * terminator: the extended-LOD reveal appends its own records after them and
 * has to be able to put the terminator back where the cull left it. */
static uint32_t s_rec_snap_queued;
static int      s_rec_snap_ended;
static int      s_rec_snap_appended;
static void moby_rec_append_reset(void);

static void moby_rec_snapshot(CPUState* cpu) {
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

/* Drop every appended record, leaving the copy exactly as the cull hook took
 * it. The appends belong to one tick's reveal and are rebuilt on the next. */
static void moby_rec_append_reset(void) {
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
 * pass rebuilds the record queue with its own set — including mobys the first
 * pass did not draw — and overwrites the per-vertex colour scratch leaves the
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
static void moby_native_capture_spec_scratch(CPUState* cpu) {
    /* Runahead: speculative fires must not stamp/overwrite the canonical
     * SPEC capture (same rule as scratch_row_capture — the stamp would be
     * the uncommitted epoch and the SPEC pass would skip on every
     * intermediate present, strobing specular mobys). */
    extern int openpete_speculative;
    if (openpete_speculative) return;
    if (!SPEC_SCR_LEN || SPEC_SCR_LEN > SPEC_SCR_MAX) return;
    const void* h = openpete_subtick_host_ptr(moby_game.scratch + SPEC_SCR_LO);
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
 * game authored — HUD icons (address-range tagged) and every billboard-path
 * record. Rebuilt with their source views. */
static ngte_view s_view_canon;
static ngte_view s_view_shd_canon;
static void moby_view_build(CPUState* cpu)     { ngte_view_build(cpu, &s_view);     ngte_view_canon(&s_view,     &s_view_canon); }
static void moby_view_shd_build(CPUState* cpu) { ngte_view_build(cpu, &s_view_shd); ngte_view_canon(&s_view_shd, &s_view_shd_canon); }
/* The regular-walk view a record projects through: HUD-range mobys keep the
 * game's projection, the rest take the pass view. */
static const ngte_view* moby_record_view(uint32_t mob) {
    return (openpete_ui_moby_is_addr_static(mob) || openpete_ui_moby_is_addr_ui(mob))
        ? &s_view_canon : &s_view;
}

/* Float RTPS mirror WITH translation: ngte_project_raw (shared).
 * Returns view-Z (MAC3); caller owns the near-plane policy. */
static double project(const int M[3][3], const double TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    return ngte_project_raw(M, TR, vx, vy, vz, ofx_px, ofy_px, v, out_sx, out_sy, out_sat);
}

/* GTE near-plane mirror for one projected vertex.
 *
 * project() above is the RAW float H/z: it keeps the true screen X the
 * widescreen reveal needs, but it has no counterpart to the GTE's UNR divide
 * cap. RTPS saturates the quotient at 0x1FFFF once SZ3 <= H/2, then clamps SXY
 * to the 11-bit signed window — so on hardware a vertex that close is pinned
 * near the screen edge. Ours divides by an arbitrarily small view-Z instead and
 * flings the vertex to |x| in the hundred-thousands, which the rasterizer then
 * streaks across the frame.
 *
 * Inside that regime the raw coordinate carries no information the hardware
 * ever had, so take the GTE's: re-project through the faithful path (divide
 * cap + ±1024 SXY saturation) and flag the vertex. Outside it — every vertex
 * further than H/2 — nothing changes, so the reveal keeps its true X.
 *
 * Returns 1 when the vertex was in the regime (and sx/sy were replaced). */
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
/* Walk by OT tag length (stride=((w0>>24)+1)*4) — robust regardless of the
 * exact per-code layout. Decode XY/RGB/UV by code byte (prim[0x7]); the
 * offsets are confirmed by the hex dump. */
static void capture_gte(CPUState* cpu, uint32_t start, uint32_t end) {
    s_gte_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < MAX_PRIMS) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFF;
        if (len == 0 || len > 24) break;          /* not a tag / corrupt — stop */
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
             * xy2 @ [8], xy3 @ [11]. (Confirmed by the hex dump.) */
            /* General PSX polygon decode by command bits (bit4=gouraud,
             * bit3=quad, bit2=textured). Handles every moby variant uniformly:
             * GT3/GT4 (0x34/0x3C), G3/G4 (0x30/0x38), FT3/FT4 (0x24/0x2C, incl.
             * the +0x02 semi-transparent 0x26/0x2E), F3/F4. Layout: word[1] =
             * rgb0|code, then per vertex {rgb if gouraud&&k>0}{xy}{uv if tex};
             * clut rides v0's uv-word hi, tpage rides v1's. Bit-identical to the
             * old hardcoded GT3/GT4 path, so the verified match is preserved. */
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
                /* Loud-once per unknown code (extends the ABR-FIXME discipline:
                 * silent until it matters, loud when it does). Silent-skip is only
                 * OK for explicitly-culled geometry — never for "didn't recognize
                 * this". Was VERIFY-census-only (report_code_hist). */
                static uint8_t s_uncoded_warned[256];
                if (!s_uncoded_warned[code & 0xFF]) {
                    s_uncoded_warned[code & 0xFF] = 1;
                    fprintf(stderr, "native: WARNING undecoded moby prim code 0x%02X (vbl=%u) — polys "
                            "dropped from the native feed; add a decoder or explicitly cull\n",
                            code & 0xFF, openpete_frame);
                }
            }
            s_gte_n++;
        }
        p += stride;
    }
}

/* Cumulative prim-code coverage line: every code F798 has emitted so far, and
 * which we still DON'T decode (those are dropped from the native feed = missing
 * polys). "UNDECODED: none" across a whole demo run means the feed is complete
 * for the regular material. */
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
/* Does this record carry the pose (header, frame)? A keyframe reference is a
 * pointer into the model's frame array, or — where the cull copies the
 * keyframe's leading stream word into the record instead — that word, which
 * may hold any value, so the pose exists exactly when its header does. */
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
} moby_pose_t;

/* A keyframe named the way the record names it: the address of its 8-byte
 * record in the header's frame array, or, where the cull loads that record's
 * leading stream word into the draw record (rec_frame_is_word), the word
 * (mobydraw.s:463-467 the derivation and the load, :532-535 the store). */
static uint32_t moby_keyframe_ref(CPUState* cpu, uint32_t header, uint32_t index) {
    uint32_t at = header + game_layout.moby.anim_frames + index * game_layout.moby.anim_frame_stride;
    return game_layout.moby.rec_frame_is_word ? cpu->read_word(at) : at;
}

/* st = the five animation state bytes, live or from an interp snapshot.
 * Returns 0 when anims[id] is not a header. */
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
    return 1;
}

static void vstream_init(CPUState* cpu, vstream* s, uint32_t header, uint32_t frame, int s5) {
    moby_game.vstream_init(cpu, s, header, frame, s5);
}
static void vstream_next(CPUState* cpu, vstream* s, int first) {
    moby_game.vstream_next(cpu, s, first);
}

/* Decode + float-project one record's vertices. Mirrors F798: the no-blend path
 * (block 8001FCC8) when the keyframe blend factor is 0, else the INTPL blend
 * path (block 8001FA44) -- decode BOTH frame A and frame B streams and lerp each
 * vertex in MODEL space by blend/4096 before RTPS (the asm INTPLs IR(=A) toward
 * FC(=B) by IR0(=blend) then RTPSs the result). Writes screen XY + view-Z.
 * Returns vertex count. The caller set-matches these vs the emitted prims. */
typedef struct { float sx, sy, vz; int sat; int unr; int rec;
                 float ex, ey, ez; float tvz; } proj_xy;  /* vz=view-Z; rec=source record (diag);
                 unr = this vertex fell in the GTE divide-overflow regime (see
                 moby_gte_near_fixup); sx/sy are then the hardware-clamped pair;
                 ex/ey/ez = the model->view rotated vertex offset (M.v)/4096 = viewPos-TR,
                 the input to the specular env-reflection UV map (#1). */

/* Opens the previous pose's keyframe streams for the two-pass shape lerp.
 * Both decode with that pose's own ShortShift, equal to the current one within
 * a clip and free to differ across a count-matched clip switch; keyframe B's
 * stream starts on keyframe B's own header. Returns the pose's blend numerator
 * (1.0 = 4096; 0 opens keyframe A alone), or -1 when there is no previous pose. */
static int moby_prev_streams(CPUState* cpu, const moby_pose_t* prev, vstream* A, vstream* B) {
    if (!prev || !moby_pose_live(prev->header, prev->frame_a)) return -1;
    int s5 = (cpu->read_byte(prev->header + game_layout.moby.anim_shift_off) & 0xFF) + 1;
    int blend = (int)(((prev->blend << 8) >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask);
    vstream_init(cpu, A, prev->header, prev->frame_a, s5);
    if (blend <= 0 || !moby_pose_live(prev->header_b, prev->frame_b)) return 0;
    vstream_init(cpu, B, prev->header_b, prev->frame_b, s5);
    return blend;
}

/* Field-driven decode + float-project core. Both the .s-record path
 * (moby_project_record) and the host-built widescreen-reveal path
 * (append_revealed_regular) feed it the SAME six fields, so a revealed moby
 * decodes/projects bit-identically to a culled one:
 *  header  = AnimationHeader* (m_Scale@+5, ShortShift@+6, count@+8, faces@+0x14, colors@+0x18)
 *  frameA/frameB = AnimationFrame* keyframes (vertex stream packed in word0)
 *  w0      = record flags word (keyframe blend factor in bits 8-15)
 *  TR_raw  = stored translation (pre (<<2)>>m_Scale), the GTE view-space MAC
 *  M       = model->view rotation (int16, 1.0=4096) */
/* prev + blend_alpha = OPTIONAL second (prev) keyframe pose for the high-FPS
 * sub-tick SHAPE interpolation. When prev names a live pose, the decoder
 * reconstructs BOTH the prev rendered pose (its keyframes at its blend byte)
 * and the curr rendered pose (frameA,frameB at blend w0), then lerps the two
 * poses in MODEL space by blend_alpha before projecting once. This
 * interpolates between two poses the game ACTUALLY drew — never a
 * synthesized/extrapolated keyframe blend (the stretched-poly bug the Spyro
 * path hit; see spyro.c). prev == NULL (the canonical render + the .s-verify
 * path) keeps the single-pose decode.
 *
 * prev->header = the PREV pose's animation header. WITHIN one clip prev==curr
 * header so count/s5/m_Scale are common. ACROSS a clip switch the prev pose
 * lives in a DIFFERENT keyframe table: the caller (moby_prev_keyframe) only
 * crossfades when both clips share the same vertex COUNT — i.e. they deform one
 * shared mesh, so vertex i is the same physical vertex in both (the
 * fixed-topology argument that makes Spyro's cross-clip lerp safe, applied per
 * moby). The two clips' ShortShift (s5) MAY differ freely: the prev streams
 * decode with prev->header's own s5 (so they reconstruct real model-space
 * coords) while the curr streams use header's; the lerp of two real
 * same-topology model-space poses is always on-model. The prev B stream starts
 * on prev->header_b, keyframe B's own table, exactly as the curr B stream
 * starts on headerB. m_Scale only feeds TR (shared, curr) so a mismatch is
 * irrelevant to the shape lerp. */
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
     * this, models whose m_Scale != 2 mis-project (the multi-moby A/B residual).
     * (a3 = w0&0xFF != 0 records take the GPF-TR rescale below instead.)
     *
     * TR_frac (host-rebuilt path) carries the moby-centre translation at FLOAT
     * precision — the camera/moby delta computed as ÷4.0 instead of >>2, so the
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
     * MAC3>>5 exceeds 63000 (-31500 twice, .L800208A4) — the game draws NOTHING
     * for them beyond that depth. Mirror it or the native draws mobys the game
     * popped out. IR3 is an int16 GTE data reg, so the mtc2 truncates — keep
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
                                    * (prev→curr m_ScaleOverride, reextract_record_pass)
                                    * so the on-screen scale animates at present rate
                                    * instead of stepping per canonical tick */
            double g = (gpf_frac >= 0.0) ? gpf_frac : (double)gpf;
            TR[0] *= g / 32.0;
            TR[1] *= g / 32.0;
            TR[2] *= g / 32.0;
        }
    } else if (gpf) {
        /* .s GPF-TR path [.L8001F870..F8DC; spec twin @80021024]: when the record's
         * low byte is set, the PROJECTION translation is GPF-rescaled —
         *   TRi = (IR0(=w0&0xFF) × int16((TRi<<2)>>m_Scale)) >> 5   (GPF sf=0, then sra 5)
         * scl/32 > 1 pushes the moby AWAY along the view ray, < 1 pulls it in; the
         * class code animates the byte to scale the moby ON SCREEN by moving it in
         * depth (Gnasty-blast dome: the byte ticks down as the blast plays out).
         * Without this the native projected the dome at its TRUE distance — the
         * user-visible giant constant dome. The mtc2 int16 truncation is kept. */
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
     * (slots, 128 world each), not geometry — per-pixel Z over TRUE depth
     * mirrors the per-face OT sort, so the authored slot pull never enters the
     * stored depth (a 512+-world pull here punched mobys through terrain that
     * genuinely occludes them). Its authored job — "win the surface I sit
     * on" — survives as the sub-slot tie pull below, which can only flip
     * pairs the OT calls a tie. */
    static int s_otbias_on = -1;   /* OPENPETE_MOBY_OTBIAS=0 disables the tie pull (A/B) */
    if (s_otbias_on < 0) { const char* e = getenv("OPENPETE_MOBY_OTBIAS"); s_otbias_on = (e && *e) ? (*e != '0') : 1; }
    /* OPENPETE_DEPTHDBG="f0,f1": per-record OT-key inputs (header, w0/dofs,
     * pull, centre) for frames [f0,f1] — the OT-sort parity forensics
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
    if (s_otbias_on) {
        double centre_full = TR_frac
            ? TR_frac[2] * 4.0                       /* full-world = stored x4 */
            : (double)TR_raw[2] * 4.0;
        if (centre_full > 0.0)
            ot_centre = centre_full;
    }
    double ofx = v->ofx, ofy = v->ofy;

    int s5    = (cpu->read_byte(header + game_layout.moby.anim_shift_off) & 0xFF) + 1;
    /* .s tier-adjusted vertex COUNT [@8001F9D4..FA04 / spec twin @80021178]:
     * count = [hdr + 8 + (ZSF4>>31)] — hdr+9 is the LP vertex count — and
     * count == 0 SKIPS the record entirely (bnez → func_80020860 restore +
     * advance). Models with NO LP mesh store 0 there, so beyond the LOD band
     * the game draws NOTHING for them (the distant-gem leak: native read the
     * HP count unconditionally and kept drawing). --no-lod forces the HP
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
    if (count == 0) return 0;
    if (count > cap) count = cap;

    /* Keyframe blend: IR0 = (w0 & 0xFF00) >> 2 (1.0=4096); frame B at frameB.
     * 0 (or B absent) => the no-blend path (frame A only). */
    int blend = (int)((w0 >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask);
    int do_blend = (blend > 0 && moby_pose_live(headerB, frameB));
    double t = (double)blend / 4096.0;

    double rec_minfvz = 1e30;   /* nearest in-front vertex after the pulls (near-floor lift) */
    vstream A, B, Ap, Bp;
    vstream_init(cpu, &A, header, frameA, s5);
    if (do_blend) vstream_init(cpu, &B, headerB, frameB, s5);
    /* Optional prev pose (two-pass sub-tick shape lerp). */
    int blend_p = moby_prev_streams(cpu, prev, &Ap, &Bp);
    int two = blend_p >= 0, do_blend_p = blend_p > 0;
    double tp = (double)blend_p / 4096.0;
    for (int i = 0; i < count; i++) {
        vstream_next(cpu, &A, i == 0);
        double vx = A.VX, vy = A.VY, vz = A.VZ;
        if (do_blend) {
            vstream_next(cpu, &B, i == 0);
            vx += t * (B.VX - A.VX);
            vy += t * (B.VY - A.VY);
            vz += t * (B.VZ - A.VZ);
        }
        if (two) {
            /* curr pose is in (vx,vy,vz); reconstruct the prev pose the same way and
             * lerp prev->curr by blend_alpha (the moby's per-object sub-tick alpha). */
            vstream_next(cpu, &Ap, i == 0);
            double vxp = Ap.VX, vyp = Ap.VY, vzp = Ap.VZ;
            if (do_blend_p) {
                vstream_next(cpu, &Bp, i == 0);
                vxp += tp * (Bp.VX - Ap.VX);
                vyp += tp * (Bp.VY - Ap.VY);
                vzp += tp * (Bp.VZ - Ap.VZ);
            }
            vx = vxp + (vx - vxp) * blend_alpha;
            vy = vyp + (vy - vyp) * blend_alpha;
            vz = vzp + (vz - vzp) * blend_alpha;
        }
        double sx, sy; int sat;
        double vzz = project(M, TR, vx, vy, vz, ofx, ofy, v, &sx, &sy, &sat);
        int unr = moby_gte_near_fixup(M, TR, vx, vy, vz, vzz, ofx, ofy, v, &sx, &sy);
        /* The Z-buffer needs FULL-WORLD view-Z so mobys sort vs terrain / Spyro /
         * each other. project's mac3 is in the m_scale-adjusted world/4 scale
         * (C2_TR = (stored<<2)>>m_scale, stored is world/4), so multiply by
         * 2^m_scale to reach full-world (probe: m_scale=2 mac3 x4 == true cam-Z).
         * Sign-preserving, so emit_wtri's vz<=0 near cull still holds; sx/sy and
         * the saturation flag keep the unscaled GTE-faithful mac3 (vz is depth-only). */
        int ds = m_scale; if (ds < 0) ds = 0; else if (ds > 8) ds = 8;
        out[i].sx = (float)sx; out[i].sy = (float)sy;
        double fvz = (vzz + vz_bias) * (double)(1 << ds);
        /* Fed depth is TRUE per-vertex view-Z. The game's OT is per-FACE —
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
        /* Perspective W = the view-Z the SCREEN PROJECTION divided by, so it
         * excludes both the OT pulls below and the GPF depth restore: sx/sy came
         * from the rescaled TR, and only that vzz makes their UV interpolation
         * consistent. (Any positive multiple works — interpolation sees ratios
         * only — so the 1<<ds normalizer just keeps it in depth units.) */
        out[i].tvz = (float)(vzz * (double)(1 << ds));
        /* rotated vertex offset (M.v)/4096 = viewPos-TR -- the specular env-UV input. */
        out[i].ex = (float)((M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0);
        out[i].ey = (float)((M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0);
        out[i].ez = (float)((M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0);
    }
    /* Near-floor: if the pulls pushed this record past the near plane, lift
     * the WHOLE record uniformly so its nearest vertex sits on the floor —
     * a per-vertex clamp here flattened close-up mobys to one depth and
     * killed their self-occlusion. Uniform lift keeps depth monotonic. */
    if (rec_minfvz < 1.0 && rec_minfvz > -1e29) {
        float lift = (float)(1.0 - rec_minfvz);
        for (int i = 0; i < count; i++) out[i].vz += lift;
    }
    return count;
}

/* The record's ZSF4 LOD/colour-fade tier verdict = high16 of the matrix word
 * @rec+0x2C (renderer: ctc2/cfc2 gte_ctrl[30]); see moby_tier_faceblock.
 * --no-lod: neutralize the DISTANCE term host-side (READ-side twin of the
 * retired game-side zsf4-store hook; idempotent when a store hook did run).
 * Same carve-out as the hook: only when the zero-distance tier is near-band
 * ((md<<8) >= 1024) — sub-near is effect-AUTHORED fade (ring puffs shrink
 * m_Renderer) and keeps the canonical value. */
static int record_zsf4(CPUState* cpu, const moby_record_t* R) {
    int zsf4 = R->tier;
    if (openpete_no_lod && moby_game.renderer_off && R->moby >= 0x80000000u) {
        int md = (int)(cpu->read_byte(R->moby + moby_game.renderer_off) & moby_game.renderer_mask);
        if ((md << 8) >= 1024) zsf4 = md << 8;
    }
    return zsf4;
}

/* Project one live cull record, whose fields the game's record reader already
 * extracted. zsf4 = the record's (--no-lod-adjusted) tier verdict, computed by
 * the caller (record_zsf4) — feeds the tier-adjusted vertex-count read. */
static int moby_project_record(CPUState* cpu, const moby_record_t* R, int zsf4,
                               proj_xy* out, int cap) {
    const ngte_view* v = moby_record_view(R->moby);
    return moby_decode_project(cpu, R->header, R->header_b, R->frame_a, R->frame_b, R->w0,
                               NULL, 0.0, -1.0, R->TR_raw, NULL, R->M,
                               zsf4, v, out, cap);
}

#define MAX_PROJ 32768
static proj_xy s_proj[MAX_PROJ]; static int s_proj_n;

/* Per-record metadata for the independent face-stream walk (3a.2b): where this
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
                                    * level (ApplyFlameHeat*) — consumed by the
                                    * mod-texture face red-shift */
                 int M[3][3];      /* model-to-view rotation, Q12: the per-face
                                    * special material rotates a face normal by
                                    * it */ } rec_meta;
                 /* spec_solo (trailing, set after the positional literal) =
                 * reveal-built record whose moby is in NO .s queue this tick
                 * (beyond canonical range under --lod extended, or off-4:3).
                 * The record is the moby's SOLE feeder, so the walk must apply
                 * the SPEC per-face treatment (env reflection / diffuse
                 * highlight) that the .s SPEC pass would have drawn — the
                 * s_cur_mat=='SP' pass gate alone left revealed metal capes
                 * rendering their dark baked base texture (the cliff_town
                 * caped gnorc). */
                 /* frameA = the record's keyframe (rec+0x08 / f.frameA): the frame
                 * may carry a render-time FACEBLOCK PATCH list (see
                 * moby_faces_for_record) — 0 = no patch lookup.
                 * frameA_p/header_p/clr_alpha = the PREV drawn keyframe + the pose
                 * crossfade alpha (sub-tick re-extract only; 0 = canonical/no prev):
                 * drives the per-face COLOUR crossfade for patch-animated effects,
                 * whose colour indices step per keyframe (see the FCp block in
                 * build_walk_tris_inner). */
                 /* env_az = view-space TR azimuth atan2(TR.z,TR.x) — the
                 * .s SPEC env per-moby u-bias input (.L800226F8 cfc2 TRZ/TRX octant atan) */  /* metal = m_SpecularMetalColor
                 (rec+0x30) high byte (env flag); mscol = full word (highlight BK + spec byte);
                 moby = Moby* (rec+0x34 for .s records; set directly for host-built reveal records,
                 whose addr is then 0 — there is no live +0x1600 record behind them);
                 zsf4 = per-moby colour fade level (high16 of the cull record's matrix word @+0x2C,
                 cfc2 gte_ctrl[30]); see moby_color_fade_ir0 */
static rec_meta s_recs[MAX_RECORDS]; static int s_nrecs;

/* Flame-heat scalar from a moby's m_SpecularMetalColor word. TWO writer
 * encodings exist (moby_helpers.c): ApplyFlameHeat bumps byte2 0..96
 * directly; ApplyFlameHeatExternal (metal chests) packs the whole word
 * (0xa00000 + (scaled+24)<<12 + (24-scaled)<<6 + (24-scaled), scaled =
 * heat>>2, 0..20 — cold silver 0xa18618 decodes to 0, max heat 0xa2c104
 * to 20; probe-verified on live chests). byte2 > 96 identifies the
 * packed form. Returns ~0..96. */
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
 * instead of flickering back to full brightness. Cleared once per canonical frame. */
static struct { uint32_t moby; int zsf4; } s_fade_snap[MAX_RECORDS]; static int s_fade_snap_n;
static void fade_snap_clear(void) { s_fade_snap_n = 0; }
static void fade_snap_put(uint32_t moby, int zsf4) {
    if (moby < 0x80000000u) return;
    for (int i = 0; i < s_fade_snap_n; i++)
        if (s_fade_snap[i].moby == moby) { s_fade_snap[i].zsf4 = zsf4; return; }
    if (s_fade_snap_n < MAX_RECORDS) {
        s_fade_snap[s_fade_snap_n].moby = moby; s_fade_snap[s_fade_snap_n].zsf4 = zsf4; s_fade_snap_n++;
    }
}
static int fade_snap_get(uint32_t moby, int dflt) {
    for (int i = 0; i < s_fade_snap_n; i++) if (s_fade_snap[i].moby == moby) return s_fade_snap[i].zsf4;
    return dflt;   /* snapshot miss: caller picks the safe tier */
}

/* Moby pointers fed by the REGULAR pass this frame — the SPECULAR pass dedups
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
 * g_LevelMobys, but the game also renders mobys that live OUTSIDE that array —
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
     * from before a stall — a CD load or level swap, after which the addresses
     * name whatever now occupies that memory. Reporting none is correct there:
     * nothing was drawn to interpolate. */
    return (openpete_frame - s_canon_drawn_frame <= 2u) ? s_canon_drawn_n : 0;
}

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

/* ---- LOD tier (mirrors func_8001F798 block_8001FDD0; spec twin @80021670) ----
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
 * Under --no-lod the face select goes HP-everywhere NATIVELY (keyed on the
 * flag, not the game-side psx_runtime_moby_lp_face_offset knob — the knob
 * only arms in PsyCross mode now; on the native path the .s producers run
 * canonical and the native walk owns the tier). The zsf4≤0 residue under
 * --no-lod is the effect-AUTHORED fades (m_Renderer&0x3F < 4, exempt from
 * the zsf4 neutralization) — the game-side knob gave those HP faces too,
 * so mirror that. Colours stay tier-faithful in both modes — the game
 * keeps the far palette under --no-lod too. */
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
                              * cull mirror (block_80022B40..90 — the distant-gem hide) */
static long s_tier_mirror_ok, s_tier_mirror_bad;
static long s_hold_replayed;   /* Tier-2 hold: moby replays served per census window */
static int tier_dbg_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_TIER_DBG"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s;
}

/* Extraction µs accumulators (printed in the OPENPETE_MOBY_TIER_DBG census):
 * split the moby extraction cost into its two halves — the vertex decode +
 * projection (proj) and the face-stream walk + tri build (walk) — across ALL
 * callers (canonical pass + per-present re-extract + SPEC), so a caching
 * change is measured by the same instrument before and after. */
static long s_us_proj, s_us_walk, s_us_walk_rex, s_us_calls;
/* Which pass the face walk is running under. The canonical producers walk once
 * per TICK and the sub-tick re-extract walks once per PRESENT; pooled into one
 * accumulator the total is neither quantity and has to be corrected by hand
 * before it can be compared against a per-present scope. */
static int s_walk_in_rex;
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

/* OPENPETE_MOBY_TRACE=<moby addr>: per-call gate trace for ONE moby — the
 * canonical extract logs queue membership, the reveal builder logs every
 * skip/admit verdict with the gate values. Built for the boundary-crossing
 * handoff hunt; env-gated, inert otherwise. */
/* The key the sub-tick interp ring stores a moby's prev/curr snapshots
 * under; the moby address itself where the game supplies no mapping. */
static uint32_t moby_interp_key(uint32_t moby) {
    return moby_game.interp_key ? moby_game.interp_key(moby) : moby;
}

static uint32_t moby_trace_target(void) {
    static uint32_t t = 0xFFFFFFFFu;
    if (t == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_MOBY_TRACE");
                            t = e ? (uint32_t)strtoul(e, NULL, 0) : 0u; }
    return t;
}

/* Project EVERY live record's vertices into s_proj[] (native float decode +
 * RTPS+TR). Shared by the verify A/B and the scene feed. Returns record count. */
static int s_max_blend;   /* max keyframe-blend factor (rec.w0 IR0) across this frame's records */
static int project_all_records_inner(CPUState* cpu);
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
static int special_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SPECIAL_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}
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

static void moby_animsw_census(CPUState* cpu, uint32_t moby);

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
        /* ZSF4 verdict BEFORE the projection — the decode's tier-adjusted
         * vertex-count read ([hdr+8+(ZSF4>>31)], 0 = record skipped) needs it.
         * Snapshotted into fade_snap below, so the sub-tick re-extract
         * inherits the (--no-lod-neutralized) tier too. */
        int zsf4 = record_zsf4(cpu, &R);
        moby_animsw_census(cpu, R.moby);
        int got = moby_project_record(cpu, &R, zsf4, s_proj + s_proj_n, MAX_PROJ - s_proj_n);
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
            fade_snap_put(s_recs[nrec].moby, zsf4);
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
                     * for near-band-at-zero-distance records — mirror that */
                    if (openpete_no_lod && (md << 8) >= 1024) ex = md << 8;
                    if (ex == zsf4) s_tier_mirror_ok++; else s_tier_mirror_bad++;
                }
            }
        }
        s_proj_n += got; nrec++;
        if (s_proj_n >= MAX_PROJ) break;
    }
    s_nrecs = nrec;
    return nrec;
}

/* ---- widescreen live-moby reveal (host-built records) ---------------------
 * In widescreen the .s cull (run headless at canonical 4:3) only writes +0x1600
 * records for 4:3-visible mobys, so enemies/fodder/Sparx/gems pop in at the
 * screen edges. We must NOT re-run the cull (it sets m_WasDrawn — a render->
 * gameplay leak). Instead rebuild the per-moby record FIELDS host-side from
 * gamestate, exactly as func_8001F158 does, for the mobys the 4:3 cull skipped,
 * and decode/project them through moby_decode_project (the same core).
 *
 * Validated against the .s records for in-frustum mobys (OPENPETE_MOBY_BUILDDBG):
 * a host-built record for a moby the .s already culled in must match the .s record
 * field-for-field (M/TR within GTE truncation, header/frameA/frameB/blend exact).
 * Only after that A/B passes is the builder trusted on the revealed (off-4:3) set. */
typedef struct { uint32_t header, header_b, frameA, frameB, w0, mscol; int metal; int TR_raw[3]; double TR_frac[3]; int M[3][3]; } moby_fields;

/* Build the REGULAR (animated) render-record fields for one moby from gamestate.
 * Returns 1 on success, 0 if the moby has no animated model (SimpleModel =>
 * shaded material, handled by the shaded path) or an invalid model/header. */
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
     * cull [@8001F464..F46C] — the render walk's OT near-pull reads it there. */
    f->w0 |= (cpu->read_word(moby + game_layout.moby.angle_off + 0u) >> 24) << 16;
    f->mscol = cpu->read_word(moby + moby_game.spec_colour_off);                 /* m_SpecularMetalColor */
    f->metal = (int)((f->mscol >> 24) & 0xFFu);
    /* M = ProjMatrix . Ry . Rx . Rz(m_Rotation) — the SAME euler compose the shaded
     * path uses (verified bit-exact there), from the moby angle bytes. */
    int PM[3][3]; memcpy(PM, s_view.cam_M, sizeof PM);
    shaded_compose_euler(cpu, PM, moby, f->M);
    /* TR = ProjMatrix . swizzled (cam-moby)>>2  (the cull's centre MVMVA: IR1=(cy-my)>>2,
     * IR2=(cz-mz)>>2, IR3=(mx-cx)>>2; MAC=(PM·IR)>>12). NOT scaled by m_Scale or <<1
     * here — moby_decode_project applies (<<2)>>m_Scale, matching the .s record store. */
    int32_t cx=(int32_t)s_view.cam_ram[0], cy=(int32_t)s_view.cam_ram[1], cz=(int32_t)s_view.cam_ram[2];
    int32_t mx=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u), my=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u), mz=(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    int32_t Vy=(cy-my)>>2, Vz=(cz-mz)>>2, Vx=(mx-cx)>>2;
    /* 64-bit accumulate: the products reach 4096*|V|, so an int32 sum wraps once
     * a component passes ~175 K view units (~700 K world) and the wrapped view-Z
     * reads as behind-camera. Unreachable while a distance ceiling culled first;
     * reachable now that extended LOD has none. Identical to the 32-bit form
     * everywhere it did not overflow, and closer to the GTE, whose MAC
     * accumulator is 44-bit. */
    for (int i=0;i<3;i++) {
        int64_t acc = ((int64_t)PM[i][0]*Vy + (int64_t)PM[i][1]*Vz + (int64_t)PM[i][2]*Vx) >> 12;
        f->TR_raw[i] = (int32_t)(acc > INT32_MAX ? INT32_MAX
                                : (acc < INT32_MIN ? INT32_MIN : acc));
    }
    /* TR_frac: the same view-space centre, FRACTIONAL — delta as ÷4.0 (not >>2) so
     * the moby tracks the smooth sub-tick camera instead of stepping in 4-world-unit
     * chunks (moby analog of the terrain cam>>4 fix). Same scale as TR_raw. The
     * native path projects in float, so this is consumed at full precision; the byte
     * truncation in TR_raw is kept only for the .s-record verify/validate paths.
     * Sub-tick floor fix: prefer the FRACTIONAL interpolated camera + moby
     * positions (un-rounded) over the floored RAM, so neither the camera component
     * nor the moby's own motion steps on the 1-world-unit lerp32 grid. */
    double cfx=s_view.cam[0], cfy=s_view.cam[1], cfz=s_view.cam[2];
    double mfx=(double)mx, mfy=(double)my, mfz=(double)mz;
    int fpos_hit = 0, cam_hit = s_view.cam_interp;
    { double mf[3];
      if (openpete_native_interp_pos(moby,     mf)) { mfx=mf[0]; mfy=mf[1]; mfz=mf[2]; fpos_hit=1; } }
    /* OPENPETE_REG_INTERP_MISS: regular/specular mobys with NO fractional position
     * (not reached by the spine's lerp walk → they step at the sim rate while the
     * camera interpolates around them). The shaded twin is
     * OPENPETE_SHADED_INTERP_MISS. Gated on the CAMERA having a fractional
     * position: that is what marks a sub-tick re-extract, and on the canonical
     * pass (where the resolver is inert by design) every moby would "miss".
     * Rate-limited to a dozen lines a second. */
    {
        static int s_rm = -1;
        if (s_rm < 0) { const char* e = getenv("OPENPETE_REG_INTERP_MISS"); s_rm = (e && *e && *e != '0'); }
        if (s_rm && cam_hit && !fpos_hit) {
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
        f->TR_frac[i] = (PM[i][0]*dVy + PM[i][1]*dVz + PM[i][2]*dVx) / 4096.0;
    return 1;
}

/* TR-only slice of build_moby_record_fields for the Tier-2 hold replay: the
 * origin projection needs the fractional view-space centre (camera MVMVA +
 * interp positions) but NOT the per-moby euler rotation compose — that was
 * most of the held path's residual cost. Callers guarantee the moby extracted
 * as an animated record this epoch, so the model checks are skipped. */
static void build_moby_record_tr(CPUState* cpu, uint32_t moby, double tr[3]) {
    const int (*PM)[3] = s_view.cam_M;
    double cfx=s_view.cam[0], cfy=s_view.cam[1], cfz=s_view.cam[2];
    double mfx=(double)(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u),
           mfy=(double)(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
           mfz=(double)(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    { double mf[3];
      if (openpete_native_interp_pos(moby,     mf)) { mfx=mf[0]; mfy=mf[1]; mfz=mf[2]; } }
    double dVy=(cfy-mfy)/4.0, dVz=(cfz-mfz)/4.0, dVx=(mfx-cfx)/4.0;
    for (int i=0;i<3;i++)
        tr[i] = (PM[i][0]*dVy + PM[i][1]*dVz + PM[i][2]*dVx) / 4096.0;
}

/* Builder A/B (OPENPETE_MOBY_BUILDDBG): for each .s record, rebuild its fields
 * host-side from the moby and compare. A clean match => the builder reproduces the
 * cull, so it is trusted for the off-4:3 reveal set. Prints once at the target. */
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

/* Last reveal set — hysteresis stand-in for host-revealed mobys (they never
 * get m_WasDrawn set, so the .s +0x200 range hysteresis needs a host mirror;
 * membership = revealed on the previous builder call). */
static uint32_t s_rev_prev[MAX_RECORDS]; static int s_rev_prev_n;
static int reveal_prev_has(uint32_t m) {
    for (int i = 0; i < s_rev_prev_n; i++) if (s_rev_prev[i] == m) return 1;
    return 0;
}

/* Run-once mobys: instances whose FIRST update does a one-shot job and then
 * retires them (m_State >= 0x80). The update-list builder (@80052040)
 * auto-includes any moby whose m_WasDrawn is set, ahead of its
 * m_UpdateDistance path, so canonically "drawn" implies "updated next tick"
 * and such an instance is on screen for at most the single tick that admits
 * it — never long enough to be intended as scenery.
 *
 * Two display bugs follow. The extended-LOD reveal must not write m_WasDrawn
 * (gameplay-readable), so a revealed instance is drawn WITHOUT ever entering
 * the update list: it never runs, never retires, and stands there until the .s
 * draw range finally reaches it and kills it. And even in range, the canonical
 * queue still yields that one-tick flash. Both are display-side, so the fix is
 * too: the walk emits no triangles for these and the reveal never admits them,
 * while the .s cull, m_WasDrawn, the update list and the moby's one-shot job
 * all proceed untouched.
 *
 * Identity is the level author's own spawn parameter: m_Props (+0x00) points
 * at a per-instance block whose word at +0x04 selects the class's behaviour
 * variant. Jacques' fools ship four instances at variant 6 — the ones that
 * only set a moving platform's height — against eight at variants 0/1/2, the
 * ordinary hopping fools. Nothing in the Moby record itself distinguishes
 * them: an ordinary fool sits at m_State 0 with a zero rotation matrix and
 * animation frame 0 exactly like an unreached run-once one, and STAYS at
 * m_State 0 after being drawn, so a liveness test would hide every distant
 * fool in the level. The table is keyed per level because class ids index the
 * level overlay's own g_Models, and the props layout is class-specific. */
static int moby_run_once_hidden(CPUState* cpu, uint32_t m, unsigned cls) {
    return moby_game.run_once_hidden ? moby_game.run_once_hidden(cpu, m, cls) : 0;
}

/* Append host-built records for mobys the canonical 4:3 cull skipped but that
 * the retired producer widenings WOULD have drawn. Dedups against the .s queue
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
static int append_revealed_regular_inner(CPUState* cpu);
static int append_revealed_regular(CPUState* cpu) {
    if (!tier_dbg_on()) return append_revealed_regular_inner(cpu);
    uint64_t t0 = openpete_phase_now_us();
    int n = append_revealed_regular_inner(cpu);
    s_us_reveal += (long)(openpete_phase_now_us() - t0);
    return n;
}
static int append_revealed_regular_inner(CPUState* cpu) {
    /* The reveal rebuilds cull records in the game's own encoding, so it runs
     * only where the game's provider supplies that builder. Without it the walk
     * feeds exactly the records the cull queued — which is also what a game
     * whose cull writes a gameplay-readable verdict needs, since nothing the
     * host widens may reach that cull. */
    if (!moby_game.host_reveal) return 0;
    /* Two reveal modes, composable — each mirrors a retired game-side
     * producer widening, so admission is deliberately liveness-BLIND
     * (the .s cull never tested liveness; the canonical PSX draws
     * out-of-update-disk mobys STALE-POSED all the time — drawing is
     * camera-cull, animation is Spyro's update disk. The live-only v1
     * of this reveal culled every static prop/chest/idle enemy at the
     * 4:3 boundary):
     *  - EXTENDED VIEW (Hor+ or a wider FOV): off-4:3 mobys the canonical
     *    4:3 X-cone skipped (parity with the retired cone widening).
     *  - --no-lod (native display policy; the game-side producer stack
     *    is PsyCross-only now): mobys BEYOND the canonical draw range —
     *    parity with the retired m_Distance saturation. Ceiling = the
     *    same 0x7F<<8 view-Z the saturated compare admitted. */
    int nl = openpete_no_lod;
    if (!s_view.extended && !nl) return 0;              /* canonical view, LOD faithful — nothing to reveal */
    /* While the rescue cutscene owns the draw it renders its own actor set and
     * no level geometry, so the widened view must add none either. Revealing
     * only the off-window mobys put props in the side strips while the same
     * props inside the 4:3 window were ceded to a queue that does not draw
     * them — present at the edges, absent from the centre of one scene. The
     * cutscene actors are excluded by the same rule: they are drawn by the
     * cutscene renderer, are depth-punched as ACTOR_FRONT, and their radius
     * exceeds their view-Z through most of the cutscene, so a revealed copy
     * straddles the camera plane and has no valid projection. Applies at every
     * LOD — extended LOD reaches the cutscene through this builder too. */
    if (moby_cutscene_owns_draw(cpu)) return 0;
    float xlo = s_view.xlo, xhi = s_view.xhi;
    double ofx = s_view.ofx;
    double H   = s_view.H_eff;
    /* OPENPETE_REVEAL_DBG=frame — reveal-admission forensics: dumps every
     * builder candidate near the window edge (cls/state/rad/rz/sx/bok) on
     * the target frame, so a missing edge moby can be attributed to its
     * skip gate without a rebuild (named the cls-0xDF radius-margin tree
     * and the box-vs-radial range mismatch). */
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
        /* dedup vs every REG-handled moby (s_reg_moby: the .s queue set, plus —
         * on the sub-tick re-extract — occl-culled and hold-replayed mobys, so
         * a held off-4:3 speck is never double-drawn by the reveal) */
        int dup = 0;
        for (int k = 0; k < s_reg_moby_n; k++) if (s_reg_moby[k] == m) { dup = 1; break; }
        if (dup) { TRC("skip=REG-dedup (queued/culled/replayed this pass)"); m += game_layout.moby.array_stride; continue; }
        /* SPECULAR-queue mobys are fed by the SPEC pass (its cone cull is not
         * 4:3 — the native walk draws the whole queue), WITH the per-face
         * specular treatment the REG walk lacks. Revealing them here too drew
         * a second coplanar copy without env/highlight faces that z-fought the
         * SPEC copy (the cliff-town armored gnorc's shield rendered teal).
         * The live +0x1600 queue is CLOBBERED at REG-render time (the scratch
         * is reused mid-frame; the spec cull rebuilds it later), so test the
         * snapshot taken at the last SPEC pass — 1 tick stale at worst. The
         * VERDICT is deferred below to the admit_canon gate: on the tick a
         * spec-queued moby crosses OUT of canonical draw range, the stale
         * snapshot still lists it while this tick's spec queue (canonical
         * cull ⊆ canonical range) provably does not — trusting the snapshot
         * there fed the moby from NEITHER pass for exactly one frame (the
         * boundary-crossing flicker; drift-harness
         * trace: IN-QUEUE @f, skip=SPEC-dedup @f+1, REVEALED @f+2). While
         * the moby is still canonically in range (admit_canon) the snapshot
         * stays trusted — the z-fight protection is unchanged there. */
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
        double rx = (double)f.TR_raw[0], rz = (double)f.TR_raw[2];           /* origin in view space (world/4) */
        /* per-moby radius = AnimationHeader byte[7]<<4 (view units) — the .s
         * cone's X-bias + its behind-camera reject both use it (@8001F2A0) */
        double rr = (double)((int)cpu->read_byte(f.header + game_layout.moby.anim_radius) << 4);
        if (rz + rr <= 0.0) { TRC("skip=behind-cam rz=%.0f rr=%.0f", rz, rr); m += game_layout.moby.array_stride; continue; }  /* behind camera (.s: MAC3 + r <= 0) */
        /* Distance admission = the .s cull's EXACT 3-axis box test (F158
         * @8001F1F8..F260; the earlier radial view-Z mirror was strictly
         * narrower and dropped far-off-axis mobys the .s draws — the
         * "culled outside 4:3" tree):
         *   t4 = (m_RenderRadius << 8) + (m_WasDrawn & 1 ? 0x200 : 0)
         *   admit iff every axis: -t4 < (moby[i]-cam[i])>>2 < t4
         * FULL byte (bit7 included, exactly like the lhu path); dist==0
         * needs no special sentinel — its box is degenerate and never
         * admits, same as the .s. Hysteresis: canonical mobys carry live
         * m_WasDrawn; host-revealed mobys never get the byte set, so
         * membership in the LAST reveal set stands in for it (same
         * pop-flicker damping the .s got from the widened render's byte).
         * --no-lod: the retired game-side hook saturated the LOW 7 bits to
         * 0x7F when nonzero (bit7 + sentinel preserved) — mirror that on
         * the effective threshold. */
        uint8_t rad_byte = cpu->read_byte(m + moby_game.render_radius_off);
        /* Hysteresis, split per consumer. The .s cull widens by 0x200 keyed
         * ONLY on the moby's live m_WasDrawn bit, so the canonical mirror
         * (t4c / admit_canon) must use exactly that — mixing in reveal_prev
         * made "what the .s would admit" depend on what the HOST drew: a
         * boundary moby inside the band (t4, t4+0x200) with m_WasDrawn clear
         * was revealed (hyst 0), which set reveal_prev, which widened t4c,
         * which flipped admit_canon true, which ceded it to a .s queue that
         * never admits it — one dark frame, reveal_prev cleared, repeat.
         * Period-2 strobe at exactly the --lod original cull distance (the
         * artisans tower flag). reveal_prev stands in for the
         * byte only on the reveal's OWN threshold t4e (host-revealed mobys
         * never get m_WasDrawn set; t4e is where the .s's pop-damping needs
         * a mirror — the widescreen edge / --no-lod ceiling). */
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
         * high enough above a level clears outright — every moby then failed the
         * box while terrain (a PVS sector list with no distance term) kept
         * drawing. m_Distance == 0 keeps its canonical meaning: the dormant /
         * unpopulated slot sentinel, whose degenerate box admits nothing. */
        uint32_t eff = rad_byte;
        int32_t t4e;
        if (nl && (eff & 0x7Fu)) t4e = INT32_MAX;                    /* no ceiling */
        else                     t4e = ((int32_t)eff << 8) + hyst;
        #define REVEAL_BOX(t) (d0 > -(t) && d0 < (t) && d1 > -(t) && d1 < (t) && d2 > -(t) && d2 < (t))
        /* the .s pairs the box test with a view-Z cap on the SAME threshold
         * (@8001F2E8: reject MAC3 >= t4) — mirror both or far-axis mobys the
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
        /* Radius margin: the .s cone admits by CENTRE ± radius bias
         * (4(|x| - 25r/32) < 3(z + 19r/32)), so a wide prop whose centroid
         * sits past the window edge but whose body pokes in is still drawn
         * (the edge tree, cls 0xDF, centroid sx=-104 at
         * xlo=-85). Mirror it as a projected-radius margin on the bounds. */
        double margin = (25.0 * rr / 32.0) * H / rz;
        /* central is tested at the CANONICAL projection: the .s cone is wider
         * than the 4:3 screen by the radius bias, so no canonically-central
         * moby is cone-rejected; a moby central only in a wider view can be
         * cone-rejected and must not be ceded to a queue that does not hold
         * it. The window test below uses the effective projection. */
        int central = (sxc >= 0.0 && sxc <= 512.0);
        /* central + canonically-admitted = the .s queue's territory (anything
         * there and not in the queue was cull-rejected for a non-box reason —
         * leave it); central + beyond-canonical is the no-lod clause's NEW
         * coverage. */
        if (central && admit_canon) { TRC("skip=ceded-to-canon sx=%.0f", sx); m += game_layout.moby.array_stride; continue; }
        if (sx < (double)xlo - margin || sx > (double)xhi + margin) { TRC("skip=off-window sx=%.0f", sx); m += game_layout.moby.array_stride; continue; }  /* outside widened view (+radius) */
        /* Occlusion cull, exactly as the queue pass tests its own records: a
         * moby behind nearer world-opaque depth costs a full decode + walk for
         * pixels the depth test erases. Placed after the admission tests above
         * so the verdict below is "admitted, but hidden" — extended LOD removes
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
                if (occl_rev_n < MAX_RECORDS) occl_rev[occl_rev_n++] = m;
                TRC("skip=occluded");
                m += game_layout.moby.array_stride; continue;
            }
        }
        s_occl_seen++;
        if (s_nrecs >= MAX_RECORDS || s_proj_n >= MAX_PROJ) break;
        int off = s_proj_n;
        /* ZSF4 mirrored from the cull's formula (@8001F404) — there is no live
         * record behind a revealed moby to read the verdict from. Validated by
         * the OPENPETE_MOBY_TIER_DBG mirror check on the canonical records.
         * Computed BEFORE the decode: the tier-adjusted vertex-count read
         * ([hdr+8+(ZSF4>>31)], 0 = skip) hides far no-LP-mesh mobys (gems). */
        int rmd   = (int)(cpu->read_byte(m + moby_game.renderer_off) & moby_game.renderer_mask);
        int rzsf4 = (int)(int16_t)(uint16_t)((((uint32_t)rmd << 8)
                    - (uint32_t)f.TR_raw[2]) & 0xFFFFu);
        /* --no-lod parity with the game-side zsf4-store hook: neutralize the
         * distance term for records that are near-band at zero distance */
        if (openpete_no_lod && (rmd << 8) >= 1024) rzsf4 = rmd << 8;
        else if (openpete_no_lod) {
            /* Sub-near (effect-authored fade) records keep the distance term —
             * but the .s holds the verdict in an int16, which WRAPS once view-Z
             * passes 32767. Canonically unreachable (nothing is drawn that far),
             * yet live now that the reveal has no distance ceiling: a wrapped
             * verdict re-enters the 0 < ZSF4 < 1024 fog band and re-fades a moby
             * that is unambiguously far-tier, cycling every 65536 view units.
             * Saturate instead. */
            int v = (rmd << 8) - (int)f.TR_raw[2];
            rzsf4 = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
        }
        int got;
        { uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          got = moby_decode_project(cpu, f.header, f.header_b, f.frameA, f.frameB, f.w0, NULL, 0.0, -1.0, f.TR_raw, f.TR_frac, f.M,
                                    rzsf4, moby_record_view(m), s_proj + s_proj_n, MAX_PROJ - s_proj_n);
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
        s_recs[s_nrecs].spec_solo = 1;   /* sole feeder: walk applies the SPEC face treatment */
        fade_snap_put(m, rzsf4);
        if (revealed < MAX_RECORDS) revlist[revealed] = m;
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
    /* Publish this call's ADMITTED set (drawn + occlusion-culled) as the next
     * call's hysteresis set — the widening mirrors what the .s cull would have
     * admitted, which the display-side cull must not perturb. */
    {
        int hn = (revealed < MAX_RECORDS) ? revealed : MAX_RECORDS;
        memcpy(s_rev_prev, revlist, (size_t)hn * sizeof(uint32_t));
        for (int k = 0; k < occl_rev_n && hn < MAX_RECORDS; k++) s_rev_prev[hn++] = occl_rev[k];
        s_rev_prev_n = hn;
    }
    static int s_logged = 0;
    if (!s_logged && revealed > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-reveal-feed] frame=%u F=%.3f s=%.3f host-built %d revealed regular mobys "
                "(off-4:3, live) appended to the native scene\n", openpete_frame, s_view.F, s_view.s, revealed);
    }
    /* Admitted, not drawn: a present whose every reveal was occlusion-culled
     * still exercised this producer, and the caller's render-path mark keys off
     * the return — reporting 0 there would retire the sub-tick moby passes for
     * as long as the wall stands in front of them. */
    return revealed + occl_rev_n;
}

/* ---- extended-LOD reveal for a game with its own record encoding ---------
 * The reveal above composes record fields and hands them to the decode, which
 * works only where the fields carry this project's Spyro 1 shapes: a keyframe
 * reference that is a pointer, a translation the draw still has to rescale, a
 * draw radius in the byte moby_game.render_radius_off names. A game whose cull
 * writes another encoding supplies two slots instead — the cull's own box
 * threshold and a record builder for its own queue — and this walk drives
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
     * component passes ~175 K view units — unreachable while a distance
     * ceiling culled first, reachable now that extended LOD has none. */
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

int moby_reveal_candidates(CPUState* cpu, moby_reveal_kind_t kind) {
    long seen = 0, canon = 0, occl = 0;
    long declined[MOBY_REVEAL_REASONS]; memset(declined, 0, sizeof declined);
    const char* kname = (kind == MOBY_REVEAL_ANIMATED) ? "anim" : "simple";
    if (!openpete_no_lod) return 0;          /* the extended-LOD clause only */
    if (!moby_game.cull_box_threshold || !moby_game.reveal_record) return 0;
    const uint32_t stride = game_layout.moby.array_stride;
    int PM[3][3]; memcpy(PM, s_view.cam_M, sizeof PM);
    const int32_t cx = (int32_t)s_view.cam_ram[0],
                  cy = (int32_t)s_view.cam_ram[1],
                  cz = (int32_t)s_view.cam_ram[2];
    int added = 0, guard = 0;
    uint32_t m = moby_array_head(cpu);
    while (m >= 0x80000000u && guard++ < 4096) {
        const uint8_t stt = cpu->read_byte(m + game_layout.moby.state_off);
        if (stt == 0xFFu) break;                            /* array sentinel */
        if (stt >= 0x80u) { m += stride; continue; }        /* dead / inactive */
        if (moby_run_once_hidden(cpu, m,
                (unsigned)cpu->read_half(m + game_layout.moby.class_off))) {
            m += stride; continue;
        }
        seen++;
        /* The cull's own threshold, read with the moby's live verdict byte:
         * inside the box is the cull's territory, and a moby there that is not
         * in the queue was rejected for a reason the box does not express. */
        const int t = moby_game.cull_box_threshold(cpu, m,
                          cpu->read_byte(m + game_layout.moby.drawn_off));
        if (t <= 0) { m += stride; continue; }   /* degenerate box: dormant slot */
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
        if (inbox) { canon++; m += stride; continue; }
        if (rz <= 0) { m += stride; continue; }   /* behind the camera plane */
        if (occl_record_culled(cpu, m)) { occl++; m += stride; continue; }
        moby_reveal_reason_t why = MOBY_REVEAL_OK;
        if (moby_game.reveal_record(cpu, m, kind, &why)) added++;
        else declined[why]++;
        m += stride;
    }
    if (moby_verify_enabled())
        fprintf(stderr, "[moby-reveal vbl=%u] kind=%s live=%ld canon=%ld occluded=%ld added=%d "
                        "declined{nomodel=%ld otherkind=%ld dup=%ld nopose=%ld full=%ld}\n",
                openpete_frame, kname, seen, canon, occl, added,
                declined[MOBY_REVEAL_NO_MODEL], declined[MOBY_REVEAL_OTHER_KIND],
                declined[MOBY_REVEAL_DUPLICATE], declined[MOBY_REVEAL_NO_POSE],
                declined[MOBY_REVEAL_QUEUE_FULL]);
    return added;
}

/* ---- native scene triangle (fed to panel 0) ------------------------------
 * Filled by the 3a.2b independent face-stream walk (below). The 3a.2a
 * nearest-neighbour prim scaffold it replaced was removed when the walk was
 * promoted; the walk is jitter-free (exact word0 indices) where
 * the scaffold's screen-proximity matching jittered. */
typedef struct { float sx, sy, vz; int u, v; uint32_t rgb;
                 float ex, ey, ez; float tvz; } ntri_vert;  /* vz=depth; u,v=texel; ex/ey/ez=env-UV input (#1);
                 tvz = the same vertex's UNSHIFTED view-Z, the perspective W (see
                 openpete_scene_vert.pvz): vz carries the OT-slot pull, which can
                 reach 16256 world units on a saturated authored bias. */
typedef struct {
    ntri_vert v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp;  /* PSX material (VRAM px) */
    int blend;   /* 0 = opaque, 1 = semi-transparent (prim ABE bit) */
} ntri_t;
#define MAX_NTRI (MAX_PRIMS * 2)              /* GT4 quad -> 2 tris */

/* ---- 3a.2b: INDEPENDENT gamestate face-stream walk -----------------------
 * Derive triangle TOPOLOGY from the moby face stream itself (not the captured
 * prims). Each face's word0 packs three stream-vertex indices (byte offsets
 * into the per-record SXY scratchpad; >>2 = the s_proj index); textured faces
 * bake a Tiledef at fp-0xC/-0x8/-0x4; per-vertex colour comes from the fog
 * scratchpad (g_SonyImage+0x1100), word0-indexed. This replaces the 3a.2a
 * nearest-neighbour scaffold whose screen-proximity matching was the measured
 * cause of the 55%-NN-ambiguous jitter -- here connectivity is exact.
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
                                        * intact — the --lod extended leak fix. */
/* Per-record emit tags. Thread-local because the face walk fans records out
 * across the extraction worker pool: each worker holds the tags of the record
 * it is walking, and without that separation workers stamp their triangles with
 * each other's moby identity, HUD routing and OT bias. */
static _Thread_local int     s_cur_ui;      /* set per-record by the walks before emitting its tris */
static _Thread_local int     s_cur_front;   /* per-record: 1 = a cutscene actor (front-pass) */

/* The cutscene actor set: the mobys a cutscene renderer of the game's own
 * draws while it owns the draw. Matching a moby pointer against the game's own
 * selector is exact — no class or heuristic — and a game with no such renderer
 * supplies neither slot, so every moby is an ordinary actor. */
static int moby_is_cutscene_actor(CPUState* cpu, uint32_t mob) {
    if (mob < 0x80000000u || !moby_game.cutscene_actor) return 0;
    return moby_game.cutscene_actor(cpu, mob);
}

/* True while that renderer owns the draw: it renders its own actor set and no
 * level geometry, so the host-side reveal must add nothing either. */
static int moby_cutscene_owns_draw(CPUState* cpu) {
    return moby_game.cutscene_owns_draw ? moby_game.cutscene_owns_draw(cpu) : 0;
}

/* Render-time-animated colour cache (the boundary-barrier shimmer). Identity of
 * each emitted walk tri so the canonical frame can snapshot the .s's transient
 * per-vertex colour by FACE (moby pointer + topology vertex-index triple) and the
 * high-FPS sub-tick re-extract can REPLAY it by identity — instead of re-matching
 * by screen position every present. The old screen-space sub-tick match was both
 * O(walk×prim) per present (the 480→60fps moby regression) and flicker-prone
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
                                          * projection — never a key emitter */
static uint32_t s_wtri_fkey[MAX_NTRI]; /* packed SXY vertex-index triple (face topology id) */
static uint16_t s_wtri_cls[MAX_NTRI];  /* m_Class of the emitting moby (provenance tag) */
/* Which per-face special material produced this tri's colours, 0 = none: the
 * colour A/B prices those faces against the prims separately, because they are
 * the only ones whose colour the walk computes rather than reads. */
static uint8_t  s_wtri_special[MAX_NTRI];
static _Thread_local uint32_t s_cur_moby_key;   /* moby for the record currently being walked */
static _Thread_local uint16_t s_cur_moby_cls;   /* m_Class of that moby (set beside the key) */
static _Thread_local int8_t   s_cur_moby_dofs;  /* authored OT bias (set beside the key) */
static _Thread_local float    s_cur_env_az;     /* current record's TR azimuth (env-UV bias) */
static _Thread_local uint32_t s_emit_fkey;      /* packed index triple for the next emit_wtri */
static _Thread_local uint8_t  s_emit_special;   /* special-material id for the next emit_wtri */
#define PACK_FKEY(a,b,c) (((uint32_t)(a)&0x3FFu) | (((uint32_t)(b)&0x3FFu)<<10) | (((uint32_t)(c)&0x3FFu)<<20))

/* signed 2*area of a screen-space triangle (NCLIP analog) -- backface/winding. */
static double tri_nclip(float ax,float ay,float bx,float by,float cx,float cy) {
    return (double)(bx-ax)*(cy-ay) - (double)(cx-ax)*(by-ay);
}

/* The four decode pieces the player feeder shares with this walk; see
 * renderer/moby.h for what each one is. */
double moby_tri_nclip(float ax, float ay, float bx, float by, float cx, float cy) {
    return tri_nclip(ax, ay, bx, by, cx, cy);
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
    int n, cap;
} mn_out;
static mn_out s_gout = { s_wtri, s_wtri_fkey, s_wtri_moby, s_wtri_cls, s_wtri_ui,
                         s_wtri_front, s_wtri_mangle, s_wtri_dofs, s_wtri_special,
                         0, MAX_NTRI };
static _Thread_local mn_out* t_out = &s_gout;

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
     * — e.g. the gem value-popup the player charges THROUGH — renders inverted
     * ("upside down") at screen top. Matches the .s (it draws nothing for a
     * fully-behind moby). Cheap whole-triangle cull (no near-plane clipping):
     * mobys rarely straddle the near plane, and a 1-frame early disappear beats
     * a flipped explosion. */
    if (a->vz <= 0.0f || b->vz <= 0.0f || c->vz <= 0.0f) return;
    /* NEAR-PLANE STRADDLE: a face with a vertex inside the GTE divide-overflow
     * regime got that vertex's screen position from the hardware-clamped path
     * (moby_gte_near_fixup), so it is pinned to the ±1024 SXY window rather
     * than the true perspective point — the face's shape is no longer the one
     * the camera sees. The GPU is what resolves this on hardware: it discards
     * any primitive spanning more than 1023 px horizontally or 511 vertically,
     * which is exactly the class the clamp produces. Mirror that and the
     * straddling face drops, as it does on PSX; without it the face survives
     * with one corner slammed to the window edge and sweeps the frame (mobys
     * the camera passes through — the 1-frame whole-screen flash).
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
    O->cls[O->n] = s_cur_moby_cls;            /* provenance tag (mod-shaders S6) */
    ntri_t* t = &O->tri[O->n++];
    t->v[0]=(ntri_vert){a->sx,a->sy,a->vz,ua,va,ca, a->ex,a->ey,a->ez, a->tvz};
    t->v[1]=(ntri_vert){b->sx,b->sy,b->vz,ub,vb,cb, b->ex,b->ey,b->ez, b->tvz};
    t->v[2]=(ntri_vert){c->sx,c->sy,c->vz,uc,vc,cc, c->ex,c->ey,c->ez, c->tvz};
    t->textured=tex;t->tpage_x=tpx;t->tpage_y=tpy;t->clut_x=clx;t->clut_y=cly;t->bpp=bpp;t->blend=blend;
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
    s_cur_ui    = openpete_ui_moby_is_addr_static(moby) ? 2
                : openpete_ui_moby_is_addr_ui(moby)     ? 1 : 0;
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

void moby_simple_tri(const double p[3][3], const uint32_t rgb[3],
                     const int u[3], const int v[3],
                     int textured, int tpage_x, int tpage_y,
                     int clut_x, int clut_y, int bpp, int blend,
                     uint32_t fkey) {
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
    }
    s_emit_fkey = fkey;
    s_emit_special = 0u;
    emit_wtri(&q[0], &q[1], &q[2], rgb[0], rgb[1], rgb[2],
              u[0], v[0], u[1], v[1], u[2], v[2],
              textured, tpage_x, tpage_y, clut_x, clut_y, bpp, blend);
}

/* specular env-reflection UV (#1) -- definitions below (near recolor); forward-
 * declared so the walk can derive metal-face UVs natively. s_env_* = the static
 * metal Tiledef (D_800756F0, level-load asset), decoded to VRAM px by _load. */
static void specular_env_load(CPUState* cpu);
static void specular_env_uv(float ex, float ey, float ez, int* pu, int* pv);
static int  s_env_tpx, s_env_tpy, s_env_clx, s_env_cly, s_env_bpp;
static float s_specL[3], s_specLlen;   /* D_800770C8+0xC specular light dir (highlight) */

/* GTE DPCS (depth-cue) on one 8-bit colour channel toward a 12-bit far colour by
 * IR0/4096. Bit-exact mirror of the runner's color_depth_cue_finish (gte.cpp):
 *   mac    = c<<16
 *   ir_d   = sat16(((fc<<12) - mac) >> 12)
 *   total  = mac + IR0*ir_d
 *   out    = sat8((total>>12) >> 4)
 * Verified vs the live GTE: dpcs_ch(255, 2032, 1312) == 214. */
static int dpcs_ch(int c8, int fc12, int ir0) {
    return ngte_dpcs_ch(c8, fc12, ir0);   /* shared bit-exact GTE mirror */
}

/* FOG-band colour fade (the .s moby colour pass, block_8001FDD0/.L8001FDE8).
 * The cull record overloads the high 16 bits of its matrix word at +0x2C as the
 * per-record LOD tier ZSF4 (cfc2 gte_ctrl[30]) — full ladder documented at
 * moby_tier_faceblock. This helper handles ONLY the middle band:
 *   0 < ZSF4 < 1024 -> DPCS toward the far palette (header+0x20) by
 *                      IR0 = (1024-ZSF4)*4
 * ZSF4 >= 1024 (near, raw colour) and ZSF4 <= 0 (LP band — the far palette
 * itself + cyclorama-fog fade) are the caller's tier select in build_walk_tris.
 * Effect mobys (e.g. the dream-weavers fireball's ring puffs, cls 0x77) drop
 * ZSF4 1024->0 over their last frames, so they FADE OUT instead of popping.
 * Returns IR0 (0 = no fade) and the far-colour base. zsf4 is the per-moby tier
 * (rec+0x2C high16 for canonical records, the snapshot for sub-tick records). */
static int moby_color_fade_ir0(CPUState* cpu, int zsf4, uint32_t header, uint32_t* farbase) {
    *farbase = 0;
    if (header < 0x80000000u) return 0;
    if (zsf4 <= 0 || zsf4 >= 1024) return 0;
    uint32_t fb = cpu->read_word(header + ANIM_COL_FAR);
    if (fb < 0x80000000u) return 0;
    *farbase = fb;
    return (1024 - zsf4) * 4;
}

/* Read a per-vertex moby colour (cbase + byte-offset ci), applying the DPCS fade
 * when ir0 != 0. Far colour: per-vertex 16-bit packed entry at farbase+ci (the
 * fog band, .s 8001FE14-FE2C) — or, when fcc != NULL, a CONSTANT 12-bit-expanded
 * colour (the LP band fades every vertex toward the one cyclorama fog colour,
 * .s 8001FEE4-FF08). */
static uint32_t moby_color_at(CPUState* cpu, uint32_t cbase, uint32_t farbase,
                              const int* fcc, int ir0, uint32_t ci) {
    uint32_t col = cpu->read_word(cbase + ci) & 0xFFFFFFu;
    if (!ir0) return col;
    int r = (int)(col & 0xFFu), g = (int)((col >> 8) & 0xFFu), b = (int)((col >> 16) & 0xFFu);
    int fcr, fcg, fcb;
    if (fcc) { fcr = fcc[0]; fcg = fcc[1]; fcb = fcc[2]; }
    else {
        uint32_t fw = cpu->read_word(farbase + ci);
        fcr = (int)((fw << 4) & 0xFF0u); fcg = (int)((fw >> 4) & 0xFF0u); fcb = (int)((fw >> 12) & 0xFF0u);
    }
    r = dpcs_ch(r, fcr, ir0); g = dpcs_ch(g, fcg, ir0); b = dpcs_ch(b, fcb, ir0);
    return (uint32_t)(r | (g << 8) | (b << 16));
}

/* Host-pointer variant: identical maths, but the colour bases are resolved to
 * host memory ONCE per record instead of a cpu->read_word memory-map dispatch
 * per vertex (3-4 colour lookups per face — the walk's hottest remaining reads
 * after the Tier-1 face cache). PSX RAM and the host are both little-endian;
 * memcpy keeps the loads alignment-safe. Callers fall back to moby_color_at
 * when a base doesn't resolve. */
static inline uint32_t le32_at(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t moby_color_at_host(const uint8_t* cb, const uint8_t* fbp,
                                   const int* fcc, int ir0, uint32_t ci) {
    uint32_t col = le32_at(cb + ci) & 0xFFFFFFu;
    if (!ir0) return col;
    int r = (int)(col & 0xFFu), g = (int)((col >> 8) & 0xFFu), b = (int)((col >> 16) & 0xFFu);
    int fcr, fcg, fcb;
    if (fcc) { fcr = fcc[0]; fcg = fcc[1]; fcb = fcc[2]; }
    else {
        uint32_t fw = le32_at(fbp + ci);
        fcr = (int)((fw << 4) & 0xFF0u); fcg = (int)((fw >> 4) & 0xFF0u); fcb = (int)((fw >> 12) & 0xFF0u);
    }
    r = dpcs_ch(r, fcr, ir0); g = dpcs_ch(g, fcg, ir0); b = dpcs_ch(b, fcb, ir0);
    return (uint32_t)(r | (g << 8) | (b << 16));
}

/* ---- Tier-1 face-decode cache (one entry per faceblock) -------------------
 * Everything the walk parses OUT OF THE FACE STREAM is static per faceblock:
 * vertex/colour indices, flags, Tiledef UV/tpage/CLUT/ABR. Only the projected
 * vertices, the colour VALUES and the view-dependent spec env-UV/highlight
 * change per present. The walk used to re-parse every face word through
 * cpu->read_word on EVERY pass (canonical + per-present re-extract + SPEC) —
 * the dominant half of extraction (baseline census at the --no-lod attract
 * vista: walk 3847us/s vs proj 1484us/s). Decode once per faceblock and
 * iterate the decoded array; keyed by vaddr + bytecount + a 3-word content
 * signature so a level load (same vaddr, new model data) re-decodes. Slot
 * collision = evict + re-decode (never lies). Display-only: reads static
 * model RAM; the per-present half (NCLIP emit tree, colours, env, blend map)
 * is byte-identical to the previous inline parse. */
/* One decoded face: renderer/moby_game.h owns the layout, since the game's
 * face decoder fills it. */
typedef moby_wface_t wface_t;
typedef struct {
    uint32_t fb, bytecount, sig1, sig2, sig3;
    int      nfaces;              /* emittable faces in f[] */
    int      raw, sprites, skips; /* walkdiag bookkeeping (raw = total stream faces) */
    uint8_t  align;               /* 0 = align_lo, 1 = align_hi, 2 = desync */
    uint32_t gen;                 /* resolve generation that last handed this entry out */
    wface_t* f;
} fcache_t;
#define FC_SLOTS 512
static fcache_t s_fcache[FC_SLOTS];
static long s_fc_hit, s_fc_miss;      /* tier-census counters */
static wface_t s_fc_tmp[8192];        /* decode scratch (guard cap matches the old walk) */

/* ---- resolve generation -------------------------------------------------
 * The record prologue hands out an fcache_t* per record and the walk reads
 * them all afterwards, so an entry a record already holds may not be evicted
 * before that walk ends. Each resolve pass takes a generation; every entry it
 * returns is stamped with it, and an entry stamped with the current generation
 * is not a victim. When both ways of a hash pair are stamped, the decode goes to
 * a private overflow entry instead — outside the cache, so it evicts nothing
 * and is dropped when the next pass reuses the slot. Without this a third
 * faceblock on one hash pair silently rewrites an entry an earlier record
 * points at, and that record then walks another model's faces and colours. */
#define FC_OVF_SLOTS 64
static fcache_t s_fc_ovf[FC_OVF_SLOTS];
static int      s_fc_ovf_n;                  /* overflow entries taken this pass */
static uint32_t s_fc_gen = 1;                /* current resolve generation */
static long     s_fc_ovf_used, s_fc_ovf_full;  /* engagement census */

/* Opening a pass retires the previous one: every entry still carries the older
 * generation, so none of them is held any more. */
static void fcache_pass_begin(void) { s_fc_gen++; s_fc_ovf_n = 0; }
static int  fcache_pinned(const fcache_t* C) { return C->gen == s_fc_gen; }
/* The next free overflow entry, or NULL when the pass has exhausted them. */
static fcache_t* fcache_ovf_take(void) {
    if (s_fc_ovf_n >= FC_OVF_SLOTS) { s_fc_ovf_full++; return NULL; }
    s_fc_ovf_used++;
    return &s_fc_ovf[s_fc_ovf_n++];
}

/* Where a missing faceblock may be decoded, given the two ways of its hash
 * pair: an empty way, then a way no open resolve pass holds, then a private
 * overflow entry. NULL = nothing free (overflow exhausted): the caller drops
 * the decode rather than rewriting an entry a record already points at. */
static fcache_t* fcache_victim(fcache_t* C, fcache_t* C2) {
    if (!C->f)  return C;
    if (!C2->f) return C2;
    if (!fcache_pinned(C))  return C;
    if (!fcache_pinned(C2)) return C2;
    return fcache_ovf_take();
}

static int fcache_decode_into(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                              uint32_t s1, uint32_t s2, uint32_t s3, fcache_t* C);

static const fcache_t* fcache_get(CPUState* cpu, uint32_t fb) {
    uint32_t bytecount = cpu->read_word(fb);
    if (bytecount == 0 || bytecount > 0x10000u) return NULL;    /* desync, as before */
    uint32_t s1 = cpu->read_word(fb + 4u), s2 = cpu->read_word(fb + 8u), s3 = cpu->read_word(fb + 12u);
    /* 2-way probe: a scene holds ~80+ hot faceblocks, so a 1-way map collides
     * (~60 evict/re-decode per second measured); the second slot absorbs pairs. */
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

/* Decode one faceblock into C (malloc'd face array; C's previous array freed).
 * The stream parse itself is the game's (moby_game.decode_faces); this is the
 * cache shell around it, factored out of fcache_get so the render-time PATCHED
 * decode (fpcache_get) reuses the exact same parse. Returns 0 on failure. */
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
 * without this mirror it decodes the unpatched model — the user-visible
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

/* Stage the span the patched decode touches: the faceblock the decode reads
 * and every word the list patches, which the .s writes at an offset into the
 * HP faceblock. The list is walked read-only first, because the span must
 * exist before the first write. Returns 0 when nothing could be staged, in
 * which case the caller may not patch. */
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

/* Apply the frame's patch list to PSX RAM, recording originals. Returns the
 * number of patched words, 0 = no list/invalid, -1 = list too large (rolled
 * back, caller must fall back to the unpatched decode). */
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

/* Face decode for one (faceblock, header, keyframe) triple: the patched
 * decode when the keyframe carries a patch list under the .s gate (ZSF4 > 0
 * — the LP tier never patches), else the plain Tier-1 cache. */
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
static const fcache_t* moby_faces_for_record(CPUState* cpu, const rec_meta* R) {
    return moby_faces_for(cpu, R->addr, R->model_held, R->faceblock,
                          R->header, R->frameA, R->zsf4);
}
/* PREV-keyframe faces for the sub-tick COLOUR crossfade (the faces the game
 * drew last tick, with last tick's patches). NULL = snap. Face-for-face
 * colour correspondence needs prev/curr to share faceblock + colour blocks
 * (true within a clip and across the blast's clip switch; anything else
 * snaps). */
static const fcache_t* moby_faces_for_prev(CPUState* cpu, const rec_meta* R) {
    if (!moby_pose_live(R->header_p, R->frameA_p)) return NULL;
    uint32_t hp = (R->header_p >= 0x80000000u) ? R->header_p : R->header;
    if (hp != R->header) {
        if (cpu->read_word(hp + ANIM_FACES_HP) != cpu->read_word(R->header + ANIM_FACES_HP) ||
            cpu->read_word(hp + ANIM_COL_HP)   != cpu->read_word(R->header + ANIM_COL_HP)   ||
            cpu->read_word(hp + ANIM_COL_FAR)  != cpu->read_word(R->header + ANIM_COL_FAR)) return NULL;
    }
    return moby_faces_for(cpu, R->addr, 0u, R->faceblock, hp, R->frameA_p, R->zsf4);
}
/* Per-channel colour lerp (values are 0x00BBGGRR walk colours). */
static inline uint32_t moby_clerp(uint32_t a, uint32_t b, float t) {
    int r = (int)(a & 0xFFu)         + (int)lroundf(((int)(b & 0xFFu)         - (int)(a & 0xFFu))         * t);
    int g = (int)((a >> 8) & 0xFFu)  + (int)lroundf(((int)((b >> 8) & 0xFFu)  - (int)((a >> 8) & 0xFFu))  * t);
    int bl = (int)((a >> 16) & 0xFFu) + (int)lroundf(((int)((b >> 16) & 0xFFu) - (int)((a >> 16) & 0xFFu)) * t);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)bl << 16);
}
static long s_clerp_recs;   /* tier-census: records colour-crossfaded this window */

/* Build s_wtri[] from the independent face-stream walk. Returns triangle count.
 * No backface cull yet (the winding sign is determined from the A/B's nclip
 * split first); quads emit both sub-tris when quad-enable is set. */
static int build_walk_tris_inner(CPUState* cpu);
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
 * and both face caches allocate and evict — none of which is safe to run on
 * several workers at once. */
static const fcache_t* s_pre_FC[MAX_RECORDS];
static const fcache_t* s_pre_FCp[MAX_RECORDS];
static float           s_pre_clra[MAX_RECORDS];
/* Where each record's triangles landed, for the ordered merge. */
static struct { int worker, start, count; } s_pre_rec[MAX_RECORDS];

/* Walk records [r0, r1) into the calling worker's emit target. */
static void walk_records(CPUState* cpu, int is_spec, int r0, int r1) {
    for (int r = r0; r < r1; r++) {
        rec_meta* R = &s_recs[r];
        uint32_t mob = R->moby;   /* set in all paths (==rec+0x34 for .s recs); host-built reveal/re-extract records have addr=0 so reading R->addr+0x34 would miss the dedup */
        /* SPECULAR dedup: skip a moby the REGULAR pass already fed (overlap → would
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
         * the occlusion/hold bookkeeping dedup against) — only its triangles
         * are dropped, so this is display-side alone: the .s pass still sets
         * m_WasDrawn @8001F4CC, the moby still auto-includes into the next
         * tick's update list, and its one-shot job still runs on schedule. */
        if (mob && moby_run_once_hidden(cpu, mob, s_cur_moby_cls)) continue;
        /* OPENPETE_MOBY_CLASSDBG=1: one line per drawn moby (class + vaddr +
         * world position) — names the producer when a scene object needs a
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
          s_cur_ui = openpete_ui_moby_is_addr_static(mob) ? 2
                   : openpete_ui_moby_is_addr_ui(mob) ? 1 : 0;
          /* Regular-walk twin of the shaded HUD3D promotion: a UI-range moby
           * that is a real 3D MODEL self-occludes and needs its own Z pass —
           * the flat HUD layer's per-tri painter can't sort interpenetrating
           * faces (the level-transition fly-in Spyro showed his teeth through
           * the back of his head). Promote by PROJECTED view-Z extent of this
           * record's verts (full-world units; a text glyph spans a few units,
           * a body model hundreds). Thin glyphs stay flat HUD, under the
           * player, like the shaded walk's falsified-promotion note demands. */
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
        /* per-vertex colour base + fade by the record's LOD tier (ladder documented
         * at moby_tier_faceblock; indexed by w1f). NEAR: model colours (hdr+0x18)
         * raw. FOG band: model colours DPCS-faded toward the per-vertex far palette
         * (moby_color_fade_ir0). LP/FAR band: the far palette (hdr+0x20) itself —
         * the .s walks it with the LP faces; under --no-lod the faces flip to HP
         * but the palette stays, so mirror both — DPCS-faded toward the ONE
         * cyclorama fog colour once ZSF4 < -2048 while g_SkipLowPolyWorld > 0
         * (.L8001FEA0..FECC: IR0 = min(-(ZSF4+2048), 2047) << 1). */
        uint32_t farbase = 0; int dpcs_ir0 = 0;
        int fogfc[3]; const int* fcc = NULL;
        uint32_t cbase;
        if (R->zsf4 <= 0) {
            cbase = (R->header >= 0x80000000u) ? cpu->read_word(R->header + ANIM_COL_FAR) : 0u;
            if (R->zsf4 < -2048 && moby_game.skip_lowpoly &&
                (int32_t)cpu->read_word(moby_game.skip_lowpoly) > 0) {
                int v = -(R->zsf4 + 2048); if (v > 2047) v = 2047;
                dpcs_ir0 = v << 1;
                uint32_t fog = cpu->read_word(game_layout.cyc.bg_colour);
                fogfc[0] = (int)((fog << 4) & 0xFF0u);   /* 12-bit expand, .s 8001FEF4-FF08 */
                fogfc[1] = (int)((fog >> 4) & 0xFF0u);
                fogfc[2] = (int)((fog >> 12) & 0xFF0u);
                fcc = fogfc;
            }
        } else {
            cbase = (R->header >= 0x80000000u) ? cpu->read_word(R->header + ANIM_COL_HP) : 0u;
            /* Fog-band / effect-moby depth-cue fade (fade-out instead of popping). */
            dpcs_ir0 = moby_color_fade_ir0(cpu, R->zsf4, R->header, &farbase);
        }
        if (cbase < 0x80000000u) cbase = COLOR_SCRATCH;   /* fall back to fog scratch */
        { /* OPENPETE_MOBY_FADEDBG=frameLo: per-record LOD-fade forensics (4 frames). */
            static int s_fd0 = -2;
            if (s_fd0 == -2) { const char* e = getenv("OPENPETE_MOBY_FADEDBG"); s_fd0 = (e && *e) ? atoi(e) : -1; }
            if (s_fd0 >= 0 && (int)openpete_frame >= s_fd0 && (int)openpete_frame < s_fd0 + 4) {
                uint32_t c0 = cpu->read_word(cbase), c1 = cpu->read_word(cbase + 4);
                uint32_t f0 = farbase ? cpu->read_word(farbase) : 0, f1 = farbase ? cpu->read_word(farbase + 4) : 0;
                fprintf(stderr, "[fade vbl=%u] rec=%d moby=%08x cls=%03x zsf4=%d hdr=%08x cbase=%08x farbase=%08x ir0=%d "
                        "c0=%06X c1=%06X f0=%06X f1=%06X\n",
                        openpete_frame, r, R->moby, R->moby >= 0x80000000u ? (cpu->read_half(R->moby + game_layout.moby.class_off) & 0xFFFu) : 0xFFF,
                        R->zsf4, R->header, cbase, farbase, dpcs_ir0, c0 & 0xFFFFFFu, c1 & 0xFFFFFFu, f0 & 0xFFFFFFu, f1 & 0xFFFFFFu);
                if (dpcs_ir0 > 0 && farbase) {   /* one fog-band record: full table triplets */
                    int nc = (int)(cpu->read_half(R->header + game_layout.moby.anim_ccount_off) & 0xFFFFu); if (nc > 24) nc = 24;
                    for (int k = 0; k < nc; k++) {
                        uint32_t nw = cpu->read_word(cbase + (uint32_t)k*4u) & 0xFFFFFFu;
                        uint32_t fw2 = cpu->read_word(farbase + (uint32_t)k*4u) & 0xFFFFFFu;
                        uint32_t ov = moby_color_at(cpu, cbase, farbase, NULL, dpcs_ir0, (uint32_t)k*4u);
                        fprintf(stderr, "  [ftab k=%d] near=%06X far=%06X out=%06X\n", k, nw, fw2, ov);
                    }
                }
            }
        }
        /* Resolve the colour bases to host pointers once per record; the loop
         * then reads colours directly (moby_color_at_host). Fallback = the
         * cpu->read_word path when a base doesn't map (never in practice). */
        const uint8_t* cb_host = (const uint8_t*)openpete_subtick_host_ptr(cbase);
        const uint8_t* fb_host = farbase ? (const uint8_t*)openpete_subtick_host_ptr(farbase) : NULL;
        int host_col = (cb_host != NULL) && (farbase == 0 || fb_host != NULL);
        /* Sub-tick COLOUR crossfade (patch-animated effects): the fade of the
         * Gnasty-blast dome/rings is per-KEYFRAME colour-index re-pointing down
         * a pre-baked ladder (faceblock patches) — the colour block itself is
         * static, so colours snap each keyframe (~15Hz). Crossfade per face
         * between the PREV drawn keyframe's colours and the curr ones by the
         * SAME alpha that lerps the vertex streams — endpoints are exactly the
         * two states the game drew. Only fetched when a patch list is in play
         * (prev==curr faces otherwise makes it a no-op). */
        const fcache_t* FCp = s_pre_FCp[r]; float clr_a = s_pre_clra[r];
        t_diag->faces  += FC->raw;
        t_diag->sprite += FC->sprites;
        t_diag->skip   += FC->skips;
        if      (FC->align == 0) t_diag->align_lo++;
        else if (FC->align == 1) t_diag->align_hi++;
        else                     t_diag->desync++;
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
             * (Measured: walkQ matches the .s's primQ=49/frame on the Insomniac sign,
             * recall 64%->99%. The earlier inverted map drew one half of every quad.)
             * STRIPS (.L8002031C) are single triangles, keep if NCLIP>0. */
            proj_xy* p3 = (i3 < vcount) ? &s_proj[base+i3] : NULL;
            /* Backface NCLIP on the precise FLOAT screen coords (NOT the GTE's floored
             * int16): the 1px floor flips a near-zero-area face's sign at grazing angles,
             * culling genuinely front-facing thin-moby tops (Artisans water platforms,
             * cls 0x12 → waterfall showed THROUGH the disc). Float is exact there, and the
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
                c0 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, F->c0);
                c1 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, F->c1);
                c2 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, F->c2);
                c3 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, F->c3);
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
                c0 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, F->c0);
                c1 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, F->c1);
                c2 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, F->c2);
                c3 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, F->c3);
            }
            if (FCp) {   /* colour crossfade: prev-keyframe colours -> curr by clr_a */
                const wface_t* P = &FCp->f[fi];
                uint32_t p0, p1, p2, p3v;
                if (host_col) {
                    p0 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, P->c0);
                    p1 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, P->c1);
                    p2 = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, P->c2);
                    p3v = moby_color_at_host(cb_host, fb_host, fcc, dpcs_ir0, P->c3);
                } else {
                    p0 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, P->c0);
                    p1 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, P->c1);
                    p2 = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, P->c2);
                    p3v = moby_color_at(cpu, cbase, farbase, fcc, dpcs_ir0, P->c3);
                }
                uint32_t c0curr = c0;
                c0 = moby_clerp(p0, c0, clr_a);
                c1 = moby_clerp(p1, c1, clr_a);
                c2 = moby_clerp(p2, c2, clr_a);
                c3 = moby_clerp(p3v, c3, clr_a);
                /* OPENPETE_MOBY_CLERP_DBG=1: face-0 ramp per present — must run
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
             * field. Previously approximated as c2, which forced every quad's 2nd
             * tri (v1,v3,v2) and every (v3,v1,v2) tri to colours (c1,c2,c2)/(c2,..)
             * — a broken gouraud gradient that rendered as hard facets on ~all
             * quad faces (51/face-stream here). Reading the real colour restores
             * the smooth per-vertex shading. (c3 computed with c0-c2 above.) */

            /* Tiledef material from the cache (locals: the spec env path below
             * overwrites UV/tpage for reflective faces). */
            int tpx=F->tpx, tpy=F->tpy, clx=F->clx, cly=F->cly, bpp=F->bpp, abr=F->abr;
            /* MOD-TEXTURE faces on a heated moby: mirror the .s flame-heat
             * red-shift (.L80021FE0) on the modulating colours. The stock
             * metal chest's own face flavors get it via the env/highlight
             * branches below, which a replacement model's mod-textured
             * faces never enter — a flamed spyro3-chests vase stayed cold.
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
             * prim's UV word — word bits 22-30 ARE the CLUT y, so the row is
             * biased by IR0>>9 = 0..7 down an 8-row fogged-CLUT ladder; same
             * mechanism as the terrain CLUT-row texel fog). Without it the walk
             * sampled the base (unfogged) row at every tier — distant HP mobys
             * rendered full-bright texels over correctly-faded vertex colours
             * (the ram's white blowout at the LP boundary, verified +6 rows at
             * ir0=3512 vs the captured .s prims). Per-record and view-dependent,
             * so applied here at emit — never baked into the Tier-1 face cache. */
            int cly_fog = (dpcs_ir0 > 0 && !fcc) ? (dpcs_ir0 >> 9) : 0;
            cly += cly_fog;
            int u[4]={F->u[0],F->u[1],F->u[2],F->u[3]}, v[4]={F->v[0],F->v[1],F->v[2],F->v[3]};
            /* #1 SPECULAR per-face shading. STRIP faces (!fresh) with word0&0x4
             * get the special per-face treatment (highlight-enable @80021B64;
             * dispatcher .L80021DB4): METAL record -> env reflection (the
             * .L80021FE0 chain, per-vertex .L800222C0 atan2 + the per-moby
             * .L800226F8 TR-azimuth u-bias); non-metal -> diffuse highlight.
             * FRESH faces with &0x4 are BILLBOARD SPRITES (.L8002256C, handled
             * above with authored Tiledef). Every OTHER face keeps its baked
             * Tiledef texture + per-vertex colour. This per-face gate is
             * load-bearing: a metal moby's sombrero/eyes are regular textured
             * faces and must NOT be env-mapped -- only its at&4 strip faces
             * reflect. (The earlier gate env-mapped EVERY textured face of a
             * metal moby -> sombrero went shiny-green with the wrong texture.) */
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
               * at moby range — far below any real geometry separation). */
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
                 * chest's BK is gray -> silver. Using the gray *base* colour missed
                 * this. BK channels (the .s packing @80021E38): RBK=(ms>>10)&0xFF,
                 * GBK=(ms>>4)&0xFF, BBK=(ms<<2)&0xFF; spec = byte2 = (ms>>16)&0xFF.
                 * facing add fitted to the prims: ~ -1.19 * spec * NL (gray), NL =
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
             * bit0 of the per-face colour-index word w1 (fp+4) — deliberately the
             * bit the &0x7FC colour-index masks drop. When ABE is set the blend
             * MODE is the texpage ABR field (bits 5-6); untextured ABE faces have
             * no texpage of their own so they take the GPU's default ABR 0 (50%).
             * Mapping mirrors particle_native: 0=ABR0(B/2+F/2), 1=ABR1(add),
             * 2=ABR2(subtract), 3=ABR3(B+F/4 -> additive). */
            int blend = (F->blendbit && !special_tex) ? moby_abr_blend(abr) : 0;

            /* OPENPETE_MOBY_FACEDUMP=<moby address, hex>: every face of that
             * record as the walk emits it — topology, the two flag bits that
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

            if (emit_a) {
                s_emit_fkey = PACK_FKEY(i0,i1,i2);
                emit_wtri(p0,p1,p2, c0,c1,c2, u[0],v[0],u[1],v[1],u[2],v[2],
                          textured,tpx,tpy,clx,cly,bpp,blend);
            }
            if (emit_b) {
                if (b_is_v3tri) {        /* tri-2nd (v3,v1,v2) */
                    s_emit_fkey = PACK_FKEY(i3,i1,i2);
                    emit_wtri(p3,p1,p2, c3,c1,c2, u[3],v[3],u[1],v[1],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
                } else {                 /* quad 2nd half (v1,v3,v2) */
                    s_emit_fkey = PACK_FKEY(i1,i3,i2);
                    emit_wtri(p1,p3,p2, c1,c3,c2, u[1],v[1],u[3],v[3],u[2],v[2],
                              textured,tpx,tpy,clx,cly,bpp,blend);
                }
            }
        }
    }
}


/* Serial prologue: resolve every record's face sets, and decide whether this
 * present's walk may fan out at all.
 *
 * The face walk writes guest RAM. A keyframe that carries a patch list has its
 * high-poly faceblock patched in place, decoded and restored — mirroring what
 * the .s does before rendering each record, which is how effect models animate
 * STRUCTURE per keyframe (the Gnasty-blast dome shrinks and fades entirely
 * through it). What makes it legal is that patch -> decode -> restore is
 * synchronous, so nothing can observe the transient; per-record parallelism
 * destroys exactly that property, and the buffer holding the originals is one
 * shared array, so a torn restore writes WRONG words back and the corruption is
 * permanent and guest-visible.
 *
 * Resolving here disposes of that, and of the two cache hazards with it — an
 * eviction frees a face array a concurrent reader may still hold, and the
 * 32-slot patched-decode ring inserts round-robin, so a later insertion can
 * clobber a pointer already handed out. All three reduce to one rule: fan out
 * only when the prologue inserted into NEITHER cache. In steady state neither
 * ever misses (tens of thousands of hits against zero, and sixteen patched
 * decodes a second against eighty thousand hits), so this costs a serial walk
 * on the rare present that loads a new face set, and nothing otherwise.
 *
 * Returns 1 when the walk must stay serial. */
/* Engagement census: walks that fanned out against walks that stayed serial. */
static long s_walk_par, s_walk_serial;

static int walk_dbg_env(void) {
    /* Any active walk instrument forces serial: their censuses and dumps are
     * whole-walk statics with serial semantics, and several latch mid-walk. */
    static int v = -1;
    if (v < 0)
        v = (getenv("OPENPETE_MOBY_CLASSDBG") || getenv("OPENPETE_MOBY_FADEDBG") ||
             getenv("OPENPETE_MOBY_CLERP_DBG")) ? 1 : 0;
    return v;
}

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
        /* Address arithmetic on the prev keyframe: only where the record names
         * a keyframe by pointer. Where it names it by stream word
         * (rec_frame_is_word) the patch list hangs off the record, not off the
         * keyframe, and the prev record is gone by now, so the colour
         * crossfade has no prev patch list to read and the faces snap. */
        if (!game_layout.moby.rec_frame_is_word &&
            R->frameA_p >= 0x80000000u && R->zsf4 > 0 && R->header >= 0x80000000u) {
            int curr_p = (R->frameA >= 0x80000000u) && (cpu->read_word(R->frameA + 4u) & 0xFFFFu) != 0u;
            int prev_p = (cpu->read_word(R->frameA_p + 4u) & 0xFFFFu) != 0u;
            if (curr_p || prev_p) {
                const fcache_t* FCp = moby_faces_for_prev(cpu, R);
                if (FCp == FC || (FCp && FCp->nfaces != FC->nfaces)) FCp = NULL;
                if (FCp) { s_pre_FCp[r] = FCp; s_pre_clra[r] = R->clr_alpha; s_clerp_recs++; }
            }
        }
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
     * signalled — including one that finds no work left — so a run costs on the
     * order of 20 us however small it is, while the walk itself runs at ~0.1 us
     * per face (307 faces / 30.6 us and 615 / 66.4 us, two scenes a factor of
     * two apart). Break-even is therefore a few hundred faces, and below it the
     * walk measured slower parallel than serial.
     *
     * The bound needs no per-machine calibration, because being wrong about it
     * is wildly asymmetric: fanning out a scene that was too small costs a few
     * microseconds, while failing to fan out a heavy one costs more than a
     * millisecond — a whole-level vista walks ~17600 faces, 1852 us serial
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
        O->cap = MAX_NTRI;
        if (!O->tri || !O->fkey || !O->moby || !O->cls || !O->ui || !O->front ||
            !O->mangle || !O->dofs || !O->special) return 0;
    }
    return 1;
}

typedef struct { CPUState* cpu; int is_spec; } mn_job;

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
    }
}

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

/* A/B the walked triangles vs the captured prims (topology desync detector).
 * Build prim-triangles from s_gte (quads split), match each walked tri to a
 * prim tri by order-independent vertex coincidence (<=2px), report precision
 * (walk tris that hit a prim) + recall (prims covered) + the nclip-sign split
 * of matched-vs-unmatched walk tris (to choose the backface-cull winding). */
typedef struct { float x[3], y[3]; uint32_t rgb[3]; int u[3], v[3]; int tex, clut, tpage; } ptri;
#define MAX_PTRI (MAX_PRIMS*2)
static ptri s_ptri[MAX_PTRI]; static int s_ptri_n;
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
/* best order-independent max-vertex distance of walk tri w vs prim tri p (greedy). */
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
/* Box-dump latch, one shot per producer pass. A single latch fired on the
 * first pass of the first frame and silenced every later pass, so a dump
 * keyed to a box only ever described the animated walk; the material name
 * (s_cur_mat) identifies the pass, and each name is dumped once. Returns
 * non-zero when the current pass has already dumped. */
#define BOX_PASS_MAX 4
static const char* s_pb_mats[BOX_PASS_MAX]; static int s_pb_nmat;
static const char* s_wb_mats[BOX_PASS_MAX]; static int s_wb_nmat;
static int box_pass_latched(const char** seen, int* n) {
    for (int i = 0; i < *n; i++) if (seen[i] == s_cur_mat) return 1;
    if (*n < BOX_PASS_MAX) seen[(*n)++] = s_cur_mat;
    return 0;
}

static void compare_walk_vs_prims(void) {
    build_prim_tris();
    static int s_cmp_dumped = 0; int s_cmp_dump = !s_cmp_dumped; s_cmp_dumped = 1;
    { /* OPENPETE_MOBY_PRIMBOX="x0,y0,x1,y1": dump every captured .s prim with a
       * vertex in the PSX-screen box (per-vertex xy+rgb) — the position-matched
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
       * [pbox] — every walk triangle with a vertex in the same PSX-screen box,
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
             * corner — comparing rgb[0] vs rgb[0] scored a rotated correspondence
             * of the same gouraud face as a miss. Pair each walk vertex with the
             * nearest prim vertex (<=2.5px) and compare THERE; faces that still
             * differ carry genuinely wrong colours. */
            uint32_t wc=w->v[0].rgb&0xFFFFFFu, pc=s_ptri[bp].rgb[0]&0xFFFFFFu;
            {
                /* Corner pairing is only meaningful when the prim's vertices are
                 * mutually separated by more than the pairing radius — on a tiny
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
                else if (paired < 3) { colmatch++; /* geometric pairing failed — not colour evidence */ }
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
        "| special_faces=%ld/%ld/%ld bit4_faces=%ld\n",
        openpete_frame,
        s_wdiag[0].fresh_g, s_wdiag[0].fresh_t, s_wdiag[0].strip, s_wdiag[0].sprite,
        s_wdiag[0].skip, s_wdiag[0].quad, s_wdiag[0].oor, s_wdiag[0].nearstraddle,
        s_wdiag[0].align_lo, s_wdiag[0].align_hi, s_wdiag[0].desync,
        neg_m,pos_m,neg_u,pos_u, mt_n,mt_p,mf_n,mf_p, colmatch,coln, uvmatch,uvn, wdup,wn,
        prim_quad, prim_tri, s_wdiag[0].quad, coln?wluma_sum/coln:0.0, coln?pluma_sum/coln:0.0,
        s_special_faces[0], s_special_faces[1], s_special_faces[2], s_bit4_faces);
    fprintf(stderr, "[moby-walk-special vbl=%u] pairedChannels=%ld meanErr=%.2f maxErr=%ld\n",
            openpete_frame, spc_n, spc_n ? (double)spc_sum/(double)spc_n : 0.0, spc_max);
    #undef ONSCR
}

/* Within-matched-tri colour A/B for the native shaded lighting (#2b). Global
 * position-NN is unreliable here — cliff_town packs ~58 shaded mobys into a
 * small region, so a vertex's nearest prim point often belongs to a DIFFERENT
 * moby. So: match each native walk tri to a prim tri by geometry (centroid
 * prune + tri_match_cost, the same as compare_walk_vs_prims), then min-assign
 * the 3 colour pairs (best of 6 permutations) and tally per-vertex exact match
 * + mean per-channel error. Staying inside a geometry-matched face pair keeps
 * the comparison within the same moby. On-screen tris only. */
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

/* Specular (shiny-metal) per-face colour is a GTE env-mapped reflection
 * (.L800226F8: light matrix D_800770C8+0x18 · face normal, atan2 reflection,
 * env lookup) gated by the metal flag in m_SpecularMetalColor's high byte. The
 * regular per-vertex colour path produces only the near-black DPCS *base*
 * (+0x1100), so specular metal rendered dark. Until the reflection is re-derived
 * natively, reuse the .s's already-correct per-face colour from the captured
 * prims (NN-matched to the walk geometry, per-vertex by nearest point) -- the
 * same "reuse the game-computed colour" the regular path does with the +0x1100
 * scratch, just per-face. Walk geometry (float, no wobble) is kept; only RGB is
 * overwritten. Walk tris with no nearby prim (the game's distance-culled far
 * mobys) keep their base colour. */
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

/* ---- specular env-reflection UV (#1), native float ---------------------
 * The shiny-metal faces reflect the environment: the .s computes per-vertex UV
 * from refl = metalMatrix . (viewPos-TR) mapped to spherical coords (atan2 + the
 * D_8006D908/D_8006CBF8 tables), gated by m_SpecularMetalColor's high byte and
 * offset per-moby by the camera heading (.L800226F8/.L800222C0). metalMatrix
 * (D_800770C8+0x18) is the camera rotation with z negated (MetalBuildMatrix,
 * update-side), so refl is already CAMERA-relative -> the reflection tracks the
 * camera for free. We re-derive in FLOAT (design choice): a clean spherical map,
 * HD-ready, the prim UV is a closeness/camera-tracking sanity check (not 100%).*/
static int   s_metalM[3][3];     /* D_800770C8+0x18, int16 1.0=4096 */
static int   s_camHead;          /* g_Camera+0x50 (camera heading word) */
/* s_specL[3]/s_specLlen forward-declared above build_walk_tris (highlight uses them) */
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
/* refl = metalMatrix . vec  (vec = the rotated vertex offset ex/ey/ez). */
static void specular_env_refl(float ex, float ey, float ez, float r[3]) {
    r[0] = (s_metalM[0][0]*ex + s_metalM[0][1]*ey + s_metalM[0][2]*ez) / 4096.0f;
    r[1] = (s_metalM[1][0]*ex + s_metalM[1][1]*ey + s_metalM[1][2]*ez) / 4096.0f;
    r[2] = (s_metalM[2][0]*ex + s_metalM[2][1]*ey + s_metalM[2][2]*ez) / 4096.0f;
}

/* CALIBRATION (falsify-before-ship): for the metal/env textured walk faces,
 * dump refl + several candidate spherical mappings alongside the captured prim
 * U/V/tpage so the exact axis/scale/origin can be read off real data before the
 * final mapping is wired in. Mirrors recolor's NN walk->prim match. */
#define PI_F 3.14159265358979f
static float wrap128(float x) { x = fmodf(x, 128.0f); return x < 0 ? x + 128.0f : x; }
/* Native env-UV, fitted byte-exact (<=1 texel) to the prim-UV dump:
 *   refl = metalMatrix . (viewPos-TR)              [camera-relative pitch/roll]
 *   u = (52.5 - (128/2pi)*atan2(rx,ry) - camHead/16) mod 128
 *   v = 64 + (128/pi)*asin(rz/|r|)
 * The metalMatrix (MetalBuildMatrix, z-negated camera rot) is NOT rebuilt every
 * frame, so the .s tracks fine camera YAW with a per-frame camHead term
 * (g_Camera+0x50, an 11-bit 2048/circle heading; 2048 -> the 128-texel azimuth
 * wrap == camHead/16). This keeps the reflection WORLD-anchored: turning the
 * camera slides the reflected environment across the metal. */
static float env_c2(void) {   /* calibration constant (temp env override) */
    static float c = -1e9f;
    if (c < -1e8f) { const char* e = getenv("OPENPETE_ENV_C2"); c = (e && *e) ? (float)atof(e) : 14.5f; }
    return c;
}
static float env_azsign(void) {
    static float c = -1e9f;
    if (c < -1e8f) { const char* e = getenv("OPENPETE_ENV_AZSIGN"); c = (e && *e) ? (float)atof(e) : 1.0f; }
    return c;
}
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

/* SPECULAR HIGHLIGHT correlation (#1 follow-up: the flat-lit chest). The .s gives
 * non-metal shiny faces a per-face highlight (.L80021DB4: OP screen-face-normal ->
 * normalize -> CC against m_SpecularMetalColor + the specular light) that REPLACES
 * the per-vertex base colour. The native walk omits it -> flat/dim. Dump the
 * view-space face-normal . specular-light vs the prim colour to find the float
 * model. Runs BEFORE recolor so w->v[].rgb is still the native base. */
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

/* Append the moby triangles to the native scene (panel 0). Frame-keyed begin
 * composes with the cyclorama (drawn first, owns the bg fill) — mobys append
 * on top. Depth ordering is the real per-pixel Z-buffer (3a.3): each vertex
 * carries its own view-space Z, so opaque faces sort correctly regardless of
 * submission order. Determinism-safe: host-side display only. */
static void push_scene_moby(void) {
    if (!render_path_enabled(RP_MOBY)) return;  /* --render-paths: mobys not selected */
    static int s_logged = 0;
    if (!s_logged && s_gout.n > 0) {
        s_logged = 1;
        fprintf(stderr, "[moby-native] feeding panel 0: %d moby tris from gamestate "
                "(3a.2b independent face-stream walk) frame %u\n", s_gout.n, openpete_frame);
    }
    openpete_native_scene_begin_frame(openpete_frame);
    /* OPENPETE_MOBY_TRIDBG="x0,y0,x1,y1,fA,fB": dump every walk tri whose
     * centroid falls in the PSX-screen box for frames [fA,fB] — offline
     * identity diff (moby+fkey) across frames names which attribute flaps.
     * (Reinstated from b726f6c — the display-only forensics instrument.) */
    { static int s_td = -1; static int tx0,ty0,tx1,ty1,tfa,tfb;
      if (s_td < 0) { const char* e = getenv("OPENPETE_MOBY_TRIDBG");
          s_td = 0;
          if (e && sscanf(e, "%d,%d,%d,%d,%d,%d", &tx0,&ty0,&tx1,&ty1,&tfa,&tfb) == 6) s_td = 1; }
      if (s_td == 1 && (int)openpete_frame >= tfa && (int)openpete_frame <= tfb) {
          for (int i = 0; i < s_gout.n; i++) {
              const ntri_t* w = &s_wtri[i];
              float cx = (w->v[0].sx + w->v[1].sx + w->v[2].sx) / 3.0f;
              float cy = (w->v[0].sy + w->v[1].sy + w->v[2].sy) / 3.0f;
              if (cx < tx0 || cx > tx1 || cy < ty0 || cy > ty1) continue;
              fprintf(stderr, "[mtri] vbl=%u m=%08X k=%08X tex=%d tp=%d,%d cl=%d,%d bl=%d fr=%d ui=%d dofs=%d mg=%d rk=%d",
                      openpete_frame, s_wtri_moby[i], s_wtri_fkey[i], w->textured,
                      w->tpage_x, w->tpage_y, w->clut_x, w->clut_y, w->blend,
                      s_wtri_front[i], s_wtri_ui[i], s_wtri_dofs[i], s_wtri_mangle[i],
                      openpete_mobyrank_lookup(s_wtri_moby[i], s_wtri_fkey[i]));
              for (int k = 0; k < 3; k++)
                  fprintf(stderr, " v%d=%.2f,%.2f,%.1f,%.1f,%d,%d,%06X", k,
                          w->v[k].sx, w->v[k].sy, w->v[k].vz, w->v[k].tvz,
                          w->v[k].u, w->v[k].v, w->v[k].rgb & 0xFFFFFF);
              fputc('\n', stderr);
          }
      } }
    /* mobys cover shadows (ACTOR); HUD/UI mobys overlay on top. Stamped per-tri
     * from the walk's UI tag (0=ACTOR, 1=HUD flat overlay, 2=HUD3D depth-sorted
     * icon): the in-game 3D HUD icons (treasure/dragon/lives, the static g_Hud
     * range) go to HUD3D so their own faces Z-sort (depth-off paints them
     * inside-out); the flat near-OT text/2D billboards stay HUD. */
    /* Production rank source: snapshot THIS pose's eligible faces, then
     * match — the matcher walks the CURRENT tick's OT chain (complete in
     * the drawing buffer DrawOTag hasn't submitted yet) for pose-exact
     * ranks, falling back to the last submitted chain (one tick stale)
     * when the fresh root is unavailable. Ranks apply by (moby,fkey), so
     * sub-tick re-extracts inherit them for free. */
    if (openpete_mobyrank_on()) {
        openpete_mobyrank_snap_begin(openpete_frame);
        for (int i = 0; i < s_gout.n; i++) {
            if (s_wtri[i].blend != 0 || !s_wtri_moby[i] || s_wtri_ui[i] != 0 ||
                s_wtri_mangle[i]) continue;
            uint8_t su[3], sv[3];
            float sxs[3], sys[3];
            for (int k = 0; k < 3; k++) {
                sxs[k] = s_wtri[i].v[k].sx; sys[k] = s_wtri[i].v[k].sy;
                su[k] = (uint8_t)s_wtri[i].v[k].u; sv[k] = (uint8_t)s_wtri[i].v[k].v;
            }
            openpete_mobyrank_snap_face(s_wtri_moby[i], s_wtri_fkey[i], sxs, sys,
                                        su, sv, s_wtri[i].textured,
                                        s_wtri[i].clut_x, s_wtri[i].clut_y);
        }
        openpete_mobyrank_snap_end();
        openpete_mobyrank_match();
    }
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
            t.v[k].z    = openpete_native_depth_from_viewz(openpete_worldvz_full(s_wtri[i].v[k].vz));
            /* State the perspective W only where the OT pulls actually moved
             * this vertex; an unshifted vertex keeps the derive-from-z path
             * (identical result, no round-trip difference). */
            t.v[k].pvz  = (s_wtri[i].v[k].tvz == s_wtri[i].v[k].vz)
                        ? 0.0f : s_wtri[i].v[k].tvz;
            t.v[k].u    = (float)s_wtri[i].v[k].u;
            t.v[k].v    = (float)s_wtri[i].v[k].v;
            t.v[k].rgba = s_wtri[i].v[k].rgb | 0xFF000000u;
        }
        t.textured = s_wtri[i].textured;
        t.tpage_x  = s_wtri[i].tpage_x; t.tpage_y = s_wtri[i].tpage_y;
        t.clut_x   = s_wtri[i].clut_x;  t.clut_y  = s_wtri[i].clut_y;
        t.bpp      = s_wtri[i].bpp;
        t.blend    = s_wtri[i].blend;
        /* og OT key channel, moby twin of the terrain emit: the .s inserts
         * each moby face at its average-Z slot minus the authored
         * m_DepthOffset (the angle word's top byte, which the cull packs into record
         * w0 bits 16-23 / the shaded walk's rot>>24) — an ordering KEY,
         * never stored depth. The face's ABSOLUTE key rasters into the
         * MOBY key image (set 1, split from the terrain keys), where the
         * contest scopes the yield by the covering face's own TRUE view-Z
         * (MOBY_TRUESCOPE, psx_scene_key.frag): keyed terrain within the
         * moby's own authored pull yields per-pixel (a pedestal disc set
         * into sloped ground paints complete), while og's pull+push
         * compound — which paints a chest through the wall lip genuinely
         * in front of it — is not mirrored: a wall in front keeps
         * occluding by TRUE depth. The moby face contests nearer keys
         * symmetrically. Blend/FRONT/HUD classes
         * stay unkeyed — and so do MANGLED faces (near-plane fixup /
         * coordinate saturation): their screen footprint is not the true
         * projection, so a flat key stamped across one discards terrain
         * far outside the moby's real silhouette (a right-at-the-camera
         * moby flooded a whole wall away). A genuinely-near moby needs no
         * key anyway — it is the viewer-side surface and TRUE depth
         * already wins its own pixels. */
        { static int s_kob = -1;
          if (s_kob < 0) { const char* e = getenv("OPENPETE_MOBY_OTBIAS");
              s_kob = (e && *e) ? (*e != '0') : 1; }
          if (s_kob && cat == OPENPETE_SCENE_ACTOR && t.blend == 0 &&
              !s_wtri_mangle[i] &&
              s_wtri_moby[i] && openpete_native_key_resolve_on()) {
              int dofs = (int)s_wtri_dofs[i];
              double mean = ((double)s_wtri[i].v[0].tvz + (double)s_wtri[i].v[1].tvz
                           + (double)s_wtri[i].v[2].tvz) / 3.0;
              double key = mean - 128.0 * (double)dofs;
              if (key < 1.0) key = 1.0;      /* og floors the slot at 0 */
              t.koff = (float)(key + 0.5);
              /* Embedded-PROP classes (the k_lp_artfix seam pattern):
               * static scenery authored sunk into the ground — near-band
               * terrain always yields to them within the embed bound, so
               * they render whole from every angle instead of og's
               * view-dependent piece-key roulette. Curated, because no
               * geometric quantity separates an embedded prop from an
               * actor standing just behind a crest. */
              switch (s_wtri_cls[i]) {
              case 250:   /* dragon-pedestal statue */
              case 332:   /* dragon-pedestal base disc */
                  t.prop = 1; break;
              default: break;
              }
          } }
        /* Rank eligibility: regular/front-pass opaque moby faces with a true
         * screen footprint. Mangled faces (near fixup / coordinate
         * saturation) are excluded like they are from koff — their raster is
         * not the true projection, so a rank stamped across one would
         * discard content far outside the real silhouette. */
        int rank_ok = t.blend == 0 && s_wtri_moby[i] && s_wtri_ui[i] == 0 &&
                      !s_wtri_mangle[i];
        int rank = 0;
        if (rank_ok && openpete_mobyrank_on()) {
            rank = openpete_mobyrank_lookup(s_wtri_moby[i], s_wtri_fkey[i]);
        }
        if (rank > 0) {
            int oi;
            for (oi = 0; oi < rord_n; oi++)
                if (rord_moby[oi] == s_wtri_moby[i]) break;
            if (oi == rord_n && rord_n < 256) rord_moby[rord_n++] = s_wtri_moby[i];
            if (oi < 255) { t.rank = (float)rank; t.mordinal = oi + 1; }
        } else if (cat == OPENPETE_SCENE_ACTOR && t.blend == 0) {
            /* Actor-fragment marker (rank -1, still "unranked" to every
             * rank consumer): the key contest's prop-embed rule must fire
             * on TERRAIN fragments only — a statue standing on its disc
             * must never yield the pixel to it (psx_scene_key.frag). */
            t.rank = -1.0f;
        }
        /* Per-instance material slot, resolved HERE rather than banked at
         * extraction. Slot ids are interned per feed cycle and this loop
         * runs AFTER scene_begin_frame() has reset that table, so a slot
         * captured during the walk names nothing by the time it would be
         * stamped — and because ids are handed out in first-seen order, a
         * stale one can even name another instance's block. Re-resolving
         * from the banked IDENTITY (moby ptr + class) is the same rule the
         * residency ladder applies everywhere else: bank identity, never
         * outcomes. Memoized on the moby ptr — the walk emits a moby's
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
        openpete_native_scene_add_tri(&t, cat, 0);
    }
    openpete_native_scene_set_tag(0);   /* leave the tagged emit loop clean */
}

/* ---- projection A/B: native decode+project vs the emitted prim verts --
 * Nearest-neighbour match each captured GTE prim vertex against the native
 * s_proj set (the cyclorama instrument). The prims carry the authoritative final
 * SXY; a close match proves the decode + RTPS+TR projection reproduce the GTE.
 * Requires project_all_records() already called (s_proj filled). */
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
             * two diverge; we then bin those out (sanctioned, exactly as the
             * cyclorama harness did) rather than score them as decode failures.
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
                 * -> NN can flip frame-to-frame (the scaffold jitter; 3a.2b fixes). */
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
 * values `0` and `1` keep their boolean meaning — `1` selects the default
 * window plus an every-200-VBL sample, so one boot reports on each level the
 * run loads — and OPENPETE_MOBY_VERIFY_FRAME moves that default window's
 * start. */
static unsigned s_verify_lo = 2000u, s_verify_hi = 2012u;
static int      s_verify_sparse = 1;
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

/* Shared moby pass: run the real renderer (REGULAR or SPECULAR -- both loop the
 * SAME +0x1600 record buffer with identical vertex decode + RTPS projection +
 * face walk; only the per-face highlight COLOUR differs, which is display-side),
 * capture its prims, re-derive native float geometry, feed + A/B. Determinism-
 * safe: calls the real render exactly once, only reads gamestate afterward. */
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

    /* Verify aid: the pass's polybuf window — pairs with the full-OT dump
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
     * per-face in build_walk_tris (#1) already, so re-feeding the same moby from the
     * SPECULAR pass is redundant — AND wrong-pose: the native re-projects AFTER
     * func_80020F34 (and the intervening specular cull) has advanced the per-record
     * keyframe/blend state, so the SPEC feed paints a SECOND, different animation
     * pose over the REGULAR one (the "death animation shows 2 frames at once"
     * doubling — e.g. a dying moby read blend=3072 in REG, blend=0 in SPEC). So
     * feed each moby ONCE: the REGULAR pass feeds its whole set and snapshots it
     * (s_reg_moby); the SPECULAR pass feeds ONLY mobys REGULAR did not (build_walk_tris
     * skips records whose moby is in s_reg_moby) — which keeps specular-ONLY mobys
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
    if (!is_spec) {                           /* REGULAR runs first — snapshot its moby set */
        s_reg_moby_n = 0;
        for (int r = 0; r < nrec && s_reg_moby_n < MAX_RECORDS; r++)
            s_reg_moby[s_reg_moby_n++] = s_recs[r].moby;
    }
    if (feed) {
        if (!is_spec) {
            /* Host-built records are native-only — they never reach the .s
             * polybuf, so a present whose mobys came ENTIRELY from the reveal
             * records no RP_MOBY interval and reads as "canonical drew no
             * mobys". moby_reextract then skips every sub-tick pass and
             * the mobys exist on canonical presents alone: solid at
             * --render-fps 0, flickering-to-absent the moment presents outrun
             * ticks. That is the state past the canonical cull distance, where
             * the .s queue is empty by construction — so mark the path drawn on
             * the reveal's own yield. Can only fire when the .s REGULAR render
             * fn actually ran this tick (this is its hook), so a load screen —
             * where only the SHADED/text producer runs — still cannot mark it. */
            if (append_revealed_regular(cpu) > 0) render_path_mark_drawn(RP_MOBY);
        }
        { OP_PROF_ACCUM_SCOPE("moby.build");
          build_walk_tris(cpu); }             /* 3a.2b walk; SPEC dedups vs s_reg_moby; shading baked (#1) */
        /* The walk admits what its own cull mirror admits; on a tick where the
         * .s pass emitted nothing (an empty interval: the portal-entry tick
         * culls every record) but the walk still produced tris, the path
         * must read canon-drawn or the spine drops those tris on every
         * intermediate present while the canonical present shows them. */
        if (s_gout.n > 0 && end <= start) render_path_mark_drawn(RP_MOBY);
        { OP_PROF_ACCUM_SCOPE("moby.feed");
          push_scene_moby(); }                /* recolor_walk_from_prims dropped: colour already native
                                               * (walk +0x1100/base), env-UV now native (build_walk_tris) */
        /* Sub-tick spine: snapshot the cull records + per-vertex fog colour
         * scratch (both live now) so intermediates can re-derive these mobys.
         * REGULAR feeds the shared bracket; the SPECULAR pass rebuilds the
         * +0x1600 queue with the specular set (incl. specular-ONLY mobys like
         * the rescue dragon that REGULAR didn't draw), so capture it separately
         * for the re-extract's SPEC pass — else those mobys vanish on sub-ticks. */
        /* colour row is [0x1100,0x1600); the old 0x800 over-capture spilled
         * into the records, which capture_recs covers in full anyway. */
        {
            /* Runahead: the whole capture/invalidate tail is canonical-only.
             * A speculative REGULAR pass clearing s_canon_spec_valid (with
             * the guarded spec capture unable to re-validate) skipped the
             * SPEC replay on every intermediate — specular mobys strobed. */
            extern int openpete_speculative;
            if (!openpete_speculative) {
                if (!is_spec) {
                    spyro_native_canon_capture_range(cpu, 0x1100u, 0x500u);
                    spyro_native_canon_capture_recs(cpu);    /* full records (>13 overflow 0x1900) */
                    spyro_native_canon_capture_camera(cpu, SPYRO_PROD_MOBYREG);
                    /* Invalidate the SPECULAR capture at the START of each canonical
                     * moby render (REGULAR runs first every frame). The game only calls
                     * func_80020F34 when there ARE specular mobys to draw; once the last
                     * one goes (e.g. the rescue-dragon CRYSTAL statue, cls 0xFA, whose
                     * moby flips to state 0xFD when the dragon breaks out), the SPEC pass
                     * stops firing and never re-captures — so without this clear the
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
            build_walk_tris(cpu);             /* 3a.2b independent walk + topology A/B */
            compare_walk_vs_prims();
            if (mat && mat[0] == 'S' && mat[1] == 'P') {   /* confirm specular recolor attaches the prim colour */
                specular_env_calibrate(cpu);  /* #1 env-UV calibration dump (native refl vs prim UV) */
                specular_hl_calibrate(cpu);   /* #1 follow-up: highlight (face-normal.light vs prim) */
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

/* OPENPETE_MOBY_REVEALDBG: size the widescreen moby-reveal problem (the live-moby
 * task's mandated first step). Walks g_LevelMobys, replicates the .s moby cone
 * cull (spyro-moby-update-vs-render.md §3.3) at 4:3 AND at the active widescreen
 * factor, and classifies each moby the wide cone reveals (in the wide cone, NOT
 * the 4:3 cone) as LIVE (on the Spyro-anchored update list → animating, current
 * pose) or FROZEN (not updating → stale pose). The update gate is camera-angle-
 * INDEPENDENT (Spyro-anchored m_UpdateDistance xy-disk + always/never sentinels +
 * the m_WasDrawn grace), so a revealed moby being LIVE means we can draw it at its
 * current gameplay pose with no synthesis (the easy path); FROZEN means it needs
 * display-only animation (the deferred path). The live/frozen ratio sizes the
 * split. Read-only; prints once at the target frame. */
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
         * S=1 → 4:3 (slope 0.75), S=F/s → the view's horizontal half-FOV
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

/* The cull hook: run the game's cull, then take its record list before any
 * draw pass reuses the scratch it sits in. The cull is called exactly once,
 * as the fall-through would have called it, and nothing is written back. */
static int on_cull(CPUState* cpu, uint32_t addr) {
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_run_draw(cpu, addr);
    moby_rec_snapshot(cpu);
    s_in = 0;
    return 1;
}

static int on_draw_regular(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    fade_snap_clear();                             /* refresh per-moby ZSF4 fade snapshot this canonical frame */
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
    s_in = 0;
    return 1;
}
static int on_draw_specular(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!moby_verify_enabled() && !openpete_native_scene_enabled() && !render_paths_active()) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    moby_native_pass(cpu, game_layout.moby.hook_draw_specular, "SPEC");  /* same geometry, same record queue */
    s_in = 0;
    return 1;
}

/* ---- SHADED material (func_80022A2C) -- layout-confirming dump (3a.4.3) ----
 * Shaded is structurally different from regular/specular: it loops a flat
 * Moby* array the descriptor names, decodes 3-signed-byte (×2) verts, and rebuilds
 * R per-moby = ProjMatrix · Rz·Ry·Rx(m_Rotation). Before implementing that,
 * dump the data layout to confirm the RE offsets empirically (falsify-first).
 * func_80022A2C writes m_WasDrawn (verdict) -- we call it exactly once, like
 * the other hooks, so determinism is unchanged. */
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

/* SHADED geometry A/B. Decode the moby's verts (3 signed bytes x2: byte0=Z,
 * byte1=X, byte2=Y per .L80023010) from model[+0x4] (count=model[+0x0]) and
 * project with the per-moby R+TR. The attract demo has exactly ONE shaded moby,
 * so the GTE R+TR left after func_80022A2C ARE that moby's (RTPS doesn't modify
 * them and the face loop doesn't touch them) -- read them post-hoc to validate
 * the signed-byte decode + projection independent of the (harder) R-rebuild.
 * Set-matches the decoded verts vs the captured shaded prims, like the regular
 * vertex A/B. */

/* Compose the moby's full euler rotation onto `start`, mirroring func_80022A2C's
 * matrix build (block_80022DBC..80022F34): out = start·Ry·Rx·Rz, applied in that
 * order. The per-axis rotation columns come straight from the .s's MVMVA inputs
 * (Y@80022DE4, X@80022E90, Z@80022F40); the sin/cos table values go in verbatim
 * (sin may be negative). VERIFIED bit-exact vs the live RT registers: base·Ry =
 * gte_ctrl[0..4] for a billboard moby (cliff_town cls 0x3A). Euler indices:
 * Y=(rot>>15)&0x1FE, X=(rot&0xFF00)>>7, Z=(rot&0xFF)<<1 (rot=m_Rotation@+0x44). */
/* Sub-tick moby ROTATION interpolation state (set by moby_native_set_rot_interp,
 * declared in reextract.h). s_rot_now is the present's op_now()
 * timestamp, captured once when enabled so every moby composed this present uses
 * one consistent clock (each still gets its OWN per-object alpha from its distinct
 * prev/curr roll timestamps). 0 = inactive → byte-exact path (canonical render,
 * or --render-fps off where the snapshot table is empty anyway). */
static op_time_t s_rot_now = 0;
void moby_native_set_rot_interp(int on) { s_rot_now = on ? openpete_interp_present_now() : 0; }

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

int moby_native_tick_camera_pos(int32_t out[3]) {
    if (!s_tickcam_have) return 0;
    for (int i = 0; i < 3; i++) out[i] = s_tickcam_pos[i];
    return 1;
}

/* Inverse of a 3x3 by cofactors, in double. Returns 0 on a singular matrix. */
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

/* Shortest-arc interpolated byte angle (256/circle) between two byte endpoints. */
static double lerp_byte_angle(int prev_b, int curr_b, double t) {
    int d = curr_b - prev_b;
    if (d > 128) d -= 256; else if (d < -128) d += 256;
    return (double)prev_b + (double)d * t;
}

/* The moby's three euler bytes at this present's phase, as GTE sin/cos pairs:
 * each axis shortest-arc interpolated between the object's prev and curr
 * snapshots and sampled out of the in-RAM table by linear interpolation, so a
 * rotation that steps one byte per tick sweeps instead. Z is +0, X is +1 and Y
 * is +2 of the angle word; its high byte is an overlay flag and no axis.
 *
 * Returns 0 and leaves *e untouched when the caller must fall back to the
 * byte-exact table lookup: rotation interp off (every canonical render), no
 * pair, a pair with no distinct prev, or a slot whose two states hold different
 * classes — a reused slot would lerp across two objects. */
int moby_record_euler_at_present(CPUState* cpu, uint32_t moby, ngte_euler_sc* e) {
    if (!s_rot_now) return 0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(moby_interp_key(moby));
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + game_layout.moby.class_off, 2);
    memcpy(&ccls, o->curr + game_layout.moby.class_off, 2);
    if (pcls != ccls) return 0;
    double t = (double)(s_rot_now - o->curr_ns) / (double)(o->curr_ns - o->prev_ns);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
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

/* OPENPETE_MOBY_SNAP_STALE=0 restores the pre-fix behaviour (interpolate across
 * a stale-prev pair) for same-window A/B. Default on. */
static int moby_interp_stale_snap_enabled(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_STALE"); s = (e && *e) ? (*e != '0') : 1; }
    return s;
}

/* A per-object interp pair is STALE when the distinct-state gap exceeds ~1.5
 * ticks: the ring only rolls prev←curr on a change, so a gap that big means the
 * prev straddles a static stretch — the pair is a DISCONTINUITY, not continuous
 * motion.
 *
 * Used ONLY by the billboard-POSITION lerp below, which is reached only by mobys
 * that miss the primary sub-tick position path (openpete_native_interp_pos) —
 * i.e. HUD-pool text / tally / icon mobys, whose identity is fragile: a text
 * glyph keys by (class+slot), which is not a stable per-glyph identity across a
 * HUD relayout. At a submenu switch the pool re-lays out and a slot's key can
 * name a different logical letter, so its prev is the ~20-tick-old glyph that
 * last held the slot; lerping the ~15px position delta over that ~20-tick gap
 * crawled the letter across the screen (the pause-menu text "swim"). Snapping
 * presents the letter in place. A genuinely moving billboard (a flying tally
 * gem, a sliding-in HUD icon) rolls every tick (gap == 1 tick) → NOT stale →
 * still interpolates.
 *
 * NOT applied to the rotation (shaded_compose_euler) or keyframe-shape
 * (moby_prev_keyframe) lerps: those legitimately sweep a delta over a multi-tick
 * gap for SLOWLY-rotating / slowly-animating mobys (gems, idle dragons) whose
 * sub-byte-per-tick change only rolls the record every few ticks — snapping them
 * would restore the 1.4°/step stair-step that sub-tick interp exists to remove.
 * OPENPETE_MOBY_SNAP_STALE=0 reverts (interpolate across the stale pair). */
static int moby_interp_pair_stale(op_time_t prev_ns, op_time_t curr_ns) {
    if (!moby_interp_stale_snap_enabled()) return 0;
    return (curr_ns - prev_ns) > MOBY_TICK_PERIOD_NS + MOBY_TICK_PERIOD_NS / 2;
}

/* ---- host-side idle animation: gem wobble ---------------------------------
 * OPENPETE_MOBY_IDLE=1: mobys outside the canonical update gates freeze in
 * RAM (the game's own power-saving), so the reveal/extended paths draw them
 * as statues. Recompute what the frozen moby's own idle math WOULD produce,
 * host-side, and substitute it at this euler chokepoint — zero PSX RAM
 * writes; m_WasDrawn keeps its single writer (the .s cull).
 *
 * Scope: the collectable wobble (gems cls 83..87, life statue cls 14). The
 * level updater's collectable case skips its whole state machine IFF
 *   m_WasDrawn==0 && OctDistance(moby,g_Spyro)>=0x2001
 *   && m_Substate==1 && props->m_SpawnState==0        (level_10 .s 8007F9B8)
 * and otherwise, while m_Substate<3, writes
 *   rot.x(+0x44) = ((s16)cos[ticks*2]>>7) + props->m_RotY(+0x11)
 *   rot.y(+0x45) = ((s16)sin[ticks*2]>>7) + props->m_RotZ(+0x12)
 *   props->m_RotationTicks(+0x13) += 2*g_DeltaTime      (800802F4..80080360;
 * cls 14 uses >>9, +0x46 untouched). LUT == game_layout.sin_table (cos at +0x80) — the
 * same table sincos_interp blends. Life orb (cls 15) is an ACCUMULATED
 * heading (z+=8,y-=6/tick) — excluded: a wake would snap the whole offset. */
static int moby_idle_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_IDLE"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s;
}

/* Update-list membership mirror (func_80051FEC, moby_lists.s:35-96): drawn-
 * grace auto-include, m_UpdateDistance sentinels (0 = never, sign bit after
 * <<10 = always), else a strict 3-axis bbox |d| < ud*1024 around g_Spyro plus
 * a horizontal squared-distance test on (dx>>3, dy>>3). A moby outside the
 * list gets NO update at all — its whole state machine (including the gem
 * settle + wobble) is frozen in RAM. */
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

/* Freeze-verdict mirror — a pure function of gamestate the .s itself reads,
 * never of host render state (the reveal-hysteresis strobe lesson). The
 * canonical wobble advances only when the gem is ON the update list AND the
 * collectable case's inner skip gate doesn't fire (level_10 .s 8007F9B8:
 * skip iff !drawn && OctDistance>=0x2001 && resting && settled — OctDistance
 * (math.s) is XY-only octagonal: max + min*3/8). Beast-makers: far gems
 * can rest UNSETTLED (m_Substate 0 — never updated
 * since level load), so "outside the update list" alone must count as frozen
 * — the old substate==1-only mirror left those as statues. */
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
                     * cos[t0]>>7 + m_RotY). An UNSETTLED far gem (substate 0 —
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

/* ---- shaded directional lighting (#2b) — faithful integer GTE mirror -------
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

static int32_t sat_ir_lm(int64_t v, int lm) {   /* mirrors GTEState::saturate_ir */
    int32_t mn = lm ? 0 : -0x8000;
    if (v < mn) return mn;
    if (v > 0x7FFF) return 0x7FFF;
    return (int32_t)v;
}
static int32_t sat_col8(int64_t v) {            /* mirrors GTEState::saturate_color */
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int32_t)v;
}

/* RT = model->view rotation (same matrix used to project this moby's verts).
 * base = per-moby base-colour word. highlight=1 applies the gouraud spec boost
 * (block_800231C4..80023250); the flat path omits it. Returns 0x00BBGGRR (the
 * low 24 bits the .s stores, r in the low byte — matches s_gte/s_ptri rgb). */
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
    /* CC step 1 — light_color: LC matrix = light dir broadcast to all 3 rows */
    int64_t lc = (int64_t)s_shL[0]*ir1 + (int64_t)s_shL[1]*ir2 + (int64_t)s_shL[2]*ir3;
    ir1 = sat_ir_lm((((int64_t)rbk << 12) + lc) >> 12, 1);
    ir2 = sat_ir_lm((((int64_t)gbk << 12) + lc) >> 12, 1);
    ir3 = sat_ir_lm((((int64_t)bbk << 12) + lc) >> 12, 1);
    /* CC step 2 — color_output: RGBC = white (0xFFFFFF). MAC = (255*IR<<4)>>12 */
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


/* Shaded METAL sub-mode colour (word0 bit1, block_80023720) — env-mapped
 * reflection. The face normal is rotated to view space (RT·N), then the
 * reflection intensity = light·N' (the LCM is the directional light broadcast
 * across all rows, set from D_800770C8). The metal table entry (8B at
 * D_8006E44C[scolor>>21]) gives word0 = base tint, word1 = reflected tint; the
 * final colour = word0_base + (intensity·word1)>>12 + a brightness boost,
 * clamped 12->8. (This is the gem shine — without it gems read as the gray flat
 * table. The exact far-distance branch >>10/word0>>1 is approximated by the near
 * >>8 path; env-texture/depth select is display-irrelevant.) Returns 0x00BBGGRR. */
static uint32_t shaded_metal_color(const int RT[3][3], int nx, int ny, int nz,
                                   uint32_t w0, uint32_t w1) {
    int64_t m1=((int64_t)RT[0][0]*nx+(int64_t)RT[0][1]*ny+(int64_t)RT[0][2]*nz)>>12;
    int64_t m2=((int64_t)RT[1][0]*nx+(int64_t)RT[1][1]*ny+(int64_t)RT[1][2]*nz)>>12;
    int64_t m3=((int64_t)RT[2][0]*nx+(int64_t)RT[2][1]*ny+(int64_t)RT[2][2]*nz)>>12;
    int32_t ir1=sat_ir_lm(m1,0), ir2=sat_ir_lm(m2,0), ir3=sat_ir_lm(m3,0);
    int64_t refl=(int64_t)s_shL[0]*ir1+(int64_t)s_shL[1]*ir2+(int64_t)s_shL[2]*ir3;
    int32_t intensity=(int32_t)(refl>>8);
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
               } shaded_rec;   /* vzspread = model-space Z extent (max-min);
                 * a large extent means a real 3D model that self-occludes → needs the HUD3D
                 * depth pass, vs a thin text/decal overlay that renders fine depth-off.
                 * pvzspread = PROJECTED per-vertex view-Z extent (raw project() mac3,
                 * pre-vz_scale — same 4096-scale unit as model VZ). Catches what the
                 * static model measure can't: a thin model the game ROTATES (the
                 * inventory screen's spinning letter/dragon-icon mobys) swings its
                 * large X extent into view-Z, so it self-occludes despite a small
                 * model-space Z. Either spread ≥ threshold → HUD3D. */
static shaded_rec s_shrecs[256]; static int s_nshrecs;

/* Project EVERY shaded moby's verts (billboard or 3D, both verified 100%) into
 * out[]. Returns vertex count. Shared by shaded_verify + the feed. Also records
 * per-moby metadata in s_shrecs[] for the native topology walk. */
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
         * — the mechanism behind in-spawner objects that are dormant until released
         * (e.g. cliff-town's cauldron gems, which sit at m_RenderRadius 0 until you
         * flame the cauldron). The list still contains them, so replicate the skip
         * here, else they render as ghost gems beside their spawner. (m_RenderRadius
         * & 0x80 is the billboard/HUD flag, handled below — nonzero, so it passes.) */
        if (cpu->read_byte(m + moby_game.render_radius_off) == 0) continue;
        /* .s in-walk 3-axis distance-band cull (block_80022B40..80022B90) —
         * NON-BILLBOARD mobys only (the lhu@80022B20 halfword's bit7 <<24 sign
         * branches billboards past it). eff = (m_RenderRadius & 0x7F) << 8,
         * plus 0x200 when the SAME lhu's high byte (m_WasDrawn, +0x51) has
         * bit0 set — a one-tick hysteresis so boundary mobys don't flicker.
         * The moby is SKIPPED unless (pos−cam)>>2 is STRICTLY inside ±eff on
         * ALL THREE axes (camera = the canonical g_Camera+0x28.. words the
         * walk stashes at scratch+0x3E0 — the same cx/cy/cz above; axis signs
         * and >>2 rounding mirrored exactly). This in-WALK test — not the
         * queue — is what hides distant gems/props on PSX (the SHADED_BASE
         * list still contains them); without it native drew every listed
         * moby at any distance (the distant-gem leak).
         * Extended LOD drops the band outright (the reveal builder's twin —
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
        uint32_t mraw  = cpu->read_word(game_layout.moby.model_table + cls*4u); /* RAW entry — bit31 is a colour-mode switch */
        uint32_t model = mraw;                              if (model<0x80000000u) model|=0x80000000u;
        uint32_t vraw  = cpu->read_word(model + 0x4u);
        uint32_t vbase = vraw;                              if (vbase<0x80000000u) vbase|=0x80000000u;
        int count = cpu->read_byte(model + 0x0u) & 0xFF;
        uint32_t faces = cpu->read_word(model + 0xCu); if (faces<0x80000000u) faces|=0x80000000u;  /* shaded face stream */
        int fcount = cpu->read_byte(model + 0x1u) & 0xFF;   /* face count: base model+0xC, count model+0x1, stride 8 */
        /* STATIC-GOURAUD colour mode (.s block_800230EC..80023268, held-egg fix):
         * after the vertex loop the .s compares (vbase<<1>>1) against
         * raw_g_Models_entry + 0x10. Equal (entry stored WITHOUT bit31 and verts
         * inline at model+0x10) → it SKIPS the per-vertex lighting pre-pass and
         * takes s4 = [model+8] — the model's BAKED colour table — in the branch
         * delay slot at 0x800230FC. Mode-0 face colour fields are then BYTE
         * OFFSETS into that table (0x00BBGGRR words, emitted verbatim). bit31
         * SET in the entry makes the compare fail → lit-gouraud (+0x2A00 path).
         * (The egg thief's egg, cls 0x22, is the first static-gouraud moby met:
         * lit-gouraud painted it uniform gray.) */
        uint32_t ctab = 0; int ctab_len = 0;
        if (!(mraw & 0x80000000u) && (vraw & 0x7FFFFFFFu) == ((mraw & 0x7FFFFFFFu) + 0x10u)) {
            uint32_t craw = cpu->read_word(model + 0x8u);
            ctab = craw; if (ctab < 0x80000000u) ctab |= 0x80000000u;
            /* table spans [model+8 ptr, faces ptr) — bound the face-field offsets */
            ctab_len = (faces > ctab && faces - ctab < 0x2000u) ? (int)(faces - ctab) : 0x800;
        }
        int sv_off = n;
        int32_t px=(int32_t)cpu->read_word(m+0xC), py=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 4u), pz=(int32_t)cpu->read_word(m + game_layout.moby.pos_off + 8u);
        int billboard = (cpu->read_byte(m + moby_game.render_radius_off) & 0x80) != 0;
        uint32_t rot = cpu->read_word(m + game_layout.moby.angle_off + 0u);   /* m_Rotation (3 euler bytes) */
        /* OT-bias overlay flag: the .s billboard path (block_80022D90) computes the
         * OT index as (pz>>7) - ((int32_t)m_0x44 >> 24) clamped >= 0. A large bias
         * byte forces the index to 0 — the NEAR OT slot — so the moby draws OVER the
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
             * coords the game animates per tick (pz = depth) — fed raw, a
             * moving billboard (the level-transition tally gems flying to the
             * chest) stepped at 30 Hz regardless of --render-fps. Prefer the
             * spine's fractional position; else lerp the interp-objs prev/curr
             * at the rotation-interp clock (same guards as the moby lerp:
             * class unchanged + teleport snap). */
            double bfx=(double)px, bfy=(double)py, bfz=(double)pz;
            { double mf[3];
              if (openpete_native_interp_pos(m, mf)) { bfx=mf[0]; bfy=mf[1]; bfz=mf[2]; }
              else if (s_rot_now) {
                const openpete_interp_obj_t* o = openpete_interp_objs_find(moby_interp_key(m));
                if (o && o->have_prev && o->prev_ns != 0 && o->curr_ns > o->prev_ns) {
                    uint16_t pcls2, ccls2;
                    memcpy(&pcls2, o->prev + 0x36u, 2); memcpy(&ccls2, o->curr + 0x36u, 2);
                    int32_t pp[3], cp[3];
                    memcpy(pp, o->prev + 0x0Cu, 12); memcpy(cp, o->curr + 0x0Cu, 12);
                    double ddx=(double)cp[0]-pp[0], ddy=(double)cp[1]-pp[1], ddz=(double)cp[2]-pp[2];
                    /* px/py are SCREEN px — a 5000 guard never fires. 64 px/tick
                     * is ~4x the tally gem's flight speed; a same-class slot shift
                     * (gem inheriting another gem's slot) jumps farther and snaps.
                     * pz is DEPTH (the billboard's perspective scale) on a
                     * different unit: the credits letters fly in at ~375/tick, so
                     * it stays out of the guard. */
                    /* stale prev = a relaid-out submenu glyph paired with the old
                     * glyph that last held its pool slot → snap (leave bf at the
                     * canonical curr) so the letter appears in place instead of
                     * swimming from the previous word's position. */
                    if (pcls2 == ccls2 && ddx*ddx + ddy*ddy <= 64.0*64.0 &&
                        !moby_interp_pair_stale(o->prev_ns, o->curr_ns)) {
                        double t = (double)(s_rot_now - o->curr_ns) / (double)(o->curr_ns - o->prev_ns);
                        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
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
             * depth to pz>>1, but orders the prim in the OT by pz>>7 — i.e. the FULL
             * pz). Scale the fed depth back to full-world so billboards (e.g. the
             * level-intro "THE ADVENTURE BEGINS" text, and any sprite moby) sort vs
             * terrain / Spyro / 3D mobys on the same scale. Without this they sit at
             * half-depth — artificially near — and a billboard wrongly occludes
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
             * PM·(pos-cam) — that swizzle/scale mismatch sank 3D mobys (gems)
             * below ground (verified: gem v0 nnPrim 35-130px -> 0.1px). */
            /* Delta as ÷4.0 (not >>2) so shaded mobys (gems/props) track the smooth
             * sub-tick camera instead of stepping in 4-world-unit chunks — same fix
             * as the regular path + terrain. TR is fractional; scale unchanged.
             * Floor fix: also prefer the FRACTIONAL interpolated moby position so the
             * moby's own motion is continuous (not just the camera component). */
            double mfx=(double)px, mfy=(double)py, mfz=(double)pz;
            int fpos_hit;
            { double mf[3]; fpos_hit = openpete_native_interp_pos(m, mf); if (fpos_hit) { mfx=mf[0]; mfy=mf[1]; mfz=mf[2]; } }
            /* OPENPETE_SHADED_INTERP_MISS: shaded mobys with NO fractional
             * position (not in the spine's lerp walk → they step at 30 Hz). */
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
             * every cls-0x53 gem for ~40 frames from frameLo — does the fed
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
            /* Draw-scale (p57): apply to the FRACTIONAL TR directly — no int16 round.
             * (The earlier int16 truncation here re-quantized the just-fractional TR
             * back to a ~1-unit grid for draw-scaled shaded mobys, undoing the ÷4.0
             * delta fix for exactly those props. The GTE's IR16 saturation only
             * matters for off-screen/huge TR, which project()'s sat flag culls.) */
            if (p57){ for(int i=0;i<3;i++){ TR[i]=(double)p57 * TR[i] / 32.0; } }
            mofx=ofx; mofy=ofy;
            /* TR here = trueViewZ/2 (D=(pos-cam)>>2 then MAC<<1), or x p57/32 with
             * draw-scale: scale the fed depth back to full-world so shaded mobys
             * (gems) sort vs terrain/Spyro. p57=0 -> x2 (probe: ratio exactly 2.0);
             * p57!=0 -> x64/p57. Sign-preserving (near cull intact); depth-only. */
            vz_scale = p57 ? 64.0 / (double)p57 : 2.0;
        }
        int vzmin=1<<30, vzmax=-(1<<30);   /* model-space Z extent (HUD3D 3D-vs-flat test) */
        double rec_minfvz = 1e30;          /* nearest in-front vertex after pulls (near-floor lift) */
        double pvzmin=1e30, pvzmax=-1e30;  /* projected view-Z extent (rotation-aware half) */
        /* .s OT bias, SHADED path: the walk subtracts the SIGNED m_Rotation>>24
         * byte from the moby's OT index (0x80022D9C: t9 -= s4>>24; OT slots are
         * 128 world units). Like the regular walk's m_DepthOffset, that bias is
         * an OT ORDERING KEY, not geometry — it never enters stored depth
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
            /* TRUE per-vertex view-Z (project returns mac3). The old code threw this
             * away and stamped the moby-constant TR[2] on every vertex, collapsing
             * within-moby depth so the Z-buffer couldn't separate a moby's faces
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
            /* One-slot tie pull — see the regular walk's twin above. The
             * pedestal discs (dofs 0) rely on it for the ground they touch. */
            if (ot_centre > 0.0) fvz -= OPENPETE_MOBY_TIE_PULL_VZ;
            if (vzz > 0.0 && fvz < rec_minfvz) rec_minfvz = fvz;
            out[n].sx=(float)sx; out[n].sy=(float)sy; out[n].vz=(float)fvz; out[n].sat=sat; out[n].unr=unr;
            out[n].tvz=(float)(vzz*vz_scale);            /* pre-bias: the perspective W */
            out[n].ex=out[n].ey=out[n].ez=0.0f;   /* env-UV unused on shaded faces */
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
            R->ctab=ctab; R->ctab_len=ctab_len;
            R->vzspread = (vzmax>vzmin) ? (vzmax-vzmin) : 0;
            R->pvzspread = (pvzmax>pvzmin) ? (float)(pvzmax-pvzmin) : 0.0f;
            for (int i=0;i<3;i++) for (int j=0;j<3;j++) R->RT[i][j]=M[i][j];
        }
    }
    /* Idle-anim census (~1/s, under OPENPETE_MOBY_TIER_DBG): overridden-gem
     * composes + fresh seeds since the last print. active counts euler composes
     * (canonical + per-present re-extract), so it scales with render fps —
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

/* Native-topology shaded walk (backlog #2a) — VERIFIED, FED. Derives the shaded
 * triangle set from the face stream (func_80022A2C .L800232A8) instead of riding
 * the .s's prims: per moby, word0 packs 4 vertex indices (idx_k =
 * (w0>>{21,14,7,0})&0x1FC, >>2 = slot into this moby's projected verts).
 * tri-vs-quad = (idx2==idx3):
 *   TRI (v0,v1,v2)             kept if NCLIP1>0
 *   QUAD (v0,v1,v2)+(v1,v3,v2) kept if NCLIP1>0 || NCLIP2(v3,v1,v2)<0
 * (bit0/bit1 of word0 only select the shading MODE — gouraud vs flat — not the
 * topology, so they're ignored here.) Positions come from project_all_shaded
 * (100% A/B-verified); s_shrecs[] gives each moby's vert base + face stream.
 *
 * SLOT MAP = IDENTITY (sv[slot]). Confirmed by reading the .s's projected SXY
 * straight from PSX scratchpad 0x1F800000 on a clean shaded moby (cliff_town
 * class 0x3A, 115 verts): direct sv[slot]-vs-scratch[slot] = ~1-2px (GTE
 * truncation) for every slot, and the topology A/B hit ~90% ONSCREEN precision.
 * The earlier "permutation / 33%" reading was a phantom of the attract demo's
 * degenerate-tiny shaded mobys (6-14px, verts ~2px apart), where NN matching and
 * the direct check both break down.
 *
 * COLOUR (#2b) is now native too — shaded_lit_color() reproduces the .s's
 * directional-lighting GTE chain bit-for-bit (verified vs the +0x2A00 scratchpad:
 * flat 100%, gouraud Y-only 61-64/64, err ~0.02). word0 bit0 selects the shading
 * MODE: gouraud (per-vertex normals at model+0x10, colour-indexed by word1) vs
 * flat (one colour from word1's 3-byte face normal). word0 bit1 on a flat face
 * marks the METAL env-reflection sub-mode (block_80023720) — NOT yet derived
 * natively (same family as #1 specular reflection); rendered as basic-flat for
 * now (approximate but leak-free). The feed reads ZERO .s prim output. */
static int build_shaded_walk_tris(CPUState* cpu) {
    static proj_xy sv[8192]; project_all_shaded(cpu, sv, 8192);   /* fills sv[] + s_shrecs[] */
    if (s_sh_nohl < 0) { const char* e=getenv("OPENPETE_SHADED_NOHL"); s_sh_nohl = (e&&*e!='0'); }
    s_shmode[0]=s_shmode[1]=s_shmode[2]=0;
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
         * painter's order and stay flat HUD (drawn UNDER the player — keeps the
         * level-intro/RESCUED text behind Spyro). Static icons are always 3D → HUD3D;
         * a near-OT/UI billboard is HUD3D only if its model Z-extent is large.
         * ROTATING thin glyphs (the inventory screen spins its letter/dragon-icon
         * mobys, swinging their width into view-Z — pvzspread measures it) are
         * NOT promoted here: promotion would move them over the player (DEMO
         * MODE letters wave at pvzspread 41-95 in ordinary attract gameplay,
         * where Spyro must cover them — measured via OPENPETE_SHADED_HUDDBG,
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
         * (model+0x10, 3 signed bytes/vertex: byte0=Nz, byte1=Nx, byte2=Ny —
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
         * NOT the flat gray table — see shaded_metal_color. Read once per moby. */
        uint32_t mscolor = cpu->read_word(R->moby + moby_game.spec_colour_off);
        uint32_t mtab = moby_game.metal_coltab + (uint32_t)((int32_t)mscolor >> 21);
        uint32_t metal_w0 = cpu->read_word(mtab) & 0xFFFFFFu;
        uint32_t metal_w1 = cpu->read_word(mtab + 4u) & 0xFFFFFFu;
        /* flat-basic ABE (func_80022A2C block_8002363C): a flat-shaded moby whose flat
         * colour-table index (m_SpecularMetalColor>>22) is 0 emits its faces
         * semi-transparent (the .s adds 0x02000000 = the GPU ABE bit). Per-moby, not
         * depth-gated. (Metal-face ABE is depth(SZ3>=2048)/NCLIP-keyed and intertwined
         * with the far-darkening approximation — deferred to a calibrated pass.) */
        int flat_abe = (((int32_t)mscolor) >> 22) == 0;
        uint32_t fp = R->faces;
        for (int f = 0; f < R->fcount && s_gout.n < MAX_NTRI; f++, fp += 8u) {
            uint32_t w0 = cpu->read_word(fp), w1 = cpu->read_word(fp + 4u);
            if (w0 & 1u) { if (w0 & 2u) s_shmode[2]++; else s_shmode[1]++; } else s_shmode[0]++;
            int i0 = (int)((w0 >> 21) & 0x1FCu) >> 2;
            int i1 = (int)((w0 >> 14) & 0x1FCu) >> 2;
            int i2 = (int)((w0 >>  7) & 0x1FCu) >> 2;
            int i3 = (int)( w0        & 0x1FCu) >> 2;
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
            /* Colour: w0 bit0 selects shading MODE — flat (1) lights once from
             * w1's 3-byte face normal; gouraud (0) reads per-vertex colours
             * indexed by w1's colour fields (>>{21,14,7,0}&0x1FC, >>2 = slot). */
            uint32_t c0,c1,c2,c3; uint8_t mode;
            if (w0 & 1u) {
                int fnz=(int8_t)(w1>>24), fnx=(int8_t)(w1>>16), fny=(int8_t)(w1>>8);
                if (w0 & 2u)   /* METAL: env-reflection tint (block_80023720) */
                    c0=c1=c2=c3 = shaded_metal_color(R->RT, fnx, fny, fnz, metal_w0, metal_w1);
                else           /* flat-basic: directional light, flat table base */
                    c0=c1=c2=c3 = shaded_lit_color(R->RT, fnx, fny, fnz, R->base, 0);
                mode = (w0 & 2u) ? 2 : 1;
            } else if (R->ctab) {
                /* STATIC-GOURAUD: w1 fields are BYTE offsets into the baked
                 * colour table at [model+8] (.s: t2 = ((w1>>sh)&0x1FC) + s4;
                 * colour word used verbatim). */
                uint32_t o0=(w1>>21)&0x1FCu, o1=(w1>>14)&0x1FCu, o2=(w1>>7)&0x1FCu, o3=w1&0x1FCu;
                uint32_t L=(uint32_t)R->ctab_len;
                c0 = o0<L ? cpu->read_word(R->ctab+o0)&0xFFFFFFu : 0x808080u;
                c1 = o1<L ? cpu->read_word(R->ctab+o1)&0xFFFFFFu : 0x808080u;
                c2 = o2<L ? cpu->read_word(R->ctab+o2)&0xFFFFFFu : 0x808080u;
                c3 = o3<L ? cpu->read_word(R->ctab+o3)&0xFFFFFFu : 0x808080u;
                mode = 0;
            } else {
                int j0=(int)((w1>>21)&0x1FCu)>>2, j1=(int)((w1>>14)&0x1FCu)>>2;
                int j2=(int)((w1>> 7)&0x1FCu)>>2, j3=(int)( w1     &0x1FCu)>>2;
                c0 = j0<pcn?pcol[j0]:0x808080u; c1 = j1<pcn?pcol[j1]:0x808080u;
                c2 = j2<pcn?pcol[j2]:0x808080u; c3 = j3<pcn?pcol[j3]:0x808080u;
                mode = 0;
            }
            /* flat-basic faces inherit the moby's flat ABE (mode 1 only); gouraud
             * (mode 0) and metal (mode 2) stay opaque here (metal ABE deferred).
             * blend=1 == ABR0 (50% B/2+F/2), the untextured-ABE default. */
            int sh_blend = (mode == 1 && flat_abe) ? 1 : 0;
            int mt0 = s_gout.n;
            proj_xy* p0 = &base[i0]; proj_xy* p1 = &base[i1]; proj_xy* p2 = &base[i2];
            double ncl1 = tri_nclip(p0->sx,p0->sy,p1->sx,p1->sy,p2->sx,p2->sy);  /* float backface cull (see build_walk_tris) */
            if (i2 == i3) {                                       /* TRI */
                if (ncl1 > 0.0)
                    emit_wtri(p0,p1,p2, c0,c1,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,sh_blend);
            } else if (i3 < vc) {                                 /* QUAD */
                proj_xy* p3 = &base[i3];
                double ncl2 = tri_nclip(p3->sx,p3->sy,p1->sx,p1->sy,p2->sx,p2->sy);
                if (ncl1 > 0.0 || ncl2 < 0.0) {
                    emit_wtri(p0,p1,p2, c0,c1,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,sh_blend);
                    if (s_gout.n < MAX_NTRI)
                        emit_wtri(p1,p3,p2, c1,c3,c2, 0,0,0,0,0,0, 0,0,0,0,0,16,sh_blend);
                }
            }
            { /* OPENPETE_SHADED_FACEDBG="cls,frameLo": per-face NCLIP forensics for
               * one class over 2 VBLs — float ncl vs the GTE's integer NCLIP (which
               * runs on truncated SXY), to catch sign flips on near-edge-on faces. */
                static long s_fd_cls = -2, s_fd_f0 = -1;
                if (s_fd_cls == -2) {
                    const char* e = getenv("OPENPETE_SHADED_FACEDBG");
                    s_fd_cls = -1;
                    if (e && *e) { char* c = NULL; s_fd_cls = strtol(e, &c, 0);
                                   if (c && *c == ',') s_fd_f0 = strtol(c+1, NULL, 0); }
                }
                if (s_fd_cls >= 0 && (int)s_cur_moby_cls == (int)s_fd_cls &&
                    (int)openpete_frame >= s_fd_f0 && (int)openpete_frame < s_fd_f0 + 2) {
                    long i0i = (long)((int)p1->sx - (int)p0->sx) * ((int)p2->sy - (int)p0->sy)
                             - (long)((int)p2->sx - (int)p0->sx) * ((int)p1->sy - (int)p0->sy);
                    fprintf(stderr, "[shface vbl=%u] moby=%08x cls=%03x face=%d idx=%d,%d,%d,%d "
                            "p0=(%.2f,%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f)%s ncl1=%.3f ncl2=%.3f int1=%ld emit=%d\n",
                            openpete_frame, R->moby, s_cur_moby_cls, f, i0, i1, i2, i3,
                            (double)p0->sx, (double)p0->sy, (double)p1->sx, (double)p1->sy,
                            (double)p2->sx, (double)p2->sy, i2==i3?" TRI":" QUAD",
                            ncl1, i2==i3 ? 0.0 :
                                (i3<vc ? tri_nclip(base[i3].sx,base[i3].sy,p1->sx,p1->sy,p2->sx,p2->sy) : -999.0),
                            i0i, s_gout.n - mt0);
                }
            }
            for (int t = mt0; t < s_gout.n; t++) s_wtri_mode[t] = mode;
        }
        /* ABR-CARVE-OUT: the game builds metal SimpleModel faces
         * semi-transparent (cmd 0x2A) then REMOVES ABE per-face on a runtime depth
         * (SZ3>=2048) / NCLIP (MAC0) test, and rates the kept ones via a per-moby
         * DR_MODE (metal-only->ABR1 additive). We draw metal faces opaque. This was
         * flagged as a mismatch by the static recomp read, but it is NOT a visible
         * bug: on a 54-gem demo frame (f3494) the native-vs-PsyCross A/B diff is
         * mean 0.9 with no systematic per-gem divergence (only sub-pixel edge jitter)
         * and the user confirms gems look correct — the unmodelled ABE-removal gate
         * clears it for these faces, and any kept additive shimmer is subtle and also
         * dropped by PsyCross. Left opaque; revisit only if a metal moby ever looks
         * wrong. (The earlier per-frame ABR-FIXME alarm here was a false-positive
         * firehose — it counted every metal-mode face without the ABE-removal gate.) */
    }
    return s_gout.n;
}

/* Ground-truth check for the gouraud per-vertex lighting: the .s writes its
 * per-vertex lit colours to the scratch window the descriptor names, one
 * word per vertex, for the LAST gouraud moby it processed (overwritten each
 * moby). Recompute pcol[] for each rec, find the one whose pcol matches the
 * scratchpad (that's the last gouraud moby), and dump element-wise — zero
 * tri-match / cross-moby confound. Call from the verify path. */
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

/* Cutscene actors draw in FRONT of terrain (the depth-isolated ACTOR pass in
 * native_render.c). The gate is the game's own, through moby_game: while its
 * cutscene renderer owns the draw it renders only those actors, which the
 * PSX's coarse OT bucketing painted over terrain they interpenetrate. A game
 * with no such renderer answers 0 and the front pass never arms.
 * OPENPETE_ACTOR_FRONT=0 disables it (A/B kill-switch; the corpus guard flips
 * it). Called at both feed sites (canonical and sub-tick re-extract), after
 * begin_frame's reset.
 *
 * Safe at ALL LOD levels: draw_native_scene front-passes ONLY the tagged
 * cutscene actors (OPENPETE_SCENE_ACTOR_FRONT, stamped by pointer match above)
 * and clears depth only WITHIN THEIR SCREEN BBOX, so extended LOD's un-culled
 * distant level mobys keep their true terrain occlusion (the whole-frame clear
 * used to punch them through). */
static void moby_native_set_actor_front(CPUState* cpu) {
    static int s_en = -1;
    if (s_en < 0) { const char* e = getenv("OPENPETE_ACTOR_FRONT"); s_en = (e && *e) ? (*e != '0') : 1; }
    openpete_native_scene_set_actor_front(s_en && moby_cutscene_owns_draw(cpu));
}

/* The shaded pass's own entry, not the hooked address. */
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

static int on_draw_shaded(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_shaded_bracket, &startp, &endp, &feed, &verify))
        return 0;
    moby_view_shd_build(cpu);

    if (feed) {                                 /* feed shaded mobys to the scene — fully native */
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
        spyro_native_canon_capture_range(cpu, 0x2400u, 0x800u);
        spyro_native_canon_capture_camera(cpu, SPYRO_PROD_SHADED);
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
    return moby_game.simple_replay && s_canon_simple_valid &&
           spyro_native_canon_epoch_fresh(s_canon_simple_epoch);
}
static void moby_run_draw_simple(CPUState* cpu, uint32_t addr) {
    (void)addr;
    s_simple_ntri = moby_game.simple_pass(cpu);
    extern int openpete_speculative;
    if (!openpete_speculative) {
        s_canon_simple_valid = 1;
        s_canon_simple_epoch = spyro_native_canon_epoch_stamp();
    }
}
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

/* Compute the PREV rendered keyframe pose for a moby — the second input to the
 * two-pass sub-tick SHAPE lerp in reextract_record_pass. Returns MOBY_PREV_LERP
 * (and fills prev + alpha) when the moby has a lerpable prev snapshot;
 * MOBY_PREV_HOLD (prev + alpha filled) when the two clips have no vertex
 * correspondence, so the pass shows the prev pose whole until alpha 1;
 * MOBY_PREV_SNAP when it should show the curr pose from alpha 0: interp off,
 * spawn / no prev interval, or a cutscene path-follower's clip switch.
 * The pose is the one the cull drew from
 * the prev snapshot's animation state bytes (moby_pose_derive, the rule the
 * live pose takes too), so on a tick where the moby blends into another
 * animation or into keyframe 0 the prev pose is that blend, keyframe B out of
 * the next animation's own table. Uses the SAME per-object clock (s_rot_now)
 * and distinct-roll alpha as the moby rotation interp, so
 * shape/rotation/position all interpolate on one timeline.
 *
 * Clip switches (prev m_Animation != curr) ARE crossfaded when the two animations
 * share the same vertex COUNT — a moby model's animations deform one shared mesh, so
 * an equal count means vertex i is the same physical vertex in both keyframe tables,
 * and a model-space lerp prev→curr stays on-model (the fixed-topology argument that
 * makes Spyro's cross-clip lerp safe, applied per moby). When the counts differ the
 * meshes genuinely differ, and the pass HOLDS the prev pose whole — its own
 * header, vertex count, face list and keyframe patch list — for every present
 * below alpha 1, reaching the curr pose at alpha 1, where every other channel
 * reaches its canonical value for this tick. prev->header is the previous clip's
 * header so the decoder can use its own ShortShift (the two clips' s5 may
 * differ; the decoded coords are real model-space either way). */
/* What the sub-tick pass may do with a clip switch, shared by the pass and the
 * census so both read one rule. CROSS: the two clips deform one shared mesh
 * (equal vertex count), so vertex i is the same physical vertex in both and a
 * model-space lerp prev→curr stays on-model. CUTPATH: a cutscene path-follower,
 * whose flight clips carry large baked whole-body translation, so crossfading
 * two spatially-distant poses lurches the whole model. COUNTSW: the clips have
 * different vertex counts, so no vertex correspondence exists between them. */
typedef enum { MOBY_CLIP_CROSS = 0, MOBY_CLIP_CUTPATH, MOBY_CLIP_COUNTSW } moby_clip_t;

/* moby_prev_keyframe's verdict; also the prevkf field of the [moby-anim]
 * trace, which the sub-tick harness bins on. */
enum { MOBY_PREV_SNAP = 0, MOBY_PREV_LERP = 1, MOBY_PREV_HOLD = 2 };

static moby_clip_t moby_clip_switch_kind(CPUState* cpu, uint32_t moby,
                                         uint32_t header_prev, uint32_t header_curr,
                                         int* count_p, int* count_c) {
    *count_c = cpu->read_byte(header_curr + game_layout.moby.anim_vcount_hi) & 0xFFu;
    *count_p = cpu->read_byte(header_prev + game_layout.moby.anim_vcount_hi) & 0xFFu;
    if (moby_game.cutpath_actor && moby_game.cutpath_actor(moby)) return MOBY_CLIP_CUTPATH;
    return (*count_p == *count_c) ? MOBY_CLIP_CROSS : MOBY_CLIP_COUNTSW;
}

/* OPENPETE_MOBY_ANIMSW_CENSUS=1: one line per queued record per canonical tick
 * whose animation clip changed since the tick before, with the switch's kind
 * and the two clips' vertex counts. Sits on the canonical pass, so one replay
 * censuses every tick at replay speed; the sub-tick pass evaluates the same
 * rule once per present instead. */
static void moby_animsw_census(CPUState* cpu, uint32_t moby) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_ANIMSW_CENSUS");
                    s_on = (e && *e && *e != '0'); }
    if (!s_on || moby < 0x80000000u) return;
    /* Engagement tally: a census that reports no switch is only evidence once
     * the stages it passed through are visible. */
    static long n_rec = 0, n_obj = 0, n_prev = 0, n_cls = 0, n_pose = 0, n_sw = 0;
    static unsigned at = 0;
    n_rec++;
    if (openpete_frame >= at + 400u) {
        at = openpete_frame;
        fprintf(stderr, "[moby-animsw-census vbl=%u] stage recs=%ld obj=%ld prev=%ld "
                        "sameclass=%ld pose=%ld switches=%ld\n",
                openpete_frame, n_rec, n_obj, n_prev, n_cls, n_pose, n_sw);
    }
    const openpete_interp_obj_t* o = openpete_interp_objs_find(moby_interp_key(moby));
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
    if (pp.header == pc.header) return;
    n_sw++;
    int np = 0, nc = 0;
    moby_clip_t k = moby_clip_switch_kind(cpu, moby, pp.header, pc.header, &np, &nc);
    static const char* const kind[] = { "cross", "cutpath", "countsw" };
    fprintf(stderr, "[moby-animsw-census vbl=%u] m=%08x cls=%03x kind=%s anim=%u->%u "
                    "hdr=%08x->%08x n=%d->%d\n",
            openpete_frame, moby, (unsigned)ccls, kind[k],
            (unsigned)o->prev[game_layout.moby.anim_state_off],
            (unsigned)o->curr[game_layout.moby.anim_state_off],
            pp.header, pc.header, np, nc);
}

static int moby_prev_keyframe(CPUState* cpu, uint32_t moby, uint32_t header_curr,
                              moby_pose_t* prev, double* alpha) {
    if (!s_rot_now) return 0;                                  /* interp off → snap */
    /* The prev pose is the decoder's second stream input, so it has to be
     * expressible in the form this game's record uses for a keyframe: a
     * pointer into the model's frame array where the game's provider rebuilds
     * records from gamestate, or the keyframe's leading stream word where the
     * cull copies that word into the record (rec_frame_is_word). A game with
     * neither form has no way to name the prev keyframe, so the sub-tick pass
     * replays the record the cull wrote and holds the pose. */
    if (!moby_game.host_reveal && !game_layout.moby.rec_frame_is_word) return 0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(moby_interp_key(moby));
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    /* Occupant guard: prev snapshot must be the SAME class as the live moby —
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
    int verdict = MOBY_PREV_LERP;
    if (prev->header != header_curr) {                        /* clip switch */
        int count_p = 0, count_c = 0;
        switch (moby_clip_switch_kind(cpu, moby, prev->header, header_curr,
                                      &count_p, &count_c)) {
        case MOBY_CLIP_CUTPATH: s_animsw_snap++;  return MOBY_PREV_SNAP;
        case MOBY_CLIP_COUNTSW: s_animsw_snap++;  verdict = MOBY_PREV_HOLD; break;
        default:                s_animsw_cross++; break;
        }
    }
    double t = (double)(s_rot_now - o->curr_ns) / (double)(o->curr_ns - o->prev_ns);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    *alpha = t;
    return verdict;
}

/* ---- moby occlusion cull (native-side, display-only) ------------------------
 *
 * Skips the sub-tick re-extraction (decode + project + walk + draw) of a moby
 * whose whole screen rect is provably behind nearer WORLD-opaque depth — the
 * depth test would erase every pixel of it anyway (the game's canonical cull
 * is a view-cone + distance test with no occlusion knowledge, so enclosed
 * areas queue every moby behind the walls). Reads the occlusion grid via
 * openpete_native_occl_query; the CANONICAL cull pass and its gameplay-
 * readable writes are untouched (determinism-safe by construction).
 *
 * Per-moby cache: the last extraction's projected bounds — PSX-screen rect +
 * nearest FULL-WORLD view-Z, straight off s_proj (so no per-class scale
 * reverse-engineering). A hit older than 2 ticks is ignored (the moby
 * re-extracts, refreshing the cache — so a permanently-hidden moby still
 * pays full extraction every ≤2 ticks, ~15 Hz instead of every present).
 * Staleness within those 2 ticks is absorbed conservatively: the rect
 * inflates with the camera's measured pan speed (set_occl_pan) + a generous
 * own-motion allowance, and the view-Z gets a per-tick translation slack on
 * top of the grid's own OPENPETE_MOBY_OCCL_SLACK. HUD/UI-routed mobys are
 * exempt (drawn depth-off — never depth-occluded). Direct-mapped cache: a
 * collision only evicts (→ that moby extracts normally), never lies. */
typedef struct { uint32_t moby; float x0, y0, x1, y1, vz_min; unsigned tick;
                 float mot, mot_vz;      /* measured screen / view-Z motion per VBL (px, world units) */
                 /* Anchor for the extraction-free bounds refresh below: the
                  * record's origin projection, view-space TR.z, model radius
                  * and class at the last REAL extraction. Written only there,
                  * so a refresh re-derives from a fixed reference and cannot
                  * drift however long a moby stays hidden. */
                 float asx, asy, atrz, arad; uint16_t acls; } occl_rect_t;
#define OCCL_HASH_N 1024                       /* power of two; ~500 live mobys max */
static occl_rect_t s_occl_cache[OCCL_HASH_N];
static float s_occl_pan_px = 0.0f;             /* px/tick pan allowance (orchestrator) */
/* s_occl_culled / s_occl_seen: census (records, re-extract + reveal passes) —
 * declared above append_revealed_regular, which is also a cull site. */

void moby_native_set_occl_pan(float px_per_tick) { s_occl_pan_px = px_per_tick; }

static unsigned occl_slot(uint32_t moby) {
    return ((moby >> 3) * 2654435761u >> 16) & (OCCL_HASH_N - 1);
}

static long s_occl_f[4];   /* feeder-side pre-query outcomes: ui-exempt / no-cache / stale-cache / refreshed */

static int hold_project_origin(CPUState* cpu, const double tr[3], double* sx, double* sy);

/* ---- extraction-free bounds refresh ---------------------------------------
 * A culled record never reaches occl_cache_update, so its bounds age out and
 * the age test forces a full extraction — decode, project and walk — purely to
 * re-derive a screen rect whose only consumer is the cull that then skips it
 * again. That cycle capped the cull's value at one skipped present in three.
 *
 * The rect can be rebuilt without any of it: project the record ORIGIN through
 * the current camera (the TR-only slice — no euler compose, no decode) and
 * carry the anchor's rect around it, rescaled by the view-Z ratio, unioned with
 * the model's own radius box. The radius bounds every pose and rotation of the
 * model, so the union contains the true projected extent whatever the moby has
 * animated into since. Both bounds are taken conservatively — an over-large
 * rect or an over-near view-Z can only fail to cull, never cull something
 * visible.
 *
 * Nothing is written: repeated refreshes re-derive from the same anchor, so
 * they cannot drift, and the measured-motion fields (which compare successive
 * extractions) never see a rect built by this second method. Two guards stand
 * in for the staleness bound that is being removed — m_Class is re-checked
 * because moby slots are recycled and a reused address would otherwise inherit
 * the previous occupant's radius, and the anchor expires outright after
 * OCCL_ANCHOR_MAX_VBL so no rect survives a scene indefinitely. */
#define OCCL_ANCHOR_MAX_VBL 64u

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
     * NEAREST vertex, so it under-estimates the origin's own ratio — nearer,
     * conservative — and a full radius comes off again to bound any rotation.
     * The anchor rect rescaled is kept as the alternative and the nearer of the
     * two wins. */
    double k   = (double)e->vz_min / (double)e->atrz;
    double vzr = k * (tr[2] - (double)e->arad);
    double vza = (double)e->vz_min / scale;
    *vz = (float)(vzr < vza ? vzr : vza);
    return 1;
}

/* Cull verdict for one record BEFORE any decode work. 1 = provably hidden. */
static int occl_record_culled(CPUState* cpu, uint32_t moby) {
    if (openpete_ui_moby_is_addr_static(moby) || openpete_ui_moby_is_addr_ui(moby))
        { s_occl_f[0]++; return 0; }                /* HUD/UI: drawn depth-off, never occluded */
    const occl_rect_t* e = &s_occl_cache[occl_slot(moby)];
    if (e->moby != moby) { s_occl_f[1]++; return 0; }   /* no cached bounds → extract (refreshes) */
    unsigned age = openpete_frame - e->tick;
    float rx0 = e->x0, ry0 = e->y0, rx1 = e->x1, ry1 = e->y1, rvz = e->vz_min;
    float own;
    if (age > 2u) {
        if (!occl_refresh_bounds(cpu, moby, e, &rx0, &ry0, &rx1, &ry1, &rvz))
            { s_occl_f[2]++; return 0; }            /* no usable anchor → extract (refreshes) */
        s_occl_f[3]++;
        /* The refreshed rect is built around the moby's CURRENT origin and hard-
         * bounded by its model radius, so no own-motion allowance is owed — only
         * the camera's pan across this present. */
        age = 0u;
        own = 0.0f;
    } else {
        /* Own-motion allowance ≈ 256 world units/tick at an overestimated H≈400:
         * px = 400·256/vz, scaled by the view's H scale (the pan allowance is
         * measured at the canonical H and scaled the same way). Camera pan
         * rides s_occl_pan_px (measured per tick).
         * Own-motion allowance: the 256-u/tick model floor OR the moby's MEASURED
         * screen motion ×1.5, whichever is larger — fast movers (birds ~750 u/tick)
         * beat the model by 3× and could stay culled ≤3 presents while emerging
         * from cover (found by the OCCL_VERIFY audit, magic-crafters f2154). */
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

/* ---- occl-verify audit mode (OPENPETE_MOBY_OCCL_VERIFY=1, inert otherwise) --
 * When the cull says "hidden", the pass extracts the moby ANYWAY and re-runs the
 * query on the bounds that extraction just produced. A fresh verdict of
 * "visible" means the cached bounds would have culled a moby the depth test
 * would NOT fully erase — a would-be single-present flicker. Census printed once
 * per second. The moby still draws in verify mode (verdict overridden), so a
 * live run with verify on is also the zero-flicker reference.
 *
 * A would-culled record does NOT write its cache entry here, exactly as it would
 * not in a production run. Refreshing it would keep every entry young, the
 * extraction-free refresh path would never engage, and the audit would report on
 * a state the cull never reaches — so the fresh bounds are taken straight off the
 * projection instead of through the cache. */
static int occl_verify_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_OCCL_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static long s_ver_would = 0;   /* stale-cache verdicts said CULL */
static long s_ver_wrong = 0;   /* ...of which fresh bounds said VISIBLE */

static void occl_proj_bounds(const proj_xy* p, int n, float* px0, float* py0,
                             float* px1, float* py1, float* pvz);

/* Query one explicit set of bounds with the age-0 inflation. */
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
static int occl_verify_begin(uint32_t moby) {
    if (!occl_verify_on()) return 0;
    s_ver_would++;
    s_ver_snap = s_occl_cache[occl_slot(moby)];
    return 1;
}
static void occl_verify_end(uint32_t moby, const proj_xy* p, int n) {
    if (n <= 0) return;                 /* no verts projected → nothing drawable */
    float x0, y0, x1, y1, vz;
    occl_proj_bounds(p, n, &x0, &y0, &x1, &y1, &vz);
    if (occl_query_at(x0, y0, x1, y1, vz, s_ver_snap.mot, s_ver_snap.mot_vz)) return;
    /* Classify: a fresh rect fully outside the visible extent produces ZERO
     * pixels regardless of the verdict — the conservative inflation merely
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

/* Refresh the cache from a record's freshly projected verts (s_proj carries
 * PSX screen px + FULL-WORLD view-Z — exactly the query's units). Bounds over
 * ALL verts ⊇ bounds over the drawn subset, so the rect is conservative. */
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

static void occl_cache_update(CPUState* cpu, uint32_t moby, const proj_xy* p, int n,
                              const double tr[3], uint32_t header) {
    if (n <= 0) return;
    float x0, y0, x1, y1, vzm;
    occl_proj_bounds(p, n, &x0, &y0, &x1, &y1, &vzm);
    occl_rect_t* e = &s_occl_cache[occl_slot(moby)];
    /* Measured motion (px + view-Z per VBL) from the previous bounds of the
     * SAME moby: rect-centre delta over the refresh interval. Slot collision
     * or first sighting → 0 (the model floor covers it). */
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
 * its screen size (< the hold threshold) — imperceptible at speck scale, and
 * the pose lerp it skips is equally sub-pixel there. Gates: screen extent
 * from the occl cache (< OPENPETE_MOBY_HOLD_PX, default 10, 0 = off), not
 * HUD/UI (special projection), origin safely in front of the camera, same
 * canonical epoch (a new tick always re-extracts — gameplay state only
 * changes at ticks, so no discontinuity can hide inside an epoch).
 * Display-only. */
#define HOLD_TRI_MAX  32768
#define HOLD_MOBY_MAX 512
static ntri_t   s_hold_tri[HOLD_TRI_MAX];
static uint32_t s_hold_fkey[HOLD_TRI_MAX];
static uint8_t  s_hold_mode[HOLD_TRI_MAX];
typedef struct { uint32_t moby; unsigned epoch; int off, n, spec;
                 double sx, sy, trz;              /* captured origin projection + view-Z */
                 double H;                        /* projection distance the tris were built with */
                 uint16_t cls;                    /* m_Class at capture (provenance tag) */
                 uint8_t front; } hold_t;         /* per-moby emit flags at capture */
static hold_t   s_hold[HOLD_MOBY_MAX];
static int      s_hold_n, s_hold_tri_n;
static unsigned s_hold_epoch;

static int hold_px_max(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_HOLD_PX"); v = e ? atoi(e) : 10; if (v < 0) v = 0; }
    return v;
}
static hold_t* hold_find(uint32_t moby, int spec) {
    for (int i = 0; i < s_hold_n; i++)
        if (s_hold[i].moby == moby && s_hold[i].spec == spec) return &s_hold[i];
    return NULL;
}
/* Screen projection of a record ORIGIN (v=0): sx = OFX + TRx·H/TRz. The
 * m_Scale shift cancels in the x/z ratio, so raw TR is exact here. */
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

/* Sub-tick re-extract (high-FPS spine): render the moby scene re-derived from
 * gamestate through the (interpolated) g_Camera the orchestrator wrote, so
 * mobys move COHERENTLY with the interpolated terrain (no swimming) instead of
 * snapping to the baked tick-N records.
 *
 * Render set = the moby addresses the .s REGULAR cull recorded this tick (its
 * frustum/LOD verdict, reused as-is — no re-cull, no m_WasDrawn write); each is
 * REBUILT host-side via build_moby_record_fields (M/TR re-derived from
 * g_Camera, validated bit-exact vs the .s records by OPENPETE_MOBY_BUILDDBG)
 * and re-projected. The SHADED path (build_shaded_walk_tris → project_all_shaded)
 * already reads g_Camera, so it interpolates for free. Per-moby own-motion
 * (position/anim) lerp from the snapshot table is a later add (step 3b); for
 * now each moby holds its tick-N world pose but projects through the
 * interpolated camera — coherent with the rest of the scene. Extract-only. */
/* One record-queue pass: read the cull records at the per-pass base, rebuild +
 * re-project each moby through the interpolated camera, walk + push. mat="REG"
 * snapshots s_reg_moby (no dedup); mat="SP" makes build_walk_tris dedup vs
 * s_reg_moby (so only specular-ONLY mobys feed). Returns the record count. */
/* Re-base one replayed cull record from the camera the tick composed it
 * against onto the camera this present stages.
 *
 * A cull that stores view-space records hands the replay a matrix M = Rt*Rm,
 * the camera rotation times the moby's own (mobydraw.s:355-359), and a
 * translation TR = S*(Rt*d)/4096, where d is the camera-relative moby position
 * in quarter-world units in the GTE's axis order — ((camY-mobyY)/4,
 * (camZ-mobyZ)/4, (mobyX-camX)/4), mobydraw.s:79-94, transformed and left in
 * the GTE translation at mobydraw.s:125-130,189 — and S is the per-moby size
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
 * back from the record's own inputs — the animation header's scale byte is the
 * shift the cull applied, and the record's low byte its GPF factor only on a
 * game whose cull left the translation unscaled — because only the
 * camera-motion term needs it; the N*TR term carries S already.
 *
 * A present whose staged camera words equal the tick's (the canonical present,
 * or alpha 1) returns with the record untouched, so that leg stays bit-exact.
 * Host-side only: the staged camera is a sub-tick view copy and no guest word
 * is written. */
/* The moby's own world-position offset between the pose the record encodes and
 * the pose this present displays, in world units.
 *
 * The cull composed the record from the moby's position at the tick, which is
 * the snapshot table's curr; the present displays lerp(prev, curr, t), so the
 * offset is (prev - curr)*(1 - t). t is the present's phase past the curr
 * stamp over the roll interval, capped at one tick, so a pair whose two
 * distinct states are further apart sweeps the whole delta over the tick and
 * still lands exactly on curr at t = 1. Returns 0 (and leaves out[] untouched)
 * when the object has no usable pair, when its slot changed class between the
 * two states, or when the pair is stale — a multi-tick gap is a
 * discontinuity, not motion, and snapping keeps the record's own pose.
 * Returns 1 with a zero out[] for a moby that is static in the world, which is
 * what keeps such a record bit-identical. */
int moby_record_world_offset(uint32_t moby, double out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(moby_interp_key(moby));
    if (!o || !o->have_prev || o->prev_ns == 0 || o->curr_ns <= o->prev_ns) return 0;
    uint16_t pcls, ccls;
    memcpy(&pcls, o->prev + game_layout.moby.class_off, 2);
    memcpy(&ccls, o->curr + game_layout.moby.class_off, 2);
    if (pcls != ccls) return 0;
    if (moby_interp_pair_stale(o->prev_ns, o->curr_ns)) return 0;
    double period = (double)(o->curr_ns - o->prev_ns);
    if (period > (double)MOBY_TICK_PERIOD_NS) period = (double)MOBY_TICK_PERIOD_NS;
    double t = (double)(openpete_interp_present_now() - o->curr_ns) / period;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    int32_t pv[3], cv[3];
    memcpy(pv, o->prev + game_layout.moby.pos_off, 12);
    memcpy(cv, o->curr + game_layout.moby.pos_off, 12);
    for (int i = 0; i < 3; i++)
        out[i] = ((double)pv[i] - (double)cv[i]) * (1.0 - t);
    return 1;
}

static void record_rebase_to_present(CPUState* cpu, moby_fields* f, uint32_t moby) {
    if (!s_tickcam_have || f->header < 0x80000000u) return;

    int Rp[3][3];
    ngte_read_matrix(cpu, game_layout.cam.proj_matrix, Rp);
    int32_t pp[3];
    for (int i = 0; i < 3; i++)
        pp[i] = (int32_t)cpu->read_word(game_layout.cam.position + (uint32_t)i * 4u);
    double mw[3];
    moby_record_world_offset(moby, mw);
    if (memcmp(Rp, s_tickcam_M, sizeof Rp) == 0 &&
        memcmp(pp, s_tickcam_pos, sizeof pp) == 0 &&
        mw[0] == 0.0 && mw[1] == 0.0 && mw[2] == 0.0) return;

    double Ti[3][3];
    if (!mat3_inverse(s_tickcam_M, Ti)) return;

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
        ((double)pp[1] - (double)s_tickcam_pos[1]) / 4.0 - mw[1] / 4.0,
        ((double)pp[2] - (double)s_tickcam_pos[2]) / 4.0 - mw[2] / 4.0,
       -((double)pp[0] - (double)s_tickcam_pos[0]) / 4.0 + mw[0] / 4.0,
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

/* Led coherence (runahead): build_moby_record_fields read the LIVE moby's
 * anim state — under runahead that is the restored REAL tick, which is about
 * the table's PREV (led-1): the sub-tick pose pair degenerated and keyframe
 * animations stepped at the tick rate while positions (already table-driven)
 * stayed smooth. Re-derive the CURR pose from the snapshot table's curr blob,
 * by the same rule as the live build — identical bytes when runahead is off
 * (curr was captured from this very moby at the tick). Same guards as
 * moby_prev_keyframe. */
/* Overwrite a record's pose fields from a derived pose, keeping w0's other
 * bytes: the m_DepthOffset OT pull in bits 16-23 and the GPF scale in the low
 * byte both belong to the record, not to the pose. */
static void moby_fields_set_pose(moby_fields* f, const moby_pose_t* p) {
    f->header   = p->header;
    f->header_b = p->header_b;
    f->frameA   = p->frame_a;
    f->frameB   = p->frame_b;
    f->w0       = (f->w0 & ~0xFF00u) | ((p->blend & 0xFFu) << 8);
}

/* The pose a sub-tick present shows for a record whose moby switched into a
 * clip with a different vertex count. No vertex correspondence exists between
 * the two meshes, so there is no pose between them: show the tick before's
 * pose whole: its own header, vertex count, face list and keyframe patch
 * list — and reach this tick's pose at alpha 1, the instant every other
 * channel reaches its canonical value for this tick. Translation and rotation
 * keep interpolating: the keyframe stream is model-space only and the world
 * position is the moby's own, which the guest steps per tick independently of
 * the clip (the cutscene path-follower, whose clips DO carry baked whole-body
 * translation, snaps to the current pose instead).
 *
 * Returns the model whose own table resolves that pose's keyframe patch list,
 * for a game that hangs the list off the cull record, and 0 when the record
 * keeps the pose it already carries. */
static uint32_t moby_fields_hold_prev(CPUState* cpu, uint32_t moby, moby_fields* f,
                                      const moby_pose_t* prev, int verdict, double alpha) {
    if (verdict != MOBY_PREV_HOLD || alpha >= 1.0) return 0u;
    moby_fields_set_pose(f, prev);
    uint32_t cls = cpu->read_half(moby + game_layout.moby.class_off);
    return cpu->read_word(game_layout.moby.model_table + cls * 4u);
}

static void moby_fields_pose_from_snapshot(CPUState* cpu, uint32_t moby, moby_fields* f) {
    if (!s_rot_now) return;
    const openpete_interp_obj_t* oc = openpete_interp_objs_find(moby_interp_key(moby));
    if (!oc) return;
    uint16_t ccls = 0;
    memcpy(&ccls, oc->curr + 0x36, 2);
    if (ccls != (uint16_t)cpu->read_half(moby + game_layout.moby.class_off)) return;
    uint32_t model = cpu->read_word(game_layout.moby.model_table + (uint32_t)ccls * 4u);
    moby_pose_t pose;
    if (model < 0x80000000u ||
        !moby_pose_derive(cpu, model, oc->curr + game_layout.moby.anim_state_off, &pose)) return;
    /* keep the snapshot's m_DepthOffset in bits 16-23 (OT pull) */
    f->w0 = (uint32_t)oc->curr[game_layout.moby.angle_off + 3u] << 16;
    moby_fields_set_pose(f, &pose);
}

static int reextract_record_pass(CPUState* cpu, const char* mat) {
    int is_spec = (mat && mat[0] == 'S' && mat[1] == 'P');
    s_walk_in_rex = 1;
    static uint32_t mobys[MAX_RECORDS], recs[MAX_RECORDS]; int nm = 0;
    /* Tier-2 hold pool is per canonical epoch: a new tick invalidates every
     * hold (its first present re-extracts everything and re-captures). */
    unsigned epoch = spyro_native_canon_epoch_stamp();
    if (epoch != s_hold_epoch) { s_hold_epoch = epoch; s_hold_n = 0; s_hold_tri_n = 0; }
    static struct { const hold_t* h; double dx, dy, vzs; } replays[MAX_RECORDS];
    int nreplay = 0;
    static uint32_t replay_moby[MAX_RECORDS]; int nreplay_moby = 0;
    s_rec_base = game_layout.moby.rec_base ? game_layout.moby.rec_base : s_rec_base_canon;
    if (!s_rec_base) { s_walk_in_rex = 0; return 0; }
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
         * untouched. Culled REG mobys still join the SPEC dedup set below —
         * their specular twin is equally hidden. */
        /* OPENPETE_MOBY_INTERP_MISS: name the record mobys with NO interp-objs
         * snapshot (they present at 30 Hz — the capture walk never saw them).
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
         * its built tris translated by the origin-projection delta — skipping
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
            /* No host record builder on this game: replay the record the cull
             * wrote, read exactly as the canonical pass read it. The pose is
             * then the one the tick drew — identical to canonical at alpha 1.
             * Its matrix and translation are view-space against the camera the
             * tick composed them with, so record_rebase_to_present turns them
             * onto the camera this present stages; without that the record
             * would be the one thing in the scene that does not follow the
             * interpolated view. */
            moby_record_t LR;
            if (!moby_game.read_record(cpu, recs[i], &LR)) continue;
            memset(&f, 0, sizeof f);
            f.header  = LR.header;  f.header_b = LR.header_b;
            f.frameA  = LR.frame_a; f.frameB   = LR.frame_b;
            f.w0      = LR.w0;
            f.mscol   = LR.material;
            f.metal   = (int)((LR.material >> 24) & 0xFFu);
            memcpy(f.M, LR.M, sizeof f.M);
            for (int k = 0; k < 3; k++) {
                f.TR_raw[k]  = LR.TR_raw[k];
                f.TR_frac[k] = (double)LR.TR_raw[k];
            }
            record_rebase_to_present(cpu, &f, mobys[i]);
        } else {
        { int bok;
          uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          bok = build_moby_record_fields(cpu, mobys[i], &f);
          if (t0) s_us_fields += (long)(openpete_phase_now_us() - t0);
          if (!bok) continue; }   /* shaded → handled separately */
        moby_fields_pose_from_snapshot(cpu, mobys[i], &f);
        /* .s far-skip parity: the host-rebuilt w0 has no low byte (the GPF-TR
         * scale), so moby_decode_project's mirror of .L8001F888 can't fire on
         * sub-ticks. Re-apply the canonical verdict from the LIVE record's w0 +
         * TR.z (frozen per tick — no sub-tick boundary flicker), else a GPF-far
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
                /* GPF sub-tick scale lerp: the byte is the cull's copy of
                 * m_ScaleOverride (moby+0x57, folded into w0 @8001F4A8/F4C4),
                 * animated per tick by effect classes (Gnasty-blast dome).
                 * Frozen per tick the on-screen scale steps at 30Hz under
                 * --render-fps; lerp prev→curr from the interp snapshots on
                 * the SAME clock as the pose crossfade (moby_prev_keyframe's
                 * t), so scale stays in phase with the poses. Snap when the
                 * prev byte is 0 or missing: byte==0 takes the .s's non-GPF
                 * (true-distance) branch — a semantic discontinuity, and the
                 * spine never lerps across a discontinuity. */
                if (s_rot_now) {
                    const openpete_interp_obj_t* og =
                        openpete_interp_objs_find(moby_interp_key(mobys[i]));
                    if (og && og->have_prev && og->prev_ns && og->curr_ns > og->prev_ns) {
                        int gprev = (int)og->prev[game_layout.moby.size_off];
                        int gnow  = (int)(rw0 & 0xFFu);
                        if (gprev > 0) {
                            double t = (double)(s_rot_now - og->curr_ns)
                                     / (double)(og->curr_ns - og->prev_ns);
                            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                            gpf_frac = (double)gprev + ((double)gnow - (double)gprev) * t;
                        }
                        /* OPENPETE_MOBY_GPF_DBG=1: one line per GPF re-extract —
                         * the ramp must be monotone across presents within a tick */
                        { static int s_gd = -1;
                          if (s_gd < 0) { const char* e = getenv("OPENPETE_MOBY_GPF_DBG"); s_gd = (e && *e && *e != '0'); }
                          if (s_gd) fprintf(stderr, "[gpf] vbl=%u m=%08x prev=%d now=%d frac=%.3f\n",
                                            openpete_frame, mobys[i], gprev, gnow, gpf_frac); }
                    }
                }
            }
        }
        /* Re-apply any render-time per-row scale the game baked into the
         * cull-record matrix that build_moby_record_fields' euler rebuild
         * cannot see — a cutscene disappear scales the record's M rows by a
         * shrink and a squash. The rebuild is a pure camera·euler rotation, so
         * |rebuild row| == |UNSCALED record row|; the ratio |record row|/|rebuild row|
         * is exactly that render-time scale (camera-independent — both are rotations).
         * Without it, cutscene mobys that squash/shrink render full-size on every
         * sub-tick present (--render-fps), i.e. they "don't animate out". The deadband
         * leaves normal mobys (ratio≈1, only Q12 truncation noise) untouched. */
        {
            int rM[3][3]; moby_rec_matrix(cpu, recs[i] + game_layout.moby.rec_matrix_off, rM);
            for (int row = 0; row < 3; row++) {
                double mr = sqrt((double)rM[row][0]*rM[row][0] + (double)rM[row][1]*rM[row][1] + (double)rM[row][2]*rM[row][2]);
                double mb = sqrt((double)f.M[row][0]*f.M[row][0] + (double)f.M[row][1]*f.M[row][1] + (double)f.M[row][2]*f.M[row][2]);
                if (mb > 1.0) {
                    double sc = mr / mb;
                    if (sc < 0.99 || sc > 1.01)       /* a real render-time scale, not truncation noise */
                        for (int c = 0; c < 3; c++) f.M[row][c] = (int)lround(f.M[row][c] * sc);
                }
            }
        }
        }
        if (s_nrecs >= MAX_RECORDS || s_proj_n >= MAX_PROJ) break;
        int off = s_proj_n;
        /* Two-pass shape lerp: f is the curr REAL pose (no synthesized blend — the
         * spine no longer writes m_FrameProgress); add the prev REAL pose and lerp
         * the two in model space. Crossfades within a clip AND across a clip switch
         * when the two animations share vertex topology; snaps to curr (no prev
         * pose, prev zeroed for the trace and the record) on spawn or a
         * topology-changing clip switch. */
        moby_pose_t prev; double balpha = 0.0;
        int prevkf = moby_prev_keyframe(cpu, mobys[i], f.header, &prev, &balpha);
        uint32_t model_held =
            moby_fields_hold_prev(cpu, mobys[i], &f, &prev, prevkf, balpha);
        /* The held pose decodes alone: it is the whole shape, not one end of a
         * lerp. prevkf keeps the verdict for the trace. */
        int lerp_prev = (prevkf == MOBY_PREV_LERP);
        if (!lerp_prev) memset(&prev, 0, sizeof prev);
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
                        openpete_frame, mobys[i], tcls2,
                        prevkf, balpha,
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 2u), cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 3u),
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 4u), cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 0u),
                        cpu->read_byte(mobys[i] + game_layout.moby.anim_state_off + 1u),
                        f.frameA, f.frameB, prev.frame_a, prev.frame_b, f.w0 & 0xFFFFu, (prev.blend << 8) & 0xFFFFu,
                        f.header, prev.header,
                        mrot & 0xFFFFFFu);
            }
        }
        /* LOD tier from the last canonical pass (sub-tick reuse — the tier is a
         * per-tick verdict, held across intermediates). Miss default 1024 = the
         * near/HP tier, today's behaviour for a moby with no canonical record.
         * Read BEFORE the decode: the tier-adjusted vertex-count read
         * ([hdr+8+(ZSF4>>31)], 0 = skip) hides far no-LP-mesh mobys (gems). */
        int zsf4 = fade_snap_get(mobys[i], 1024);
        int got;
        { uint64_t t0 = tier_dbg_on() ? openpete_phase_now_us() : 0;
          got = moby_decode_project(cpu, f.header, f.header_b, f.frameA, f.frameB, f.w0,
                                    lerp_prev ? &prev : NULL, balpha, gpf_frac,
                                    f.TR_raw, f.TR_frac, f.M, zsf4, moby_record_view(mobys[i]),
                                    s_proj + s_proj_n, MAX_PROJ - s_proj_n);
          if (t0) s_us_dec += (long)(openpete_phase_now_us() - t0); }
        if (got <= 0) continue;
        /* OPENPETE_MOBY_POSTRACE_CLS: final SUBMITTED screen-space centroid —
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
                        openpete_frame, mobys[i],
                        cpu->read_word(mobys[i] + game_layout.moby.angle_off + 0u),
                        f.frameA, f.frameB, (unsigned)((f.w0 >> 8) & 0xFFu),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 0u),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 4u),
                        (int)cpu->read_word(mobys[i] + game_layout.moby.pos_off + 8u),
                        dmob[0], dmob[1], dmob[2]);
                fprintf(stderr, "[moby-screen vbl=%u] leg=%s now=%.3f m=%08x cls=%03x n=%d c=(%.2f,%.2f) vz=%.1f fpos=%d/%d cam=(%.1f,%.1f,%.1f) mob=(%.1f,%.1f,%.1f) TRf=(%.2f,%.2f,%.2f)\n",
                        openpete_frame, g_openpete_canon_lag_leg ? "CANON" : "inter",
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
        s_proj_n += got; s_nrecs++;
    }
    if (!is_spec) {                            /* REGULAR snapshots its moby set for the SPEC dedup */
        s_reg_moby_n = 0;
        for (int r = 0; r < s_nrecs && s_reg_moby_n < MAX_RECORDS; r++)
            s_reg_moby[s_reg_moby_n++] = s_recs[r].moby;
        /* Culled REG mobys count as "handled" too — without this, a moby in
         * BOTH queues would slip past the dedup and draw via SPEC anyway. */
        for (int c = 0; c < culled_n && s_reg_moby_n < MAX_RECORDS; c++)
            s_reg_moby[s_reg_moby_n++] = culled_moby[c];
        /* Hold-replayed REG mobys likewise (they render, just from the hold). */
        for (int c = 0; c < nreplay_moby && s_reg_moby_n < MAX_RECORDS; c++)
            s_reg_moby[s_reg_moby_n++] = replay_moby[c];
        /* Widescreen reveal on SUB-TICKS too: the render set above is the .s
         * cull-record queue, which never contains the off-4:3 mobys the 4:3
         * cone cull skipped — the canonical feed host-builds those via
         * append_revealed_regular, so without this they existed ONLY on
         * canonical presents and flickered at high FPS in wide aspects.
         * Same builder, re-projected through the
         * interpolated g_Camera; dedups vs s_recs (the queue set — an
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
            if (openpete_ui_moby_is_addr_static(mob) || openpete_ui_moby_is_addr_ui(mob)) continue;
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
         * at high FPS). The material SLOT is no longer among them — it is
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
            ntri_t* o = &s_wtri[s_gout.n];
            *o = s_hold_tri[h->off + t];
            for (int vv = 0; vv < 3; vv++) { o->v[vv].sx += dx; o->v[vv].sy += dy; o->v[vv].vz *= vzs; }
            s_gout.n++;
        }
    }
    push_scene_moby();
    s_walk_in_rex = 0;
    return nm;
}

static int moby_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_LOAD_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}

void moby_reextract(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    /* Only replay mobys the canonical frame actually drew. During a level
     * transition the moby render path may not fire (or only for the collected-gem
     * mobys); gating on the canon-drawn mask reproduces exactly that set instead
     * of replaying the previous level's mobys from stale cull records. */
    if (!render_path_canon_drawn(RP_MOBY)) {
        if (moby_census_on())
            fprintf(stderr, "[census vbl=%u MOBY-RE] SKIP (not canon-drawn)\n", openpete_frame);
        return;
    }

    /* Per-producer freshness (canonical-render epoch): RP_MOBY is shared by
     * THREE canonical producers — REGULAR (func_8001F798), SPECULAR
     * (func_80020F34) and SHADED/HUD/text (func_80022A2C) — and a pause menu or
     * portal-load screen runs ONLY the SHADED/text one. Each re-extract pass
     * replays its capture only if ITS producer ran during the last canonical
     * present; otherwise the pass is skipped (the canonical present didn't draw
     * that content, so neither may the sub-ticks). Without this, the load
     * screen's text kept RP_MOBY canon-drawn and the stale REG/SPEC queues from
     * the last gameplay tick painted the previous level's mobys over the frozen
     * backdrop — the "moby soup" during level transitions (--render-fps only). */
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
    spyro_native_prod_camera_leave();

    /* Shaded mobys (gems/props/HUD icons/text) — already re-derive from g_Camera. */
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
    static int animswdbg = -1;
    if (animswdbg < 0) { const char* e = getenv("OPENPETE_MOBY_ANIMSW_DBG"); animswdbg = (e && *e && *e != '0'); }
    if (animswdbg) {
        static long last_cross = -1, last_snap = -1;
        if (s_animsw_cross != last_cross || s_animsw_snap != last_snap) {
            last_cross = s_animsw_cross; last_snap = s_animsw_snap;
            fprintf(stderr, "[moby-animsw vbl=%u] clip-switch crossfaded=%ld snapped(count-mismatch)=%ld\n",
                    openpete_frame, s_animsw_cross, s_animsw_snap);
        }
    }
}

/* One entry per draw pass the descriptor names, filled at apply_options. The
 * registry stops at the first zero address, so the table is COMPACTED: a
 * pass the layout leaves at 0 contributes no entry instead of truncating the
 * ones after it. A game that names no pass registers nothing and never enters
 * this feeder. */
static feature_dispatch_entry_t moby_dispatch[6];

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
