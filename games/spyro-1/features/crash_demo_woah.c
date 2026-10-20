// games/spyro-1/features/crash_demo_woah.c - the Crash 3 demo cheat, ported
// as an easter egg.
//
// Holding L1+Triangle on the title screen's "PRESS START" state makes the
// original game shut the engine down (display mask, pad comms, callbacks)
// and LoadExec cdrom:\S0\CRASH.EXE - a Crash Bandicoot 3 demo shipped on the
// Spyro disc. openpete cannot host a second EXE (LoadExec is a no-op thunk),
// so unintercepted the guest returns from CrashDemoLaunch into the engine it
// just tore down: masked display, dead pad, black screen.
//
// Instead the cheat becomes a sound: Crash's "woah!" plays in place of the
// UI blip the branch fires, and the title screen carries on untouched. The
// sample is the player's own - decoded out of the demo's level data on the
// mounted disc image (S0/S000000D.NSF), never staged on disk; nothing ships
// in the binary.
//
// Three interceptions make that work, all gated on the exact state the cheat
// branch tests (titlescreen overlay, substate 3, combo held), so every other
// use of these functions is untouched:
//
//   KillSoundsAndMusic() - the branch's first act. On hardware it is moot
//   (the program is about to be replaced); here it stops the title track for
//   good and, worse, the branch then spins in a busy-wait until the CD has
//   fully stopped, which costs a visible hitch every tick the combo is held.
//   Swallowed. Its wait loop tests g_CdMusic.m_Flags for the SILENT bit, so
//   the bit is spoofed for the handful of instructions between the swallow
//   and CrashDemoLaunch and restored there - the loop falls straight
//   through, the CD is never stopped, and there is nothing to restart.
//
//   TitlescreenPlaySound(9) - the back/cancel blip. Left to run, with a
//   one-shot sample substitution armed underneath it (sfx_pack_arm_oneshot),
//   so the game's own voice plays the woah: correct mixing, volume and mute
//   for free, and no second audio path. Later ticks of the same held press
//   are suppressed so the woah is not retriggered every frame.
//
//   CrashDemoLaunch() - swallowed whole; its body's shutdown sequence
//   assumes LoadExec never returns.
//
// The NSF is 2.5 MB and is read straight off the disc image, outside the
// guest's CD path and its cache, so the decode runs on a worker thread
// started when the title screen becomes resident - seconds before the combo
// can be pressed - rather than inside the frame that presses it.
//
// NSF format (Crash 2/3 engine): a sequence of 64 KiB chunks, u16 magic
// 0x1234 + u16 type at +2 (type 3 = sound chunk; Spyro 1's demo NSFs are all
// stored uncompressed, so the 0x1235 LZ form is not handled). An entry chunk
// holds u32 entry-count at +8 and entry offsets from +16; each entry is
// u32 magic 0x100FFFF, eID, type (12 = sound), item count, item offsets.
// A sound entry's single item is raw SPU-ADPCM 16-byte lines. eIDs pack a
// 5-char name into 6-bit fields; "arg0A" (Crash's woah/aargh yell) is
// 0x14DA0049.

#include "crash_demo_woah.h"

#include "feature_registry.h"

#include "openpete.h"       /* openpete_options_t */
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_psx.h"   /* openpete_psx_load_file_from_disc */
#include "psx_runtime_internal.h"    /* CPUState */
#include "sfx_pack.h"       /* sfx_pack_arm_oneshot */
#include "spyro_overlay_table.h" /* spyro_get_active_overlay_magic */

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADDR_CRASH_DEMO_LAUNCH 0x8002BAB8u
#define ADDR_KILL_SOUNDS_MUSIC 0x80056B28u

/* Titlescreen overlay (id 48 in the overlay table). Overlays share one
 * vaddr window, so a hook on an overlay-resident address must confirm the
 * right overlay is the one currently resident. */
