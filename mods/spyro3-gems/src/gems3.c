/* spyro3-gems — Spyro 3 gem models, burst colours, and the pitch-cycling
 * collect chime, injected into Spyro 1 entirely through the mod API.
 *
 * Mechanism (see docs/injection-spec.md for the byte-level evidence):
 *  1. entry:   copy the generated asset pack (5 baked-gouraud SimpleModels
 *              + the S3 chime ADPCM body) into a free guest-RAM park at
 *              0x80008000, and recolour the collect-burst particle words.
 *  2. post-hook PatchMobyModelPointers (0x800133E0): after the game's own
 *              install pass, repoint g_Models[83..87] at the park models
 *              (bit31 clear = SimpleModel, m_Verts==model+0x10 = baked).
 *  3. post-hook SetNewSoundTable (0x80012CF0): after the level's SPU bank
 *              upload, SpuWrite the chime body into the free tail of SPU
 *              RAM and repoint the gemPickup SoundDefinition at it.
 *  4. pre-hook func_8003B854 (gem collect): step the S3 pitch ladder and
 *              write the next pitch into the definition before PlaySound
 *              reads it.
 *
 * Without the asset pack the mod runs in calibration mode: it only logs
 * the stock gem model geometry (extent/counts) and touches nothing.
 */

#include "psx_runtime.h"
#include "openpete_mod_api.h"
#include "openpete_sdk_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── layout contract with tools/convert_to_s1.py ─────────────────────── */

#define PACK_MAGIC   0x31473353u /* "S3G1" little-endian */
#define PARK_BASE    0x80008000u /* free guest RAM: below EXE, above page 0 */
#define PARK_LIMIT   0x80010000u

typedef struct {
    uint32_t magic;
    uint32_t payload_size;      /* bytes copied verbatim to PARK_BASE */
    uint32_t model_vaddr[5];    /* absolute guest vaddrs, inside payload */
    uint32_t vag_vaddr;         /* staged ADPCM body (guest vaddr) */
    uint32_t vag_size;
    uint32_t pitch_base;        /* SPU pitch for ladder step 0 */
    uint32_t burst_rgb[5];      /* 0x00BBGGRR collect-burst colours */
} pack_header_t;

/* ── fixed guest addresses (SCUS-94228; evidence in injection-spec.md) ── */

#define GEM_MODEL_SLOT0   (OP_GADDR_g_Models + 83u * 4u)  /* g_Models[83] */
#define BURST_TABLE_VA    0x8006E47Cu   /* D_8006E44C words 12..16 */
#define LEVELHDR_VRAMSPU  0x80076C04u   /* g_LevelHeader.m_VramSramSize */
#define SPU_SOUNDDEFS_VA  0x800761D0u   /* g_Spu.m_SoundDefinitions */
#define SPU_SOUNDTABLE_VA 0x800761D4u   /* g_Spu.m_SoundTable */
#define FN_SPU_SET_XFER   0x8005CB24u   /* psyq SpuSetTransferStartAddr */
#define FN_SPU_WRITE      0x8005CAC4u   /* psyq SpuWrite */
#define FN_GEM_COLLECT    0x8003B854u   /* plays gemPickup; a0 = value */
#define SOUNDDEF_STRIDE   0x14u

/* S3 pitch ladder (assets/s3/pitch_canonical.md): voice pitch =
 * 0x400 + 0x40*n, n resetting to 0 per burst (~0.25s cadence). Cap at
 * one octave and reset after ~0.5s so a normal gem trail re-anchors at
 * the canonical 11kHz chime instead of parking high. */
#define LADDER_MAX        16
#define STREAK_RESET_TICKS 15u          /* ~0.5s at 29.97Hz game ticks */

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

static pack_header_t s_pack;
static int      s_have_pack;
static int      s_calibrated;          /* stock-gem dump done */
static uint32_t s_chime_spu_addr;      /* where the chime landed in SPU RAM */
static uint32_t s_streak;
static uint32_t s_last_collect_tick;

static uint16_t ladder_pitch(uint32_t step)
{
    uint32_t base = s_pack.pitch_base ? s_pack.pitch_base : 0x400u;
    uint32_t p;
    if (step >= LADDER_MAX) step = LADDER_MAX - 1;
    p = base + step * (base >> 4);
    return (uint16_t)(p > 0x3FFFu ? 0x3FFFu : p);
}

/* ── calibration: log the stock gem models once, before we overwrite ─── */

