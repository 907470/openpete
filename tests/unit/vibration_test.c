/* vibration_test - controller vibration: the byte-to-motor conversion's
 * public properties and the --vibration strength scale, and the --vibration
 * preset's state machine over guest RAM.
 *
 * The per-byte motor intensity is rumble_response_test's: it compares every
 * byte of each motor with the measured table. This test checks the
 * properties the vibration path relies on whatever the table holds: a zero
 * byte is silent at every strength, the small motor's on bit drives it at
 * full scale, the large motor is monotonic, and the strength scale is a
 * kill switch at 0, the unscaled response at 10 and at -1 (unset), and
 * proportional between.
 *
 * The PRESET half is a state machine over guest RAM - when it stamps, when it
 * re-stamps over the game's own reseeds, when it hands the setting back - all
 * reachable with a fake CPUState over a small array, which is what this does.
 * The alternative is a controller replay through the options menu.
 */

#include "features/vibration.h"
#include "openpete_rumble.h"
#include "psx_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rc = 0;
static void fail(const char *msg) { printf("FAIL: %s\n", msg); rc = 1; }

/* ---- the host-side request the module reads (core/runtime/run.c) ---- */

static int s_vib_req = -1;
int  openpete_get_vibration(void)  { return s_vib_req; }
void openpete_set_vibration(int v) { s_vib_req = v; }

/* Runahead speculation flag; the module skips speculative ticks. */
int openpete_speculative = 0;

/* ---- fake guest RAM ---- */

#define ACT_ENABLED     0x800757A4u   /* g_ActEnabled */
#define OPTIONS_SUBMENU 0x800757C8u   /* D_800757C8, 1 = options submenu open */
#define OPTIONS_ROW     0x80075720u   /* D_80075720, menu cursor row */

#define ROW_VIBRATION   3u
#define ROW_MUSIC       1u

#define RAM_BASE 0x80075000u
#define RAM_SIZE 0x2000u
static uint32_t s_ram[RAM_SIZE / 4];
static int      s_writes;

static uint32_t fake_read(uint32_t addr)              { return s_ram[(addr - RAM_BASE) / 4]; }
static void     fake_write(uint32_t addr, uint32_t v) { s_ram[(addr - RAM_BASE) / 4] = v; s_writes++; }

static CPUState s_cpu;
static void tick(void) { spyro1_vibration_on_tick(&s_cpu); }

static void expect(const char *what, uint32_t addr, uint32_t want) {
    uint32_t got = fake_read(addr);
    if (got != want) {
        printf("FAIL: %s: 0x%08X = %u, want %u\n", what, addr, got, want);
        rc = 1;
    }
}

/* ---- the byte-to-motor conversion -------------------------------------- */

static void test_motor(void) {
    /* The small motor reads bit 0 only, and "on" is full scale. */
    if (openpete_rumble_response(1, 1) != 1.0f ||
        openpete_rumble_response(255, 1) != 1.0f ||
        openpete_rumble_response(2, 1) != 0.0f)
        fail("small motor is not bit 0 at full scale");
    if (openpete_rumble_motor_u16(1, 1, 10) != 65535)
        fail("small motor on did not reach the top of the u16 range");

    /* Monotonic across the whole byte range, so a harder-commanded rumble
     * never drives the large motor weaker. */
    {
        int b;
        float prev = -1.0f;
        for (b = 0; b <= 255; b++) {
            float v = openpete_rumble_response(b, 0);
            if (v < prev) { fail("large-motor response is not monotonic"); break; }
            prev = v;
        }
    }

    /* u16 conversion and the host strength scale. */
    if (openpete_rumble_motor_u16(255, 0, 10) < 65500)
        fail("strength 10 on a full byte should reach the top of the u16 range");
    if (openpete_rumble_motor_u16(120, 0, 0) != 0)
        fail("strength 0 must be a hard kill switch");
    if (openpete_rumble_motor_u16(120, 0, -1) != openpete_rumble_motor_u16(120, 0, 10))
        fail("unset strength (-1) must read as 10");
    {
        /* Half strength halves the amplitude - the guest toggle has no
         * intensity of its own, so this scale is the only thing that makes
         * --vibration a level rather than an on/off switch. */
        uint16_t full = openpete_rumble_motor_u16(200, 0, 10);
        uint16_t half = openpete_rumble_motor_u16(200, 0, 5);
        if (half == 0 || full == 0) fail("expected non-zero amplitudes");
        else if (abs((int)full / 2 - (int)half) > 2)
            fail("strength 5 is not half of strength 10");
    }
    /* A zero byte stays silent at every strength. */
    {
        int s;
        for (s = -1; s <= 10; s++)
            if (openpete_rumble_motor_u16(0, 0, s) != 0 ||
                openpete_rumble_motor_u16(0, 1, s) != 0) {
                fail("a commanded zero rumbled");
                break;
            }
    }
}

/* ---- the preset --------------------------------------------------------- */

