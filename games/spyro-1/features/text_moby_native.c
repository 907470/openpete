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
#include "openpete_menu.h"          /* menu state/layout for the settings-menu text */
#include "text_moby_native.h"

#include <math.h>
#include <stdint.h>
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

/* Glyph lighting direction is settable so the FPS counter keeps its ORIGINAL
 * fixed front light while the settings menu uses Spyro's ROTATING specular light
 * (the metallic shimmer). emit_glyph dots the face normal with this unit vector,
 * exactly like the game's shaded_lit_color (moby_native.c) does with s_shL. The
 * default reproduces the original fixed model; text_moby_native_emit resets it,
 * the menu builder installs the rotating ellipse (specular_and_metal.c). */
static float s_light[3] = { -0.34f, 0.50f, 0.80f };  /* == the old fixed TXT_L */
/* 0 (FPS): add-only, faces range base..base+add. 1 (menu): SIGNED like the game's
 * shaded_lit_color — faces turned away darken BELOW base, giving metal contrast. */
static int   s_light_signed = 0;

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
        float nz0 = (float)(int8_t)(w1 >> 24);
        float nx0 = TXT_XSIGN * (float)(int8_t)(w1 >> 16); /* match mirrored geometry */
        float ny  = (float)(int8_t)(w1 >>  8);
        float nx = nx0 * cs + nz0 * sn;                    /* yaw the normal with the glyph */
        float nz = -nx0 * sn + nz0 * cs;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-3f) nl = 1.0f;
        float t = (nx * s_light[0] + ny * s_light[1] + nz * s_light[2]) / nl; /* N·L (unit L) */
        if (s_light_signed) { if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f; }
        else                { if (t <  0.0f) t =  0.0f; else if (t >  1.0f) t =  1.0f; }
        int add = (int)(TXT_LIGHT_ADD * t);          /* signed (menu) darkens shadowed faces */
        int R = baseR + add; if (R < 0) R = 0; else if (R > 255) R = 255;
        int G = baseG + add; if (G < 0) G = 0; else if (G > 255) G = 255;
        int B = baseB + add; if (B < 0) B = 0; else if (B > 255) B = 255;
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

/* Emit one string's glyphs into `out`. wob_phase >= 0 applies the selected-row
 * yaw wobble (a per-glyph phase ripple, like the PSX pause menu); < 0 = upright. */
static int emit_string(const char* str, float origin_x, float origin_y, float cap_h,
                       unsigned base_rgb, int wob_phase,
                       openpete_hud_vert* out, int out_cap) {
    s_out = out; s_out_cap = out_cap; s_out_n = 0;
    if (!s_cache_ready || !out || out_cap <= 0 || !str) return 0;
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
            float w = emit_glyph(327, cursor, center_y, scale * TXT_COLON_SCALE,
                                 TXT_COLON_RECENTER + TXT_COLON_SPREAD, yaw, baseR, baseG, baseB);
            if (w < 0.0f) { cursor += TXT_SPACE; continue; }
            emit_glyph(327, cursor, center_y, scale * TXT_COLON_SCALE,
                       TXT_COLON_RECENTER - TXT_COLON_SPREAD, yaw, baseR, baseG, baseB);
            cursor += w + TXT_GAP; gi++;
            continue;
        }
        int cls = glyph_class(c);
        if (cls < 0) { cursor += TXT_SPACE; continue; }
        float w = emit_glyph(cls, cursor, center_y, scale, 0.0f, yaw, baseR, baseG, baseB);
        if (w < 0.0f) { cursor += TXT_SPACE; continue; }  /* not cached -> skip */
        cursor += w + TXT_GAP; gi++;
    }
    return s_out_n;
}

int text_moby_native_emit(const char* str,
                          float origin_x, float origin_y, float cap_h,
                          unsigned base_rgb,
                          openpete_hud_vert* out, int out_cap) {
    /* FPS counter etc. keep the original fixed front light + add-only shading. */
    s_light[0] = TXT_L[0]; s_light[1] = TXT_L[1]; s_light[2] = TXT_L[2];
    s_light_signed = 0;
    return emit_string(str, origin_x, origin_y, cap_h, base_rgb, -1, out, out_cap);
}

