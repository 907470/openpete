/* core/ui/openpete_osd.c — the OSD's toast ring. See openpete_osd.h.
 *
 * State only: no drawing, no glyphs, no renderer. What lives here is the ring,
 * the coalescing rule, the rate limit, the animation state machine and the two
 * id-correlated snapshots the sim and present threads read. Everything that
 * decides WHEN is driven by the `now` the present thread passes in, so the
 * whole module runs on a synthetic clock in a test. */

#include "openpete_osd.h"
#include "openpete_overlay_anchor.h"
#include "openpete_fiber.h"   /* openpete_speculative / openpete_replaying */

#include <SDL3/SDL_mutex.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- tuning ------------------------------------------------------------- */
#define OSD_T_IN_NS    (150 * OP_NS_PER_MS)   /* fade + slide in            */
#define OSD_T_OUT_NS   (300 * OP_NS_PER_MS)   /* fade + slide out           */
/* Time constant of the stack glide: when a toast above expires, the ones below
 * chase their new home rather than jumping to it. Short enough to feel like a
 * settle, long enough to read as motion. */
#define OSD_GLIDE_TAU_NS (120.0 * (double)OP_NS_PER_MS)
#define OSD_STACK_GAP   0.010f                /* between toasts, overlay units */

/* Per-source rate limit. The burst absorbs a handful of settings changed in
 * one breath; the refill is what stops a mod in a tick loop owning the ring. */
#define OSD_BUCKET_BURST   4.0f
#define OSD_BUCKET_REFILL  2.0f   /* tokens per second */
#define OSD_BUCKETS        8

typedef enum { ST_FREE = 0, ST_RISING, ST_HOLD, ST_FALLING } toast_state_t;

typedef struct {
    uint32_t      id;          /* 0 = free slot                              */
    uint32_t      seq;         /* monotonic post order — the age sort key    */
    char          key[OPENPETE_OSD_KEY_MAX];
    char          text[OPENPETE_OSD_TEXT_MAX];
    uint32_t      ttl_ms;
    toast_state_t state;
    op_time_t     birth;       /* first present WITH geometry; 0 until then  */
    op_time_t     fall_start;
    float         alpha, slide;
    float         y;           /* eased stack offset from the anchor edge    */
    int           y_seeded;    /* 0 = snap to the first target, don't glide  */
    int           repaint;     /* coalesced since the last present           */
} toast_t;

typedef struct {
    char     name[OPENPETE_OSD_SRC_MAX];
    float    tokens;
    uint32_t last_seq;         /* for LRU replacement when the table is full */
} bucket_t;

static SDL_Mutex *s_mx;
static SDL_Mutex *osd_mx(void) {
    /* Lazily created. The first notify() can come from any thread, but it
     * cannot RACE the first one: the configuration setters run at
     * options-apply time on the host loop, before mods or the sim thread
     * exist, and they take the lock. */
    if (!s_mx) s_mx = SDL_CreateMutex();
    return s_mx;
}

static toast_t   s_ring[OPENPETE_OSD_MAX];
static bucket_t  s_bucket[OSD_BUCKETS];
static uint32_t  s_next_id  = 1;
static uint32_t  s_next_seq = 1;
static op_time_t s_prev_now;        /* the previous present's clock reading */

static int      s_enabled = 1;
static int      s_anchor  = OPENPETE_ANCHOR_TOP_LEFT;
static uint32_t s_default_ms = 3000;

/* ---- configuration ------------------------------------------------------ */
void openpete_osd_set_enabled(int on) {
    const int was = s_enabled;
    s_enabled = on ? 1 : 0;
    /* Turning notifications OFF drops whatever is up. Leaving them in the ring
     * would strand them: the builder stops publishing geometry, so nothing
     * starts their timers, and they would all reappear the moment the player
     * turned notifications back on — announcements of things that happened
     * minutes ago. */
    if (was && !s_enabled) openpete_osd_reset();
}
int  openpete_osd_enabled(void)       { return s_enabled; }
void openpete_osd_set_anchor(int a) {
    if (a >= 0 && a < OPENPETE_ANCHOR_COUNT) s_anchor = a;
}
int  openpete_osd_anchor(void) { return s_anchor; }
void openpete_osd_set_default_timeout_ms(uint32_t ms) {
    if (ms == 0) return;
    s_default_ms = (ms > OPENPETE_OSD_TIMEOUT_MAX_MS) ? OPENPETE_OSD_TIMEOUT_MAX_MS : ms;
}
uint32_t openpete_osd_default_timeout_ms(void) { return s_default_ms; }

