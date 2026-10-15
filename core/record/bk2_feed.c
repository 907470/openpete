/* core/record/bk2_feed.c - see bk2_feed.h. */
#include "bk2_feed.h"
#include "openpete_fiber.h"
#include "openpete_log.h"

#include <SDL3/SDL_atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    const bk2_movie_t* m;
    int       offset;
    int       stamped;     /* the row stamped before the coming yield */
    int       ended;       /* bk2_feed_row reached the movie's end */
    unsigned  iter_writes, iter_e, iter_e_last, iter_j;
    unsigned* taken;       /* consume key: writes the replay took per group */
    unsigned char* closed; /* consume key: the group's consumer ran again */
    FILE*     map;
} s_feed = { NULL, 0, -1, 0, 0u, 0u, 0u, 0u, NULL, NULL, NULL };

/** @brief Observer: count a pad write of a yield for the recorder and write its map line.
 * @param e openpete_consume at the write.
 * @param j openpete_consume_write at the write. */
static void feed_on_write(unsigned e, unsigned j) {
    if (openpete_speculative) return;
    if (s_feed.iter_writes++ == 0u) { s_feed.iter_e = e; s_feed.iter_j = j; }
    s_feed.iter_e_last = e;
    if (s_feed.map) fprintf(s_feed.map, "W %d %u %u\n", s_feed.stamped, e, j);
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
    if (openpete_speculative) return;
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

unsigned bk2_feed_iter_take(unsigned* e, unsigned* e_last, unsigned* j) {
    const unsigned n = s_feed.iter_writes;
    *e = s_feed.iter_e;
    *e_last = s_feed.iter_e_last;
    *j = s_feed.iter_j;
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
