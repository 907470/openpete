/* render_paths.c — per-path render isolation registry. See render_paths.h. */

#include "render_paths.h"
#include "feature_registry.h"
#include "psx_runtime.h"   /* CPUState, read_word — for the path brackets */
#include "openpete.h"
#include "openpete_native_render.h"  /* isolation toggle */

#include <stdio.h>
#include <string.h>

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "shadow", "flame", "particles",
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

/* ---- polybuf brackets for paths WITHOUT a native feature module ----------
 * cyclorama/spyro/moby/flame record their intervals from their own native
 * modules. The remaining presentation-only paths (terrain/shadow/particles) get a
 * thin bracket hook here so the OT filter can isolate them in PsyCross now, even
 * before they have a native renderer. Each runs the real function exactly once
 * (gameplay writes — sector visibility, m_WasDrawn — happen identically to
 * normal dispatch) and records its polybuf range; presentation is then
 * filtered by selection in psyq_DrawOTag. Only armed when a selection is in
 * effect (else zero overhead). */
#define POLYBUF_CURSOR 0x800757B0u

extern void func_8002B9CC(CPUState* cpu);  /* environment render (-> func_800258F0 world renderer) */
extern void func_800573C8(CPUState* cpu);  /* particle systems */
extern void func_80059F8C(CPUState* cpu);  /* moby shadows */
extern void func_80059A48(CPUState* cpu);  /* Spyro silhouette shadow */

#define RP_BRACKET(hookname, realfn, pathid)                                   \
    static int hookname(CPUState* cpu, uint32_t addr) {                        \
        (void)addr;                                                            \
        if (!render_paths_active()) return 0;  /* normal dispatch; no overhead */ \
        static int s_in = 0; if (s_in) return 0; s_in = 1;                     \
        uint32_t start = cpu->read_word(POLYBUF_CURSOR);                       \
        realfn(cpu);                                                           \
        uint32_t end = cpu->read_word(POLYBUF_CURSOR);                         \
        render_path_record(pathid, start, end);                               \
        s_in = 0;                                                             \
        return 1;                                                             \
    }

RP_BRACKET(on_terrain_bracket,     func_8002B9CC, RP_TERRAIN)
RP_BRACKET(on_particles_bracket,   func_800573C8, RP_PARTICLES)
RP_BRACKET(on_shadow_moby_bracket, func_80059F8C, RP_SHADOW)
RP_BRACKET(on_shadow_spyro_bracket,func_80059A48, RP_SHADOW)

static const feature_dispatch_entry_t render_paths_dispatch[] = {
    { 0x8002B9CCu, on_terrain_bracket },
    { 0x800573C8u, on_particles_bracket },
    { 0x80059F8Cu, on_shadow_moby_bracket },
    { 0x80059A48u, on_shadow_spyro_bracket },
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
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = render_paths_dispatch,
};