#define ADDR_TS_PLAY_SOUND     0x8007AAD4u
#define OVERLAY_TITLESCREEN    48u
#define TS_SOUND_BACK          9    /* the blip the cheat branch fires */
#define TS_SUBSTATE_PRESS_START 3
#define ADDR_TS_SUBSTATE       0x80078D88u /* g_TitlescreenState.m_SubState */

#define PAD_L1                 (1u << 2)
#define PAD_TRIANGLE           (1u << 4)
#define CHEAT_COMBO            (PAD_L1 | PAD_TRIANGLE)

#define ADDR_G_PAD_HELD        0x80077380u /* g_Pad 0x80077378 + m_Held @ 0x8 */
/* g_CdMusic @ 0x800774B0, m_Flags at +0x4 (CdMusic layout in music.h). */
#define ADDR_G_CDMUSIC_FLAGS   0x800774B4u
#define MUSIC_FLAG_SILENT      0x40 /* set once playback has fully stopped */

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
static int      s_press_handled;   /* this held press already got its woah */
static int      s_flags_spoofed;   /* g_CdMusic.m_Flags carries a fake SILENT */
static uint32_t s_saved_flags;
static unsigned s_arm_ticks;       /* ticks the pending substitution has waited */

/* The arm is spent by the key-on in SoundsUpdate(), which for this branch
 * lands on a later tick than the PlaySound that queued it. Drop it if that
 * key-on never comes, so a stale arm cannot land on an unrelated sound. */
#define ARM_EXPIRY_TICKS 30u

/** @brief Loads a 32-bit word from an unaligned byte pointer in host byte order. */
static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/** @brief Decodes SPU-ADPCM lines to mono s16, stopping before the loop-end line.
 *
 * Each 16-byte line carries a shift and filter nibble, a flag byte and 28
 * four-bit samples; a line whose flag bit 0 is set ends the decode before
 * it is decoded, and a filter index above 4 is treated as 0. Samples clamp
 * to the s16 range.
 * @param body The ADPCM lines.
 * @param len The byte length of body; a trailing partial line is ignored.
 * @param out Receives the malloc'd sample buffer, which the caller frees;
 *        untouched when the allocation fails.
 * @return The sample count, or 0 when the allocation fails. */
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

/** @brief Finds the woah sound entry's ADPCM item in the demo NSF's raw bytes.
 *
 * Walks every uncompressed sound chunk rather than trusting a chunk index,
 * so a re-mastered image with the same payload still resolves. The first
 * entry with the entry magic, eID WOAH_EID and the sound type yields its
 * first item, the bytes between item offsets 0 and 1. A chunk declaring
 * more than 256 entries is skipped.
 * @param nsf The NSF file's bytes.
 * @param len The byte length of nsf.
 * @param body Receives the item's start when found.
 * @return The item's byte length, or 0 when absent. */
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

/* Decode-once, keep the PCM. ~10 KB held for the session, decoded on a
 * worker thread (the disc read is 2.5 MB; it is served by the disc cache
 * where resident and from the file otherwise, and as a host read it never
 * reorders the cache's loading) and published through s_ready. */
static int16_t*      s_pcm;
static uint32_t      s_pcm_frames;
static SDL_AtomicInt s_ready;   /* 0 = pending/absent, 1 = s_pcm usable */
static SDL_Thread*   s_loader;

/** @brief Worker thread: loads the demo NSF, finds the woah entry and decodes it.
 *
 * Reads WOAH_NSF from s_disc_path, locates the sound with find_woah, decodes
 * it with adpcm_to_pcm, stores s_pcm and s_pcm_frames, and sets s_ready to 1
 * last so the tick thread sees both. A failed disc read (logged by the
 * loader), a missing entry (warned) or an empty decode (warned) leaves s_ready
 * at 0. The NSF buffer is freed on every path; the PCM buffer is kept for the
 * session.
 * @param unused Unused.
 * @return 0. */
