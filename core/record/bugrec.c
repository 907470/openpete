/* core/record/bugrec.c - the always-on bug-report recorder; see
 * openpete_bugrec.h. */
#include "openpete_bugrec.h"

#include "bk2_record.h"
#include "game_extras.h"
#include "openpete.h"
#include "openpete_discontinuity.h"
#include "openpete_fiber.h"
#include "openpete_game_runtime.h"
#include "openpete_host_trace.h"
#include "openpete_input_log.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_modsave.h"
#include "openpete_pace_log.h"
#include "openpete_psx.h"
#include "openpete_state.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

/* The PsyCross CD command queue's pending reads (LIBCD.C). */
extern int PsyX_CD_QueuePending(void);
/* gameplay-state hash over the manifest regions (core/runtime/run.c). */
extern uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);
/* The --gameplay-hash-log CSV writers (core/runtime/run.c). */
extern void gameplay_hash_log_emit_header(FILE* fp);
extern void gameplay_hash_log_emit(FILE* fp, unsigned frame, int vbl, uint64_t total,
                                   const uint64_t* per_region);

/* Why a due anchor was not taken at a final yield. */
enum {
    DEF_BOOT = 0,     /* fast boot still running */
    DEF_SPECULATIVE,  /* the timeline is a runahead speculation */
    DEF_SITE,         /* the park site is not in the replay-site table */
    DEF_CD,           /* the CD command queue holds a pending read */
    DEF_CARD,         /* a speculative card write is pending */
    DEF_REWOUND,      /* a rewind seek ran this iteration */
    DEF_N
};
static const char* const k_def_names[DEF_N] = {
    "boot", "speculative", "site", "cd-queue", "card-pending", "rewound",
};

/* Why a held anchor was dropped. */
enum { DROP_DISCONTINUITY = 0, DROP_REWIND, DROP_AGE, DROP_ALLOC, DROP_N };
static const char* const k_drop_names[DROP_N] = { "discontinuity", "rewind", "age", "alloc" };

static const char* const k_taint_names[BUGREC_TAINT_N] = {
    "freecam", "teleport", "mod-input", "mod-toggle", "mod-config", "rewind", "state-load",
};

static _Atomic uint64_t s_taint[BUGREC_TAINT_N];

#define CAPTURE_SAMPLES 4096u

static struct {
    openpete_bugrec_anchor_t slot[OPENPETE_BUGREC_SLOTS];
    uint64_t seq;
    unsigned next_due;        /* openpete_frame at which the cadence makes an anchor due */
    int      due_extra;       /* a harness feed key made one due */
    unsigned long long taken, deferred_yields[DEF_N], deferred_episodes[DEF_N], dropped[DROP_N];
    int      deferring;       /* the cause the current due anchor last waited on, or -1 */
    uint32_t capture_us[CAPTURE_SAMPLES];
    unsigned capture_n;       /* samples held (the first CAPTURE_SAMPLES captures) */
    uint64_t capture_max_us;
    /* Harness schedule. */
    unsigned* extra_at;       /* OPENPETE_BUGREC_ANCHOR_AT feed keys, ascending */
    int       extra_n, extra_next;
    int       dump_at;        /* OPENPETE_BUGREC_DUMP_AT, or -1 */
    char*     dump_dir;
    /* Hash ring: rows of stride u64, [0, rows_n) live. */
    uint64_t* rows;
    size_t    rows_n, rows_cap, stride;
    unsigned long long rows_total, rows_trimmed;
    uint64_t  hash_ns_total, hash_ns_max, hash_ticks;
} s_br = { .deferring = -1, .dump_at = -1 };

/** @brief Return the slot index holding the newest valid anchor, or -1. */
static int newest_slot(void) {
    int best = -1;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++)
        if (s_br.slot[i].valid && (best < 0 || s_br.slot[i].seq > s_br.slot[best].seq)) best = i;
    return best;
}

/** @brief Return the slot index holding the oldest valid anchor, or -1. */
static int oldest_slot(void) {
    int best = -1;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++)
        if (s_br.slot[i].valid && (best < 0 || s_br.slot[i].seq < s_br.slot[best].seq)) best = i;
    return best;
}

