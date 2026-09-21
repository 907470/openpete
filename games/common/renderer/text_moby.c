// games/common/renderer/text_moby.c
//
// Render text with the game's own extruded 3D glyph models: the gold letters
// Spyro 1 draws its gem and life counters, "DEMO MODE", dragon names and
// pause menu with, and the ones Spyro 3 spells its home-world portal names
// with. A general "string -> 3D glyph triangles" engine
// (text_moby_native_emit) with three consumers: the performance readout, the
// settings menu and the OSD toasts.
//
// Where a game keeps its glyph models, how a character maps to one and how a
// model is packed all come from game_layout.font (core/include/game_layout.h);
// both packings are decoded once, at cache time, into one host form (int16
// vertices with +X right, +Y down and +Z toward the reader, four vertex
// indices and an int16 normal per face), and everything after the cache works
// on that form alone.
//
// The glyph models are not permanently resident. Spyro 1 loads them with the
// HUD font asset (title / attract) and evicts them later (the level-intro
// cutscene); Spyro 3 has them only while a home world is loaded. So the
// decoded geometry is snapshotted the first time it is available
// (text_moby_native_cache) and every draw works from the snapshot. Where the
// font is a per-level asset (GAME_FONT_ONE_CLASS) the snapshot is also kept in
// a cache file, so a later session has the font from boot in any level. Until
// a snapshot exists, ready() is false and the consumers publish no glyphs.
//
// Glyphs are projected orthographically at a fixed window-relative spot (not
// through the camera: an overlay sits at a window corner and looks identical
// every present, wherever the game camera points), shaded per face by a
// fixed or rotating light, and emitted in resolution-independent overlay
// coordinates. The native renderer draws them depth-tested on the live present.
//
// Pure presentation: reads model geometry from RAM and calls native-render
// setters. Writes no gameplay-readable state.

#include "psx_runtime_internal.h"            /* CPUState (read_word / read_byte) */
#include "openpete_native_render.h" /* openpete_hud_vert + the overlay API */
#include "openpete_menu.h"          /* menu state/layout for the settings-menu text */
#include "openpete_perf_stats.h"    /* the overlay's levels + line text */
#include "openpete_overlay_anchor.h" /* per-line alignment inside an anchored block */
#include "openpete_osd.h"          /* toast ring — the OSD builder below */
#include "openpete_build_id.h"     /* the version line below */
#include "openpete_fiber.h"        /* speculative / replayed ticks skip the shimmer */
#include "openpete_paths.h"        /* the per-user state directory */
#include "openpete_fnv1a64.h"
#include "openpete_log.h"
#include "game_extras.h"           /* game_get_id, game_get_exe_filename */
#include "game_layout.h"
#include "renderer/text_moby.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT (game_layout.font)

/* --- layout (window-height fractions) --- */
#define TXT_GAP       0.004f   /* gap between glyphs (window-height frac) */
#define TXT_SPACE     0.011f   /* advance for a space / missing glyph (frac) */

/* depth band: map model Z (~±24) to a thin slice around the middle so a glyph's
 * own faces z-sort (front, +Z, nearer = smaller depth) without being huge. */
#define TXT_DEPTH_MID 0.5f
#define TXT_DEPTH_K   0.0016f

/* Screen mapping signs of the host form: +X runs right and +Y down, so both
 * pass through as +1. */
#define TXT_YSIGN     (1.0f)
#define TXT_XSIGN     (1.0f)

/* Lighting follows how both games shade these models: a dark base colour plus
 * a directional term added on top, not a multiply. A face's colour is
 * base + light_add * t (+ the highlight that opens above hi_threshold), t the
 * unit N·L, with the constants in game_layout.font. The performance readout
 * uses the descriptor's fixed light, so the readable glyph fronts are bright
 * and only the grazing edges fall back toward the base. */

/* Selected-menu-row wobble: peak yaw about the glyph's vertical axis. The PSX
 * pause menu uses COSINE_8((tick*8+i*12)&0xFF)*3>>9 → peak 24/256 turn ≈ ±33.75°
 * ≈ ±0.589 rad. Phase advances 8/frame, +12 per letter (a ripple along the row). */
#define TXT_WOBBLE_AMP 0.589f

/* Glyph lighting direction is per-emission so the FPS counter keeps its
 * fixed front light while the settings menu and the OSD use a rotating
 * specular light (the metallic shimmer). emit_glyph dots the face normal with
 * this unit vector. text_moby_native_emit installs the fixed model; the menu
 * and OSD builders install the rotating ellipse.
 *
 * light_signed: 0 (FPS) = add-only, faces range base..base+add. 1 (menu/OSD) =
 * signed — faces turned away darken below base, giving metal contrast. */

/* ---- glyph cache (snapshot of the model geometry, immune to later unload) ---
 * One entry per slot; the largest glyph seen is ~62 verts / 67 faces, and a
 * face's vertex index is 7 bits in both packings. */
#define TXT_MAX_GLYPH_V 128
#define TXT_MAX_GLYPH_F 128
typedef struct {
    int      valid;
    int      vcount, fcount, vxmin, vxmax;
    int16_t  vx[TXT_MAX_GLYPH_V], vy[TXT_MAX_GLYPH_V], vz[TXT_MAX_GLYPH_V];
    uint8_t  fi[TXT_MAX_GLYPH_F][4];   /* vertex indices; a triangle repeats the third */
    int16_t  fn[TXT_MAX_GLYPH_F][3];   /* face normal x, y, z (host form) */
} glyph_cache_t;

/* Slots: '0'-'9', 'A'-'Z', the descriptor's punctuation list in order, then
 * the synthesized period and the synthesized hyphen. */
