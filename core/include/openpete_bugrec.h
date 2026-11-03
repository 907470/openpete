/* core/include/openpete_bugrec.h - the always-on bug-report recorder.
 *
 * The recorder keeps what a bug report needs to replay the last 30-60 s of
 * a session in another process:
 *
 *   - an anchor ring of two slots, each a complete savestate file image
 *     (openpete_state_image_capture) taken at a final yield, with the card
 *     images and `.modsave` record sets the session held, the input-log
 *     index and the bk2 row position (E, j) at the capture;
 *   - the per-tick gameplay-state hash rows (total and per region, the
 *     --gameplay-hash-log row) of every tick since the oldest anchor;
 *   - the bk2 row notes (bk2_record.h) and the host-loop trace records
 *     (openpete_host_trace.h) since the oldest anchor;
 *   - taint counters: the events a replay of the window cannot reproduce.
 *
 * An anchor is due every OPENPETE_BUGREC_CADENCE_VBLS VBLs and is admitted
 * at a final yield only when the fiber is parked at a site of the game's
 * replay-site table (openpete_game_runtime_t.replay_sites), the PsyCross CD
 * command queue holds no pending read, no speculative card write is pending
 * and the timeline is the committed one; a refused yield is counted by
 * cause and the next final yield retries. Level loads do not clear the
 * ring. A guest discontinuity (savestate load) clears it; a rewind seek
 * drops the anchors and hash rows past its target and truncates the row
 * notes with the input log. The ring keeps its newest admitted anchor
 * whatever its age, up to OPENPETE_BUGREC_RETAIN_MAX_VBLS, past which the
 * anchor is dropped and retention restarts at the next admitted one.
 *
 * Everything here runs on the sim thread at the host loop's quiescent
 * point, except the taint increments (any thread). Nothing stores into
 * PSX RAM.
 */
#ifndef OPENPETE_BUGREC_H
#define OPENPETE_BUGREC_H

#include "openpete_park_site.h"
#include "openpete_thread_roles.h"
typedef struct CPUState CPUState;

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Anchor cadence: 30 s of VBLs at 2250000/37609 Hz. */
#define OPENPETE_BUGREC_CADENCE_VBLS 1795u
/* Retention bound of the oldest held anchor: 20 minutes of VBLs. */
#define OPENPETE_BUGREC_RETAIN_MAX_VBLS (40u * OPENPETE_BUGREC_CADENCE_VBLS)
#define OPENPETE_BUGREC_SLOTS 2
/* Bytes of one card image (128 KiB). */
#define OPENPETE_BUGREC_CARD_BYTES (128u * 1024u)
#define OPENPETE_BUGREC_CARD_PATH 1280

/* Events a replay of the window cannot reproduce. */
typedef enum {
    BUGREC_TAINT_FREECAM = 0,   /* free camera engaged */
    BUGREC_TAINT_TELEPORT,      /* free camera teleport applied */
    BUGREC_TAINT_MOD_INPUT,     /* a mod read host input (binding_down, mouse_read) from a tick hook */
    BUGREC_TAINT_MOD_TOGGLE,    /* the enabled mod set changed */
    BUGREC_TAINT_MOD_CONFIG,    /* a mod config edit was applied */
    BUGREC_TAINT_REWIND,        /* a rewind seek moved the timeline */
    BUGREC_TAINT_STATE_LOAD,    /* guest state was replaced (savestate load) */
    BUGREC_TAINT_N
} openpete_bugrec_taint_t;

/* One anchor: a savestate file image and what replaying from it needs. */
typedef struct {
    int       valid;
    uint64_t  seq;            /* capture order, from 1 */
    uint8_t*  state;          /* the v6 state file's bytes */
    size_t    state_len, state_cap;
    int       card_held[2];   /* the port held a card */
    int       card_loaded[2]; /* its image was in memory (the game had read it) */
    char      card_path[2][OPENPETE_BUGREC_CARD_PATH];
    uint8_t*  card[2];        /* OPENPETE_BUGREC_CARD_BYTES each when loaded */
    uint8_t*  modsave[2];     /* the `.modsave` file's bytes, or NULL for none */
    size_t    modsave_len[2];
    int       log_idx;        /* input_log_count() at the capture: the first entry the window replays */
    unsigned  frame;          /* openpete_frame */
    int       vbl;            /* openpete_vbl */
    unsigned  feed_key;       /* openpete_feed_key_now() */
    unsigned  consume, consume_write;  /* the bk2 row position (E, j) */
    openpete_park_site_t site;
    int32_t   level;          /* game_read_player_pose's level id, -1 when none */
    uint32_t  trace_iter;     /* the host-trace iteration index at the capture */
    uint64_t  taint_at[BUGREC_TAINT_N];  /* taint totals at the capture */
    uint64_t  capture_us;
} openpete_bugrec_anchor_t;

/** @brief Register the guest-discontinuity handler and read the harness variables.
 *
 * OPENPETE_BUGREC_ANCHOR_AT=K[,K...] makes an anchor due at each feed key
 * K besides the cadence (admitted under the same rule).
 * OPENPETE_BUGREC_DUMP_AT=K with OPENPETE_BUGREC_DUMP_DIR=DIR writes the
 * ring with openpete_bugrec_dump at the first final yield at or past feed
 * key K. Called once before the host loop. */