/** @brief Mark an anchor invalid, keeping its buffers for reuse.
 * @param a The anchor.
 * @param cause The DROP_* cause counted. */
static void anchor_drop(openpete_bugrec_anchor_t* a, int cause) {
    if (!a->valid) return;
    a->valid = 0;
    s_br.dropped[cause]++;
}

/** @brief Trim the hash rows, the bk2 row notes and the host-trace records to the oldest held anchor.
 *
 * With no anchor held nothing before the current point is retained. */
static void retention_apply(void) OP_REQUIRES(sim) {
    const int o = oldest_slot();
    const unsigned from_frame = o >= 0 ? s_br.slot[o].frame : openpete_frame;
    size_t keep_from = 0;
    while (keep_from < s_br.rows_n && s_br.rows[keep_from * s_br.stride] < from_frame) keep_from++;
    if (keep_from) {
        memmove(s_br.rows, s_br.rows + keep_from * s_br.stride,
                (s_br.rows_n - keep_from) * s_br.stride * sizeof *s_br.rows);
        s_br.rows_n -= keep_from;
        s_br.rows_trimmed += keep_from;
    }
    input_log_lock();
    bk2_notes_retain_from(o >= 0 ? s_br.slot[o].log_idx : input_log_count());
    input_log_unlock();
    host_trace_ring_retain_from(o >= 0 ? s_br.slot[o].trace_iter : host_trace_iter_index());
}

/** @brief Guest discontinuity handler: a savestate load replaced the timeline, so every anchor and hash row is dropped and the next final yield takes an anchor.
 *
 * The row notes and host-trace records are trimmed by the next step's
 * retention pass, which finds no anchor held. */
static void bugrec_on_discontinuity(void) {
    openpete_bugrec_taint(BUGREC_TAINT_STATE_LOAD);
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++) anchor_drop(&s_br.slot[i], DROP_DISCONTINUITY);
    s_br.rows_trimmed += s_br.rows_n;
    s_br.rows_n = 0;
    s_br.next_due = openpete_frame;
}

/** @brief Parse a comma-separated list of non-negative feed keys, sorted ascending.
 * @param s The list.
 * @param out Receives the malloc'd keys.
 * @return The count. */
