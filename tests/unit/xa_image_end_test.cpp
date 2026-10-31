/* tests/unit/xa_image_end_test.cpp - the XA decoder's reads stay inside the
 * disc image when the drive head runs past the image's last sector.
 *
 * A stream the game leaves running without a CdlPause keeps the drive head
 * moving; past the last sector of a one-track image the head is in the
 * lead-out, where no sector exists. PsyX_XA.cpp is compiled into this TU,
 * and the image reader it calls (vfseek, vfread) is replaced by one that
 * records every byte range requested. The test streams a synthetic image of
 * XA audio sectors and pumps the decoder with the head advancing past the
 * image's end in small steps and in one jump (the catch-up reseek path), and
 * checks that no read reaches past the image, that the decoder read the
 * image's last sector (engagement), and that it decoded audio. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"     // warning-ratchet: PsyCross source compiled in unchanged; its stubs leave parameters unused
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"  // warning-ratchet: PsyCross source compiled in unchanged; ADPCM decode history GCC cannot prove set
#include "../../third_party/PsyCross/src/audio/PsyX_XA.cpp"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <vector>

namespace {

constexpr int kImageSectors = 64;
std::vector<uint8_t> s_image;
size_t s_reads_past_end = 0;   ///< read requests that reached past the image
size_t s_reads = 0;            ///< read requests
long long s_pending = 0;       ///< frames held by the stubbed CD input

}  // namespace

VFILE g_imageFile = { NULL, NULL, 0, 0 };
extern "C" {
int g_cdSectorSize = 2352;
}

/** @brief Image read: copy the in-image part of the request and record a request that reaches past the image.
 *
 * @param buf Receives up to es * ec bytes.
 * @param es Element size.
 * @param ec Element count.
 * @param s The image.
 * @return The whole elements read. */
size_t vfread(void* buf, size_t es, size_t ec, VFILE* s) {
    const size_t want = es * ec;
    s_reads++;
    if (s->pos + want > s->size) s_reads_past_end++;
    const size_t n = s->pos >= s->size ? 0 : (want < s->size - s->pos ? want : s->size - s->pos);
    memcpy(buf, s_image.data() + s->pos, n);
    memset((uint8_t*)buf + n, 0, want - n);
    s->pos += n;
    return es ? n / es : 0;
}

/** @brief Image seek; SEEK_SET only, as the decoder uses it.
 *
 * @param s The image.
 * @param off Byte offset.
 * @param origin SEEK_SET.
 * @return 0. */
int vfseek(VFILE* s, int off, int origin) {
    if (origin == SEEK_SET) s->pos = (size_t)off;
    return 0;
}

/** @brief Image position.
 *
 * @param s The image.
 * @return The byte offset. */
int vftell(VFILE* s) { return (int)s->pos; }

/** @brief Whether an image is attached.
 *
 * @return 1. */
int PsyX_IsCDImageInit() { return 1; }

/** @brief Read-source tag stub.
 *
 * @param source Ignored.
 * @return 0. */
extern "C" int PsyX_CDFS_SetReadSource(int source) { (void)source; return 0; }

/** @brief CD input stub: accepts every frame and counts it as pending.
 *
 * @param pcm Ignored.
 * @param frames Frames pushed.
 * @param channels Ignored.
 * @return frames. */
int PsyX_SPUAL_CDInputPush(const short* pcm, int frames, int channels) {
    (void)pcm;
    (void)channels;
    s_pending += frames;
    return frames;
}
/** @brief CD input stub: the pending frame count.
 *
 * @return The frames pushed since the last clear. */
int PsyX_SPUAL_CDInputPending(void) { return (int)s_pending; }
/** @brief CD input stub: drops the pending frames. */
void PsyX_SPUAL_CDInputClear(void) { s_pending = 0; }
/** @brief CD input stub.
 *
 * @param gain Ignored. */
void PsyX_SPUAL_CDInputSetGain(float gain) { (void)gain; }

/** @brief Build an image of kImageSectors Mode 2 XA audio sectors, file 1 channel 0, stereo 37.8 kHz, with nonzero ADPCM data. */
static void build_image(void) {
    s_image.assign((size_t)kImageSectors * 2352u, 0u);
    for (int s = 0; s < kImageSectors; s++) {
        uint8_t* sec = s_image.data() + (size_t)s * 2352u;
        sec[15] = 2;           /* mode */
        uint8_t* user = sec + 16;
        user[0] = 1;           /* file */
        user[1] = 0;           /* channel */
        user[2] = 0x64;        /* submode: audio, form 2, real time */
        user[3] = 0x01;        /* coding: stereo, 37.8 kHz, 4-bit */
        for (int i = 8; i < 8 + 18 * 128; i++) user[i] = (uint8_t)(i * 7 + s);
    }
    g_imageFile.size = s_image.size();
}

/** @brief Stream from LBA 0 and pump with the head stepping past the image's end, then jumping far past it.
 *
 * @return 0 on pass, 1 on failure. */
int main(void) {
    build_image();
    constexpr int per = 294;   /* output frames per LBA at double speed */
    PsyX_XA_Start(0, 1, 0);
    for (int head = 1; head <= kImageSectors + 40; head++)
        PsyX_XA_PumpTo(head, (long long)(head - 1) * per, per);
    PsyX_XA_PumpTo(kImageSectors + 5000, (long long)(kImageSectors + 4999) * per, per);

    unsigned long long sectors = 0, image_end = 0;
    int max_lba = -1, image_sectors = 0;
    PsyX_XA_Stats(&sectors, NULL, NULL, NULL, NULL);
    PsyX_XA_EndStats(&image_end, &max_lba, &image_sectors);
    printf("xa_image_end_test: reads=%zu reads_past_end=%zu sectors_decoded=%llu "
           "image_end_pumps=%llu max_read_lba=%d image_sectors=%d\n",
           s_reads, s_reads_past_end, sectors, image_end, max_lba, image_sectors);
    int fail = 0;
    if (s_reads_past_end != 0) { printf("FAIL: a read reached past the image\n"); fail = 1; }
    if (max_lba != kImageSectors - 1) { printf("FAIL: the decoder did not read the image's last sector\n"); fail = 1; }
    if (image_end == 0) { printf("FAIL: no pump had its head past the image (VACUOUS)\n"); fail = 1; }
    if (sectors == 0) { printf("FAIL: no sector decoded (VACUOUS)\n"); fail = 1; }
    printf("verdict: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
