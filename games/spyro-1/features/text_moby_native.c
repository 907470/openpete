// games/spyro-1/features/text_moby_native.c
//
// Render text using Spyro 1's OWN 3D HUD "text mobys" — the gold extruded glyph
// models the game draws for the gem/life counters (digits, classes 260-269),
// "DEMO MODE" / dragon names / "THE ADVENTURE BEGINS" / the pause & inventory
// menus (letters A-Z, classes 426-451) and assorted punctuation. A general
// "string -> 3D Spyro glyph mobys" engine (text_moby_native_emit); the
// performance-stats overlay (text_moby_native_perf_overlay) is its first consumer.
//
// Why these models work as a font asset (probed empirically during bring-up):
//   * Each glyph is a distinct moby CLASS whose model is g_Models[class]; '0'-'9'
//     map to class 260+N, 'A'-'Z' to 426+(c-'A'); punctuation is scattered
//     (period 327, slash 277, apostrophe 76, ...). There is no ':' model, so a
//     colon is faked by stacking two period dots.
//   * They are SimpleModels (m_NumAnimations < 0): a header where
//       byte0 = vertex count, byte1 = face count,
//       +0x04 -> vertex stream (3 bytes/vert, packed 9-bit signed VX/VY/VZ),
//       +0x0C -> face stream  (8 bytes/face; flat-shaded: w0 packs 4 vertex
//                indices @ bits {21,14,7,0} & 0x1FC >> 2, tri iff i2==i3, else
//                quad; w1's high 3 bytes are the face normal Nz/Nx/Ny).
//     (Same decode as features/moby_native.c's shaded path.)
//   * Pointers in g_Models are PHYSICAL addresses (low 0x1F_xxxx) -> OR 0x80000000.
//
// IMPORTANT: the glyph models are NOT permanently resident. They load with the
// HUD font asset (~frame 300, title/attract) but get EVICTED later (e.g. during
// the level-intro cutscene). So we SNAPSHOT their decoded geometry into a cache
// the first time they are available (text_moby_native_cache), and render purely
// from that cache forever after — immune to the later unload. Until the snapshot
// exists, ready() is false and the overlay renders nothing (no bitmap fallback).
//
// Glyphs are projected ORTHOGRAPHICALLY at a fixed window-relative spot (NOT
// through the camera: an overlay must sit at a window corner and look identical
// every frame, independent of where Spyro is looking), shaded per face by a
// fixed light, and emitted in resolution-independent overlay coordinates. The
// native renderer draws them depth-tested on the live present.
//
// PURE PRESENTATION: only READS model geometry from RAM and (in the consumer)
// calls native-render setters. Writes no gameplay-readable state.

#include "psx_runtime.h"            /* CPUState (read_word / read_byte) */
#include "openpete_native_render.h" /* openpete_hud_vert + the overlay API */
#include "openpete_menu.h"          /* menu state/layout for the settings-menu text */
#include "openpete_perf_stats.h"    /* the overlay's levels + line text */
#include "openpete_overlay_anchor.h" /* per-line alignment inside an anchored block */
#include "openpete_osd.h"          /* toast ring — the OSD builder below */
#include "openpete_fiber.h"        /* speculative / replayed ticks skip the shimmer */
#include "text_moby_native.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "spyro1_symbols.gen.h"


/* --- glyph metrics (model units) + layout (window-height fractions) --- */
#define TXT_MODEL_H   152.0f   /* glyph y-extent (-76..76) the cap-height maps to */
#define TXT_GAP       0.004f   /* gap between glyphs (window-height frac) */
#define TXT_SPACE     0.011f   /* advance for a space / missing glyph (frac) */

/* depth band: map model VZ (~±24) to a thin slice around the middle so a glyph's
 * own faces z-sort (front, +VZ, nearer = smaller depth) without being huge. */
#define TXT_DEPTH_MID 0.5f
#define TXT_DEPTH_K   0.0016f

/* Screen mapping signs, pinned by eyeballing the LIVE window (the --record path
 * matches live; an early chunky-glyph misread is what the comment guards): model
 * +X already runs right and +Y is authored down, so both pass through as +1. */
#define TXT_YSIGN     (1.0f)
#define TXT_XSIGN     (1.0f)

/* Lighting matches how the game shades these mobys (shaded_lit_color): a dark
 * base BACK-colour plus a WHITE directional light added on top — NOT a multiply.
 * Measured on the Artisans HUD (skip-to-artisans bk2 f1634, sampling the glyph
 * BODIES at y~24-64): back-colour (96,96,0) olive on unlit edges, but the digit
 * is mostly BRIGHT — median ~(182,182,~) up to ~(214,214,118). So it's front-lit
 * bright with a ~118 white add at full facing, R==G (yellow), never the saturated
 * orange-gold a multiply gives. result_i = base_i + TXT_LIGHT_ADD * max(0, N·L),
 * with a front-dominant unit light so the readable glyph fronts are bright and
 * only the grazing edges fall back toward the olive base. */
static const float TXT_L[3] = { -0.34f, 0.50f, 0.80f };   /* unit, front-dominant */
#define TXT_LIGHT_ADD 118.0f

/* Selected-menu-row wobble: peak yaw about the glyph's vertical axis. The PSX
 * pause menu uses COSINE_8((tick*8+i*12)&0xFF)*3>>9 → peak 24/256 turn ≈ ±33.75°
 * ≈ ±0.589 rad. Phase advances 8/frame, +12 per letter (a ripple along the row). */
#define TXT_WOBBLE_AMP 0.589f

/* Glyph lighting direction is per-emission so the FPS counter keeps its ORIGINAL
 * fixed front light while the settings menu and the OSD use Spyro's ROTATING
 * specular light (the metallic shimmer). emit_glyph dots the face normal with
 * this unit vector, exactly like the game's shaded_lit_color (moby_native.c)
 * does with s_shL. text_moby_native_emit installs the fixed model; the menu and
 * OSD builders install the rotating ellipse (specular_and_metal.c).
 *
 * light_signed: 0 (FPS) = add-only, faces range base..base+add. 1 (menu/OSD) =
 * SIGNED like shaded_lit_color — faces turned away darken BELOW base, giving
 * metal contrast. */

