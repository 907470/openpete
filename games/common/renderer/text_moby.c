// games/common/renderer/text_moby.c
//
// Render text with the game's own extruded 3D glyph models: the gold letters
// Spyro 1 draws its gem and life counters, "DEMO MODE", dragon names and
// pause menu with, and the ones Spyro 3 spells its home-world portal names
// with, and the credits glyphs Spyro 2 and Spyro 3 spell their end credits
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
// cutscene), so its decoded geometry is snapshotted from guest RAM the first
// time it is available (text_moby_native_cache). Spyro 2 and 3 load their
// credits font only for the credits, so the producer reads that model from
// the disc image instead (GAME_FONT_WAD_MODEL), once, at its first call. Every
// draw works from the snapshot; until one exists, ready() is false and the
// consumers publish no glyphs.
//
// Glyphs are projected orthographically at a fixed window-relative spot (not
// through the camera: an overlay sits at a window corner and looks identical
// every present, wherever the game camera points), shaded per face by a
// fixed or rotating light, and emitted in resolution-independent overlay
// coordinates. The native renderer draws them depth-tested on the live present.
//
// Pure presentation: reads model geometry from guest RAM or the disc image and
// calls native-render setters. Writes no gameplay-readable state.

#include "psx_runtime_internal.h"            /* CPUState (read_word / read_byte) */
#include "openpete_native_render.h" /* openpete_hud_vert + the overlay API */
#include "openpete_menu.h"          /* menu state/layout for the settings-menu text */
#include "openpete_perf_stats.h"    /* the overlay's levels + line text */
#include "openpete_overlay_anchor.h" /* per-line alignment inside an anchored block */
#include "openpete_osd.h"          /* toast ring - the OSD builder below */
#include "openpete_build_id.h"     /* the version line below */
#include "openpete_fiber.h"        /* speculative / replayed ticks skip the shimmer */
#include "openpete_disc_trace.h"   /* OPENPETE_DISC_TAG_FONT */
#include "openpete_log.h"
#include "game_layout.h"
#include "renderer/text_moby.h"
#include "renderer/wad_subfile.h"   /* the WAD read of GAME_FONT_WAD_MODEL */
#include "openpete_fnv1a64.h"

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

/* depth band: map model Z (~±24) to a thin slice of view-Z around 4096 so a
 * glyph's own faces z-sort (front, +Z, nearer = smaller view-Z) without being
 * huge. The slice is linear in model Z on the hyperbolic curve
 * d = vz / (vz + 4096): d = TXT_DEPTH_MID - mz * TXT_DEPTH_K, and the view-Z is
 * that curve's inverse 4096 * d / (1 - d). */
#define TXT_DEPTH_MID 0.5f
#define TXT_DEPTH_K   0.0016f
/** @brief Converts a normalised depth d to a view-Z on the hyperbolic depth curve.
 *
 * Returns OPENPETE_DEPTH_VIEWZ_SCALE * d / (1 - d), the inverse of
 * d = vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE).
 * @param d The depth, below 1.
 * @return The view-Z. */
static float txt_depth_vz(float d)
{
    return OPENPETE_DEPTH_VIEWZ_SCALE * d / (1.0f - d);
}

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
 * pause menu uses COSINE_8((tick*8+i*12)&0xFF)*3>>9 -> peak 24/256 turn ≈ ±33.75°
 * ≈ ±0.589 rad. Phase advances 8/frame, +12 per letter (a ripple along the row). */
#define TXT_WOBBLE_AMP 0.589f

/* Glyph lighting direction is per-emission so the FPS counter keeps its
 * fixed front light while the settings menu, the OSD and the launcher's title
 * use a rotating specular light (the metallic shimmer). glyph_pose dots the
 * face normal with this unit vector. text_moby_native_emit installs the fixed
 * model; the menu and OSD builders install the rotating ellipse.
 *
 * signed_nl: 0 (FPS) = add-only, faces range base..base+add. 1 (menu/OSD) =
 * signed - faces turned away darken below base, giving metal contrast. */
typedef struct {
    float add[3];               /* the font's light_add                 */
    float hi_threshold, hi_gain;/* the font's highlight terms           */
    float dir[3];               /* unit light direction (N.L)           */
    int   signed_nl;            /* see the note above                   */
} glyph_light_t;

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
 * the synthesized hyphen. */
#define SLOT_DIGIT   0
#define SLOT_LETTER  10
#define SLOT_PUNCT   36
#define N_PUNCT      ((int)(sizeof FONT.punct / sizeof FONT.punct[0]))
#define SLOT_SYNTH_HYPHEN (SLOT_PUNCT + 8)
#define NGLYPH       (SLOT_SYNTH_HYPHEN + 1)
_Static_assert(sizeof FONT.punct / sizeof FONT.punct[0] == 8, "slot table sized for 8 punctuation pairs");

/* The GAME_FONT_WAD_MODEL model is read whole into a host buffer of this
 * many bytes (the credits font of Spyro 2 and Spyro 3 is 0x5000). */
#define WAD_MODEL_MAX 0x10000u

/* One glyph set: everything a fill writes. The producer's own set is s_set;
 * an ingest job fills a set it allocates (text_glyph_set_read_disc). */
struct text_glyph_set {
    glyph_cache_t glyph[NGLYPH];
    int ready;
    /* Common advance for '0'-'9' (model units) - see cache_finish. */
    int digit_adv;
    /* Where the snapshot came from: 0 none, 1 guest RAM, 2 the disc image. */
    int source;
    /* GAME_FONT_WAD_MODEL: one read attempt per set, and the model read. */
    int wad_tried;
    uint8_t model[WAD_MODEL_MAX];
};

static text_glyph_set s_set;

/** @brief Reports whether a glyph cache slot holds a digit '0'-'9'.
 *
 * @param slot The cache slot.
 * @return 1 for SLOT_DIGIT..SLOT_LETTER-1, else 0. */
static int is_digit_slot(int slot) { return slot >= SLOT_DIGIT && slot < SLOT_LETTER; }

/** @brief Maps a character to its glyph cache slot.
 *
 * Letters map case-insensitively; a character in the font's punctuation
 * list maps to its punctuation slot; '-' maps to the synthesized hyphen
 * when the font lists no hyphen.
 * @param c The character.
 * @return The slot, or -1 for a character that only advances (a space or an
 *         unsupported character). */
static int glyph_slot(char c) {
    if (c >= '0' && c <= '9') return SLOT_DIGIT + (c - '0');
    if (c >= 'A' && c <= 'Z') return SLOT_LETTER + (c - 'A');
    if (c >= 'a' && c <= 'z') return SLOT_LETTER + (c - 'a');
    for (int p = 0; p < N_PUNCT && FONT.punct[p].ch; p++)
        if (FONT.punct[p].ch == c) return SLOT_PUNCT + p;
    /* Unconditional, unlike the period: a font that carries a hyphen returns
     * it from the punctuation loop above and never reaches here, and one that
     * does not gets the synthesized bar. */
    if (c == '-') return SLOT_SYNTH_HYPHEN;
    return -1;
}

/** @brief Maps a glyph cache slot to the font descriptor's glyph index.
 *
 * The index is a moby class for GAME_FONT_CLASS_PER_GLYPH and a block index
 * for GAME_FONT_WAD_MODEL. A punctuation slot at or past the first empty
 * entry of the descriptor's list has none.
 * @param slot The cache slot.
 * @return The glyph index, or -1 for a slot the game's font does not fill
 *         (an unused punctuation slot and the synthesized hyphen). */
static int slot_glyph_index(int slot) {
    if (slot < SLOT_LETTER) return FONT.digit_base + (slot - SLOT_DIGIT);
    if (slot < SLOT_PUNCT)  return FONT.letter_base + (slot - SLOT_LETTER);
    if (slot < SLOT_SYNTH_HYPHEN) {
        for (int p = 0; p <= slot - SLOT_PUNCT; p++)
            if (!FONT.punct[p].ch) return -1;
        return FONT.punct[slot - SLOT_PUNCT].index;
    }
    return -1;
}

/** @brief Returns a cached glyph's width in model units.
 *
 * The width is the span of the glyph's vertex X range (vxmax - vxmin).
 * @param slot The cache slot; any int is accepted.
 * @return The width, or -1 when the slot is out of range or not cached. */
static float glyph_natural_w(int slot) {
    if (slot < 0 || slot >= NGLYPH || !s_set.glyph[slot].valid) return -1.0f;
    return (float)(s_set.glyph[slot].vxmax - s_set.glyph[slot].vxmin);
}