#define SLOT_DIGIT   0
#define SLOT_LETTER  10
#define SLOT_PUNCT   36
#define N_PUNCT      ((int)(sizeof FONT.punct / sizeof FONT.punct[0]))
#define SLOT_SYNTH_PERIOD (SLOT_PUNCT + 8)
#define SLOT_SYNTH_HYPHEN (SLOT_SYNTH_PERIOD + 1)
#define NGLYPH       (SLOT_SYNTH_HYPHEN + 1)
_Static_assert(sizeof FONT.punct / sizeof FONT.punct[0] == 8, "slot table sized for 8 punctuation pairs");

static glyph_cache_t s_glyph[NGLYPH];
static int s_cache_ready = 0;
/* Common advance for '0'-'9' (model units) — see cache_finish. */
static int s_digit_adv = 0;
/* Where the snapshot came from: 0 none, 1 guest RAM, 2 the cache file. */
static int s_cache_source = 0;

static int is_digit_slot(int slot) { return slot >= SLOT_DIGIT && slot < SLOT_LETTER; }

/* char -> cache slot, or -1 for "advance only" (space / unsupported). */
static int glyph_slot(char c) {
    if (c >= '0' && c <= '9') return SLOT_DIGIT + (c - '0');
    if (c >= 'A' && c <= 'Z') return SLOT_LETTER + (c - 'A');
    if (c >= 'a' && c <= 'z') return SLOT_LETTER + (c - 'a');
    for (int p = 0; p < N_PUNCT && FONT.punct[p].ch; p++)
        if (FONT.punct[p].ch == c) return SLOT_PUNCT + p;
    if (c == '.' && FONT.synth_period) return SLOT_SYNTH_PERIOD;
    /* Unconditional, unlike the period: a font that carries a hyphen returns
     * it from the punctuation loop above and never reaches here, and one that
     * does not gets the synthesized bar. */
    if (c == '-') return SLOT_SYNTH_HYPHEN;
    return -1;
}

/* slot -> the descriptor's glyph index (a class or a block index), or -1 for
 * a slot the game's font does not fill. */
static int slot_glyph_index(int slot) {
    if (slot < SLOT_LETTER) return FONT.digit_base + (slot - SLOT_DIGIT);
    if (slot < SLOT_PUNCT)  return FONT.letter_base + (slot - SLOT_LETTER);
    if (slot < SLOT_SYNTH_PERIOD) {
        for (int p = 0; p <= slot - SLOT_PUNCT; p++)
            if (!FONT.punct[p].ch) return -1;
        return FONT.punct[slot - SLOT_PUNCT].index;
    }
    return -1;
}

static float glyph_natural_w(int slot) {
    if (slot < 0 || slot >= NGLYPH || !s_glyph[slot].valid) return -1.0f;
    return (float)(s_glyph[slot].vxmax - s_glyph[slot].vxmin);
}

/* How much room a character takes, and how far into that room its glyph sits
 * (overlay units). Everything but a digit is proportional — advance is the
 * glyph width, inset 0. A digit takes the shared tabular advance and is
 * centred in it. Both emit_string and measure_string go through here, so the
 * two can never disagree about a string's width. adv < 0 = not cached. */
static void glyph_metrics(int slot, float scale, float* adv, float* inset) {
    const float w = glyph_natural_w(slot);
    if (w < 0.0f) { *adv = -1.0f; *inset = 0.0f; return; }
    if (is_digit_slot(slot) && s_digit_adv > 0) {
        *adv   = (float)s_digit_adv * scale;
        *inset = 0.5f * (*adv - w * scale);   /* centred in its column */
    } else {
        *adv   = w * scale;
        *inset = 0.0f;
    }
}

static void glyph_set_xrange(glyph_cache_t* g) {
    int vxmin = 1 << 30, vxmax = -(1 << 30);
    for (int i = 0; i < g->vcount; i++) {
        if (g->vx[i] < vxmin) vxmin = g->vx[i];
        if (g->vx[i] > vxmax) vxmax = g->vx[i];
    }
    g->vxmin = vxmin; g->vxmax = vxmax;
}

/* GAME_FONT_CLASS_PER_GLYPH: decode the model of class `cls` into `g`. Leaves
 * it invalid if the model isn't a loaded simple model. */
static void cache_one_class(CPUState* cpu, glyph_cache_t* g, int cls) {
    g->valid = 0;
    uint32_t model = cpu->read_word(FONT.model_table + (uint32_t)cls * 4u);
    if (model == 0u) return;
    if (model < 0x80000000u) model |= 0x80000000u;
    if ((int32_t)cpu->read_word(model) >= 0) return;     /* not a simple model / data not in */
    int vcount = cpu->read_byte(model + 0x0u) & 0xFF;
    int fcount = cpu->read_byte(model + 0x1u) & 0xFF;
    if (vcount <= 0 || vcount > TXT_MAX_GLYPH_V || fcount > TXT_MAX_GLYPH_F) return;
    uint32_t vbase = cpu->read_word(model + 0x4u); if (vbase < 0x80000000u) vbase |= 0x80000000u;
    uint32_t faces = cpu->read_word(model + 0xCu); if (faces < 0x80000000u) faces |= 0x80000000u;
    for (int i = 0; i < vcount; i++) {
        uint32_t p = vbase + (uint32_t)i * 3u;
        uint32_t w = cpu->read_byte(p) | (cpu->read_byte(p + 1) << 8) | (cpu->read_byte(p + 2) << 16);
        int VZ = ((int32_t)(w << 24)) >> 23;
        int VX = ((int32_t)(w << 16)) >> 23;
        int VY = ((int32_t)(w <<  8)) >> 23;
        g->vx[i] = (int16_t)VX; g->vy[i] = (int16_t)VY; g->vz[i] = (int16_t)VZ;
    }
    for (int f = 0; f < fcount; f++) {
        uint32_t fp = faces + (uint32_t)f * 8u;
        uint32_t w0 = cpu->read_word(fp), w1 = cpu->read_word(fp + 4u);
        g->fi[f][0] = (uint8_t)(((w0 >> 21) & 0x1FCu) >> 2);
        g->fi[f][1] = (uint8_t)(((w0 >> 14) & 0x1FCu) >> 2);
        g->fi[f][2] = (uint8_t)(((w0 >>  7) & 0x1FCu) >> 2);
        g->fi[f][3] = (uint8_t)(( w0        & 0x1FCu) >> 2);
        g->fn[f][0] = (int16_t)(int8_t)(w1 >> 16);
        g->fn[f][1] = (int16_t)(int8_t)(w1 >>  8);
        g->fn[f][2] = (int16_t)(int8_t)(w1 >> 24);
    }
    g->vcount = vcount; g->fcount = fcount;
    glyph_set_xrange(g);
    g->valid = 1;
}