/* faked colon: stack two '.' (period, class 327) dots. The period model sits at
 * y[24,77] (mid ~50); recenter it then spread the two dots apart vertically. */
#define TXT_COLON_RECENTER (-50.0f)
#define TXT_COLON_SPREAD    44.0f
#define TXT_COLON_SCALE     0.72f

/* ---- glyph cache (snapshot of the model geometry, immune to later unload) ---
 * One entry per supported class; the largest glyph seen is ~62 verts / 67 faces. */
#define TXT_MAX_GLYPH_V 128
#define TXT_MAX_GLYPH_F 128
typedef struct {
    int      valid;
    int      vcount, fcount, vxmin, vxmax;
    int16_t  vx[TXT_MAX_GLYPH_V], vy[TXT_MAX_GLYPH_V], vz[TXT_MAX_GLYPH_V];
    uint32_t fw0[TXT_MAX_GLYPH_F], fw1[TXT_MAX_GLYPH_F];
} glyph_cache_t;

/* the classes we know how to render (digits, A-Z, punctuation). */
static const int SUPPORTED_CLASSES[] = {
    260,261,262,263,264,265,266,267,268,269,                 /* 0-9 */
    426,427,428,429,430,431,432,433,434,435,436,437,438,     /* A-M */
    439,440,441,442,443,444,445,446,447,448,449,450,451,     /* N-Z */
    272,277,278,317,321,327,76,                              /* % / ? + ^ . ' */
};
#define NGLYPH ((int)(sizeof(SUPPORTED_CLASSES) / sizeof(SUPPORTED_CLASSES[0])))

static glyph_cache_t s_glyph[NGLYPH];
static int s_class_to_idx[512];   /* class -> slot in s_glyph, or -1 */
static int s_idx_init = 0;
static int s_cache_ready = 0;
/* Common advance for '0'-'9' (model units) — see text_moby_native_cache. */
static int s_digit_adv = 0;

static int is_digit_class(int cls) { return cls >= 260 && cls <= 269; }

static float glyph_natural_w(int cls) {
    const int slot = (cls >= 0 && cls < 512) ? s_class_to_idx[cls] : -1;
    if (slot < 0 || !s_glyph[slot].valid) return -1.0f;
    return (float)(s_glyph[slot].vxmax - s_glyph[slot].vxmin);
}

/* How much room a character takes, and how far into that room its glyph sits
 * (overlay units). Everything but a digit is proportional — advance is the
 * glyph width, inset 0. A digit takes the shared tabular advance and is
 * centred in it. Both emit_string and measure_string go through here, so the
 * two can never disagree about a string's width. adv < 0 = not cached. */
static void glyph_metrics(int cls, float scale, float* adv, float* inset) {
    const float w = glyph_natural_w(cls);
    if (w < 0.0f) { *adv = -1.0f; *inset = 0.0f; return; }
    if (is_digit_class(cls) && s_digit_adv > 0) {
        *adv   = (float)s_digit_adv * scale;
        *inset = 0.5f * (*adv - w * scale);   /* centred in its column */
    } else {
        *adv   = w * scale;
        *inset = 0.0f;
    }
}

/* char -> moby class, or -1 for "advance only" (space / unsupported). */
static int glyph_class(char c) {
    if (c >= '0' && c <= '9') return 260 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 426 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 426 + (c - 'a');
    switch (c) {
        case '%':  return 272;
        case '/':  return 277;
        case '?':  return 278;
        case '+':  return 317;
        case '^':  return 321;
        case '.':  return 327;
        case '\'': return 76;
        default:   return -1;
    }
}

/* Decode one glyph model from RAM into its cache slot. Marks invalid (and leaves
 * it so) if the model isn't a loaded SimpleModel. */
static void cache_one(CPUState* cpu, int slot, int cls) {
    glyph_cache_t* g = &s_glyph[slot];
    g->valid = 0;
    uint32_t model = cpu->read_word(G_MODELS + (uint32_t)cls * 4u);
    if (model == 0u) return;
    if (model < 0x80000000u) model |= 0x80000000u;
    if ((int32_t)cpu->read_word(model) >= 0) return;     /* not a SimpleModel / data not in */
    int vcount = cpu->read_byte(model + 0x0u) & 0xFF;
    int fcount = cpu->read_byte(model + 0x1u) & 0xFF;
    if (vcount <= 0 || vcount > TXT_MAX_GLYPH_V || fcount > TXT_MAX_GLYPH_F) return;
    uint32_t vbase = cpu->read_word(model + 0x4u); if (vbase < 0x80000000u) vbase |= 0x80000000u;
    uint32_t faces = cpu->read_word(model + 0xCu); if (faces < 0x80000000u) faces |= 0x80000000u;
    int vxmin = 1 << 30, vxmax = -(1 << 30);
    for (int i = 0; i < vcount; i++) {
        uint32_t p = vbase + (uint32_t)i * 3u;
        uint32_t w = cpu->read_byte(p) | (cpu->read_byte(p + 1) << 8) | (cpu->read_byte(p + 2) << 16);
        int VZ = ((int32_t)(w << 24)) >> 23;
        int VX = ((int32_t)(w << 16)) >> 23;
        int VY = ((int32_t)(w <<  8)) >> 23;
        g->vx[i] = (int16_t)VX; g->vy[i] = (int16_t)VY; g->vz[i] = (int16_t)VZ;
        if (VX < vxmin) vxmin = VX;
        if (VX > vxmax) vxmax = VX;
    }
    for (int f = 0; f < fcount; f++) {
        uint32_t fp = faces + (uint32_t)f * 8u;
        g->fw0[f] = cpu->read_word(fp);
        g->fw1[f] = cpu->read_word(fp + 4u);
    }
    g->vcount = vcount; g->fcount = fcount; g->vxmin = vxmin; g->vxmax = vxmax;
    g->valid = 1;
}

