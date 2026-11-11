/* core/record/bugreport_internal.h - the bug-report bundle's member list,
 * redactor and finding scanner, shared by the press (host_bugreport.c), the
 * worker (bugreport_write.c) and the redactor (bugreport_redact.c). */
#ifndef OPENPETE_BUGREPORT_INTERNAL_H
#define OPENPETE_BUGREPORT_INTERNAL_H

#include "openpete_audio_feed_report.h"
#include "openpete_bugrec.h"
#include "openpete_bugthumbs.h"

#include <SDL3/SDL_atomic.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the worker treats a member before it is hashed and zipped. */
typedef enum {
    BRM_TEXT = 0,  /* redacted by path token, user name and host name */
    BRM_STATE,     /* a state file: card path fields rewritten, guest sections scanned */
    BRM_GUEST,     /* guest bytes (card image, `.modsave`): scanned, never rewritten */
    BRM_BINARY,    /* host binary records holding no path field: scanned */
    BRM_MODULE,    /* a compiled module: shipped unredacted, scanned */
    BRM_BK2,       /* a bk2 zip: its text entries redacted, the archive rebuilt */
    BRM_AUTHOR,    /* a file of a mod's directory as its author wrote it: scanned */
    BRM_PNG        /* an image: scanned, stored uncompressed */
} br_kind_t;

typedef struct {
    char*     name;  /* path inside the zip */
    uint8_t*  data;
    size_t    len;
    br_kind_t kind;
} br_member_t;

typedef struct {
    br_member_t* v;
    size_t       n, cap;
} br_members_t;

/* What a finding matched. */
typedef enum { BRF_HOME = 0, BRF_USER, BRF_HOST, BRF_N } br_needle_kind_t;

typedef struct {
    char             member[160];
    char             section[24];  /* the state section, or "" */
    uint64_t         offset;       /* byte offset in the member */
    br_needle_kind_t what;
} br_finding_t;


/* One loaded mod as the press saw it. */
typedef struct {
    char   id[64];
    char   version[32];
    char   dir[1024];   /* its directory under the mods root */
    char   obj[1024];   /* the compiled module the game loaded, or "" */
    int    enabled;
    int    mutative;
    char*  cfg;         /* config.toml as shipped: the declared non-secret rows' effective values */
    char** declared;    /* every [[config]] key, secret ones included */
    int    ndeclared;
} br_mod_t;

/* An answer another thread owes the worker. */
enum { BR_PENDING = 0, BR_DONE = 1, BR_ABSENT = -1 };

/* One bundle from the press to the zip. The press fills everything up to
 * the async answers on the sim thread; the worker owns the job after it
 * starts, except the answers, which other threads publish through the
 * atomics; the sim thread frees it after the join. */