void openpete_osd_reset(void) {
    SDL_LockMutex(osd_mx());
    memset(s_ring, 0, sizeof s_ring);
    memset(s_bucket, 0, sizeof s_bucket);
    s_next_id = 1; s_next_seq = 1; s_prev_now = 0;
    SDL_UnlockMutex(osd_mx());
}

/* ---- rate limit --------------------------------------------------------- */
/** @brief Find a source's rate-limit bucket, creating it full when absent.
 *
 * A new bucket takes a free entry, else replaces the entry with the lowest
 * last_seq. The caller debits a bucket only for a new ring entry, never for
 * a repaint: key repeat delivers tens of adjust events a second, a burst-4
 * bucket drains in under a second, and a debited repaint would leave the
 * toast showing a stale value while the menu shows the live one. Called with
 * the lock held.
 * @param src The source name, truncated to OPENPETE_OSD_SRC_MAX - 1 bytes.
 * @return The bucket; never NULL. */
static bucket_t *bucket_for(const char *src) {
    int free_i = -1, lru_i = 0;
    for (int i = 0; i < OSD_BUCKETS; i++) {
        if (s_bucket[i].name[0] == 0) { if (free_i < 0) free_i = i; continue; }
        if (strcmp(s_bucket[i].name, src) == 0) return &s_bucket[i];
        if (s_bucket[i].last_seq < s_bucket[lru_i].last_seq) lru_i = i;
    }
    bucket_t *b = &s_bucket[free_i >= 0 ? free_i : lru_i];
    memset(b, 0, sizeof *b);
    snprintf(b->name, sizeof b->name, "%s", src);
    b->tokens = OSD_BUCKET_BURST;
    return b;
}

/* ---- ring helpers (all called with the lock held) ----------------------- */
/** @brief Find the live toast, not yet fading out, that carries a coalescing key.
 *
 * @param key The key; NULL or empty never matches.
 * @return The toast, or NULL. */
static toast_t *find_key(const char *key) {
    if (!key || !*key) return NULL;
    for (int i = 0; i < OPENPETE_OSD_MAX; i++)
        if (s_ring[i].id && s_ring[i].state != ST_FALLING &&
            strcmp(s_ring[i].key, key) == 0)
            return &s_ring[i];
    return NULL;
}

/** @brief Pick the slot a new toast takes.
 *
 * A free slot, else the oldest toast past its rise, else, when every toast
 * is still rising, the oldest outright. The last fallback keeps a burst of
 * nine posts inside one present from dropping the newest.
 * @return The slot; never NULL. */
static toast_t *evict_victim(void) {
    toast_t *best = NULL, *oldest = NULL;
    for (int i = 0; i < OPENPETE_OSD_MAX; i++) {
        toast_t *t = &s_ring[i];
        if (!t->id) return t;
        if (!oldest || t->seq < oldest->seq) oldest = t;
        if (t->state != ST_RISING && (!best || t->seq < best->seq)) best = t;
    }
    return best ? best : oldest;
}

