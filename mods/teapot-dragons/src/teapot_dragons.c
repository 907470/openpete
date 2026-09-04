/**
 * @file teapot_dragons.c
 * @brief teapot-dragons: the crystal dragon statue (moby class 250) is a
 *        Utah teapot. A model-replacement mod built on the arena and the
 *        vertex-stream handle table.
 *
 * Three parts:
 *
 * 1. Entry reads assets/teapot_s1pack.bin (written by tools/gen_teapot.py),
 *    parks the model payload in the mod arena with guest_alloc, relocates
 *    the payload's absolute words to the park address, and exchanges each
 *    frame word's vertex-stream offset for a stream handle from
 *    vstream_handle. The handles are requested once, in pack order, so
 *    the engine's handle ledger replays identically every session.
 * 2. A post-hook on PatchMobyModelPointers fires after the game installs
 *    a level's models. When the class-250 row holds a stock model (neither
 *    0 nor the park), the hook copies the per-level bytes a static pack
 *    cannot know, m_Sounds and m_CollisionModels, from the stock header
 *    into the parked one and points the row at the park. The game nulls
 *    and re-installs the row on every level load and dragon-cutscene
 *    load, so the hook re-fires per level and is idempotent within one.
 * 3. A hook around LoadDragonCutscene keeps the game's own invariant
 *    that the class-250 row bounds the level's model buffer. Stage 0 of
 *    that loader sizes the cutscene read as the distance from the model
 *    buffer to that row, because the statue is the last model in the
 *    buffer; with the row pointing at the park the distance is enormous,
 *    the loader takes its "fits" path, and the read overruns the buffer
 *    into live level data. The hook shows the loader the stock row for
 *    the duration of stage 0 and puts the teapot back afterwards.
 *
 * debug.dump logs the stock model and the live statue mobys once per
 * level. It is the calibration that sized the generator, kept because a
 * model-replacement mod for another class starts by reading the same
 * numbers.
 *
 * Why a handle and not an address: the moby format's frame word holds
 * the vertex-stream address in a 21-bit field, which cannot reach the
 * arena. The engine keeps a table of stream addresses and lets a mod
 * store a small handle in the field instead; every reader of the field
 * resolves handles before use.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_game_structs.h>
#include <psx_runtime.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

#define STATUE_CLASS 250u   /**< The crystal dragon statue, last model of every level. */
#define MOBY_STRIDE  0x58u  /**< sizeof(Moby) in the level moby array. */

static int s_dump;           /**< debug.dump config. */
static int s_enabled;        /**< teapot.enabled config. */
static int s_dumped_level;   /**< g_LevelId last dumped; 0 = never. */

/**
 * @name Asset pack
 * Layout, little-endian: header | reloc_words | reloc_frames | payload.
 * reloc_words are payload offsets of absolute-vaddr words linked at
 * base 0; reloc_frames are payload offsets of frame words whose 21-bit
 * field holds a stream offset. The generator's docstring is the
 * authoritative description.
 * @{
 */
#define PACK_MAGIC 0x544F5054u   /**< 'TPOT' */

typedef struct {
    uint32_t magic, version, payload_size, n_reloc_words, n_reloc_frames,
             model_off, hp_verts, hp_faces, lp_verts, lp_faces;
} pack_header_t;

static pack_header_t s_pack;
static uint8_t*      s_file;       /**< The whole file, host memory. */
static uint8_t*      s_payload;    /**< Relocated model bytes inside s_file. */
static uint32_t      s_park;       /**< Guest vaddr of the parked payload. */
static uint32_t      s_model;      /**< Guest vaddr of the Model header. */
static uint32_t      s_stock;      /**< Row value the game installed before the teapot: the
                                    *   stock statue, the last model in the level's buffer. */
static int           s_patched_levels;
/** @} */

static uint32_t rd32(uint32_t a) { return op_read_u32(g_api, a); }
static uint8_t  rd8(uint32_t a)  { return op_read_u8(g_api, a); }

/**
 * @brief Log one animation header of the stock model, with the frame-0
 *        vertex extents and the face-block census.
 * @param ah  Guest vaddr of the animation header.
 * @param idx Animation index, for the log line.
 *
 * The vertex walk mirrors the renderer's stream decode: a literal word
 * per vertex when the previous word's flag bit is 0, a packed short
 * delta otherwise. A frame field below 0x10000 is a stream handle, not
 * an address; the game's readers resolve it and a mod cannot, so the
 * walk is skipped for such a frame.
 */
