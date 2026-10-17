/* render_paths.c - per-path render isolation registry. See render_paths.h. */

#include "renderer/render_paths.h"
#include "renderer/ui_game.h"      /* retained freeze backdrop commit */
#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* CPUState, read_word - for the path brackets */
#include "openpete.h"
#include "openpete_knobs.h"          /* dev-overlay row bind */
#include "openpete_native_render.h"  /* isolation toggle */
#include "renderer/orchestrator.h"  /* canonical-render epoch commit */
#include "renderer/terrain.h"       /* deferred terrain feed fallback */
#include "renderer/moby.h"          /* held overlay-pass feed */
#include "psyq/psyq_game_hooks.h"   /* the OT walk's view of this filter */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_fiber.h"   /* openpete_frame: interval and mark VBLs, the ivdump probe */
#include "renderer/env_knob.h"
#include "game_layout.h"             /* level_id: the level push */
#include "openpete_mods.h"           /* openpete_mods_set_level */

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "shadow", "flame", "particles", "tracer",
    "transition", "portal", "glow", "sparkle", "moby_shadow", "ui",
};

static uint32_t s_mask   = 0;      /* bit per selected path */
static bool     s_active = false;  /* a selection is in effect */

void render_paths_init(const char* spec) {
    s_mask = 0;
    s_active = false;
    if (!spec || !*spec) return;   /* unset = all paths, no filtering */
    s_active = true;

    const char* p = spec;
    char tok[32];
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ',' && *p != ' ' && n < (int)sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        int matched = -1;
        for (int i = 0; i < RP_COUNT; i++)
            if (strcmp(tok, k_names[i]) == 0) { matched = i; break; }
        if (matched >= 0) s_mask |= (1u << matched);
        else if (strcmp(tok, "none") == 0) { /* selects no path */ }
        else fprintf(stderr, "[render-paths] unknown path '%s' (known: "
                             "cyclorama,spyro,moby,terrain,shadow,flame,particles,"
                             "tracer,transition,portal,glow,sparkle,moby_shadow,ui)\n", tok);
    }

    fprintf(stderr, "[render-paths] isolating:");
    for (int i = 0; i < RP_COUNT; i++)
        if (s_mask & (1u << i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, "%s\n", s_mask ? "" : " (none matched - nothing will render)");
}

bool render_paths_active(void) { return s_active; }

void render_paths_set(const char* spec) {
    render_paths_init(spec);
    /* In isolation an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer - same sync as boot. */
    openpete_native_set_isolation(render_paths_active());
}

void render_paths_current(char* buf, size_t cap) {
    if (!s_active) { snprintf(buf, cap, "(all)"); return; }
    if (!s_mask)   { snprintf(buf, cap, "none"); return; }
    size_t off = 0;
    if (cap) buf[0] = '\0';
    for (int i = 0; i < RP_COUNT; i++) {
        if (!(s_mask & (1u << i))) continue;
        off += (size_t)snprintf(buf + off, cap > off ? cap - off : 0,
                                "%s%s", off ? "," : "", k_names[i]);
    }
}

bool render_path_enabled(render_path_t p) {
    if (!s_active) return true;                 /* no selection = all on */
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_mask & (1u << (unsigned)p)) != 0;
}

/* Per-frame intervals. Sized generously: mobys can emit many separate dispatch
 * intervals per frame.
 *
 * Two banks: the game submits DrawOTag at the START of a VBL, before that
 * VBL's render functions record any intervals - so at walk time the current
 * table describes the OT being BUILT, not the one being DRAWN. The commit
 * (once per canonical present) rolls current -> previous instead of
 * discarding, and the OT-walk filter consults current first, then previous.
 * The polybuf double-buffers per frame, so the two tables cover disjoint
 * address banks and cannot false-match each other. */
#define MAX_INTERVALS 1024
typedef struct { uint32_t start, end; render_path_t path; unsigned vbl; } interval_t;
static interval_t s_iv[MAX_INTERVALS];
static int        s_iv_n = 0;
static interval_t s_iv_prev[MAX_INTERVALS];
static int        s_iv_prev_n = 0;

/* Interval-less canon-drawn marks (render_path_mark_drawn). Kept separate from
 * the intervals so they can never leak into the OT-walk addr filter. */