/** @brief Computes a glyph's advance and the inset of its glyph inside that advance, in overlay units.
 *
 * A non-digit is proportional: the advance is the glyph width and the inset
 * 0. A digit takes the shared tabular advance (s_set.digit_adv) and is centred
 * in it. emit_string and measure_string both go through here, so the two
 * agree on a string's width. The trailing inter-glyph gap is not included.
 * @param slot The cache slot, or -1.
 * @param scale Overlay units per model unit.
 * @param adv Receives the advance, or -1 when the slot is not cached.
 * @param inset Receives the inset. */
static void glyph_metrics(int slot, float scale, float* adv, float* inset) {
    const float w = glyph_natural_w(slot);
    if (w < 0.0f) { *adv = -1.0f; *inset = 0.0f; return; }
    if (is_digit_slot(slot) && s_set.digit_adv > 0) {
        *adv   = (float)s_set.digit_adv * scale;
        *inset = 0.5f * (*adv - w * scale);   /* centred in its column */
    } else {
        *adv   = w * scale;
        *inset = 0.0f;
    }
}

/** @brief Recomputes a glyph's vxmin and vxmax from its vertices.
 *
 * Scans the first vcount entries of vx. With vcount 0 the range is left
 * inverted (vxmin 1<<30, vxmax -(1<<30)).
 * @param g The glyph cache entry, updated in place. */
static void glyph_set_xrange(glyph_cache_t* g) {
    int vxmin = 1 << 30, vxmax = -(1 << 30);
    for (int i = 0; i < g->vcount; i++) {
        if (g->vx[i] < vxmin) vxmin = g->vx[i];
        if (g->vx[i] > vxmax) vxmax = g->vx[i];
    }
    g->vxmin = vxmin; g->vxmax = vxmax;
}

/* Where a GAME_FONT_CLASS_PER_GLYPH font's models are read from: guest RAM
 * through `cpu` when `buf` is NULL (the class's model through
 * FONT.model_table, its pointers absolute), else a host copy of the font
 * file the game loads them from (a data-size word, then 64 (offset, class)
 * pairs ended by a zero offset, each offset relative to the file and each
 * model pointer relative to its model); a host read outside the copy reads
 * 0. */
typedef struct {
    CPUState*      cpu;
    const uint8_t* buf;
    uint32_t       len;
} glyph_src;

/** @brief Reads one byte of a glyph source.
 * @param s The source.
 * @param a The address (guest) or offset (host).
 * @return The byte, 0 outside a host source. */
static uint32_t gs_rd8(const glyph_src* s, uint32_t a) {
    if (!s->buf) return s->cpu->read_byte(a) & 0xFFu;
    return a < s->len ? s->buf[a] : 0u;
}

/** @brief Reads one little-endian word of a glyph source.
 * @param s The source.
 * @param a The address (guest) or offset (host).
 * @return The word, 0 when it does not fit a host source. */
