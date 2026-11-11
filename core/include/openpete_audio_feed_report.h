/* core/include/openpete_audio_feed_report.h - the audio device feed's
 * counters and event ring as text: the exit summary's lines and the bug
 * report's audio-feed.txt (core/host/audio_feed_report.c). */
#ifndef OPENPETE_AUDIO_FEED_REPORT_H
#define OPENPETE_AUDIO_FEED_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* A copy of the feed's state at one instant; opaque. */
typedef struct openpete_audio_feed_snap openpete_audio_feed_snap_t;

/* Receives one line of text, without its newline. */
typedef void (*openpete_audio_feed_line_fn)(void* ud, const char* line);

/** @brief Copy the device feed's counters, its event ring and the host's mute state and clocks at the call into a heap snapshot.
 *
 * The counters (and the SPU's feature and host-input counters) are read
 * under the feed's mutex; the event ring is read without a lock. Off the
 * sim's thread at a fatal exit a stuck thread may hold the mutex, so the
 * caller passes @p counters 0 there and only the ring is read.
 * @param counters 1 to read the counters, 0 to leave them out.
 * @param events 1 to copy the event ring, 0 to leave it out.
 * @return The snapshot, or NULL when allocation fails. */
openpete_audio_feed_snap_t* openpete_audio_feed_snapshot(int counters, int events);

/** @brief Free a snapshot.
 * @param s The snapshot; NULL is ignored. */
void openpete_audio_feed_snap_free(openpete_audio_feed_snap_t* s);

/** @brief Emit the counter lines (`spu:` and `feed:`) of a snapshot, the text the exit summary logs.
 * @param s The snapshot.
 * @param line Called once per line.
 * @param ud Passed to @p line. */
void openpete_audio_feed_counters(const openpete_audio_feed_snap_t* s, openpete_audio_feed_line_fn line, void* ud);

/** @brief Emit a snapshot as the bug report's audio-feed.txt: the version, mute and device state, the clocks of the snapshot, the event counts, the counter lines, then one row per held event, oldest first.
 * @param s The snapshot.
 * @param line Called once per line.
 * @param ud Passed to @p line. */
void openpete_audio_feed_text(const openpete_audio_feed_snap_t* s, openpete_audio_feed_line_fn line, void* ud);

/** @brief The number of events a snapshot holds.
 * @param s The snapshot; NULL gives 0.
 * @return The count. */
int openpete_audio_feed_snap_events(const openpete_audio_feed_snap_t* s);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_AUDIO_FEED_REPORT_H */