static uint32_t s_pending_marks = 0;
static unsigned s_mark_vbl[RP_COUNT];   /* VBL of each path's pending mark */

void render_paths_frame_reset(void) {
    if (s_iv_n) {   /* roll into the drawn-OT bank; keep it across idle VBLs */
        memcpy(s_iv_prev, s_iv, (size_t)s_iv_n * sizeof(s_iv[0]));
        s_iv_prev_n = s_iv_n;
    }
    s_iv_n = 0; s_pending_marks = 0;
}

void render_path_mark_drawn(render_path_t p) {
    if (p < 0 || p >= RP_COUNT) return;
    s_pending_marks |= (1u << (unsigned)p);
    s_mark_vbl[p] = openpete_frame;
}

void render_path_record(render_path_t p, uint32_t start, uint32_t end) {
    if (end <= start || s_iv_n >= MAX_INTERVALS) return;
    /* OPENPETE_RENDER_PATH_IVDUMP=frame: print every recorded polybuf interval
     * on that openpete_frame with its path name (OT-prim producer attribution). */
    {
        static int s_ivdump = -2;
        ENV_INT_LATCH(s_ivdump, "OPENPETE_RENDER_PATH_IVDUMP", -1);
        if (s_ivdump >= 0 && (int)openpete_frame == s_ivdump)
            fprintf(stderr, "[render-paths] ivdump f=%u path=%s [0x%08X,0x%08X)\n",
                    openpete_frame, k_names[p], start, end);
    }
    /* A range that continues the previous one of the same path extends it:
     * the 2D layer records one range per primitive, and the polybuf allocates
     * them back to back. */
    if (s_iv_n > 0 && s_iv[s_iv_n - 1].path == p && s_iv[s_iv_n - 1].end == start &&
        s_iv[s_iv_n - 1].vbl == openpete_frame) {
        s_iv[s_iv_n - 1].end = end;
        return;
    }
    s_iv[s_iv_n].start = start;
    s_iv[s_iv_n].end   = end;
    s_iv[s_iv_n].path  = p;
    s_iv[s_iv_n].vbl   = openpete_frame;
    s_iv_n++;
}

/* "Canon drawn" mask - latched from the per-frame intervals once per canonical
 * render (before frame_reset clears them). A path is "drawn" iff it recorded at
 * least one non-empty polybuf interval this frame; render_path_record already
 * drops empty ranges, so a path that ran its render fn but emitted nothing (fully
 * culled / suppressed during a transition) reads as NOT drawn - which is what the
 * spine wants (reproduce only what the canonical frame actually painted). */
static uint32_t s_canon_mask = 0;

void render_paths_canon_snapshot(void) {
    uint32_t m = s_pending_marks;
    for (int i = 0; i < s_iv_n; i++) m |= (1u << (unsigned)s_iv[i].path);
    s_canon_mask = m;
}

/** @brief Drops what a render superseded before any canonical present recorded, when the game opens its next scene.
 *
 * Registered with openpete_native_scene_register_superseded. The canonical
 * present that follows shows only the newest scene, so the polybuf
 * intervals and drawn marks recorded on VBLs before that scene's VBL leave
 * the pending set, and the canonical-render epoch moves past the captures
 * the superseded render stamped (spyro_native_canon_epoch_supersede). A
 * render never spans a VBL boundary, so a path the new scene's render
 * recorded before its first feed opened the scene stays.
 * @param frame The VBL stamp of the scene being opened. */
static void render_paths_superseded(unsigned frame) {
    int w = 0;
    for (int i = 0; i < s_iv_n; i++)
        if (s_iv[i].vbl >= frame) s_iv[w++] = s_iv[i];
    s_iv_n = w;
    for (int p = 0; p < RP_COUNT; p++)
        if (s_mark_vbl[p] < frame) s_pending_marks &= ~(1u << (unsigned)p);
    spyro_native_canon_epoch_supersede();
}

bool render_path_canon_drawn(render_path_t p) {
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_canon_mask & (1u << (unsigned)p)) != 0;
}

/* Per-canonical-present commit, registered with the core host present loop
 * (openpete_native_register_commit). Runs once per canonical render, after the
 * game fiber's render + DrawOTag, before the sub-tick re-extract: latch the
 * canon-drawn mask, then clear the intervals for the next frame. */