/* ---- posting ------------------------------------------------------------ */
void openpete_osd_notify(const char *source, const char *key,
                         uint32_t timeout_ms, const char *fmt, ...) {
    if (!s_enabled || !fmt) return;
    /* Speculative ticks are rolled back and replayed ticks are re-executed: a
     * notification from either announces something that never committed, or
     * announces it a second time. Read outside the lock — plain cross-thread
     * ints, the same relaxed idiom their other consumers use. */
    if (openpete_speculative || openpete_replaying) return;

    char text[OPENPETE_OSD_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (timeout_ms == 0) timeout_ms = s_default_ms;
    else if (timeout_ms > OPENPETE_OSD_TIMEOUT_MAX_MS)
        timeout_ms = OPENPETE_OSD_TIMEOUT_MAX_MS;

    SDL_LockMutex(osd_mx());
    toast_t *t = find_key(key);
    if (t) {
        /* Repaint. The id is KEPT deliberately: a new id would be absent from
         * the geometry the sim thread has already published, so the toast
         * would vanish for up to a tick before reappearing. Keeping it shows
         * one tick of stale text at the restarted timer instead — correct, and
         * invisible. The timer restarts on the REPAINT's timeout, so a
         * recurring notification can lengthen its own dwell without abandoning
         * its key (and so its slot). */
        memcpy(t->text, text, sizeof text);
        t->ttl_ms  = timeout_ms;
        t->repaint = 1;   /* the present thread rewinds the hold — it owns the clock */
        SDL_UnlockMutex(osd_mx());
        return;
    }

    bucket_t *b = bucket_for((source && *source) ? source : "host");
    if (b->tokens < 1.0f) { SDL_UnlockMutex(osd_mx()); return; }
    b->tokens -= 1.0f;
    b->last_seq = s_next_seq;

    t = evict_victim();
    memset(t, 0, sizeof *t);
    t->id     = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;   /* ids are compared, never ordered */
    t->seq    = s_next_seq++;
    t->state  = ST_RISING;
    t->ttl_ms = timeout_ms;
    snprintf(t->key, sizeof t->key, "%s", (key && *key) ? key : "");
    memcpy(t->text, text, sizeof text);
    SDL_UnlockMutex(osd_mx());
}

/* ---- both readers share one order --------------------------------------- */
/** @brief Gather the occupied slots, sorted oldest first by post order.
 *
 * The post order is the stack order; both readers use this function so the
 * ids they correlate on stand in the same order. An insertion sort over at
 * most OPENPETE_OSD_MAX entries. Called with the lock held.
 * @param out Receives the toasts; holds OPENPETE_OSD_MAX entries.
 * @return The count written. */
static int collect(toast_t **out) {
    int n = 0;
    for (int i = 0; i < OPENPETE_OSD_MAX; i++)
        if (s_ring[i].id) out[n++] = &s_ring[i];
    for (int i = 1; i < n; i++) {
        toast_t *k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->seq > k->seq) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return n;
}

/* ---- the sim-thread reader ---------------------------------------------- */
int openpete_osd_tick_items(openpete_osd_item_t *out, int cap) {
    if (!out || cap <= 0) return 0;
    toast_t *live[OPENPETE_OSD_MAX];
    SDL_LockMutex(osd_mx());
    int n = collect(live);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        out[i].id = live[i]->id;
        memcpy(out[i].text, live[i]->text, sizeof out[i].text);
    }
    SDL_UnlockMutex(osd_mx());
    return n;
}

int openpete_osd_any_live(void) {
    int any = 0;
    SDL_LockMutex(osd_mx());
    for (int i = 0; i < OPENPETE_OSD_MAX; i++) if (s_ring[i].id) { any = 1; break; }
    SDL_UnlockMutex(osd_mx());
    return any;
}

/* ---- the present-thread reader ------------------------------------------ */
/** @brief Ease x with the cubic 3x^2 - 2x^3, clamped to [0, 1].
 *
 * @param x The linear progress.
 * @return 0 for x <= 0, 1 for x >= 1, else the eased value. */