static int parse_keys(const char* s, unsigned** out) {
    int n = 0, cap = 0;
    unsigned* v = NULL;
    while (s && *s) {
        char* end = NULL;
        const unsigned long k = strtoul(s, &end, 10);
        if (end == s) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            unsigned* g = (unsigned*)realloc(v, (size_t)cap * sizeof *g);
            if (!g) break;
            v = g;
        }
        v[n++] = (unsigned)k;
        s = *end == ',' ? end + 1 : end;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { unsigned t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    *out = v;
    return n;
}

void openpete_bugrec_init(void) {
    (void)openpete_on_guest_discontinuity(bugrec_on_discontinuity);
    s_br.extra_n = parse_keys(getenv("OPENPETE_BUGREC_ANCHOR_AT"), &s_br.extra_at);
    const char* at = getenv("OPENPETE_BUGREC_DUMP_AT");
    const char* dir = getenv("OPENPETE_BUGREC_DUMP_DIR");
    if (at && *at) {
        if (dir && *dir) {
            s_br.dump_at = atoi(at);
            s_br.dump_dir = strdup(dir);
        } else {
            OPL_ERROR_F("bugrec", "OPENPETE_BUGREC_DUMP_AT is set without OPENPETE_BUGREC_DUMP_DIR; no dump");
        }
    }
}

/** @brief Report whether the fiber's park site is one the game's replay route reaches.
 * @return 1 when the table holds the site or the game declares no table. */
static int site_admitted(void) {
    const openpete_game_runtime_t* rt = openpete_game_runtime();
    if (!rt->replay_sites || rt->replay_site_count <= 0) return 1;
    for (int i = 0; i < rt->replay_site_count; i++)
        if (rt->replay_sites[i].ra == openpete_yield_site.ra &&
            rt->replay_sites[i].sp == openpete_yield_site.sp)
            return 1;
    return 0;
}

/** @brief Return why an anchor cannot be taken at this final yield, or -1 when it can.
 * @param rewound Nonzero when a rewind seek ran this iteration. */
static int admission_refusal(int rewound) {
    if (rewound) return DEF_REWOUND;
    if (openpete_fast_boot_active) return DEF_BOOT;
    if (openpete_speculative) return DEF_SPECULATIVE;
    if (!site_admitted()) return DEF_SITE;
    if (PsyX_CD_QueuePending() > 0) return DEF_CD;
    if (openpete_memcard_spec_pending()) return DEF_CARD;
    return -1;
}

/** @brief Copy the session's card images and `.modsave` record sets into an anchor.
 * @param a The anchor.
 * @return 0, or -1 when a buffer could not be allocated. */
static int anchor_cards(openpete_bugrec_anchor_t* a) {
    for (int p = 0; p < 2; p++) {
        a->card_held[p] = openpete_memcard_held_path(p, a->card_path[p], sizeof a->card_path[p]);
        a->card_loaded[p] = 0;
        free(a->modsave[p]);
        a->modsave[p] = NULL;
        a->modsave_len[p] = 0;
        if (!a->card_held[p]) continue;
        if (!a->card[p] && !(a->card[p] = (uint8_t*)malloc(OPENPETE_BUGREC_CARD_BYTES))) return -1;
        a->card_loaded[p] = openpete_memcard_image_copy(p, a->card[p], OPENPETE_BUGREC_CARD_BYTES);
        if (openpete_modsave_sidecar_bytes(p, &a->modsave[p], &a->modsave_len[p]) < 0) return -1;
    }
    return 0;
}

int openpete_bugrec_capture(openpete_bugrec_anchor_t* a, const CPUState* cpu) OP_REQUIRES(sim) {
    a->valid = 0;
    const uint64_t t0 = SDL_GetTicksNS();
    if (openpete_state_image_capture(&a->state, &a->state_len, &a->state_cap,
                                     cpu, openpete_frame) != 0 ||
        anchor_cards(a) != 0)
        return -1;
    a->log_idx = input_log_count();
    a->frame = openpete_frame;
    a->vbl = SDL_GetAtomicInt(&openpete_vbl);
    a->feed_key = openpete_feed_key_now();
    a->consume = openpete_consume;
    a->consume_write = openpete_consume_write;
    a->site = openpete_yield_site;
    int32_t x, y, z, yaw, level = -1;
    if (game_read_player_pose((CPUState*)cpu, &x, &y, &z, &yaw, &level) < 0) level = -1;
    a->level = level;
    a->trace_iter = host_trace_iter_index();
    for (int k = 0; k < BUGREC_TAINT_N; k++)
        a->taint_at[k] = atomic_load_explicit(&s_taint[k], memory_order_relaxed);
    a->capture_us = (SDL_GetTicksNS() - t0) / 1000u;
    a->valid = 1;
    return 0;
}

/** @brief Capture an anchor into the oldest slot (an empty one first) and rotate the retention.
 * @param cpu The guest register file.
 * @return 0 on success, -1 when the capture failed (the slot is left invalid). */
static int anchor_take(const CPUState* cpu) OP_REQUIRES(sim) {
    int s = -1;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS && s < 0; i++)
        if (!s_br.slot[i].valid) s = i;
    if (s < 0) s = oldest_slot();
    openpete_bugrec_anchor_t* a = &s_br.slot[s];
    if (openpete_bugrec_capture(a, cpu) != 0) {
        s_br.dropped[DROP_ALLOC]++;
        return -1;
    }
    a->seq = ++s_br.seq;
    if (s_br.capture_n < CAPTURE_SAMPLES) s_br.capture_us[s_br.capture_n++] = (uint32_t)a->capture_us;
    if (a->capture_us > s_br.capture_max_us) s_br.capture_max_us = a->capture_us;
    s_br.taken++;
    retention_apply();
    return 0;
}

/** @brief Report whether an anchor is due: none is held, the cadence elapsed, or a harness feed key was reached. */
static int anchor_due(void) {
    const unsigned key = openpete_feed_key_now();
    while (s_br.extra_next < s_br.extra_n && key >= s_br.extra_at[s_br.extra_next]) {
        s_br.due_extra = 1;
        s_br.extra_next++;
    }
    return s_br.due_extra || newest_slot() < 0 || openpete_frame >= s_br.next_due;
}