/* GAME_FONT_ONE_CLASS: the font model's guest address, or 0 while the level
 * loaded has none. The table entry of the block-per-glyph kind is a KUSEG
 * address; word 0 is the block count. */
static uint32_t one_class_model(CPUState* cpu, int need_blocks) {
    uint32_t model = cpu->read_word(FONT.model_table + (uint32_t)FONT.font_class * 4u);
    if (model == 0u || model >= 0x80000000u) return 0u;
    model |= 0x80000000u;
    if ((int32_t)cpu->read_word(model) < need_blocks) return 0u;
    return model;
}

/* GAME_FONT_ONE_CLASS: decode block `index` of the font model into `g`. The
 * model's -Z faces the reader, so Z and the normal's Z are negated into the
 * host form. A face that is textured or not flat-lit is not a glyph face. */
static void cache_one_block(CPUState* cpu, glyph_cache_t* g, uint32_t model, int index) {
    g->valid = 0;
    uint32_t H = cpu->read_word(model + FONT.block_table_off + (uint32_t)index * 4u);
    if (H < 0x80000000u) return;
    int vcount = (int)((cpu->read_word(H) >> 8) & 0xFFu);
    if (vcount < 3 || vcount > TXT_MAX_GLYPH_V) return;
    for (int i = 0; i < vcount; i++) {
        uint32_t w = cpu->read_word(H + 0x10u + (uint32_t)i * 4u);
        g->vx[i] = (int16_t)((int32_t)(w << 10) >> 21);
        g->vy[i] = (int16_t)((int32_t)(w << 20) >> 20);
        g->vz[i] = (int16_t)-((int32_t)w >> 21);
    }
    uint32_t faces = H + (cpu->read_word(H + 0x08u) & 0xFFFFu);
    uint32_t fp = faces + 4u, end = fp + cpu->read_word(faces);
    int fcount = 0;
    while (fp + 8u <= end && fcount < TXT_MAX_GLYPH_F) {
        uint32_t w0 = cpu->read_word(fp), w1 = cpu->read_word(fp + 4u);
        if ((int32_t)w1 < 0) { fp += 0x14u; continue; }   /* textured: 20-byte face */
        fp += 8u;
        if (!(w1 & 2u)) continue;                          /* not flat-lit */
        g->fi[fcount][0] = (uint8_t)((w0 >> 21) & 0x7Fu);
        g->fi[fcount][1] = (uint8_t)((w0 >> 14) & 0x7Fu);
        g->fi[fcount][2] = (uint8_t)((w0 >>  7) & 0x7Fu);
        g->fi[fcount][3] = (uint8_t)( w0        & 0x7Fu);
        g->fn[fcount][0] = (int16_t)(int8_t)(w1 >> 15);
        g->fn[fcount][1] = (int16_t)(int8_t)(w1 >>  7);
        g->fn[fcount][2] = (int16_t)-(int)(int8_t)(w1 >> 23);
        fcount++;
    }
    if (fcount == 0) return;
    g->vcount = vcount; g->fcount = fcount;
    glyph_set_xrange(g);
    g->valid = 1;
}

/* The period a font lacks: a square box standing on the digits' baseline, as
 * wide as the glyph of `synth_depth_char` and as deep. */
static void synth_period(void) {
    glyph_cache_t* g = &s_glyph[SLOT_SYNTH_PERIOD];
    g->valid = 0;
    const int ds = glyph_slot(FONT.synth_depth_char);
    const glyph_cache_t* d = (ds >= 0) ? &s_glyph[ds] : NULL;
    const glyph_cache_t* zero = &s_glyph[SLOT_DIGIT];
    if (!d || !d->valid || !zero->valid) return;
    int zmin = 1 << 30, zmax = -(1 << 30), base = -(1 << 30);
    for (int i = 0; i < d->vcount; i++) {
        if (d->vz[i] < zmin) zmin = d->vz[i];
        if (d->vz[i] > zmax) zmax = d->vz[i];
    }
    for (int i = 0; i < zero->vcount; i++)
        if (zero->vy[i] > base) base = zero->vy[i];
    const int side = d->vxmax - d->vxmin;
    if (side <= 0 || zmax <= zmin) return;
    /* vertex i: bit 0 = right, bit 1 = bottom, bit 2 = back */
    for (int i = 0; i < 8; i++) {
        g->vx[i] = (int16_t)((i & 1) ? side : 0);
        g->vy[i] = (int16_t)((i & 2) ? base : base - side);
        g->vz[i] = (int16_t)((i & 4) ? zmin : zmax);
    }
    static const uint8_t quad[6][4] = {
        {0,1,2,3}, {5,4,7,6},      /* front, back   */
        {4,0,6,2}, {1,5,3,7},      /* left, right   */
        {4,5,0,1}, {2,3,6,7},      /* top, bottom   */
    };
    static const int16_t normal[6][3] = {
        {0,0,127}, {0,0,-127}, {-127,0,0}, {127,0,0}, {0,-127,0}, {0,127,0},
    };
    memcpy(g->fi, quad, sizeof quad);
    memcpy(g->fn, normal, sizeof normal);
    g->vcount = 8; g->fcount = 6;
    glyph_set_xrange(g);
    g->valid = 1;
}

