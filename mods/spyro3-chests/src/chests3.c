/* spyro3-chests — Spyro 3's basket & vase replace Spyro 1's wooden &
 * metal chests: models + textures, destruction fragment models, break
 * SFX, and COLLISION primitives — entirely through the mod API.
 *
 * Mapping (fixed):
 *   g_Models[194] wooden chest  <- S3 basket (class 200)
 *   g_Models[195] metal  chest  <- S3 vase   (class 201)
 *   g_Models[255/256/257]       <- S3 basket fragments (306 s0/s1/s0)
 *   g_Models[309/310/311]       <- S3 vase fragments   (307 s0/s1, 308 s0)
 *
 * Mechanism (plan: notes/plans/openpete-mod-spyro3-chests.md):
 *  1. entry: read assets/s3/chests_s1pack.bin host-side. The pack is
 *     linked at the FIXED low park 0x80008000 — animated-model frame
 *     records address their vertex stream through a 21-bit offset
 *     (|0x80000000), so the payload MUST live in the low 2 MB; the
 *     guest_alloc arena (>2 MB) is unusable here, unlike spyro3-gems.
 *  2. post-hook PatchMobyModelPointers (0x800133E0): after the game's
 *     install pass, copy the payload to the park and repoint the
 *     g_Models rows at our parked full Model structs (each: header +
 *     1-frame AnimationHeader + all-literal vertex stream + faceblock
 *     + colour blocks + collision primitive blobs).
 *  3. textures: the pack's texel strip registers as a MOD-OWNED RGBA
 *     texture (api->texture_register, id 0x53C1); faceblocks bake the
 *     PSX-reserved tpage MODE 3 + the id, and the native renderer binds
 *     it through the HD pipe. No VRAM is claimed (a claimed rect
 *     corrupted level terrain: levels upload a full 512x512 image over
 *     the whole right half, so no rect is ever free). The PSX-path
 *     panes (og/diff) show garbage on chest faces — native-only by
 *     design.
 *  4. SFX: S3 break samples SpuWrite'd into the free SPU tail and the
 *     stock SoundDefinitions repointed (per-level def indices read from
 *     the live models' m_Sounds[0]).
 *
 * Collision: our Models carry their own primitive blobs (u16 bounding
 * radius header + typed prims; type 2 = vertical cylinder) sized to the
 * S3 shapes — gameplay-affecting BY DESIGN (this mod stress-tests the
 * collision axis; it is not sim_neutral).
 *
 * Without the asset pack the mod runs in CALIBRATION mode: it dumps the
 * stock chest + fragment Models (header fields, vertex extents, face
 * stream summary, collision primitives, m_Sounds) and touches nothing.
 * The dump feeds tools/convert_chests.py.
 */

#include "psx_runtime.h"
#include "openpete_mod_api.h"
#include "openpete_sdk_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_MAGIC  0x31433353u /* "S3C1" little-endian */
#define PARK_LINK   0x8000B000u /* fixed low park (21-bit vertex offsets);
                                     * 0x80008000 is spyro3-gems' fixed park */
#define PARK_LIMIT  0x80010000u

#define N_SLOTS 8
static const uint16_t k_s1_classes[N_SLOTS] = {194, 195, 255, 256, 257,
                                               309, 310, 311};
/* calibration-only extras: locked chest + key (Stone Hill's second
 * chest type candidate) */
static const uint16_t k_dump_extra[2] = {174, 173};

typedef struct {
    uint32_t magic;
    uint32_t payload_size;        /* copied verbatim to PARK_LINK */
    uint32_t model_vaddr[N_SLOTS];/* absolute guest vaddrs inside payload */
    uint32_t basket_vag_off;      /* FILE offsets (host-only tail) */
    uint32_t basket_vag_size;
    uint32_t vase_vag_off;
    uint32_t vase_vag_size;
    uint32_t tex_off;             /* 15bpp texel strip, FILE offset */
    uint32_t tex_w, tex_h;        /* halfwords x rows */
    uint32_t vram_x, vram_y;      /* claimed VRAM rect origin */
} pack_header_t;

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

