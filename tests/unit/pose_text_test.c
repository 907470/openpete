/* pose_text_test - the free-camera pose grammar (core/config/pose_text.c).
 *
 * Accepted and refused texts, each refusal naming the offending text; then,
 * with the source root as the argument, every pose in the committed corpus
 * manifests (every .manifest and .tsv file in games/<game>/corpus/): each `|`- or
 * tab-separated field that starts with "pos=(", the pose after the level of
 * a "pose:<level>:" field, and each `--freecam-pose` value. A malformed pose there fails the test and is printed with its file
 * and line; the count checked is printed, and zero poses checked fails.
 * POSIX only (glob). */

#include "openpete_pose_text.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

/** @brief Check one text against the expected verdict and print a failure.
 * @param text The pose text.
 * @param want_ok 1 when the grammar must accept it. */
static void expect(const char* text, int want_ok) {
    char why[256] = "";
    const int ok = openpete_pose_text_check(text, why, sizeof why) == 0;
    if (ok != want_ok) {
        printf("FAIL: \"%s\" %s\n", text, want_ok ? "refused" : "accepted");
        if (why[0]) printf("      %s\n", why);
        s_fail = 1;
    } else if (!ok && !strstr(why, text)) {
        printf("FAIL: the refusal of \"%s\" does not name it: %s\n", text, why);
        s_fail = 1;
    }
}

/** @brief The grammar's accepted and refused forms. */
static void grammar_cases(void) {
    static const char* const ok[] = {
        "pos=(84992, 45582, 9948) yaw=1024 pitch=160",
        "pos=(84992,45582,9948)yaw=1024",
        "pos=(1, 2, 3)",
        "1,2,3,4,5,6",
        "pos=(1, 2, 3) yaw=-1135 pitch=900 roll=12",
        "pos=(1, 2, 3) roll=-4 yaw=5",
        "84992,45582,9948,1024,160",
        "1 2 3",
        "(1, 2, 3, 4, 5) roll=2",
        "  pos=(1, 2, 3) yaw=4 pitch=5   ",
        "topdown",
        "sweep3",
    };
    static const char* const bad[] = {
        "pos=(bogus",
        "pos=(1, 2",
        "1,2,3,4,5,6,7",
        "pos=(1, 2, 3",
        "pos=(1, 2, 3, 4)",
        "pos=(1, 2, 3) pitch=5",
        "pos=(1, 2, 3) yaw=4 yaw=5",
        "pos=(1, 2, 3) 4",
        "pos=1,2,3",
        "1.5, 2, 3",
        "1, 2",
        "1, 2, 3 roll=1 roll=2",
        "roll=1 1, 2, 3",
        "1, 2, 3)",
        "1, 2, 3 yaw=x",
        "bogus",
        "sweep",
        "sweepx",
        "",
        "   ",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) expect(ok[i], 1);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) expect(bad[i], 0);
}

/** @brief Check one pose taken from a manifest line, printing a refusal with its place.
 * @param pose The pose text, NUL-terminated.
 * @param file The manifest.
 * @param line The line number. */
static void check_manifest_pose(const char* pose, const char* file, int line) {
    char why[512];
    if (openpete_pose_text_check(pose, why, sizeof why) == 0) return;
    printf("FAIL: %s:%d: %s\n", file, line, why);
    s_fail = 1;
}

/** @brief Check the pose fields of one manifest line: a `|`- or tab-separated field starting with "pos=(", or the pose after "pose:<level>:".
 * @param text The line, without its newline.
 * @param file The manifest.
 * @param line The line number.
 * @return The count of poses checked. */
static int scan_fields(const char* text, const char* file, int line) {
    int n = 0;
    char buf[2048];
    for (const char* f = text; f && *f;) {
        const char* end = f + strcspn(f, "|\t");
        while (f < end && *f == ' ') f++;
        if (!strncmp(f, "pose:", 5)) {
            const char* colon = memchr(f + 5, ':', (size_t)(end - f - 5));
            if (colon) f = colon + 1;
        }
        if (!strncmp(f, "pos=(", 5) && (size_t)(end - f) < sizeof buf) {
            memcpy(buf, f, (size_t)(end - f));
            buf[end - f] = '\0';
            check_manifest_pose(buf, file, line);
            n++;
        }
        f = *end ? end + 1 : NULL;
    }
    return n;
}

/** @brief Check the pose values one spelling of the knob takes on a manifest line, quoted or one token, skipping a shell variable a harness expands.
 * @param text The line, without its newline.
 * @param key The spelling: the flag, or the environment variable's name.
 * @param file The manifest.
 * @param line The line number.
 * @return The count of poses checked. */
static int scan_values(const char* text, const char* key, const char* file, int line) {
    int n = 0;
    char buf[2048];
    for (const char* k = strstr(text, key); k; k = strstr(k + 1, key)) {
        const char* v = k + strlen(key);
        v += (*v == '=') ? 1 : strspn(v, " ");
        const char close = (*v == '"' || *v == '\'') ? *v++ : 0;
        const char stop[2] = { close, '\0' };
        size_t len = close ? strcspn(v, stop) : strcspn(v, " \t|");
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, v, len);
        buf[len] = '\0';
        if (buf[0] == '$') continue;
        check_manifest_pose(buf, file, line);
        n++;
    }
    return n;
}

/** @brief Check the poses of every committed corpus manifest under root.
 * @param root The source root.
 * @return The count of poses checked. */
static int scan_manifests(const char* root) {
    static const char* const pats[] = { "games/*/corpus/*.manifest", "games/*/corpus/*.tsv" };
    int total = 0, files = 0;
    for (size_t p = 0; p < sizeof pats / sizeof pats[0]; p++) {
        char pat[1024];
        snprintf(pat, sizeof pat, "%s/%s", root, pats[p]);
        glob_t g;
        if (glob(pat, 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE* fp = fopen(g.gl_pathv[i], "r");
            if (!fp) continue;
            char line[8192];
            int ln = 0, here = 0;
            while (fgets(line, sizeof line, fp)) {
                ln++;
                line[strcspn(line, "\r\n")] = '\0';
                const char* t = line + strspn(line, " \t");
                if (*t == '#') continue;
                const char* rel = g.gl_pathv[i] + strlen(root) + 1;
                here += scan_fields(t, rel, ln) + scan_values(t, "--freecam-pose", rel, ln)
                      + scan_values(t, "OPENPETE_FREECAM_POSE", rel, ln);
            }
            fclose(fp);
            if (here) files++;
            total += here;
        }
        globfree(&g);
    }
    printf("corpus poses checked: %d in %d manifests\n", total, files);
    return total;
}

int main(int argc, char** argv) {
    grammar_cases();
    if (argc > 1 && scan_manifests(argv[1]) == 0) {
        printf("FAIL: no pose found under %s/games/*/corpus\n", argv[1]);
        s_fail = 1;
    }
    printf("pose_text_test: %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail;
}
