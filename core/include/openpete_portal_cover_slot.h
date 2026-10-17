/* openpete_portal_cover_slot.h - the published slot of each portal opening
 * plane.
 *
 * The terrain feed derives up to six opening planes per present and
 * publishes to the renderer (openpete_native_set_cover_planes) only those
 * whose projected outline has at least three points, in feed order. An
 * inner-rim face names its accepting plane by its published slot, so the
 * fragment stage tests that plane's outline and no other; this map converts
 * the feed's plane index into that slot. */
#ifndef OPENPETE_PORTAL_COVER_SLOT_H
#define OPENPETE_PORTAL_COVER_SLOT_H

/** @brief Map each feed plane to its published cover-plane slot.
 *
 * Plane i is published when its outline point count pn[i] is at least 3 and
 * fewer than @p cap planes precede it in the published list; slot[i] is then
 * its index in that list, else -1.
 * @param pn Outline point count per feed plane.
 * @param n Feed plane count.
 * @param cap Maximum published planes.
 * @param slot Receives the published slot per feed plane, n entries.
 * @return The published plane count. */
static inline int openpete_portal_cover_slots(const int* pn, int n, int cap, int* slot)
{
    int cn = 0;
    for (int i = 0; i < n; i++)
        slot[i] = (pn[i] >= 3 && cn < cap) ? cn++ : -1;
    return cn;
}

#endif
