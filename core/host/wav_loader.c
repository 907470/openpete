#include "wav_loader.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "openpete_log.h"

int op_wav_load(const char* path, op_wav_pcm* out)
{
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;

    if (!SDL_LoadWAV(path, &spec, &buf, &len)) {
        OPL_WARN_F("music", "wav: %s: %s", path, SDL_GetError());
        return -1;
    }

    int format_ok = (spec.format == SDL_AUDIO_S16LE)
                 && (spec.channels == 1 || spec.channels == 2)
                 && (spec.freq == 11025 || spec.freq == 22050
                     || spec.freq == 32000 || spec.freq == 44100
                     || spec.freq == 48000);
    if (!format_ok) {
        OPL_WARN_F("music",
                   "wav: %s: unsupported format (fmt=0x%X ch=%d rate=%d); "
                   "need PCM int16 mono/stereo @ 11025/22050/32000/44100/48000",
                   path, (unsigned)spec.format, (int)spec.channels,
                   (int)spec.freq);
        SDL_free(buf);
        return -1;
    }

    out->samples     = (int16_t*)buf;
    out->frame_count = len / (uint32_t)(sizeof(int16_t) * spec.channels);
    out->channels    = (int)spec.channels;
    out->sample_rate = (int)spec.freq;
    return 0;
}

/** @brief Return the little-endian 16-bit value at p. */
static uint32_t rd_le16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/** @brief Return the little-endian 32-bit value at p. */
static uint32_t rd_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/** @brief Return 1 when a fmt chunk body describes a PCM layout op_wav_load accepts.
 *
 * 16-bit integer samples (format tag 1, or 0xFFFE whose sub-format GUID
 * starts with tag 1), 1 or 2 channels, a block of channels * 2 bytes, and
 * one of the five supported rates.
 * @param b The chunk body.
 * @param n Bytes of the body in @p b. */
static int fmt_supported(const uint8_t* b, uint32_t n)
{
    if (n < 16) return 0;
    uint32_t tag = rd_le16(b), ch = rd_le16(b + 2), rate = rd_le32(b + 4);
    uint32_t align = rd_le16(b + 12), bits = rd_le16(b + 14);
    if (tag == 0xFFFEu) tag = n >= 26 ? rd_le16(b + 24) : 0;
    return tag == 1 && bits == 16 && (ch == 1 || ch == 2) && align == ch * 2 &&
           (rate == 11025 || rate == 22050 || rate == 32000 || rate == 44100 ||
            rate == 48000);
}

/** @brief Read the next RIFF chunk header.
 * @param f Stream positioned at a chunk header.
 * @param id Receives the 4-byte chunk id.
 * @param size Receives the chunk body size (before padding).
 * @return 1 when a header was read, 0 at the end of the stream. */
static int riff_next(FILE* f, uint8_t id[4], uint32_t* size)
{
    uint8_t ck[8];
    if (fread(ck, 1, 8, f) != 8) return 0;
    memcpy(id, ck, 4);
    *size = rd_le32(ck + 4);
    return 1;
}

/** @brief Seek past the rest of a chunk body, including its pad byte to an even size.
 * @param f Stream inside the chunk body.
 * @param size The chunk body size.
 * @param consumed Body bytes already read.
 * @return 1 on success, 0 when the seek fails. */
static int riff_skip(FILE* f, uint32_t size, uint32_t consumed)
{
    const uint64_t padded = ((uint64_t)size + 1u) & ~1ull;
    return fseek(f, (long)(padded - consumed), SEEK_CUR) == 0;
}

/** @brief Fill a WAV layout from a data chunk whose header was just read, leaving f at its first sample.
 *
 * The frame count is the chunk size clamped to the bytes the file holds
 * after the chunk start, over the block size.
 * @param f Stream positioned at the data chunk body.
 * @param size The data chunk body size.
 * @param fmt A supported fmt chunk body.
 * @param out Receives the layout.
 * @return 1 on success, 0 when the stream cannot be measured. */
static int data_layout(FILE* f, uint32_t size, const uint8_t* fmt, op_wav_info* out)
{
    const long off = ftell(f);
    if (off < 0 || fseek(f, 0, SEEK_END) != 0) return 0;
    const long end = ftell(f);
    if (end < off || fseek(f, off, SEEK_SET) != 0) return 0;
    const uint64_t avail = (uint64_t)(end - off);
    out->channels    = (int)rd_le16(fmt + 2);
    out->sample_rate = (int)rd_le32(fmt + 4);
    out->frame_count = (uint32_t)((size < avail ? size : avail) / rd_le16(fmt + 12));
    out->data_offset = off;
    return 1;
}

int op_wav_read_info(FILE* f, const char* path, op_wav_info* out)
{
    uint8_t h[12], id[4], fmt[40];
    uint32_t sz = 0, fmt_n = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        OPL_WARN_F("music", "wav: %s: not a RIFF/WAVE file", path);
        return -1;
    }
    while (riff_next(f, id, &sz)) {
        uint32_t used = 0;
        if (!memcmp(id, "data", 4)) {
            if (fmt_n && fmt_supported(fmt, fmt_n) && data_layout(f, sz, fmt, out)) return 0;
            break;
        }
        if (!memcmp(id, "fmt ", 4)) {
            fmt_n = sz < sizeof fmt ? sz : (uint32_t)sizeof fmt;
            used = (uint32_t)fread(fmt, 1, fmt_n, f);
            if (used != fmt_n) break;
        }
        if (!riff_skip(f, sz, used)) break;
    }
    OPL_WARN_F("music",
               "wav: %s: no supported PCM data; need PCM int16 mono/stereo "
               "@ 11025/22050/32000/44100/48000", path);
    return -1;
}

double op_wav_probe_seconds(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        OPL_WARN_F("music", "wav: %s: cannot open", path);
        return 0.0;
    }
    op_wav_info info;
    const int rc = op_wav_read_info(f, path, &info);
    fclose(f);
    if (rc != 0 || info.sample_rate <= 0) return 0.0;
    return (double)info.frame_count / (double)info.sample_rate;
}

void op_wav_free(op_wav_pcm* pcm)
{
    if (!pcm) return;
    SDL_free(pcm->samples);
    pcm->samples     = NULL;
    pcm->frame_count = 0;
}