/** @brief Drop the oldest anchor once it is older than the retention bound, and with no anchor held trim the rows to the current point. */
static void retention_bound(void) OP_REQUIRES(sim) {
    const int o = oldest_slot();
    if (o < 0) {
        if (s_br.rows_n) retention_apply();
        return;
    }
    if (openpete_frame - s_br.slot[o].frame <= OPENPETE_BUGREC_RETAIN_MAX_VBLS) return;
    anchor_drop(&s_br.slot[o], DROP_AGE);
    retention_apply();
}

void openpete_bugrec_step(const CPUState* cpu, int rewound) OP_REQUIRES(sim) {
    retention_bound();
    if (anchor_due()) {
        const int why = admission_refusal(rewound);
        if (why >= 0) {
            s_br.deferred_yields[why]++;
            if (s_br.deferring != why) s_br.deferred_episodes[why]++;
            s_br.deferring = why;
        } else if (anchor_take(cpu) == 0) {
            s_br.deferring = -1;
            s_br.due_extra = 0;
            s_br.next_due = openpete_frame + OPENPETE_BUGREC_CADENCE_VBLS;
        }
    }
    if (s_br.dump_at >= 0 && !rewound && (int)openpete_feed_key_now() >= s_br.dump_at) {
        s_br.dump_at = -1;
        (void)openpete_bugrec_dump(s_br.dump_dir);
    }
}

/** @brief Grow the hash ring to hold one more row.
 * @return The new row's storage, or NULL when allocation failed. */
static uint64_t* rows_append(void) {
    const size_t stride = 4u + openpete_state_region_count;
    if (stride != s_br.stride) { s_br.rows_n = 0; s_br.stride = stride; }
    if (s_br.rows_n == s_br.rows_cap) {
        const size_t cap = s_br.rows_cap ? s_br.rows_cap * 2u : 2048u;
        uint64_t* g = (uint64_t*)realloc(s_br.rows, cap * stride * sizeof *g);
        if (!g) return NULL;
        s_br.rows = g;
        s_br.rows_cap = cap;
    }
    return s_br.rows + s_br.rows_n++ * stride;
}

uint64_t openpete_bugrec_hash_tick(unsigned frame, int vbl, const uint64_t** per_out) OP_REQUIRES(sim) {
    static uint64_t fallback[4u + 256u];
    uint64_t* row = rows_append();
    if (!row || openpete_state_region_count > 256u) row = fallback;
    const uint64_t t0 = SDL_GetTicksNS();
    row[0] = frame;
    row[1] = (uint64_t)(uint32_t)vbl;
    row[2] = openpete_feed_key_now();
    row[3] = gameplay_hash_compute_ex(row + 4);
    const uint64_t dt = SDL_GetTicksNS() - t0;
    s_br.hash_ns_total += dt;
    if (dt > s_br.hash_ns_max) s_br.hash_ns_max = dt;
    s_br.hash_ticks++;
    if (row != fallback) s_br.rows_total++;
    *per_out = row + 4;
    return row[3];
}

void openpete_bugrec_rewound(unsigned frame) OP_REQUIRES(sim) {
    openpete_bugrec_taint(BUGREC_TAINT_REWIND);
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++)
        if (s_br.slot[i].valid && s_br.slot[i].frame > frame) anchor_drop(&s_br.slot[i], DROP_REWIND);
    while (s_br.rows_n && s_br.rows[(s_br.rows_n - 1) * s_br.stride] > frame) s_br.rows_n--;
    input_log_lock();
    bk2_notes_truncate(input_log_count());
    input_log_unlock();
    retention_apply();
    if (newest_slot() < 0) s_br.next_due = frame;
}

void openpete_bugrec_taint(openpete_bugrec_taint_t kind) {
    if ((unsigned)kind < BUGREC_TAINT_N) atomic_fetch_add_explicit(&s_taint[kind], 1u, memory_order_relaxed);
}

void openpete_bugrec_taint_since(const openpete_bugrec_anchor_t* a, uint64_t out[BUGREC_TAINT_N]) {
    for (int k = 0; k < BUGREC_TAINT_N; k++)
        out[k] = atomic_load_explicit(&s_taint[k], memory_order_relaxed) - (a ? a->taint_at[k] : 0u);
}