/* Feed audit: every native tri carries a provenance tag whose family (bits
 * 31:28) names the render path that emitted it. A path that fed geometry this
 * canonical frame but recorded no polybuf interval and no mark_drawn reads as
 * not canon-drawn, so the sub-tick spine drops it on every intermediate
 * present: geometry on the canonical present, gone between ticks. Detected
 * here, at the commit, instead of by watching the screen at a high present
 * rate. Untagged tris (family 0) and families without a path are not
 * auditable. Reported per path on first occurrence and in the exit census;
 * OPENPETE_RENDER_PATH_FEED_ASSERT=1 aborts on the first one. */
/* A family maps to the SET of paths that may have emitted it: the particle
 * family (5) is fed by three independent .s renderers, each with its own path,
 * so it reads as drawn when any of them drew. The first path in the set names
 * the family in reports. */
#define RPM(p) (1u << (unsigned)(p))
static const uint32_t k_tag_family_paths[16] = {
    0, RPM(RP_MOBY), RPM(RP_TERRAIN), RPM(RP_SPYRO), RPM(RP_FLAME),
    RPM(RP_PARTICLES) | RPM(RP_GLOW) | RPM(RP_SPARKLE), RPM(RP_CYCLORAMA),
    RPM(RP_SHADOW) | RPM(RP_MOBY_SHADOW), RPM(RP_TRACER), RPM(RP_PORTAL), 0, 0, 0, 0, 0, 0
};
/** @brief The lowest-numbered path in a family's path set.
 *
 * @param m The path set, one bit per render_path_t.
 * @return The path index, or -1 when the set is empty. */
static int family_first_path(uint32_t m) { for (int p = 0; p < RP_COUNT; p++) if (m & RPM(p)) return p; return -1; }
/** @brief Whether any path of a family's path set is canon-drawn.
 *
 * @param m The path set, one bit per render_path_t.
 * @return true when render_path_canon_drawn holds for a member. */
static bool family_canon_drawn(uint32_t m) { for (int p = 0; p < RP_COUNT; p++) if ((m & RPM(p)) && render_path_canon_drawn((render_path_t)p)) return true; return false; }
static uint32_t s_feed_audit_frames[RP_COUNT];   /* frames with unmarked feed, per path */
static uint32_t s_feed_audit_tris[RP_COUNT];     /* tris fed on those frames, per path */
static uint32_t s_feed_audit_fed_frames[RP_COUNT]; /* frames the path fed at all (engagement) */
static int      s_feed_audit_registered = 0;

/** @brief Prints the exit summary of the feed audit to stderr.
 *
 * One line: the path-frames that fed at all, the path-frames that fed with no
 * polybuf interval and no mark, and each path with a nonzero unmarked count
 * with its triangle total. Registered with atexit by the first audited scene. */
static void render_paths_feed_audit_census(void) {
    uint32_t fed = 0, bad = 0;
    for (int p = 0; p < RP_COUNT; p++) { fed += s_feed_audit_fed_frames[p]; bad += s_feed_audit_frames[p]; }
    fprintf(stderr, "render-paths: feed audit: fed_path_frames=%u unmarked_path_frames=%u",
            fed, bad);
    for (int p = 0; p < RP_COUNT; p++)
        if (s_feed_audit_frames[p])
            fprintf(stderr, " %s=%u(%u tris)", k_names[p], s_feed_audit_frames[p], s_feed_audit_tris[p]);
    fprintf(stderr, "\n");
}

/** @brief Audits the canonical scene's tagged triangle families against the canon-drawn mask.
 *
 * Runs once per built scene. For each tag family with triangles and a path
 * set, counts the path as fed, and when no path of the set is canon-drawn
 * counts an unmarked frame and its triangles, printing a DEFECT line on the
 * path's first occurrence. With OPENPETE_RENDER_PATH_FEED_ASSERT set it
 * prints every occurrence and aborts on the first. */
