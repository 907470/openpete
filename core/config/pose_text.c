/* pose_text.c - the free-camera pose grammar (openpete_pose_text.h). */

#include "openpete_pose_text.h"

#include <stdio.h>
#include <string.h>

/* One pass over pose text: the cursor, the positional numbers read, and the
 * bracket and key state the grammar orders. */
typedef struct {
    const char* text;   /* the whole text, for offsets */
    const char* p;      /* the cursor */
    int nums;           /* positional numbers read: x, y, z, yaw, pitch, roll */
    int keyed;          /* the text opened with "pos=" */
    int paren;          /* 1 inside the bracket, 2 once it closed */
    int roll;           /* roll= read */
    char* why;
    size_t cap;
} pose_scan;

/** @brief Write the refusal naming the text at the cursor and its offset.
 * @param sc The scan.
 * @param what The rule the text breaks.
 * @return -1. */
static int pose_refuse(pose_scan* sc, const char* what) {
    if (sc->why && sc->cap) {
        if (*sc->p)
            snprintf(sc->why, sc->cap, "%s at \"%.24s\" (offset %d) in \"%s\"", what, sc->p,
                     (int)(sc->p - sc->text), sc->text);
        else
            snprintf(sc->why, sc->cap, "%s at the end of \"%s\"", what, sc->text);
    }
    return -1;
}

/** @brief Return 1 when c separates the parts of a pose: a space, a tab or a comma. */
static int pose_is_sep(char c) {
    return c == ' ' || c == '\t' || c == ',';
}

/** @brief Return 1 when c is a decimal digit. */
static int pose_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/** @brief Read one signed decimal integer at the cursor.
 *
 * A sign must stand directly before a digit, and a '.' or a letter directly
 * after the digits refuses the number, since the runtime reader would split
 * "1.5" into two numbers.
 * @param sc The scan.
 * @return 0 with the cursor past the number, -1 with the refusal written. */
static int pose_take_int(pose_scan* sc) {
    const char* q = sc->p;
    if (*q == '-' || *q == '+') q++;
    if (!pose_is_digit(*q)) return pose_refuse(sc, "expected an integer");
    while (pose_is_digit(*q)) q++;
    if (*q && !pose_is_sep(*q) && *q != ')') {
        sc->p = q;
        return pose_refuse(sc, "expected a separator after the integer");
    }
    sc->p = q;
    return 0;
}

/** @brief Return 1 and step past key when the cursor starts with it. */
static int pose_take_key(pose_scan* sc, const char* key) {
    const size_t n = strlen(key);
    if (strncmp(sc->p, key, n) != 0) return 0;
    sc->p += n;
    return 1;
}

/** @brief Read a keyed number: yaw= as the fourth, pitch= as the fifth, roll= once after the position.
 * @param sc The scan, the cursor past the key.
 * @param slot 3 for yaw, 4 for pitch, -1 for roll.
 * @return 0, or -1 with the refusal written. */
static int pose_keyed(pose_scan* sc, int slot) {
    if (sc->paren == 1) return pose_refuse(sc, "a key inside the bracket");
    if (slot < 0) {
        if (sc->roll || sc->nums < 3) return pose_refuse(sc, "roll= before the position or twice");
        sc->roll = 1;
        return pose_take_int(sc);
    }
    if (sc->nums != slot)
        return pose_refuse(sc, slot == 3 ? "yaw= must follow the three position numbers"
                                         : "pitch= must follow yaw");
    sc->nums++;
    return pose_take_int(sc);
}

/** @brief Read one part at the cursor: a bracket, a key with its number, or a positional number.
 * @param sc The scan, the cursor on a non-separator character.
 * @return 0, or -1 with the refusal written. */
static int pose_part(pose_scan* sc) {
    if (*sc->p == ')') {
        if (sc->paren != 1) return pose_refuse(sc, "')' without '('");
        if (sc->nums < 3 || (sc->keyed && sc->nums != 3))
            return pose_refuse(sc, "the bracket closes before x, y and z");
        sc->paren = 2;
        sc->p++;
        return 0;
    }
    if (pose_take_key(sc, "yaw=")) return pose_keyed(sc, 3);
    if (pose_take_key(sc, "pitch=")) return pose_keyed(sc, 4);
    if (pose_take_key(sc, "roll=")) return pose_keyed(sc, -1);
    const char* d = (*sc->p == '-' || *sc->p == '+') ? sc->p + 1 : sc->p;
    if (!pose_is_digit(*d)) return pose_refuse(sc, "unexpected text");
    if (sc->paren == 2 || sc->roll) return pose_refuse(sc, "a number without a key after the position");
    if (sc->nums >= 6) return pose_refuse(sc, "more than six numbers");
    sc->nums++;
    return pose_take_int(sc);
}

/** @brief Return 1 when the text, without surrounding spaces, is a named pose of the shared terrain resolver: topdown, or sweep followed by digits.
 * @param s The text from its first non-space character.
 * @param n Its length without trailing spaces. */
static int pose_is_named(const char* s, size_t n) {
    if (n == 7 && strncmp(s, "topdown", 7) == 0) return 1;
    if (n <= 5 || strncmp(s, "sweep", 5) != 0) return 0;
    for (size_t i = 5; i < n; i++)
        if (!pose_is_digit(s[i])) return 0;
    return 1;
}

int openpete_pose_text_check(const char* text, char* why, size_t cap) {
    pose_scan sc = { text ? text : "", text ? text : "", 0, 0, 0, 0, why, cap };
    while (*sc.p == ' ' || *sc.p == '\t') sc.p++;
    size_t n = strlen(sc.p);
    while (n && (sc.p[n - 1] == ' ' || sc.p[n - 1] == '\t')) n--;
    if (!n) return pose_refuse(&sc, "empty pose");
    if (pose_is_named(sc.p, n)) return 0;
    if (pose_take_key(&sc, "pos=")) {
        sc.keyed = 1;
        if (*sc.p != '(') return pose_refuse(&sc, "expected '(' after pos=");
    }
    if (*sc.p == '(') {
        sc.paren = 1;
        sc.p++;
    }
    for (;;) {
        while (pose_is_sep(*sc.p)) sc.p++;
        if (!*sc.p) break;
        if (pose_part(&sc) != 0) return -1;
    }
    if (sc.paren == 1) return pose_refuse(&sc, "'(' is not closed");
    if (sc.nums < 3) return pose_refuse(&sc, "fewer than three position numbers");
    return 0;
}