static pack_header_t s_pack;
static uint8_t* s_file;            /* whole pack file */
static uint32_t s_file_size;
static const uint8_t* s_payload;   /* = s_file + sizeof header */
static int s_have_pack;
static int s_calibrated;
static int s_need_tex;    /* park done; SPU upload pending (camera hook) */

/* ── calibration: dump one stock Model in full ───────────────────────── */

static void dump_collision_blob(uint32_t blob, int slot)
{
    uint32_t radius, p;
    int n = 0;
    if (!blob) return;
    p = blob | 0x80000000u;
    radius = op_read_u32(A, p) & 0xFFFFu;
    A->log(SELF, OP_MOD_LOG_INFO,
           "    coll[%d] @%08x (top bit %s): bounding radius %u",
           slot, blob, (blob & 0x80000000u) ? "SET(mesh/damage)" : "clear",
           radius);
    p += 4;
    while (n++ < 16) {
        uint32_t w0 = op_read_u32(A, p);
        uint32_t type = (w0 >> 8) & 0xFFu;
        uint32_t flags = w0 & 0xFFu;
        uint32_t rad = w0 >> 16;
        if (type == 0) {
            A->log(SELF, OP_MOD_LOG_INFO,
                   "      prim %d: SPHERE r=%u flags=%02x [%08x %08x]",
                   n, rad, flags, w0, op_read_u32(A, p + 4));
            p += 8;
        } else if (type == 1) {
            A->log(SELF, OP_MOD_LOG_INFO,
                   "      prim %d: ZSPHERE r=%u zoff=%d flags=%02x [%08x %08x %08x]",
                   n, rad, (int16_t)(op_read_u32(A, p + 4) >> 16), flags,
                   w0, op_read_u32(A, p + 4), op_read_u32(A, p + 8));
            p += 12;
        } else if (type == 2) {
            uint32_t w1 = op_read_u32(A, p + 4);
            A->log(SELF, OP_MOD_LOG_INFO,
                   "      prim %d: CYL r=%u ztop=%d zbot=%d capz=%d/%d capr=%u flags=%02x",
                   n, rad, (int16_t)(w1 >> 16), (int16_t)(w1 & 0xFFFFu),
                   (int16_t)(op_read_u32(A, p + 8) & 0xFFFFu),
                   (int16_t)(op_read_u32(A, p + 8) >> 16),
                   op_read_u32(A, p + 12), flags);
            p += 20;
        } else if (type == 3) {
            A->log(SELF, OP_MOD_LOG_INFO,
                   "      prim %d: VTXSPHERE r=%u vtx=%u flags=%02x",
                   n, rad, op_read_u32(A, p + 4) & 0xFFFFu, flags);
            p += 12;
        } else {
            A->log(SELF, OP_MOD_LOG_INFO,
                   "      prim %d: UNKNOWN type %u [%08x] — stop", n, type, w0);
            break;
        }
        if (flags & 1u) break;   /* terminator */
    }
}