/* The hyphen a font lacks: a thin horizontal bar, half the digit cap height
 * wide and an eighth of it thick, centred at half the cap height above the
 * baseline, extruded over the digit's own depth range. Every dimension is a
 * fraction of the digit's cap height, so it scales with the font instead of
 * carrying absolute numbers.
 *
 * The bar is a box, built like synth_period's, so it takes the same lighting
 * and the same extrusion as a real glyph and does not read as a flat decal
 * among them. */
static void synth_hyphen(void) {
    glyph_cache_t* g = &s_glyph[SLOT_SYNTH_HYPHEN];
    g->valid = 0;
    /* Everything is measured off the digit '0': it is the one glyph the
     * snapshot is not taken without, so the bar exists wherever text does.
     * (synth_period's depth-donor descriptor field is optional and unset on
     * fonts that have their own period, which would leave the bar unbuilt.) */
    const glyph_cache_t* zero = &s_glyph[SLOT_DIGIT];
    if (!zero->valid || zero->vcount <= 0) return;
    int zmin = 1 << 30, zmax = -(1 << 30);
    /* Model y grows DOWNWARD (emit_glyph flips it with TXT_YSIGN), so the
     * baseline is the digit's largest y and the cap line its smallest. */
    int base = -(1 << 30), top = 1 << 30;
    for (int i = 0; i < zero->vcount; i++) {
        if (zero->vz[i] < zmin) zmin = zero->vz[i];
        if (zero->vz[i] > zmax) zmax = zero->vz[i];
        if (zero->vy[i] > base) base = zero->vy[i];
        if (zero->vy[i] < top)  top  = zero->vy[i];
    }
    const int cap = base - top;
    if (cap <= 0 || zmax <= zmin) return;
    int wide  = cap / 2;          /* bar length  */
    int thick = cap / 8;          /* bar height  */
    if (wide  < 1) wide  = 1;
    if (thick < 1) thick = 1;
    const int mid = base - cap / 2;
    const int y0 = mid - thick / 2, y1 = y0 + thick;
    /* vertex i: bit 0 = right, bit 1 = bottom, bit 2 = back */
    for (int i = 0; i < 8; i++) {
        g->vx[i] = (int16_t)((i & 1) ? wide : 0);
        g->vy[i] = (int16_t)((i & 2) ? y1 : y0);
        g->vz[i] = (int16_t)((i & 4) ? zmin : zmax);
    }
    static const uint8_t quad[6][4] = {
        {0,1,2,3}, {5,4,7,6},      /* front, back   */
        {4,0,6,2}, {1,5,3,7},      /* left, right   */
        {4,5,0,1}, {2,3,6,7},      /* top, bottom   */
    };
    static const int16_t normal[6][3] = {
        {0,0,127}, {0,0,-127}, {-127,0,0}, {127,0,0}, {0,-127,0}, {0,127,0},
    };
    memcpy(g->fi, quad, sizeof quad);
    memcpy(g->fn, normal, sizeof normal);
    g->vcount = 8; g->fcount = 6;
    glyph_set_xrange(g);
    g->valid = 1;
}

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
static void cache_finish(int source) {
    s_digit_adv = 0;
    for (int d = 0; d < 10; d++) {
        const glyph_cache_t* g = &s_glyph[SLOT_DIGIT + d];
        if (g->valid && (g->vxmax - g->vxmin) > s_digit_adv)
            s_digit_adv = g->vxmax - g->vxmin;
    }
    s_cache_source = source;
    s_cache_ready = 1;                                    /* snapshot taken; never re-read RAM */
}

/* ---- the cache file ------------------------------------------------------
 * GAME_FONT_ONE_CLASS only: the font is resident in some levels and absent in
 * the rest, so the first snapshot is written to a file and later sessions
 * load it at boot.
 *
 * OPENPETE_FONT_CACHE names the file; "0" disables it. Unset, a windowed run
 * uses <state dir>/cache/font-<game id>.bin and a headless run uses no file,
 * so an unattended capture never depends on what an earlier session left on
 * the machine.
 *
 * Layout, little-endian: "OPFC", u32 version, the disc's boot-file name in 16
 * bytes, u32 form, u32 slot count, u32 payload bytes, u64 FNV-1a of the
 * payload; then per slot u8 valid, u8 vcount, u8 fcount, vcount * 3 int16
 * vertices, fcount * (4 u8 indices + 3 int16 normal). A file that fails any
 * field is ignored and overwritten by the next snapshot. */
#define FONT_FILE_VERSION 1u
#define FONT_FILE_HDR     44
#define FONT_FILE_MAX     (FONT_FILE_HDR + NGLYPH * (3 + TXT_MAX_GLYPH_V * 6 + TXT_MAX_GLYPH_F * 10))

static const char* font_file_path(void) {
    static char s_path[1024];
    static int  s_resolved = 0;
    if (s_resolved) return s_path[0] ? s_path : NULL;
    s_resolved = 1;
    s_path[0] = 0;
    if (FONT.form != GAME_FONT_ONE_CLASS) return NULL;
    const char* e = getenv("OPENPETE_FONT_CACHE");
    if (e && *e) {
        if (strcmp(e, "0") != 0) snprintf(s_path, sizeof s_path, "%s", e);
    } else if (openpete_skip_ffwd_windowed) {             /* 1 in a windowed run */
        char dir[900], leaf[64];
        if (openpete_paths_state_dir("cache", dir, sizeof dir, 1) == 0) {
            snprintf(leaf, sizeof leaf, "font-%s.bin", game_get_id());
            openpete_paths_join(s_path, sizeof s_path, dir, leaf);
        }
    }
    return s_path[0] ? s_path : NULL;
}

