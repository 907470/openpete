/* audio_volume_test — the --sfx / --music slider presets.
 *
 * The interesting half of features/audio_volume_native.c is a state machine
 * over guest RAM: when it stamps, when it re-stamps over the game's own
 * reseeds, and when it hands the channel back to the player. All of that is
 * reachable with a fake CPUState over a small RAM array, which is what this
 * does — the alternative is a controller replay through the options menu.
 *
 * The module is compiled straight in with its host-side channels stubbed. */

#include "features/audio_volume_native.h"
#include "psx_runtime.h"

#include <stdio.h>
#include <string.h>

/* ---- the host-side request the module reads (core/runtime/run.c) ---- */

static int s_sfx_req   = -1;
static int s_music_req = -1;

int  openpete_get_sfx_volume(void)     { return s_sfx_req; }
void openpete_set_sfx_volume(int v)    { s_sfx_req   = v; }
int  openpete_get_music_volume(void)   { return s_music_req; }
void openpete_set_music_volume(int v)  { s_music_req = v; }

/* ---- the audio channels it drives ---- */

static float s_xa_gain    = -1.0f;
static float s_wav_gain   = -1.0f;

void PsyX_XA_SetVolume(float g)      { s_xa_gain  = g; }
void music_override_set_gain(float g) { s_wav_gain = g; }

/* Runahead speculation flag; the module skips speculative ticks. */
int openpete_speculative = 0;

/* ---- fake guest RAM ---- */

#define SFX_SLIDER      0x80075754u
#define MUSIC_SLIDER    0x80075748u
#define OPTIONS_SUBMENU 0x800757C8u
#define SPU_CD_VOLUME   0x80075F18u
#define SPU_MUSIC_VOL   0x80076224u
#define SPU_SFX_RAW     0x80076228u
#define SPU_SOUND_VOL   0x8007622Cu

/* One page around the globals under test — everything the module touches sits
 * inside 0x80075000..0x80077000. */
#define RAM_BASE 0x80075000u
#define RAM_SIZE 0x2000u
static uint32_t s_ram[RAM_SIZE / 4];
static int      s_writes;   /* guest words written since the last check */

static uint32_t fake_read(uint32_t addr)              { return s_ram[(addr - RAM_BASE) / 4]; }
static void     fake_write(uint32_t addr, uint32_t v) { s_ram[(addr - RAM_BASE) / 4] = v; s_writes++; }

static CPUState s_cpu;

static int  rc = 0;
static int fail(const char *msg) { printf("FAIL: %s\n", msg); rc = 1; return 1; }

static void expect(const char *what, uint32_t addr, uint32_t want) {
    uint32_t got = fake_read(addr);
    if (got != want) {
        printf("FAIL: %s: 0x%08X = 0x%X, want 0x%X\n", what, addr, got, want);
        rc = 1;
    }
}

static void tick(void) { spyro1_audio_volume_on_tick(&s_cpu); }

/* The game's own writers, replayed so the module faces what it faces in a
 * real run. */
static void game_title_reseed(void) {          /* gamestates/init.c:549,555 */
    fake_write(SFX_SLIDER, 10);
    fake_write(SPU_SFX_RAW, 0x3FFF);
    fake_write(SPU_SOUND_VOL, 0x1000);
    fake_write(MUSIC_SLIDER, 10);
    fake_write(SPU_MUSIC_VOL, 0x5000);
    fake_write(SPU_CD_VOLUME, 0x50005000u);
}
static void game_spu_initialize(void) {        /* spu.c:25-31 — derived only */
    fake_write(SPU_SOUND_VOL, 0x1000);
    fake_write(SPU_MUSIC_VOL, 0x5000);
    fake_write(SPU_CD_VOLUME, 0x50005000u);
}

