/* render_paths.c — per-path render isolation registry. See render_paths.h. */

#include "renderer/render_paths.h"
#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* CPUState, read_word — for the path brackets */
#include "openpete.h"
#include "openpete_knobs.h"          /* dev-overlay row bind */
#include "openpete_native_render.h"  /* isolation toggle */
#include "renderer/reextract.h"  /* canonical-render epoch commit */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_fiber.h"   /* openpete_frame — for the ivdump env probe */

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "shadow", "flame", "particles", "tracer",
    "transition", "portal", "glow", "sparkle",
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
        else fprintf(stderr, "[render-paths] unknown path '%s' (known: "
                             "cyclorama,spyro,moby,terrain,shadow,flame,particles,"
                             "tracer,transition,portal,glow,sparkle)\n", tok);
    }

    fprintf(stderr, "[render-paths] isolating:");
    for (int i = 0; i < RP_COUNT; i++)
        if (s_mask & (1u << i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, "%s\n", s_mask ? "" : " (none matched — nothing will render)");
}

bool render_paths_active(void) { return s_active; }

void render_paths_set(const char* spec) {
    render_paths_init(spec);
    /* In isolation an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer — same sync as boot. */
    openpete_native_set_isolation(render_paths_active());
}

void render_paths_current(char* buf, size_t cap) {
    if (!s_active) { snprintf(buf, cap, "(all)"); return; }
    if (!s_mask)   { snprintf(buf, cap, "(none)"); return; }
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
 * VBL's render functions record any intervals — so at walk time the current
 * table describes the OT being BUILT, not the one being DRAWN. The commit
 * (once per canonical present) rolls current -> previous instead of
 * discarding, and the OT-walk filter consults current first, then previous.
 * The polybuf double-buffers per frame, so the two tables cover disjoint
 * address banks and cannot false-match each other. */
#define MAX_INTERVALS 1024
typedef struct { uint32_t start, end; render_path_t path; } interval_t;
static interval_t s_iv[MAX_INTERVALS];
static int        s_iv_n = 0;
static interval_t s_iv_prev[MAX_INTERVALS];
static int        s_iv_prev_n = 0;

/* Interval-less canon-drawn marks (render_path_mark_drawn). Kept separate from
 * the intervals so they can never leak into the OT-walk addr filter. */
static uint32_t s_pending_marks = 0;

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
}

void render_path_record(render_path_t p, uint32_t start, uint32_t end) {
    if (end <= start || s_iv_n >= MAX_INTERVALS) return;
    /* OPENPETE_RENDER_PATH_IVDUMP=frame: print every recorded polybuf interval
     * on that openpete_frame with its path name (OT-prim producer attribution). */
    {
        static int s_ivdump = -2;
        if (s_ivdump == -2) {
            const char* e = getenv("OPENPETE_RENDER_PATH_IVDUMP");
            s_ivdump = (e && *e) ? atoi(e) : -1;
        }
        if (s_ivdump >= 0 && (int)openpete_frame == s_ivdump)
            fprintf(stderr, "[render-paths] ivdump f=%u path=%s [0x%08X,0x%08X)\n",
                    openpete_frame, k_names[p], start, end);
    }
    s_iv[s_iv_n].start = start;
    s_iv[s_iv_n].end   = end;
    s_iv[s_iv_n].path  = p;
    s_iv_n++;
}

/* "Canon drawn" mask — latched from the per-frame intervals once per canonical
 * render (before frame_reset clears them). A path is "drawn" iff it recorded at
 * least one non-empty polybuf interval this frame; render_path_record already
 * drops empty ranges, so a path that ran its render fn but emitted nothing (fully
 * culled / suppressed during a transition) reads as NOT drawn — which is what the
 * spine wants (reproduce only what the canonical frame actually painted). */
static uint32_t s_canon_mask = 0;

void render_paths_canon_snapshot(void) {
    uint32_t m = s_pending_marks;
    for (int i = 0; i < s_iv_n; i++) m |= (1u << (unsigned)s_iv[i].path);
    s_canon_mask = m;
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
    RPM(RP_SHADOW), RPM(RP_TRACER), RPM(RP_PORTAL), 0, 0, 0, 0, 0, 0
};
static int family_first_path(uint32_t m) { for (int p = 0; p < RP_COUNT; p++) if (m & RPM(p)) return p; return -1; }
static bool family_canon_drawn(uint32_t m) { for (int p = 0; p < RP_COUNT; p++) if ((m & RPM(p)) && render_path_canon_drawn((render_path_t)p)) return true; return false; }
static uint32_t s_feed_audit_frames[RP_COUNT];   /* frames with unmarked feed, per path */
static uint32_t s_feed_audit_tris[RP_COUNT];     /* tris fed on those frames, per path */
static uint32_t s_feed_audit_fed_frames[RP_COUNT]; /* frames the path fed at all (engagement) */
static int      s_feed_audit_registered = 0;

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
    if (s_assert < 0) { const char* e = getenv("OPENPETE_RENDER_PATH_FEED_ASSERT"); s_assert = (e && *e && *e != '0'); }
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

