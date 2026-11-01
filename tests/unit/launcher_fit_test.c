/* tests/unit/launcher_fit_test.c - the launcher window's first size on a
 * scaled Windows display.
 *
 * The 1280x800 design size is scaled by the display's content scale
 * (openpete_launcher_scale_size) and fitted to the display's full and usable
 * bounds (openpete_launcher_fit), both in pixels, as on Windows. Scales 1.0,
 * 1.25, 1.5 and 2.0 run against 1920x1080, 2560x1440 and 3840x2160 displays
 * whose usable bounds are 48 px shorter (a taskbar), plus two displays where
 * the scaled size fits only the full bounds, so the maximized arm is reached.
 * Each row's opened size and decision are asserted and printed. */

#include "openpete_launcher_fit.h"

#include <stdio.h>

/** @brief One display and scale with the expected opened size and decision. */
typedef struct {
    int full_w, full_h; /**< Display bounds. */
    float scale;        /**< Display content scale. */
    int w, h;           /**< Expected opened size. */
    openpete_launcher_fit_how how; /**< Expected decision. */
} Row;

/** @brief Run every row and return the number that fail.
 * @return 0 when every row opens at its expected size and decision. */
int main(void)
{
    static const char* const kHow[] = {"as requested", "maximized", "clamped"};
    static const Row rows[] = {
        {1920, 1080, 1.0f, 1280, 800, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {1920, 1080, 1.25f, 1600, 1000, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {1920, 1080, 1.5f, 1486, 929, OPENPETE_LAUNCHER_FIT_CLAMPED},
        {1920, 1080, 2.0f, 1486, 929, OPENPETE_LAUNCHER_FIT_CLAMPED},
        {2560, 1440, 1.0f, 1280, 800, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {2560, 1440, 1.25f, 1600, 1000, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {2560, 1440, 1.5f, 1920, 1200, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {2560, 1440, 2.0f, 2004, 1253, OPENPETE_LAUNCHER_FIT_CLAMPED},
        {3840, 2160, 1.0f, 1280, 800, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {3840, 2160, 1.25f, 1600, 1000, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {3840, 2160, 1.5f, 1920, 1200, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {3840, 2160, 2.0f, 2560, 1600, OPENPETE_LAUNCHER_FIT_AS_REQUESTED},
        {1920, 1200, 1.5f, 1920, 1200, OPENPETE_LAUNCHER_FIT_MAXIMIZED},
        {2560, 1600, 2.0f, 2560, 1600, OPENPETE_LAUNCHER_FIT_MAXIMIZED},
    };
    int bad = 0, seen[3] = {0, 0, 0};
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const Row* r = &rows[i];
        int rw = 1280, rh = 800, w = 0, h = 0;
        openpete_launcher_scale_size(&rw, &rh, r->scale);
        const openpete_launcher_fit_how how =
            openpete_launcher_fit(rw, rh, r->full_w, r->full_h, r->full_w, r->full_h - 48, &w, &h);
        const int ok = how == r->how && w == r->w && h == r->h;
        seen[how]++;
        printf("%s display %dx%d usable %dx%d scale %.2f: request %dx%d opens %s %dx%d", ok ? "ok  " : "FAIL",
               r->full_w, r->full_h, r->full_w, r->full_h - 48, (double)r->scale, rw, rh, kHow[how], w, h);
        if (!ok) printf(" (expected %s %dx%d)", kHow[r->how], r->w, r->h);
        printf("\n");
        bad += !ok;
    }
    /* A scale SDL could not read (0) leaves the design size. */
    int zw = 1280, zh = 800;
    openpete_launcher_scale_size(&zw, &zh, 0.0f);
    if (zw != 1280 || zh != 800) { printf("FAIL scale 0: %dx%d\n", zw, zh); bad++; }
    if (!seen[0] || !seen[1] || !seen[2]) { printf("FAIL: a decision arm was not reached\n"); bad++; }
    printf("launcher_fit_test: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