static void dump_stock_model(uint16_t cls)
{
    uint32_t mp = op_read_u32(A, OP_GADDR_g_Models + (uint32_t)cls * 4u);
    uint32_t m, num_anims, ah, frame0, fw0, fw1, vstream_va;
    int i, nverts, short_words;
    int minx = 99999, maxx = -99999, miny = 99999, maxy = -99999,
        minz = 99999, maxz = -99999;
    int vx = 0, vy = 0, vz = 0, flag = 0;
    uint32_t litp, shortp;
    int s5;
    char snd[3 * 16 + 1];

    if (!mp) { A->log(SELF, OP_MOD_LOG_WARN, "cls %u: no model", cls); return; }
    m = mp | 0x80000000u;
    num_anims = op_read_u32(A, m);
    for (i = 0; i < 16; i++)
        sprintf(snd + i * 3, "%02x ", op_read_u8(A, m + 4u + (uint32_t)i));
    A->log(SELF, OP_MOD_LOG_INFO,
           "stock cls %u: model=%08x numAnims=%d sounds=[%s]",
           cls, mp, (int)num_anims, snd);
    if ((int)num_anims < 0) {
        A->log(SELF, OP_MOD_LOG_INFO, "  (SimpleModel — no further dump)");
        return;
    }
    for (i = 0; i < 8; i++)
        dump_collision_blob(op_read_u32(A, m + 0x14u + (uint32_t)i * 4u), i);

    ah = op_read_u32(A, m + 0x38u);   /* m_Animations[0] */
    if (ah < 0x80000000u) ah |= 0x80000000u;
    /* the two count bytes are separate per-tier vertex counts (HP/LP),
     * NOT a 16-bit split — walk the HP one */
    nverts = op_read_u8(A, ah + 8u);
    A->log(SELF, OP_MOD_LOG_INFO,
           "  anim0 @%08x: frames=%d colors=%d scale=%d shortshift=%d radius=%d "
           "verts=%d/%d depthscale=%d progress=%d ptrs faces=%08x colors=%08x "
           "lpfaces=%08x lpcolors=%08x",
           ah,
           (int)(int16_t)(op_read_u32(A, ah) & 0xFFFFu),
           op_read_u32(A, ah) >> 16,
           op_read_u8(A, ah + 5u), op_read_u8(A, ah + 6u),
           op_read_u8(A, ah + 7u), nverts, op_read_u8(A, ah + 9u),
           op_read_u8(A, ah + 0xBu), op_read_u8(A, ah + 0xCu),
           op_read_u32(A, ah + 0x14u), op_read_u32(A, ah + 0x18u),
           op_read_u32(A, ah + 0x1Cu), op_read_u32(A, ah + 0x20u));

    frame0 = ah + 0x24u;
    fw0 = op_read_u32(A, frame0);
    fw1 = op_read_u32(A, frame0 + 4u);
    A->log(SELF, OP_MOD_LOG_INFO,
           "  frame0: vtxoff=%06x collmodel=%u framesound=%u coloroff=%u "
           "shadow=%u shortoff=%u",
           fw0 & 0x1FFFFFu, (fw0 >> 21) & 7u, fw0 >> 24,
           fw1 & 0xFFFFu, (fw1 >> 16) & 0xFFu, fw1 >> 24);

    /* vertex stream walk (mirror of the renderer's vstream decode):
     * literals reset, shorts add; extents in model units. */
    vstream_va = (fw0 & 0x1FFFFFu) | 0x80000000u;
    short_words = (int)(fw1 >> 24);
    litp = vstream_va;
    shortp = vstream_va + (uint32_t)short_words * 4u;
    s5 = op_read_u8(A, ah + 6u);
    for (i = 0; i < nverts; i++) {
        if (i == 0 || flag == 0) {
            uint32_t w = op_read_u32(A, litp); litp += 4;
            vz = (int32_t)w >> 21;
            vx = (int32_t)(w << 10) >> 21;
            vy = (int32_t)(w << 20) >> 19;
            flag = (int)(w & 1u);
        } else {
            int16_t d = (int16_t)(op_read_u32(A, shortp) & 0xFFFFu); shortp += 2;
            vz += ((int32_t)d >> 11) << s5;
            vx -= ((int32_t)((uint32_t)(int32_t)d << 21) >> 27) << s5;
            vy -= ((int32_t)((uint32_t)(int32_t)d << 26) >> 27) << s5;
            flag = (int)((uint16_t)d & 1u);
        }
        if (vx < minx) minx = vx; if (vx > maxx) maxx = vx;
        if (vy < miny) miny = vy; if (vy > maxy) maxy = vy;
        if (vz < minz) minz = vz; if (vz > maxz) maxz = vz;
    }
    A->log(SELF, OP_MOD_LOG_INFO,
           "  vert extents (model units): x[%d,%d] y[%d,%d] z[%d,%d]",
           minx, maxx, miny, maxy, minz, maxz);

    /* faceblock summary: record walk with the renderer's strides */
    {
        uint32_t fb = op_read_u32(A, ah + 0x14u);
        if (fb) {
            uint32_t bc, fp, end;
            int nf = 0, ntex = 0, nfresh = 0;
            if (fb < 0x80000000u) fb |= 0x80000000u;
            bc = op_read_u32(A, fb);
            fp = fb + 4u; end = fb + 4u + bc;
            while (fp + 8u <= end && nf < 4096) {
                uint32_t w0 = op_read_u32(A, fp);
                int fresh = (int32_t)w0 < 0;
                int tex = (w0 & 2u) != 0;
                int sprite = fresh && (w0 & 4u);
                uint32_t adv = sprite ? 0x14u
                             : fresh ? (tex ? 0x18u : 0x0Cu)
                                     : (tex ? 0x14u : 0x08u);
                if (!sprite && !(w0 & 8u)) {
                    nf++; ntex += tex; nfresh += fresh;
                    if (tex && ntex <= 3) {
                        uint32_t td0 = op_read_u32(A, fp + adv - 0xCu);
                        uint32_t td1 = op_read_u32(A, fp + adv - 0x8u);
                        A->log(SELF, OP_MOD_LOG_INFO,
                               "    tex face: clut=%04x tpage=%04x",
                               td0 >> 16, td1 >> 16);
                    }
                }
                fp += adv;
            }
            A->log(SELF, OP_MOD_LOG_INFO,
                   "  faceblock @%08x: %u bytes, %d faces (%d fresh, %d textured)",
                   fb, bc, nf, nfresh, ntex);
        }
    }
}

