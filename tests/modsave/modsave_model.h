/* modsave_model.h - the per-mod save-record rules as one pure C state machine.
 *
 * No game, no I/O, no allocation: a state is a plain struct, copied by
 * value, and every event is a pure function of the state and the event's
 * parameters. The domain is finite (MSM_MAX_* below; the run picks n <= max
 * through msm_config_t) and every value is abstract: a record content is
 * one of MSM_C_*, a mod's arena state one of 0..2, a lineage a (root,
 * counter) pair; every fresh lineage is a new root (OS entropy minted at
 * consumption), so the counter stays 0.
 *
 * The rules are the normative "Block and events" table of the save-record
 * design: the key's block (key, generation, legacy copy,
 * held identity per mod, pending load, pending write entries, and each
 * mod's arena value) rolls back with savestates, rewind and runahead; the
 * sidecar tables, the card images, the speculative queue and the arming
 * flag are host state.
 *
 * Effects are appended to a caller buffer as ';'-separated tokens:
 *   R<m>:<STATUS>:<schema>:<payload|->   read callback
 *   W<m>                                 write callback called
 *   S<slot>.<idx>:<op>                   sidecar mutation committed
 *   Q<slot>.<idx>:<op>                   sidecar mutation queued (led tick)
 *   L:<kind>                             log line (committed ticks only)
 */
#ifndef MODSAVE_MODEL_H
#define MODSAVE_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define MSM_MAX_MODS   2
#define MSM_MAX_SLOTS  2
#define MSM_MAX_IDX    2
#define MSM_MAX_CAP    4
#define MSM_MAX_QUEUE  12

/** Record contents: empty, two contents with a zero legacy span, one whose
 *  legacy span carries mod 0's magic, and a torn record failing the checksum. */
enum { MSM_C_E = 0, MSM_C_X, MSM_C_Y, MSM_C_P, MSM_C_T, MSM_C_COUNT };

/** Delivered status, in openpete_mod_save_status_t order. */
enum { MSM_OK = 0, MSM_ABSENT, MSM_GAP, MSM_STALE, MSM_NEWER, MSM_LEGACY_PAD };

/** Key state. */
enum { MSM_KEY_UNKNOWN = 0, MSM_KEY_CLEARED, MSM_KEY_SET };

/** Event kinds; msm_event_name() gives the spelling the search uses. */
enum {
    MSM_EV_T = 0,        /* tick with no game action */
    MSM_EV_LOAD,         /* a: slot, b: index - title load of that record */
    MSM_EV_LOAD_STALE,   /* a, b - SaveLoad with a selection naming no candidate */
    MSM_EV_RESET,        /* new-playthrough reset */
    MSM_EV_WRITE,        /* a slot, b index, c content, d mask of mods whose write returns -1 */
    MSM_EV_WRITE_FAIL,   /* a, b, c - the write's card flush fails */
    MSM_EV_ISSUE,        /* a, b, c, d - asynchronous write issued; none while the slot's entry is pending */
    MSM_EV_COMPLETE,     /* a slot, b outcome (0 ok, 1 fail, 2 torn) */
    MSM_EV_FILE_CREATE,  /* a slot - one write of empty records over every index */
    MSM_EV_FORMAT,       /* a slot */
    MSM_EV_PLAY,         /* a mod, b arena value */
    MSM_EV_TOGGLE,       /* a mod */
    MSM_EV_ENTER,        /* a mod - deferred first entry */
    MSM_EV_SCHEMA,       /* a mod - hot reload with the schema flipped 1 <-> 2 */
    MSM_EV_RESTART,      /* a loaded mask, b entered mask - new process, no card read yet */
    MSM_EV_MOUNT,        /* a slot - first card access: the image and the sidecar are read */
    MSM_EV_MOUNT_ADOPT,  /* a slot - first card access under --adopt-saves */
    MSM_EV_SS_TAKE,
    MSM_EV_SS_LOAD,
    MSM_EV_SS_LOAD_OLD,  /* the savestate's blob predates the block */
    MSM_EV_RA_BEGIN,
    MSM_EV_RA_ROLLBACK,
    MSM_EV_RA_ADOPT,
    MSM_EV_RA_ADOPT_FAIL,
    MSM_EV_RA_PARTIAL,
    MSM_EV_RW_MARK,
    MSM_EV_REWIND,
    MSM_EV_EDIT,         /* a slot, b index, c content - card file edited outside */
    MSM_EV_SC_CORRUPT,   /* a slot - sidecar file made unparseable */
    MSM_EV_SC_FAULT,     /* the next sidecar write fails */
    MSM_EV_HANDOFF,      /* a loaded mask, b entered mask */
    MSM_EV_MODSET,       /* a slot, b tag mask + 1 (0 = no .modset) */
    MSM_EV_COUNT
};