const char* openpete_bugrec_taint_name(openpete_bugrec_taint_t kind) {
    return (unsigned)kind < BUGREC_TAINT_N ? k_taint_names[kind] : "?";
}

const openpete_bugrec_anchor_t* openpete_bugrec_anchor(int i) {
    const int n = newest_slot(), o = oldest_slot();
    if (i == 0) return n >= 0 ? &s_br.slot[n] : NULL;
    if (i == 1 && o >= 0 && o != n) return &s_br.slot[o];
    return NULL;
}

const openpete_bugrec_anchor_t* openpete_bugrec_pick(void) {
    const openpete_bugrec_anchor_t* newest = openpete_bugrec_anchor(0);
    const openpete_bugrec_anchor_t* older = openpete_bugrec_anchor(1);
    if (newest && openpete_frame - newest->frame >= OPENPETE_BUGREC_CADENCE_VBLS) return newest;
    return older ? older : newest;
}

int openpete_bugrec_segment_bk2(const openpete_bugrec_anchor_t* a, void** zip, size_t* len,
                                size_t* rows) {
    if (!a || !a->valid) return -1;
    input_log_lock();
    const int rc = bk2_record_window_zip(a->log_idx, a->consume, zip, len, rows);
    input_log_unlock();
    return rc;
}

const uint64_t* openpete_bugrec_hash_rows(unsigned from_frame, size_t* count) {
    size_t i = 0;
    while (i < s_br.rows_n && s_br.rows[i * s_br.stride] < from_frame) i++;
    *count = s_br.rows_n - i;
    return *count ? s_br.rows + i * s_br.stride : NULL;
}

size_t openpete_bugrec_hash_stride(void) { return s_br.stride; }

size_t openpete_bugrec_hash_csv_rows(FILE* fp, const uint64_t* rows, size_t n, size_t stride) {
    gameplay_hash_log_emit_header(fp);
    for (size_t i = 0; i < n; i++, rows += stride)
        gameplay_hash_log_emit(fp, (unsigned)rows[0], (int)(uint32_t)rows[1], rows[3], rows + 4);
    return n;
}

size_t openpete_bugrec_hash_csv(FILE* fp, unsigned from_frame) {
    size_t n = 0;
    const uint64_t* r = openpete_bugrec_hash_rows(from_frame, &n);
    return openpete_bugrec_hash_csv_rows(fp, r, n, s_br.stride);
}

/** @brief Copy n bytes into a buffer the caller frees, keeping NULL for an empty source.
 * @return 0, or -1 when allocation failed. */
static int dup_bytes(uint8_t** dst, const uint8_t* src, size_t n) {
    *dst = NULL;
    if (!src || !n) return 0;
    if (!(*dst = (uint8_t*)malloc(n))) return -1;
    memcpy(*dst, src, n);
    return 0;
}

int openpete_bugrec_anchor_copy(openpete_bugrec_anchor_t* dst, const openpete_bugrec_anchor_t* src) {
    *dst = *src;
    dst->state = NULL;
    dst->state_cap = 0;
    for (int p = 0; p < 2; p++) dst->card[p] = dst->modsave[p] = NULL;
    int rc = dup_bytes(&dst->state, src->state, src->state_len);
    dst->state_cap = dst->state ? src->state_len : 0;
    for (int p = 0; p < 2; p++) {
        rc |= dup_bytes(&dst->card[p], src->card_loaded[p] ? src->card[p] : NULL,
                        OPENPETE_BUGREC_CARD_BYTES);
        rc |= dup_bytes(&dst->modsave[p], src->modsave[p], src->modsave_len[p]);
    }
    if (rc) openpete_bugrec_anchor_release(dst);
    return rc ? -1 : 0;
}

void openpete_bugrec_anchor_release(openpete_bugrec_anchor_t* a) {
    free(a->state);
    for (int p = 0; p < 2; p++) { free(a->card[p]); free(a->modsave[p]); }
    memset(a, 0, sizeof *a);
}