static void put16(uint8_t** p, int v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static void put32(uint8_t** p, uint32_t v) { for (int i = 0; i < 4; i++) (*p)[i] = (uint8_t)(v >> (8 * i)); *p += 4; }
static int  get16(const uint8_t** p) { int v = (int16_t)((*p)[0] | ((*p)[1] << 8)); *p += 2; return v; }
static uint32_t get32(const uint8_t** p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(*p)[i] << (8 * i); *p += 4; return v; }

static void font_file_disc_id(char out[16]) {
    memset(out, 0, 16);
    snprintf(out, 16, "%s", game_get_exe_filename());
}

static void font_file_save(void) {
    const char* path = font_file_path();
    if (!path) return;
    static uint8_t buf[FONT_FILE_MAX];
    uint8_t* p = buf + FONT_FILE_HDR;
    for (int s = 0; s < NGLYPH; s++) {
        const glyph_cache_t* g = &s_glyph[s];
        *p++ = (uint8_t)(g->valid ? 1 : 0);
        *p++ = (uint8_t)(g->valid ? g->vcount : 0);
        *p++ = (uint8_t)(g->valid ? g->fcount : 0);
        if (!g->valid) continue;
        for (int i = 0; i < g->vcount; i++) { put16(&p, g->vx[i]); put16(&p, g->vy[i]); put16(&p, g->vz[i]); }
        for (int f = 0; f < g->fcount; f++) {
            memcpy(p, g->fi[f], 4); p += 4;
            put16(&p, g->fn[f][0]); put16(&p, g->fn[f][1]); put16(&p, g->fn[f][2]);
        }
    }
    const uint32_t payload = (uint32_t)(p - (buf + FONT_FILE_HDR));
    const uint64_t h = openpete_fnv1a64(buf + FONT_FILE_HDR, payload);
    uint8_t* q = buf;
    memcpy(q, "OPFC", 4); q += 4;
    put32(&q, FONT_FILE_VERSION);
    font_file_disc_id((char*)q); q += 16;
    put32(&q, (uint32_t)FONT.form);
    put32(&q, (uint32_t)NGLYPH);
    put32(&q, payload);
    put32(&q, (uint32_t)h); put32(&q, (uint32_t)(h >> 32));
    FILE* fp = fopen(path, "wb");
    if (!fp) { OPL_WARN_F("font", "cannot write the glyph cache %s", path); return; }
    const size_t total = FONT_FILE_HDR + payload;
    const int ok = fwrite(buf, 1, total, fp) == total;
    if (fclose(fp) != 0 || !ok) { remove(path); OPL_WARN_F("font", "short write, removed %s", path); return; }
    OPL_INFO_F("font", "glyph cache written: %s (%u bytes)", path, (unsigned)total);
}

/* Fill s_glyph from the cache file. Returns 1 on success; on any mismatch
 * s_glyph is left all-invalid and 0 is returned. */
static int font_file_load(void) {
    const char* path = font_file_path();
    if (!path) return 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    static uint8_t buf[FONT_FILE_MAX + 1];
    const size_t got = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    const char* why = NULL;
    char disc[16];
    font_file_disc_id(disc);
    const uint8_t* q = buf + 4;
    if (got < FONT_FILE_HDR || got > FONT_FILE_MAX)  why = "size";
    else if (memcmp(buf, "OPFC", 4) != 0)           why = "magic";
    else if (get32(&q) != FONT_FILE_VERSION)        why = "version";
    else if (memcmp(q, disc, 16) != 0)              why = "disc";
    if (!why) {
        q += 16;
        const uint32_t form = get32(&q), slots = get32(&q), payload = get32(&q);
        uint64_t h = get32(&q); h |= (uint64_t)get32(&q) << 32;
        if (form != (uint32_t)FONT.form || slots != (uint32_t)NGLYPH) why = "descriptor";
        else if ((size_t)payload + FONT_FILE_HDR != got)            why = "length";
        else if (openpete_fnv1a64(buf + FONT_FILE_HDR, payload) != h) why = "checksum";
    }
    if (!why) {
        const uint8_t* p = buf + FONT_FILE_HDR;
        const uint8_t* end = buf + got;
        for (int s = 0; s < NGLYPH && !why; s++) {
            glyph_cache_t* g = &s_glyph[s];
            g->valid = 0;
            if (p + 3 > end) { why = "truncated"; break; }
            const int valid = p[0], vcount = p[1], fcount = p[2];
            p += 3;
            if (!valid) continue;
            if (vcount < 3 || vcount > TXT_MAX_GLYPH_V || fcount < 1 || fcount > TXT_MAX_GLYPH_F ||
                p + (size_t)vcount * 6 + (size_t)fcount * 10 > end) { why = "glyph bounds"; break; }
            for (int i = 0; i < vcount; i++) {
                g->vx[i] = (int16_t)get16(&p); g->vy[i] = (int16_t)get16(&p); g->vz[i] = (int16_t)get16(&p);
            }
            for (int f = 0; f < fcount; f++) {
                memcpy(g->fi[f], p, 4); p += 4;
                g->fn[f][0] = (int16_t)get16(&p); g->fn[f][1] = (int16_t)get16(&p); g->fn[f][2] = (int16_t)get16(&p);
            }
            g->vcount = vcount; g->fcount = fcount;
            glyph_set_xrange(g);
            g->valid = 1;
        }
        if (!why && !s_glyph[SLOT_DIGIT].valid) why = "no digit 0";
    }
    if (why) {
        for (int s = 0; s < NGLYPH; s++) s_glyph[s].valid = 0;
        OPL_WARN_F("font", "glyph cache %s rejected (%s)", path, why);
        return 0;
    }
    OPL_INFO_F("font", "glyph cache loaded: %s", path);
    return 1;
}

void text_moby_native_cache(CPUState* cpu) {
    if (s_cache_ready) return;
    if (FONT.form == GAME_FONT_CLASS_PER_GLYPH) {
        /* Gate on digit '0' being a loaded simple model with real geometry — the
         * whole font asset loads together, so this means the rest are in too. */
        uint32_t m0 = cpu->read_word(FONT.model_table + (uint32_t)FONT.digit_base * 4u);
        if (m0 == 0u) return;
        if (m0 < 0x80000000u) m0 |= 0x80000000u;
        if ((int32_t)cpu->read_word(m0) >= 0) return;        /* pointer set but data not yet in */
        if ((cpu->read_byte(m0) & 0xFF) == 0) return;        /* vcount 0 -> not ready */
        for (int s = 0; s < SLOT_SYNTH_PERIOD; s++) {
            const int idx = slot_glyph_index(s);
            if (idx >= 0) cache_one_class(cpu, &s_glyph[s], idx);
        }
        if (FONT.synth_period) synth_period();
        synth_hyphen();
        cache_finish(1);
    } else if (FONT.form == GAME_FONT_ONE_CLASS) {
        static int s_file_tried = 0;
        if (!s_file_tried) {
            s_file_tried = 1;
            if (font_file_load()) { cache_finish(2); return; }
        }
        int need = 0;
        for (int s = 0; s < SLOT_SYNTH_PERIOD; s++)
            if (slot_glyph_index(s) >= need) need = slot_glyph_index(s) + 1;
        const uint32_t model = one_class_model(cpu, need);
        if (!model) return;
        for (int s = 0; s < SLOT_SYNTH_PERIOD; s++) {
            const int idx = slot_glyph_index(s);
            if (idx >= 0) cache_one_block(cpu, &s_glyph[s], model, idx);
        }
        /* The model-table entry is set before the level's data is all in: take
         * the snapshot only once every digit and letter decoded. */
        for (int s = 0; s < SLOT_PUNCT; s++)
            if (!s_glyph[s].valid) return;
        for (int p = 0; p < N_PUNCT && FONT.punct[p].ch; p++) {
            glyph_cache_t* g = &s_glyph[SLOT_PUNCT + p];
            for (int i = 0; g->valid && i < g->vcount; i++)
                g->vy[i] = (int16_t)(g->vy[i] + FONT.punct[p].dy);
        }
        if (FONT.synth_period) synth_period();
        synth_hyphen();
        cache_finish(1);
        font_file_save();
    }
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
    int   glyph_n;             /* of those, glyph faces (the rest is frame) */
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
    cx->glyph_n = 0;
    cx->light[0] = FONT.fixed_light[0]; cx->light[1] = FONT.fixed_light[1]; cx->light[2] = FONT.fixed_light[2];
    cx->light_signed = 0;
}

/* base + light_add * t per channel, plus the highlight above hi_threshold. A
 * signed t (menu, OSD) darkens faces turned away from the light. */
static uint32_t lit_colour(int baseR, int baseG, int baseB, float t) {
    const int hi = (t > FONT.hi_threshold) ? (int)((t - FONT.hi_threshold) * FONT.hi_gain) : 0;
    int R = baseR + (int)(FONT.light_add[0] * t) + hi; if (R < 0) R = 0; else if (R > 255) R = 255;
    int G = baseG + (int)(FONT.light_add[1] * t) + hi; if (G < 0) G = 0; else if (G > 255) G = 255;
    int B = baseB + (int)(FONT.light_add[2] * t) + hi; if (B < 0) B = 0; else if (B > 255) B = 255;
    return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
}

static void push_tri(emit_ctx_t* cx, int a, int b, int c, uint32_t rgba) {
    if (!cx->out || cx->n + 3 > cx->cap) return;
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[a], cx->gy[a], cx->gz[a], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[b], cx->gy[b], cx->gz[b], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[c], cx->gy[c], cx->gz[c], rgba };
    cx->glyph_n += 3;
}