void text_moby_native_cache(CPUState* cpu) {
    if (s_cache_ready) return;
    if (!s_idx_init) {
        for (int i = 0; i < 512; i++) s_class_to_idx[i] = -1;
        for (int s = 0; s < NGLYPH; s++) s_class_to_idx[SUPPORTED_CLASSES[s]] = s;
        s_idx_init = 1;
    }
    /* Gate on digit '0' being a loaded SimpleModel with real geometry — the whole
     * font asset loads together, so this means the rest are in too. */
    uint32_t m0 = cpu->read_word(G_MODELS + 260u * 4u);
    if (m0 == 0u) return;
    if (m0 < 0x80000000u) m0 |= 0x80000000u;
    if ((int32_t)cpu->read_word(m0) >= 0) return;        /* pointer set but data not yet in */
    if ((cpu->read_byte(m0) & 0xFF) == 0) return;        /* vcount 0 -> not ready */
    for (int s = 0; s < NGLYPH; s++) cache_one(cpu, s, SUPPORTED_CLASSES[s]);
    /* TABULAR FIGURES. The glyph models are proportional — '1' is markedly
     * narrower than '4' — so a number redrawn with different digits changes
     * width, and any text laid out from its measured width then reflows. On a
     * right-aligned readout that updates twice a second (the FPS counter) the
     * whole line visibly slides back and forth as the value changes.
     *
     * So every digit gets the SAME advance, the widest one's, and is centred
     * in it. A number's width then depends only on how many digits it has.
     * Done in the emitter rather than per consumer, so measure_string and
     * emit_string cannot disagree about it and every consumer — the counter,
     * the menu's values, the OSD, a mod's HUD — gets columns that line up. */
    s_digit_adv = 0;
    for (int d = 0; d < 10; d++) {
        const glyph_cache_t* g = &s_glyph[s_class_to_idx[260 + d]];
        if (g->valid && (g->vxmax - g->vxmin) > s_digit_adv)
            s_digit_adv = g->vxmax - g->vxmin;
    }
    s_cache_ready = 1;                                    /* snapshot taken; never re-read RAM */
}

int text_moby_native_ready(void) { return s_cache_ready; }

/* ---- emit (works purely from the cache) --------------------------------- */

/* Emission state, per call rather than per file. Every emitter — the stats
 * overlay and settings menu on the sim thread, the mod draw service on the
 * present thread, the OSD every tick a toast is live — writes through one of
 * these, so two threads emitting at once cannot tear each other's output
 * cursor, glyph scratch or light direction. (A torn out/cap pair defeats
 * push_tri's bounds check and writes past the caller's buffer; a light
 * overwritten mid-string tears the shimmer across the glyphs.) */
typedef struct {
    openpete_hud_vert* out;    /* caller's buffer                      */
    int   cap;                 /* its capacity, in verts               */
    int   n;                   /* verts written so far                 */
    float light[3];            /* unit light direction (N·L)           */
    int   light_signed;        /* see the note above                   */
    /* per-glyph vertices in overlay space (filled by emit_glyph). */
    float gx[TXT_MAX_GLYPH_V], gy[TXT_MAX_GLYPH_V], gz[TXT_MAX_GLYPH_V];
} emit_ctx_t;

/* Start an emission into `out`, lit by the fixed front model (the FPS
 * counter's original look). Callers wanting the rotating specular overwrite
 * cx->light / cx->light_signed afterwards. */
static void emit_ctx_init(emit_ctx_t* cx, openpete_hud_vert* out, int cap) {
    cx->out = out;
    cx->cap = cap;
    cx->n   = 0;
    cx->light[0] = TXT_L[0]; cx->light[1] = TXT_L[1]; cx->light[2] = TXT_L[2];
    cx->light_signed = 0;
}

static void push_tri(emit_ctx_t* cx, int a, int b, int c, uint32_t rgba) {
    if (!cx->out || cx->n + 3 > cx->cap) return;
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[a], cx->gy[a], cx->gz[a], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[b], cx->gy[b], cx->gz[b], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[c], cx->gy[c], cx->gz[c], rgba };
}

/* Emit one cached glyph with its left edge at cursor_x, centred on center_y.
 * dy_model shifts the model vertically (model units) — used for the colon dots.
 * baseR/G/B is the (0..255) glyph colour. Returns the glyph width (overlay
 * frac), or -1 if the glyph isn't in the cache. */
