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
 *  3. SFX overlay service: sfx_replace("gem_pickup", ...) stages the S3
 *              chime host-side and sfx_hook drives the rising ladder
 *              per play — guest RAM, sound tables and the gameplay
 *              rand() stream stay byte-stock (the service substitutes
 *              on a host copy of the voice attrs at key-on).
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

/* S3 pitch ladder (assets/s3/pitch_canonical.md): voice pitch =
 * 0x400 + 0x40*n, n resetting to 0 per burst (~0.25s cadence). Cap at
 * one octave and reset after ~0.5s so a normal gem trail re-anchors at
 * the canonical 11kHz chime instead of parking high. */
#define LADDER_MAX        16
#define CHIME_RESET_MS    500u

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

static pack_header_t s_pack;
static int      s_have_pack;
static int      s_calibrated;          /* stock-gem dump done */
static uint8_t* s_vag_host;            /* host copy for sfx_replace */
static uint32_t s_streak;

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

/* ── chime: SFX overlay service ──────────────────────────────────────
 * The whole guest-side dance this used to need (call_addr'd SpuWrite
 * upload, SoundDefinition repoint, variance zeroing that shifted the
 * gameplay rand() stream) now lives in the engine: sfx_replace stages
 * the sample host-side and the per-play hook drives the S3 ladder.
 * Guest RAM stays byte-stock. */

static void chime_hook(openpete_sfx_event_t* ev)
{
    if (ev->play_index == 0 || ev->ms_since_prev > CHIME_RESET_MS)
        s_streak = 0;
    ev->pitch = ladder_pitch(s_streak);
    if (s_streak + 1 < LADDER_MAX) s_streak++;
    A->log(SELF, OP_MOD_LOG_INFO, "chime: streak=%u pitch=%03x gap=%ums",
           s_streak, ev->pitch, ev->ms_since_prev);
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
    /* host copy of the chime body for the SFX overlay service (the
     * models still live in the guest park; the sample never needs to) */
    if (s_pack.vag_size &&
        s_pack.vag_vaddr >= PARK_BASE &&
        s_pack.vag_vaddr + s_pack.vag_size <= PARK_BASE + s_pack.payload_size) {
        s_vag_host = (uint8_t*)malloc(s_pack.vag_size);
        if (s_vag_host)
            memcpy(s_vag_host, park + (s_pack.vag_vaddr - PARK_BASE),
                   s_pack.vag_size);
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

    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers, on_patch_models)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "override registration failed");
        return 1;
    }
    if (s_have_pack && s_vag_host) {
        if (api->sfx_replace(self, "gem_pickup", s_vag_host, s_pack.vag_size,
                             (uint16_t)s_pack.pitch_base) ||
            api->sfx_hook(self, "gem_pickup", chime_hook)) {
            A->log(SELF, OP_MOD_LOG_ERROR, "sfx registration failed");
            return 1;
        }
    }
    if (getenv("S3G_DUMP_VERTS") &&
        api->override_addr(self, OP_FNADDR_CameraUpdate, on_camera_update_dump)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "S3G_DUMP_VERTS hook registration failed");
        return 1;
    }
    return 0;
}
