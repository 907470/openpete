/* core/include/openpete_bugrec_subtick.h - the bug-report recorder's
 * sub-tick present ring: per composed present its kind, alpha, the tick
 * stamps the present lerps between and the scene ring slot it was
 * published into, and per drawn object record the pairing state the
 * sub-tick interpolation read and the position it was drawn at.
 *
 * A present opens with openpete_bugrec_subtick_begin at the host site that
 * runs the scene re-extract for a displayed present (an eager ring scene,
 * an inline intermediate, the inline canonical re-extract), the feeders
 * append one entry per record they draw (openpete_bugrec_subtick_obj),
 * and openpete_bugrec_subtick_end closes it with the alpha the re-extract
 * resolved and the slot it went to. Appends outside an open present (a
 * screenshot leg, the identity audit) are dropped. The ring keeps the
 * newest OPENPETE_BUGREC_SUB_PRESENTS presents and the newest
 * OPENPETE_BUGREC_SUB_OBJS entries; a present whose entries were
 * overwritten keeps its header and reads as truncated.
 *
 * OPENPETE_BUGREPORT_SUBTICK=0 turns the ring off: no storage is
 * allocated and every call returns at once.
 *
 * Every call runs on the sim thread, which also takes the press snapshot,
 * so the ring holds no lock. Nothing stores into PSX RAM; the entries hold
 * guest addresses, positions and host clock stamps, no path or user text.
 */
#ifndef OPENPETE_BUGREC_SUBTICK_H
#define OPENPETE_BUGREC_SUBTICK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Presents held: 2.13 s at 480 Hz. */
#define OPENPETE_BUGREC_SUB_PRESENTS 1024u
/* Object entries held: 32 drawn records per present over the present ring
 * (2.6 MiB). A scene drawing more shortens the window the entries cover;
 * the presents before it keep their headers. */
#define OPENPETE_BUGREC_SUB_OBJS (32u * OPENPETE_BUGREC_SUB_PRESENTS)

/* The member's layout version (subtick.bin). */
#define OPENPETE_BUGREC_SUB_VERSION 1u

/* How a present was produced. */
enum {
    BUGREC_SUB_EAGER = 0,   /* an eager scene published into a ring slot */
    BUGREC_SUB_INLINE,      /* re-extracted and rendered inline on the host loop */
    BUGREC_SUB_REPEAT       /* inline, the world not live: the last scene shown again */
};

/* The pairing verdict of one drawn record. */
enum {
    BUGREC_SUB_PAIRED = 0,  /* lerps between its prev and curr states */
    BUGREC_SUB_NEWBORN,     /* no prev state (spawn, or no snapshot at all) */
    BUGREC_SUB_DYING,       /* evicted at the newest tick, replayed at its last state */
    BUGREC_SUB_HELD         /* held at its previous tick (jump, held cut, previous queue) */
};

/* Entry flags. */
enum {
    BUGREC_SUB_F_SPEC     = 1u << 0,  /* drawn by the specular pass */
    BUGREC_SUB_F_GHOST    = 1u << 1,  /* drawn by the late switch's despawned-record pass */
    BUGREC_SUB_F_REFPREV  = 1u << 2,  /* the previous tick's queue was replayed */
    BUGREC_SUB_F_NOOBJ    = 1u << 3,  /* the interpolation holds no snapshot for the key */
    BUGREC_SUB_F_WORLD    = 1u << 4,  /* world[] holds the drawn world position */
    BUGREC_SUB_F_UNSHOWN  = 1u << 5,  /* the prev state was not shown on its tick */
    BUGREC_SUB_F_KF_SHIFT = 6u        /* bits 6-7: keyframe verdict, 0 snap, 1 lerp, 2 hold */
};

/* One present (96 B). */
typedef struct {
    uint64_t seq;            /* presents begun since startup, from 1 */
    int64_t  present_ns;     /* the present clock the re-extract read */
    int64_t  target_ns;      /* the scene's display instant (eager), else 0 */
    int64_t  tick_prev_ns;   /* the interpolation ring's prev and curr tick stamps, 0 when unpaired */
    int64_t  tick_curr_ns;
    uint32_t tick_prev;      /* their openpete_frame values */
    uint32_t tick_curr;
    uint32_t feed_key;
    uint32_t vbl;
    float    alpha;          /* the alpha the re-extract resolved */
    uint8_t  kind;           /* OP_PACE_CANONICAL / OP_PACE_INTERMEDIATE */
    uint8_t  paired;
    uint8_t  path;           /* BUGREC_SUB_EAGER / _INLINE / _REPEAT */
    uint8_t  late;           /* the present showed tick N-1's record set */
    int32_t  slot;           /* the scene ring slot, -1 inline */
    uint32_t scene_id;       /* the slot's scene id, 0 inline */
    uint64_t obj_first;      /* index of its first entry among all entries appended */
    uint32_t obj_n;          /* entries appended while it was open */
    uint32_t stamp;          /* the slot's terrain-cache build stamp, 0 inline */
    int64_t  period_ns;      /* the tick period the alphas divide by */
} openpete_bugrec_sub_present_t;