static float emit_glyph(emit_ctx_t* cx, int cls, float cursor_x, float center_y,
                        float scale, float dy_model, float yaw,
                        int baseR, int baseG, int baseB) {
    int slot = (cls >= 0 && cls < 512) ? s_class_to_idx[cls] : -1;
    if (slot < 0 || !s_glyph[slot].valid) return -1.0f;
    const glyph_cache_t* g = &s_glyph[slot];
    int vcount = g->vcount, fcount = g->fcount, vxmin = g->vxmin, vxmax = g->vxmax;
    if (vcount > TXT_MAX_GLYPH_V) vcount = TXT_MAX_GLYPH_V;
    /* Selected-row wobble: yaw about the glyph's own vertical axis (its mid-X), so
     * it turns left↔right revealing the extrusion; the normals rotate too (below)
     * so the directional highlight sweeps. yaw==0 → the original front-facing glyph
     * (cs=1, sn=0 leaves X/Z untouched). The advance width is the un-yawed width,
     * so letters keep their slots and just rock in place. */
    const float cs = cosf(yaw), sn = sinf(yaw);
    const float midx = 0.5f * (float)(vxmin + vxmax);
    for (int i = 0; i < vcount; i++) {
        float my = (float)g->vy[i] + dy_model;
        float dx = (float)g->vx[i] - midx;      /* centre X before rotating */
        float dz = (float)g->vz[i];
        float vx_r = midx + dx * cs + dz * sn;  /* yawed X */
        float mz   =       -dx * sn + dz * cs;  /* yawed Z (depth) */
        float gx_local = (TXT_XSIGN < 0.0f) ? (vxmax - vx_r) : (vx_r - vxmin);
        cx->gx[i] = cursor_x + gx_local * scale;
        cx->gy[i] = center_y + TXT_YSIGN * my * scale;
        cx->gz[i] = TXT_DEPTH_MID - mz * TXT_DEPTH_K;        /* front nearer */
    }
    for (int f = 0; f < fcount; f++) {
        uint32_t w0 = g->fw0[f], w1 = g->fw1[f];
        int i0 = (int)((w0 >> 21) & 0x1FCu) >> 2;
        int i1 = (int)((w0 >> 14) & 0x1FCu) >> 2;
        int i2 = (int)((w0 >>  7) & 0x1FCu) >> 2;
        int i3 = (int)( w0        & 0x1FCu) >> 2;
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        float nz0 = (float)(int8_t)(w1 >> 24);
        float nx0 = TXT_XSIGN * (float)(int8_t)(w1 >> 16); /* match mirrored geometry */
        float ny  = (float)(int8_t)(w1 >>  8);
        float nx = nx0 * cs + nz0 * sn;                    /* yaw the normal with the glyph */
        float nz = -nx0 * sn + nz0 * cs;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-3f) nl = 1.0f;
        float t = (nx * cx->light[0] + ny * cx->light[1] + nz * cx->light[2]) / nl; /* N·L (unit L) */
        if (cx->light_signed) { if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f; }
        else                { if (t <  0.0f) t =  0.0f; else if (t >  1.0f) t =  1.0f; }
        int add = (int)(TXT_LIGHT_ADD * t);          /* signed (menu) darkens shadowed faces */
        int R = baseR + add; if (R < 0) R = 0; else if (R > 255) R = 255;
        int G = baseG + add; if (G < 0) G = 0; else if (G > 255) G = 255;
        int B = baseB + add; if (B < 0) B = 0; else if (B > 255) B = 255;
        uint32_t rgba = (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
        /* Draw front AND back faces; the depth test keeps the (nearer) front and
         * the edge-on side walls have ~zero area — so no winding cull is needed. */
        if (i2 == i3) {
            push_tri(cx, i0, i1, i2, rgba);                   /* tri */
        } else if (i3 < vcount) {
            push_tri(cx, i0, i1, i2, rgba);                   /* quad = 2 tris */
            push_tri(cx, i1, i3, i2, rgba);
        }
    }
    return (float)(vxmax - vxmin) * scale;
}

/* Emit one string's glyphs into `out`. wob_phase >= 0 applies the selected-row
 * yaw wobble (a per-glyph phase ripple, like the PSX pause menu); < 0 = upright. */
static int emit_string(emit_ctx_t* cx, const char* str,
                       float origin_x, float origin_y, float cap_h,
                       unsigned base_rgb, int wob_phase) {
    if (!s_cache_ready || !cx->out || cx->cap <= 0 || !str) return 0;
    const int n_before = cx->n;
    const int baseR = (int)((base_rgb >> 16) & 0xFFu);
    const int baseG = (int)((base_rgb >>  8) & 0xFFu);
    const int baseB = (int)( base_rgb        & 0xFFu);
    const float scale    = cap_h / TXT_MODEL_H;
    const float center_y = origin_y + cap_h * 0.5f;
    float cursor = origin_x;
    int gi = 0;   /* non-space glyph index — drives the wobble ripple */

    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        float yaw = 0.0f;
        if (wob_phase >= 0) {
            unsigned au = ((unsigned)wob_phase * 8u + (unsigned)gi * 12u) & 0xFFu;
            yaw = cosf((float)au * (6.2831853f / 256.0f)) * TXT_WOBBLE_AMP;
        }
        if (c == ':') {                                  /* faked colon: two dots */
            float w = emit_glyph(cx, 327, cursor, center_y, scale * TXT_COLON_SCALE,
                                 TXT_COLON_RECENTER + TXT_COLON_SPREAD, yaw, baseR, baseG, baseB);
            if (w < 0.0f) { cursor += TXT_SPACE; continue; }
            emit_glyph(cx, 327, cursor, center_y, scale * TXT_COLON_SCALE,
                       TXT_COLON_RECENTER - TXT_COLON_SPREAD, yaw, baseR, baseG, baseB);
            cursor += w + TXT_GAP; gi++;
            continue;
        }
        int cls = glyph_class(c);
        if (cls < 0) { cursor += TXT_SPACE; continue; }
        float adv, inset;
        glyph_metrics(cls, scale, &adv, &inset);
        if (adv < 0.0f) { cursor += TXT_SPACE; continue; }  /* not cached -> skip */
        if (emit_glyph(cx, cls, cursor + inset, center_y, scale, 0.0f, yaw,
                       baseR, baseG, baseB) < 0.0f) {
            cursor += TXT_SPACE; continue;
        }
        cursor += adv + TXT_GAP; gi++;
    }
    return cx->n - n_before;   /* verts THIS string added */
}

int text_moby_native_emit(const char* str,
                          float origin_x, float origin_y, float cap_h,
                          unsigned base_rgb,
                          openpete_hud_vert* out, int out_cap) {
    /* FPS counter etc. keep the original fixed front light + add-only shading. */
    emit_ctx_t cx;
    emit_ctx_init(&cx, out, out_cap);
    return emit_string(&cx, str, origin_x, origin_y, cap_h, base_rgb, -1);
}

/* Mod draw-service text rasterizer (core/mods/mod_draw.c; registered in
 * main.cpp). void* out keeps the core-facing fn type free of the
 * openpete_hud_vert dependency. The glyph cache is populated by the
 * per-tick stats-overlay driver below (unconditionally under the native
 * renderer, at every level including off), so at present time we only read
 * the snapshot: mod text appears as soon as the game has loaded the font —
 * absent before that (no bitmap fallback exists). */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap) {
    if (!text_moby_native_ready()) return 0;
    return text_moby_native_emit(str, x, y, cap_h, rgb,
                                 (openpete_hud_vert*)out_verts, cap);
}

/* Advance width of `str` at `cap_h` (defined with the menu text below, which
 * is its other consumer): the perf readout needs it to right-align / centre a
 * line inside its region. */
static float measure_string(const char* str, float cap_h);