typedef struct { uint8_t kind, a, b, c, d; } msm_event_t;

typedef struct { uint8_t n_mods, n_slots, n_idx, cap; } msm_config_t;

typedef struct { uint32_t root, n; } msm_lin_t;  /* root 0 = no lineage */

typedef struct { uint8_t present, schema, gap, payload; } msm_rec_t;

typedef struct {
    uint8_t   content;
    uint8_t   tag;                 /* mask of entered mods at the write */
    msm_lin_t lin;
    msm_rec_t rec[MSM_MAX_MODS];
} msm_snap_t;

typedef struct { uint8_t n; msm_snap_t s[MSM_MAX_CAP]; } msm_snaps_t;

/** A sidecar: absent, present, or (on disk only) unparseable. */
enum { MSM_SC_NONE = 0, MSM_SC_PRESENT, MSM_SC_CORRUPT };
typedef struct { uint8_t state; msm_snaps_t idx[MSM_MAX_IDX]; } msm_table_t;

typedef struct {
    uint8_t   used, captured, fresh, slot, idx, content, tag;
    uint8_t   en;                  /* mods enabled at issue: they take the held identity at completion */
    uint32_t  gen;
    msm_lin_t lin;                 /* root 0 for an empty-record entry */
    msm_rec_t rec[MSM_MAX_MODS];
} msm_entry_t;

typedef struct {
    uint8_t   key, key_slot, key_idx, key_content;
    msm_lin_t key_lin;
    uint32_t  gen;
    uint8_t   legacy;
    uint8_t   held[MSM_MAX_MODS];  /* 1 when held_lin/held_gen are valid */
    msm_lin_t held_lin[MSM_MAX_MODS];
    uint32_t  held_gen[MSM_MAX_MODS];
    uint8_t   pload, pload_legacy, pload_content, pload_chosen; /* chosen: slot*16+idx, 0xFF none */
    uint8_t   pload_cands;         /* bit slot*MSM_MAX_IDX+idx */
    msm_entry_t entry[MSM_MAX_SLOTS];
    uint8_t   arena[MSM_MAX_MODS];
} msm_block_t;

typedef struct { uint8_t kind, slot, idx; msm_snap_t snap; } msm_qop_t;
enum { MSM_Q_SNAP = 1, MSM_Q_DROP, MSM_Q_FORMAT };

typedef struct {
    msm_config_t cfg;
    msm_block_t  blk, spec, ss, rw;
    uint8_t      have_spec, have_ss, have_rw;
    uint8_t      loaded, entered, enabled;   /* per-mod bit masks */
    uint8_t      schema[MSM_MAX_MODS];
    uint8_t      armed;
    uint8_t      mounted[MSM_MAX_SLOTS];
    uint8_t      card[MSM_MAX_SLOTS][MSM_MAX_IDX];
    uint8_t      disk[MSM_MAX_SLOTS][MSM_MAX_IDX];
    uint8_t      dirty[MSM_MAX_SLOTS];
    msm_table_t  sc_mem[MSM_MAX_SLOTS], sc_disk[MSM_MAX_SLOTS];
    uint8_t      sc_fault;
    uint8_t      modset[MSM_MAX_SLOTS];       /* 0 none, else tag mask + 1 */
    uint8_t      nq;
    msm_qop_t    queue[MSM_MAX_QUEUE];
    uint32_t     next_root;
} msm_state_t;

/** @brief Calibration hook (0 in every real run): 1 swaps GAP and OK, 2 skips the held-identity update at a write completion, 3 keeps the key at a reset, 4 reads each mod's status from the other mod's record. */
extern int msm_seed_fault;

/** @brief Boot state: every mod in @p loaded_mask loaded, entered and enabled at schema 1; blank cards, no sidecars. */
void msm_init(msm_state_t* s, const msm_config_t* cfg, uint8_t loaded_mask);

/** @brief 1 when the event's precondition holds in @p s. */
int msm_enabled(const msm_state_t* s, const msm_event_t* ev);

/** @brief Apply @p ev to @p s in place, appending its effects to @p fx (NUL-terminated, at most @p cap bytes); returns 0, or -1 when @p fx overflowed. */
int msm_apply(msm_state_t* s, const msm_event_t* ev, char* fx, size_t cap);

/** @brief 1 when @p ev is a tick event (one that runs the delivery arm first). */
int msm_is_tick(uint8_t kind);

/** @brief Write the canonical form of @p s into @p out; returns its length, or 0 when @p cap is too small. */
size_t msm_canon(const msm_state_t* s, uint8_t* out, size_t cap);

/** @brief The spelling of an event kind, or NULL past MSM_EV_COUNT. */
const char* msm_event_name(int kind);

/** @brief sizeof(msm_state_t), for callers that hold states as opaque bytes. */
size_t msm_state_size(void);

#endif