static void dump_anim(uint32_t ah, int idx)
{
    uint32_t hw = rd32(ah);
    int nframes = (int)(int16_t)(hw & 0xFFFFu), f;
    g_api->log(g_self, OP_MOD_LOG_INFO,
        "  anim%d @%08x: frames=%d colors=%u scale=%u shortshift=%u radius=%u "
        "verts=%u/%u depthscale=%u progress=%u faces=%08x colors=%08x lpfaces=%08x lpcolors=%08x",
        idx, ah, nframes, hw >> 16, rd8(ah + 5), rd8(ah + 6), rd8(ah + 7),
        rd8(ah + 8), rd8(ah + 9), rd8(ah + 0xB), rd8(ah + 0xC),
        rd32(ah + 0x14), rd32(ah + 0x18), rd32(ah + 0x1C), rd32(ah + 0x20));
    for (f = 0; f < nframes && f < 4; f++) {
        uint32_t fr = ah + 0x24u + 8u * (uint32_t)f;
        uint32_t w0 = rd32(fr), w1 = rd32(fr + 4);
        g_api->log(g_self, OP_MOD_LOG_INFO,
            "    frame%d: vtxoff=%06x coll=%u sound=%u coloroff=%u shadow=%u shortoff=%u",
            f, w0 & 0x1FFFFFu, (w0 >> 21) & 7u, w0 >> 24, w1 & 0xFFFFu,
            (w1 >> 16) & 0xFFu, w1 >> 24);
    }
    if ((rd32(ah + 0x24u) & 0x1FFFFFu) >= 0x10000u) {
        uint32_t fr = ah + 0x24u, w0 = rd32(fr), w1 = rd32(fr + 4);
        uint32_t litp = (w0 & 0x1FFFFFu) | 0x80000000u;
        uint32_t shortp = litp + (w1 >> 24) * 4u;
        int n = rd8(ah + 8), s5 = rd8(ah + 6), i;
        int vx = 0, vy = 0, vz = 0, flag = 0;
        int mn[3] = {99999, 99999, 99999}, mx[3] = {-99999, -99999, -99999};
        for (i = 0; i < n; i++) {
            if (i == 0 || flag == 0) {
                uint32_t w = rd32(litp); litp += 4;
                vz = (int32_t)w >> 21; vx = (int32_t)(w << 10) >> 21;
                vy = (int32_t)(w << 20) >> 19; flag = (int)(w & 1u);
            } else {
                int16_t d = (int16_t)(rd32(shortp) & 0xFFFFu); shortp += 2;
                vz += ((int32_t)d >> 11) << s5;
                vx -= ((int32_t)((uint32_t)(int32_t)d << 21) >> 27) << s5;
                vy -= ((int32_t)((uint32_t)(int32_t)d << 26) >> 27) << s5;
                flag = (int)((uint16_t)d & 1u);
            }
            if (vx < mn[0]) mn[0] = vx;
            if (vx > mx[0]) mx[0] = vx;
            if (vy < mn[1]) mn[1] = vy;
            if (vy > mx[1]) mx[1] = vy;
            if (vz < mn[2]) mn[2] = vz;
            if (vz > mx[2]) mx[2] = vz;
        }
        g_api->log(g_self, OP_MOD_LOG_INFO,
            "    frame0 extents: x[%d,%d] y[%d,%d] z[%d,%d]",
            mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }
    {
        uint32_t fb = rd32(ah + 0x14u);
        if (fb) {
            uint32_t bc, fp, end; int nf = 0, ntex = 0, nfresh = 0;
            fb |= 0x80000000u;
            bc = rd32(fb); fp = fb + 4u; end = fp + bc;
            /* Face record sizes by kind: sprite 0x14; fresh textured 0x18,
             * fresh gouraud 0x0C; repeat textured 0x14, repeat gouraud 8. */
            while (fp + 8u <= end && nf < 8192) {
                uint32_t w0 = rd32(fp);
                int fresh = (int32_t)w0 < 0, tex = (w0 & 2u) != 0;
                int sprite = fresh && (w0 & 4u);
                uint32_t adv = sprite ? 0x14u : fresh ? (tex ? 0x18u : 0x0Cu)
                                                     : (tex ? 0x14u : 0x08u);
                if (!sprite && !(w0 & 8u)) { nf++; ntex += tex; nfresh += fresh; }
                fp += adv;
            }
            g_api->log(g_self, OP_MOD_LOG_INFO,
                "    faceblock @%08x: %u bytes, %d faces (%d fresh, %d textured)",
                fb, bc, nf, nfresh, ntex);
        }
    }
}

/** @brief Log the stock class-250 Model header and its first animations. */
static void dump_stock_model(void)
{
    uint32_t mp = rd32(OP_GADDR_g_Models + STATUE_CLASS * 4u), m, na, i;
    char snd[3 * 16 + 1];
    if (!mp) return;
    m = mp | 0x80000000u;
    na = rd32(m);
    for (i = 0; i < 16; i++) sprintf(snd + i * 3, "%02x ", rd8(m + 4u + i));
    g_api->log(g_self, OP_MOD_LOG_INFO,
        "stock cls 250 (level %d): model=%08x numAnims=%d sounds=[%s] coll=[%08x %08x %08x %08x] data=%08x",
        (int)rd32(OP_GADDR_g_LevelId), mp, (int)na, snd,
        rd32(m + 0x14), rd32(m + 0x18), rd32(m + 0x1C), rd32(m + 0x20), rd32(m + 0x34));
    if ((int)na < 0) return;
    for (i = 0; i < na && i < 8; i++) {
        uint32_t ah = rd32(m + 0x38u + 4u * i);
        if (ah) dump_anim(ah | 0x80000000u, (int)i);
    }
}

/**
 * @brief Log every live statue moby in the level moby array.
 *
 * The array ends at the first moby whose m_State is -1. Field offsets
 * come from the typed Moby view in openpete_game_structs.h, which is the
 * same header a mod uses to read any moby field by name.
 */
static void dump_statue_mobys(void)
{
    uint32_t m = rd32(OP_GADDR_g_LevelMobys);
    int n, found = 0;
    if (!m) return;
    for (m |= 0x80000000u, n = 0; n < 4096; n++, m += MOBY_STRIDE) {
        int state = (int)(int8_t)rd8(m + offsetof(Moby, m_State));
        uint16_t cls = (uint16_t)(rd32(m + 0x34u) >> 16);
        if (state == -1) break;
        if (cls != STATUE_CLASS) continue;
        found++;
        g_api->log(g_self, OP_MOD_LOG_INFO,
            "  statue moby %08x: anim=%u frame=%u state=%d pos=(%d,%d,%d) "
            "renderer=%02x radius=%02x scaleovr=%02x spec=%02x%02x%02x type=%u props=%08x",
            m, rd8(m + offsetof(Moby, m_AnimationState) + offsetof(AnimationState, m_Animation)), rd8(m + offsetof(Moby, m_AnimationState) + offsetof(AnimationState, m_Frame)), state,
            (int)rd32(m + 0xC), (int)rd32(m + 0x10), (int)rd32(m + 0x14),
            rd8(m + offsetof(Moby, m_Renderer)), rd8(m + offsetof(Moby, m_RenderRadius)),
            rd8(m + offsetof(Moby, m_ScaleOverride)),
            rd8(m + offsetof(Moby, m_SpecularMetalColor)),
            rd8(m + offsetof(Moby, m_SpecularMetalColor) + 1),
            rd8(m + offsetof(Moby, m_SpecularMetalColor) + 2),
            rd8(m + offsetof(Moby, m_SpecularMetalType)), rd32(m + 0x4));
    }
    g_api->log(g_self, OP_MOD_LOG_INFO, "  %d statue mobys", found);
}

/**
 * @brief Read, validate, park and relocate the asset pack.
 * @return 1 when the model is parked and s_model is valid; 0 otherwise,
 *         with the cause logged.
 *
 * The pack path is derived from data_dir, the mod's own persistent
 * directory, so the mod finds its assets wherever the mod directory
 * lives. Every header field is checked against the file size and the
 * format's ceilings before any of it is used; a pack from a different
 * generator version fails here rather than in the renderer.
 *
 * The park is not allocated IMMUTABLE. The payload is re-copied on
 * every fresh level and the game reads per-level bytes from the parked
 * header, so the engine must be free to restore the arena across a
 * state load. An IMMUTABLE park is for data the game only ever reads
 * and the mod never rewrites.
 */
static int load_pack(void)
{
    char path[512];
    FILE* f;
    long fsize;
    const uint32_t* rw;
    const uint32_t* rf;

    snprintf(path, sizeof path, "%s/../assets/teapot_s1pack.bin", g_api->data_dir(g_self));
    f = fopen(path, "rb");
    if (!f) {
        g_api->log(g_self, OP_MOD_LOG_WARN, "no asset pack at %s (run tools/gen_teapot.py)", path);
        return 0;
    }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof s_pack || fsize > (1 << 20)) { fclose(f); return 0; }
    s_file = (uint8_t*)malloc((size_t)fsize);
    if (!s_file || fread(s_file, 1, (size_t)fsize, f) != (size_t)fsize) { fclose(f); return 0; }
    fclose(f);
    memcpy(&s_pack, s_file, sizeof s_pack);
    if (s_pack.magic != PACK_MAGIC || s_pack.version != 1 ||
        sizeof s_pack + 4u * (s_pack.n_reloc_words + s_pack.n_reloc_frames) +
            s_pack.payload_size != (uint32_t)fsize ||
        s_pack.hp_verts > 255u || s_pack.lp_verts > s_pack.hp_verts) {
        g_api->log(g_self, OP_MOD_LOG_ERROR, "bad pack header (magic=%08x)", s_pack.magic);
        return 0;
    }
    rw = (const uint32_t*)(s_file + sizeof s_pack);
    rf = rw + s_pack.n_reloc_words;
    s_payload = (uint8_t*)(rf + s_pack.n_reloc_frames);

    {
        void* view = NULL;
        uint32_t i;
        s_park = g_api->guest_alloc(g_self, s_pack.payload_size, 16, 0, &view);
        if (!s_park || !view) {
            g_api->log(g_self, OP_MOD_LOG_ERROR, "park allocation failed");
            return 0;
        }
        for (i = 0; i < s_pack.n_reloc_words; i++) {
            uint32_t w;
            memcpy(&w, s_payload + rw[i], 4);
            w += s_park;
            memcpy(s_payload + rw[i], &w, 4);
        }
        /* Handles are requested here, once, in pack order. The engine
         * records handle allocations in a ledger that replays on a state
         * load; the same request sequence every session is what makes
         * the replay land on the same handles. */
        for (i = 0; i < s_pack.n_reloc_frames; i++) {
            uint32_t w, h;
            memcpy(&w, s_payload + rf[i], 4);
            h = g_api->vstream_handle(g_self, s_park + (w & 0x1FFFFFu));
            if (!h) {
                g_api->log(g_self, OP_MOD_LOG_ERROR, "no stream handle for frame %u", i);
                return 0;
            }
            w = (w & ~0x1FFFFFu) | h;
            memcpy(s_payload + rf[i], &w, 4);
        }
        s_model = s_park + s_pack.model_off;
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
        "pack: %u verts (LP %u), %u HP / %u LP faces, %u B parked at %08x",
        s_pack.hp_verts, s_pack.lp_verts, s_pack.hp_faces, s_pack.lp_faces,
        s_pack.payload_size, s_park);
    return 1;
}

