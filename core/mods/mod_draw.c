// core/mods/mod_draw.c — the mod-facing screen-space draw service.
//
// P2 workhorse of notes/plans/openpete-modding-framework.md (native
// draw/overlay service, vk-renderer-first — USER-LOCKED direction): mods
// draw HUD text through the native renderer, never guest-side, so
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
#include "openpete_interp_objs.h"   /* g_openpete_tick_period_ns */
#include "openpete_time.h"
#include "openpete_fiber.h" /* openpete_frame */
#include "../ui/openpete_imgui_guard.h" /* hook-owner attribution */
#include "mod_vec.h"

#include <SDL3/SDL.h>   /* SDL_GetCurrentThreadID — the single-owner check */

#include <stdio.h>
#include <string.h>

/* Texts a command buffer allocates up front; it grows past this. */
#define DRAW_TEXTS_INIT 64
#define DRAW_STR_MAX    96
#define PRESENT_HOOKS_MAX 16

typedef struct {
    float       x, y, cap_h;
    uint32_t    rgb;
    const char* owner;   /* mod id of the hook that submitted this text */
    char        str[DRAW_STR_MAX];
} cmd_text_t;

typedef struct {
    mod_fvec texts;      /* cmd_text_t */
    int      inited;
} cmd_buf_t;

/* The tick double buffer. Staging takes this tick's draws; committed is
 * what the presents of this tick display. The canonical commit exchanges
 * the two by pointer, so neither buffer's heap block is ever aliased or
 * dropped, and clears the new staging. */
static cmd_buf_t  s_tick_bufs[2];
static cmd_buf_t* s_tick_staging   = &s_tick_bufs[0];
static cmd_buf_t* s_tick_committed = &s_tick_bufs[1];
static cmd_buf_t  s_present;       /* rebuilt inside each present hook  */

static void buf_init(cmd_buf_t* b, const char* name) {
    if (b->inited) return;
    b->inited = 1;
    mod_fvec_init(&b->texts, sizeof(cmd_text_t), DRAW_TEXTS_INIT, name,
                  DRAW_TEXTS_INIT);
}

static int buf_count(const cmd_buf_t* b) {
    return (int)mod_fvec_count(&b->texts);
}

/* Drop the commands, keep the storage: the buffers refill every tick, so
 * the allocation is paid once for the session. */
static void buf_clear(cmd_buf_t* b) { mod_fvec_clear(&b->texts); }

/* The draw service has one owner thread. The tick buffers are exchanged
 * by pointer and a mod pushes into staging from a hook, so a submission
 * on a second thread could be in flight across the swap. Registration,
 * removal, submission and the flush all run on the worker thread that
 * owns the game fiber; this latches the first thread to reach the swap
 * and reports any later one, which is the only way the argument can
 * break without a visible symptom. */
static void assert_one_owner(void) {
    static SDL_ThreadID s_owner_tid = 0;
    static int s_reported = 0;
    SDL_ThreadID tid = SDL_GetCurrentThreadID();
    if (!s_owner_tid) { s_owner_tid = tid; return; }
    if (tid != s_owner_tid && !s_reported) {
        s_reported = 1;
        OPL_ERROR_F("mods", "draw service: tick buffers exchanged from a "
                    "second thread (owner %llu, now %llu) — a mod's text "
                    "can be lost or double-freed",
                    (unsigned long long)s_owner_tid,
                    (unsigned long long)tid);
    }
}

/* Per thread: present hooks run on the thread that extracts while a mod UI
 * section can run inside a compose on a present thread; one shared flag
 * would let either clear the other's purity guard. */
OPENPETE_THREAD_LOCAL int openpete_mods_in_present = 0;

static openpete_mods_text_emit_fn s_text_fn = NULL;
void openpete_mods_draw_set_text_fn(openpete_mods_text_emit_fn fn) {
    s_text_fn = fn;
}

typedef struct {
    openpete_mod_present_fn fn;
    const char*             owner_id;
    int                     disabled; /* live toggle */
} present_hook_t;

static present_hook_t s_phooks[PRESENT_HOOKS_MAX];
static int            s_phook_count = 0;
static int            s_any_draw = 0; /* fast bail for modless runs */

/* Last present's ctx (tick/alpha/canonical) — the postfx engine UBO
 * reads these through openpete_mods_present_ctx (same source as the
 * present-hook ctx, so mods and shaders can never disagree on phase). */
static uint32_t s_pctx_tick;
static float    s_pctx_alpha;
static int      s_pctx_canonical = 1;

void openpete_mods_present_ctx(uint32_t* tick, float* alpha, int* canonical) {
    if (tick) *tick = s_pctx_tick;
    if (alpha) *alpha = s_pctx_alpha;
    if (canonical) *canonical = s_pctx_canonical;
}

