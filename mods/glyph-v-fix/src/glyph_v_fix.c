/**
 * @file glyph_v_fix.c
 * @brief Restore the missing end-cap face on the 3D letter 'V'.
 *
 * The game draws its gold 3D text - dragon-rescue names, "DEMO MODE",
 * the pause and inventory menus, the gem and life counters - with one
 * moby class per character, 'A'-'Z' at 426+(c-'A'). Each is a
 * SimpleModel: header byte0 = vertex count, byte1 = face count,
 * +0x04 -> vertex stream (3 bytes/vertex, packed 9-bit signed VZ/VX/VY),
 * +0x0C -> face stream (8 bytes/face). A face's w0 packs four vertex
 * indices, i0 at bit 23, i1 at 16, i2 at 9, i3 at 2, with bit 0 set; the
 * face is a triangle when i2 == i3 and otherwise a quad split into
 * (i0,i1,i2) and (i1,i3,i2). w1's high three bytes are the flat-shading
 * normal Nz/Nx/Ny, at a length of 128.
 *
 * The 'V' model (class 447) carries 18 vertices and 17 faces: six front,
 * six back, and five side walls. The right arm has all three of its
 * walls - outer edge, top end cap, inner edge - and the left arm has
 * only two. The quad closing the left arm's top, over the four vertices
 * the outer and inner walls already share, is absent, so the top-left of
 * every 'V' opens into the glyph's unlit interior.
 *
 * The stream cannot grow where it lies: the 'W' model header starts at
 * the byte after the last 'V' face. So the 17 faces are copied to the
 * mod arena, the missing quad is appended, and the model header's face
 * count and stream pointer are pointed at the copy. Nothing is
 * hardcoded from the asset: the faces are copied from whatever the game
 * loaded and the new face's normal is computed from the four vertices.
 *
 * The font asset is not permanently resident - it is evicted and
 * reloaded across level and cutscene loads - so the patch is applied
 * from a per-tick hook that re-fires whenever a freshly loaded model
 * appears in the row, and is a no-op once the header already points at
 * the arena.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>

#include <stdint.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

#define GLYPH_V_CLASS   447u  /**< 426 + ('V' - 'A'). */
#define STOCK_VERTS      18u  /**< Vertex count of the stock 'V' model. */
#define STOCK_FACES      17u  /**< Face count of the stock 'V' model: one short. */
#define PATCHED_FACES    18u
#define FACE_BYTES        8u

/**
 * @name The missing quad
 * The four vertices are already in the model, shared with the two walls
 * that meet at the arm's top: V0 and V1 are the inner corner, front and
 * back, and V5 and V4 the outer corner. The order below is the winding
 * the rest of the model uses, under which a face's stored normal is
 * -((P1-P0) x (P2-P0)) - the convention the stock walls satisfy.
 * @{
 */
#define CAP_I0 0u
#define CAP_I1 1u
#define CAP_I2 5u
#define CAP_I3 4u
/** @} */

static int s_enabled;
static int s_dump;
static int s_dumped;
static int s_patched;      /**< Installs performed, for the status line. */
static uint32_t s_park;    /**< Arena vaddr of the 18-face stream. */
static void*    s_park_host;

static uint32_t rd32(uint32_t a) { return op_read_u32(g_api, a); }
static uint8_t  rd8(uint32_t a)  { return op_read_u8(g_api, a); }

/** @brief Resolve g_Models[cls] to a readable vaddr, or 0 if no SimpleModel is loaded. */
static uint32_t glyph_model(unsigned cls)
{
    uint32_t m = rd32(OP_GADDR_g_Models + cls * 4u);
    if (!m) return 0u;
    if (m < 0x80000000u) m |= 0x80000000u;   /* rows hold physical addresses */
    /* Only a model inside the 2 MB of main RAM is one the game loaded; the
     * row can otherwise hold this mod's own arena address, or a stale
     * value left by an eviction. */
    if (m < 0x80010000u || m >= 0x80200000u - 0x20u) return 0u;
    if ((int32_t)rd32(m) >= 0) return 0u;    /* m_NumAnimations >= 0: not a SimpleModel */
    return m;
}

