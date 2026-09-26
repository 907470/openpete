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

/** @brief Classify a pack file's basename as a hash-form, symbolic or ignored name.
 *
 * The name must end in ".wav" (any case); an optional "-flat" before it
 * (any case) sets flat on every form and is stripped. The hash form is a
 * run of 16 hex digits, either case, that starts the stem or follows a
 * '-', and is followed by the end of the stem or by "-p" and 4 hex digits
 * (the pitch). Any text before the run is the label and is discarded. The
 * first qualifying run from the start of the stem is taken. Any other
 * .wav whose stem is under 64 bytes is symbolic, its stem copied to name.
 * @param basename File name without directory.
 * @param out Receives the parse; pitch is -1 unless tagged.
 * @return OP_SFX_NAME_HASH, OP_SFX_NAME_SYMBOLIC, or OP_SFX_NAME_NONE for
 *         a non-.wav name or a symbolic stem of 64 bytes or more. */
int  op_sfx_name_parse(const char* basename, op_sfx_name* out);

/** @brief Write the canonical hash-form file name, "<label>-<hash16>[-p<pitch4>].wav".
 *
 * The hash is written as 16 lowercase hex digits and the pitch as 4. With
 * no label the leading "<label>-" is omitted. The -flat qualifier is
 * never written.
 * @param out Destination.
 * @param cap Capacity of out.
 * @param label Label prefix; NULL or "" for none.
 * @param hash XXH3-64 of the ADPCM body.
 * @param pitch Base pitch for the tagged form; negative writes the untagged form.
 * @return 0 on success, -1 when the name does not fit in cap. */
int  op_sfx_name_format(char* out, size_t cap, const char* label,
                        uint64_t hash, int pitch);

/** @brief Write 16-bit mono PCM as a WAV file, with a `smpl` chunk carrying one forward loop when loop_len > 0.
 *
 * The chunks are `fmt `, then `smpl` when present, then `data`, all
 * little-endian. The loop's end sample is loop_start + loop_len - 1,
 * inclusive; the sfx pack loader reads the chunk back to rebuild the
 * voice's loop.
 * @param path Output file path.
 * @param pcm Samples.
 * @param count Sample count.
 * @param rate Header sample rate in Hz.
 * @param loop_start First looped sample.
 * @param loop_len Looped sample count; 0 or less writes no `smpl` chunk.
 * @return 0 on success, -1 when the file cannot be opened or a write fails. */
int  op_wav_write_mono16(const char* path, const int16_t* pcm,
                         uint32_t count, uint32_t rate,
                         int loop_start, int loop_len);

/** @brief Return the WAV header rate at which a tagged file sounds as in-game at its base pitch in any player.
 *
 * The rate is round(44100 * pitch / 0x1000), rounding half up; the pack
 * loader applies the same function, so a tagged dump loaded unmodified
 * lands within 1 Hz of stock.
 * @param pitch SPU pitch, 0x1000 = 44.1 kHz.
 * @return Sample rate in Hz. */
uint32_t op_sfx_rate_for_pitch(int pitch);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SFX_IDENT_H */