/* Mod draw-service text rasterizer (core/mods/mod_draw.c; registered in
 * main.cpp). void* out keeps the core-facing fn type free of the
 * openpete_hud_vert dependency. The glyph cache is populated by the
 * per-tick fps-overlay driver above (unconditionally under the native
 * renderer, --show-fps or not), so at present time we only read the
 * snapshot: mod text appears as soon as the game has loaded the font —
 * absent before that (no bitmap fallback exists). */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap) {
    if (!text_moby_native_ready()) return 0;
    return text_moby_native_emit(str, x, y, cap_h, rgb,
                                 (openpete_hud_vert*)out_verts, cap);
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
 * so it receives the EXACT same lighting as the glyph rows: the current s_light
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

static uint32_t border_lit(float nx, float ny) {
    float t = nx * s_light[0] + ny * s_light[1];   /* N·L, N=(nx,ny,0) unit; signed like the glyphs */
    if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f;
    int add = (int)(TXT_LIGHT_ADD * t);
    int R = MENU_BORDER_R + add; if (R < 0) R = 0; else if (R > 255) R = 255;
    int G = MENU_BORDER_G + add; if (G < 0) G = 0; else if (G > 255) G = 255;
    int B = MENU_BORDER_B + add; if (B < 0) B = 0; else if (B > 255) B = 255;
    return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
}

/* Emit the lit frame into `out` (uses the current s_light — call after the menu
 * builder has set it). Returns verts written. Four straight edge strips: the
 * outer edge lies on the panel rect, the inner edge is a PERPENDICULAR inset by
 * bt (so each strip is a clean parallel-sided rectangle — no inward bulge). Each
 * strip is subdivided along its length; every column's colour comes from the
 * outward RADIAL normal at its outer point, so the glint travels smoothly along
 * the edge. Adjacent strips meet at a shared corner whose radial normal is
 * identical from either side, so the overlap is seamless. */
static int emit_menu_border(const openpete_menu_layout_t* L,
                            openpete_hud_vert* out, int cap) {
    const float x0 = L->panel_x0, y0 = L->panel_y0, x1 = L->panel_x1, y1 = L->panel_y1;
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float bt = MENU_BORDER_BT, z = TXT_DEPTH_MID;
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
            const uint32_t col = border_lit(dx / dl, dy / dl);
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
        int cls  = glyph_class(c);
        int slot = (cls >= 0 && cls < 512) ? s_class_to_idx[cls] : -1;
        if (slot < 0 || !s_glyph[slot].valid) { cursor += TXT_SPACE; continue; }
        cursor += (float)(s_glyph[slot].vxmax - s_glyph[slot].vxmin) * scale + TXT_GAP;
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

    /* Rotating specular light — reproduce SpecularUpdate's ellipse host-side (the
     * game runs SpecularUpdate only in menus/cutscenes, NOT gameplay where this
     * menu lives, and we must not write its RAM). Same N·L shading as the world's
     * shaded mobys (moby_native.c shaded_lit_color), so every glyph's highlight
     * sweeps as the light turns — the metal shimmer — on all rows, not just the
     * wobbling one. Clock advances +3/frame like SpecularUpdate(3). */
    {
        unsigned sp = ((unsigned)phase * 3u) & 0xFFu;
        float sth = (float)sp * (6.2831853f / 256.0f);
        float Lx = cosf(sth) * 0.968f;   /* COSINE_8 * 1983 >> 11 */
        float Ly = sinf(sth) * 1.549f;   /* SINE_8   * 6345 >> 12 */
        float Lz = -0.25f;               /* -1024 / 4096 (small fixed into-screen) */
        float Ll = sqrtf(Lx*Lx + Ly*Ly + Lz*Lz); if (Ll < 1e-3f) Ll = 1.0f;
        s_light[0] = Lx/Ll; s_light[1] = Ly/Ll; s_light[2] = Lz/Ll;
        s_light_signed = 1;   /* metal contrast: shadowed faces darken below base */
    }

    /* Pause-menu alignment: title centred; labels RIGHT-aligned to L.label_x;
     * values LEFT-aligned at L.value_x (the two columns meet at a central split). */
    const float pcx = 0.5f * (L.panel_x0 + L.panel_x1);   /* panel centre x */
    int n = 0;
    {
        const char* title = openpete_menu_title();
        const float tw = measure_string(title, L.title_cap_h);
        n += emit_string(title, pcx - 0.5f * tw,
                         L.title_y - L.title_cap_h * 0.5f, L.title_cap_h,
                         MENU_TITLE_TINT, -1, s_menu_buf + n, MENU_TXT_MAX - n);
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
        const float lw = measure_string(label, row_cap);
        n += emit_string(label, L.label_x - lw,
                         L.row_y[r] - row_cap * 0.5f, row_cap,
                         MENU_TXT_TINT, wob, s_menu_buf + n, MENU_TXT_MAX - n);
        /* Safety net for any future value wider than the reference: shrink
         * that one value further so it still ends before the panel border. */
        float vcap = row_cap;
        const float vw = measure_string(value, vcap);
        if (vw > vavail && vw > 0.0f) vcap *= vavail / vw;
        n += emit_string(value, L.value_x, L.row_y[r] - vcap * 0.5f, vcap,
                         MENU_TXT_TINT, wob, s_menu_buf + n, MENU_TXT_MAX - n);
    }
    /* Gold frame — same rotating light as the rows above (s_light still set). */
    n += emit_menu_border(&L, s_menu_buf + n, MENU_TXT_MAX - n);
    openpete_native_set_menu_glyphs(s_menu_buf, n);
}
