/* crash3-models - a Crash Bandicoot 3 demo model installed over a Spyro 1
 * moby class. See mod.toml.
 *
 *  1. entry: read assets/crash3_s1pack.bin (tools/crash3/convert.py),
 *     find the configured model in the pack table, park ITS blob with
 *     guest_alloc, relocate the blob's absolute words to the park and
 *     exchange its frame words' stream offset for one stream handle
 *     (vstream_handle) - once, so the ledger replays identically.
 *  2. PatchMobyModelPointers post-hook: whenever the game has just
 *     installed a level's stock model on the target row (row holds
 *     neither 0 nor our vaddr), copy the per-level bytes the pack cannot
 *     know (m_Sounds, m_CollisionModels) from the stock header, widen our
 *     header to the stock model's shape - numAnims and the largest frame
 *     count, both into pre-filled reserve slots that all alias the one
 *     pose - and point the row at the park. The row is nulled and
 *     re-installed on every level load, so this re-fires per level and is
 *     idempotent within one.
 *
 * Pack blob layout (link base 0): model hdr 0x38 + 4*anim_slots pointers,
 * every one at the single anim header; anim hdr 0x24 + 8*frame_slots
 * frame words, every one the same. Widening is therefore two integer
 * writes; the game's readers stay inside the reserve. */

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

#define PACK_MAGIC 0x50335243u   /* 'CR3P' */
#define PACK_VERSION 1u
#define TABLE_ROW 52u
#define MODEL_HDR 0x38u
#define ANIM_HDR 0x24u

typedef struct {
    uint32_t magic, version, n_models, table_off, payload_off, payload_size,
             anim_slots, frame_slots;
} pack_header_t;

typedef struct {
    uint32_t eid;
    char     name[8];
    uint32_t blob_off, blob_size;
    uint16_t hp_verts, lp_verts, hp_faces, lp_faces, src_verts, pose_frame;
    uint32_t pose_anim, rw_off, n_rw, rf_off, n_rf;
} pack_row_t;

static pack_header_t s_pack;
static pack_row_t    s_row;          /* the installed model's table row */
static uint8_t*      s_file;         /* whole pack file */
static uint8_t*      s_blob;         /* relocated model bytes (host copy) */
static uint32_t      s_park;         /* guest vaddr of the parked blob */
static uint32_t      s_anim;         /* guest vaddr of the anim header */
static int           s_dump;
static uint32_t      s_class;
static char          s_model_name[16];
static int           s_patched_levels;

static uint32_t rd32(uint32_t a) { return op_read_u32(g_api, a); }
static uint8_t  rd8(uint32_t a)  { return op_read_u8(g_api, a); }

static void row_read(pack_row_t* r, const uint8_t* p)
{
    memcpy(&r->eid, p, 4);
    memcpy(r->name, p + 4, 8);
    memcpy(&r->blob_off, p + 12, 4);
    memcpy(&r->blob_size, p + 16, 4);
    memcpy(&r->hp_verts, p + 20, 2);  memcpy(&r->lp_verts, p + 22, 2);
    memcpy(&r->hp_faces, p + 24, 2);  memcpy(&r->lp_faces, p + 26, 2);
    memcpy(&r->src_verts, p + 28, 2); memcpy(&r->pose_frame, p + 30, 2);
    memcpy(&r->pose_anim, p + 32, 4);
    memcpy(&r->rw_off, p + 36, 4);    memcpy(&r->n_rw, p + 40, 4);
    memcpy(&r->rf_off, p + 44, 4);    memcpy(&r->n_rf, p + 48, 4);
}

