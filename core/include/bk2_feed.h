/* core/include/bk2_feed.h - the replayed movie's row at the current row
 * position, the run's (E, j) map, and the group-count instrument.
 *
 * A consume-keyed movie stamps row j of group E (openpete_consume,
 * openpete_consume_write; bk2_movie_group_row); a movie on an older key
 * stamps the row its counter indexes (openpete_frame, or openpete_vbl for
 * "OpenpeteFeedKey vbl"), which replays it as recorded and is the
 * conversion tools/bk2_rekey.py writes to the consume key.
 *
 * The module observes every pad write of a yield and consumer entry
 * (openpete_feed_observer_t) to the following ends; the events of a runahead
 * speculation are held and delivered only when the speculation is adopted
 * (bk2_feed_speculation_end):
 *   - collect each iteration's writes for the recorder (bk2_feed_iter_take),
 *     each with the player 1 pad the host delivered at it (bk2_pad_write_t);
 *   - write the OPENPETE_FEED_MAP log: `W row e j` per write, with the row
 *     the host stamped before it (as bk2_feed_row returned it; -1 with no
 *     movie), and `P frame vbl e j` per canonical
 *     present (bk2_feed_map_present);
 *   - count, for a consume-keyed movie, the groups whose recorded row count
 *     differs from the writes the replay took in them (bk2_feed_report).
 */
#ifndef OPENPETE_BK2_FEED_H
#define OPENPETE_BK2_FEED_H

#include "bk2_movie.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Player 1's pad as the host delivered it at one pad write: the bytes in
 * the first pad buffer when the write is counted, before the vsync's
 * takeback can replace them. Axes in the bk2 order LX, LY, RX, RY. */
typedef struct bk2_pad_write {
    uint16_t      bits;     /* 1 released, 0 pressed */
    unsigned char axes[4];
} bk2_pad_write_t;

/** @brief Install the write and consumer observer, and select the replayed movie.
 *
 * @param m The movie replayed by --bk2, or NULL when none; it must outlive
 *        the run.
 * @param offset The counter value at which row 0 (group 0) plays. */
void bk2_feed_open(const bk2_movie_t* m, int offset);

/** @brief End a runahead speculation's hold on the observer events it ran.
 *
 * Adopted: its pad writes and consumer entries are the timeline, delivered
 * in order with the rows stamped before them (the recorder, the map and the
 * group instrument see them as if run outside speculation). Rolled back:
 * they are dropped.
 * @param adopted Nonzero when the speculation became the timeline. */
void bk2_feed_speculation_end(int adopted);

/** @brief Return the movie row the host stamps at the current row position.
 *
 * @return The row index; negative before the movie (-1 with none open);
 *         the movie's row count or more once it has ended. */
int  bk2_feed_row(void);

/** @brief Hold the movie before its first row, or release it.
 *
 * While held bk2_feed_row returns -1, as before the movie's offset, so no
 * row stamps and the pad is live input (neutral in a headless run).
 * @param on Nonzero to hold, 0 to release. */
void bk2_feed_hold(int on);

/** @brief Note the row the host stamped for the next yield, for the map's W lines.
 * @param row The row, as bk2_feed_row returned it. */
void bk2_feed_stamped(int row);

/** @brief Take the writes the iteration's yield ran, for the recorder, and clear them.
 *
 * @param e Receives openpete_consume at the first of them.
 * @param e_last Receives openpete_consume at the last of them.
 * @param j Receives openpete_consume_write at the first of them.
 * @param pads Receives the player 1 pad of each write in order, valid until
 *        the next pad write; NULL when not wanted.
 * @return The number of pad writes of yields since the previous call. */
unsigned bk2_feed_iter_take(unsigned* e, unsigned* e_last, unsigned* j,
                            const bk2_pad_write_t** pads);

/** @brief Write a `P frame vbl e j` line to the OPENPETE_FEED_MAP log, when it is open. */
void bk2_feed_map_present(void);

/** @brief Log the group-count instrument of a consume-keyed movie.
 *
 * Compared: every group the replay closed (its consumer ran again) and,
 * once the movie ended, the last group. Logged as `bk2 feed groups:` with
 * the groups compared, those with fewer writes than rows (unread trailing
 * rows) and those with more (held rows). Nothing for a movie on an older
 * key or with no movie. Closes the map and frees the speculation hold. */
void bk2_feed_report(void);

#ifdef __cplusplus
}
#endif

#endif
