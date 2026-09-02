/* sfx_ident — the sound-effect pack's identity: how a sample's bytes
 * become a name, how a name becomes a lookup key, and the WAV writer
 * the census dumper and the pack share.
 *
 * A game sound is identified by the XXH3-64 of its SPU-ADPCM body (the
 * bytes from the sample's start address up to and including the first
 * END-flagged 16-byte block). The identity is content, not the role a
 * level assigns it: the same chime shipped in several level banks
 * hashes the same everywhere, and a hack that re-records a sound gets a
 * new hash rather than being silently overridden by a pack aimed at the
 * stock one.
 *
 * File names, in match precedence:
 *   <label>-<hash16>-p<pitch4>.wav   tagged: "at its own header rate this
 *                                     recording is the sound at base
 *                                     pitch <pitch>" (SPU units, 0x1000 =
 *                                     44.1 kHz)
 *   <label>-<hash16>.wav             untagged: raw sample at decoder rate;
 *                                     the game pitches it as stock
 *   <name>.wav                       symbolic: a SoundTable slot name
 * The label is organisational only and is stripped by the parser. */
#ifndef OPENPETE_SFX_IDENT_H
#define OPENPETE_SFX_IDENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t op_sfx_hash(const void* adpcm, size_t len);

enum {
    OP_SFX_NAME_NONE     = 0, /* not a .wav, or a path the pack ignores */
    OP_SFX_NAME_HASH     = 1, /* <label>-<hash16>[-p<pitch4>].wav */
    OP_SFX_NAME_SYMBOLIC = 2, /* anything else ending in .wav */
};

typedef struct {
    int      kind;         /* OP_SFX_NAME_* */
    uint64_t hash;         /* HASH form */
    int      pitch;        /* HASH form: base pitch, or -1 when untagged */
    int      flat;         /* "-flat" qualifier: play at the file's own rate,
                            * ignoring the game's pitch writes (audible only) */
    char     name[64];     /* SYMBOLIC form: basename without .wav */
} op_sfx_name;

/* Parse a file's basename. Case-insensitive on the hex digits; a leading
 * label is any text before the 16-hex run. */
int  op_sfx_name_parse(const char* basename, op_sfx_name* out);

/* Compose the canonical file name. pitch < 0 writes the untagged form.
 * label may be NULL or empty. */
int  op_sfx_name_format(char* out, size_t cap, const char* label,
                        uint64_t hash, int pitch);

/* 16-bit mono WAV with an optional `smpl` loop chunk (loop_len > 0).
 * Returns 0 on success. */
int  op_wav_write_mono16(const char* path, const int16_t* pcm,
                         uint32_t count, uint32_t rate,
                         int loop_start, int loop_len);

/* The WAV header rate a tagged file is written at so it sounds as
 * in-game at base pitch in any player: round(44100 * pitch / 0x1000). */
uint32_t op_sfx_rate_for_pitch(int pitch);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SFX_IDENT_H */
