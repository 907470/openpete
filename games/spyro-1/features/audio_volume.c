// games/spyro-1/features/audio_volume.c — --sfx / --music: preset the
// game's own in-game volume sliders, and give the music slider an effect.
//
// Spyro's options menu carries a SOUND EFFECTS and a MUSIC row, each 0 (OFF)
// to 10 (max), backed by two globals plus the derived fields the audio code
// actually reads:
//
//   D_80075754  sfx slider    -> g_Spu.unk_0x320    = slider * 0x3FFF / 10
//                               g_Spu.m_SoundVolume = unk_0x320 * 0x1000 / 0x3FFF
//                                 (PlaySound scales every voice by this)
//   D_80075748  music slider  -> g_Spu.m_MusicVolume = slider << 11
//                               g_Spu.m_CommonAttr.cd.volume.{left,right}
//                                 (spu.c also refuses to start music at 0)
//
// PRESETS. --sfx / --music write those globals so a launch starts at the
// chosen level without visiting the menu. A one-shot write at boot would not
// survive: the game re-seeds both sliders to 10 every time it enters the title
// screen (gamestates/init.c func_8002D170), SpuInitialize re-seeds the derived
// fields alone (spu.c:25-31), and loading a save file restores whatever the
// card holds (save_file.c). So the preset is re-stamped whenever the guest
// values drift from it — but only while the player is not in the sound-options
// submenu. Moving that slider by hand is how the player takes the channel
// back: the preset is dropped (host request cleared to -1) and the menu owns
// it from then on.
//
// Every guest write happens here, on the tick hook — the dev overlay's rows
// only move the host-side request (core/runtime/run.c), so a UI-thread edit
// never lands in gameplay RAM off-schedule.
//
// MUSIC GAIN. PsyCross implements SpuSetCommonAttr as a no-op, so the cd
// volume the game computes never reaches the XA stream's OpenAL source: music
// played at full gain for every non-zero slider position. The mirror below
// closes that gap by driving the XA (and music-WAV override) gain from the
// game's own slider each tick, which is what makes both the menu row and
// --music N audible as levels rather than as an on/off switch. Host audio
// only, and it layers UNDER --mute: PsyX_XA_ForceGain still wins while the
// forced gain is active.
//
// Addresses from manifest/globals.toml; g_Spu field offsets from
// spyro-1/include/spu.h.

#include "features/audio_volume.h"

#include "openpete.h"        /* openpete_get/set_{sfx,music}_volume */
#include "music_override.h"  /* music_override_set_gain */
#include "psx_runtime_internal.h"     /* CPUState + its read/write callbacks */

#include <stdint.h>

/* PsyCross XA streaming source. SetVolume is the game-driven channel; the
 * forced gain (--mute) sits above it and keeps winning while it is set. */
extern void PsyX_XA_SetVolume(float linear_gain);
/* SPU main volume, as forwarded from SpuSetCommonAttr. */
extern float PsyX_SPUAL_GetMasterVolume(void);

/* Runahead speculation flag (openpete_fiber.h). Declared rather than
 * included: that header pulls in SDL, which would drag a window library into
 * this module's console unit test. */
extern int openpete_speculative;

#define ADDR_SFX_SLIDER      0x80075754u /* D_80075754, 0..10 */
#define ADDR_MUSIC_SLIDER    0x80075748u /* D_80075748, 0..10 */
#define ADDR_OPTIONS_SUBMENU 0x800757C8u /* D_800757C8, 1 = sound options open */

#define ADDR_G_SPU           0x80075F08u
#define ADDR_SPU_CD_VOLUME   (ADDR_G_SPU + 0x010u) /* m_CommonAttr.cd.volume, two shorts */
#define ADDR_SPU_MUSIC_VOL   (ADDR_G_SPU + 0x31Cu) /* m_MusicVolume */
#define ADDR_SPU_SFX_RAW     (ADDR_G_SPU + 0x320u) /* unk_0x320 */
#define ADDR_SPU_SOUND_VOL   (ADDR_G_SPU + 0x324u) /* m_SoundVolume */

#define OPTIONS_SUBMENU_SOUND 1u

/* --- per-channel stamping ------------------------------------------------ */

/* The game's own slider math, so the guest sees exactly the values its options
 * menu would have produced. Each writes the slider plus every derived field,
 * and reports the derived word the checker compares against. */

static uint32_t sfx_derived(int level) {
    uint32_t raw = (uint32_t)((level * 0x3FFF) / 10);
    return (raw * 0x1000u) / 0x3FFFu;   /* g_Spu.m_SoundVolume */
}

static void sfx_stamp(CPUState* cpu, int level) {
    uint32_t raw = (uint32_t)((level * 0x3FFF) / 10);
    cpu->write_word(ADDR_SFX_SLIDER,    (uint32_t)level);
    cpu->write_word(ADDR_SPU_SFX_RAW,   raw);
    cpu->write_word(ADDR_SPU_SOUND_VOL, (raw * 0x1000u) / 0x3FFFu);
}