static float smoothstep(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

static float ready_height(const openpete_osd_ready_t *r, int n, uint32_t id) {
    for (int i = 0; i < n; i++) if (r[i].id == id) return r[i].box_h;
    return -1.0f;   /* no geometry published for this toast yet */
}

int openpete_osd_present_update(op_time_t now,
                                const openpete_osd_ready_t *ready, int n_ready,
                                openpete_osd_view_t *out, int cap) {
    if (!out || cap <= 0) return 0;
    toast_t *live[OPENPETE_OSD_MAX];
    int written = 0;

    SDL_LockMutex(osd_mx());

    /* Step length for everything rate-based below. First call (or one that
     * goes backwards, e.g. a deterministic-capture clock reset) contributes
     * nothing rather than a wild jump. */
    const double dt_s = (s_prev_now != 0 && now > s_prev_now)
                      ? (double)(now - s_prev_now) / (double)OP_NS_PER_SEC
                      : 0.0;
    const op_time_t dt_ns = (s_prev_now != 0 && now > s_prev_now)
                          ? (now - s_prev_now) : 0;
    s_prev_now = now;

    /* Refill every bucket. Done here because this is the one entry point that
     * carries a clock; notify() only ever debits. */
    if (dt_s > 0.0) {
        for (int i = 0; i < OSD_BUCKETS; i++) {
            if (!s_bucket[i].name[0]) continue;
            s_bucket[i].tokens += (float)(dt_s * OSD_BUCKET_REFILL);
            if (s_bucket[i].tokens > OSD_BUCKET_BURST)
                s_bucket[i].tokens = OSD_BUCKET_BURST;
        }
    }

    /* Exponential approach factor for the stack glide, derived from the actual
     * step so the settle takes the same wall time at any present rate. */
    float glide = 1.0f;
    if (dt_ns > 0) {
        double x = (double)dt_ns / OSD_GLIDE_TAU_NS;
        /* 1 - e^-x, without pulling in <math.h> for one call: two terms of the
         * series are exact enough well past x = 1, and it is clamped anyway. */
        double e = 1.0 / (1.0 + x + 0.5 * x * x);
        glide = (float)(1.0 - e);
        if (glide > 1.0f) glide = 1.0f;
    }

    int n = collect(live);
    float stack = 0.0f;   /* cumulative height of the toasts above this one */

    for (int i = 0; i < n; i++) {
        toast_t *t = live[i];
        const float h = ready_height(ready, n_ready, t->id);
        if (h < 0.0f) continue;   /* no geometry yet: skip it, and leave the
                                   * timer unstarted — a toast posted before
                                   * the glyph font cached is late, never
                                   * lost. Drawing its slot anyway would draw
                                   * some OTHER toast's glyphs. */
        if (t->birth == 0) {
            t->birth   = now;
            t->state   = ST_RISING;
            t->repaint = 0;
        } else if (t->repaint) {
            /* Restart the HOLD, not the whole animation: replaying the fade-in
             * on every repaint would pump a held key's toast between
             * transparent and solid a dozen times a second. Rewinding birth to
             * the end of the rise leaves it fully faded in and gives it a
             * fresh full dwell. */
            t->repaint = 0;
            if (now - t->birth > OSD_T_IN_NS) {
                t->birth = now - OSD_T_IN_NS;
                t->state = ST_HOLD;
            } else {
                t->state = ST_RISING;   /* still rising; let it finish */
            }
            t->fall_start = 0;
        }

        const op_time_t age = now - t->birth;
        switch (t->state) {
        case ST_RISING:
            if (age >= OSD_T_IN_NS) t->state = ST_HOLD;
            break;
        case ST_HOLD:
            if (age >= OSD_T_IN_NS + (op_time_t)t->ttl_ms * OP_NS_PER_MS) {
                t->state      = ST_FALLING;
                t->fall_start = now;
            }
            break;
        case ST_FALLING:
            if (now - t->fall_start >= OSD_T_OUT_NS) {
                memset(t, 0, sizeof *t);   /* retired */
                continue;
            }
            break;
        default: break;
        }

        /* One 0..1 presence drives BOTH the fade and the slide, so they can
         * never disagree about how far in a toast is. */
        float p;
        if (t->state == ST_FALLING)
            p = 1.0f - smoothstep((float)((double)(now - t->fall_start) /
                                          (double)OSD_T_OUT_NS));
        else
            p = smoothstep((float)((double)age / (double)OSD_T_IN_NS));
        t->alpha = p;
        t->slide = p;

        /* Stack glide: a toast appearing snaps to its slot (there is nothing
         * to glide FROM), an existing one eases toward its new home when
         * something above it retires — so the stack settles instead of
         * jumping. */
        if (!t->y_seeded) { t->y = stack; t->y_seeded = 1; }
        else               t->y += (stack - t->y) * glide;

        out[written].id    = t->id;
        out[written].alpha = t->alpha;
        out[written].slide = t->slide;
        out[written].y     = t->y;
        written++;

        stack += h + OSD_STACK_GAP;
        if (written >= cap) break;
    }

    SDL_UnlockMutex(osd_mx());
    return written;
}
