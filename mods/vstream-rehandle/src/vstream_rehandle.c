/* vstream-rehandle — route every animated model's vertex streams through
 * stream handles (see mod.toml).
 *
 * Model layout, from the game's own headers: a g_Models row with bit 31
 * set is an animated Model (m_NumAnimations at +0, m_Animations[] at
 * +0x38); an AnimationHeader has m_NumFrames at +0, m_IsSpyroAnimation at
 * +4, the vertex counts at +8 (high detail) and +9 (low), and m_Frames at +0x24 —
 * 8-byte AnimationFrame records (word 0 = stream field, word 1 byte 3 =
 * short offset) for mobys, 4-byte SpyroAnimationFrame words (field
 * halved) for Spyro's own model.
 *
 * A stream's extent is not stored; the readers consume exactly the vertex
 * count in literals (4 B each, from the stream start) and shorts (2 B
 * each, from the short offset), each entry's bit 0 selecting the next, so
 * walking the stream the way the renderer does gives the exact extent.
 */
#include <openpete_mod_api.h>
#include "openpete_sdk_symbols.h"
#include <string.h>

#define MODEL_COUNT 512u
#define HANDLE_BOUND 0x8000u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int g_mode = 1;
static unsigned g_levels, g_frames, g_bytes;

/* Exact byte extent of a moby stream: the renderer's own walk. */
static unsigned stream_extent(uint32_t stream, unsigned shortoff, unsigned nverts) {
    uint32_t litp = stream, shortp = stream + shortoff * 4u;
    int flag = 0;
    for (unsigned i = 0; i < nverts; i++) {
        if (i == 0 || flag == 0) { flag = (int)(op_read_u32(g_api, litp) & 1u); litp += 4; }
        else { flag = (int)(op_read_u32(g_api, shortp) & 1u); shortp += 2; }
    }
    uint32_t end = litp > shortp ? litp : shortp;
    return (unsigned)(end - stream) + 4u;
}

/* The game re-runs its model-install pass several times per level over
 * the same data, so a stream already copied is reused, never re-copied. */
#define COPIES_MAX 8192
static struct { uint32_t orig, copy; } g_copies[COPIES_MAX];
static unsigned g_copies_n;

static uint32_t stream_copy(uint32_t stream, unsigned extent) {
    for (unsigned i = 0; i < g_copies_n; i++)
        if (g_copies[i].orig == stream) return g_copies[i].copy;
    if (g_copies_n >= COPIES_MAX) return 0;
    void* host = NULL;
    uint32_t copy = g_api->guest_alloc(g_self, extent, 16,
                                       OP_GALLOC_IMMUTABLE, &host);
    const void* src = g_api->guest(stream);
    if (!copy || !host || !src) return 0;
    memcpy(host, src, extent);
    g_bytes += extent;
    g_copies[g_copies_n].orig = stream;
    g_copies[g_copies_n].copy = copy;
    g_copies_n++;
    return copy;
}

static uint32_t rehandle_field(uint32_t field, int halved, unsigned extent) {
    uint32_t stream = halved ? field << 1 : field;
    stream |= 0x80000000u;
    if (g_mode == 2 && extent) {
        stream = stream_copy(stream, extent);
        if (!stream) return 0;
    }
    return g_api->vstream_handle(g_self, stream);
}

static void rehandle_level(CPUState* cpu) {
    (void)cpu;
    unsigned frames = 0;
    for (uint32_t i = 0; i < MODEL_COUNT; i++) {
        uint32_t row = op_read_u32(g_api, OP_GADDR_g_Models + i * 4u);
        if (!(row & 0x80000000u)) continue;          /* empty or simple */
        int32_t nanim = (int32_t)op_read_u32(g_api, row);
        if (nanim <= 0 || nanim > 256) continue;
        for (int32_t a = 0; a < nanim; a++) {
            uint32_t ah = op_read_u32(g_api, row + 0x38u + (uint32_t)a * 4u);
            if (!ah) continue;
            ah |= 0x80000000u;
            int nframes = (int16_t)(op_read_u32(g_api, ah) & 0xFFFFu);
            uint32_t w1 = op_read_u32(g_api, ah + 4u);
            int spyro = (int)(w1 & 0xFFu);
            /* +8 = high-detail vertex count, +9 = low-detail; one stream
             * serves both, so size it by the larger. */
            uint32_t w2 = op_read_u32(g_api, ah + 8u);
            unsigned nhi = w2 & 0xFFu, nlo = (w2 >> 8) & 0xFFu;
            unsigned nverts = nhi > nlo ? nhi : nlo;
            if (nframes <= 0 || nframes > 4096) continue;
            for (int f = 0; f < nframes; f++) {
                uint32_t fa = ah + 0x24u + (uint32_t)f * (spyro ? 4u : 8u);
                uint32_t w0 = op_read_u32(g_api, fa);
                uint32_t field = w0 & 0x1FFFFFu;
                if (field < HANDLE_BOUND) continue;   /* already a handle */
                unsigned shortoff = spyro ? 0u : (op_read_u32(g_api, fa + 4u) >> 24);
                uint32_t stream = (spyro ? field << 1 : field) | 0x80000000u;
                /* A Spyro animation header carries no vertex count (its
                 * other header words are unused), so its streams cannot be
                 * sized here: mode 2 leaves them in place (mode 1 shape). */
                unsigned extent = spyro ? 0u : stream_extent(stream, shortoff, nverts);
                uint32_t h = rehandle_field(field, spyro, extent);
                if (!h) continue;
                op_write_u32(g_api, fa, (w0 & ~0x1FFFFFu) | h);
                frames++;
            }
        }
    }
    if (frames) {
        g_levels++; g_frames += frames;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "vstream-rehandle: level rehandled — %u frame words (mode %d, %u B copied)",
                   frames, g_mode, g_bytes);
    }
}

static void on_patch_models(CPUState* cpu) {
    g_api->base(cpu);          /* the game installs the level's models */
    rehandle_level(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_mode = (int)api->config_int(self, "mode", 1);
    if (g_mode < 1 || g_mode > 2) g_mode = 1;
    return api->override_name(self, "PatchMobyModelPointers", on_patch_models);
}