static uint32_t music_derived(int level) {
    return (uint32_t)level << 11;       /* g_Spu.m_MusicVolume */
}

static void music_stamp(CPUState* cpu, int level) {
    uint32_t vol = (uint32_t)level << 11;
    cpu->write_word(ADDR_MUSIC_SLIDER,  (uint32_t)level);
    cpu->write_word(ADDR_SPU_MUSIC_VOL, vol);
    /* cd.volume is two shorts side by side: low half left, high half right. */
    cpu->write_word(ADDR_SPU_CD_VOLUME, (vol & 0xFFFFu) | (vol << 16));
}

typedef struct {
    uint32_t  slider_addr;
    uint32_t  derived_addr;
    uint32_t (*derived)(int level);
    void     (*stamp)(CPUState* cpu, int level);
    int      (*get_request)(void);
    void     (*set_request)(int level);

    int requested;   /* the request we are currently serving, -1 = none */
    int stamped;     /* the level we last wrote, -1 = never */
} vol_channel_t;

static vol_channel_t s_sfx = {
    ADDR_SFX_SLIDER, ADDR_SPU_SOUND_VOL, sfx_derived, sfx_stamp,
    openpete_get_sfx_volume, openpete_set_sfx_volume, -1, -1,
};

static vol_channel_t s_music = {
    ADDR_MUSIC_SLIDER, ADDR_SPU_MUSIC_VOL, music_derived, music_stamp,
    openpete_get_music_volume, openpete_set_music_volume, -1, -1,
};

static void channel_tick(CPUState* cpu, vol_channel_t* ch, int menu_open) {
    int want = ch->get_request();

    /* A new request — the boot seed, or a dev-overlay edit — starts over. */
    if (want != ch->requested) {
        ch->requested = want;
        ch->stamped   = -1;
    }
    if (want < 0) return;

    /* While the sound options are open the player owns the slider: stamping
     * would fight their input. Moving it away from our level while they are
     * in there is them taking the channel back for good — and it is the only
     * place the slider can move under a hand, since the options menu is the
     * one writer that runs with this submenu open. */
    if (menu_open) {
        if (ch->stamped >= 0 &&
            (int)cpu->read_word(ch->slider_addr) != ch->stamped) {
            ch->requested = -1;
            ch->set_request(-1);
        }
        return;
    }

    /* Re-stamp only on drift. The slider and its derived field are checked
     * separately because the game clobbers them separately: the title screen
     * and a save load rewrite both, SpuInitialize rewrites the derived fields
     * on their own. */
    if ((int)cpu->read_word(ch->slider_addr) == want &&
        cpu->read_word(ch->derived_addr) == ch->derived(want)) {
        ch->stamped = want;
        return;
    }
    ch->stamp(cpu, want);
    ch->stamped = want;
}

/* --- music slider -> OpenAL gain ----------------------------------------- */

static int   s_mirrored_music = -1;
static float s_mirrored_master = -1.0f;

/* Reproduce the two hardware stages the XA stream passes through rather than
 * treating the slider as a 0..1 fader. The game derives its CD input volume as
 * m_MusicVolume = slider << 11, and the SPU applies both that and the main
 * volume as (sample * volume) >> 15. CD input volume is written to the
 * register unshifted, so full scale for it is 32768 and slider 10 is 0.625,
 * not 1.0; scaling it to 1.0 overdrives the music and, because gains above
 * unity clamp at AL_MAX_GAIN, flattens the top of the slider's range. The
 * main volume factor carries its own (14-bit) scale and arrives already
 * normalised. */
static void music_gain_mirror(CPUState* cpu) {
    int level = (int)cpu->read_word(ADDR_MUSIC_SLIDER);
    if (level < 0)  level = 0;
    if (level > 10) level = 10;
    const float master = PsyX_SPUAL_GetMasterVolume();
    if (level == s_mirrored_music && master == s_mirrored_master) return;
    s_mirrored_music = level;
    s_mirrored_master = master;

    const float cd_volume = (float)(level * 2048) / 32768.0f;
    const float gain = cd_volume * master;
    PsyX_XA_SetVolume(gain);
    music_override_set_gain(gain);
}

/* --- tick hook ------------------------------------------------------------ */

void spyro1_audio_volume_on_tick(CPUState* cpu) {
    /* Runahead: a speculative tick's guest writes roll back, but the latches
     * here — and a release, which clears the host-side request — would not.
     * Let the real tick decide. */
    if (openpete_speculative) return;

    int menu_open = cpu->read_word(ADDR_OPTIONS_SUBMENU) == OPTIONS_SUBMENU_SOUND;
    channel_tick(cpu, &s_sfx,   menu_open);
    channel_tick(cpu, &s_music, menu_open);
    music_gain_mirror(cpu);
}