/* ---- performance-stats overlay (the first consumer) --------------------- */
/* Sized for the detailed readout's four lines, not the one-line counter; must
 * not exceed the renderer's OVL_MAX_VERTS, which drops the overflow. */
#define PERF_OVL_MAX   16384
#define PERF_ORIGIN_X  0.014f
#define PERF_ORIGIN_Y  0.016f
#define PERF_CAP_H     0.020f   /* small corner counter */
#define PERF_LINE_STEP 0.030f   /* cap height + a line's worth of leading */
#define PERF_TINT      0x606000u   /* the game's HUD text back-colour, olive (96,96,0) */
/* Region WIDTH is a per-level constant, deliberately not measured from the live
 * text: the readout's digits change twice a second, and a content-measured
 * width would resize the renderer's supersample target that often — every
 * resize idles the device. The slack is absorbed by per-line alignment
 * instead, so a right-anchored block still sits flush against the edge. */
#define PERF_REGION_W        0.30f
#define PERF_REGION_W_DETAIL 0.45f

static openpete_hud_vert s_perf_buf[PERF_OVL_MAX];

/* What the buffer currently holds. This runs once per tick, and the readout
 * only moves twice a second, so re-emitting every tick would spend the glyph
 * builder's whole cost ~15× over for identical geometry — on the thread that
 * owns the tick deadline, which is precisely what the detailed level exists
 * to measure. Rebuild only when the text (or the layout it was built for)
 * actually changes. */
static char s_shown[OPENPETE_PERF_MAX_LINES][OPENPETE_PERF_LINE_MAX];
static int  s_shown_n = -1;   /* -1 = nothing pushed yet */
static int  s_shown_anchor = -1;

void text_moby_native_perf_overlay(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;   /* inert headless: no cache, no draw */
    text_moby_native_cache(cpu);                     /* snapshot the font as soon as available */
    int level = openpete_native_perf_stats_level();
    if (level <= OPENPETE_PERF_OFF || !text_moby_native_ready()) {
        if (s_shown_n != 0) {
            openpete_native_set_overlay_glyphs(NULL, 0); /* nothing until cached (no fallback) */
            s_shown_n = 0;
        }
        return;
    }

    char lines[OPENPETE_PERF_MAX_LINES][OPENPETE_PERF_LINE_MAX];
    int nl = openpete_perf_stats_lines(level, lines, OPENPETE_PERF_MAX_LINES);
    const int anchor = openpete_native_perf_anchor();
    if (nl == s_shown_n && anchor == s_shown_anchor &&
        memcmp(lines, s_shown, sizeof lines) == 0)
        return;                                      /* geometry already correct */

    /* The region this block occupies. Height follows the live line count so a
     * one-line counter doesn't reserve four lines of screen (and four lines of
     * supersample target); width is the per-level constant above. The renderer
     * reads this back for the supersample target, the composite quad and the
     * anchor ledger's claim — one owner, no third place to disagree. */
    const float region_w = (level >= OPENPETE_PERF_DETAIL) ? PERF_REGION_W_DETAIL
                                                           : PERF_REGION_W;
    const float region_h = 2.0f * PERF_ORIGIN_Y + PERF_CAP_H
                         + (float)(nl > 0 ? nl - 1 : 0) * PERF_LINE_STEP;
    openpete_native_set_overlay_extent(region_w, region_h);

    /* Align EACH LINE, not the block: the lines differ in length, and aligning
     * the block alone would leave a right-anchored readout with a ragged right
     * edge against the screen edge it is hugging. */
    /* Right-aligned and centred text derives its origin from the MEASURED
     * string, so the origin moves whenever the readout does — and the readout
     * changes twice a second. Unsnapped, that lands the glyphs at a different
     * sub-pixel phase every update, which reads as the counter softening and
     * crawling rather than simply changing value. Left-aligned text never had
     * this: its origin is a constant. Snap to a whole output pixel so every
     * update reproduces the same phase. */
    const float ppu = openpete_native_overlay_px_per_unit();
    int nv = 0;
    for (int i = 0; i < nl && nv < PERF_OVL_MAX; i++) {
        float x = PERF_ORIGIN_X;
        if (openpete_anchor_is_right(anchor)) {
            x = region_w - PERF_ORIGIN_X - measure_string(lines[i], PERF_CAP_H);
        } else if (openpete_anchor_is_center(anchor)) {
            x = 0.5f * (region_w - measure_string(lines[i], PERF_CAP_H));
        }
        if (ppu > 0.0f) x = (float)(int)(x * ppu + 0.5f) / ppu;
        if (x < PERF_ORIGIN_X) x = PERF_ORIGIN_X;   /* never start off the region */
        /* One buffer, one push: each line emits into the tail of what the
         * previous one wrote, so the whole readout reaches the renderer as a
         * single glyph list rather than overwriting itself line by line. */
        nv += text_moby_native_emit(lines[i], x,
                                    PERF_ORIGIN_Y + (float)i * PERF_LINE_STEP,
                                    PERF_CAP_H, PERF_TINT,
                                    s_perf_buf + nv, PERF_OVL_MAX - nv);
    }
    openpete_native_set_overlay_glyphs(nv > 0 ? s_perf_buf : NULL, nv);
    memcpy(s_shown, lines, sizeof lines);
    s_shown_n = nl;
    s_shown_anchor = anchor;
}

/* ---- the gold shimmer's phase ------------------------------------------
 * Spyro's menus and cutscenes run SpecularUpdate, a light source travelling
 * an ellipse, and the metallic sweep across the gold glyphs is that light
 * turning. The game does not run it during gameplay (and we must not write
 * its RAM), so it is reproduced host-side.
 *
 * It lives here, free-standing, rather than being derived from the settings
 * menu's wobble tick: the OSD's toasts shimmer whether or not the menu is
 * open, and a counter that only advanced while the menu was open would leave
 * every toast lit by a frozen light.
 *
 * Advanced at the game's LOGIC rate — every other call, since this runs once
 * per sim tick (paced per VBL, ~59.826 Hz) while the PSX bumps its rotation
 * once per DRAWN frame (draw.c ++D_800758B8). Speculative and replayed ticks
 * sit it out: those are rolled back or re-executed, and letting them advance
 * makes the shimmer sprint through a rewind.
 *
 * One cosmetic consequence, priced rather than assumed: the wobble and the
 * specular used to share one phase that only ran while the menu was open, so
 * the glint sat at the same offset on every open. It now free-runs, so each
 * open starts at a different offset. Imperceptible, and the alternative is a
 * frozen glint on every toast. */
