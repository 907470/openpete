/* core/include/openpete_disc_trace.h - one row per host read of the disc image.
 *
 * OPENPETE_DISC_TRACE=PATH writes a tab-separated row for every host read
 * of the disc image, whatever reads it: PsyCross's libcd and XA reads (its
 * read observer), the disc cache loader's chunk reads, and the host paths
 * that read through disc_image (EXE load, disc serial, recomp extract,
 * woah NSF, sfx census, WAD overlay, library ingest, mod read_sector).
 * Columns:
 *
 *   ns      op_now() at the read, monotonic nanoseconds
 *   vbl     openpete_vbl at the read, -1 before the runtime registers it
 *   tag     the read path (openpete_disc_trace_tag_name)
 *   lba     first 2352-byte sector the read touches
 *   count   sectors the read touches
 *   result  hit or miss (a read served by the disc cache), direct (a read
 *           of the file)
 *   offset  first byte offset in the image (lba * 2352 for a row recorded
 *           by sector)
 *   length  bytes from offset to the end of the read
 *
 * Contiguous reads on one thread with the same tag, result and VBL merge
 * into one row; cache-load rows never merge (one per loader read). A row is
 * written when its thread's next read does not continue it, so rows from
 * different threads are not in ns order. One writer, buffered, flushed at
 * exit. Unset, every entry point costs one branch on
 * openpete_disc_trace_state. */
#ifndef OPENPETE_DISC_TRACE_H
#define OPENPETE_DISC_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Read paths. ISO is a disc_image read issued with no tag outside any
 * tagged scope; MOD is a mod API read_sector; FONT is the host text
 * producer's read of a glyph model; TERRAIN is the terrain feeder's read of
 * a source level's terrain and texture sub-files. */
enum {
    OPENPETE_DISC_TAG_NONE = 0,
    OPENPETE_DISC_TAG_LIBCD,
    OPENPETE_DISC_TAG_XA,
    OPENPETE_DISC_TAG_EXE,
    OPENPETE_DISC_TAG_SERIAL,
    OPENPETE_DISC_TAG_EXTRACT,
    OPENPETE_DISC_TAG_WOAH,
    OPENPETE_DISC_TAG_SFX,
    OPENPETE_DISC_TAG_WAD,
    OPENPETE_DISC_TAG_INGEST,
    OPENPETE_DISC_TAG_CACHE_LOAD,
    OPENPETE_DISC_TAG_ISO,
    OPENPETE_DISC_TAG_MOD,
    OPENPETE_DISC_TAG_FONT,
    OPENPETE_DISC_TAG_TERRAIN,
    OPENPETE_DISC_TAG_COUNT
};

/* Read results. */
enum {
    OPENPETE_DISC_HIT = 0,
    OPENPETE_DISC_MISS,
    OPENPETE_DISC_DIRECT
};

/* -1 until OPENPETE_DISC_TRACE is read, then 1 when a trace file is open, else 0. */
extern int openpete_disc_trace_state;

/** @brief Resolve openpete_disc_trace_state from OPENPETE_DISC_TRACE on first use, opening the trace file.
 *
 * Thread-safe; the first caller reads the variable, opens the file with a
 * 1 MiB buffer, writes the header line and registers the exit flush. An
 * unopenable path logs a warning and leaves the trace off.
 * @return 1 when the trace is on. */
int openpete_disc_trace_init(void);

/** @brief Re-arm the first-use read when it found OPENPETE_DISC_TRACE unset and the variable is now set.
 *
 * The identification reads the disc's serial before the configuration is
 * materialized, which latches the trace off unless the real environment
 * set it; called after the materialization, this lets an openpete.toml
 * [env] value open the trace at the next read. A trace already open is
 * kept. */
void openpete_disc_trace_reload(void);

/** @brief Report whether the trace is on, reading OPENPETE_DISC_TRACE on the first call.
 *
 * @return 1 when rows are being written. */
static inline int openpete_disc_trace_enabled(void) {
    return openpete_disc_trace_state && openpete_disc_trace_init();
}

/** @brief Name a read path as written in the tag column.
 *
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return The column spelling, "?" for an unknown value. */
const char* openpete_disc_trace_tag_name(int tag);

