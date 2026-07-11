// core/mods/mod_draw.c — the mod-facing screen-space draw service.
//
// P2 workhorse of notes/plans/openpete-modding-framework.md (native
// draw/overlay service, vk-renderer-first — USER-LOCKED direction): mods
// draw HUD text/rects through the native renderer, never guest-side, so
// an observer/HUD mod stays sim-neutral by construction (zero guest-RAM
// writes; guest prim injection provably leaks — the 16:9
// particle_drawn_flag regression).
//
// Two submission contexts (plan Round 7):
//   - sim-tick hooks (inside overrides): commands land in a staging
//     buffer; the canonical present commits it for display across ALL of
//     that tick's presents (immediate mode per tick — draw every tick or
//     it disappears).
//   - present hooks (OPENPETE mod api register_present_hook): run once
//     per present on the host fiber with {tick, alpha, dt, aspect};
//     commands live for that present only. Mods evaluate their own
//     curves at alpha — the engine never interpolates mod visuals.
//
// Render-purity is enforced by API shape: while a present hook runs,
// openpete_mods_in_present is set and the loader's guest/call/override
// entry points refuse with an error (presents are non-deterministic in
// count/timing; sim-affecting work belongs in tick hooks).
//
// Text is rasterized by a GAME-registered emitter (Spyro: the 3D gold
// text-moby glyphs via text_moby_native_emit — digits, A-Z, limited
// punctuation). Coordinates: overlay space = window-height fractions,
// origin top-left, +y down, x spans [0, aspect].

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_interp.h"
#include "openpete_time.h"
#include "openpete_fiber.h" /* openpete_frame */

#include <stdio.h>
#include <string.h>

#define DRAW_TEXTS_MAX 64
#define DRAW_RECTS_MAX 128
#define DRAW_STR_MAX   96
#define PRESENT_HOOKS_MAX 16

typedef struct {
    float    x, y, cap_h;
    uint32_t rgb;
    char     str[DRAW_STR_MAX];
} cmd_text_t;

typedef struct {
    float    x, y, w, h;
    uint32_t rgba;
} cmd_rect_t;

typedef struct {
    cmd_text_t texts[DRAW_TEXTS_MAX];
    int        n_texts;
    cmd_rect_t rects[DRAW_RECTS_MAX];
    int        n_rects;
} cmd_buf_t;

static cmd_buf_t s_tick_staging;   /* written by tick-context draws     */
static cmd_buf_t s_tick_committed; /* displayed for the current tick    */
static cmd_buf_t s_present;        /* rebuilt inside each present hook  */

int openpete_mods_in_present = 0;

static openpete_mods_text_emit_fn s_text_fn = NULL;
void openpete_mods_draw_set_text_fn(openpete_mods_text_emit_fn fn) {
    s_text_fn = fn;
}

typedef struct {
    openpete_mod_present_fn fn;
    const char*             owner_id;
} present_hook_t;

static present_hook_t s_phooks[PRESENT_HOOKS_MAX];
static int            s_phook_count = 0;
static int            s_any_draw = 0; /* fast bail for modless runs */

int openpete_mods_register_present_hook(openpete_mod_present_fn fn,
                                        const char* owner_id) {
    if (!fn) return -1;
    if (s_phook_count >= PRESENT_HOOKS_MAX) {
        OPL_ERROR_F("mods", "present-hook table full (%d) — cannot add %s",
                    PRESENT_HOOKS_MAX, owner_id ? owner_id : "?");
        return -1;
    }
    s_phooks[s_phook_count].fn = fn;
    s_phooks[s_phook_count].owner_id = owner_id;
    s_phook_count++;
    s_any_draw = 1;
    return 0;
}

/* ---- command submission (mod-facing, via the api struct) ------------ */

static cmd_buf_t* target_buf(void) {
    return openpete_mods_in_present ? &s_present : &s_tick_staging;
}

void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str) {
    if (!str || !str[0]) return;
    cmd_buf_t* b = target_buf();
    if (b->n_texts >= DRAW_TEXTS_MAX) return;
    cmd_text_t* t = &b->texts[b->n_texts++];
    t->x = x; t->y = y; t->cap_h = cap_h; t->rgb = rgb;
    snprintf(t->str, sizeof t->str, "%s", str);
    s_any_draw = 1;
}