/** @brief Repaint the Mods-panel status block. */
static void publish_status(void)
{
    g_api->ui_status(g_self, "Teapot statues");
    if (!s_file)
        g_api->ui_status(g_self, "no asset pack - run tools/gen_teapot.py");
    else
        g_api->ui_status(g_self, "%u-vertex teapot at %08x; installed in %d level load(s)",
                         s_pack.hp_verts, s_model, s_patched_levels);
}

/**
 * @brief Post-hook on PatchMobyModelPointers: install the teapot over
 *        class 250 after the game installs a level's models.
 *
 * base() runs first so the game's own install is complete before the
 * row is inspected. A row holding neither 0 nor the park means the game
 * just installed a level's stock statue.
 *
 * The park contents are verified before use by comparing a window the
 * game never writes, the tail of the vertex stream, against the host
 * copy. After a state load or a fresh session the arena may have been
 * restored to older bytes or never filled; the comparison detects both
 * and re-copies. The header's m_Sounds (16 per-level sound definition
 * indices) and m_CollisionModels (8 words) are copied from the stock
 * model on every install because they differ per level and a static
 * pack cannot carry them.
 */
static void on_patch_models(CPUState* cpu)
{
    uint32_t row = OP_GADDR_g_Models + STATUE_CLASS * 4u, stock;
    void* park_host;
    int b;

    g_api->base(cpu);
    if (!s_enabled || !s_file) return;
    stock = rd32(row);
    if (!stock || stock == s_model) return;

    park_host = g_api->guest(s_park);
    if (!park_host) { g_api->log(g_self, OP_MOD_LOG_ERROR, "park unmapped"); return; }
    {
        uint32_t w = s_pack.payload_size < 64u ? s_pack.payload_size : 64u;
        uint32_t off = s_pack.payload_size - w;
        if (memcmp((uint8_t*)park_host + off, s_payload + off, w) != 0)
            memcpy(park_host, s_payload, s_pack.payload_size);
    }
    s_stock = stock;
    stock |= 0x80000000u;
    for (b = 0; b < 16; b++)
        op_write_u8(g_api, s_model + 4u + (uint32_t)b, rd8(stock + 4u + (uint32_t)b));
    for (b = 0; b < 8; b++)
        op_write_u32(g_api, s_model + 0x14u + 4u * (uint32_t)b, rd32(stock + 0x14u + 4u * (uint32_t)b));
    op_write_u32(g_api, row, s_model);
    s_patched_levels++;
    g_api->log(g_self, OP_MOD_LOG_INFO, "level %d: class 250 %08x -> teapot %08x",
               (int)rd32(OP_GADDR_g_LevelId), stock, s_model);
    publish_status();
}