static unsigned s_shimmer_phase = 0;

void text_moby_native_shimmer_tick(void) {
    if (openpete_speculative || openpete_replaying) return;
    static unsigned s_call = 0;
    if ((s_call++ & 1u) == 0u) s_shimmer_phase++;
}

unsigned text_moby_native_shimmer_phase(void) { return s_shimmer_phase; }

/* Install SpecularUpdate's rotating ellipse as this emission's light. Same
 * N·L shading as the world's shaded mobys (moby_native.c shaded_lit_color),
 * so every glyph's highlight sweeps as the light turns. The clock advances
 * +3 per logic frame, like the game's SpecularUpdate(3). */
static void install_specular_light(emit_ctx_t* cx, unsigned phase) {
    const unsigned sp = (phase * 3u) & 0xFFu;
    const float sth = (float)sp * (6.2831853f / 256.0f);
    float Lx = cosf(sth) * 0.968f;   /* COSINE_8 * 1983 >> 11 */
    float Ly = sinf(sth) * 1.549f;   /* SINE_8   * 6345 >> 12 */
    float Lz = -0.25f;               /* -1024 / 4096 (small fixed into-screen) */
    float Ll = sqrtf(Lx*Lx + Ly*Ly + Lz*Lz); if (Ll < 1e-3f) Ll = 1.0f;
    cx->light[0] = Lx/Ll; cx->light[1] = Ly/Ll; cx->light[2] = Lz/Ll;
    cx->light_signed = 1;   /* metal contrast: shadowed faces darken below base */
}

/* ---- in-game settings menu text (labels / values / title) --------------- */
#define MENU_TXT_MAX    32768   /* must match MENU_GLYPH_MAX (native_render.c);
                                 * 8 rows of label+value+title+border overflowed
                                 * 16384 and emit_string drops trailing glyphs */
/* Mid-tone gold BASE — the signed light swings faces both ways around it (lit →
 * white-gold, shadowed → dark), so base is the average, not the floor. The game's
 * HUD/menu gold is R==G with a low B (measured (96,96,0)→(214,214,118)); R>G is
 * what read as ORANGE, so keep R==G. The uniform white light add preserves R==G. */
#define MENU_TXT_TINT   0x96962Au   /* mid yellow-gold (R150,G150,B42) */
#define MENU_TITLE_TINT 0xB4B43Eu   /* brighter for the title (R180,G180,B62) */

static openpete_hud_vert s_menu_buf[MENU_TXT_MAX];

/* ---- lit gold border ---------------------------------------------------- */
/* The frame around the panel is drawn here (not as flat quads in native_render)
 * so it receives the EXACT same lighting as the glyph rows: the emission's light
 * (SpecularUpdate's rotating ellipse), signed N·L, TXT_LIGHT_ADD. The frame is a
 * closed quad strip (outer = panel rect, inner = inset toward the panel centre by
 * MENU_BORDER_BT); each perimeter vertex carries an outward RADIAL normal in the
 * overlay XY plane (z=0, same convention as the glyph normals), so a specular
 * glint sweeps around the frame in lock-step with the text shimmer. Colour is
 * per-vertex and interpolates along the perimeter → a smooth travelling
 * highlight, not a per-edge on/off. Mid-gold BASE (R==G, low B — same rationale
 * as the text tint) so the signed light swings it dark↔bright-gold either side. */
#define MENU_BORDER_R   190
#define MENU_BORDER_G   190
#define MENU_BORDER_B    80
#define MENU_BORDER_BT   0.0035f  /* frame thickness, overlay units */
#define MENU_BORDER_SAMP 12       /* perimeter samples per edge (smooth glint) */

static uint32_t border_lit(const emit_ctx_t* cx, float nx, float ny) {
    float t = nx * cx->light[0] + ny * cx->light[1];   /* N·L, N=(nx,ny,0) unit; signed like the glyphs */
    if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f;
    int add = (int)(TXT_LIGHT_ADD * t);
    int R = MENU_BORDER_R + add; if (R < 0) R = 0; else if (R > 255) R = 255;
    int G = MENU_BORDER_G + add; if (G < 0) G = 0; else if (G > 255) G = 255;
    int B = MENU_BORDER_B + add; if (B < 0) B = 0; else if (B > 255) B = 255;
    return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
}

/* Emit a lit frame around (x0,y0)-(x1,y1) into `out` (lit by ecx's light — call
 * after the builder has installed the rotating specular). Returns verts written. Four straight edge strips: the
 * outer edge lies on the panel rect, the inner edge is a PERPENDICULAR inset by
 * bt (so each strip is a clean parallel-sided rectangle — no inward bulge). Each
 * strip is subdivided along its length; every column's colour comes from the
 * outward RADIAL normal at its outer point, so the glint travels smoothly along
 * the edge. Adjacent strips meet at a shared corner whose radial normal is
 * identical from either side, so the overlap is seamless. */