/** @brief Read vertex @p i of the model whose vertex stream is at @p vbase. */
static void read_vertex(uint32_t vbase, unsigned i, int32_t out[3])
{
    uint32_t p = vbase + i * 3u;
    uint32_t w = (uint32_t)rd8(p) | ((uint32_t)rd8(p + 1) << 8) | ((uint32_t)rd8(p + 2) << 16);
    out[2] = ((int32_t)(w << 24)) >> 23;   /* VZ */
    out[0] = ((int32_t)(w << 16)) >> 23;   /* VX */
    out[1] = ((int32_t)(w <<  8)) >> 23;   /* VY */
}

/** @brief Log the decoded vertex and face streams of one glyph model. */
static void dump_model(uint32_t model)
{
    uint32_t vbase = rd32(model + 4u), faces = rd32(model + 0xCu);
    int vcount = rd8(model), fcount = rd8(model + 1u), i;

    if (vbase < 0x80000000u) vbase |= 0x80000000u;
    if (faces < 0x80000000u) faces |= 0x80000000u;
    g_api->log(g_self, OP_MOD_LOG_INFO, "'V' model=%08x verts=%d faces=%d vbase=%08x fbase=%08x",
               model, vcount, fcount, vbase, faces);
    for (i = 0; i < vcount; i++) {
        int32_t v[3];
        read_vertex(vbase, (unsigned)i, v);
        g_api->log(g_self, OP_MOD_LOG_INFO, "  V%02d x=%4d y=%4d z=%4d", i, v[0], v[1], v[2]);
    }
    for (i = 0; i < fcount; i++) {
        uint32_t w0 = rd32(faces + (uint32_t)i * FACE_BYTES);
        uint32_t w1 = rd32(faces + (uint32_t)i * FACE_BYTES + 4u);
        int i2 = (int)((w0 >> 7) & 0x1FCu) >> 2, i3 = (int)(w0 & 0x1FCu) >> 2;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "  F%02d w0=%08x w1=%08x i=[%2d %2d %2d %2d] %s n=(%4d %4d %4d)", i, w0, w1,
                   (int)((w0 >> 21) & 0x1FCu) >> 2, (int)((w0 >> 14) & 0x1FCu) >> 2, i2, i3,
                   (i2 == i3) ? "tri " : "quad", (int)(int8_t)(w1 >> 16),
                   (int)(int8_t)(w1 >> 8), (int)(int8_t)(w1 >> 24));
    }
}

/**
 * @brief Build the end-cap face words for the model at @p vbase.
 *
 * The normal is derived from the quad's own corners rather than assumed,
 * and scaled to the length of 128 the stock faces use. Its Z component
 * is zero to within rounding - the cap spans the full extrusion depth on
 * both of its edges - and is stored as zero, as the stock side walls do.
 */
