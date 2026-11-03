/* core/record/bk2_feed.c - see bk2_feed.h. */
#include "bk2_feed.h"
#include "openpete_fiber.h"
#include "openpete_log.h"

#ifdef OPENPETE_HAS_PSYCROSS
#include "PsyX/PsyX_public.h"
#endif

#include <SDL3/SDL_atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pad writes one iteration holds a pad for. A fiber switch takes one write,
 * at the yield it resumes from (the boot's first stamp is the first
 * switch's); writes past the array take its last slot and are counted. */
#define BK2_FEED_ITER_PADS 64u

static struct {
    const bk2_movie_t* m;
    int       offset;
    int       stamped;     /* the row stamped before the coming yield */
    int       ended;       /* bk2_feed_row reached the movie's end */
    unsigned  iter_writes, iter_e, iter_e_last, iter_j;
    bk2_pad_write_t iter_pads[BK2_FEED_ITER_PADS];  /* the pad of each of the iteration's writes */
    unsigned long long pads_lost; /* writes past the array, recorded with the last slot's pad */
    unsigned* taken;       /* consume key: writes the replay took per group */
    unsigned char* closed; /* consume key: the group's consumer ran again */
    FILE*     map;
} s_feed = { NULL, 0, -1, 0, 0u, 0u, 0u, 0u, {{0u, {0, 0, 0, 0}}}, 0ull, NULL, NULL, NULL };

/* Observer events of the current runahead speculation, held until it is
 * rolled back (dropped) or adopted (delivered in order). */
typedef struct {
    unsigned char close;  /* 1: a consumer entry; 0: a pad write */
    int      stamped;     /* the row stamped before the event's yield */
    unsigned e, n;        /* (E, j) of a write; (E, writes) of a close */
    bk2_pad_write_t pad;  /* a write's player 1 pad, taken at the write */
} feed_spec_ev_t;

static struct {
    feed_spec_ev_t* ev;
    size_t n, cap;
    unsigned long long lost;  /* events dropped when the buffer could not grow */
} s_spec = { NULL, 0u, 0u, 0ull };

/** @brief Read player 1's pad from the first pad buffer, as the host's write left it.
 *
 * Called when a pad write is counted, which is before the vsync replaces
 * the bytes (a vsync that is not fresh shows the slots the last pad data,
 * and one with no pad data gets back the bytes the game left before it), so
 * they are the ones the host delivered for this write. With no pad
 * buffer registered the result is PsyCross's no-buffer default (0xFFFF,
 * axes 128), as the post-yield readback reports it.
 * @param out Receives the bits and the axes in the bk2 order. */
static void feed_pad_capture(bk2_pad_write_t* out) {
#ifdef OPENPETE_HAS_PSYCROSS
    unsigned char ax[4];
    PsyX_Pad_GetLastP1Analog(ax);   /* RX, RY, LX, LY */
    out->bits = PsyX_Pad_GetLastP1Bits();
    out->axes[0] = ax[2]; out->axes[1] = ax[3]; out->axes[2] = ax[0]; out->axes[3] = ax[1];
#else
    out->bits = 0xFFFFu;
    memset(out->axes, 128, sizeof out->axes);
#endif
}

/** @brief Hold one observer event of a speculation for delivery at adoption.
 * @param close 1 for a consumer entry, 0 for a pad write.
 * @param e The event's E.
 * @param n The write's j, or the closing group's writes.
 * @param pad A write's player 1 pad; NULL for a consumer entry. */
static void feed_spec_hold(unsigned char close, unsigned e, unsigned n,
                           const bk2_pad_write_t* pad) {
    if (s_spec.n == s_spec.cap) {
        const size_t cap = s_spec.cap ? s_spec.cap * 2u : 64u;
        feed_spec_ev_t* grown = (feed_spec_ev_t*)realloc(s_spec.ev, cap * sizeof *grown);
        if (!grown) { s_spec.lost++; return; }
        s_spec.ev = grown;
        s_spec.cap = cap;
    }
    feed_spec_ev_t* ev = &s_spec.ev[s_spec.n++];
    *ev = (feed_spec_ev_t){ close, s_feed.stamped, e, n, { 0xFFFFu, { 128, 128, 128, 128 } } };
    if (pad) ev->pad = *pad;
}

/** @brief Count a pad write of the timeline for the recorder, with its player 1 pad, and write its map line.
 * @param e openpete_consume at the write.
 * @param j openpete_consume_write at the write.
 * @param pad The pad the host delivered at the write. */
static void feed_note_write(unsigned e, unsigned j, const bk2_pad_write_t* pad) {
    unsigned at = s_feed.iter_writes;
    if (at >= BK2_FEED_ITER_PADS) { at = BK2_FEED_ITER_PADS - 1u; s_feed.pads_lost++; }
    s_feed.iter_pads[at] = *pad;
    if (s_feed.iter_writes++ == 0u) { s_feed.iter_e = e; s_feed.iter_j = j; }
    s_feed.iter_e_last = e;
    if (s_feed.map) fprintf(s_feed.map, "W %d %u %u\n", s_feed.stamped, e, j);
}

/** @brief Observer: take player 1's pad at a pad write of a yield, then count the write or hold it with the speculation.
 * @param e openpete_consume at the write.
 * @param j openpete_consume_write at the write. */
static void feed_on_write(unsigned e, unsigned j) {
    bk2_pad_write_t pad;
    feed_pad_capture(&pad);
    if (openpete_speculative) { feed_spec_hold(0u, e, j, &pad); return; }
    feed_note_write(e, j, &pad);
}

/** @brief Record the writes a group took in the replayed movie's instrument.
 * @param e The group's E.
 * @param writes The writes the replay took in it. */
static void feed_group_taken(unsigned e, unsigned writes) {
    if (!s_feed.taken) return;
    const long g = (long)e - s_feed.offset;
    if (g < 0 || g >= s_feed.m->group_count) return;
    if (writes > s_feed.taken[g]) s_feed.taken[g] = writes;
    s_feed.closed[g] = 1;
}

/** @brief Observer: close a group at its consumer's next entry.
 * @param e The closing group's E.
 * @param writes The writes the run took in it. */
static void feed_on_close(unsigned e, unsigned writes) {
    if (openpete_speculative) { feed_spec_hold(1u, e, writes, NULL); return; }
    feed_group_taken(e, writes);
}

static const openpete_feed_observer_t k_feed_obs = { feed_on_write, feed_on_close };

void bk2_feed_open(const bk2_movie_t* m, int offset) {
    s_feed.m = m;
    s_feed.offset = offset;
    if (m && m->row_key == BK2_ROW_KEY_CONSUME && m->group_count > 0) {
        s_feed.taken  = (unsigned*)calloc((size_t)m->group_count, sizeof *s_feed.taken);
        s_feed.closed = (unsigned char*)calloc((size_t)m->group_count, 1);
        if (!s_feed.taken || !s_feed.closed) {
            free(s_feed.taken); free(s_feed.closed);
            s_feed.taken = NULL; s_feed.closed = NULL;
            OPL_ERROR_F("bk2", "out of memory for the group instrument; it is off");
        }
    }
    const char* path = getenv("OPENPETE_FEED_MAP");
    if (path && *path) {
        s_feed.map = fopen(path, "w");
        if (s_feed.map) setvbuf(s_feed.map, NULL, _IOFBF, 1 << 16);
        else OPL_ERROR_F("bk2", "OPENPETE_FEED_MAP: cannot open %s", path);
    }
    openpete_feed_observer_set(&k_feed_obs);
}

void bk2_feed_speculation_end(int adopted) {
    if (adopted) {
        const int stamped = s_feed.stamped;
        for (size_t i = 0; i < s_spec.n; i++) {
            const feed_spec_ev_t* ev = &s_spec.ev[i];
            s_feed.stamped = ev->stamped;
            if (ev->close) feed_on_close(ev->e, ev->n);
            else           feed_note_write(ev->e, ev->n, &ev->pad);
        }
        s_feed.stamped = stamped;
        if (s_spec.lost)
            OPL_ERROR_F("bk2", "adopted speculation: %llu observer event(s) lost (out of "
                        "memory); the recorder and the group instrument miss them", s_spec.lost);
    }
    s_spec.n = 0u;
    s_spec.lost = 0ull;
}

int bk2_feed_row(void) {
    const bk2_movie_t* m = s_feed.m;
    if (!m) return -1;
    int row;
    if (m->row_key == BK2_ROW_KEY_CONSUME)
        row = bk2_movie_group_row(m, (int)openpete_consume - s_feed.offset, openpete_consume_write);
    else {
        const unsigned clock = m->row_key == BK2_ROW_KEY_VBL
                                   ? (unsigned)SDL_GetAtomicInt(&openpete_vbl) : openpete_frame;
        row = (int)clock - s_feed.offset;
    }
    if (row >= m->frame_count) s_feed.ended = 1;
    return row;
}

void bk2_feed_stamped(int row) { s_feed.stamped = row; }

unsigned bk2_feed_iter_take(unsigned* e, unsigned* e_last, unsigned* j,
                            const bk2_pad_write_t** pads) {
    const unsigned n = s_feed.iter_writes;
    *e = s_feed.iter_e;
    *e_last = s_feed.iter_e_last;
    *j = s_feed.iter_j;
    if (pads) *pads = s_feed.iter_pads;
    s_feed.iter_writes = 0u;
    return n;
}

void bk2_feed_map_present(void) {
    if (!s_feed.map) return;
    fprintf(s_feed.map, "P %u %d %u %u\n", openpete_frame, SDL_GetAtomicInt(&openpete_vbl),
            openpete_consume, openpete_consume_write);
}

void bk2_feed_report(void) {
    if (s_feed.map) { fclose(s_feed.map); s_feed.map = NULL; }
    if (s_feed.pads_lost)
        OPL_ERROR_F("bk2", "%llu pad write(s) past %u in one iteration took the last held pad",
                    s_feed.pads_lost, BK2_FEED_ITER_PADS);
    free(s_spec.ev);
    s_spec.ev = NULL; s_spec.n = s_spec.cap = 0u;
    if (!s_feed.taken) return;
    const bk2_movie_t* m = s_feed.m;
    /* The movie ended inside its last group: that group's writes are final. */
    if (s_feed.ended) feed_group_taken(openpete_consume, openpete_consume_write);
    int compared = 0, fewer = 0, more = 0, first_fewer = -1, first_more = -1;
    for (int g = 0; g < m->group_count; g++) {
        if (!s_feed.closed[g]) continue;
        compared++;
        const unsigned rows = (unsigned)m->group_rows[g];
        if (s_feed.taken[g] < rows) { fewer++; if (first_fewer < 0) first_fewer = g; }
        if (s_feed.taken[g] > rows) { more++;  if (first_more  < 0) first_more  = g; }
    }
    OPL_INFO_F("bk2", "bk2 feed groups: %d of %d compared; fewer writes than rows %d "
               "(first group %d), more %d (first group %d)",
               compared, m->group_count, fewer, first_fewer, more, first_more);
    free(s_feed.taken); free(s_feed.closed);
    s_feed.taken = NULL; s_feed.closed = NULL;
}
