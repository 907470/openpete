/* tests/unit/portal_cover_slot_test.c - openpete_portal_cover_slots maps a
 * feed plane to its published cover-plane slot. A plane with fewer than three
 * outline points is not published, so every plane after it shifts down one
 * slot; an inner-rim face whose plane maps one slot off would be discarded
 * inside a different opening's outline. */

#include "openpete_portal_cover_slot.h"

#include <stdio.h>

/** @brief Compare a computed slot map and count against the expected ones.
 * @param name Case name for the failure line.
 * @param pn Outline point count per feed plane.
 * @param n Feed plane count.
 * @param cap Maximum published planes.
 * @param want Expected slot per feed plane.
 * @param want_n Expected published count.
 * @return 0 on a match, else 1. */
static int check(const char* name, const int* pn, int n, int cap, const int* want, int want_n)
{
    int slot[8];
    const int got_n = openpete_portal_cover_slots(pn, n, cap, slot);
    int bad = got_n != want_n;
    for (int i = 0; i < n; i++) bad |= slot[i] != want[i];
    if (bad) {
        fprintf(stderr, "FAIL %s: count %d (want %d), slots", name, got_n, want_n);
        for (int i = 0; i < n; i++) fprintf(stderr, " %d/%d", slot[i], want[i]);
        fprintf(stderr, "\n");
    }
    return bad;
}

/** @brief Run the slot-map cases.
 * @return 0 when every case passes. */
int main(void)
{
    int fail = 0;
    { const int pn[] = { 4, 5, 8 }, w[] = { 0, 1, 2 };
      fail |= check("all published", pn, 3, 6, w, 3); }
    { const int pn[] = { 4, 2, 6 }, w[] = { 0, -1, 1 };
      fail |= check("unpublished between two", pn, 3, 6, w, 2); }
    { const int pn[] = { 0, 3, 1, 2, 9 }, w[] = { -1, 0, -1, -1, 1 };
      fail |= check("leading and repeated unpublished", pn, 5, 6, w, 2); }
    { const int pn[] = { 3, 3, 3, 3, 3, 3, 3 }, w[] = { 0, 1, 2, 3, 4, 5, -1 };
      fail |= check("cap", pn, 7, 6, w, 6); }
    { const int pn[] = { 3, 2, 3 }, w[] = { 0, -1, -1 };
      fail |= check("cap after an unpublished plane", pn, 3, 1, w, 1); }
    if (!fail) printf("portal_cover_slot_test: 5 cases PASS\n");
    return fail;
}