static int load_pack(void)
{
    char path[512];
    FILE* f;
    long fsize;
    uint32_t i, found = 0;

    snprintf(path, sizeof path, "%s/../assets/crash3_s1pack.bin", g_api->data_dir(g_self));
    f = fopen(path, "rb");
    if (!f) {
        g_api->log(g_self, OP_MOD_LOG_WARN, "no pack at %s (run tools/crash3/convert.py)", path);
        return 0;
    }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof s_pack || fsize > (8 << 20)) { fclose(f); return 0; }
    s_file = (uint8_t*)malloc((size_t)fsize);
    if (!s_file || fread(s_file, 1, (size_t)fsize, f) != (size_t)fsize) { fclose(f); return 0; }
    fclose(f);
    memcpy(&s_pack, s_file, sizeof s_pack);
    if (s_pack.magic != PACK_MAGIC || s_pack.version != PACK_VERSION ||
        s_pack.table_off + TABLE_ROW * s_pack.n_models > (uint32_t)fsize ||
        s_pack.payload_off + s_pack.payload_size > (uint32_t)fsize ||
        s_pack.anim_slots == 0 || s_pack.frame_slots == 0) {
        g_api->log(g_self, OP_MOD_LOG_ERROR, "bad pack header (magic=%08x)", s_pack.magic);
        return 0;
    }
    for (i = 0; i < s_pack.n_models; i++) {
        pack_row_t r;
        row_read(&r, s_file + s_pack.table_off + TABLE_ROW * i);
        if (s_dump)
            g_api->log(g_self, OP_MOD_LOG_INFO,
                "  pack[%u] %.8s: %u/%u verts (LP %u), %u HP / %u LP faces, %u B",
                i, r.name, r.hp_verts, r.src_verts, r.lp_verts, r.hp_faces,
                r.lp_faces, r.blob_size);
        if (!found && strncmp(r.name, s_model_name, 8) == 0) { s_row = r; found = 1; }
    }
    if (!found) {
        g_api->log(g_self, OP_MOD_LOG_ERROR, "model %s not in the pack (%u models)",
                   s_model_name, s_pack.n_models);
        return 0;
    }
    if (s_row.blob_off + s_row.blob_size > s_pack.payload_size ||
        s_row.blob_size < MODEL_HDR + 4u * s_pack.anim_slots + ANIM_HDR + 8u * s_pack.frame_slots ||
        s_row.rw_off + 4u * s_row.n_rw > (uint32_t)fsize ||
        s_row.rf_off + 4u * s_row.n_rf > (uint32_t)fsize || s_row.n_rf == 0) {
        g_api->log(g_self, OP_MOD_LOG_ERROR, "bad table row for %s", s_model_name);
        return 0;
    }
    s_blob = (uint8_t*)malloc(s_row.blob_size);
    if (!s_blob) return 0;
    memcpy(s_blob, s_file + s_pack.payload_off + s_row.blob_off, s_row.blob_size);

    /* Park + relocate. Not IMMUTABLE: the blob is re-copied on every
     * fresh level and the game reads per-level bytes from its header. */
    {
        void* view = NULL;
        const uint32_t* rw = (const uint32_t*)(s_file + s_row.rw_off);
        const uint32_t* rf = (const uint32_t*)(s_file + s_row.rf_off);
        uint32_t handle = 0, target = 0;
        s_park = g_api->guest_alloc(g_self, s_row.blob_size, 16, 0, &view);
        if (!s_park || !view) {
            g_api->log(g_self, OP_MOD_LOG_ERROR, "park allocation failed");
            return 0;
        }
        for (i = 0; i < s_row.n_rw; i++) {
            uint32_t w;
            if (rw[i] + 4u > s_row.blob_size) return 0;
            memcpy(&w, s_blob + rw[i], 4);
            w += s_park;
            memcpy(s_blob + rw[i], &w, 4);
        }
        /* Every frame word names the same stream: one handle, requested
         * once so the ledger sequence is the same every session. */
        for (i = 0; i < s_row.n_rf; i++) {
            uint32_t w;
            if (rf[i] + 4u > s_row.blob_size) return 0;
            memcpy(&w, s_blob + rf[i], 4);
            if (i == 0 || (w & 0x1FFFFFu) != target) {
                target = w & 0x1FFFFFu;
                handle = g_api->vstream_handle(g_self, s_park + target);
                if (!handle) {
                    g_api->log(g_self, OP_MOD_LOG_ERROR, "no stream handle for frame %u", i);
                    return 0;
                }
            }
            w = (w & ~0x1FFFFFu) | handle;
            memcpy(s_blob + rf[i], &w, 4);
        }
        memcpy(&s_anim, s_blob + MODEL_HDR, 4);   /* already relocated */
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
        "pack: %.8s %u verts (LP %u, from %u), %u HP / %u LP faces, %u B parked at %08x",
        s_row.name, s_row.hp_verts, s_row.lp_verts, s_row.src_verts,
        s_row.hp_faces, s_row.lp_faces, s_row.blob_size, s_park);
    return 1;
}

static void publish_status(void)
{
    g_api->ui_status(g_self, "Crash 3 model on class %u", s_class);
    if (!s_blob)
        g_api->ui_status(g_self, "no pack / model - run tools/crash3/convert.py");
    else
        g_api->ui_status(g_self, "%.8s: %u verts at %08x; installed in %d level load(s)",
                         s_row.name, s_row.hp_verts, s_park, s_patched_levels);
}

/* Post-hook: runs after every install pass. A row holding neither 0 nor
 * our vaddr means the game just installed a level's stock model. */