/**
 * @brief Hook around LoadDragonCutscene: restore the game's model-buffer
 *        bound for the loader's sizing stage.
 *
 * `g_DragonCutscene.m_Stage` 0 computes the read length from the row; no
 * other stage reads the row. The stock row goes in before base() and the
 * teapot comes back after, so gameplay never renders a frame with the
 * stock statue. The hook only acts once a level install has recorded
 * the stock value.
 */
#define DRAGON_CUTSCENE_STAGE_OFF 0x2Cu  /**< int m_Stage in g_DragonCutscene. */

static void on_load_dragon_cutscene(CPUState* cpu)
{
    uint32_t row = OP_GADDR_g_Models + STATUE_CLASS * 4u;
    int swapped = 0;

    if (s_file && s_stock && rd32(row) == s_model &&
        rd32(OP_GADDR_g_DragonCutscene + DRAGON_CUTSCENE_STAGE_OFF) == 0u) {
        op_write_u32(g_api, row, s_stock);
        swapped = 1;
    }
    g_api->base(cpu);
    if (swapped && rd32(row) == s_stock)
        op_write_u32(g_api, row, s_model);
}

/**
 * @brief Pre-hook on the specular-moby renderer (func_80020F34), which
 *        runs every gameplay tick and draws the statue.
 *
 * Used only as a per-tick point at which mobys exist, for the one dump
 * per level. The renderer itself is untouched.
 */