static int SDLCALL woah_load_main(void* unused) {
    (void)unused;
    size_t nsf_len = 0;
    int scope = openpete_disc_trace_scope_begin(OPENPETE_DISC_TAG_WOAH);
    uint8_t* nsf = openpete_psx_load_file_from_disc(s_disc_path, WOAH_NSF, &nsf_len);
    openpete_disc_trace_scope_end(scope);
    if (!nsf) return 0; /* loader logged why (e.g. a demo-less disc) */

    const uint8_t* body = NULL;
    uint32_t body_len = find_woah(nsf, nsf_len, &body);
    if (!body_len) {
        OPL_WARN_F("game", "crash-woah: sound entry not found in %s", WOAH_NSF);
        free(nsf);
        return 0;
    }
    int16_t* pcm = NULL;
    uint32_t frames = adpcm_to_pcm(body, body_len, &pcm);
    free(nsf);
    if (!frames) {
        free(pcm);
        OPL_WARN_F("game", "%s", "crash-woah: ADPCM decode produced no audio");
        return 0;
    }
    s_pcm = pcm;
    s_pcm_frames = frames;
    SDL_SetAtomicInt(&s_ready, 1);   /* publishes s_pcm / s_pcm_frames */
    OPL_INFO_F("game", "crash-woah: decoded %u frames from %s", frames, WOAH_NSF);
    return 0;
}

/** @brief Starts the woah decode on a worker thread, once per process.
 *
 * Called each tick the title screen is resident; the title sequence runs
 * for seconds before substate 3 takes the combo. Does nothing without a disc
 * path or after a first attempt, including one whose thread creation
 * failed. The thread is joined by crash_demo_woah_shutdown. */
static void woah_load_start(void) {
    if (s_loader || !s_disc_path[0]) return;
    s_loader = SDL_CreateThread(woah_load_main, "openpete-crashwoah", NULL);
    if (!s_loader) s_loader = (SDL_Thread*)-1;  /* thread refused; do not retry per tick */
}

/** @brief Joins the woah decode thread when one was started.
 *
 * The feature module's shutdown callback, run before SDL_Quit. Waits for a
 * decode still in progress; the decoded PCM stays allocated. */
static void crash_demo_woah_shutdown(void) {
    if (s_loader && s_loader != (SDL_Thread*)-1) SDL_WaitThread(s_loader, NULL);
    s_loader = (SDL_Thread*)-1;  /* joined: never restarted */
}

/** @brief Reports whether the guest is inside the L1+Triangle branch of the title screen's substate 3.
 *
 * Tests the resident overlay, g_TitlescreenState.m_SubState and the held
 * pad buttons. Every hook is gated on this, so the same functions used
 * elsewhere on the title screen behave normally.
 * @param cpu The guest CPU state.
 * @return 1 inside the cheat branch, else 0. */
static int in_cheat_branch(CPUState* cpu) {
    return spyro_get_active_overlay_magic() == OVERLAY_TITLESCREEN &&
           cpu->read_word(ADDR_TS_SUBSTATE) == TS_SUBSTATE_PRESS_START &&
           (cpu->read_word(ADDR_G_PAD_HELD) & CHEAT_COMBO) == CHEAT_COMBO;
}

/** @brief Swallows the cheat branch's KillSoundsAndMusic(0) so the title track keeps playing.
 *
 * The CD is never stopped. The branch then spins until
 * g_CdMusic.m_Flags reports playback fully stopped, which will now never
 * happen, so hand it the SILENT bit for the few instructions until
 * CrashDemoLaunch restores the real value. Nothing else reads m_Flags in
 * between: the wait loop's body (CDLoadTime / SoundsUpdate /
 * CDMusicUpdate) is what the spoof skips. Outside the cheat branch the call
 * runs normally.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return 1 to skip the guest function inside the cheat branch, else 0. */
static int on_kill_sounds_and_music(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!in_cheat_branch(cpu)) return 0;
    if (!s_flags_spoofed) {
        s_saved_flags = cpu->read_word(ADDR_G_CDMUSIC_FLAGS);
        cpu->write_word(ADDR_G_CDMUSIC_FLAGS, s_saved_flags | MUSIC_FLAG_SILENT);
        s_flags_spoofed = 1;
    }
    return 1;
}