void openpete_mods_draw_remove_owner(const char* owner_id) {
    if (!owner_id) return;
    int w = 0;
    for (int i = 0; i < s_phook_count; i++) {
        if (s_phooks[i].owner_id &&
            strcmp(s_phooks[i].owner_id, owner_id) == 0)
            continue;
        s_phooks[w++] = s_phooks[i];
    }
    s_phook_count = w;
}

void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on) {
    if (!owner_id) return;
    for (int i = 0; i < s_phook_count; i++)
        if (s_phooks[i].owner_id &&
            strcmp(s_phooks[i].owner_id, owner_id) == 0)
            s_phooks[i].disabled = !on;
}

/* A hook registered by a mod the loader already publishes comes up in
 * that mod's current live-toggle state. Hot reload sweeps a mod's
 * registrations and re-runs its entry without consulting the toggle
 * state, so a re-registration by a currently-disabled mod would
 * otherwise silently resume presenting.
 *
 * An owner the loader does not list yet is a mod inside its own entry:
 * boot load (the row is published only after entry returns) and a
 * deferred-entry mod's first enable (the row still reads disabled until
 * the apply completes). Both register enabled — the apply's
 * set_owner_enabled(id, 1) immediately follows the deferred entry, and
 * boot entry means the mod is in the enabled set by construction. */
static int owner_starts_disabled(const char* owner_id) {
    if (!owner_id) return 0;
    const int n = openpete_mods_count();
    for (int i = 0; i < n; i++) {
        const char* id = openpete_mods_id(i);
        if (id && strcmp(id, owner_id) == 0)
            return !openpete_mods_is_enabled(owner_id);
    }
    return 0;
}

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
    s_phooks[s_phook_count].disabled = owner_starts_disabled(owner_id);
    s_phook_count++;
    s_any_draw = 1;
    return 0;
}

/* ---- command submission (mod-facing, via the api struct) ------------ */

static cmd_buf_t* target_buf(void) {
    return openpete_mods_in_present ? &s_present : s_tick_staging;
}

void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str) {
    if (!str || !str[0]) return;
    cmd_buf_t* b = target_buf();
    cmd_text_t* t;
    buf_init(b, b == &s_present ? "draw_text_present"
                                : (b == &s_tick_bufs[0] ? "draw_text_tick_a"
                                                        : "draw_text_tick_b"));
    t = (cmd_text_t*)mod_fvec_push(&b->texts);
    if (!t) {
        OPL_ERROR_F("mods", "draw service: text list allocation failed — "
                    "\"%.32s\" dropped", str);
        return;
    }
    t->x = x; t->y = y; t->cap_h = cap_h; t->rgb = rgb;
    /* The hook dispatch publishes the running mod for attribution; a
     * draw from anywhere else has no owner to name. */
    t->owner = openpete_imgui_guard_hook_owner();
    snprintf(t->str, sizeof t->str, "%s", str);
    s_any_draw = 1;
}

/* ---- per-present flush (called from run.c's two present legs) -------- */

#define MOD_DRAW_VERTS_MAX 16384
static openpete_hud_vert s_verts[MOD_DRAW_VERTS_MAX];

/* One warning per owner per session: the vertex stream is a real bound
 * (the renderer's buffer is sized to it), so the remedy is the mod
 * drawing less, and repeating the line every present would bury it. */
static void warn_glyph_overflow(const char* owner) {
    static const char* s_warned[32];
    static int s_warned_n = 0;
    const char* id = owner ? owner : "?";
    for (int i = 0; i < s_warned_n; i++)
        if (s_warned[i] == owner) return;
    if (s_warned_n < (int)(sizeof s_warned / sizeof s_warned[0]))
        s_warned[s_warned_n++] = owner;
    OPL_WARN_F("mods", "draw service: %s filled the overlay vertex budget "
               "(%d verts) — the rest of its text this present is dropped",
               id, MOD_DRAW_VERTS_MAX);
}

static int emit_buf(const cmd_buf_t* b, int n) {
    /* A text that laid out no glyphs at all is the drop this reports. The
     * emitter refuses a glyph only when the remaining budget cannot hold
     * it, so once the budget is full every later text writes nothing —
     * but it writes nothing for a missing font too, which is why the
     * report waits until some text in this flush has laid out glyphs.
     * The one text straddling the boundary keeps the glyphs that fit and
     * is not reported: it drew. */
    int emitted_any = 0;
    if (!s_text_fn) return n;
    for (int i = 0; i < buf_count(b); i++) {
        const cmd_text_t* t = (const cmd_text_t*)mod_fvec_at(&b->texts,
                                                             (size_t)i);
        int avail = MOD_DRAW_VERTS_MAX - n;
        int wrote = avail > 0
                  ? s_text_fn(t->str, t->x, t->y, t->cap_h, t->rgb,
                              &s_verts[n], avail)
                  : 0;
        n += wrote;
        if (wrote > 0) { emitted_any = 1; continue; }
        if (emitted_any) warn_glyph_overflow(t->owner);
    }
    return n;
}

