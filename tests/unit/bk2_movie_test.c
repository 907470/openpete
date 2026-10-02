/* tests/unit/bk2_movie_test.c - exercises bk2_movie_parse_log on an
 * inline Octoshock-style Input Log. Avoids a real .bk2 in CI by skipping
 * the zip layer (bk2_movie_load just unwraps a zip into the same blob). */

#include "bk2_movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                msg);                                              \
        return 1;                                                  \
    }                                                              \
} while (0)

/* Same convention as PsyX_Pad_UpdateKeyboardInput - released = bit set. */
#define BTN_SELECT  0x0001
#define BTN_L3      0x0002
#define BTN_R3      0x0004
#define BTN_START   0x0008
#define BTN_UP      0x0010
#define BTN_RIGHT   0x0020
#define BTN_DOWN    0x0040
#define BTN_LEFT    0x0080
#define BTN_L2      0x0100
#define BTN_R2      0x0200
#define BTN_L1      0x0400
#define BTN_R1      0x0800
#define BTN_TRI     0x1000
#define BTN_CIRCLE  0x2000
#define BTN_CROSS   0x4000
#define BTN_SQUARE  0x8000

static int test_basic_parse(void) {
    /* Hand-rolled minimal log: header, LogKey, 4 frames, [/Input].
     * Frame 0: no input.
     * Frame 1: Start + Cross - matches the first real press in tas.bk2.
     * Frame 2: D-pad Up.
     * Frame 3: every digital button down (lets us catch a bit-mask typo
     *          without writing 16 separate frames). */
    const char* log =
        "[Input]\r\n"
        "LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|\r\n"
        "|    1,...|  128,  128,  128,  128,.................|\r\n"
        "|    1,...|  128,  128,  128,  128,.....S...X.......|\r\n"
        "|    1,...|  128,  128,  128,  128,U................|\r\n"
        "|    1,...|  128,  128,  128,  128,UDLRsSqTOX112233.|\r\n"
        "[/Input]\r\n";

    bk2_movie_t m;
    int rc = bk2_movie_parse_log(log, strlen(log), &m);
    CHECK(rc == 0, "parse failed");
    CHECK(m.frame_count == 4, "expected 4 frames");
    CHECK(m.first_input_frame == 1, "first_input_frame should be 1");

    /* Frame 0 - clean, sticks centred. */
    CHECK(m.frames[0].buttons == 0xFFFF, "frame 0 should be 0xFFFF");
    CHECK(m.frames[0].axes[0] == 128 && m.frames[0].axes[1] == 128 &&
          m.frames[0].axes[2] == 128 && m.frames[0].axes[3] == 128,
          "frame 0 axes should be centred");

    /* Frame 1 - Start + Cross. Bits cleared = pressed. */
    uint16_t f1 = m.frames[1].buttons;
    CHECK((f1 & BTN_START) == 0, "frame 1: Start should be pressed");
    CHECK((f1 & BTN_CROSS) == 0, "frame 1: Cross should be pressed");
    CHECK((f1 & BTN_UP) != 0,    "frame 1: Up should NOT be pressed");
    CHECK((f1 & BTN_SELECT) != 0,"frame 1: Select should NOT be pressed");

    /* Frame 2 - Up only. Catches Up/Down/Left/Right ordering. */
    uint16_t f2 = m.frames[2].buttons;
    CHECK((f2 & BTN_UP) == 0,   "frame 2: Up should be pressed");
    CHECK((f2 & BTN_DOWN) != 0, "frame 2: Down should NOT be pressed");
    CHECK((f2 & BTN_LEFT) != 0, "frame 2: Left should NOT be pressed");
    CHECK((f2 & BTN_RIGHT) != 0,"frame 2: Right should NOT be pressed");

    /* Frame 3 - all 16 digital buttons (MODE at index 16 ignored). */
    uint16_t f3 = m.frames[3].buttons;
    CHECK(f3 == 0x0000, "frame 3: all 16 digital should be pressed");

    bk2_movie_free(&m);
    return 0;
}

static int test_no_input(void) {
    /* All-released movie - first_input_frame should equal frame_count. */
    const char* log =
        "[Input]\r\n"
        "LogKey:...\r\n"
        "|    1,...|  128,  128,  128,  128,.................|\r\n"
        "|    1,...|  128,  128,  128,  128,.................|\r\n"
        "[/Input]\r\n";
    bk2_movie_t m;
    int rc = bk2_movie_parse_log(log, strlen(log), &m);
    CHECK(rc == 0, "parse failed");
    CHECK(m.frame_count == 2, "expected 2 frames");
    CHECK(m.first_input_frame == 2, "first_input_frame should equal count");
    CHECK(m.frames[0].buttons == 0xFFFF && m.frames[1].buttons == 0xFFFF, "frames clean");
    bk2_movie_free(&m);
    return 0;
}

static int test_lf_only(void) {
    /* Some hand-edited movies use bare LF. Make sure the parser is happy
     * with no CR. */
    const char* log =
        "[Input]\n"
        "LogKey:...\n"
        "|    1,...|  128,  128,  128,  128,.....S...........|\n"
        "[/Input]\n";
    bk2_movie_t m;
    int rc = bk2_movie_parse_log(log, strlen(log), &m);
    CHECK(rc == 0, "parse failed");
    CHECK(m.frame_count == 1, "expected 1 frame");
    CHECK((m.frames[0].buttons & BTN_START) == 0, "Start should be pressed");
    bk2_movie_free(&m);
    return 0;
}

static int test_analog_parse(void) {
    /* Axes column order is LX, LY, RX, RY. Distinct values catch a swap. */
    const char* log =
        "[Input]\r\n"
        "LogKey:...\r\n"
        "|    1,...|  128,  128,  128,  128,.................|\r\n"
        "|    1,...|    0,   64,  192,  255,.................|\r\n"
        "[/Input]\r\n";
    bk2_movie_t m;
    int rc = bk2_movie_parse_log(log, strlen(log), &m);
    CHECK(rc == 0, "parse failed");
    CHECK(m.frame_count == 2, "expected 2 frames");
    CHECK(m.frames[1].axes[0] == 0,   "LStick X should be 0");
    CHECK(m.frames[1].axes[1] == 64,  "LStick Y should be 64");
    CHECK(m.frames[1].axes[2] == 192, "RStick X should be 192");
    CHECK(m.frames[1].axes[3] == 255, "RStick Y should be 255");
    CHECK(m.frames[1].buttons == 0xFFFF, "no buttons on the analog frame");
    /* Off-centre sticks count as input for first-input offset detection. */
    CHECK(m.first_input_frame == 1, "first_input_frame should be 1 (analog)");
    bk2_movie_free(&m);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_basic_parse();
    failures += test_no_input();
    failures += test_lf_only();
    failures += test_analog_parse();
    if (failures) {
        fprintf(stderr, "bk2_movie_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bk2_movie_test: ok\n");
    return 0;
}