/** @brief Swallows CrashDemoLaunch(), first restoring the g_CdMusic.m_Flags that on_kill_sounds_and_music spoofed.
 *
 * The function is reached only from the cheat branch. It is skipped because
 * its shutdown sequence (SetDispMask, PadStopCom, callback clears) assumes
 * LoadExec never returns.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused).
 * @return 1, always skipping the guest function. */
static int on_crash_demo_launch(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (s_flags_spoofed) {
        cpu->write_word(ADDR_G_CDMUSIC_FLAGS, s_saved_flags);
        s_flags_spoofed = 0;
    }
    /* Always swallow: the body's shutdown sequence (SetDispMask,
     * PadStopCom, callback clears) assumes LoadExec never returns. */
    return 1;
}

/** @brief Replaces the cheat branch's back/cancel blip with the woah on the first tick of a press.
 *
 * On the first tick of a press the woah is armed as a one-shot sample
 * substitution and the guest's own voice plays it; while the woah is not
 * decoded the blip plays unchanged. On the remaining ticks of the same held
 * press the call is suppressed so the sound is not retriggered every tick.
 * Sound 9 is the generic back/cancel blip used all over the title screen;
 * the sound-id and cheat-branch gates keep every other use audible.
 * @param cpu The guest CPU state; a0 holds the sound id.
 * @param addr The hooked address (unused).
 * @return 1 to skip the guest call on a repeat tick of the press, else 0. */
static int on_titlescreen_play_sound(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if ((int)cpu->a0 != TS_SOUND_BACK) return 0;
    if (!in_cheat_branch(cpu)) return 0;
    if (s_press_handled) {
        s_blips_suppressed++;
        return 1;
    }
    s_press_handled = 1;
    if (!SDL_GetAtomicInt(&s_ready))
        return 0;  /* woah not decoded (yet, or at all) - the blip stands in */
    sfx_pack_arm_oneshot(s_pcm, (int)s_pcm_frames, WOAH_RATE);
    s_arm_ticks = 0;
    OPL_INFO_F("game", "crash demo cheat: suppressing LoadExec teardown, playing"
               " the woah in place of the blip (%u repeat blip(s) muted so far)",
               s_blips_suppressed);
    return 0;
}

/* Declared in crash_demo_woah.h. */
void spyro1_crash_woah_on_tick(CPUState* cpu) {
    if (spyro_get_active_overlay_magic() != OVERLAY_TITLESCREEN) return;
    woah_load_start();

    if ((cpu->read_word(ADDR_G_PAD_HELD) & CHEAT_COMBO) != CHEAT_COMBO)
        s_press_handled = 0;

    if (sfx_pack_oneshot_armed() && ++s_arm_ticks > ARM_EXPIRY_TICKS)
        sfx_pack_disarm_oneshot();
}

/** @brief Copies opts->disc_path into s_disc_path, the image the woah loader reads.
 *
 * The path is truncated to the buffer. With no disc path the buffer stays
 * empty and woah_load_start never starts the loader.
 * @param opts The options, or NULL. */
static void crash_demo_woah_apply_options(const openpete_options_t* opts) {
    if (opts && opts->disc_path)
        snprintf(s_disc_path, sizeof s_disc_path, "%s", opts->disc_path);
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_KILL_SOUNDS_MUSIC, on_kill_sounds_and_music, 0u },
    { ADDR_TS_PLAY_SOUND,     on_titlescreen_play_sound, 0u },
    { ADDR_CRASH_DEMO_LAUNCH, on_crash_demo_launch, 0u },
    { 0, NULL, 0u },
};

const feature_module_t crash_demo_woah_module = {
    .name           = "crash-demo-woah",
    .apply_options  = crash_demo_woah_apply_options,
    .dispatch_table = k_dispatch,
    .shutdown       = crash_demo_woah_shutdown,
};