int main(void) {
    memset(&s_cpu, 0, sizeof s_cpu);
    s_cpu.read_word  = fake_read;
    s_cpu.write_word = fake_write;
    game_title_reseed();

    /* Unset: the module writes nothing into gameplay RAM. */
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("an unset preset wrote guest RAM");

    /* ...but the music gain still follows the game's own slider, which is
     * what makes the in-game MUSIC row audible at all. */
    if (s_xa_gain != 1.0f || s_wav_gain != 1.0f)
        fail("music gain did not mirror a slider of 10");

    /* A preset stamps the slider and every derived field. */
    openpete_set_sfx_volume(7);
    openpete_set_music_volume(3);
    tick();
    expect("sfx slider",   SFX_SLIDER,    7);
    expect("sfx raw",      SPU_SFX_RAW,   (7 * 0x3FFF) / 10);
    expect("m_SoundVolume", SPU_SOUND_VOL, (((7 * 0x3FFF) / 10) * 0x1000) / 0x3FFF);
    expect("music slider", MUSIC_SLIDER,  3);
    expect("m_MusicVolume", SPU_MUSIC_VOL, 3u << 11);
    expect("cd volume",    SPU_CD_VOLUME, (3u << 11) | (3u << 27));
    if (s_xa_gain != 0.3f) fail("music gain did not follow the preset");

    /* Steady state costs no writes. */
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("re-stamped an already-correct slider");

    /* The title screen reseeds both sliders to 10; the preset takes them
     * back on the next tick. */
    game_title_reseed();
    tick();
    expect("sfx slider after title reseed",   SFX_SLIDER,   7);
    expect("music slider after title reseed", MUSIC_SLIDER, 3);

    /* SpuInitialize rewrites the derived fields alone — the sliders still
     * read our values, so only a derived-side check catches this. */
    game_spu_initialize();
    tick();
    expect("m_SoundVolume after SpuInitialize", SPU_SOUND_VOL,
           (((7 * 0x3FFF) / 10) * 0x1000) / 0x3FFF);
    expect("m_MusicVolume after SpuInitialize", SPU_MUSIC_VOL, 3u << 11);

    /* Options menu open: the player owns the slider, so no stamping — even
     * as the game's menu code moves it. */
    fake_write(OPTIONS_SUBMENU, 1);
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("stamped while the sound options were open");

    /* Leaving it without touching a row keeps the preset — including when the
     * game reseeds the sliders on the way out (quit to title). */
    fake_write(OPTIONS_SUBMENU, 0);
    game_title_reseed();
    tick();
    expect("sfx slider after an untouched menu visit", SFX_SLIDER, 7);

    /* Moving the MUSIC row by hand hands that channel back for good — and
     * leaves the SFX preset alone. */
    fake_write(OPTIONS_SUBMENU, 1);
    tick();
    fake_write(MUSIC_SLIDER, 9);         /* gamestates/update.c:299-307 */
    fake_write(SPU_MUSIC_VOL, 9u << 11);
    tick();
    fake_write(OPTIONS_SUBMENU, 0);
    tick();
    if (openpete_get_music_volume() != -1)
        fail("a hand-moved music slider did not release the preset");
    expect("music slider stays where the player left it", MUSIC_SLIDER, 9);
    if (s_xa_gain != 0.9f) fail("music gain did not follow the player's edit");
    expect("sfx preset survives a music edit", SFX_SLIDER, 7);

    /* A released channel stays released across the game's own reseeds. */
    game_title_reseed();
    tick();
    expect("released music slider follows the game", MUSIC_SLIDER, 10);
    expect("sfx preset still held", SFX_SLIDER, 7);

    /* A fresh request (a dev-overlay edit) re-arms the channel. */
    openpete_set_music_volume(0);
    tick();
    expect("re-armed music slider", MUSIC_SLIDER, 0);
    if (s_xa_gain != 0.0f) fail("music gain did not follow the re-arm");

    printf(rc ? "audio_volume_test: FAILED\n" : "audio_volume_test: ok\n");
    return rc;
}