/* ── calibration: SoundTable semantic-slot map (names live at the
 * engine's k_sfx_names; we need the slot whose def == the chest models'
 * m_Sounds[0] to pick sfx_replace names) ─────────────────────────────── */

static void dump_sound_table(void)
{
    uint32_t tbl = op_read_u32(A, 0x800761D4u);   /* g_Spu.m_SoundTable */
    char line[3 * 24 + 1];
    int i;
    if (!tbl) return;
    for (i = 0; i < 24; i++)
        sprintf(line + i * 3, "%02x ", op_read_u8(A, tbl + (uint32_t)i));
    A->log(SELF, OP_MOD_LOG_INFO, "SoundTable[0..23]: %s", line);
    A->log(SELF, OP_MOD_LOG_INFO,
           "  slot 0x03 (chest_break) -> def %02x; slot 0x11 -> def %02x",
           op_read_u8(A, tbl + 3u), op_read_u8(A, tbl + 0x11u));
}

/* ── park + patch ────────────────────────────────────────────────────── */

static void park_and_patch(CPUState* cpu)
{
    uint32_t bounce_va = (PARK_LINK + s_pack.payload_size + 15u) & ~15u;
    void* park_host = A->guest(PARK_LINK);
    int i, patched = 0, fresh_level = 0;

    if (!park_host) { A->log(SELF, OP_MOD_LOG_ERROR, "park unmapped"); return; }
    if (bounce_va + 16u + s_pack.tex_w * 16u > PARK_LIMIT) {
        A->log(SELF, OP_MOD_LOG_ERROR, "park overflow"); return;
    }

    /* a row holding neither 0 nor our vaddr = the game just installed
     * the stock model for a (re)loaded level */
    for (i = 0; i < N_SLOTS; i++) {
        uint32_t row = OP_GADDR_g_Models + (uint32_t)k_s1_classes[i] * 4u;
        uint32_t cur = op_read_u32(A, row);
        if (cur && cur != s_pack.model_vaddr[i]) { fresh_level = 1; break; }
    }
    if (!fresh_level) return;

    /* (re)copy the payload only when the park content is actually gone —
     * a blind copy every install pass wiped the per-level stock bytes
     * (m_Sounds) written by earlier passes of the same load. Compare a
     * window we never mutate (past the sound bytes: collision ptrs +
     * anim header), NOT the header start. */
    if (memcmp((uint8_t*)park_host + 0x18, s_payload + 0x18, 64) != 0)
        memcpy(park_host, s_payload, s_pack.payload_size);

    for (i = 0; i < N_SLOTS; i++) {
        uint32_t row = OP_GADDR_g_Models + (uint32_t)k_s1_classes[i] * 4u;
        uint32_t stock = op_read_u32(A, row);
        uint32_t ours = s_pack.model_vaddr[i];
        int b;
        if (!stock || stock == ours) continue;
        /* per-level stock bytes our static pack can't know: the sound
         * slots (sound def indices differ per level) */
        for (b = 0; b < 16; b++)
            op_write_u8(A, ours + 4u + (uint32_t)b,
                        op_read_u8(A, (stock | 0x80000000u) + 4u + (uint32_t)b));
        op_write_u32(A, row, ours);
        patched++;
    }
    if (patched) {
        s_need_tex = 1;   /* uploaded on the next CameraUpdate — calling
                           * LoadImage inside the install hook while the
                           * level's CD stream is active hangs the load */
        A->log(SELF, OP_MOD_LOG_INFO,
               "parked %u bytes at %08x, patched %d model rows",
               s_pack.payload_size, PARK_LINK, patched);
    }
}