static void render_paths_commit(void) {
    render_paths_canon_snapshot();
    render_paths_feed_audit();
    render_paths_frame_reset();
    /* Same anchor drives the canonical-render epoch: captures taken during the
     * present that just committed become the (only) fresh set for the sub-tick
     * re-extracts that follow. */
    spyro_native_canon_epoch_commit();
    /* Sync the native background clear from g_CurDB->m_DrawEnv (the env the
     * game's DrawOTag will apply to the scene being presented) — the PutDrawEnv
     * thunk latch trails the OT build by one VSync and flashed the stale
     * homeworld clear at the portal-entry transition boundary. */
    cyclorama_native_env_clear_commit();
}

bool render_path_addr_visible(uint32_t addr) {
    for (int i = 0; i < s_iv_n; i++) {
        if (addr >= s_iv[i].start && addr < s_iv[i].end)
            return render_path_enabled(s_iv[i].path);
    }
    /* The walked OT was built on the PREVIOUS render VBL (DrawOTag submits at
     * the start of a VBL, before this VBL's records) — its intervals live in
     * the rolled bank. */
    for (int i = 0; i < s_iv_prev_n; i++) {
        if (addr >= s_iv_prev[i].start && addr < s_iv_prev[i].end)
            return render_path_enabled(s_iv_prev[i].path);
    }
    return false;   /* untracked geometry: dropped while a selection is active */
}

/* ---- polybuf brackets for paths WITHOUT a native feature module ----------
 * cyclorama/spyro/moby/flame record their intervals from their own native
 * modules. The remaining presentation-only paths (terrain/shadow/particles) get a
 * thin bracket hook here so the OT filter can isolate them in PsyCross now, even
 * before they have a native renderer. Each runs the real function exactly once
 * (gameplay writes — sector visibility, m_WasDrawn — happen identically to
 * normal dispatch) and records its polybuf range; presentation is then
 * filtered by selection in psyq_DrawOTag. Only armed when a selection is in
 * effect (else zero overhead). */
/* All per-path polybuf brackets are now retired here — each path's native
 * feature module records its own RP_* range: terrain (func_8002B9CC) →
 * terrain.c; shadow (func_80059F8C/func_80059A48) → shadow.c;
 * particles (func_800573C8/0x800580F4/0x800584C4) → particle.c;
 * cyclorama/spyro/moby/flame from their own modules. This module is now just
 * the --render-paths parser + the OT-walk filter (above). */

static const feature_dispatch_entry_t render_paths_dispatch[] = {
    { 0u, NULL },
};

/* Dev-overlay adapters: the registry row lives in core, which cannot link
 * against this game module — bind the live channel at apply-options time. */
static void rp_knob_get(const knob_desc_t* k, char* buf, size_t cap) {
    (void)k;
    render_paths_current(buf, cap);
}
static int rp_knob_set(knob_desc_t* k, const char* v) {
    (void)k;
    if (v && *v == '(') v = NULL;   /* "(all)" / "(none)" placeholder = clear */
    render_paths_set(v);
    return 0;
}

/* Feature module: parse --render-paths at startup + bracket the
 * presentation-only paths (above). cyclorama/spyro/moby bracket from their own
 * native modules. */
static void render_paths_apply_options(const openpete_options_t* opts) {
    render_paths_init(opts ? opts->render_paths : NULL);
    /* In isolation, an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer (which would show every path). */
    openpete_native_set_isolation(render_paths_active());
    /* Drive the per-frame canon-mask latch + interval reset from the host present
     * loop (renderer-agnostic), not psyq_DrawOTag (future-removed). */
    openpete_native_register_commit(render_paths_commit);
    /* Attach the live channel to the dev overlay's row (absent in dist
     * builds — ignore the miss). */
    (void)openpete_knob_bind_str("render_paths", rp_knob_get, rp_knob_set);
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = render_paths_dispatch,
};