size_t openpete_bugrec_hash_rows_copy(unsigned from_frame, uint64_t** out) OP_REQUIRES(sim) {
    size_t n = 0;
    const uint64_t* r = openpete_bugrec_hash_rows(from_frame, &n);
    *out = NULL;
    if (!n) return 0;
    if (dup_bytes((uint8_t**)out, (const uint8_t*)r, n * s_br.stride * sizeof *r) != 0) return 0;
    return n;
}

/** @brief Create a directory when it does not exist.
 * @param dir The path.
 * @return 0 when it exists afterwards. */
static int dir_make(const char* dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return (st.st_mode & S_IFMT) == S_IFDIR ? 0 : -1;
#ifdef _WIN32
    return _mkdir(dir);
#else
    return mkdir(dir, 0755);
#endif
}

/** @brief Write a buffer to "<dir>/<name>".
 * @return 0 on success, -1 on failure (logged). */
static int dump_file(const char* dir, const char* name, const void* buf, size_t n) {
    char path[1536];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    const int ok = f && fwrite(buf, 1, n, f) == n;
    if (f && fclose(f) != 0) return -1;
    if (!ok) OPL_ERROR_F("bugrec", "dump: writing %s failed: %s", path, strerror(errno));
    return ok ? 0 : -1;
}

/** @brief Write one anchor's metadata as key=value lines.
 * @param fp The stream.
 * @param a The anchor. */
static void dump_meta(FILE* fp, const openpete_bugrec_anchor_t* a) {
    uint64_t t[BUGREC_TAINT_N];
    openpete_bugrec_taint_since(a, t);
    fprintf(fp, "seq=%llu\nframe=%u\nvbl=%d\nfeed_key=%u\nlog_idx=%d\nconsume=%u\n"
            "consume_write=%u\npark_ra=0x%08X\npark_sp=0x%08X\nlevel=%d\ncapture_us=%llu\n"
            "state_bytes=%zu\ntrace_iter=%u\n",
            (unsigned long long)a->seq, a->frame, a->vbl, a->feed_key, a->log_idx, a->consume,
            a->consume_write, a->site.ra, a->site.sp, a->level,
            (unsigned long long)a->capture_us, a->state_len, a->trace_iter);
    for (int p = 0; p < 2; p++)
        fprintf(fp, "card%d_held=%d\ncard%d_loaded=%d\ncard%d_path=%s\ncard%d_modsave_bytes=%zu\n",
                p + 1, a->card_held[p], p + 1, a->card_loaded[p], p + 1, a->card_path[p], p + 1,
                a->modsave_len[p]);
    for (int k = 0; k < BUGREC_TAINT_N; k++)
        fprintf(fp, "taint_%s=%llu\n", k_taint_names[k], (unsigned long long)t[k]);
}

/** @brief Write one anchor's files: state, cards, `.modsave`, metadata and segment bk2.
 * @param dir The dump directory.
 * @param i 0 for the newest anchor, 1 for the older.
 * @param a The anchor.
 * @return 0 when every file was written. */
static int dump_anchor(const char* dir, int i, const openpete_bugrec_anchor_t* a) {
    char name[96];
    int rc = 0;
    snprintf(name, sizeof name, "anchor-%d.state", i);
    rc |= dump_file(dir, name, a->state, a->state_len);
    for (int p = 0; p < 2; p++) {
        if (a->card_loaded[p]) {
            snprintf(name, sizeof name, "anchor-%d.card%d.mcr", i, p + 1);
            rc |= dump_file(dir, name, a->card[p], OPENPETE_BUGREC_CARD_BYTES);
        }
        if (a->modsave[p]) {
            snprintf(name, sizeof name, "anchor-%d.card%d.mcr.modsave", i, p + 1);
            rc |= dump_file(dir, name, a->modsave[p], a->modsave_len[p]);
        }
    }
    void* zip = NULL;
    size_t zlen = 0, rows = 0;
    if (openpete_bugrec_segment_bk2(a, &zip, &zlen, &rows) == 0) {
        snprintf(name, sizeof name, "anchor-%d.seg.bk2", i);
        rc |= dump_file(dir, name, zip, zlen);
        free(zip);
    }
    char path[1536];
    snprintf(path, sizeof path, "%s/anchor-%d.txt", dir, i);
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    dump_meta(fp, a);
    fprintf(fp, "segment_rows=%zu\n", rows);
    fclose(fp);
    return rc;
}