static int emit_lit_border(const emit_ctx_t* ecx,
                           float x0, float y0, float x1, float y1, float thickness,
                           openpete_hud_vert* out, int cap) {
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float bt = thickness, z = TXT_DEPTH_MID;
    const int   S  = MENU_BORDER_SAMP;
    /* {outerA, outerB, innerA, innerB} per strip; inner = outer inset by bt. */
    const float strip[4][8] = {
        { x0,y0,   x1,y0,    x0,y0+bt, x1,y0+bt },   /* top    */
        { x1,y0,   x1,y1,    x1-bt,y0, x1-bt,y1 },   /* right  */
        { x1,y1,   x0,y1,    x1,y1-bt, x0,y1-bt },   /* bottom */
        { x0,y1,   x0,y0,    x0+bt,y1, x0+bt,y0 },   /* left   */
    };
    int n = 0;
    for (int e = 0; e < 4; e++) {
        const float* s = strip[e];
        float pox = 0, poy = 0, pix = 0, piy = 0; uint32_t pcol = 0;
        for (int c = 0; c <= S; c++) {
            const float f   = (float)c / (float)S;
            const float oxf = s[0] + (s[2] - s[0]) * f, oyf = s[1] + (s[3] - s[1]) * f;
            const float ixf = s[4] + (s[6] - s[4]) * f, iyf = s[5] + (s[7] - s[5]) * f;
            float dx = oxf - cx, dy = oyf - cy;               /* outward radial (lighting only) */
            float dl = sqrtf(dx * dx + dy * dy); if (dl < 1e-6f) dl = 1.0f;
            const uint32_t col = border_lit(ecx, dx / dl, dy / dl);
            if (c > 0 && n + 6 <= cap) {
                out[n++] = (openpete_hud_vert){ pox, poy, z, pcol };
                out[n++] = (openpete_hud_vert){ oxf, oyf, z, col  };
                out[n++] = (openpete_hud_vert){ ixf, iyf, z, col  };
                out[n++] = (openpete_hud_vert){ pox, poy, z, pcol };
                out[n++] = (openpete_hud_vert){ ixf, iyf, z, col  };
                out[n++] = (openpete_hud_vert){ pix, piy, z, pcol };
            }
            pox = oxf; poy = oyf; pix = ixf; piy = iyf; pcol = col;
        }
    }
    return n;
}

/* Advance width of `str` at `cap_h`, in overlay units — mirrors emit_string's
 * cursor accumulation exactly (per-glyph width + TXT_GAP, TXT_SPACE for spaces,
 * the two-dot colon). Used to right-align labels and centre the title so the
 * menu reads like the game's own pause menu (right-aligned labels, left-aligned
 * values, meeting at a central split). */
static float measure_string(const char* str, float cap_h) {
    if (!s_cache_ready || !str) return 0.0f;
    const float scale = cap_h / TXT_MODEL_H;
    float cursor = 0.0f;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        if (c == ':') {
            int slot = s_class_to_idx[327];
            if (slot < 0 || !s_glyph[slot].valid) { cursor += TXT_SPACE; continue; }
            cursor += (float)(s_glyph[slot].vxmax - s_glyph[slot].vxmin)
                      * scale * TXT_COLON_SCALE + TXT_GAP;
            continue;
        }
        int cls = glyph_class(c);
        float adv, inset;
        glyph_metrics(cls, scale, &adv, &inset);
        if (adv < 0.0f) { cursor += TXT_SPACE; continue; }
        cursor += adv + TXT_GAP;
    }
    return cursor;
}

/* Build the settings-menu glyph text (title + each row's label/value) from the
 * same gold glyph mobys, with the selected row's per-glyph yaw wobble, and push
 * it to the native renderer (draw_menu_overlay draws it over the panel). Reads
 * the menu's shared layout so text lands on the panel's rows. Pure presentation. */
void text_moby_native_menu(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    text_moby_native_cache(cpu);
    if (!openpete_menu_is_open() || !text_moby_native_ready()) {
        openpete_native_set_menu_glyphs(NULL, 0);
        return;
    }
    /* Advance the wobble at the game's LOGIC rate (~29.913 Hz). This builder runs
     * once per sim tick — which openpete paces per-VBL (~59.826 Hz) — but the PSX
     * pause menu bumps its rotation tick once per *drawn* frame (30 fps, draw.c
     * ++D_800758B8). So advance every other call to match the game's speed. */
    static unsigned s_menu_call = 0;
    if ((s_menu_call++ & 1u) == 0u) openpete_menu_tick();
    const int phase = (int)(openpete_menu_wobble_phase() & 0x7FFFFFFFu);
    openpete_menu_layout_t L;
    openpete_menu_get_layout(&L);
    const int cursor = openpete_menu_cursor();

    /* One emission for the whole menu: the title, every row and the frame share
     * this context's rotating light, and nothing else can perturb it mid-build. */
    emit_ctx_t cx;
    emit_ctx_init(&cx, s_menu_buf, MENU_TXT_MAX);

    install_specular_light(&cx, text_moby_native_shimmer_phase());

    /* Pause-menu alignment: title centred; labels RIGHT-aligned to L.label_x;
     * values LEFT-aligned at L.value_x (the two columns meet at a central split). */
    const float pcx = 0.5f * (L.panel_x0 + L.panel_x1);   /* panel centre x */
    {
        const char* title = openpete_menu_title();
        const float tw = measure_string(title, L.title_cap_h);
        emit_string(&cx, title, pcx - 0.5f * tw,
                    L.title_y - L.title_cap_h * 0.5f, L.title_cap_h,
                    MENU_TITLE_TINT, -1);
    }
    /* Uniform row font: shrink EVERY row (labels + values; the SETTINGS title
     * keeps its own cap) to the size the widest value needs to fit its column
     * — "FULLSCREEN" — so no single row reads smaller than its neighbours.
     * Measured from the FIXED reference string, not the live values, so the
     * size never breathes as a value toggles (WINDOWED <-> FULLSCREEN).
     * measure_string is linear in cap_h, so one proportional shrink is exact.
     * Rows keep their spacing (L.row_y); only the glyph size changes. */
    const float vavail = (L.panel_x1 - 0.015f) - L.value_x;
    float row_cap = L.row_cap_h;
    {
        const float w = measure_string("FULLSCREEN", L.row_cap_h);
        if (w > vavail && w > 0.0f) row_cap = L.row_cap_h * (vavail / w);
    }
    for (int r = 0; r < L.row_count; r++) {
        const int wob = (r == cursor) ? phase : -1;   /* only the selected row wobbles */
        char vb[16];
        const char* label = openpete_menu_row_label(r);
        const char* value = openpete_menu_row_value(r, vb, sizeof vb);
        /* Action rows (EXIT GAME) have no value: centre the label on the panel
         * so it reads as a button rather than a setting whose value went
         * missing. Shrunk if the armed text outgrows the panel width. */
        if (openpete_menu_row_centered(r)) {
            float ccap = row_cap;
            const float cavail = (L.panel_x1 - 0.015f) - (L.panel_x0 + 0.015f);
            const float cw0 = measure_string(label, ccap);
            if (cw0 > cavail && cw0 > 0.0f) ccap *= cavail / cw0;
            const float cw = measure_string(label, ccap);
            emit_string(&cx, label, pcx - 0.5f * cw,
                        L.row_y[r] - ccap * 0.5f, ccap, MENU_TXT_TINT, wob);
            continue;
        }
        const float lw = measure_string(label, row_cap);
        emit_string(&cx, label, L.label_x - lw,
                    L.row_y[r] - row_cap * 0.5f, row_cap, MENU_TXT_TINT, wob);
        /* Safety net for any future value wider than the reference: shrink
         * that one value further so it still ends before the panel border. */
        float vcap = row_cap;
        const float vw = measure_string(value, vcap);
        if (vw > vavail && vw > 0.0f) vcap *= vavail / vw;
        emit_string(&cx, value, L.value_x, L.row_y[r] - vcap * 0.5f, vcap,
                    MENU_TXT_TINT, wob);
    }
    /* Gold frame — same rotating light as the rows above (cx still carries it). */
    cx.n += emit_lit_border(&cx, L.panel_x0, L.panel_y0, L.panel_x1, L.panel_y1,
                            MENU_BORDER_BT, s_menu_buf + cx.n, MENU_TXT_MAX - cx.n);
    openpete_native_set_menu_glyphs(s_menu_buf, cx.n);
}