static void on_patch_models(CPUState* cpu)
{
    uint32_t row = OP_GADDR_g_Models + s_class * 4u, stock;
    void* park_host;
    int b;
    uint32_t na, max_frames = 1, i;

    g_api->base(cpu);
    if (!s_blob) return;
    stock = rd32(row);
    if (!stock || stock == s_park) return;

    park_host = g_api->guest(s_park);
    if (!park_host) { g_api->log(g_self, OP_MOD_LOG_ERROR, "park unmapped"); return; }
    /* Fresh level: the arena may have been restored or never filled -
     * compare a window the game never writes (the vertex stream tail). */
    {
        uint32_t w = s_row.blob_size < 64u ? s_row.blob_size : 64u;
        uint32_t off = s_row.blob_size - w;
        if (memcmp((uint8_t*)park_host + off, s_blob + off, w) != 0)
            memcpy(park_host, s_blob, s_row.blob_size);
    }
    stock |= 0x80000000u;
    for (b = 0; b < 16; b++)                     /* m_Sounds: per-level def indices */
        op_write_u8(g_api, s_park + 4u + (uint32_t)b, rd8(stock + 4u + (uint32_t)b));
    for (b = 0; b < 8; b++)                      /* m_CollisionModels: the stock model's */
        op_write_u32(g_api, s_park + 0x14u + 4u * (uint32_t)b, rd32(stock + 0x14u + 4u * (uint32_t)b));

    /* Widen to the stock shape so any (anim, frame) a live moby of this
     * class sits on lands inside our aliased reserve. */
    na = rd32(stock);
    if ((int32_t)na < 1) na = 1;
    for (i = 0; i < na && i < 64u; i++) {
        uint32_t ah = rd32(stock + MODEL_HDR + 4u * i);
        if (!ah) continue;
        ah |= 0x80000000u;
        {
            int nf = (int)(int16_t)(rd32(ah) & 0xFFFFu);
            if (nf > (int)max_frames) max_frames = (uint32_t)nf;
        }
    }
    if (na > s_pack.anim_slots) {
        g_api->log(g_self, OP_MOD_LOG_WARN, "stock model has %u anims, reserve is %u",
                   na, s_pack.anim_slots);
        na = s_pack.anim_slots;
    }
    if (max_frames > s_pack.frame_slots) {
        g_api->log(g_self, OP_MOD_LOG_WARN, "stock model has %u frames, reserve is %u",
                   max_frames, s_pack.frame_slots);
        max_frames = s_pack.frame_slots;
    }
    op_write_u32(g_api, s_park, na);
    op_write_u8(g_api, s_anim, (uint8_t)(max_frames & 0xFFu));      /* s16 nframes, LE */
    op_write_u8(g_api, s_anim + 1u, (uint8_t)(max_frames >> 8));
    op_write_u32(g_api, row, s_park);
    s_patched_levels++;
    g_api->log(g_self, OP_MOD_LOG_INFO, "level %d: class %u %08x -> %.8s %08x (anims %u, frames %u)",
               (int)rd32(OP_GADDR_g_LevelId), s_class, stock & 0x7FFFFFFFu,
               s_row.name, s_park, na, max_frames);
    publish_status();
}

/* debug.dump: the live mobys of the target class, once per level - the
 * calibration for placing a freecam pose on the installed model. Hooked
 * on the specular-moby renderer, which runs every gameplay frame once
 * mobys exist. */
#define MOBY_STRIDE 0x58u
static int s_dumped_level;

static void dump_target_mobys(void)
{
    uint32_t m = rd32(OP_GADDR_g_LevelMobys);
    int n, found = 0;
    if (!m) return;
    for (m |= 0x80000000u, n = 0; n < 4096; n++, m += MOBY_STRIDE) {
        int state = (int)(int8_t)rd8(m + offsetof(Moby, m_State));
        uint16_t cls = (uint16_t)(rd32(m + 0x34u) >> 16);
        if (state == -1) break;
        if (cls != s_class) continue;
        found++;
        g_api->log(g_self, OP_MOD_LOG_INFO,
            "  class %u moby %08x: anim=%u frame=%u pos=(%d,%d,%d) renderer=%02x",
            s_class, m,
            rd8(m + offsetof(Moby, m_AnimationState) + offsetof(AnimationState, m_Animation)),
            rd8(m + offsetof(Moby, m_AnimationState) + offsetof(AnimationState, m_Frame)),
            (int)rd32(m + 0xC), (int)rd32(m + 0x10), (int)rd32(m + 0x14),
            rd8(m + offsetof(Moby, m_Renderer)));
    }
    g_api->log(g_self, OP_MOD_LOG_INFO, "  %d class-%u mobys", found, s_class);
}

static void on_specular_render(CPUState* cpu)
{
    int level = (int)rd32(OP_GADDR_g_LevelId);
    if (s_dump && level != s_dumped_level && rd32(OP_GADDR_g_LevelMobys)) {
        s_dumped_level = level;
        dump_target_mobys();
    }
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    g_api = api; g_self = self;
    s_dump = api->config_bool(self, "debug.dump", 0);
    s_class = (uint32_t)api->config_int(self, "target.class", 250);
    api->config_str(self, "model", "Cr10G", s_model_name, sizeof s_model_name);
    if (s_class >= 512u) {
        api->log(self, OP_MOD_LOG_ERROR, "target.class %u out of range", s_class);
        return -1;
    }
    if (!load_pack()) { free(s_blob); s_blob = NULL; }
    publish_status();
    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers, on_patch_models) != 0) return -1;
    if (s_dump && api->override_addr(self, OP_FNADDR_func_80020F34, on_specular_render) != 0) return -1;
    api->log(self, OP_MOD_LOG_INFO, "crash3-models: armed (%s -> class %u, dump=%d)",
             s_model_name, s_class, s_dump);
    return 0;
}