/** @brief Write the recorder's counters as key=value lines.
 * @param fp The stream. */
static void dump_counters(FILE* fp) {
    const openpete_bugrec_anchor_t* pick = openpete_bugrec_pick();
    fprintf(fp, "frame=%u\nvbl=%d\nfeed_key=%u\nlog_count=%d\nconsume=%u\nconsume_write=%u\n"
            "pick=%d\nanchors_taken=%llu\n",
            openpete_frame, SDL_GetAtomicInt(&openpete_vbl), openpete_feed_key_now(),
            input_log_count(), openpete_consume, openpete_consume_write,
            pick ? (pick == openpete_bugrec_anchor(0) ? 0 : 1) : -1, s_br.taken);
    for (int d = 0; d < DEF_N; d++)
        fprintf(fp, "deferred_%s=%llu\n", k_def_names[d], s_br.deferred_yields[d]);
    for (int d = 0; d < DROP_N; d++)
        fprintf(fp, "dropped_%s=%llu\n", k_drop_names[d], s_br.dropped[d]);
}

int openpete_bugrec_dump(const char* dir) OP_REQUIRES(sim) {
    if (!dir || dir_make(dir) != 0) {
        OPL_ERROR_F("bugrec", "dump: cannot create %s", dir ? dir : "(null)");
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++) {
        const openpete_bugrec_anchor_t* a = openpete_bugrec_anchor(i);
        if (a) rc |= dump_anchor(dir, i, a);
    }
    char path[1536];
    snprintf(path, sizeof path, "%s/hashes.csv", dir);
    FILE* fp = fopen(path, "w");
    size_t nrows = 0;
    if (fp) { nrows = openpete_bugrec_hash_csv(fp, 0u); fclose(fp); } else rc = -1;
    const uint8_t* trace = NULL;
    const size_t ntrace = host_trace_ring_records(&trace);
    if (ntrace) rc |= dump_file(dir, "hosttrace.bin", trace, ntrace * HT_REC_BYTES);
    void* pace = NULL;
    size_t pace_len = 0, npace = 0;
    if (openpete_pace_ring_image(OP_PACE_RING_WINDOW_NS, &pace, &pace_len, &npace) == 0) {
        rc |= dump_file(dir, "pace.bin", pace, pace_len);
        free(pace);
    }
    size_t ninput = 0;
    int ndev = 0;
    snprintf(path, sizeof path, "%s/input-events.csv", dir);
    fp = fopen(path, "w");
    if (fp) { ninput = openpete_bugrec_input_csv(fp); fclose(fp); } else rc = -1;
    snprintf(path, sizeof path, "%s/devices.txt", dir);
    fp = fopen(path, "w");
    if (fp) { ndev = openpete_bugrec_devices_write(fp); fclose(fp); } else rc = -1;
    snprintf(path, sizeof path, "%s/recorder.txt", dir);
    fp = fopen(path, "w");
    if (fp) {
        dump_counters(fp);
        fprintf(fp, "hash_rows=%zu\ntrace_records=%zu\npace_records=%zu\ninput_events=%zu\n"
                "devices=%d\n", nrows, ntrace, npace, ninput, ndev);
        fclose(fp);
    } else {
        rc = -1;
    }
    OPL_INFO_F("bugrec", "dump: wrote %s at frame %u (feed key %u): %d anchor(s), %zu hash row(s), "
               "%zu trace record(s), %zu pace record(s), %zu input event(s), %d device(s)%s", dir,
               openpete_frame, openpete_feed_key_now(),
               (openpete_bugrec_anchor(0) != NULL) + (openpete_bugrec_anchor(1) != NULL), nrows,
               ntrace, npace, ninput, ndev, rc ? " - a file failed" : "");
    return rc ? -1 : 0;
}

/** @brief Compare two u32 values for qsort. */
static int cmp_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

/** @brief Return the bytes the held anchors occupy (buffers allocated). */
static size_t anchor_bytes(void) {
    size_t b = 0;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++) {
        const openpete_bugrec_anchor_t* a = &s_br.slot[i];
        b += a->state_cap + a->modsave_len[0] + a->modsave_len[1];
        for (int p = 0; p < 2; p++) b += a->card[p] ? OPENPETE_BUGREC_CARD_BYTES : 0u;
    }
    return b;
}

