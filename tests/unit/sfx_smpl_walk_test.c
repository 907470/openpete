/* tests/unit/sfx_smpl_walk_test.c - the WAV `smpl` chunk walk ends on a
 * chunk size that runs past the file, and refuses loop points whose sum
 * leaves int.
 *
 * sfx_pack.c is compiled into this TU so its static wav_read_smpl runs on
 * crafted files: a chunk of size 0xFFFFFFF0 before the smpl chunk (a
 * backward seek on a 32-bit long, an endless walk), a valid smpl loop, and
 * a loop end of 0xFFFFFFF0. */
#include "../../core/host/sfx_pack.c"

/** @brief PsyCross stub. */
const u_char* PsyX_SPUAL_MemPtr(unsigned int* size_out) { if (size_out) *size_out = 0; return NULL; }
/** @brief PsyCross stub. */
void PsyX_SPUAL_SetSampleOverride(PsyX_SPUAL_SampleOverrideFn fn) { (void)fn; }
/** @brief PsyCross stub. */
int PsyX_SPUAL_SampleByteLength(const u_char* adpcm, int max_bytes) { (void)adpcm; (void)max_bytes; return 0; }
/** @brief PsyCross stub. */
int PsyX_SPUAL_DecodeSample(const u_char* adpcm, int bytes, short* out, int* loop_start, int* loop_len) {
    (void)adpcm; (void)bytes; (void)out; (void)loop_start; (void)loop_len; return 0;
}
/** @brief Mod registry stub: no mod is enabled. */
int openpete_mods_is_enabled(const char* id) { (void)id; return 0; }
/** @brief Mod registry stub: no asset roots. */
int openpete_mods_asset_root_count(void) { return 0; }
/** @brief Mod registry stub. */
const char* openpete_mods_asset_root(int i, const char** owner) { (void)i; if (owner) *owner = NULL; return NULL; }
/** @brief Paths stub: no data directory. */
int openpete_paths_data_dir(const char* leaf, char* out, size_t cap, int create) {
    (void)leaf; (void)out; (void)cap; (void)create; return -1;
}
int openpete_speculative;

/** @brief Store a little-endian word. */
static void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/** @brief Write RIFF/WAVE with one filler chunk of the declared size (8 real bytes) and a smpl loop s0..e0. */
static void write_wav(const char* path, uint32_t filler_size, uint32_t s0, uint32_t e0) {
    uint8_t b[12 + 16 + 8 + 60];
    memset(b, 0, sizeof b);
    memcpy(b, "RIFF", 4); le32(b + 4, sizeof b - 8); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "junk", 4); le32(b + 16, filler_size);
    memcpy(b + 28, "smpl", 4); le32(b + 32, 60);
    le32(b + 36 + 28, 1u); le32(b + 36 + 44, s0); le32(b + 36 + 48, e0);
    FILE* f = fopen(path, "wb");
    fwrite(b, 1, sizeof b, f);
    fclose(f);
}

/** @brief Runs the oversized-chunk, valid-loop and oversized-loop cases.
 * @return 0 when all pass. */
int main(void) {
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/sfx_smpl_walk_test.wav", tmp && *tmp ? tmp : "/tmp");
    int fail = 0, st = -1, ln = -1;

    write_wav(path, 8u, 100u, 199u);
    if (!wav_read_smpl(path, &st, &ln) || st != 100 || ln != 100) { fprintf(stderr, "FAIL valid loop\n"); fail++; }

    write_wav(path, 0xFFFFFFF0u, 100u, 199u);
    st = ln = -1;
    if (wav_read_smpl(path, &st, &ln) || st != -1) { fprintf(stderr, "FAIL oversized chunk walked on\n"); fail++; }

    write_wav(path, 8u, 0u, 0xFFFFFFF0u);
    st = ln = -1;
    if (wav_read_smpl(path, &st, &ln) || ln != -1) { fprintf(stderr, "FAIL oversized loop accepted (len=%d)\n", ln); fail++; }
    remove(path);
    if (fail) return 1;
    printf("sfx_smpl_walk_test: OK\n");
    return 0;
}