/* ── the models hook ─────────────────────────────────────────────────── */

static void on_patch_models(CPUState* cpu)
{
    A->base(cpu);

    /* nothing installed yet? (boot-time call before the level loads) */
    if (op_read_u32(A, OP_GADDR_g_Models + 194u * 4u) == 0) return;

    if (!s_calibrated) {
        int i;
        s_calibrated = 1;
        if (getenv("S3C_DUMP")) {
            for (i = 0; i < N_SLOTS; i++) dump_stock_model(k_s1_classes[i]);
            for (i = 0; i < 2; i++) dump_stock_model(k_dump_extra[i]);
        }
        dump_sound_table();
    }

    if (s_have_pack) park_and_patch(cpu);
}

/* break-SFX: upload the S3 ADPCM bodies into the free SPU tail (after
 * the level bank) and REPOINT the stock SoundDefinitions the chest
 * models reference (m_Sounds[0] = per-level def index). Guest-side by
 * design — this mod is sim-divergent anyway (collision), and the def
 * index differs per level so a static host overlay can't name it. */
static void upload_sfx(CPUState* cpu, uint32_t bounce_va)
{
    uint32_t defs_va = op_read_u32(A, 0x800761D0u);  /* g_Spu defs */
    uint32_t vram = op_read_u32(A, 0x80076C04u);     /* level SPU size */
    uint32_t tail;
    uint8_t* bounce = (uint8_t*)A->guest(bounce_va);
    uint32_t saved_a0 = cpu->a0, saved_a1 = cpu->a1;
    int i;
    static const uint16_t chest_cls[2] = {194, 195};
    const uint32_t vag_off[2] = {s_pack.basket_vag_off, s_pack.vase_vag_off};
    const uint32_t vag_size[2] = {s_pack.basket_vag_size, s_pack.vase_vag_size};

    if (!defs_va || !bounce || vram < 0x80000u || vram > 0x100000u) return;
    tail = (0x1010u + vram - 0x80000u + 15u) & ~15u;

    for (i = 0; i < 2; i++) {
        uint32_t ours = 0, def_idx, len = vag_size[i];
        int slot;
        for (slot = 0; slot < N_SLOTS; slot++)
            if (k_s1_classes[slot] == chest_cls[i])
                ours = s_pack.model_vaddr[slot];
        if (!ours || !len || len > 0x4000u) continue;
        def_idx = op_read_u8(A, ours + 4u);          /* copied stock byte */
        if (def_idx == 0xFF) continue;
        if (tail + len > 0x7F000u) break;            /* SPU 512K ceiling */
        /* chunked through the small tex bounce — the park has no room
         * for a whole VAG above the payload (0x80010000 = EXE base) */
        {
            uint32_t done = 0;
            while (done < len) {
                uint32_t chunk = len - done;
                if (chunk > 2048u) chunk = 2048u;
                memcpy(bounce, s_file + vag_off[i] + done, chunk);
                cpu->a0 = tail + done;
                A->call_addr(cpu, OP_FNADDR_SpuSetTransferStartAddr);
                cpu->a0 = bounce_va;
                cpu->a1 = chunk;
                A->call_addr(cpu, OP_FNADDR_SpuWrite);
                done += chunk;
            }
        }
        op_write_u32(A, defs_va + def_idx * 0x14u, tail);
        A->log(SELF, OP_MOD_LOG_INFO,
               "sfx: def %02x repointed to SPU %05x (%u bytes, %s)",
               def_idx, tail, len, i == 0 ? "basket" : "vase");
        tail = (tail + len + 15u) & ~15u;
    }
    cpu->a0 = saved_a0; cpu->a1 = saved_a1;
}

/* CameraUpdate post-hook: the render loop is alive here — safe spot for
 * the VRAM + SPU uploads (and a fully-loaded SoundTable dump). */