typedef struct br_job {
    char     stem[96];          /* bug-<game>-<yyyymmdd>-<hhmmss> */
    char     out_dir[1024];     /* <data>/bugreports */
    char     stage[1200];       /* <out_dir>/.stage-<stem> */
    char     zip_path[1400];
    char     shot_path[1300];   /* <stage>/screen.png */
    char     game[32];
    char     disc_path[1024];
    char     log_path[1024];    /* the current log file, or "" */
    char**   argv;
    int      argc;
    /* A fatal exit's bundle: the exit's reason ("hang", "cdsync-spin") and
     * the thread that wrote it ("sim", "other"); NULL for a press. */
    const char* fatal;
    const char* exit_thread;
    /* The press, on the sim thread. */
    openpete_bugrec_anchor_t press;   /* the press snapshot and its card images */
    openpete_bugrec_anchor_t anchor;  /* a copy of the recorder's replay anchor */
    int      have_anchor;
    void*    segment;
    size_t   segment_len, segment_rows;
    uint64_t* hash_rows;
    size_t   hash_n, hash_stride;
    uint8_t* trace;
    size_t   trace_n;
    void*    pace;
    size_t   pace_len, pace_n;
    void*    pace_coarse;       /* the coarse session ring's image (pace-coarse.bin) */
    size_t   pace_coarse_len, pace_coarse_rows;
    openpete_bugrec_input_t* input;
    size_t   input_n;
    uint8_t* hang;              /* the dispatch rings in hang-trace.bin's layout */
    size_t   hang_len;
    openpete_bugthumb_t* thumbs;  /* the thumbnail ring, oldest first */
    size_t   thumbs_n;
    uint8_t* subtick;           /* the sub-tick present ring's image (subtick.bin) */
    size_t   subtick_len, subtick_n;
    int64_t  press_present_ns;  /* the present clock at the press */
    openpete_audio_feed_snap_t* audio;  /* the audio feed's counters and event ring (audio-feed.txt) */
    int      note_wanted;       /* the press asks the tester for a note */
    char*    note;              /* the saved note, or NULL */
    char     note_state[32];    /* saved, skipped, off, no text input, not shown, ... */
    char*    reports;           /* the sim thread's subsystem reports */
    size_t   reports_len;
    char*    summary;
    char*    env;               /* every set OPENPETE_* variable */
    char*    knobs;             /* knobs.txt: the knob registry's values and live changes */
    char     pace_env[2048];    /* the pacing environment block at the press */
    br_mod_t* mods;
    int      nmods;
    uint64_t taint[BUGREC_TAINT_N];  /* since the anchor */
    uint64_t press_us;          /* sim-thread time of the press */
    uint64_t t_press_ns;
    /* Answers of the other threads. */
    SDL_AtomicInt shot;         /* BR_*: screen.png */
    SDL_AtomicInt display;      /* BR_*: display_txt */
    SDL_AtomicInt main_facts;   /* BR_*: main_txt */
    char*    display_txt;
    size_t   display_len;
    char*    main_txt;
    size_t   main_len;
    uint64_t shot_t0_ns;        /* when the present services took the screenshot request */
    uint64_t display_us;        /* sim-thread time of the display service, screenshot included */
    /* The worker's results. */
    SDL_AtomicInt noted;        /* 1 once the note prompt ended and the write began */
    int      saving_shown;      /* sim thread: the SAVING toast was posted */
    SDL_AtomicInt finished;     /* 1 once the worker returned */
    int      rc;
    size_t   members;
    uint64_t zip_bytes;
    uint64_t replacements;
    size_t   findings;
    uint64_t write_ms;
    char     mods_line[256];    /* the enabled mods' ids, comma-separated */
} br_job_t;

/* Bounds on the other threads' answers, from the worker's start: the
 * pump's device facts, and the present services' image and display
 * reports (a present must run, which a canonical tick brings). */
#define BR_MAIN_WAIT_MS 500u
#define BR_SHOT_WAIT_MS 2000u
/* The note prompt: skipped when no frame draws it, or, with no keyboard
 * attached, no on-screen keyboard shows, within this bound. */
#define BR_NOTE_SHOW_MS 2000u

/** @brief Withdraw a job's main-thread request when the pump has not claimed it.
 *
 * @param j The job.
 * @return 1 when withdrawn (the pump will not touch the job), 0 when the
 *         pump holds it (its answer follows). */
int br_main_withdraw(br_job_t* j);

/** @brief Write a job's bundle: wait for the other threads' answers, collect the files, redact, scan, hash, write manifest.json and the zip.
 *
 * Runs on the worker thread; sets job->rc and the result fields.
 * @param job The job. */
void br_job_write(br_job_t* job);

/** @brief Free a job and everything it holds.
 * @param job The job; NULL is ignored. */
void br_job_free(br_job_t* job);

#define BR_PREFIX_MAX 24
#define BR_NAME_MAX 3

/* A path prefix whose occurrences at a path token's start are replaced. */
typedef struct {
    char*       form;     /* the prefix as it occurs in text */
    size_t      len;
    const char* repl;     /* "~", or the variable name for a base outside home */
} br_prefix_t;

typedef struct {
    br_prefix_t pre[BR_PREFIX_MAX];  /* longest first */
    int         npre;
    char        user[256];           /* "" when unknown */
    char        host[BR_NAME_MAX][256];
    int         nhost;
    int         fold_case;           /* Windows: names and prefixes match in any case */
    uint64_t    replacements;        /* tokens rewritten since init */
} br_redactor_t;

/** @brief Fill a redactor with the home directory in every form, the base directories outside it, the user name and the host names.
 *
 * Home: $HOME / %USERPROFILE% as given and resolved through links, with
 * either separator, %HOMEDRIVE%%HOMEPATH% and the 8.3 short name on
 * Windows. Bases replaced by their variable name when outside home:
 * $XDG_DATA_HOME, $XDG_CONFIG_HOME, $XDG_STATE_HOME, $XDG_CACHE_HOME,
 * %LOCALAPPDATA%, %APPDATA%. User name: $USER / %USERNAME%, else the home
 * directory's last component. Host names: the uname nodename,
 * %COMPUTERNAME% and $HOSTNAME.
 * @param r The redactor. */
void br_redactor_init(br_redactor_t* r);

/** @brief Release a redactor's prefix strings.
 * @param r The redactor. */
