/* core/include/openpete_disc_build.h - which build of a game a disc image holds.
 *
 * Each game's recompiled code is built from one executable: the boot
 * executable of the NTSC-U release its decomp matches. A disc is classified
 * by the release serial of its SYSTEM.CNF BOOT line, the boot executable that
 * line names, and that executable's SHA-256:
 *
 *   SUPPORTED   the hash is a supported row's, under any boot name.
 *   RECOGNISED  a known build OpenPete does not run: a row naming the hash,
 *               or the serial alone for a row with no hash (read with or
 *               without the executable).
 *   UNKNOWN     a PlayStation disc no row names: another game, or a
 *               supported serial whose executable has another hash (another
 *               revision whose hash is not on file, or a modified image).
 *   UNREADABLE  no SYSTEM.CNF, no BOOT line, or no boot executable where
 *               the serial alone names no row.
 *
 * The warning names what a player is told; the region comes from the
 * serial's third letter (U: NTSC-U, E: PAL, P, A or K: NTSC-J), so a PAL or
 * NTSC-J disc is recognised without a hash on file.
 *
 * The rows are game data: each game supplies its own (its runtime
 * descriptor's disc_builds), and the process entry registers every linked
 * game's with openpete_disc_build_add_rows. This module holds only the
 * mechanics, the region rule and the messages. */
#ifndef OPENPETE_DISC_BUILD_H
#define OPENPETE_DISC_BUILD_H

#include "openpete_thread_roles.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENPETE_DISC_BUILD_SUPPORTED = 0,
    OPENPETE_DISC_BUILD_RECOGNISED,
    OPENPETE_DISC_BUILD_UNKNOWN,
    OPENPETE_DISC_BUILD_UNREADABLE
} openpete_disc_build_class_t;

typedef enum {
    OPENPETE_DISC_REGION_UNKNOWN = 0,
    OPENPETE_DISC_REGION_NTSC_U,
    OPENPETE_DISC_REGION_PAL,
    OPENPETE_DISC_REGION_NTSC_J
} openpete_disc_region_t;

typedef enum {
    OPENPETE_DISC_WARN_NONE = 0,   /* a supported build */
    OPENPETE_DISC_WARN_PAL,        /* a PAL release */
    OPENPETE_DISC_WARN_NTSC_J,     /* an NTSC-J release */
    OPENPETE_DISC_WARN_REVISION,   /* a known later revision of a supported serial */
    OPENPETE_DISC_WARN_MODIFIED,   /* a supported serial with an executable no row names */
    OPENPETE_DISC_WARN_OTHER,      /* any other PlayStation disc */
    OPENPETE_DISC_WARN_UNREADABLE  /* not a readable PlayStation game disc */
} openpete_disc_warning_t;

/* One known build of a game: a release serial, and the SHA-256 of the boot
 * executable its SYSTEM.CNF names (NULL: not on file, so the row matches by
 * serial alone, which only a serial no supported row shares may do). */
typedef struct openpete_disc_build_row {
    const char*            serial;
    const char*            exe_sha256;  /* 64 lowercase hex digits, or NULL */
    int                    supported;   /* 1: the build the game's code is built from */
    openpete_disc_region_t region;
    const char*            game;        /* the registered game this is a release of */
    const char*            title;       /* the release's name */
    const char*            revision;    /* "1.0", or NULL */
    const char*            bin_sha1;    /* a supported row's Redump .bin SHA-1, which a player can check, or NULL */
} openpete_disc_build_row_t;

/** @brief Register a game's known builds; a table already registered is ignored.
 *
 * Rows are matched in registration order; at most 64 rows are kept. Called
 * before openpete_run seals the group.
 * @param rows The rows, ended by a row with no serial, which outlive the process's use of them; NULL registers none. */
void openpete_disc_build_add_rows(const openpete_disc_build_row_t* rows);

/* The registered rows are written by openpete_disc_build_add_rows before
 * openpete_run seals them, and read-only after. */
OP_SEAL_DECLARE(disc_builds);

typedef struct {
    openpete_disc_build_class_t cls;
    openpete_disc_warning_t     warning;
    openpete_disc_region_t      region;
    char        serial[32];      /* "SCUS-94467", "" when unread */
    char        boot_path[64];   /* "SCUS_944.67", "" when unread */
    char        exe_sha256[65];  /* the boot executable's SHA-256, "" when unread */
    const char* game;            /* the registered game this disc is a release of, or NULL */
    const char* title;           /* the release's name from the table, or NULL */
    const char* revision;        /* the matched row's revision label, or NULL */
} openpete_disc_build_t;

/** @brief Classify a disc from its SYSTEM.CNF text and its boot executable's bytes.
 *
 * @param cnf_text The NUL-terminated SYSTEM.CNF text, or NULL when the disc has none.
 * @param exe The boot executable's bytes, or NULL when it could not be read.
 * @param exe_len Their count.
 * @param out Receives the classification. */
void openpete_disc_build_classify(const char* cnf_text, const uint8_t* exe, size_t exe_len,
                                  openpete_disc_build_t* out);

/** @brief Classify the disc image behind a disc path.
 *
 * Reads SYSTEM.CNF and the boot executable it names (a few hundred KiB)
 * and hashes the executable: about 1 ms on a local disk. Nothing is
 * cached; a caller that lists many images records the result per image.
 * @param disc_path A .cue or the image itself.
 * @param out Receives the classification; UNREADABLE when the image cannot be opened.
 * @return 0 when the image was opened, else -1. */
int openpete_disc_build_identify(const char* disc_path, openpete_disc_build_t* out);

/** @brief Write the player's message for a classification: what the disc is, which build OpenPete runs, and the outcome.
 *
 * Empty for a supported build.
 * @param b The classification.
 * @param refused Nonzero ends the message with "OpenPete cannot start it."; zero with the expected crash.
 * @param out Receives the text, cut to fit.
 * @param cap Capacity of @p out. */
void openpete_disc_build_message(const openpete_disc_build_t* b, int refused, char* out, size_t cap);

/** @brief The label of a class ("supported", "recognised", "unknown", "unreadable").
 * @param c The class.
 * @return The label, never NULL. */
const char* openpete_disc_build_class_name(openpete_disc_build_class_t c);

/** @brief The label of a region ("NTSC-U", "PAL", "NTSC-J", "unknown").
 * @param r The region.
 * @return The label, never NULL. */
const char* openpete_disc_region_name(openpete_disc_region_t r);

/** @brief Check the disc a session boots, before the game's code is attached.
 *
 * Classifies the disc and logs the result; the player is told at ingest
 * (the launcher's library), so a boot shows nothing on screen. A supported
 * disc, or an image that cannot be opened (left to the executable load,
 * which names the files it tried), returns at once. A disc that does not
 * hold @p game_exe_filename cannot boot this game (a PAL or NTSC-J release,
 * a rebuilt image booting another name): the player's message is reported
 * through openpete_fatal_message and the check refuses. Any other build,
 * including one with no usable SYSTEM.CNF, logs the message as a warning
 * with the OP-E-DISC-BUILD reference and the boot proceeds, unless
 * OPENPETE_DISC_BUILD_REFUSE is set to a value other than "0", which
 * refuses every build that is not supported.
 * @param disc_path The disc the session boots, or NULL (no check).
 * @param game_exe_filename The ISO 9660 path of the executable this game boots ("SCUS_942.28").
 * @return 0 to proceed, -1 to refuse. */
int openpete_disc_build_boot_check(const char* disc_path, const char* game_exe_filename);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DISC_BUILD_H */
