#include "sfx_ident.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

uint64_t op_sfx_hash(const void* adpcm, size_t len)
{
    return (uint64_t)XXH3_64bits(adpcm, len);
}

uint32_t op_sfx_rate_for_pitch(int pitch)
{
    /* Round to nearest: the loader inverts this with the same rounding,
     * so a tagged dump loaded unmodified lands within 1 Hz of stock. */
    return (uint32_t)((44100ull * (unsigned)pitch + 0x800u) / 0x1000u);
}

static int hex_run(const char* s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

int op_sfx_name_parse(const char* basename, op_sfx_name* out)
{
    memset(out, 0, sizeof *out);
    out->kind  = OP_SFX_NAME_NONE;
    out->pitch = -1;
    size_t n = strlen(basename);
    if (n < 5) return OP_SFX_NAME_NONE;
    const char* ext = basename + n - 4;
    if (strcasecmp(ext, ".wav") != 0) return OP_SFX_NAME_NONE;
    size_t stem_len = n - 4;
    /* Optional trailing "-flat" qualifier on any form. */
    if (stem_len > 5 && strncasecmp(basename + stem_len - 5, "-flat", 5) == 0) {
        out->flat = 1;
        stem_len -= 5;
    }

    /* Hash form: find a 16-hex run that is either the whole stem, or
     * follows a '-' and is followed by end / "-p<4 hex>". The scan runs
     * from the start; a 16-hex word inside a label is skipped because the
     * end-or-pitch test fails after it. */
    for (size_t i = 0; i + 16 <= stem_len; i++) {
        if (i > 0 && basename[i - 1] != '-') continue;
        if (!hex_run(basename + i, 16)) continue;
        size_t after = i + 16;
        int pitch = -1;
        if (after == stem_len) {
            /* untagged */
        } else if (after + 6 == stem_len && basename[after] == '-' &&
                   basename[after + 1] == 'p' &&
                   hex_run(basename + after + 2, 4)) {
            pitch = (int)strtoul(basename + after + 2, NULL, 16);
        } else {
            continue;
        }
        char hex[17];
        memcpy(hex, basename + i, 16);
        hex[16] = '\0';
        out->kind  = OP_SFX_NAME_HASH;
        out->hash  = strtoull(hex, NULL, 16);
        out->pitch = pitch;
        return out->kind;
    }

    if (stem_len >= sizeof out->name) return OP_SFX_NAME_NONE;
    memcpy(out->name, basename, stem_len);
    out->name[stem_len] = '\0';
    out->kind = OP_SFX_NAME_SYMBOLIC;
    return out->kind;
}

int op_sfx_name_format(char* out, size_t cap, const char* label,
                       uint64_t hash, int pitch)
{
    const char* pre = (label && label[0]) ? label : "";
    const char* dash = pre[0] ? "-" : "";
    int n;
    if (pitch >= 0)
        n = snprintf(out, cap, "%s%s%016llx-p%04x.wav", pre, dash,
                     (unsigned long long)hash, (unsigned)pitch & 0xFFFFu);
    else
        n = snprintf(out, cap, "%s%s%016llx.wav", pre, dash,
                     (unsigned long long)hash);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static void put_u32(FILE* f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void put_u16(FILE* f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

int op_wav_write_mono16(const char* path, const int16_t* pcm,
                        uint32_t count, uint32_t rate,
                        int loop_start, int loop_len)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data_bytes = count * 2u;
    int has_loop = loop_len > 0;
    uint32_t smpl_bytes = has_loop ? (36u + 24u) : 0u;
    uint32_t riff = 4u + (8u + 16u) + (8u + data_bytes) +
                    (has_loop ? 8u + smpl_bytes : 0u);

    fwrite("RIFF", 1, 4, f); put_u32(f, riff); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16);
    put_u16(f, 1);            /* PCM */
    put_u16(f, 1);            /* mono */
    put_u32(f, rate);
    put_u32(f, rate * 2u);    /* byte rate */
    put_u16(f, 2);            /* block align */
    put_u16(f, 16);           /* bits */
    if (has_loop) {
        /* smpl: one forward loop. Samplers and DAWs read this chunk; the
         * pack loader reads it back to reconstruct the voice's loop. */
        fwrite("smpl", 1, 4, f); put_u32(f, smpl_bytes);
        put_u32(f, 0); put_u32(f, 0);                 /* manufacturer, product */
        put_u32(f, 1000000000u / (rate ? rate : 1)); /* sample period ns */
        put_u32(f, 60); put_u32(f, 0);                /* MIDI unity note, fraction */
        put_u32(f, 0); put_u32(f, 0);                 /* SMPTE */
        put_u32(f, 1); put_u32(f, 0);                 /* loop count, sampler data */
        put_u32(f, 0);                                /* cue id */
        put_u32(f, 0);                                /* forward */
        put_u32(f, (uint32_t)loop_start);
        put_u32(f, (uint32_t)(loop_start + loop_len - 1));
        put_u32(f, 0); put_u32(f, 0);                 /* fraction, play count */
    }
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    for (uint32_t i = 0; i < count; i++) put_u16(f, (uint16_t)pcm[i]);
    int rc = ferror(f) ? -1 : 0;
    fclose(f);
    return rc;
}