void openpete_bugrec_init(void);

/** @brief Take an anchor when one is due and admitted, apply the retention bound and run a due dump.
 *
 * Called from the host loop's quiescent state ops once per iteration whose
 * drain reported a final yield, after the state requests, the rewind step
 * and the handoff poll.
 * @param cpu The guest register file the anchor's state image copies.
 * @param rewound Nonzero when a rewind seek moved the timeline this
 *        iteration: the fiber may sit below an intermediate yield, so no
 *        anchor is taken. */
void openpete_bugrec_step(const CPUState* cpu, int rewound) OP_REQUIRES(sim);

/** @brief Compute one tick's gameplay-state hash row into the ring and return it.
 *
 * Calls gameplay_hash_compute_ex once; the --gameplay-hash-log writer
 * formats the same row instead of hashing again.
 * @param frame openpete_frame after the tick.
 * @param vbl openpete_vbl after the tick.
 * @param per_out Receives the per-region hashes (openpete_state_region_count
 *        entries); valid until the next call.
 * @return The total hash. */
uint64_t openpete_bugrec_hash_tick(unsigned frame, int vbl, const uint64_t** per_out) OP_REQUIRES(sim);

/** @brief Drop the anchors and hash rows past a rewind target and truncate the bk2 row notes to the input log.
 *
 * @param frame The frame the seek restored. */
void openpete_bugrec_rewound(unsigned frame) OP_REQUIRES(sim);

/** @brief Count one taint event; safe from any thread.
 * @param kind The event. */
void openpete_bugrec_taint(openpete_bugrec_taint_t kind);

/** @brief Read the taint counts since an anchor's capture.
 *
 * @param a The anchor.
 * @param out Receives BUGREC_TAINT_N counts. */
void openpete_bugrec_taint_since(const openpete_bugrec_anchor_t* a, uint64_t out[BUGREC_TAINT_N]);

/** @brief Return the name of a taint kind ("freecam", "mod-input", ...).
 * @param kind The kind.
 * @return A static string. */
const char* openpete_bugrec_taint_name(openpete_bugrec_taint_t kind);

/** @brief Return a held anchor by age.
 *
 * @param i 0 for the newest, 1 for the one before it.
 * @return The anchor, or NULL when the slot holds none. */
const openpete_bugrec_anchor_t* openpete_bugrec_anchor(int i);

/** @brief Return the anchor a report replays from: the newest one at least OPENPETE_BUGREC_CADENCE_VBLS before the current frame, else the oldest held.
 *
 * @return The anchor, or NULL when none is held. */
const openpete_bugrec_anchor_t* openpete_bugrec_pick(void);

/** @brief Build the bk2 movie of the pad rows from an anchor to the current end of the input log, as a zip on the heap.
 *
 * The rows are the bk2 recorder's (bk2_record_window_zip): one per pad
 * write, grouped by openpete_consume from the anchor's E, so a replay
 * passes --bk2-frame-offset with the anchor's consume.
 * @param a The anchor.
 * @param zip Receives the archive; free with free().
 * @param len Receives its length.
 * @param rows Receives the movie rows written; may be NULL.
 * @return 0 on success, -1 when the window holds no pad write or allocation fails. */
int openpete_bugrec_segment_bk2(const openpete_bugrec_anchor_t* a, void** zip, size_t* len,
                                size_t* rows);

/** @brief Return the hash rows held from a frame on.
 *
 * A row is the u64 sequence frame, vbl, feed key, total, then one hash per
 * region; consecutive rows are openpete_bugrec_hash_stride() u64 apart.
 * @param from_frame The first frame wanted.
 * @param count Receives the row count.
 * @return The first row, or NULL when none; valid until the next tick. */
const uint64_t* openpete_bugrec_hash_rows(unsigned from_frame, size_t* count);

/** @brief Return the u64 count of one hash row.
 *
 * @return 4 plus the region count. */
size_t openpete_bugrec_hash_stride(void);

/** @brief Write the hash rows from a frame on in the --gameplay-hash-log CSV format, header included.
 *
 * @param fp The stream.
 * @param from_frame The first frame written.
 * @return The rows written. */
size_t openpete_bugrec_hash_csv(FILE* fp, unsigned from_frame);

/** @brief Write the ring to a directory: each anchor's state, card images, `.modsave` files, metadata and segment bk2, the hash rows, the host-trace records and the counters.
 *
 * The test hook OPENPETE_BUGREC_DUMP_AT calls it; files are written on the
 * calling thread.
 * @param dir An existing or creatable directory.
 * @return 0 when every file was written, -1 otherwise (logged). */
int openpete_bugrec_dump(const char* dir) OP_REQUIRES(sim);

/** @brief Log the exit report: anchors taken, deferred and dropped by cause, capture time percentiles, bytes held, rows and records held, hashing cost and taint totals. */
void openpete_bugrec_report(void) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGREC_H */
