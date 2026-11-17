/* openpete_error_ids.h - the stable ids (OP-E-*) of player-facing errors.
 *
 * Each row of OPENPETE_ERROR_IDS is one error a player can meet: a startup
 * failure, a disc, game-code, display or graphics-device failure, a hang
 * or crash, or a memory-card file problem. The id is printed in the log
 * line and the message box of that error, followed by the URL of its row
 * on the User troubleshooting page, so a player goes from the message to
 * the fix. Ids are never renumbered or reused: a removed error keeps its
 * row here and on the page. A new error adds a row here and one to the
 * page's "By error id" table, whose anchor is the id in lower case;
 * scripts/fences/surface_coverage.py refuses the two tables disagreeing. */
#ifndef OPENPETE_ERROR_IDS_H
#define OPENPETE_ERROR_IDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The troubleshooting page each id's URL points into. */
#define OPENPETE_TROUBLESHOOTING_URL \
    "https://openpete.com/docs/user/help/troubleshooting.html"

/* X(enum suffix, id, anchor on the troubleshooting page). */
#define OPENPETE_ERROR_IDS(X)                                   \
    X(CONFIG,        "OP-E-CONFIG",        "op-e-config")        \
    X(LIBRARY,       "OP-E-LIBRARY",       "op-e-library")       \
    X(INGEST,        "OP-E-INGEST",        "op-e-ingest")        \
    X(WRONG_DISC,    "OP-E-WRONG-DISC",    "op-e-wrong-disc")    \
    X(NO_DISC,       "OP-E-NO-DISC",       "op-e-no-disc")       \
    X(DISC_READ,     "OP-E-DISC-READ",     "op-e-disc-read")     \
    X(MODULE,        "OP-E-MODULE",        "op-e-module")        \
    X(NO_COMPILER,   "OP-E-NO-COMPILER",   "op-e-no-compiler")   \
    X(MODULE_DIR,    "OP-E-MODULE-DIR",    "op-e-module-dir")    \
    X(PACKAGE,       "OP-E-PACKAGE",       "op-e-package")       \
    X(DISPLAY,       "OP-E-DISPLAY",       "op-e-display")       \
    X(VULKAN,        "OP-E-VULKAN",        "op-e-vulkan")        \
    X(GPU_LOST,      "OP-E-GPU-LOST",      "op-e-gpu-lost")      \
    X(HANG,          "OP-E-HANG",          "op-e-hang")          \
    X(CRASH,         "OP-E-CRASH",         "op-e-crash")         \
    X(CARD_OPEN,     "OP-E-CARD-OPEN",     "op-e-card-open")     \
    X(CARD_BAD,      "OP-E-CARD-BAD",      "op-e-card-bad")      \
    X(CARD_READONLY, "OP-E-CARD-READONLY", "op-e-card-readonly") \
    X(CARD_WRITE,    "OP-E-CARD-WRITE",    "op-e-card-write")    \
    X(DISC_BUILD,    "OP-E-DISC-BUILD",    "op-e-disc-build")

#define OPENPETE_ERROR_ENUM_ROW(name, id, anchor) OP_E_##name,
typedef enum {
    OPENPETE_ERROR_IDS(OPENPETE_ERROR_ENUM_ROW)
    OP_E_COUNT
} openpete_error_t;
#undef OPENPETE_ERROR_ENUM_ROW

/** @brief The error's id, such as "OP-E-CONFIG".
 *
 * @param e The error.
 * @return A static string; "OP-E-UNKNOWN" for a value outside the table. */
const char* openpete_error_id(openpete_error_t e);

/** @brief The URL of the error's row on the troubleshooting page.
 *
 * @param e The error.
 * @return A static string; the page's "By error id" section for a value
 *         outside the table. */
const char* openpete_error_url(openpete_error_t e);

/** @brief The error's reference for a message: "[<id>: <troubleshooting URL>#<anchor>]".
 *
 * A static string literal, so it is safe to print from a signal handler.
 * @param e The error.
 * @return A static string; the unknown id's reference for a value outside
 *         the table. */
const char* openpete_error_ref(openpete_error_t e);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_ERROR_IDS_H */