void br_redactor_free(br_redactor_t* r);

/** @brief Redact a text buffer by path token, returning a new heap buffer.
 *
 * A path token starts after a delimiter (whitespace, a quote, `=`, `,`,
 * `;`, `:`, brackets) or at the buffer's start. A token that begins with a
 * home form, followed by a separator, a delimiter or the end, has the form
 * replaced by `~`; a base outside home is replaced by its variable name.
 * Any other absolute path token (`/...`, `X:\...`, `\\...`) keeps only its
 * last component. The user name and the host names are replaced by
 * `<user>` and `<host>` only where they are a whole path component or the
 * whole value after a `key=`.
 * @param r The redactor; its replacement count grows.
 * @param in The text.
 * @param n Its length.
 * @param out_n Receives the result's length.
 * @return The redacted text, NUL-terminated; NULL on allocation failure. */
char* br_redact_text(br_redactor_t* r, const char* in, size_t n, size_t* out_n);

/** @brief Find the home forms, the user name and the host names in a byte range.
 *
 * Home forms match as substrings; a name matches only between two bytes
 * that are not letters, digits, `_` or `-`.
 * @param r The redactor.
 * @param d The bytes.
 * @param n Their count.
 * @param hit Called once per match with its offset and kind.
 * @param ud Passed to @p hit.
 * @return The match count. */
size_t br_scan(const br_redactor_t* r, const uint8_t* d, size_t n,
               void (*hit)(void* ud, size_t off, br_needle_kind_t what), void* ud);

/** @brief Append a member, taking ownership of @p data.
 * @param m The list.
 * @param name The member path (copied).
 * @param data Heap bytes, freed with the list.
 * @param len Their length.
 * @param kind The member's class.
 * @return 0, or -1 when allocation fails (data freed). */
int br_members_add(br_members_t* m, const char* name, uint8_t* data, size_t len, br_kind_t kind);

/** @brief Append a member holding a copy of a buffer.
 * @param m The list.
 * @param name The member path.
 * @param data The bytes, copied.
 * @param len Their length.
 * @param kind The member's class.
 * @return 0, or -1 when allocation fails. */
int br_members_add_copy(br_members_t* m, const char* name, const void* data, size_t len,
                        br_kind_t kind);

/** @brief Free every member and the list.
 * @param m The list. */
void br_members_free(br_members_t* m);

/** @brief Find the first occurrence of a byte string.
 * @param hay The bytes searched.
 * @param n Their count.
 * @param needle The bytes sought.
 * @param k Their count.
 * @return The first occurrence, or NULL. */
const void* br_memmem(const void* hay, size_t n, const void* needle, size_t k);

/** @brief Hash a disc image's bytes as the library does (disc_image_read_bytes over the whole image).
 * @param path The disc image (a cue or an image file).
 * @param out Receives 64 hex digits and a NUL, "" on failure.
 * @return 0, or -1 when the image cannot be read. */
int br_disc_sha(const char* path, char out[65]);

/** @brief Read a whole file into a heap buffer.
 * @param path The file.
 * @param out Receives the bytes (NUL-terminated); free with free().
 * @param len Receives the length.
 * @return 0, or -1 when the file cannot be read. */
int br_read_file(const char* path, uint8_t** out, size_t* len);

/** @brief Read the bundle's knobs.txt at the anchor load and apply the live values the session had there.
 *
 * Each `render` row that changed after boot is set through this build's
 * registry setter to its value at the anchor: the old value of its first
 * change after the anchor, else its value at the press; a row whose value
 * there equals its boot value is left, since the replay boots on the
 * tester's configuration. The changes after the anchor wait for
 * br_knobs_replay_tick. Rows of the other classes that changed are printed
 * as not applied with the reason. A bundle without the member is printed
 * and changes nothing. OPENPETE_BUGREPORT_REPLAY_KNOBS=0 applies nothing
 * and prints each row it would have applied.
 * @param scratch The replay's scratch directory.
 * @param anchor_frame The anchor's tick.
 * @return 1 when changes after the anchor were dropped from the bundle's
 *   ring (the replay is labelled knobs-partial), else 0. */
int br_knobs_replay_load(const char* scratch, unsigned anchor_frame);

/** @brief Apply each recorded change of a `render` row whose tick is reached, once, in order.
 * @param frame The committed tick (openpete_frame). */
void br_knobs_replay_tick(unsigned frame);

/** @brief Print the knob summary line: rows changed after boot, applied at the anchor load and at their tick, not applied; nothing before the load. */
void br_knobs_replay_report(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGREPORT_INTERNAL_H */
