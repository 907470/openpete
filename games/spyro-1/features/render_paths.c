/* render_paths.c — per-path render isolation registry. See render_paths.h. */

#include "render_paths.h"
#include "feature_registry.h"
#include "openpete.h"
#include "openpete_native_render.h"  /* diff bg-match toggle */

#include <stdio.h>
#include <string.h>

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "flame", "particles", "hud",
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

bool render_path_addr_visible(uint32_t addr) {
    for (int i = 0; i < s_iv_n; i++) {
        if (addr >= s_iv[i].start && addr < s_iv[i].end)
            return render_path_enabled(s_iv[i].path);
    }
    return false;   /* untracked geometry: dropped while a selection is active */
}

/* Feature module: parse --render-paths at startup. No per-vaddr dispatch arms;
 * the path brackets live in the existing native feature modules. */
static void render_paths_apply_options(const openpete_options_t* opts) {
    render_paths_init(opts ? opts->render_paths : NULL);
    /* In isolation the reference background is a uniform framebuffer clear; tell
     * the diff to match it so the bg cancels (cleaner diff + path-only stats). */
    openpete_native_diff_match_ref_bg(render_paths_active());
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = NULL,
};