void openpete_mods_draw_rect(float x, float y, float w, float h,
                             uint32_t rgba) {
    cmd_buf_t* b = target_buf();
    if (b->n_rects >= DRAW_RECTS_MAX) return;
    cmd_rect_t* r = &b->rects[b->n_rects++];
    r->x = x; r->y = y; r->w = w; r->h = h; r->rgba = rgba;
    s_any_draw = 1;
}

/* ---- per-present flush (called from run.c's two present legs) -------- */

#define MOD_DRAW_VERTS_MAX 16384
static openpete_hud_vert s_verts[MOD_DRAW_VERTS_MAX];

/* Rects sit behind glyphs in the overlay's isolated depth pass (glyph
 * depth band is mid-range; see text_moby_native TXT_DEPTH_MID). */
#define RECT_DEPTH 0.95f

static int emit_rect(const cmd_rect_t* r, int n) {
    if (n + 6 > MOD_DRAW_VERTS_MAX) return n;
    const float x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
    const openpete_hud_vert q[6] = {
        { x0, y0, RECT_DEPTH, r->rgba }, { x1, y0, RECT_DEPTH, r->rgba },
        { x1, y1, RECT_DEPTH, r->rgba }, { x0, y0, RECT_DEPTH, r->rgba },
        { x1, y1, RECT_DEPTH, r->rgba }, { x0, y1, RECT_DEPTH, r->rgba },
    };
    memcpy(&s_verts[n], q, sizeof q);
    return n + 6;
}

static int emit_buf(const cmd_buf_t* b, int n) {
    for (int i = 0; i < b->n_rects; i++)
        n = emit_rect(&b->rects[i], n);
    if (s_text_fn) {
        for (int i = 0; i < b->n_texts; i++) {
            const cmd_text_t* t = &b->texts[i];
            n += s_text_fn(t->str, t->x, t->y, t->cap_h, t->rgb,
                           &s_verts[n], MOD_DRAW_VERTS_MAX - n);
        }
    }
    return n;
}

void openpete_mods_present_flush(int canonical) {
    if (!s_any_draw) return;
    if (!openpete_native_render_enabled()) return;

    /* Commit the tick buffer once per canonical present: the sim tick
     * that just ran wrote staging; it becomes the displayed set for this
     * tick's presents (and clears — immediate mode per tick). */
    if (canonical) {
        s_tick_committed = s_tick_staging;
        memset(&s_tick_staging, 0, sizeof s_tick_staging);
    }

    /* Present hooks: render-pure by API shape (see header comment). */
    memset(&s_present, 0, sizeof s_present);
    if (s_phook_count > 0) {
        static op_time_t s_prev_present_ns = 0;
        const op_time_t now = openpete_interp_present_now();
        openpete_present_ctx_t ctx;
        ctx.tick   = openpete_frame;
        ctx.alpha  = 0.0f;
        ctx.dt     = (s_prev_present_ns && now > s_prev_present_ns)
                       ? (float)((double)(now - s_prev_present_ns) / 1e9)
                       : 0.0f;
        ctx.aspect = openpete_native_overlay_aspect();
        s_prev_present_ns = now;

        /* Sub-tick phase between the last two sim snapshots — the same
         * query the high-fps spine uses. 0 on canonical presents / when
         * interpolation isn't running. */
        double a = 0.0;
        const openpete_interp_snapshot_t *prev, *curr;
        const op_time_t sim_period =
            (op_time_t)((OP_NS_PER_SEC * 1001 * 2) / 60000); /* 29.97 Hz */
        if (!canonical &&
            openpete_interp_query(now, sim_period, &a, &prev, &curr))
            ctx.alpha = (float)a;

        openpete_mods_in_present = 1;
        for (int i = 0; i < s_phook_count; i++)
            s_phooks[i].fn(&ctx);
        openpete_mods_in_present = 0;
    }

    int n = 0;
    n = emit_buf(&s_tick_committed, n);
    n = emit_buf(&s_present, n);
    openpete_native_set_mod_overlay(s_verts, n);

    /* One-shot diagnostics: confirm the pipeline end-to-end on first use. */
    static int s_logged_first = 0;
    if (!s_logged_first && n > 0) {
        s_logged_first = 1;
        OPL_INFO_F("mods", "draw service live: %d overlay verts "
                   "(%d tick rects, %d tick texts, %d present texts)",
                   n, s_tick_committed.n_rects, s_tick_committed.n_texts,
                   s_present.n_texts);
    }
}