static void render_paths_feed_audit(void) {
    /* One audit per canonical scene. The commit runs on every canonical
     * present; when the game stops rendering (a level-exit freeze, a
     * transition) the presents continue and the scene keeps the last frame's
     * counts while the intervals were already cleared, so a scene is audited
     * only on the first commit after it was built. */
    static unsigned s_last_scene = 0xFFFFFFFFu;
    const unsigned sf = openpete_native_scene_frame();
    if (sf == 0xFFFFFFFFu || sf == s_last_scene) return;
    s_last_scene = sf;
    if (!s_feed_audit_registered) { s_feed_audit_registered = 1; atexit(render_paths_feed_audit_census); }
    static int s_assert = -1;
    ENV_FLAG_LATCH(s_assert, "OPENPETE_RENDER_PATH_FEED_ASSERT");
    uint32_t n[16];
    openpete_native_scene_tag_family_counts(n);
    for (int f = 1; f < 16; f++) {
        const int p = family_first_path(k_tag_family_paths[f]);
        if (p < 0 || n[f] == 0) continue;
        s_feed_audit_fed_frames[p]++;
        if (family_canon_drawn(k_tag_family_paths[f])) continue;
        if (s_feed_audit_frames[p]++ == 0 || s_assert)
            fprintf(stderr, "[render-paths] DEFECT f=%u path=%s fed %u native tris with no polybuf "
                    "interval and no render_path_mark_drawn; sub-tick presents drop it\n",
                    openpete_frame, k_names[p], n[f]);
        s_feed_audit_tris[p] += n[f];
        if (s_assert) abort();
    }
}

/** @brief Prints the canon-drawn paths and the native scene's per-family triangle counts under OPENPETE_RP_CENSUS.
 *
 * OPENPETE_RP_CENSUS=LO-HI, or a single value, selects an openpete_frame
 * range, read on the first call. The per-canonical-present commit calls this
 * after latching the canon-drawn mask, so each line separates a producer that
 * fed the canonical scene from one that did not; a family is named by the
 * first path of its set, or "untagged". */