/* One drawn record (80 B). */
typedef struct {
    uint32_t key;            /* the interpolation's pairing key */
    uint32_t moby;           /* the record's object address */
    uint16_t cls;            /* the object's class */
    uint8_t  verdict;        /* BUGREC_SUB_PAIRED ... */
    uint8_t  flags;          /* BUGREC_SUB_F_* */
    int32_t  prev_pos[3];    /* the snapshot pair's prev and curr positions */
    int32_t  curr_pos[3];
    uint8_t  prev_rot[3];    /* their three angle bytes */
    uint8_t  curr_rot[3];
    uint16_t nverts;         /* projected vertices drawn */
    float    world[3];       /* the world position the record was drawn at */
    float    sx, sy, vz;     /* the centroid of its projected vertices: screen x, y and view z */
    int32_t  prev_dt_us;     /* the pair's prev and curr roll stamps less present_ns, in us */
    int32_t  curr_dt_us;
    float    kf_alpha;       /* the keyframe crossfade alpha the decode read */
} openpete_bugrec_sub_obj_t;

/** @brief Open a present's record in the ring.
 *
 * Reads the present clock, the interpolation ring's two newest stamps, the
 * feed key and the VBL counter. A present still open is closed first as it
 * stands.
 * @param kind OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE.
 * @param path BUGREC_SUB_EAGER, BUGREC_SUB_INLINE or BUGREC_SUB_REPEAT. */
void openpete_bugrec_subtick_begin(int kind, int path);

/** @brief Report whether a present is open, so a feeder skips building an entry outside one.
 * @return 1 while open, else 0. */
int openpete_bugrec_subtick_open(void);

/** @brief Return the open present's clock stamp, which the entries' roll stamps are taken against.
 * @return present_ns of the open present, 0 when none is open. */
int64_t openpete_bugrec_subtick_present_ns(void);

/** @brief Append one drawn record to the open present; dropped when none is open.
 * @param e The entry, copied. */
void openpete_bugrec_subtick_obj(const openpete_bugrec_sub_obj_t* e);

/** @brief Mark the open present as one that showed tick N-1's record set.
 * @param late Nonzero for a late present. */
void openpete_bugrec_subtick_late(int late);

/** @brief Close the open present with what the re-extract resolved and where the scene went.
 * @param alpha The resolved alpha.
 * @param paired The resolved pairing (0 unpaired).
 * @param slot The scene ring slot, -1 for an inline present or a refused publish.
 * @param scene_id The slot's scene id, 0 inline.
 * @param stamp The slot's build stamp, 0 inline.
 * @param target_ns The scene's display instant, 0 inline. */
void openpete_bugrec_subtick_end(double alpha, int paired, int slot, uint32_t scene_id,
                                 uint32_t stamp, int64_t target_ns);

/** @brief Serialize the held presents and their entries, oldest first, into one heap buffer (the subtick.bin layout).
 *
 * Layout, little-endian: "OPSUBTK1", u32 version, u32 present size,
 * u32 entry size, u32 presents, u32 entries, u32 presents dropped by the
 * ring, u64 the absolute index of the first entry held, u64 entries
 * appended outside an open present, then the presents, then the entries.
 * A present's obj_first stays absolute: its entries sit at obj_first less
 * the first held index, and one whose entries were overwritten has
 * obj_first below that index, which the reader reports as truncated.
 * @param out Receives the buffer, NULL when nothing is held; free with free().
 * @param len Receives its length.
 * @return The presents serialized. */
size_t openpete_bugrec_subtick_image(uint8_t** out, size_t* len);

/** @brief Fill the ring's counters for the manifest and the exit report.
 * @param presents Receives the presents begun.
 * @param objs Receives the entries appended.
 * @param bytes Receives the ring's storage size. */
void openpete_bugrec_subtick_counts(uint64_t* presents, uint64_t* objs, size_t* bytes);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGREC_SUBTICK_H */