void openpete_mods_present_flush(CPUState* cpu, int canonical) {
    /* Per-mod time report window: every 512 sim ticks (~17 s at 29.913),
     * counted at the canonical commit. ABOVE the draw gates — tick-hook
     * timing must report in headless runs too.
     *
     * Gate on mods LOADED, not openpete_mods_active (= a chain is
     * registered): the tick boundary drains the live-toggle queue, and
     * under deferred-entry load-all the very first enable can happen
     * while no mod owns a chain yet — an active-gated boundary would
     * never drain the queue, bricking every toggle. Zero mods loaded
     * still skips everything (pristine modless run). */
    if (canonical && openpete_mods_count() > 0) {
        static unsigned s_canonical_ticks = 0;
        static unsigned s_report_ticks = 0;
        openpete_mods_tick_boundary(cpu, ++s_canonical_ticks);
        if (++s_report_ticks >= 512) {
            openpete_mods_time_report(s_report_ticks);
            s_report_ticks = 0;
        }
    }

    if (!s_any_draw) return;

    /* Commit the tick buffer once per canonical present: the sim tick
     * that just ran wrote staging; it becomes the displayed set for this
     * tick's presents (and clears — immediate mode per tick).
     *
     * ABOVE the renderer gate, so staging drains even when nothing
     * consumes it (--renderer og, or any run without a native renderer).
     * An undrained staging buffer pins every command ever submitted: the
     * text cap fills for the rest of the session (further draw
     * calls fail) and the whole backlog would replay at once if the
     * renderer came up.
     *
     * Live toggle: the apply above runs BEFORE this commit, so a mod
     * disabled at this boundary still displays the tick it drew while
     * enabled — then goes quiet. Its hooks no longer run, staging stays
     * empty, and the next canonical commit clears the display set;
     * nothing of a disabled mod survives past that one tick. */
    if (canonical) {
        cmd_buf_t* prev = s_tick_committed;
        assert_one_owner();
        s_tick_committed = s_tick_staging;
        s_tick_staging = prev;
        buf_clear(s_tick_staging);
    }

    if (!openpete_native_render_enabled()) return;

    /* Present hooks: render-pure by API shape (see header comment). The
     * ctx is computed unconditionally — the postfx engine UBO reads the
     * same values through openpete_mods_present_ctx (S4), hooks or not. */
    buf_clear(&s_present);
    {
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
        /* The canonical tick period, matching what run.c paces to — never a
         * local literal, or a mod's alpha drifts against the engine's. */
        const op_time_t sim_period = g_openpete_tick_period_ns;
        if (!canonical &&
            openpete_interp_query(now, sim_period, &a, &prev, &curr))
            ctx.alpha = (float)a;

        s_pctx_tick = ctx.tick;
        s_pctx_alpha = ctx.alpha;
        s_pctx_canonical = canonical ? 1 : 0;

        if (s_phook_count > 0) {
        openpete_mods_in_present = 1;
        for (int i = 0; i < s_phook_count; i++) {
            if (s_phooks[i].disabled) continue;
            const op_time_t t0 = op_now(); /* clock-guard: wall */ /* hook cost span */
            /* Names the mod in an ImGui refusal: a present hook runs
             * outside any UI section body, so the guard has no mod index. */
            openpete_imgui_guard_set_hook_owner(s_phooks[i].owner_id);
            s_phooks[i].fn(&ctx);
            openpete_imgui_guard_set_hook_owner(NULL);
            openpete_mods_time_add(s_phooks[i].owner_id, op_now() - t0, 1); /* clock-guard: wall */
        }
        openpete_mods_in_present = 0;
        }
    }

    int n = 0;
    n = emit_buf(s_tick_committed, n);
    n = emit_buf(&s_present, n);
    openpete_native_set_mod_overlay(s_verts, n);

    /* One-shot diagnostics: confirm the pipeline end-to-end on first use. */
    static int s_logged_first = 0;
    if (!s_logged_first && n > 0) {
        s_logged_first = 1;
        OPL_INFO_F("mods", "draw service live: %d overlay verts "
                   "(%d tick texts, %d present texts)",
                   n, buf_count(s_tick_committed), buf_count(&s_present));
    }
}