static void render_paths_census(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_RP_CENSUS", openpete_frame)) return;
    uint32_t n[16];
    openpete_native_scene_tag_family_counts(n);
    fprintf(stderr, "[rp-census f=%u CANON] drawn:", openpete_frame);
    for (int i = 0; i < RP_COUNT; i++)
        if (s_canon_mask & (1u << (unsigned)i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, " | tris:");
    for (int f = 1; f < 16; f++) {
        if (!n[f]) continue;
        const int p = family_first_path(k_tag_family_paths[f]);
        fprintf(stderr, " %s=%u", p >= 0 ? k_names[p] : "untagged", n[f]);
    }
    fprintf(stderr, " scene=%u\n", openpete_native_scene_frame());
}

/** @brief The per-canonical-present commit: latches the canon-drawn mask and rolls the frame state.
 *
 * In order: runs a terrain feed still held for a portal draw that did not
 * release it, feeds the held menu overlay moby pass rebuilt from the moby's
 * current state, turns a repeat present the game cleared into an empty one
 * (cyclorama_native_repeat_resolve), latches the canon-drawn mask, prints the census, audits the feed,
 * rolls the intervals, commits the canonical-render epoch (none of these five on
 * a repeat present, openpete_native_scene_canon_repeat), then syncs the
 * native background clear and the retained freeze backdrop from the draw
 * environment the next DrawOTag applies. */
static void render_paths_commit(void) {
    terrain_native_flush_deferred_late();   /* a feed held past its portal draw */
    moby_native_overlay_commit();           /* the menu overlay pass, at the moby's present state */
    /* A present with no game render where the game cleared the screen shows
     * an empty frame: it becomes a fresh present of an empty scene. */
    cyclorama_native_repeat_resolve();
    /* A present with no game render since the last one shows the previous
     * scene: the drawn mask and the fresh epoch stay those of the render that
     * built it, so the sub-tick re-extracts keep replaying it. */
    if (!openpete_native_scene_canon_repeat()) {
        render_paths_canon_snapshot();
        render_paths_census();
        render_paths_feed_audit();
        render_paths_frame_reset();
        /* Same anchor drives the canonical-render epoch: captures taken during
         * the present that just committed become the (only) fresh set for the
         * sub-tick re-extracts that follow. */
        spyro_native_canon_epoch_commit();
    }
    /* Sync the native background clear from g_CurDB->m_DrawEnv (the env the
     * game's DrawOTag will apply to the scene being presented) - the PutDrawEnv
     * thunk latch trails the OT build by one VSync and flashed the stale
     * homeworld clear at the portal-entry transition boundary. */
    cyclorama_native_env_clear_commit();
    /* Same DRAWENV, same instant: isbg decides whether that clear happens at
     * all, and a suppressed clear is how some games hold the last rendered
     * frame behind a menu screen. */
    ui_freeze_retain_commit();
}

bool render_path_addr_visible(uint32_t addr) {
    for (int i = 0; i < s_iv_n; i++) {
        if (addr >= s_iv[i].start && addr < s_iv[i].end)
            return render_path_enabled(s_iv[i].path);
    }
    /* The walked OT was built on the PREVIOUS render VBL (DrawOTag submits at
     * the start of a VBL, before this VBL's records) - its intervals live in
     * the rolled bank. */
    for (int i = 0; i < s_iv_prev_n; i++) {
        if (addr >= s_iv_prev[i].start && addr < s_iv_prev[i].end)
            return render_path_enabled(s_iv_prev[i].path);
    }
    return false;   /* untracked geometry: dropped while a selection is active */
}

/* The OT walk's filter hooks (psyq_game_hooks.h), defined once for every game. */
int psyq_game_render_paths_active(void) { return render_paths_active(); }

/* Documented at its declaration in psyq_game_hooks.h. */
int psyq_game_render_path_addr_visible(uint32_t addr) {
    return render_path_addr_visible(addr);
}

/* This module holds no polybuf bracket of its own: each path's feature module
 * records its RP_* range through render_path_record (directly, or through
 * hook_bracket_run). This module is the --render-paths parser, the OT-walk
 * filter above and the per-canonical-present commit below. */

static const feature_dispatch_entry_t render_paths_dispatch[] = {
    { 0u, NULL },
};

/* Dev-overlay adapters: the registry row lives in core, which cannot link
 * against this game module, so apply-options binds the live channel. */
/** @brief The render_paths row's getter: writes render_paths_current() into `buf`.
 *
 * @param k Unused.
 * @param buf Receives the text.
 * @param cap The capacity of `buf`. */
static void rp_knob_get(const knob_desc_t* k, char* buf, size_t cap) {
    (void)k;
    render_paths_current(buf, cap);
}
/** @brief The render_paths row's setter: applies the text through render_paths_set().
 *
 * A value starting with '(' is the getter's "(all)" placeholder and clears
 * the selection; "none" selects no path.
 * @param k Unused.
 * @param v The new selection text, or NULL.
 * @return 0. */
static int rp_knob_set(knob_desc_t* k, const char* v) {
    (void)k;
    if (v && *v == '(') v = NULL;   /* "(all)" placeholder = clear */
    render_paths_set(v);
    return 0;
}

/** @brief Parses --render-paths, syncs the native isolation background, and registers the per-canonical-present commit.
 *
 * Also binds the dev overlay's render_paths row and its path vocabulary; the
 * binds fail silently in
 * builds without that row. The module hooks no guest address: each path's
 * own feature module records its polybuf interval.
 * @param opts The parsed options; NULL leaves every path rendering. */
static void render_paths_apply_options(const openpete_options_t* opts) {
    render_paths_init(opts ? opts->render_paths : NULL);
    /* In isolation, an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer (which would show every path). */
    openpete_native_set_isolation(render_paths_active());
    /* Drive the per-frame canon-mask latch + interval reset from the host present
     * loop (renderer-agnostic), not psyq_DrawOTag (future-removed). */
    openpete_native_register_commit(render_paths_commit);
    openpete_native_scene_register_superseded(render_paths_superseded);
    /* Attach the live channel to the dev overlay's row (absent in dist
     * builds - ignore the miss). */
    (void)openpete_knob_bind_str("render_paths", rp_knob_get, rp_knob_set);
    (void)openpete_knob_bind_vocab("render_paths", k_names, RP_COUNT);
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = render_paths_dispatch,
};

void render_material_level_push(struct CPUState* cpu) {
    if (cpu) openpete_mods_set_level((int)cpu->read_word(game_layout.level_id));
}
