/* portal_plane.c : the plane-cover near margin. See renderer/portal_plane.h. */
#include "renderer/portal_plane.h"
#include <stdlib.h>

/* The margin getter; documented in portal_plane.h. */
float openpete_portal_plane_margin(void) {
    static float m = -1.0f;
    if (m < 0.0f) {
        const char* e = getenv("OPENPETE_PORTAL_PLANE_MARGIN");
        m = (e && *e) ? (float)atof(e) : 128.0f;
        if (m < 0.0f) m = 0.0f;
    }
    return m;
}