static void dump_stock_gem(int cls_off)
{
    uint32_t entry = op_read_u32(A, GEM_MODEL_SLOT0 + (uint32_t)cls_off * 4u);
    uint32_t model = entry | 0x80000000u;
    const uint8_t* m = (const uint8_t*)A->guest(model);
    if (!entry || !m) { A->log(SELF, OP_MOD_LOG_INFO, "stock gem %d: entry=0 or unmapped", 83 + cls_off); return; }

    int nverts = (int8_t)m[0], nfaces = m[1];
    uint32_t verts = op_read_u32(A, model + 4);
    uint32_t ctab  = op_read_u32(A, model + 8);
    uint32_t faces = op_read_u32(A, model + 12);
    int minc = 255, maxc = -255;
    const int8_t* v = (const int8_t*)A->guest(verts);
    if (v && nverts > 0) {
        int i, n = nverts * 3;
        for (i = 0; i < n; i++) { int c = v[i] * 2; if (c < minc) minc = c; if (c > maxc) maxc = c; }
    }
    A->log(SELF, OP_MOD_LOG_INFO,
           "stock gem cls %d: entry=%08x hdr=%02x%02x%02x%02x verts=%d faces=%d "
           "vtab=%08x ctab=%08x ftab=%08x extent=[%d,%d] baked=%d",
           83 + cls_off, entry, m[0], m[1], m[2], m[3], nverts, nfaces,
           verts, ctab, faces, minc, maxc,
           (verts & 0x7FFFFFFFu) == ((entry & 0x7FFFFFFFu) + 0x10u));

    /* S3G_DUMP_VERTS=1: full per-vertex dump (decoded per spec §A.2:
     * stream order Z,X,Y, value = coord/2) + first face windings. */
    if (getenv("S3G_DUMP_VERTS") && v && nverts > 0) {
        int i;
        int mn[3] = { 255, 255, 255 }, mx[3] = { -255, -255, -255 };
        for (i = 0; i < nverts; i++) {
            int vz = v[i * 3 + 0] * 2;
            int vx = v[i * 3 + 1] * 2;
            int vy = v[i * 3 + 2] * 2;
            if (vx < mn[0]) mn[0] = vx; if (vx > mx[0]) mx[0] = vx;
            if (vy < mn[1]) mn[1] = vy; if (vy > mx[1]) mx[1] = vy;
            if (vz < mn[2]) mn[2] = vz; if (vz > mx[2]) mx[2] = vz;
            A->log(SELF, OP_MOD_LOG_INFO,
                   "  gem %d v%02d: VX=%4d VY=%4d VZ=%4d", 83 + cls_off, i, vx, vy, vz);
        }
        A->log(SELF, OP_MOD_LOG_INFO,
               "  gem %d axis extents: VX=[%d,%d] VY=[%d,%d] VZ=[%d,%d]",
               83 + cls_off, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
        for (i = 0; i < 4 && i < nfaces; i++) {
            uint32_t w0 = op_read_u32(A, faces + (uint32_t)i * 8u);
            uint32_t w1 = op_read_u32(A, faces + (uint32_t)i * 8u + 4u);
            A->log(SELF, OP_MOD_LOG_INFO,
                   "  gem %d f%d: w0=%08x i=(%u,%u,%u,%u) w1=%08x",
                   83 + cls_off, i, w0,
                   (w0 >> 23) & 0x7Fu, (w0 >> 16) & 0x7Fu,
                   (w0 >> 9) & 0x7Fu, (w0 >> 2) & 0x7Fu, w1);
        }
    }
}

static int dump_live_gems(void);

/* ── hook 2: models ──────────────────────────────────────────────────── */

static void on_patch_models(CPUState* cpu)
{
    int i;
    A->base(cpu);
    if (!s_calibrated && op_read_u32(A, GEM_MODEL_SLOT0) != 0) {
        s_calibrated = 1;
        for (i = 0; i < 5; i++) dump_stock_gem(i);
    }
    if (!s_have_pack) return;
    for (i = 0; i < 5; i++) {
        op_write_u32(A, GEM_MODEL_SLOT0 + (uint32_t)i * 4u,
                     s_pack.model_vaddr[i] & 0x7FFFFFFFu);
        /* burst colours live in main-EXE .data, which loads from disc
         * AFTER mod entry — (re)write them here, not at entry. */
        op_write_u32(A, BURST_TABLE_VA + (uint32_t)i * 4u, s_pack.burst_rgb[i]);
    }
}

/* ── hook 3: SPU upload + sound-definition repoint ───────────────────── */

static void on_sound_table(CPUState* cpu)
{
    uint32_t saved[8];
    uint32_t vram_spu_size, dest, defs, table, def;
    uint8_t  gem_id;

    A->base(cpu);
    if (!s_have_pack || !s_pack.vag_size) return;

    /* real SPU occupancy = 0x1010 + (m_VramSramSize - 0x80000) */
    vram_spu_size = op_read_u32(A, LEVELHDR_VRAMSPU);
    if (vram_spu_size < 0x80000u || vram_spu_size > 0x100000u) {
        A->log(SELF, OP_MOD_LOG_WARN, "implausible VramSramSize %08x; chime skipped", vram_spu_size);
        return;
    }
    dest = (0x1010u + vram_spu_size - 0x80000u + 15u) & ~15u;
    if (dest + s_pack.vag_size > 0x7EF00u) {
        A->log(SELF, OP_MOD_LOG_WARN, "no SPU room (dest=%05x len=%x); chime skipped", dest, s_pack.vag_size);
        return;
    }

    /* nested guest calls clobber a/v/t regs; caller of SetNewSoundTable
     * may still want its v0/a-regs — save/restore around the calls. */
    saved[0] = cpu->a0; saved[1] = cpu->a1; saved[2] = cpu->a2; saved[3] = cpu->a3;
    saved[4] = cpu->v0; saved[5] = cpu->v1; saved[6] = cpu->ra; saved[7] = cpu->at;

    cpu->a0 = dest;
    A->call_addr(cpu, FN_SPU_SET_XFER);
    cpu->a0 = s_pack.vag_vaddr;
    cpu->a1 = s_pack.vag_size;
    A->call_addr(cpu, FN_SPU_WRITE);

    cpu->a0 = saved[0]; cpu->a1 = saved[1]; cpu->a2 = saved[2]; cpu->a3 = saved[3];
    cpu->v0 = saved[4]; cpu->v1 = saved[5]; cpu->ra = saved[6]; cpu->at = saved[7];

    /* repoint gemPickup: def = defs + soundTable[0] * 0x14 */
    defs  = op_read_u32(A, SPU_SOUNDDEFS_VA);
    table = op_read_u32(A, SPU_SOUNDTABLE_VA);
    if (!defs || !table) { A->log(SELF, OP_MOD_LOG_WARN, "sound table ptrs null; chime skipped"); return; }
    gem_id = op_read_u8(A, table);
    def = defs + gem_id * SOUNDDEF_STRIDE;

    op_write_u32(A, def + 0x00, dest);              /* m_Addr            */
    op_write_u32(A, def + 0x04, 0);                 /* m_LoopAddr 1-shot */
    {   /* m_Pitch (u16) + zero variance so PlaySound uses it verbatim */
        uint8_t* p = (uint8_t*)A->guest(def + 0x0A);
        if (p) { uint16_t v = ladder_pitch(0); memcpy(p, &v, 2); }
        p = (uint8_t*)A->guest(def + 0x0C);
        if (p) { uint16_t z = 0; memcpy(p, &z, 2); }        /* variance  */
        op_write_u32(A, def + 0x10, 0);                     /* var type  */
    }
    s_chime_spu_addr = dest;
    A->log(SELF, OP_MOD_LOG_INFO,
           "chime uploaded: spu=%05x len=%x gemPickup id=%u def=%08x burst[0]=%08x",
           dest, s_pack.vag_size, gem_id, def, op_read_u32(A, BURST_TABLE_VA));
}

/* S3G_DUMP_VERTS=1: one-shot live-gem pose dump once level mobys exist.
 * Ground truth for the model→world axis map: m_Rotation bytes +0x44/45/46
 * (euler Z/X/Y — slot names lie; compose order start·Ry·Rx·Rz) and the
 * MobyCollectableProps base rot bytes the wobble recenters on.
 * Finding (artisans, 2026-07-11): base rot ≈ (0,0,0) — the stock gem's
 * rest pose is IDENTITY; the collect wobble cones ±45° around it. */
static int dump_live_gems(void)
{
    uint32_t m = op_read_u32(A, OP_GADDR_g_LevelMobys);
    int shown = 0, i;
    if (m < 0x80000000u) return 0;
    for (i = 0; i < 1000 && shown < 8; i++, m += 0x58u) {
        uint32_t cls = op_read_u32(A, m + 0x34) >> 16;          /* m_Class @+0x36 */
        uint32_t props;
        if (cls < 83u || cls > 87u) continue;
        props = op_read_u32(A, m + 0x00);
        if (props < 0x80000000u) continue;
        A->log(SELF, OP_MOD_LOG_INFO,
               "live gem %08x cls=%u pos=(%d,%d,%d) rot[44,45,46]=(%u,%u,%u) "
               "state=%u sub=%u props.rotXYZ=(%u,%u,%u) ticks=%u",
               m, cls,
               (int)op_read_u32(A, m + 0x0C), (int)op_read_u32(A, m + 0x10),
               (int)op_read_u32(A, m + 0x14),
               op_read_u8(A, m + 0x44), op_read_u8(A, m + 0x45), op_read_u8(A, m + 0x46),
               op_read_u8(A, m + 0x48), op_read_u8(A, m + 0x49),
               op_read_u8(A, props + 0x10), op_read_u8(A, props + 0x11),
               op_read_u8(A, props + 0x12), op_read_u8(A, props + 0x13));
        shown++;
    }
    return shown;
}

/* ── hook 4: pitch ladder per collect ────────────────────────────────── */

static void on_gem_collect(CPUState* cpu)
{
    if (s_have_pack && s_chime_spu_addr) {
        uint32_t tick = op_read_u32(A, OP_GADDR_g_GameTick);
        uint32_t defs = op_read_u32(A, SPU_SOUNDDEFS_VA);
        uint32_t table = op_read_u32(A, SPU_SOUNDTABLE_VA);
        if (tick - s_last_collect_tick > STREAK_RESET_TICKS) s_streak = 0;
        s_last_collect_tick = tick;
        if (defs && table) {
            uint32_t def = defs + op_read_u8(A, table) * SOUNDDEF_STRIDE;
            uint8_t* p = (uint8_t*)A->guest(def + 0x0A);
            if (p) { uint16_t v = ladder_pitch(s_streak); memcpy(p, &v, 2); }
        }
        A->log(SELF, OP_MOD_LOG_INFO, "gem collect: value=%u streak=%u pitch=%03x",
               cpu->a0, s_streak, ladder_pitch(s_streak));
        if (s_streak + 1 < LADDER_MAX) s_streak++;
    }
    A->base(cpu);
}

/* S3G_DUMP_VERTS only: piggyback CameraUpdate (every gameplay tick) to run
 * the live-gem pose dump once — retrying every 64 ticks until level mobys
 * are actually placed (SetNewSoundTable/PatchMobyModelPointers run before
 * moby placement, so those hooks are too early). */
static void on_camera_update_dump(CPUState* cpu)
{
    static unsigned n;
    static int done;
    A->base(cpu);
    if (!done && (++n & 63u) == 0u) done = dump_live_gems() > 0;
}

/* ── entry ───────────────────────────────────────────────────────────── */

static int load_pack(void)
{
    char path[512];
    FILE* f;
    size_t got;
    uint8_t* park;

    snprintf(path, sizeof path, "%s/../assets/s3/gems_s1pack.bin", A->data_dir(SELF));
    f = fopen(path, "rb");
    if (!f) { A->log(SELF, OP_MOD_LOG_WARN, "no asset pack at %s — calibration mode (run tools/ generators)", path); return 0; }
    got = fread(&s_pack, 1, sizeof s_pack, f);
    if (got != sizeof s_pack || s_pack.magic != PACK_MAGIC ||
        s_pack.payload_size == 0 ||
        PARK_BASE + s_pack.payload_size > PARK_LIMIT) {
        A->log(SELF, OP_MOD_LOG_ERROR, "bad pack header (magic=%08x size=%u)", s_pack.magic, s_pack.payload_size);
        fclose(f); return 0;
    }
    park = (uint8_t*)A->guest(PARK_BASE);
    if (!park) { fclose(f); return 0; }
    got = fread(park, 1, s_pack.payload_size, f);
    fclose(f);
    if (got != s_pack.payload_size) {
        A->log(SELF, OP_MOD_LOG_ERROR, "pack payload truncated (%zu/%u)", got, s_pack.payload_size);
        return 0;
    }
    return 1;
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    A = api; SELF = self;

    s_have_pack = load_pack();
    if (s_have_pack) {
        A->log(SELF, OP_MOD_LOG_INFO,
               "pack loaded: payload=%u bytes @%08x, vag=%u bytes, pitch_base=%03x",
               s_pack.payload_size, PARK_BASE, s_pack.vag_size, s_pack.pitch_base);
    }

    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers, on_patch_models) ||
        api->override_addr(self, OP_FNADDR_SetNewSoundTable, on_sound_table) ||
        api->override_addr(self, FN_GEM_COLLECT, on_gem_collect)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "override registration failed");
        return 1;
    }
    if (getenv("S3G_DUMP_VERTS") &&
        api->override_addr(self, OP_FNADDR_CameraUpdate, on_camera_update_dump)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "S3G_DUMP_VERTS hook registration failed");
        return 1;
    }
    return 0;
}