void openpete_bugrec_report(void) OP_REQUIRES(sim) {
    uint32_t sorted[CAPTURE_SAMPLES];
    const unsigned n = s_br.capture_n;
    memcpy(sorted, s_br.capture_us, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, cmp_u32);
    size_t notes_bytes = 0, trace_bytes = 0;
    const size_t notes = bk2_notes_held(&notes_bytes);
    const size_t trace = host_trace_ring_held(&trace_bytes);
    const size_t hash_bytes = s_br.rows_cap * s_br.stride * sizeof *s_br.rows;
    int held = 0;
    for (int i = 0; i < OPENPETE_BUGREC_SLOTS; i++) held += s_br.slot[i].valid;
    OPL_INFO_F("bugrec", "anchors taken=%llu held=%d; deferred final yields: boot=%llu speculative=%llu "
               "site=%llu cd-queue=%llu card-pending=%llu rewound=%llu (episodes: site=%llu cd-queue=%llu "
               "card-pending=%llu); dropped: discontinuity=%llu rewind=%llu age=%llu alloc=%llu; "
               "capture us p50=%u p99=%u max=%llu (n=%u)",
               s_br.taken, held,
               s_br.deferred_yields[DEF_BOOT], s_br.deferred_yields[DEF_SPECULATIVE],
               s_br.deferred_yields[DEF_SITE], s_br.deferred_yields[DEF_CD],
               s_br.deferred_yields[DEF_CARD], s_br.deferred_yields[DEF_REWOUND],
               s_br.deferred_episodes[DEF_SITE], s_br.deferred_episodes[DEF_CD],
               s_br.deferred_episodes[DEF_CARD],
               s_br.dropped[DROP_DISCONTINUITY], s_br.dropped[DROP_REWIND], s_br.dropped[DROP_AGE],
               s_br.dropped[DROP_ALLOC], n ? sorted[n / 2] : 0u, n ? sorted[(n * 99u) / 100u] : 0u,
               (unsigned long long)s_br.capture_max_us, n);
    OPL_INFO_F("bugrec", "held: anchors %zu B, hash rows %zu (%zu B, %llu written, %llu trimmed), "
               "bk2 row notes %zu (%zu B), host-trace records %zu (%zu B); hashing per tick: "
               "mean %.1f us max %.1f us over %llu tick(s)",
               anchor_bytes(), s_br.rows_n, hash_bytes, s_br.rows_total, s_br.rows_trimmed, notes,
               notes_bytes, trace, trace_bytes,
               s_br.hash_ticks ? (double)s_br.hash_ns_total / (double)s_br.hash_ticks / 1000.0 : 0.0,
               (double)s_br.hash_ns_max / 1000.0, (unsigned long long)s_br.hash_ticks);
    char line[256];
    int at = 0;
    for (int k = 0; k < BUGREC_TAINT_N; k++)
        at += snprintf(line + at, sizeof line - (size_t)at, "%s%s=%llu", k ? " " : "", k_taint_names[k],
                       (unsigned long long)atomic_load_explicit(&s_taint[k], memory_order_relaxed));
    OPL_INFO_F("bugrec", "taint totals: %s", line);
    openpete_pace_ring_counts_t pc;
    openpete_pace_ring_counts(&pc);
    openpete_bugrec_input_counts_t ic;
    openpete_bugrec_input_counts(&ic);
    OPL_INFO_F("bugrec", "pace ring: appended %llu held %llu (%.1f s) overwritten %llu "
               "inside-window %llu, %zu B; input ring: events %llu merged %llu entries %llu "
               "held %llu overwritten %llu, %zu B",
               (unsigned long long)pc.appended, (unsigned long long)pc.held, (double)pc.span_ns / 1e9,
               (unsigned long long)pc.overwritten, (unsigned long long)pc.young_lost, pc.bytes,
               (unsigned long long)ic.events, (unsigned long long)ic.merged,
               (unsigned long long)ic.appended, (unsigned long long)ic.held,
               (unsigned long long)ic.overwritten, ic.bytes);
}