static void on_specular_render(CPUState* cpu)
{
    int level = (int)rd32(OP_GADDR_g_LevelId);
    if (s_dump && level != s_dumped_level && rd32(OP_GADDR_g_LevelMobys)) {
        s_dumped_level = level;
        dump_stock_model();
        dump_statue_mobys();
    }
    g_api->base(cpu);
}

/**
 * @brief Mod entry point.
 *
 * A missing or bad pack leaves the mod loaded with a status line saying
 * so; the hooks still register and do nothing until a pack exists. The
 * overrides attach by address through the OP_FNADDR_ constants the SDK
 * generates from the symbol manifest, which is the form for a function
 * that has an address but no natural-signature wrapper.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    g_api = api; g_self = self;
    s_dump = api->config_bool(self, "debug.dump", 0);
    s_enabled = api->config_bool(self, "teapot.enabled", 1);
    if (!load_pack()) { free(s_file); s_file = NULL; }
    publish_status();
    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers, on_patch_models) != 0) return -1;
    if (api->override_addr(self, OP_FNADDR_func_80020F34, on_specular_render) != 0) return -1;
    if (api->override_name(self, "LoadDragonCutscene", on_load_dragon_cutscene) != 0) return -1;
    api->log(self, OP_MOD_LOG_INFO, "teapot-dragons: armed (dump=%d)", s_dump);
    return 0;
}
