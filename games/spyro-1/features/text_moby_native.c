// games/spyro-1/features/text_moby_native.c
//
// Render text using Spyro 1's OWN 3D HUD "text mobys" — the gold extruded glyph
// models the game draws for the gem/life counters (digits, classes 260-269),
// "DEMO MODE" / dragon names / "THE ADVENTURE BEGINS" / the pause & inventory
// menus (letters A-Z, classes 426-451) and assorted punctuation. A general
// "string -> 3D Spyro glyph mobys" engine (text_moby_native_emit); the FPS
// counter (text_moby_native_fps_overlay) is its first consumer.
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
#include "text_moby_native.h"

#include <math.h>
#include <stdint.h>

#define G_MODELS 0x80076378u

/* --- glyph metrics (model units) + layout (window-height fractions) --- */
#define TXT_MODEL_H   152.0f   /* glyph y-extent (-76..76) the cap-height maps to */
#define TXT_GAP       0.008f   /* gap between glyphs (window-height frac) */
#define TXT_SPACE     0.022f   /* advance for a space / missing glyph (frac) */

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
    s_cache_ready = 1;                                    /* snapshot taken; never re-read RAM */
}

int text_moby_native_ready(void) { return s_cache_ready; }

/* ---- emit (works purely from the cache) --------------------------------- */

/* per-glyph vertices in overlay space (filled by emit_glyph). */
static float s_gx[TXT_MAX_GLYPH_V], s_gy[TXT_MAX_GLYPH_V], s_gz[TXT_MAX_GLYPH_V];

/* current emit() output cursor (single-threaded). */
static openpete_hud_vert* s_out;
static int s_out_cap;
static int s_out_n;

static void push_tri(int a, int b, int c, uint32_t rgba) {
    if (!s_out || s_out_n + 3 > s_out_cap) return;
    s_out[s_out_n++] = (openpete_hud_vert){ s_gx[a], s_gy[a], s_gz[a], rgba };
    s_out[s_out_n++] = (openpete_hud_vert){ s_gx[b], s_gy[b], s_gz[b], rgba };
    s_out[s_out_n++] = (openpete_hud_vert){ s_gx[c], s_gy[c], s_gz[c], rgba };
}

/* Emit one cached glyph with its left edge at cursor_x, centred on center_y.
 * dy_model shifts the model vertically (model units) — used for the colon dots.
 * baseR/G/B is the (0..255) glyph colour. Returns the glyph width (overlay
 * frac), or -1 if the glyph isn't in the cache. */
static float emit_glyph(int cls, float cursor_x, float center_y,
                        float scale, float dy_model,
                        int baseR, int baseG, int baseB) {
    int slot = (cls >= 0 && cls < 512) ? s_class_to_idx[cls] : -1;
    if (slot < 0 || !s_glyph[slot].valid) return -1.0f;
    const glyph_cache_t* g = &s_glyph[slot];
    int vcount = g->vcount, fcount = g->fcount, vxmin = g->vxmin, vxmax = g->vxmax;
    if (vcount > TXT_MAX_GLYPH_V) vcount = TXT_MAX_GLYPH_V;
    for (int i = 0; i < vcount; i++) {
        float my = (float)g->vy[i] + dy_model;
        float mz = (float)g->vz[i];
        float gx_local = (TXT_XSIGN < 0.0f) ? (float)(vxmax - g->vx[i])
                                            : (float)(g->vx[i] - vxmin);
        s_gx[i] = cursor_x + gx_local * scale;
        s_gy[i] = center_y + TXT_YSIGN * my * scale;
        s_gz[i] = TXT_DEPTH_MID - mz * TXT_DEPTH_K;          /* front nearer */
    }
    for (int f = 0; f < fcount; f++) {
        uint32_t w0 = g->fw0[f], w1 = g->fw1[f];
        int i0 = (int)((w0 >> 21) & 0x1FCu) >> 2;
        int i1 = (int)((w0 >> 14) & 0x1FCu) >> 2;
        int i2 = (int)((w0 >>  7) & 0x1FCu) >> 2;
        int i3 = (int)( w0        & 0x1FCu) >> 2;
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        float nz = (float)(int8_t)(w1 >> 24);
        float nx = TXT_XSIGN * (float)(int8_t)(w1 >> 16);   /* match mirrored geometry */
        float ny = (float)(int8_t)(w1 >>  8);
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-3f) nl = 1.0f;
        float t = (nx * TXT_L[0] + ny * TXT_L[1] + nz * TXT_L[2]) / nl;   /* N·L (L unit) */
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        int add = (int)(TXT_LIGHT_ADD * t);                              /* white light, added */
        int R = baseR + add, G = baseG + add, B = baseB + add;
        if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
        uint32_t rgba = (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
        /* Draw front AND back faces; the depth test keeps the (nearer) front and
         * the edge-on side walls have ~zero area — so no winding cull is needed. */
        if (i2 == i3) {
            push_tri(i0, i1, i2, rgba);                       /* tri */
        } else if (i3 < vcount) {
            push_tri(i0, i1, i2, rgba);                       /* quad = 2 tris */
            push_tri(i1, i3, i2, rgba);
        }
    }
    return (float)(vxmax - vxmin) * scale;
}

