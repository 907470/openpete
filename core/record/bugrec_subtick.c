/* core/record/bugrec_subtick.c - the bug-report recorder's sub-tick present
 * ring; see openpete_bugrec_subtick.h. */
#include "openpete_bugrec_subtick.h"

#include "openpete_fiber.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"

#include <SDL3/SDL_atomic.h>

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(openpete_bugrec_sub_present_t) == 96, "subtick.bin present layout");
_Static_assert(sizeof(openpete_bugrec_sub_obj_t) == 80, "subtick.bin entry layout");
_Static_assert(sizeof(openpete_bugrec_sub_portal_t) == 32, "subtick.bin portal entry layout");

#define SUB_HEADER_BYTES 48u
#define SUB_PORTAL_HEADER_BYTES 24u   /* "OPSUBPT1", u32 size, u32 count, u64 dropped */

/* The rings, allocated at the first present. s_pres[s_pres_n % N] is the
 * next present written; s_obj and s_portal likewise. s_open is 1 while the newest
 * present takes entries. */
static openpete_bugrec_sub_present_t* s_pres;
static openpete_bugrec_sub_obj_t*     s_obj;
static openpete_bugrec_sub_portal_t*  s_portal;
static uint64_t s_pres_n;      /* presents begun */
static uint64_t s_obj_n;       /* entries appended */
static uint64_t s_portal_n;    /* portal entries appended */
static uint64_t s_outside;     /* entries offered outside an open present */
static int      s_open;
static int      s_alloc_failed;  /* allocation failed or the ring is off: never retried */

/** @brief Allocate the three rings on first use, unless OPENPETE_BUGREPORT_SUBTICK=0.
 * @return 1 when they exist, 0 when the ring is off or allocation failed. */
static int sub_alloc(void)
{
    if (s_pres) return 1;
    if (s_alloc_failed) return 0;
    const char* e = getenv("OPENPETE_BUGREPORT_SUBTICK");
    if (e && *e == '0') { s_alloc_failed = 1; return 0; }
    s_pres = (openpete_bugrec_sub_present_t*)calloc(OPENPETE_BUGREC_SUB_PRESENTS, sizeof *s_pres);
    s_obj = (openpete_bugrec_sub_obj_t*)calloc(OPENPETE_BUGREC_SUB_OBJS, sizeof *s_obj);
    s_portal = (openpete_bugrec_sub_portal_t*)calloc(OPENPETE_BUGREC_SUB_PORTALS, sizeof *s_portal);
    if (s_pres && s_obj && s_portal) return 1;
    free(s_pres);
    free(s_obj);
    free(s_portal);
    s_pres = NULL;
    s_obj = NULL;
    s_portal = NULL;
    s_alloc_failed = 1;
    return 0;
}

/** @brief Return the open present's record, or NULL when none is open. */
static openpete_bugrec_sub_present_t* sub_cur(void)
{
    return s_open ? &s_pres[(s_pres_n - 1u) % OPENPETE_BUGREC_SUB_PRESENTS] : NULL;
}

void openpete_bugrec_subtick_begin(int kind, int path)
{
    if (!sub_alloc()) return;
    openpete_bugrec_sub_present_t* p = &s_pres[s_pres_n % OPENPETE_BUGREC_SUB_PRESENTS];
    memset(p, 0, sizeof *p);
    s_pres_n++;
    p->seq = s_pres_n;
    p->present_ns = (int64_t)openpete_interp_present_now();
    p->period_ns = (int64_t)g_openpete_tick_period_ns;
    double a = 0.0;
    const openpete_interp_snapshot_t *prev = NULL, *curr = NULL;
    if (openpete_interp_query(p->present_ns, g_openpete_tick_period_ns, &a, &prev, &curr) && prev && curr) {
        p->tick_prev_ns = (int64_t)prev->tick_ns;
        p->tick_curr_ns = (int64_t)curr->tick_ns;
        p->tick_prev = prev->tick;
        p->tick_curr = curr->tick;
    }
    p->feed_key = openpete_feed_key_now();
    p->vbl = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    p->kind = (uint8_t)kind;
    p->path = (uint8_t)path;
    p->slot = -1;
    p->obj_first = s_obj_n;
    s_open = 1;
}

int openpete_bugrec_subtick_open(void) { return s_open; }

int64_t openpete_bugrec_subtick_present_ns(void)
{
    const openpete_bugrec_sub_present_t* p = sub_cur();
    return p ? p->present_ns : 0;
}

void openpete_bugrec_subtick_obj(const openpete_bugrec_sub_obj_t* e)
{
    openpete_bugrec_sub_present_t* p = sub_cur();
    if (!p) { s_outside++; return; }
    s_obj[s_obj_n % OPENPETE_BUGREC_SUB_OBJS] = *e;
    s_obj_n++;
    p->obj_n++;
}

