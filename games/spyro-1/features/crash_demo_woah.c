// games/spyro-1/features/crash_demo_woah.c — the Crash 3 demo cheat, ported
// as an easter egg.
//
// Holding L1+Triangle on the title screen's "PRESS START" state makes the
// original game shut the engine down (display mask, pad comms, callbacks)
// and LoadExec cdrom:\S0\CRASH.EXE — a Crash Bandicoot 3 demo shipped on the
// Spyro disc. openpete cannot host a second EXE (LoadExec is a no-op thunk),
// so unintercepted the guest returns from CrashDemoLaunch into the engine it
// just tore down: masked display, dead pad, black screen.
//
// Instead: intercept CrashDemoLaunch (0x8002BAB8) BEFORE the teardown runs
// and play Crash's "woah!" as a host-side one-shot. The sample is the
// player's own — decoded on first use straight out of the demo's level data
// on the mounted disc image (S0/S000000D.NSF) into an OpenAL buffer, never
// staged on disk; nothing ships in the binary. The title screen keeps running
// (the guest's own KillSoundsAndMusic has already silenced the music by the
// time the branch fires, which reads as intended drama).
//
// The branch also fires TitlescreenPlaySound(9) — a UI blip that on hardware
// is cut off within a few frames by the program swap, but here runs to
// completion over the top of the woah. The second hook below suppresses that
// one call. Sound 9 is the generic back/cancel blip used all over the title
// screen, so the hook is gated on the exact state the cheat branch tests
// (substate 3 with the combo held), leaving every other use audible.
//
// NSF format (Crash 2/3 engine): a sequence of 64 KiB chunks, u16 magic
// 0x1234 + u16 type at +2 (type 3 = sound chunk; Spyro 1's demo NSFs are all
// stored uncompressed, so the 0x1235 LZ form is not handled). An entry chunk
// holds u32 entry-count at +8 and entry offsets from +16; each entry is
// u32 magic 0x100FFFF, eID, type (12 = sound), item count, item offsets.
// A sound entry's single item is raw SPU-ADPCM 16-byte lines. eIDs pack a
// 5-char name into 6-bit fields; "arg0A" (Crash's woah/aargh yell) is
// 0x14DA0049.

#include "feature_registry.h"

#include "openpete.h"       /* openpete_options_t */
#include "openpete_fiber.h" /* openpete_frame (VBL counter) */
#include "openpete_log.h"
#include "openpete_mods.h"  /* openpete_mod_sound_play_pcm */
#include "openpete_psx.h"   /* openpete_psx_load_file_from_disc */
#include "psx_runtime.h"    /* CPUState */
#include "spyro_overlay_table.h" /* spyro_get_active_overlay_magic */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADDR_CRASH_DEMO_LAUNCH 0x8002BAB8u

/* Titlescreen overlay (id 48 in the overlay table). Overlays share one
 * vaddr window, so a hook on an overlay-resident address must confirm the
 * right overlay is the one currently resident. */
#define ADDR_TS_PLAY_SOUND     0x8007AAD4u
#define OVERLAY_TITLESCREEN    48u
#define TS_SOUND_BACK          9    /* the blip the cheat branch fires */
#define TS_SUBSTATE_PRESS_START 3

#define ADDR_G_PAD_HELD        0x80077380u /* g_Pad 0x80077378 + m_Held @ 0x8 */
#define ADDR_TS_SUBSTATE       0x80078D88u /* g_TitlescreenState.m_SubState */
#define PAD_L1                 (1u << 2)
#define PAD_TRIANGLE           (1u << 4)
#define CHEAT_COMBO            (PAD_L1 | PAD_TRIANGLE)

#define NSF_CHUNK_LEN   65536u
#define NSF_CHUNK_MAGIC 0x1234u
#define NSF_SOUND_CHUNK 3u
#define NSF_ENTRY_MAGIC 0x100FFFFu
#define NSF_ENTRY_SOUND 12u

#define WOAH_NSF  "S0/S000000D.NSF"
#define WOAH_EID  0x14DA0049u /* "arg0A" */
/* The NSF stores no playback rate (the Crash engine pitches per key-on);
 * 11025 Hz is the sample's natural voice pitch. */
#define WOAH_RATE 11025u

static char     s_disc_path[1024];
static unsigned s_blips_suppressed;

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Decode SPU-ADPCM lines to mono s16, stopping before the loop-end line
 * (flag bit 0) like the SPU's one-shot envelope would. Returns the frame
 * count; caller frees *out. */
static uint32_t adpcm_to_pcm(const uint8_t* body, uint32_t len, int16_t** out) {
    static const int k_f0[5] = {0, 60, 115, 98, 122};
    static const int k_f1[5] = {0, 0, -52, -55, -60};
    uint32_t max_frames = (len / 16u) * 28u;
    int16_t* pcm = (int16_t*)malloc(max_frames * 2u);
    if (!pcm) return 0;
    int s1 = 0, s2 = 0;
    uint32_t n = 0;
    for (uint32_t off = 0; off + 16u <= len; off += 16u) {
        int shift = body[off] & 0xF;
        int pred  = (body[off] >> 4) & 0xF;
        if (body[off + 1] & 0x1) break; /* loop end */
        if (pred > 4) pred = 0;
        for (int i = 0; i < 14; i++) {
            uint8_t b = body[off + 2u + (uint32_t)i];
            for (int hi = 0; hi < 2; hi++) {
                int nib = hi ? (b >> 4) : (b & 0xF);
                if (nib >= 8) nib -= 16;
                int s = ((nib << 12) >> shift) +
                        ((s1 * k_f0[pred] + s2 * k_f1[pred] + 32) >> 6);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                s2 = s1;
                s1 = s;
                pcm[n++] = (int16_t)s;
            }
        }
    }
    *out = pcm;
    return n;
}