int text_moby_native_emit(const char* str,
                          float origin_x, float origin_y, float cap_h,
                          unsigned base_rgb,
                          openpete_hud_vert* out, int out_cap) {
    s_out = out; s_out_cap = out_cap; s_out_n = 0;
    if (!s_cache_ready || !out || out_cap <= 0 || !str) return 0;
    const int baseR = (int)((base_rgb >> 16) & 0xFFu);
    const int baseG = (int)((base_rgb >>  8) & 0xFFu);
    const int baseB = (int)( base_rgb        & 0xFFu);
    const float scale    = cap_h / TXT_MODEL_H;
    const float center_y = origin_y + cap_h * 0.5f;
    float cursor = origin_x;

    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        if (c == ':') {                                  /* faked colon: two dots */
            float w = emit_glyph(327, cursor, center_y, scale * TXT_COLON_SCALE,
                                 TXT_COLON_RECENTER + TXT_COLON_SPREAD, baseR, baseG, baseB);
            if (w < 0.0f) { cursor += TXT_SPACE; continue; }
            emit_glyph(327, cursor, center_y, scale * TXT_COLON_SCALE,
                       TXT_COLON_RECENTER - TXT_COLON_SPREAD, baseR, baseG, baseB);
            cursor += w + TXT_GAP;
            continue;
        }
        int cls = glyph_class(c);
        if (cls < 0) { cursor += TXT_SPACE; continue; }
        float w = emit_glyph(cls, cursor, center_y, scale, 0.0f, baseR, baseG, baseB);
        if (w < 0.0f) { cursor += TXT_SPACE; continue; }  /* not cached -> skip */
        cursor += w + TXT_GAP;
    }
    return s_out_n;
}

/* ---- FPS overlay (the first consumer) ----------------------------------- */
#define FPS_OVL_MAX   4096
#define FPS_ORIGIN_X  0.014f
#define FPS_ORIGIN_Y  0.016f
#define FPS_CAP_H     0.020f   /* small corner counter */
#define FPS_TINT      0x606000u   /* the game's HUD text back-colour, olive (96,96,0) */

static openpete_hud_vert s_fps_buf[FPS_OVL_MAX];

void text_moby_native_fps_overlay(CPUState* cpu) {
    if (!openpete_native_render_enabled()) return;   /* inert headless: no cache, no draw */
    text_moby_native_cache(cpu);                     /* snapshot the font as soon as available */
    if (!openpete_native_fps_overlay_enabled() || !text_moby_native_ready()) {
        openpete_native_set_overlay_glyphs(NULL, 0); /* nothing until cached (no fallback) */
        return;
    }
    int val = openpete_native_fps_value();
    if (val < 0) val = 0; else if (val > 9999) val = 9999;

    char str[16]; int n = 0;
    str[n++] = 'F'; str[n++] = 'P'; str[n++] = 'S'; str[n++] = ':'; str[n++] = ' ';
    if (val == 0) {
        str[n++] = '0';
    } else {
        char tmp[8]; int t = 0;
        for (int v = val; v > 0; v /= 10) tmp[t++] = (char)('0' + v % 10);
        while (t > 0) str[n++] = tmp[--t];
    }
    str[n] = '\0';

    int nv = text_moby_native_emit(str, FPS_ORIGIN_X, FPS_ORIGIN_Y, FPS_CAP_H,
                                   FPS_TINT, s_fps_buf, FPS_OVL_MAX);
    openpete_native_set_overlay_glyphs(nv > 0 ? s_fps_buf : NULL, nv);
}