/* The game's own writers, replayed so the module faces what it faces in a real
 * run. Both reseed the flag to 1 (initialization.c:223; a save-file load
 * restores whatever the card holds - modelled here as the same write). */
static void game_reseed_on(void)  { fake_write(ACT_ENABLED, 1); }
static void game_save_load(int v) { fake_write(ACT_ENABLED, (uint32_t)v); }

/* The player toggling the row by hand (gamestates/update.c case 3). */
static void player_toggles(void) {
    fake_write(ACT_ENABLED, fake_read(ACT_ENABLED) ? 0 : 1);
}

static void test_preset(void) {
    memset(s_ram, 0, sizeof s_ram);
    game_reseed_on();
    fake_write(OPTIONS_SUBMENU, 0);
    fake_write(OPTIONS_ROW, 0);

    /* Unset: the module writes nothing into gameplay RAM. This is what keeps
     * the default path guest-identical, so the oracle and determinism gates
     * see no new writes from a stock run. */
    s_vib_req = -1;
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("an unset preset wrote guest RAM");

    /* --vibration 0 silences the motors host-side and must write NOTHING. This
     * is the assertion that keeps the gates honest: 0 is the value every
     * unattended launcher sets, so a guest write here would mean the oracle
     * and determinism batteries were all measuring a quietly modified game -
     * and the failure would surface as an oracle divergence nobody would
     * connect to vibration. */
    s_vib_req = 0;
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("--vibration 0 wrote guest RAM");
    expect("--vibration 0 leaves the game's own setting alone", ACT_ENABLED, 1);
    game_save_load(0);
    s_writes = 0;
    tick();
    if (s_writes != 0) fail("--vibration 0 fought a save-file load");

    /* --vibration 7: any positive strength enables the flag, so a launch can
     * rumble even when the player's save had it off. The strength itself is
     * host-side and never reaches guest RAM. */
    s_vib_req = 7;
    tick();
    expect("a positive strength enables the flag", ACT_ENABLED, 1);

    /* ...and holds it over the game's own reseeds. */
    game_save_load(0);
    tick();
    expect("re-stamped over a save-file load", ACT_ENABLED, 1);
    game_reseed_on();
    tick();
    expect("still on after the title reseed", ACT_ENABLED, 1);

    /* Steady state costs nothing: no drift, no write. */
    s_writes = 0;
    tick(); tick();
    if (s_writes != 0) fail("re-stamped with no drift");

    /* The player takes it back: toggling the row by hand while the submenu is
     * open on the vibration row releases the preset for good. */
    fake_write(OPTIONS_SUBMENU, 1);
    fake_write(OPTIONS_ROW, ROW_VIBRATION);
    player_toggles();
    tick();
    if (s_vib_req != -1) fail("a hand toggle did not release the preset");
    /* Close the menu FIRST, then zero the counter: fake_write is the same
     * counter the module's writes land in, so a setup write after the reset
     * would be miscounted as the module stamping. */
    fake_write(OPTIONS_SUBMENU, 0);
    s_writes = 0;
    tick(); tick();
    if (s_writes != 0) fail("kept stamping after the player took the setting back");
    expect("the player's choice stands", ACT_ENABLED, 0);

    /* A NEW request re-arms: an O-menu or dev-overlay edit after a release
     * must take effect, or the setting would be dead for the session. */
    s_vib_req = 9;
    tick();
    expect("a new request re-arms after a release", ACT_ENABLED, 1);

    /* Being in the submenu on a DIFFERENT row is not a hand edit of this
     * setting. Releasing on any in-menu drift would hand vibration back for
     * an edit the player made to the music slider. */
    fake_write(OPTIONS_SUBMENU, 1);
    fake_write(OPTIONS_ROW, ROW_MUSIC);
    fake_write(ACT_ENABLED, 0);          /* some other writer, not the row */
    tick();
    if (s_vib_req != 9) fail("released on a drift from a different menu row");
    fake_write(OPTIONS_SUBMENU, 0);
    tick();
    expect("re-stamped after the unrelated drift", ACT_ENABLED, 1);

    /* Speculative ticks must not stamp or release: their guest writes roll
     * back, but a release would not. */
    fake_write(OPTIONS_SUBMENU, 1);
    fake_write(OPTIONS_ROW, ROW_VIBRATION);
    fake_write(ACT_ENABLED, 0);
    openpete_speculative = 1;
    s_writes = 0;
    tick();
    openpete_speculative = 0;
    if (s_writes != 0) fail("a speculative tick wrote guest RAM");
    if (s_vib_req != 9) fail("a speculative tick released the preset");
}

int main(void) {
    memset(&s_cpu, 0, sizeof s_cpu);
    s_cpu.read_word  = fake_read;
    s_cpu.write_word = fake_write;

    test_motor();
    test_preset();

    if (rc == 0) printf("vibration_test: ok\n");
    return rc;
}
