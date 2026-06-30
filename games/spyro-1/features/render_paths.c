/* render_paths.c — per-path render isolation registry. See render_paths.h. */

#include "render_paths.h"
#include "feature_registry.h"
#include "psx_runtime.h"   /* CPUState, read_word — for the path brackets */
#include "openpete.h"
#include "openpete_native_render.h"  /* isolation toggle */

#include <stdio.h>
#include <string.h>

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "shadow", "flame", "particles", "tracer",
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
                             "cyclorama,spyro,moby,terrain,flame,particles,hud)\n", tok);
    }

    fprintf(stderr, "[render-paths] isolating:");
    for (int i = 0; i < RP_COUNT; i++)
        if (s_mask & (1u << i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, "%s\n", s_mask ? "" : " (none matched — nothing will render)");
}

bool render_paths_active(void) { return s_active; }

bool render_path_enabled(render_path_t p) {
    if (!s_active) return true;                 /* no selection = all on */
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_mask & (1u << (unsigned)p)) != 0;
}

/* Per-frame intervals. Sized generously: mobys can emit many separate dispatch
 * intervals per frame. */
#define MAX_INTERVALS 1024
typedef struct { uint32_t start, end; render_path_t path; } interval_t;
static interval_t s_iv[MAX_INTERVALS];
static int        s_iv_n = 0;

void render_paths_frame_reset(void) { s_iv_n = 0; }

void render_path_record(render_path_t p, uint32_t start, uint32_t end) {
    if (end <= start || s_iv_n >= MAX_INTERVALS) return;
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
    uint32_t m = 0;
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
static void render_paths_commit(void) {
    render_paths_canon_snapshot();
    render_paths_frame_reset();
}

bool render_path_addr_visible(uint32_t addr) {
    for (int i = 0; i < s_iv_n; i++) {
        if (addr >= s_iv[i].start && addr < s_iv[i].end)
            return render_path_enabled(s_iv[i].path);
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
 * terrain_native.c; shadow (func_80059F8C/func_80059A48) → shadow_native.c;
 * particles (func_800573C8/0x800580F4/0x800584C4) → particle_native.c;
 * cyclorama/spyro/moby/flame from their own modules. This module is now just
 * the --render-paths parser + the OT-walk filter (above). */

static const feature_dispatch_entry_t render_paths_dispatch[] = {
    { 0u, NULL },
};

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
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = render_paths_dispatch,
};