/** @brief Install the source of the vbl column.
 *
 * @param fn Returns the current VBL count; NULL reports -1. */
void openpete_disc_trace_set_vbl_source(int (*fn)(void));

/** @brief Read the VBL count from the installed source.
 *
 * @return The current VBL count; -1 when no source is installed. */
int openpete_disc_trace_vbl(void);

/** @brief Record a read of image bytes [offset, offset + length).
 *
 * @param tag An OPENPETE_DISC_TAG_* value; OPENPETE_DISC_TAG_NONE takes the
 *        calling thread's scope tag, else OPENPETE_DISC_TAG_ISO.
 * @param offset First byte offset in the image.
 * @param length Bytes read.
 * @param result OPENPETE_DISC_HIT, _MISS or _DIRECT. */
void openpete_disc_trace_bytes_slow(int tag, uint64_t offset, uint64_t length, int result);

/** @brief Record a read of @p count whole sectors from @p lba (their user data or the raw sectors).
 *
 * @param tag As for openpete_disc_trace_bytes_slow.
 * @param lba First sector.
 * @param count Sectors read.
 * @param result OPENPETE_DISC_HIT, _MISS or _DIRECT. */
void openpete_disc_trace_sectors_slow(int tag, uint32_t lba, uint32_t count, int result);

/** @brief Set the calling thread's scope tag unless an outer scope already set one.
 *
 * Rows recorded with OPENPETE_DISC_TAG_NONE on this thread take the
 * outermost scope's tag, so a helper called inside a caller's scope
 * reports under the caller's path.
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return The token to pass to openpete_disc_trace_scope_end. */
int openpete_disc_trace_scope_begin_slow(int tag);

/** @brief Restore the calling thread's scope tag from a openpete_disc_trace_scope_begin token.
 *
 * @param token The value openpete_disc_trace_scope_begin returned. */
void openpete_disc_trace_scope_end_slow(int token);

/** @brief Write the pending row and flush the trace file. */
void openpete_disc_trace_flush(void);

/** @brief Record a byte-range read when the trace is on; see openpete_disc_trace_bytes_slow.
 *
 * @param tag Read path.
 * @param offset First byte offset.
 * @param length Bytes read.
 * @param result Read result. */
static inline void openpete_disc_trace_bytes(int tag, uint64_t offset, uint64_t length, int result) {
    if (openpete_disc_trace_state) openpete_disc_trace_bytes_slow(tag, offset, length, result);
}

/** @brief Record a sector-range read when the trace is on; see openpete_disc_trace_sectors_slow.
 *
 * @param tag Read path.
 * @param lba First sector.
 * @param count Sectors read.
 * @param result Read result. */
static inline void openpete_disc_trace_sectors(int tag, uint32_t lba, uint32_t count, int result) {
    if (openpete_disc_trace_state) openpete_disc_trace_sectors_slow(tag, lba, count, result);
}

/** @brief Open a scope tag when the trace is on; see openpete_disc_trace_scope_begin_slow.
 *
 * @param tag Read path.
 * @return The token for openpete_disc_trace_scope_end. */
static inline int openpete_disc_trace_scope_begin(int tag) {
    return openpete_disc_trace_state ? openpete_disc_trace_scope_begin_slow(tag) : -1;
}

/** @brief Close a scope tag opened by openpete_disc_trace_scope_begin.
 *
 * @param token The begin call's return value; -1 is a no-op. */
static inline void openpete_disc_trace_scope_end(int token) {
    if (token >= 0) openpete_disc_trace_scope_end_slow(token);
}

#ifdef __cplusplus
}

/** @brief Holds a disc trace scope tag for the lifetime of a block. */
struct OpenpeteDiscTraceScope {
    int token;
    /** @brief Open the scope; see openpete_disc_trace_scope_begin.
     *
     * @param tag Read path. */
    explicit OpenpeteDiscTraceScope(int tag) : token(openpete_disc_trace_scope_begin(tag)) {}
    /** @brief Close the scope. */
    ~OpenpeteDiscTraceScope() { openpete_disc_trace_scope_end(token); }
    OpenpeteDiscTraceScope(const OpenpeteDiscTraceScope&) = delete;
    OpenpeteDiscTraceScope& operator=(const OpenpeteDiscTraceScope&) = delete;
};
#endif
#endif