void openpete_bugrec_subtick_portal(const openpete_bugrec_sub_portal_t* e)
{
    const openpete_bugrec_sub_present_t* p = sub_cur();
    if (!p) return;
    openpete_bugrec_sub_portal_t* d = &s_portal[s_portal_n % OPENPETE_BUGREC_SUB_PORTALS];
    *d = *e;
    d->seq = p->seq;
    s_portal_n++;
}

void openpete_bugrec_subtick_late(int late)
{
    openpete_bugrec_sub_present_t* p = sub_cur();
    if (p && late) p->late = 1;
}

void openpete_bugrec_subtick_end(double alpha, int paired, int slot, uint32_t scene_id,
                                 uint32_t stamp, int64_t target_ns)
{
    openpete_bugrec_sub_present_t* p = sub_cur();
    if (!p) return;
    p->alpha = (float)alpha;
    p->paired = (uint8_t)(paired ? 1 : 0);
    p->slot = slot;
    p->scene_id = scene_id;
    p->stamp = stamp;
    p->target_ns = target_ns;
    s_open = 0;
}

/** @brief Append a little-endian u32 at a cursor and advance it. */
static void put_u32(uint8_t** w, uint32_t v)
{
    memcpy(*w, &v, 4);
    *w += 4;
}

/** @brief Append a little-endian u64 at a cursor and advance it. */
static void put_u64(uint8_t** w, uint64_t v)
{
    memcpy(*w, &v, 8);
    *w += 8;
}

size_t openpete_bugrec_subtick_image(uint8_t** out, size_t* len)
{
    *out = NULL;
    *len = 0;
    if (!s_pres) return 0;
    const uint64_t np = s_pres_n < OPENPETE_BUGREC_SUB_PRESENTS ? s_pres_n : OPENPETE_BUGREC_SUB_PRESENTS;
    const uint64_t no = s_obj_n < OPENPETE_BUGREC_SUB_OBJS ? s_obj_n : OPENPETE_BUGREC_SUB_OBJS;
    const uint64_t nt = s_portal_n < OPENPETE_BUGREC_SUB_PORTALS ? s_portal_n : OPENPETE_BUGREC_SUB_PORTALS;
    if (!np) return 0;
    const size_t bytes = SUB_HEADER_BYTES + (size_t)np * sizeof *s_pres + (size_t)no * sizeof *s_obj
                       + SUB_PORTAL_HEADER_BYTES + (size_t)nt * sizeof *s_portal;
    uint8_t* b = (uint8_t*)malloc(bytes);
    if (!b) return 0;
    uint8_t* w = b;
    memcpy(w, "OPSUBTK1", 8);
    w += 8;
    put_u32(&w, OPENPETE_BUGREC_SUB_VERSION);
    put_u32(&w, (uint32_t)sizeof *s_pres);
    put_u32(&w, (uint32_t)sizeof *s_obj);
    put_u32(&w, (uint32_t)np);
    put_u32(&w, (uint32_t)no);
    put_u32(&w, (uint32_t)(s_pres_n - np));
    put_u64(&w, s_obj_n - no);
    put_u64(&w, s_outside);
    for (uint64_t k = 0; k < np; k++, w += sizeof *s_pres)
        memcpy(w, &s_pres[(s_pres_n - np + k) % OPENPETE_BUGREC_SUB_PRESENTS], sizeof *s_pres);
    for (uint64_t k = 0; k < no; k++, w += sizeof *s_obj)
        memcpy(w, &s_obj[(s_obj_n - no + k) % OPENPETE_BUGREC_SUB_OBJS], sizeof *s_obj);
    memcpy(w, "OPSUBPT1", 8);
    w += 8;
    put_u32(&w, (uint32_t)sizeof *s_portal);
    put_u32(&w, (uint32_t)nt);
    put_u64(&w, s_portal_n - nt);
    for (uint64_t k = 0; k < nt; k++, w += sizeof *s_portal)
        memcpy(w, &s_portal[(s_portal_n - nt + k) % OPENPETE_BUGREC_SUB_PORTALS], sizeof *s_portal);
    *out = b;
    *len = bytes;
    return (size_t)np;
}

void openpete_bugrec_subtick_counts(uint64_t* presents, uint64_t* objs, size_t* bytes)
{
    if (presents) *presents = s_pres_n;
    if (objs) *objs = s_obj_n;
    if (bytes) *bytes = s_pres ? OPENPETE_BUGREC_SUB_PRESENTS * sizeof *s_pres +
                                 OPENPETE_BUGREC_SUB_OBJS * sizeof *s_obj +
                                 OPENPETE_BUGREC_SUB_PORTALS * sizeof *s_portal : 0;
}