/* Emit one cached glyph with its left edge at cursor_x, centred on center_y.
 * dy_model shifts the model vertically (model units) — used for the colon dots.
 * baseR/G/B is the (0..255) glyph colour. Returns the glyph width (overlay
 * frac), or -1 if the glyph isn't in the cache. */
static float emit_glyph(emit_ctx_t* cx, int slot, float cursor_x, float center_y,
                        float scale, float dy_model, float yaw,
                        int baseR, int baseG, int baseB) {
    if (slot < 0 || slot >= NGLYPH || !s_glyph[slot].valid) return -1.0f;
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
        int i0 = g->fi[f][0], i1 = g->fi[f][1], i2 = g->fi[f][2], i3 = g->fi[f][3];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        float nz0 = (float)g->fn[f][2];
        float nx0 = TXT_XSIGN * (float)g->fn[f][0]; /* match mirrored geometry */
        float ny  = (float)g->fn[f][1];
        float nx = nx0 * cs + nz0 * sn;                    /* yaw the normal with the glyph */
        float nz = -nx0 * sn + nz0 * cs;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-3f) nl = 1.0f;
        float t = (nx * cx->light[0] + ny * cx->light[1] + nz * cx->light[2]) / nl; /* N·L (unit L) */
        if (cx->light_signed) { if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f; }
        else                { if (t <  0.0f) t =  0.0f; else if (t >  1.0f) t =  1.0f; }
        uint32_t rgba = lit_colour(baseR, baseG, baseB, t);
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
    const float scale    = cap_h / FONT.model_h;
    const float center_y = origin_y + cap_h * 0.5f;
    const int   period   = glyph_slot('.');
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
            float w = emit_glyph(cx, period, cursor, center_y, scale * FONT.colon_scale,
                                 FONT.colon_recenter + FONT.colon_spread, yaw, baseR, baseG, baseB);
            if (w < 0.0f) { cursor += TXT_SPACE; continue; }
            emit_glyph(cx, period, cursor, center_y, scale * FONT.colon_scale,
                       FONT.colon_recenter - FONT.colon_spread, yaw, baseR, baseG, baseB);
            cursor += w + TXT_GAP; gi++;
            continue;
        }
        int slot = glyph_slot(c);
        if (slot < 0) { cursor += TXT_SPACE; continue; }
        float adv, inset;
        glyph_metrics(slot, scale, &adv, &inset);
        if (adv < 0.0f) { cursor += TXT_SPACE; continue; }  /* not cached -> skip */
        if (emit_glyph(cx, slot, cursor + inset, center_y, scale, 0.0f, yaw,
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

/* OPENPETE_TEXT_GLYPH_CENSUS=1: one stderr line each time the vertex count a
 * consumer publishes changes, with how many of them are glyph faces (the
 * rest is frame) and the snapshot's source (none, ram, file).
 * The count a check harness reads to tell a drawn font from an absent one. */
static void glyph_census(int consumer, int nverts, int glyph_verts) {
    static int s_on = -1;
    static int s_last[4] = { -1, -1, -1, -1 };
    if (s_on < 0) { const char* e = getenv("OPENPETE_TEXT_GLYPH_CENSUS"); s_on = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_on || s_last[consumer] == nverts) return;
    s_last[consumer] = nverts;
    static const char* const name[4] = { "perf", "menu", "osd", "version" };
    static const char* const src[3]  = { "none", "ram", "file" };
    fprintf(stderr, "[text-glyph vbl=%u] consumer=%s verts=%d glyph_verts=%d source=%s\n",
            openpete_frame, name[consumer], nverts, glyph_verts, src[s_cache_source]);
}

/* ---- performance-stats overlay (the first consumer) --------------------- */
/* Sized for the detailed readout's four lines, not the one-line counter; must
 * not exceed the renderer's OVL_MAX_VERTS, which drops the overflow. */
#define PERF_OVL_MAX   16384
#define PERF_ORIGIN_X  0.014f
#define PERF_ORIGIN_Y  0.016f
#define PERF_CAP_H     0.020f   /* small corner counter */
#define PERF_LINE_STEP 0.030f   /* cap height + a line's worth of leading */
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
    if (!openpete_native_scene_enabled()) return;   /* inert headless: no cache, no draw */
    text_moby_native_cache(cpu);                     /* snapshot the font as soon as available */
    int level = openpete_native_perf_stats_level();
    if (level <= OPENPETE_PERF_OFF || !text_moby_native_ready()) {
        if (s_shown_n != 0) {
            openpete_native_set_overlay_glyphs(NULL, 0); /* nothing until cached (no fallback) */
            glyph_census(0, 0, 0);
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
                                    PERF_CAP_H, FONT.fixed_tint,   /* the game's own text base colour */
                                    s_perf_buf + nv, PERF_OVL_MAX - nv);
    }
    openpete_native_set_overlay_glyphs(nv > 0 ? s_perf_buf : NULL, nv);
    glyph_census(0, nv, nv);
    memcpy(s_shown, lines, sizeof lines);
    s_shown_n = nl;
    s_shown_anchor = anchor;
}

/* ---- the build-version line --------------------------------------------
 * One short line naming the build, region-local like the stats block: the
 * renderer pins the region to the bottom-left anchor and decides how long it
 * stays up. Same cap height and inset as the stats readout, so the two corner
 * blocks read as one set.
 *
 * Built once. The string is fixed at configure time, so re-emitting it every
 * tick would spend the glyph builder's whole cost for identical geometry on
 * the thread that owns the tick deadline.
 *
 * The game's HUD font has no hyphen model, so the separators of an untagged
 * describe string ("v0.3.0-12-gabc1234") come out as spaces. The digits and
 * the commit hash, which are what identifies the build, are unaffected. */
#define VER_CAP_H     PERF_CAP_H
#define VER_ORIGIN_X  PERF_ORIGIN_X
#define VER_ORIGIN_Y  PERF_ORIGIN_Y
/* The extruded glyph models run ~180 verts each, so a describe string of two
 * dozen characters needs thousands: sized like the stats readout's buffer,
 * not like one short word. Must not exceed the renderer's VER_MAX_VERTS,
 * which drops the overflow. */
#define VER_OVL_MAX   16384

static openpete_hud_vert s_ver_buf[VER_OVL_MAX];
static int s_ver_pushed = -1;   /* -1 = nothing pushed yet, 0 = cleared */

void text_moby_native_version(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;   /* inert headless: no cache, no draw */
    text_moby_native_cache(cpu);
    if (!openpete_native_version_line_enabled() || !text_moby_native_ready()) {
        if (s_ver_pushed != 0) {
            openpete_native_set_version_glyphs(NULL, 0, 0.0f, 0.0f);
            s_ver_pushed = 0;
        }
        return;
    }
    if (s_ver_pushed > 0) return;   /* the string never changes */

    const char* text = openpete_build_version();
    const float region_w = 2.0f * VER_ORIGIN_X + measure_string(text, VER_CAP_H);
    const float region_h = 2.0f * VER_ORIGIN_Y + VER_CAP_H;
    const int nv = text_moby_native_emit(text, VER_ORIGIN_X, VER_ORIGIN_Y,
                                         VER_CAP_H, FONT.fixed_tint,
                                         s_ver_buf, VER_OVL_MAX);
    if (nv <= 0) return;   /* font cached but this string emitted nothing: retry next tick */
    openpete_native_set_version_glyphs(s_ver_buf, nv, region_w, region_h);
    glyph_census(3, nv, nv);
    s_ver_pushed = nv;
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
 * N·L shading as the world's shaded mobys (moby.c shaded_lit_color),
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

/* OPENPETE_TEXT_GLYPH_CENSUS=2 adds the FNV-1a of published vertex buffers:
 * the menu's and the OSD's every 64th shimmer phase, and once, when the
 * snapshot is first ready, a fixed probe string through the fixed-light
 * emitter. Two builds that print the same lines emit the same vertices. */
static void glyph_hash_census(const char* what, const openpete_hud_vert* v, int n) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_TEXT_GLYPH_CENSUS"); s_on = (e && atoi(e) >= 2) ? 1 : 0; }
    if (!s_on) return;
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t* p = (const uint8_t*)v;
    for (size_t i = 0; i < (size_t)n * sizeof *v; i++) { h ^= p[i]; h *= 0x00000100000001B3ULL; }
    fprintf(stderr, "[text-glyph-hash] %s shimmer=%u n=%d fnv=%016llx\n",
            what, text_moby_native_shimmer_phase(), n, (unsigned long long)h);
}
static void glyph_hash_probe(void) {
    static int s_done = 0;
    static openpete_hud_vert s_probe[32768];
    if (s_done || !text_moby_native_ready()) return;
    s_done = 1;
    int n = text_moby_native_emit("0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ .:'%/?+^", 0.1f, 0.2f, 0.03f,
                                  0x606000u, s_probe, 32768);
    glyph_hash_census("probe", s_probe, n);
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
 * (SpecularUpdate's rotating ellipse), signed N·L, the descriptor's light terms. The frame is a
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
    return lit_colour(MENU_BORDER_R, MENU_BORDER_G, MENU_BORDER_B, t);
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
    const float scale = cap_h / FONT.model_h;
    float cursor = 0.0f;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        if (c == ':') {
            int slot = glyph_slot('.');
            if (slot < 0 || !s_glyph[slot].valid) { cursor += TXT_SPACE; continue; }
            cursor += (float)(s_glyph[slot].vxmax - s_glyph[slot].vxmin)
                      * scale * FONT.colon_scale + TXT_GAP;
            continue;
        }
        float adv, inset;
        glyph_metrics(glyph_slot(c), scale, &adv, &inset);
        if (adv < 0.0f) { cursor += TXT_SPACE; continue; }
        cursor += adv + TXT_GAP;
    }
    return cursor;
}

/* Emit the settings menu's close glyph at `v0` and anchor it by its INK.
 *
 * The layout square is an advance box: it carries the trailing inter-glyph
 * gap and, for a proportional glyph, leading space the X does not fill, so
 * placing the box leaves unequal margins above and right of the drawn
 * pixels. The glyph is emitted at the square, its drawn extent is read back
 * off the emitted vertices, and the whole run is then shifted in x until the
 * gap from the panel's right border to the rightmost ink equals the gap from
 * the panel's top border to the topmost ink. The resulting rect is published
 * so the mouse consumer tests the pixels that were drawn. */
static void menu_close_anchor(emit_ctx_t* cx, int v0,
                              const openpete_menu_layout_t* L) {
    emit_string(cx, "X", L->close_x, L->close_y, L->close_cap_h,
                MENU_TXT_TINT, -1);
    if (cx->n <= v0) return;
    float x0 = cx->out[v0].x, x1 = x0;
    float y0 = cx->out[v0].y, y1 = y0;
    for (int i = v0 + 1; i < cx->n; i++) {
        const float vx = cx->out[i].x, vy = cx->out[i].y;
        if (vx < x0) x0 = vx;
        if (vx > x1) x1 = vx;
        if (vy < y0) y0 = vy;
        if (vy > y1) y1 = vy;
    }
    const float dx = (L->panel_x1 - (y0 - L->panel_y0)) - x1;
    for (int i = v0; i < cx->n; i++) cx->out[i].x += dx;
    openpete_menu_note_close_rect(x0 + dx, y0, x1 + dx, y1);
}

/* Build the settings-menu glyph text (title + each row's label/value) from the
 * same gold glyph mobys, with the selected row's per-glyph yaw wobble, and push
 * it to the native renderer (draw_menu_overlay draws it over the panel). Reads
 * the menu's shared layout so text lands on the panel's rows. Pure presentation. */
void text_moby_native_menu(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    text_moby_native_cache(cpu);
    if (!openpete_menu_is_open() || !text_moby_native_ready()) {
        openpete_native_set_menu_glyphs(NULL, 0);
        glyph_census(1, 0, 0);
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
    /* The bottom-anchored row's y is computed from this height; publish it
     * before the rows are placed so the anchor uses what was drawn. */
    openpete_menu_note_row_cap(row_cap);
    openpete_menu_get_layout(&L);
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
    /* Close glyph: not a row, so no wobble and no cursor. */
    menu_close_anchor(&cx, cx.n, &L);
    /* Gold frame — same rotating light as the rows above (cx still carries it). */
    cx.n += emit_lit_border(&cx, L.panel_x0, L.panel_y0, L.panel_x1, L.panel_y1,
                            MENU_BORDER_BT, s_menu_buf + cx.n, MENU_TXT_MAX - cx.n);
    openpete_native_set_menu_glyphs(s_menu_buf, cx.n);
    glyph_hash_probe();
    if ((text_moby_native_shimmer_phase() & 63u) == 0u) glyph_hash_census("menu", s_menu_buf, cx.n);
    glyph_census(1, cx.n, cx.glyph_n);
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
    if (!openpete_native_scene_enabled()) return;
    text_moby_native_cache(cpu);
    if (!openpete_osd_enabled() || !text_moby_native_ready()) {
        openpete_native_set_osd_glyphs(NULL, 0, NULL, 0);
        glyph_census(2, 0, 0);
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
    if ((text_moby_native_shimmer_phase() & 63u) == 0u) glyph_hash_census("osd", s_osd_buf, cx.n);
    glyph_census(2, cx.n, cx.glyph_n);
}
