/* portal_plane.c : the plane-cover near margin. See renderer/portal_plane.h. */
#include "renderer/portal_plane.h"
#include <stdlib.h>

/* Plane-cover near margin M (world units): the preview covers everything at
 * or behind plane − M inside the opening footprint. Sized by corpus sweeps:
 * the plane ALONE covers every measured behind/at-plane intruder (doorway
 * moby, extended terrain band : identical cover counts at M 0..256); the
 * margin only widens cover into the near band, where an arch jamb slit keeps
 * opening toward the original's width up to ~256. The ceiling is portal-name
 * text mobys hovering ~192-256 in FRONT of the plane (they lose chunks at
 * 256) and inner-frame erosion from ~512. 128 sits below the text band with
 * the jamb mostly recovered. */
float openpete_portal_plane_margin(void) {
    static float m = -1.0f;
    if (m < 0.0f) {
        const char* e = getenv("OPENPETE_PORTAL_PLANE_MARGIN");
        m = (e && *e) ? (float)atof(e) : 128.0f;
        if (m < 0.0f) m = 0.0f;
    }
    return m;
}