static void on_camera_update(CPUState* cpu)
{
    A->base(cpu);
    if (s_need_tex) {
        uint32_t bounce_va = (PARK_LINK + s_pack.payload_size + 15u) & ~15u;
        s_need_tex = 0;
        upload_sfx(cpu, bounce_va);
        dump_sound_table();
    }
}

/* ── entry ───────────────────────────────────────────────────────────── */

static int load_pack(void)
{
    char path[512];
    FILE* f;
    long fsize;

    snprintf(path, sizeof path, "%s/../assets/s3/chests_s1pack.bin",
             A->data_dir(SELF));
    f = fopen(path, "rb");
    if (!f) {
        A->log(SELF, OP_MOD_LOG_WARN,
               "no asset pack at %s — calibration mode (run tools/ generators)",
               path);
        return 0;
    }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof s_pack || fsize > (2 << 20)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "bad pack size %ld", fsize);
        fclose(f);
        return 0;
    }
    s_file = (uint8_t*)malloc((size_t)fsize);
    if (!s_file || fread(s_file, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        return 0;
    }
    fclose(f);
    s_file_size = (uint32_t)fsize;
    memcpy(&s_pack, s_file, sizeof s_pack);
    if (s_pack.magic != PACK_MAGIC || s_pack.payload_size == 0 ||
        sizeof s_pack + s_pack.payload_size > s_file_size ||
        s_pack.tex_off + s_pack.tex_w * s_pack.tex_h * 2u > s_file_size ||
        s_pack.basket_vag_off + s_pack.basket_vag_size > s_file_size ||
        s_pack.vase_vag_off + s_pack.vase_vag_size > s_file_size) {
        A->log(SELF, OP_MOD_LOG_ERROR, "bad pack header (magic=%08x size=%u)",
               s_pack.magic, s_pack.payload_size);
        free(s_file); s_file = NULL;
        return 0;
    }
    s_payload = s_file + sizeof s_pack;
    return 1;
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    A = api; SELF = self;

    s_have_pack = load_pack();
    if (s_have_pack)
        A->log(SELF, OP_MOD_LOG_INFO, "pack loaded: payload=%u bytes",
               s_pack.payload_size);

    if (s_have_pack) {
        /* the texel strip becomes a mod-owned RGBA texture: faces bake
         * tpage MODE 3 + MODTEX_ID (see convert_chests.py) — no VRAM is
         * touched (the first cut claimed a rect and corrupted terrain:
         * levels upload a full 512x512 image over the whole right half,
         * so no rect is ever free) */
        uint32_t n = s_pack.tex_w * s_pack.tex_h;
        const uint16_t* t5551 = (const uint16_t*)(s_file + s_pack.tex_off);
        uint8_t* rgba = (uint8_t*)malloc((size_t)n * 4u);
        if (rgba) {
            uint32_t k;
            for (k = 0; k < n; k++) {
                uint16_t p = t5551[k];
                rgba[k * 4 + 0] = (uint8_t)(((p >> 0) & 0x1Fu) * 255u / 31u);
                rgba[k * 4 + 1] = (uint8_t)(((p >> 5) & 0x1Fu) * 255u / 31u);
                rgba[k * 4 + 2] = (uint8_t)(((p >> 10) & 0x1Fu) * 255u / 31u);
                rgba[k * 4 + 3] = (uint8_t)(p ? 255u : 0u);  /* 0x0000 = PSX transparent */
            }
            if (api->texture_register(self, 0x53C1u, rgba,
                                      (int)s_pack.tex_w, (int)s_pack.tex_h))
                A->log(SELF, OP_MOD_LOG_ERROR, "texture_register failed");
            free(rgba);   /* registry keeps its own copy */
        }
    }

    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers,
                           on_patch_models)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "override registration failed");
        return 1;
    }
    if (s_have_pack &&
        api->override_addr(self, OP_FNADDR_CameraUpdate, on_camera_update)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "camera hook registration failed");
        return 1;
    }
    /* break SFX go in via SoundDefinition repoint (upload_sfx) — the
     * host sfx_replace overlay can't target them: the chest plays its
     * model's per-level def index directly, which no stable SoundTable
     * slot names in every level. */
    return 0;
}