/* ---- OSD toast text + frames -------------------------------------------
 * Every live toast, every sim tick, into the renderer's fixed slot grid. The
 * per-tick rebuild is not an oversight: the shimmer is baked into vertex
 * COLOURS at emit time (emit_glyph dots the face normal against the light,
 * emit_lit_border bakes the travelling glint per perimeter vertex), so
 * geometry built once and reused would show a frozen glint on a live toast.
 * The settings menu already does exactly this while it is open.
 *
 * Geometry is emitted in TARGET-LOCAL overlay space — slot i's box starts at
 * y = i * slot_h — and the slot table tells the compositor which toast landed
 * where and how much of its slot it filled. Text and table are published in
 * one call, so the compositor can never resolve an id through a table that
 * describes different geometry. */
#define OSD_TXT_MAX     24576   /* must match OSD_GLYPH_MAX (native_render.c) */
#define OSD_CAP_H       0.030f  /* toast glyph height, overlay units          */
#define OSD_PAD_X       0.022f
#define OSD_PAD_Y       0.014f
#define OSD_BORDER_BT   0.0035f /* frame thickness — the menu's, so the two read as one set */
#define OSD_TINT        0x96962Au   /* the menu's mid yellow-gold */

static openpete_hud_vert s_osd_buf[OSD_TXT_MAX];

/* Trim `src` into `dst` until it fits `avail` at `cap_h`. Truncating is the
 * right failure for a fixed slot grid: a toast that overran its slot would
 * bleed its glyphs into the neighbouring slot's texels, which the compositor
 * then samples as part of a DIFFERENT toast. */
static void osd_fit_text(const char* src, float cap_h, float avail,
                         char* dst, int dstcap) {
    snprintf(dst, (size_t)dstcap, "%s", src ? src : "");
    for (int n = (int)strlen(dst); n > 0 && measure_string(dst, cap_h) > avail; n--)
        dst[n - 1] = 0;
}

void text_moby_native_osd(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;
    text_moby_native_cache(cpu);
    if (!openpete_osd_enabled() || !text_moby_native_ready()) {
        openpete_native_set_osd_glyphs(NULL, 0, NULL, 0);
        return;
    }

    openpete_osd_item_t items[OPENPETE_OSD_MAX];
    const int n = openpete_osd_tick_items(items, OPENPETE_OSD_MAX);
    if (n <= 0) {
        openpete_native_set_osd_glyphs(NULL, 0, NULL, 0);
        return;
    }

    float slot_w, slot_h;
    openpete_native_osd_slot_extent(&slot_w, &slot_h);
    if (slot_w <= 0.0f || slot_h <= 0.0f) return;
    const float text_avail = slot_w - 2.0f * OSD_PAD_X;
    if (text_avail <= 0.0f) return;

    emit_ctx_t cx;
    emit_ctx_init(&cx, s_osd_buf, OSD_TXT_MAX);
    install_specular_light(&cx, text_moby_native_shimmer_phase());

    openpete_osd_slot_t slots[OPENPETE_OSD_MAX];
    int ns = 0;

    for (int i = 0; i < n && i < OPENPETE_OSD_MAX; i++) {
        char line[OPENPETE_OSD_TEXT_MAX];
        osd_fit_text(items[i].text, OSD_CAP_H, text_avail, line, sizeof line);

        float box_w = 2.0f * OSD_PAD_X + measure_string(line, OSD_CAP_H);
        float box_h = 2.0f * OSD_PAD_Y + OSD_CAP_H;
        if (box_w > slot_w) box_w = slot_w;
        if (box_h > slot_h) box_h = slot_h;

        const float sy = (float)i * slot_h;   /* this slot's top in the grid */
        const int before = cx.n;
        emit_string(&cx, line, OSD_PAD_X, sy + OSD_PAD_Y, OSD_CAP_H, OSD_TINT, -1);
        cx.n += emit_lit_border(&cx, 0.0f, sy, box_w, sy + box_h, OSD_BORDER_BT,
                                s_osd_buf + cx.n, OSD_TXT_MAX - cx.n);
        if (cx.n == before) continue;   /* budget exhausted: publish no slot for it,
                                         * and the compositor draws nothing rather
                                         * than an empty frame */

        slots[ns].id    = items[i].id;
        slots[ns].slot  = i;
        slots[ns].box_w = box_w;
        slots[ns].box_h = box_h;
        ns++;
    }
    openpete_native_set_osd_glyphs(s_osd_buf, cx.n, slots, ns);
}