/* Find the woah sound entry in the demo NSF's raw bytes. Walks every sound
 * chunk rather than trusting a chunk index, so a re-mastered image with the
 * same payload still resolves. Returns item length, 0 if absent. */
static uint32_t find_woah(const uint8_t* nsf, size_t len, const uint8_t** body) {
    for (size_t base = 0; base + NSF_CHUNK_LEN <= len; base += NSF_CHUNK_LEN) {
        const uint8_t* ch = nsf + base;
        if ((rd32(ch) & 0xFFFFu) != NSF_CHUNK_MAGIC) continue;
        if (((rd32(ch) >> 16) & 0xFFFFu) != NSF_SOUND_CHUNK) continue;
        uint32_t nent = rd32(ch + 8);
        if (nent > 256u) continue;
        for (uint32_t i = 0; i < nent; i++) {
            uint32_t es = rd32(ch + 16 + i * 4u);
            if (es + 24u > NSF_CHUNK_LEN) continue;
            if (rd32(ch + es) != NSF_ENTRY_MAGIC) continue;
            if (rd32(ch + es + 4) != WOAH_EID) continue;
            if (rd32(ch + es + 8) != NSF_ENTRY_SOUND) continue;
            uint32_t i0 = rd32(ch + es + 16);
            uint32_t i1 = rd32(ch + es + 20);
            if (i1 <= i0 || es + i1 > NSF_CHUNK_LEN) continue;
            *body = ch + es + i0;
            return i1 - i0;
        }
    }
    return 0;
}

/* Decode-once, keep the PCM. ~10 KB held for the session so every later
 * press is a cache hit in the AL layer with no disc read. Returns 0 on
 * success; failures are logged once and latched. */
static int16_t* s_pcm;
static uint32_t s_pcm_frames;

static int woah_load(void) {
    static int s_state = 0; /* 0 = untried, 1 = ok, -1 = failed */
    if (s_state) return s_state > 0 ? 0 : -1;
    s_state = -1;

    if (!s_disc_path[0]) {
        OPL_WARN_F("game", "%s", "crash-woah: no --disc image to extract from");
        return -1;
    }
    size_t nsf_len = 0;
    uint8_t* nsf = openpete_psx_load_file_from_disc(s_disc_path, WOAH_NSF, &nsf_len);
    if (!nsf) return -1; /* loader logged why (e.g. a demo-less disc) */

    const uint8_t* body = NULL;
    uint32_t body_len = find_woah(nsf, nsf_len, &body);
    if (!body_len) {
        OPL_WARN_F("game", "crash-woah: sound entry not found in %s", WOAH_NSF);
        free(nsf);
        return -1;
    }
    s_pcm_frames = adpcm_to_pcm(body, body_len, &s_pcm);
    free(nsf);
    if (!s_pcm_frames) {
        free(s_pcm);
        s_pcm = NULL;
        OPL_WARN_F("game", "%s", "crash-woah: ADPCM decode produced no audio");
        return -1;
    }
    OPL_INFO_F("game", "crash-woah: decoded %u frames from %s",
               s_pcm_frames, WOAH_NSF);
    s_state = 1;
    return 0;
}

static int on_crash_demo_launch(CPUState* cpu, uint32_t addr) {
    (void)cpu;
    (void)addr;
    /* The trigger is a HELD combo, and on real hardware the first firing
     * replaced the program — the branch could never run twice. Here the
     * title loop re-enters it every tick for as long as the buttons stay
     * down, so debounce to one woah per distinct press (~3 s). */
    static unsigned s_last_vbl = 0;
    unsigned now = openpete_frame;
    if (!s_last_vbl || now - s_last_vbl > 180u) {
        if (woah_load() == 0)
            openpete_mod_sound_play_pcm("crash3-woah", s_pcm, s_pcm_frames,
                                        1, WOAH_RATE, 1.0f);
        /* The blip count is the engagement proof for the companion hook:
         * a 0 here means the guest sting played over the woah. */
        OPL_INFO_F("game", "crash demo cheat: suppressing LoadExec teardown"
                   " (%u guest blip(s) muted), playing the woah instead",
                   s_blips_suppressed);
    }
    s_last_vbl = now;
    /* Always swallow the call: the body's shutdown sequence (SetDispMask,
     * PadStopCom, callback clears) assumes LoadExec never returns. */
    return 1;
}

/* Suppress the back/cancel blip the cheat branch fires just before
 * CrashDemoLaunch, so it does not play over the woah. Everything else that
 * uses sound 9 (menu backs, card prompts) falls through untouched. */
static int on_titlescreen_play_sound(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (spyro_get_active_overlay_magic() != OVERLAY_TITLESCREEN) return 0;
    if ((int)cpu->a0 != TS_SOUND_BACK) return 0;
    if (cpu->read_word(ADDR_TS_SUBSTATE) != TS_SUBSTATE_PRESS_START) return 0;
    if ((cpu->read_word(ADDR_G_PAD_HELD) & CHEAT_COMBO) != CHEAT_COMBO) return 0;
    s_blips_suppressed++;
    return 1;
}

static void crash_demo_woah_apply_options(const openpete_options_t* opts) {
    if (opts && opts->disc_path)
        snprintf(s_disc_path, sizeof s_disc_path, "%s", opts->disc_path);
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_TS_PLAY_SOUND,     on_titlescreen_play_sound },
    { ADDR_CRASH_DEMO_LAUNCH, on_crash_demo_launch },
    { 0, NULL },
};

const feature_module_t crash_demo_woah_module = {
    .name           = "crash-demo-woah",
    .apply_options  = crash_demo_woah_apply_options,
    .dispatch_table = k_dispatch,
};