static void build_cap(uint32_t vbase, uint32_t* w0, uint32_t* w1)
{
    int32_t p0[3], p1[3], p2[3];
    int64_t ax, ay, az, bx, by, bz, nx, ny, nz, len2;
    int32_t scale;

    read_vertex(vbase, CAP_I0, p0);
    read_vertex(vbase, CAP_I1, p1);
    read_vertex(vbase, CAP_I2, p2);
    ax = p1[0] - p0[0]; ay = p1[1] - p0[1]; az = p1[2] - p0[2];
    bx = p2[0] - p0[0]; by = p2[1] - p0[1]; bz = p2[2] - p0[2];
    /* -((P1-P0) x (P2-P0)): the sign the stock walls' stored normals use. */
    nx = -(ay * bz - az * by);
    ny = -(az * bx - ax * bz);
    nz = -(ax * by - ay * bx);

    len2 = nx * nx + ny * ny + nz * nz;
    scale = 1;
    if (len2 > 0) {
        /* integer sqrt of len2 */
        int64_t r = 0, bit = 1ll << 40;
        while (bit > len2) bit >>= 2;
        while (bit) {
            if (len2 >= r + bit) { len2 -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }
        scale = (int32_t)(r > 0 ? r : 1);
    }
    nx = (nx * 128 + (nx >= 0 ? scale / 2 : -scale / 2)) / scale;
    ny = (ny * 128 + (ny >= 0 ? scale / 2 : -scale / 2)) / scale;

    *w0 = (CAP_I0 << 23) | (CAP_I1 << 16) | (CAP_I2 << 9) | (CAP_I3 << 2) | 1u;
    *w1 = ((uint32_t)(nx & 0xFFu) << 16) | ((uint32_t)(ny & 0xFFu) << 8);
}

/**
 * @brief Copy the stock faces into the arena, append the cap, and point
 *        the model header at the result.
 *
 * Runs only on a model whose vertex and face counts are the stock 'V's,
 * so a row holding anything else - a partly loaded asset, or another
 * mod's replacement - is left alone.
 */
static void patch_model(uint32_t model)
{
    uint32_t vbase = rd32(model + 4u), faces = rd32(model + 0xCu);
    uint32_t w0, w1, i;
    uint8_t* park = (uint8_t*)s_park_host;

    if (vbase < 0x80000000u) vbase |= 0x80000000u;
    if (faces < 0x80000000u) faces |= 0x80000000u;

    for (i = 0; i < STOCK_FACES * FACE_BYTES; i += 4u) {
        uint32_t w = rd32(faces + i);
        park[i] = (uint8_t)w; park[i + 1] = (uint8_t)(w >> 8);
        park[i + 2] = (uint8_t)(w >> 16); park[i + 3] = (uint8_t)(w >> 24);
    }
    build_cap(vbase, &w0, &w1);
    i = STOCK_FACES * FACE_BYTES;
    park[i] = (uint8_t)w0; park[i + 1] = (uint8_t)(w0 >> 8);
    park[i + 2] = (uint8_t)(w0 >> 16); park[i + 3] = (uint8_t)(w0 >> 24);
    park[i + 4] = (uint8_t)w1; park[i + 5] = (uint8_t)(w1 >> 8);
    park[i + 6] = (uint8_t)(w1 >> 16); park[i + 7] = (uint8_t)(w1 >> 24);

    op_write_u32(g_api, model + 0xCu, s_park);
    op_write_u8(g_api, model + 1u, (uint8_t)PATCHED_FACES);
    s_patched++;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "'V' model %08x: %u faces -> %u, stream %08x -> %08x (cap w0=%08x w1=%08x)",
               model, STOCK_FACES, PATCHED_FACES, faces, s_park, w0, w1);
    g_api->ui_status(g_self, "end-cap installed in %d model load(s)", s_patched);
}

/** @brief Per-tick point at which the font asset may be resident. */
static void on_specular_render(CPUState* cpu)
{
    uint32_t model = glyph_model(GLYPH_V_CLASS);

    if (model && s_dump && !s_dumped) { s_dumped = 1; dump_model(model); }
    if (model && s_enabled && s_park &&
        rd8(model) == STOCK_VERTS && rd8(model + 1u) == STOCK_FACES &&
        rd32(model + 0xCu) != s_park)
        patch_model(model);
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    g_api = api; g_self = self;
    s_enabled = api->config_bool(self, "glyph_v.enabled", 1);
    s_dump    = api->config_bool(self, "debug.dump", 0);

    s_park = api->guest_alloc(self, PATCHED_FACES * FACE_BYTES, 4, 0, &s_park_host);
    if (!s_park || !s_park_host) {
        api->log(self, OP_MOD_LOG_ERROR, "face-stream allocation failed");
        api->ui_status(self, "arena allocation failed - inactive");
        return -1;
    }
    api->ui_status(self, "waiting for the 'V' model");
    if (api->override_addr(self, OP_FNADDR_func_80020F34, on_specular_render) != 0) return -1;
    api->log(self, OP_MOD_LOG_INFO, "glyph-v-fix: park at %08x (enabled=%d)", s_park, s_enabled);
    return 0;
}
