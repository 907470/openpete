// core/include/openpete_park_site.h - where a VSync wait parked the game fiber.
#pragma once

#include <stdint.h>

/* Where a psyq_VSync wait parked the game fiber: the guest return address
 * of the VSync call and the guest stack pointer at it. The fiber's host
 * call stack below the yield is the recompiled continuation of that call;
 * a snapshot taken at one site and loaded at another resumes the other
 * site's continuation with the snapshot's registers. A site with ra 0 is
 * none (a state saved before sites were recorded). */
typedef struct {
    uint32_t ra;   /* guest $ra at the VSync call */
    uint32_t sp;   /* guest $sp at the VSync call */
} openpete_park_site_t;