static uint32_t gs_rd32(const glyph_src* s, uint32_t a) {
    if (!s->buf) return s->cpu->read_word(a);
    if (a > s->len || s->len - a < 4u) return 0u;
    const uint8_t* b = s->buf + a;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/** @brief Finds a class's model in a glyph source.
 *
 * Guest RAM: FONT.model_table[cls], with the KSEG0 bit forced. Host: the
 * offset the font file's (offset, class) pairs give the class.
 * @param s The source.
 * @param cls The moby class.
 * @return The model's address or offset, 0 when the class has none. */
static uint32_t gs_model(const glyph_src* s, int cls) {
    if (!s->buf) {
        const uint32_t m = s->cpu->read_word(FONT.model_table + (uint32_t)cls * 4u);
        return m == 0u ? 0u : (m | 0x80000000u);
    }
    for (uint32_t i = 0; i < 64u; i++) {
        const uint32_t off = gs_rd32(s, 4u + 8u * i);
        if (off == 0u) break;
        if ((int32_t)gs_rd32(s, 8u + 8u * i) == cls) return off;
    }
    return 0u;
}

/** @brief Resolves a model's pointer field in a glyph source.
 *
 * Guest RAM holds the absolute address (the KSEG0 bit forced); the host
 * copy holds it relative to the model.
 * @param s The source.
 * @param model The model's address or offset.
 * @param field The field's offset in the model header.
 * @return The pointed-to address or offset. */
static uint32_t gs_ptr(const glyph_src* s, uint32_t model, uint32_t field) {
    const uint32_t v = gs_rd32(s, model + field);
    return s->buf ? model + v : (v | 0x80000000u);
}

/** @brief Decodes the simple model of one moby class into a glyph cache entry, for GAME_FONT_CLASS_PER_GLYPH.
 *
 * Each vertex is 3 bytes: Z, X and Y are its bytes 0, 1 and 2, sign-extended
 * and doubled; X and Y also take the top bit of the byte below as bit 0.
 * Each face is 8 bytes: four 7-bit vertex indices in word 0 and a
 * signed-byte normal (x byte 2, y byte 1, z byte 3) in word 1. The entry
 * stays invalid when the class has no model, the model's word 0 is not
 * negative (not a simple model, or its data not yet loaded), or its vertex or
 * face count is out of range.
 * @param src Where the models are read.
 * @param g The cache entry.
 * @param cls The moby class. */
static void cache_one_class(const glyph_src* src, glyph_cache_t* g, int cls) {
    g->valid = 0;
    const uint32_t model = gs_model(src, cls);
    if (model == 0u) return;
    if ((int32_t)gs_rd32(src, model) >= 0) return;     /* not a simple model / data not in */
    int vcount = (int)gs_rd8(src, model + 0x0u);
    int fcount = (int)gs_rd8(src, model + 0x1u);
    if (vcount <= 0 || vcount > TXT_MAX_GLYPH_V || fcount > TXT_MAX_GLYPH_F) return;
    const uint32_t vbase = gs_ptr(src, model, 0x4u);
    const uint32_t faces = gs_ptr(src, model, 0xCu);
    for (int i = 0; i < vcount; i++) {
        uint32_t p = vbase + (uint32_t)i * 3u;
        uint32_t w = gs_rd8(src, p) | (gs_rd8(src, p + 1) << 8) | (gs_rd8(src, p + 2) << 16);
        int VZ = ((int32_t)(w << 24)) >> 23;
        int VX = ((int32_t)(w << 16)) >> 23;
        int VY = ((int32_t)(w <<  8)) >> 23;
        g->vx[i] = (int16_t)VX; g->vy[i] = (int16_t)VY; g->vz[i] = (int16_t)VZ;
    }
    for (int f = 0; f < fcount; f++) {
        uint32_t fp = faces + (uint32_t)f * 8u;
        uint32_t w0 = gs_rd32(src, fp), w1 = gs_rd32(src, fp + 4u);
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

/* ---- GAME_FONT_WAD_MODEL: the model read from the disc image ------------- */

/** @brief Loads a little-endian 32-bit word at a byte offset of a host buffer.
 *
 * @param b The buffer.
 * @param off The byte offset; the caller keeps off + 4 inside the buffer.
 * @return The word. */
static uint32_t le32(const uint8_t* b, uint32_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/** @brief Decodes one block of a GAME_FONT_WAD_MODEL model into a glyph cache entry.
 *
 * Every offset in the model is relative to its first byte (layout in
 * game_layout.h). The model's -Z faces the reader, so Z and the normal's Z
 * are negated into the host form. A textured face (20 bytes) and a face
 * without the flat-lit bit are skipped, as they are not glyph faces. The
 * entry stays invalid when the model's first word is not a negated block
 * count covering @p index, when the block, its vertices or its face list
 * reach past @p len, when the vertex count is outside 3..TXT_MAX_GLYPH_V, or
 * when no glyph face remains.
 * @param m The model bytes.
 * @param len Their count.
 * @param g The cache entry.
 * @param index The block index. */
static void wad_decode_block(const uint8_t* m, uint32_t len, glyph_cache_t* g, int index) {
    g->valid = 0;
    if (len < 0x18u || (int32_t)le32(m, 0) >= 0) return;
    const uint32_t count = (uint32_t)-(int32_t)le32(m, 0);
    if (index < 0 || (uint32_t)index >= count || 0x14u + 4u * count > len) return;
    const uint32_t H = le32(m, 0x14u + 4u * (uint32_t)index);
    if (H > len - 0x10u) return;
    const int vcount = (int)((le32(m, H) >> 8) & 0xFFu);
    if (vcount < 3 || vcount > TXT_MAX_GLYPH_V || H + 0x10u + 4u * (uint32_t)vcount > len) return;
    for (int i = 0; i < vcount; i++) {
        uint32_t w = le32(m, H + 0x10u + (uint32_t)i * 4u);
        g->vx[i] = (int16_t)((int32_t)(w << 10) >> 21);
        g->vy[i] = (int16_t)((int32_t)(w << 20) >> 20);
        g->vz[i] = (int16_t)-((int32_t)w >> 21);
    }
    const uint32_t faces = le32(m, 0x10u) + (le32(m, H + 0x08u) & 0xFFFFu);
    if (faces > len - 4u) return;
    uint32_t fp = faces + 4u;
    const uint32_t flen = le32(m, faces);
    if (flen > len - fp) return;
    const uint32_t end = fp + flen;
    int fcount = 0;
    while (fp + 8u <= end && fcount < TXT_MAX_GLYPH_F) {
        uint32_t w0 = le32(m, fp), w1 = le32(m, fp + 4u);
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

/** @brief Reads the GAME_FONT_WAD_MODEL model from the booted disc into a host buffer.
 *
 * The model is sub-file FONT.wad_subfile of WAD entry FONT.wad_entry, read
 * through wad_subfile_read under the font tag (a host path: its misses never
 * steer the disc cache loader and never enter a boot profile).
 * @param buf Receives the model.
 * @param cap The buffer's capacity in bytes.
 * @return The model's byte count, or 0 with the reason logged. */
static uint32_t wad_model_read(uint8_t* buf, uint32_t cap) {
    return wad_subfile_read(FONT.wad_entry, FONT.wad_subfile, OPENPETE_DISC_TAG_FONT, buf, cap, "font");
}

/** @brief Builds the synthesized hyphen glyph: a bar measured off digit '0'.
 *
 * The bar is half digit '0''s cap height wide and an eighth of it thick
 * (each at least 1 model unit), centred half the cap height above the
 * baseline, and spans the digit's Z range. Every dimension is a fraction of
 * the cap height, so it scales with the font. It is a six-faced box with
 * axis normals, so it takes the same lighting and extrusion as a real
 * glyph. Digit '0' is the reference because the snapshot is never
 * taken without it. The slot stays invalid when digit '0' is not cached or
 * has no height or depth.
 * @param G The glyph set. */
static void synth_hyphen(text_glyph_set* G) {
    glyph_cache_t* g = &G->glyph[SLOT_SYNTH_HYPHEN];
    g->valid = 0;
    /* Everything is measured off the digit '0': it is the one glyph the
     * snapshot is not taken without, so the bar exists wherever text does. */
    const glyph_cache_t* zero = &G->glyph[SLOT_DIGIT];
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

/** @brief Completes a glyph set: sets the tabular digit advance and marks the set ready.
 *
 * The glyph models are proportional ('1' is narrower than '4'), so a number
 * redrawn with different digits would change width and text laid out from
 * its measured width would reflow. Every digit takes the widest valid
 * digit's advance (digit_adv) and is centred in it (glyph_metrics), so a
 * number's width depends only on its digit count. measure_string and
 * emit_string both read it, so every consumer (the FPS counter, the menu's
 * values, the OSD, a mod's HUD) gets aligned columns. Once ready, neither
 * guest RAM nor the disc is read again for the set.
 * @param G The glyph set.
 * @param source The snapshot's source: 1 guest RAM, 2 the disc image. */
static void cache_finish(text_glyph_set* G, int source) {
    G->digit_adv = 0;
    for (int d = 0; d < 10; d++) {
        const glyph_cache_t* g = &G->glyph[SLOT_DIGIT + d];
        if (g->valid && (g->vxmax - g->vxmin) > G->digit_adv)
            G->digit_adv = g->vxmax - g->vxmin;
    }
    G->source = source;
    G->ready = 1;                                    /* snapshot taken; never re-read */
}

/** @brief Fills a glyph set from a GAME_FONT_CLASS_PER_GLYPH source once digit '0' is a loaded simple model.
 *
 * The whole font asset loads together, so digit '0' with real geometry means
 * the rest are in too. Decodes every slot the font fills, synthesizes the
 * hyphen and completes the set.
 * @param G The glyph set.
 * @param src Where the models are read.
 * @param source The source flag cache_finish records.
 * @return 1 when the set was filled, 0 when digit '0' is not loaded. */
static int glyph_set_fill_classes(text_glyph_set* G, const glyph_src* src, int source) {
    const uint32_t m0 = gs_model(src, FONT.digit_base);
    if (m0 == 0u) return 0;
    if ((int32_t)gs_rd32(src, m0) >= 0) return 0;        /* pointer set but data not yet in */
    if (gs_rd8(src, m0) == 0u) return 0;                 /* vcount 0 -> not ready */
    for (int s = 0; s < SLOT_SYNTH_HYPHEN; s++) {
        const int idx = slot_glyph_index(s);
        if (idx >= 0) cache_one_class(src, &G->glyph[s], idx);
    }
    synth_hyphen(G);
    cache_finish(G, source);
    return 1;
}

/** @brief Fills a glyph set from a GAME_FONT_WAD_MODEL model held in the set's model buffer.
 *
 * Kept only when every digit and letter decodes; otherwise every slot is
 * cleared and the set stays not ready. Applies the punctuation offsets,
 * synthesizes the hyphen and completes the set with the disc source.
 * @param G The glyph set; its model buffer holds the model.
 * @param len The model's byte count.
 * @return 1 when the set was filled, else 0. */
static int glyph_set_fill_wad(text_glyph_set* G, uint32_t len) {
    if (len == 0u) return 0;
    for (int s = 0; s < SLOT_SYNTH_HYPHEN; s++) {
        const int idx = slot_glyph_index(s);
        if (idx >= 0) wad_decode_block(G->model, len, &G->glyph[s], idx);
    }
    for (int s = 0; s < SLOT_PUNCT; s++) {
        if (G->glyph[s].valid) continue;
        for (int t = 0; t < NGLYPH; t++) G->glyph[t].valid = 0;
        OPL_WARN_F("font", "glyph model from WAD entry %d sub-file %d lacks glyph %d",
                   FONT.wad_entry, FONT.wad_subfile, slot_glyph_index(s));
        return 0;
    }
    for (int p = 0; p < N_PUNCT && FONT.punct[p].ch; p++) {
        glyph_cache_t* g = &G->glyph[SLOT_PUNCT + p];
        for (int i = 0; g->valid && i < g->vcount; i++)
            g->vy[i] = (int16_t)(g->vy[i] + FONT.punct[p].dy);
    }
    synth_hyphen(G);
    cache_finish(G, 2);
    return 1;
}

void text_moby_native_cache(CPUState* cpu) {
    if (s_set.ready) return;
    if (FONT.form == GAME_FONT_CLASS_PER_GLYPH) {
        const glyph_src src = { cpu, NULL, 0u };
        (void)glyph_set_fill_classes(&s_set, &src, 1);
    } else if (FONT.form == GAME_FONT_WAD_MODEL) {
        /* One attempt per session: the disc does not change under it. */
        if (s_set.wad_tried) return;
        s_set.wad_tried = 1;
        const uint32_t len = wad_model_read(s_set.model, sizeof s_set.model);
        if (glyph_set_fill_wad(&s_set, len))
            OPL_INFO_F("font", "glyphs read from the disc: WAD entry %d sub-file %d, %u bytes",
                       FONT.wad_entry, FONT.wad_subfile, (unsigned)len);
    }
}

text_glyph_set* text_glyph_set_new(void) {
    return (text_glyph_set*)calloc(1, sizeof(text_glyph_set));
}

void text_glyph_set_free(text_glyph_set* G) { free(G); }

int text_glyph_set_read_disc(text_glyph_set* G, disc_image_t* img, uint64_t* hash) {
    *hash = 0;
    if (G->ready || G->wad_tried) return G->ready ? 0 : -1;
    G->wad_tried = 1;
    uint32_t len = 0;
    if (FONT.form == GAME_FONT_CLASS_PER_GLYPH) {
        uint8_t* file = wad_disc_entry_load(img, FONT.wad_entry, 0u, 0u, &len, "font");
        if (!file) return -1;
        *hash = openpete_fnv1a64_update(openpete_fnv1a64_init(), file, len);
        const glyph_src src = { NULL, file, len };
        const int ok = glyph_set_fill_classes(G, &src, 2);
        free(file);
        return ok ? 0 : -1;
    }
    if (FONT.form != GAME_FONT_WAD_MODEL) return -1;
    uint8_t* model = wad_disc_subfile_load(img, FONT.wad_entry, FONT.wad_subfile, &len, "font");
    if (!model) return -1;
    if (len > sizeof G->model) { free(model); return -1; }
    memcpy(G->model, model, len);
    free(model);
    *hash = openpete_fnv1a64_update(openpete_fnv1a64_init(), G->model, len);
    return glyph_set_fill_wad(G, len) ? 0 : -1;
}

int text_glyph_set_ready(const text_glyph_set* G) { return G->ready; }

void text_glyph_set_write(const text_glyph_set* G, FILE* out) {
    fprintf(out, "ready %d\nsource %d\ndigit_adv %d\n", G->ready, G->source, G->digit_adv);
    for (int s = 0; s < NGLYPH; s++) {
        const glyph_cache_t* g = &G->glyph[s];
        if (!g->valid) continue;
        fprintf(out, "slot %d index %d v", s, slot_glyph_index(s));
        for (int i = 0; i < g->vcount; i++) fprintf(out, " %d,%d,%d", g->vx[i], g->vy[i], g->vz[i]);
        fprintf(out, " f");
        for (int f = 0; f < g->fcount; f++)
            fprintf(out, " %d,%d,%d,%d:%d,%d,%d", g->fi[f][0], g->fi[f][1], g->fi[f][2], g->fi[f][3],
                    g->fn[f][0], g->fn[f][1], g->fn[f][2]);
        fprintf(out, "\n");
    }
}

/** @brief Parses one "slot" line of a written glyph set into its slot.
 *
 * The line is "slot S index I v x,y,z ... f a,b,c,d:nx,ny,nz ...", as
 * text_glyph_set_write prints it; the index is not read back.
 * @param G The set.
 * @param line The line, after "slot ".
 * @return 0, or -1 when the line is malformed or exceeds a glyph's capacity. */
static int glyph_set_read_slot(text_glyph_set* G, const char* line) {
    char* e;
    const long slot = strtol(line, &e, 10);
    if (e == line || slot < 0 || slot >= NGLYPH) return -1;
    glyph_cache_t* g = &G->glyph[slot];
    const char* v = strstr(e, " v");
    const char* f = v ? strstr(v, " f") : NULL;
    if (!v || !f) return -1;
    int n = 0;
    for (const char* p = v + 2; p < f && n < TXT_MAX_GLYPH_V; n++) {
        int x, y, z, used = 0;
        if (sscanf(p, " %d,%d,%d%n", &x, &y, &z, &used) != 3) break;
        g->vx[n] = (int16_t)x; g->vy[n] = (int16_t)y; g->vz[n] = (int16_t)z;
        p += used;
    }
    int m = 0;
    for (const char* p = f + 2; *p && *p != '\n' && m < TXT_MAX_GLYPH_F; m++) {
        int a, b, c, d, nx, ny, nz, used = 0;
        if (sscanf(p, " %d,%d,%d,%d:%d,%d,%d%n", &a, &b, &c, &d, &nx, &ny, &nz, &used) != 7) break;
        if (a < 0 || b < 0 || c < 0 || d < 0 || a >= n || b >= n || c >= n || d >= TXT_MAX_GLYPH_V) return -1;
        g->fi[m][0] = (uint8_t)a; g->fi[m][1] = (uint8_t)b; g->fi[m][2] = (uint8_t)c; g->fi[m][3] = (uint8_t)d;
        g->fn[m][0] = (int16_t)nx; g->fn[m][1] = (int16_t)ny; g->fn[m][2] = (int16_t)nz;
        p += used;
    }
    if (n == 0 || m == 0) return -1;
    g->vcount = n; g->fcount = m;
    glyph_set_xrange(g);
    g->valid = 1;
    return 0;
}

int text_glyph_set_read(text_glyph_set* G, FILE* in) {
    memset(G, 0, sizeof *G);
    G->wad_tried = 1;   /* a set read from a file never reads a disc */
    char line[16384];
    int ready = 0, slots = 0;
    while (fgets(line, sizeof line, in)) {
        if (!strchr(line, '\n') && !feof(in)) return -1;   /* longer than any line the writer prints */
        if (!strncmp(line, "ready ", 6)) ready = atoi(line + 6);
        else if (!strncmp(line, "source ", 7)) G->source = atoi(line + 7);
        else if (!strncmp(line, "digit_adv ", 10)) G->digit_adv = atoi(line + 10);
        else if (!strncmp(line, "slot ", 5)) {
            if (glyph_set_read_slot(G, line + 5) != 0) return -1;
            slots++;
        }
    }
    G->ready = ready == 1 && slots > 0;
    return G->ready ? 0 : -1;
}

int text_moby_native_ready(void) { return s_set.ready; }

void text_moby_native_dump(FILE* out) {
    fprintf(out, "glyph.ready %d\nglyph.source %d\nglyph.digit_adv %d\n",
            s_set.ready, s_set.source, s_set.digit_adv);
}

/* ---- emit (works purely from the cache) --------------------------------- */

/* Emission state, per call rather than per file. Every emitter - the stats
 * overlay and settings menu, the mod draw service at scene production, the
 * OSD every tick a toast is live - writes through one of these, so no two
 * emissions share an output cursor, glyph scratch or light direction. (A
 * torn out/cap pair would defeat push_tri's bounds check and write past the
 * caller's buffer; a light overwritten mid-string would tear the shimmer
 * across the glyphs.) */
typedef struct {
    openpete_hud_vert* out;    /* caller's buffer                      */
    int   cap;                 /* its capacity, in verts               */
    int   n;                   /* verts written so far                 */
    int   glyph_n;             /* of those, glyph faces (the rest is frame) */
    int   dropped;             /* triangles refused for want of capacity */
    glyph_light_t lt;          /* the font's light terms and the light   */
    /* per-glyph vertices in overlay space and face colours (glyph_pose). */
    float gx[TXT_MAX_GLYPH_V], gy[TXT_MAX_GLYPH_V], gz[TXT_MAX_GLYPH_V];
    uint32_t frgba[TXT_MAX_GLYPH_F];
} emit_ctx_t;

/** @brief Fills a light from a font's light terms, with the font's fixed front light and unsigned N.L.
 *
 * @param lt The light.
 * @param font The font descriptor. */
static void glyph_light_init(glyph_light_t* lt, const struct game_font* font) {
    for (int k = 0; k < 3; k++) {
        lt->add[k] = font->light_add[k];
        lt->dir[k] = font->fixed_light[k];
    }
    lt->hi_threshold = font->hi_threshold;
    lt->hi_gain = font->hi_gain;
    lt->signed_nl = 0;
}

/** @brief Starts an emission into a vertex buffer, lit by the font's fixed front light.
 *
 * Counters are zeroed and the light is FONT.fixed_light with unsigned N.L
 * shading. Callers wanting the rotating specular install it afterwards
 * (install_specular_light).
 * @param cx The emission context.
 * @param out The vertex buffer, or NULL to emit nothing.
 * @param cap The buffer's capacity, in vertices. */
static void emit_ctx_init(emit_ctx_t* cx, openpete_hud_vert* out, int cap) {
    cx->out = out;
    cx->cap = cap;
    cx->n   = 0;
    cx->glyph_n = 0;
    cx->dropped = 0;
    glyph_light_init(&cx->lt, &FONT);
}

/** @brief Computes a lit face colour: the base plus the font's light term times N.L, plus the highlight above hi_threshold.
 *
 * Each channel is base + light_add * t + (t - hi_threshold) * hi_gain when
 * t exceeds hi_threshold, clamped to 0..255. A negative t (the signed
 * shading of the menu and the OSD) darkens faces turned away from the light.
 * Reads only its arguments.
 * @param lt The light carrying the font's terms.
 * @param baseR The base red, 0..255.
 * @param baseG The base green, 0..255.
 * @param baseB The base blue, 0..255.
 * @param t The N.L term, -1..1.
 * @return The colour as 0xAABBGGRR with alpha 0xFF. */
static uint32_t lit_colour(const glyph_light_t* lt, int baseR, int baseG, int baseB, float t) {
    const int hi = (t > lt->hi_threshold) ? (int)((t - lt->hi_threshold) * lt->hi_gain) : 0;
    int R = baseR + (int)(lt->add[0] * t) + hi; if (R < 0) R = 0; else if (R > 255) R = 255;
    int G = baseG + (int)(lt->add[1] * t) + hi; if (G < 0) G = 0; else if (G > 255) G = 255;
    int B = baseB + (int)(lt->add[2] * t) + hi; if (B < 0) B = 0; else if (B > 255) B = 255;
    return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | 0xFF000000u;
}

/** @brief Appends one flat-coloured triangle of the current glyph's vertices to the emission buffer.
 *
 * Vertices are taken from the context's per-glyph arrays (gx, gy, gz). A
 * triangle that does not fit in the remaining capacity is counted in
 * cx->dropped and not written. Nothing is written when cx->out is NULL.
 * @param cx The emission context.
 * @param a The first vertex index into the per-glyph arrays.
 * @param b The second vertex index.
 * @param c The third vertex index.
 * @param rgba The colour of all three vertices. */
static void push_tri(emit_ctx_t* cx, int a, int b, int c, uint32_t rgba) {
    if (!cx->out) return;
    if (cx->n + 3 > cx->cap) { cx->dropped++; return; }
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[a], cx->gy[a], cx->gz[a], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[b], cx->gy[b], cx->gz[b], rgba };
    cx->out[cx->n++] = (openpete_hud_vert){ cx->gx[c], cx->gy[c], cx->gz[c], rgba };
    cx->glyph_n += 3;
}

/** @brief Returns glyph gi's wobble yaw at a wobble tick: cos(((tick * 8 + gi * 12) & 0xFF) * 2pi/256) * TXT_WOBBLE_AMP.
 *
 * The PSX pause menu's selected-row ripple: the phase advances 8 per tick
 * and 12 per glyph. Reads only its arguments.
 * @param wob_tick The wobble tick, or below 0 for an upright glyph.
 * @param gi The glyph's index in its string (spaces not counted).
 * @return The yaw in radians; 0 when wob_tick is below 0. */
static float glyph_wobble_yaw(int wob_tick, int gi) {
    if (wob_tick < 0) return 0.0f;
    const unsigned au = ((unsigned)wob_tick * 8u + (unsigned)gi * 12u) & 0xFFu;
    return cosf((float)au * (6.2831853f / 256.0f)) * TXT_WOBBLE_AMP;
}

/* Where one glyph is placed: its left edge, its vertical centre and the
 * scale, in the caller's units (overlay units or window pixels), and a
 * vertical shift in model units (the colon dots). */
typedef struct {
    float cursor_x, center_y, scale, dy_model;
} glyph_place_t;

/** @brief Poses one glyph at a wobble tick and lights its faces, writing its vertices and face colours.
 *
 * The glyph is yawed by glyph_wobble_yaw(wob_tick, gi) about its own
 * vertical mid-line (the middle of its X extent), normals included, so it
 * turns left and right in place; yaw 0 leaves it front-facing. Vertex i
 * lands at (cursor_x + (x - xmin) * scale, center_y + y * scale) with its
 * view-Z on the depth band (front nearer). Face f's colour is lit_colour of
 * the base against N.L, the yawed normal dotted with the light, clamped to
 * -1..1 for signed shading and 0..1 otherwise. Reads only its arguments, so
 * the settings menu, the OSD, the readout and the launcher's title share it.
 * @param g The glyph's geometry.
 * @param lt The font's light terms and the light.
 * @param wob_tick The wobble tick, or below 0 for an upright glyph.
 * @param gi The glyph's index in its string.
 * @param at Its placement.
 * @param base The base colour, 0xRRGGBB.
 * @param gx Receives each vertex's x, TXT_MAX_GLYPH_V entries.
 * @param gy Receives each vertex's y.
 * @param gz Receives each vertex's view-Z.
 * @param frgba Receives each face's colour, TXT_MAX_GLYPH_F entries.
 * @return The vertex count posed (vcount, at most TXT_MAX_GLYPH_V). */
static int glyph_pose(const glyph_cache_t* g, const glyph_light_t* lt, int wob_tick, int gi,
                      const glyph_place_t* at, unsigned base,
                      float* gx, float* gy, float* gz, uint32_t* frgba) {
    const int baseR = (int)((base >> 16) & 0xFFu);
    const int baseG = (int)((base >>  8) & 0xFFu);
    const int baseB = (int)( base        & 0xFFu);
    int vcount = g->vcount;
    const int vxmin = g->vxmin, vxmax = g->vxmax;
    if (vcount > TXT_MAX_GLYPH_V) vcount = TXT_MAX_GLYPH_V;
    const float yaw = glyph_wobble_yaw(wob_tick, gi);
    /* Selected-row wobble: yaw about the glyph's own vertical axis (its mid-X), so
     * it turns left<->right revealing the extrusion; the normals rotate too (below)
     * so the directional highlight sweeps. yaw==0 -> the original front-facing glyph
     * (cs=1, sn=0 leaves X/Z untouched). The advance width is the un-yawed width,
     * so letters keep their slots and just rock in place. */
    const float cs = cosf(yaw), sn = sinf(yaw);
    const float midx = 0.5f * (float)(vxmin + vxmax);
    for (int i = 0; i < vcount; i++) {
        float my = (float)g->vy[i] + at->dy_model;
        float dx = (float)g->vx[i] - midx;      /* centre X before rotating */
        float dz = (float)g->vz[i];
        float vx_r = midx + dx * cs + dz * sn;  /* yawed X */
        float mz   =       -dx * sn + dz * cs;  /* yawed Z (depth) */
        float gx_local = (TXT_XSIGN < 0.0f) ? (vxmax - vx_r) : (vx_r - vxmin);
        gx[i] = at->cursor_x + gx_local * at->scale;
        gy[i] = at->center_y + TXT_YSIGN * my * at->scale;
        gz[i] = txt_depth_vz(TXT_DEPTH_MID - mz * TXT_DEPTH_K);   /* front nearer */
    }
    for (int f = 0; f < g->fcount && f < TXT_MAX_GLYPH_F; f++) {
        float nz0 = (float)g->fn[f][2];
        float nx0 = TXT_XSIGN * (float)g->fn[f][0]; /* match mirrored geometry */
        float ny  = (float)g->fn[f][1];
        float nx = nx0 * cs + nz0 * sn;                    /* yaw the normal with the glyph */
        float nz = -nx0 * sn + nz0 * cs;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-3f) nl = 1.0f;
        float t = (nx * lt->dir[0] + ny * lt->dir[1] + nz * lt->dir[2]) / nl; /* N·L (unit L) */
        if (lt->signed_nl) { if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f; }
        else               { if (t <  0.0f) t =  0.0f; else if (t >  1.0f) t =  1.0f; }
        frgba[f] = lit_colour(lt, baseR, baseG, baseB, t);
    }
    return vcount;
}

/** @brief Appends a posed glyph's faces to the emission buffer as triangles.
 *
 * Each face is emitted without a winding cull (a quad as two triangles);
 * the depth test keeps the nearer faces and the edge-on side walls have
 * near-zero area. A face naming a vertex past vcount is skipped.
 * @param cx The emission context; gx, gy, gz and frgba hold the posed glyph.
 * @param g The glyph's geometry.
 * @param vcount The vertex count glyph_pose posed. */
static void push_glyph(emit_ctx_t* cx, const glyph_cache_t* g, int vcount) {
    for (int f = 0; f < g->fcount; f++) {
        int i0 = g->fi[f][0], i1 = g->fi[f][1], i2 = g->fi[f][2], i3 = g->fi[f][3];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        const uint32_t rgba = cx->frgba[f];
        if (i2 == i3) {
            push_tri(cx, i0, i1, i2, rgba);                   /* tri */
        } else if (i3 < vcount) {
            push_tri(cx, i0, i1, i2, rgba);                   /* quad = 2 tris */
            push_tri(cx, i1, i3, i2, rgba);
        }
    }
}

/** @brief Emits one glyph of the producer's glyph set with its left edge at cursor_x, centred on center_y.
 *
 * Poses and lights it through glyph_pose with the context's light; the
 * advance is the unyawed width.
 * @param cx The emission context.
 * @param slot The glyph's cache slot.
 * @param cursor_x The left edge, in overlay units.
 * @param center_y The vertical centre, in overlay units.
 * @param scale Overlay units per model unit.
 * @param dy_model A vertical shift in model units (the colon dots).
 * @param wob_tick The wobble tick, or below 0 for an upright glyph.
 * @param gi The glyph's index in its string.
 * @param base The glyph colour, 0xRRGGBB.
 * @return The glyph width in overlay units, or -1 when the slot is not
 *         cached. */
static float emit_glyph(emit_ctx_t* cx, int slot, float cursor_x, float center_y,
                        float scale, float dy_model, int wob_tick, int gi, unsigned base) {
    if (slot < 0 || slot >= NGLYPH || !s_set.glyph[slot].valid) return -1.0f;
    const glyph_cache_t* g = &s_set.glyph[slot];
    const glyph_place_t at = { cursor_x, center_y, scale, dy_model };
    const int vcount = glyph_pose(g, &cx->lt, wob_tick, gi, &at, base, cx->gx, cx->gy, cx->gz, cx->frgba);
    push_glyph(cx, g, vcount);
    return (float)(g->vxmax - g->vxmin) * scale;
}

/** @brief Emits one string's glyphs into the context's buffer.
 *
 * A space, an uncached character and a character with no slot advance by
 * TXT_SPACE; a colon is two scaled periods; a digit is centred in the
 * tabular advance.
 * @param cx The emission context.
 * @param str The string.
 * @param origin_x The left edge, in overlay units.
 * @param origin_y The top edge, in overlay units.
 * @param cap_h The cap height, in overlay units.
 * @param base_rgb The glyph colour, 0xRRGGBB.
 * @param wob_phase The selected-row yaw wobble phase (a per-glyph phase
 *        ripple, like the PSX pause menu), or below 0 for upright glyphs.
 * @return The count of vertices this string added; 0 when the cache is not
 *         ready or the context has no buffer. */
static int emit_string(emit_ctx_t* cx, const char* str,
                       float origin_x, float origin_y, float cap_h,
                       unsigned base_rgb, int wob_phase) {
    if (!s_set.ready || !cx->out || cx->cap <= 0 || !str) return 0;
    const int n_before = cx->n;
    const float scale    = cap_h / FONT.model_h;
    const float center_y = origin_y + cap_h * 0.5f;
    const int   period   = glyph_slot('.');
    float cursor = origin_x;
    int gi = 0;   /* non-space glyph index - drives the wobble ripple */

    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        if (c == ':' && glyph_slot(':') < 0) {           /* faked colon: two dots */
            float w = emit_glyph(cx, period, cursor, center_y, scale * FONT.colon_scale,
                                 FONT.colon_recenter + FONT.colon_spread, wob_phase, gi, base_rgb);
            if (w < 0.0f) { cursor += TXT_SPACE; continue; }
            emit_glyph(cx, period, cursor, center_y, scale * FONT.colon_scale,
                       FONT.colon_recenter - FONT.colon_spread, wob_phase, gi, base_rgb);
            cursor += w + TXT_GAP; gi++;
            continue;
        }
        int slot = glyph_slot(c);
        if (slot < 0) { cursor += TXT_SPACE; continue; }
        float adv, inset;
        glyph_metrics(slot, scale, &adv, &inset);
        if (adv < 0.0f) { cursor += TXT_SPACE; continue; }  /* not cached -> skip */
        if (emit_glyph(cx, slot, cursor + inset, center_y, scale, 0.0f, wob_phase, gi,
                       base_rgb) < 0.0f) {
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

/* The per-tick builders below fill the cache; this reads the snapshot only. */
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

/** @brief Prints one stderr line when a consumer's published vertex count changes, under OPENPETE_TEXT_GLYPH_CENSUS.
 *
 * The line carries the vertex count, how many of them are glyph faces (the
 * rest is frame) and the snapshot's source (none, ram, disc). A check
 * harness reads it to tell a drawn font from an absent one. The variable is
 * latched on the first call; any value but 0 enables it.
 * @param consumer 0 perf, 1 menu, 2 osd, 3 version.
 * @param nverts The vertex count published.
 * @param glyph_verts Of those, the glyph-face vertices. */
static void glyph_census(int consumer, int nverts, int glyph_verts) {
    static int s_on = -1;
    static int s_last[4] = { -1, -1, -1, -1 };
    if (s_on < 0) { const char* e = getenv("OPENPETE_TEXT_GLYPH_CENSUS"); s_on = (e && *e && *e != '0') ? 1 : 0; }
    if (!s_on || s_last[consumer] == nverts) return;
    s_last[consumer] = nverts;
    static const char* const name[4] = { "perf", "menu", "osd", "version" };
    static const char* const src[3]  = { "none", "ram", "disc" };
    fprintf(stderr, "[text-glyph vbl=%u] consumer=%s verts=%d glyph_verts=%d source=%s\n",
            openpete_feed_key_now(), name[consumer], nverts, glyph_verts, src[s_set.source]);
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
 * width would resize the renderer's supersample target that often - every
 * resize idles the device. The slack is absorbed by per-line alignment
 * instead, so a right-anchored block still sits flush against the edge. */
#define PERF_REGION_W        0.30f
#define PERF_REGION_W_DETAIL 0.45f

static openpete_hud_vert s_perf_buf[PERF_OVL_MAX];

/* What the buffer currently holds. This runs once per tick, and the readout
 * only moves twice a second, so re-emitting every tick would spend the glyph
 * builder's whole cost ~15× over for identical geometry - on the thread that
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
     * anchor ledger's claim - one owner, no third place to disagree. */
    const float region_w = (level >= OPENPETE_PERF_DETAIL) ? PERF_REGION_W_DETAIL
                                                           : PERF_REGION_W;
    const float region_h = 2.0f * PERF_ORIGIN_Y + PERF_CAP_H
                         + (float)(nl > 0 ? nl - 1 : 0) * PERF_LINE_STEP;
    openpete_native_set_overlay_extent(region_w, region_h);

    /* Align EACH LINE, not the block: the lines differ in length, and aligning
     * the block alone would leave a right-anchored readout with a ragged right
     * edge against the screen edge it is hugging. */
    /* Right-aligned and centred text derives its origin from the MEASURED
     * string, so the origin moves whenever the readout does - and the readout
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
 * The separators of an untagged describe string ("v0.3.0-12-gabc1234") are
 * drawn with the synthesized hyphen bar when the font has no hyphen model. */
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
 * Advanced at the game's LOGIC rate - every other call, since this runs once
 * per sim tick (paced per VBL, ~59.826 Hz) while the PSX bumps its rotation
 * once per DRAWN frame (draw.c ++D_800758B8). Speculative and replayed ticks
 * sit it out: those are rolled back or re-executed, and letting them advance
 * makes the shimmer sprint through a rewind.
 *
 * The phase free-runs, independent of the menu's wobble tick, so each menu
 * open starts the glint at a different offset; tying it to the menu would
 * freeze the glint on every toast. */
static unsigned s_shimmer_phase = 0;

void text_moby_native_shimmer_tick(void) {
    if (openpete_speculative || openpete_replaying) return;
    static unsigned s_call = 0;
    if ((s_call++ & 1u) == 0u) s_shimmer_phase++;
}

unsigned text_moby_native_shimmer_phase(void) { return s_shimmer_phase; }

void text_moby_native_layers_tick(CPUState* cpu) {
    text_moby_native_shimmer_tick();
    text_moby_native_perf_overlay(cpu);
    text_moby_native_menu(cpu);
    text_moby_native_osd(cpu);
    text_moby_native_version(cpu);
}

/** @brief Writes SpecularUpdate's rotating light direction at a phase: (cos a * 0.968, sin a * 1.549, -0.25) normalized, a = 3 * phase / 256 turn.
 *
 * Reads only its arguments.
 * @param phase The phase, in game ticks.
 * @param dir Receives the unit direction. */
static void glyph_specular_dir(unsigned phase, float dir[3]) {
    const unsigned sp = (phase * 3u) & 0xFFu;
    const float sth = (float)sp * (6.2831853f / 256.0f);
    float Lx = cosf(sth) * 0.968f;   /* COSINE_8 * 1983 >> 11 */
    float Ly = sinf(sth) * 1.549f;   /* SINE_8   * 6345 >> 12 */
    float Lz = -0.25f;               /* -1024 / 4096 (small fixed into-screen) */
    float Ll = sqrtf(Lx*Lx + Ly*Ly + Lz*Lz); if (Ll < 1e-3f) Ll = 1.0f;
    dir[0] = Lx/Ll; dir[1] = Ly/Ll; dir[2] = Lz/Ll;
}

/** @brief Installs SpecularUpdate's rotating light as the emission's light, with signed shading.
 *
 * The light direction is (cos a * 0.968, sin a * 1.549, -0.25) normalized,
 * a = 3 * phase / 256 turn: the ellipse of SpecularUpdate(3), which steps
 * its angle by 3 per game tick. Signed shading darkens faces turned away
 * from the light below the base colour.
 * @param cx The emission context.
 * @param phase The shimmer phase (text_moby_native_shimmer_phase). */
static void install_specular_light(emit_ctx_t* cx, unsigned phase) {
    glyph_specular_dir(phase, cx->lt.dir);
    cx->lt.signed_nl = 1;   /* metal contrast: shadowed faces darken below base */
}

/** @brief Prints the FNV-1a hash of a published vertex buffer to stderr, under OPENPETE_TEXT_GLYPH_CENSUS=2 or above.
 *
 * The menu and the OSD call it on every shimmer phase that is a multiple of
 * 64, and glyph_hash_probe once, with a fixed probe string through the
 * fixed-light emitter, on the first menu build after the snapshot is ready.
 * Two builds that
 * print the same lines emit the same vertices. The variable is latched on
 * the first call.
 * @param what The buffer's name in the line.
 * @param v The vertices.
 * @param n The vertex count. */
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
/** @brief Emits a fixed probe string through the fixed-light emitter and prints its vertex hash, once.
 *
 * Called by the menu builder while the menu is open; runs on the first call
 * after the glyph snapshot is ready and does nothing on later calls or before
 * that. The print is gated by glyph_hash_census. */
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
#define MENU_TXT_MAX    OPENPETE_MENU_GLYPH_MAX
/* Mid-tone gold BASE - the signed light swings faces both ways around it (lit ->
 * white-gold, shadowed -> dark), so base is the average, not the floor. The game's
 * HUD/menu gold is R==G with a low B (measured (96,96,0)->(214,214,118)); R>G is
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
 * per-vertex and interpolates along the perimeter -> a smooth travelling
 * highlight, not a per-edge on/off. Mid-gold BASE (R==G, low B - same rationale
 * as the text tint) so the signed light swings it dark<->bright-gold either side. */
#define MENU_BORDER_R   190
#define MENU_BORDER_G   190
#define MENU_BORDER_B    80
#define MENU_BORDER_BT   0.0035f  /* frame thickness, overlay units */
#define MENU_BORDER_SAMP 12       /* perimeter samples per edge (smooth glint) */
#define MENU_BORDER_VERTS (4 * MENU_BORDER_SAMP * 6)  /* two tris per sample step */

/** @brief Computes the lit colour of a border vertex from its outward normal.
 *
 * The term N.L uses N = (nx, ny, 0) against the context's light, clamped to
 * -1..1, and shades the gold border base through lit_colour. The alpha byte
 * is OPENPETE_HUD_ALPHA_NO_OUTLINE, so an outlined surface rings its glyphs
 * and not its frame.
 * @param cx The emission context supplying the light.
 * @param nx The normal's X component.
 * @param ny The normal's Y component.
 * @return The colour as 0xAABBGGRR. */
static uint32_t border_lit(const emit_ctx_t* cx, float nx, float ny) {
    float t = nx * cx->lt.dir[0] + ny * cx->lt.dir[1];   /* N·L, N=(nx,ny,0) unit; signed like the glyphs */
    if (t >  1.0f) t =  1.0f; else if (t < -1.0f) t = -1.0f;
    return (lit_colour(&cx->lt, MENU_BORDER_R, MENU_BORDER_G, MENU_BORDER_B, t) & 0x00FFFFFFu) |
           (OPENPETE_HUD_ALPHA_NO_OUTLINE << 24);
}

/** @brief Emits a lit frame around a rectangle, lit by the emission context's light.
 *
 * Call after the builder has installed the rotating specular. The frame is
 * four straight edge strips: the outer edge lies on the rectangle, the inner
 * edge is a perpendicular inset by the thickness, so each strip is a
 * parallel-sided rectangle. Each strip is split into MENU_BORDER_SAMP
 * columns; a column's colour comes from the outward radial normal at its
 * outer point, so the glint travels along the edge. Adjacent strips meet at
 * a shared corner whose radial normal is identical from either side. A
 * column that does not fit in cap is dropped.
 * @param ecx The emission context supplying the light.
 * @param x0 The left edge.
 * @param y0 The top edge.
 * @param x1 The right edge.
 * @param y1 The bottom edge.
 * @param thickness The frame thickness, in overlay units.
 * @param out The vertex buffer.
 * @param cap The buffer's free capacity, in vertices.
 * @return The count of vertices written. */
static int emit_lit_border(const emit_ctx_t* ecx,
                           float x0, float y0, float x1, float y1, float thickness,
                           openpete_hud_vert* out, int cap) {
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float bt = thickness, z = txt_depth_vz(TXT_DEPTH_MID);
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

/** @brief Returns a string's advance width at a cap height, in overlay units.
 *
 * Mirrors emit_string's cursor accumulation: glyph_metrics' advance plus
 * TXT_GAP per glyph, TXT_SPACE for a space or an uncached character, and
 * the scaled period's width plus TXT_GAP for a colon. The width is linear
 * in cap_h and includes the trailing gap. Used to right-align and centre
 * menu, readout and version text and to size OSD boxes.
 * @param str The string, or NULL.
 * @param cap_h The cap height, in overlay units.
 * @return The width; 0 when the cache is not ready or str is NULL. */
static float measure_string(const char* str, float cap_h) {
    if (!s_set.ready || !str) return 0.0f;
    const float scale = cap_h / FONT.model_h;
    float cursor = 0.0f;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == ' ') { cursor += TXT_SPACE; continue; }
        if (c == ':' && glyph_slot(':') < 0) {
            int slot = glyph_slot('.');
            if (slot < 0 || !s_set.glyph[slot].valid) { cursor += TXT_SPACE; continue; }
            cursor += (float)(s_set.glyph[slot].vxmax - s_set.glyph[slot].vxmin)
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

/** @brief Emits the settings menu's close glyph at vertex v0 and positions it by its drawn extent.
 *
 * The layout square is an advance box: it carries the trailing inter-glyph
 * gap and, for a proportional glyph, leading space the X does not fill, so
 * placing the box leaves unequal margins above and right of the drawn
 * pixels. The glyph is emitted at the square, its drawn extent is read back
 * off the emitted vertices, and the whole run is then shifted in x until the
 * gap from the panel's right border to the rightmost ink equals the gap from
 * the panel's top border to the topmost ink. The resulting rect is published
 * so the mouse consumer tests the pixels that were drawn.
 *
 * Under the pointer the X takes the same per-glyph yaw wobble the row under
 * the cursor takes. The anchor and the published rect are measured from the
 * upright glyph and the wobbled one is emitted in its place, so neither the
 * drawn position nor the hit region moves with the animation.
 * @param cx The menu's emission context.
 * @param v0 The context's vertex count before the glyph (cx->n).
 * @param L The menu layout (close square and panel rectangle).
 * @param wob_phase The wobble phase while the pointer is on the glyph, else
 *        below 0. */
static void menu_close_anchor(emit_ctx_t* cx, int v0,
                              const openpete_menu_layout_t* L, int wob_phase) {
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
    if (wob_phase >= 0) {
        cx->n = v0;
        emit_string(cx, "X", L->close_x + dx, L->close_y, L->close_cap_h,
                    MENU_TXT_TINT, wob_phase);
    } else {
        for (int i = v0; i < cx->n; i++) cx->out[i].x += dx;
    }
    openpete_menu_note_close_rect(x0 + dx, y0, x1 + dx, y1);
}

/** @brief Returns the largest cap height, at most cap, at which a string fits a width.
 *
 * measure_string is linear in the cap height, so one proportional shrink is
 * exact.
 * @param str The text.
 * @param cap The cap height ceiling, in overlay units.
 * @param avail The width available, in overlay units.
 * @return cap when the string fits or measures zero, else the shrunk cap. */
static float fit_cap(const char* str, float cap, float avail) {
    const float w = measure_string(str, cap);
    return (w > avail && w > 0.0f) ? cap * (avail / w) : cap;
}

/** @brief Emits a string centred on a point.
 *
 * @param cx The emission context.
 * @param str The text; an empty string emits nothing.
 * @param xc Centre x, in overlay units.
 * @param yc Centre y, in overlay units.
 * @param cap The cap height, in overlay units.
 * @param rgb The tint.
 * @param wob The wobble phase, or below 0 for none. */
static void emit_centred(emit_ctx_t* cx, const char* str, float xc, float yc,
                         float cap, unsigned rgb, int wob) {
    const float w = measure_string(str, cap);
    emit_string(cx, str, xc - 0.5f * w, yc - cap * 0.5f, cap, rgb, wob);
}

/* draw_menu_overlay (native_render.c) draws the published text over the panel. */
void text_moby_native_menu(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    text_moby_native_cache(cpu);
    if (!openpete_menu_is_open() || !text_moby_native_ready()) {
        openpete_native_set_menu_glyphs(NULL, 0);
        glyph_census(1, 0, 0);
        return;
    }
    /* Advance the wobble at the game's LOGIC rate (~29.913 Hz). This builder runs
     * once per sim tick - which openpete paces per-VBL (~59.826 Hz) - but the PSX
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
    /* The frame is emitted last but its room is reserved here, so text that
     * outgrows the buffer loses its own tail and never the frame. */
    emit_ctx_init(&cx, s_menu_buf, MENU_TXT_MAX - MENU_BORDER_VERTS);

    install_specular_light(&cx, text_moby_native_shimmer_phase());

    /* Pause-menu alignment: title centred; labels RIGHT-aligned to L.label_x;
     * values LEFT-aligned at L.value_x (the two columns meet at a central split). */
    const float pcx = 0.5f * (L.panel_x0 + L.panel_x1);   /* panel centre x */
    emit_centred(&cx, openpete_menu_title(), pcx, L.title_y, L.title_cap_h,
                 MENU_TITLE_TINT, -1);
    /* Uniform row font: shrink EVERY row (labels + values; the SETTINGS title
     * keeps its own cap) to the size the widest value needs to fit its column
     * - "FULLSCREEN" - so no single row reads smaller than its neighbours.
     * Measured from the FIXED reference string, not the live values, so the
     * size never breathes as a value toggles (WINDOWED <-> FULLSCREEN).
     * Rows keep their spacing (L.row_y); only the glyph size changes. */
    const float vavail = (L.panel_x1 - 0.015f) - L.value_x;
    const float cavail = (L.panel_x1 - 0.015f) - (L.panel_x0 + 0.015f);
    const float row_cap = fit_cap("FULLSCREEN", L.row_cap_h, vavail);
    /* The bottom-anchored row's y is computed from this height; publish it
     * before the rows are placed so the anchor uses what was drawn. */
    openpete_menu_note_row_cap(row_cap);
    openpete_menu_get_layout(&L);
    for (int r = 0; r < L.row_count; r++) {
        const int wob = (r == cursor) ? phase : -1;   /* only the selected row wobbles */
        char vb[16];
        const char* label = openpete_menu_row_label(r);
        /* Action rows (EXIT GAME) have no value: centre the label on the panel
         * so it reads as a button rather than a setting whose value went
         * missing. Shrunk if the armed text outgrows the panel width. */
        if (openpete_menu_row_centered(r)) {
            emit_centred(&cx, label, pcx, L.row_y[r],
                         fit_cap(label, row_cap, cavail), MENU_TXT_TINT, wob);
            continue;
        }
        const float lw = measure_string(label, row_cap);
        emit_string(&cx, label, L.label_x - lw,
                    L.row_y[r] - row_cap * 0.5f, row_cap, MENU_TXT_TINT, wob);
        /* Safety net for any future value wider than the reference: shrink
         * that one value further so it still ends before the panel border. */
        const char* value = openpete_menu_row_value(r, vb, sizeof vb);
        const float vcap = fit_cap(value, row_cap, vavail);
        emit_string(&cx, value, L.value_x, L.row_y[r] - vcap * 0.5f, vcap,
                    MENU_TXT_TINT, wob);
    }
    /* Hint line: what the cursor row changes, centred, shrunk to the panel
     * width, without the wobble so it reads as a caption of the row. */
    const char* hint = openpete_menu_row_hint(cursor);
    emit_centred(&cx, hint, pcx, L.hint_y, fit_cap(hint, L.hint_cap_h, cavail),
                 MENU_TXT_TINT, -1);
    /* Close glyph: not a row, so the keyboard cursor never reaches it; the
     * pointer resting on it wobbles it like the row under the cursor. */
    menu_close_anchor(&cx, cx.n, &L,
                      openpete_menu_close_hovered() ? phase : -1);
    /* Gold frame - same rotating light as the rows above (cx still carries it). */
    cx.n += emit_lit_border(&cx, L.panel_x0, L.panel_y0, L.panel_x1, L.panel_y1,
                            MENU_BORDER_BT, s_menu_buf + cx.n, MENU_TXT_MAX - cx.n);
    if (cx.dropped) {
        static int s_warned;
        if (!s_warned) {
            s_warned = 1;
            OPL_WARN_F("text", "settings menu text exceeded %d verts: %d triangle(s) "
                       "dropped", MENU_TXT_MAX - MENU_BORDER_VERTS, cx.dropped);
        }
    }
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
 * Geometry is emitted in TARGET-LOCAL overlay space - slot i's box starts at
 * y = i * slot_h - and the slot table tells the compositor which toast landed
 * where and how much of its slot it filled. Text and table are published in
 * one call, so the compositor can never resolve an id through a table that
 * describes different geometry. */
#define OSD_TXT_MAX     24576   /* must match OSD_GLYPH_MAX (native_render.c) */
#define OSD_CAP_H       0.030f  /* toast glyph height, overlay units          */
#define OSD_PAD_X       0.022f
#define OSD_PAD_Y       0.014f
#define OSD_BORDER_BT   0.0035f /* frame thickness - the menu's, so the two read as one set */
#define OSD_TINT        0x96962Au   /* the menu's mid yellow-gold */

static openpete_hud_vert s_osd_buf[OSD_TXT_MAX];

/** @brief Copies a toast's text and drops trailing characters until it fits a width.
 *
 * Truncation keeps a toast inside its slot of the fixed grid; an overrun
 * would put its glyphs into the neighbouring slot's texels, which the
 * compositor samples as part of a different toast.
 * @param src The text, or NULL for an empty string.
 * @param cap_h The cap height, in overlay units.
 * @param avail The width available, in overlay units.
 * @param dst The destination.
 * @param dstcap The destination's size in bytes; a longer text is cut to it
 *        first. */
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

/* ---- a fitted title from a glyph set read from the launcher cache ------- */

/** @brief Maps a letter or digit to its glyph slot, independent of any font's punctuation list.
 *
 * @param c The character.
 * @return The slot, or -1 for any other character. */
static int title_slot(char c) {
    if (c >= '0' && c <= '9') return SLOT_DIGIT + (c - '0');
    if (c >= 'A' && c <= 'Z') return SLOT_LETTER + (c - 'A');
    if (c >= 'a' && c <= 'z') return SLOT_LETTER + (c - 'a');
    return -1;
}

/** @brief Returns the slot of a title character that the set holds, or -1.
 *
 * @param G The glyph set.
 * @param c The character.
 * @return The slot, or -1 when the character is no letter or digit or the set lacks its glyph. */
static int title_glyph(const text_glyph_set* G, char c) {
    const int slot = title_slot(c);
    return slot >= 0 && G->glyph[slot].valid ? slot : -1;
}

/** @brief Returns the index of a glyph's front face: the face whose unyawed normal points most toward the reader (+Z).
 *
 * @param g The glyph.
 * @return The face index, 0 for a glyph without faces. */
static int glyph_front_face(const glyph_cache_t* g) {
    int best = 0;
    float best_z = -2.0f;
    for (int f = 0; f < g->fcount; f++) {
        const float x = g->fn[f][0], y = g->fn[f][1], z = g->fn[f][2];
        const float l = sqrtf(x * x + y * y + z * z);
        if (l > 0.0f && z / l > best_z) { best_z = z / l; best = f; }
    }
    return best;
}

/** @brief Records a posed glyph's rect over its vertices and its front face's colour.
 *
 * @param cx The emission context holding the posed glyph.
 * @param g The glyph.
 * @param vcount The vertex count posed.
 * @param out Receives the record. */
static void title_posed(const emit_ctx_t* cx, const glyph_cache_t* g, int vcount, text_glyph_posed* out) {
    out->x0 = out->x1 = cx->gx[0];
    out->y0 = out->y1 = cx->gy[0];
    for (int i = 1; i < vcount; i++) {
        if (cx->gx[i] < out->x0) out->x0 = cx->gx[i];
        if (cx->gx[i] > out->x1) out->x1 = cx->gx[i];
        if (cx->gy[i] < out->y0) out->y0 = cx->gy[i];
        if (cx->gy[i] > out->y1) out->y1 = cx->gy[i];
    }
    out->face = glyph_front_face(g);
    out->rgba = cx->frgba[out->face];
}

int text_glyph_set_emit_title(const text_glyph_set* G, const struct game_font* font, const char* str,
                              const text_glyph_fit* fit, openpete_hud_vert* out, int cap, float* cap_h,
                              text_glyph_posed* posed, int posed_cap) {
    *cap_h = 0.0f;
    if (!G || !G->ready || !font || font->model_h <= 0.0f || !str || !out || cap <= 0) return 0;
    float model_w = 0.0f;
    int n = 0;
    for (const char* c = str; *c; c++) {
        const int slot = title_glyph(G, *c);
        if (slot < 0) continue;
        model_w += (float)(G->glyph[slot].vxmax - G->glyph[slot].vxmin);
        n++;
    }
    if (n == 0 || model_w <= 0.0f) return 0;
    const float scale = (fit->width - (float)(n - 1) * fit->gap) / model_w;
    *cap_h = scale * font->model_h;
    emit_ctx_t cx;
    memset(&cx, 0, sizeof cx);
    cx.out = out;
    cx.cap = cap;
    glyph_light_init(&cx.lt, font);
    if (fit->tick >= 0) install_specular_light(&cx, (unsigned)fit->tick);
    float cursor = fit->x0;
    int gi = 0;
    for (const char* c = str; *c; c++) {
        const int slot = title_glyph(G, *c);
        if (slot < 0) continue;
        const glyph_cache_t* g = &G->glyph[slot];
        const glyph_place_t at = { cursor, fit->cy, scale, 0.0f };
        const int vcount = glyph_pose(g, &cx.lt, fit->tick, gi, &at, MENU_TITLE_TINT, cx.gx, cx.gy, cx.gz, cx.frgba);
        if (posed && gi < posed_cap) title_posed(&cx, g, vcount, &posed[gi]);
        push_glyph(&cx, g, vcount);
        cursor += (float)(g->vxmax - g->vxmin) * scale + fit->gap;
        gi++;
    }
    return cx.n;
}
