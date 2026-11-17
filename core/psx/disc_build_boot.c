/* core/psx/disc_build_boot.c - the disc-build check of a session's boot
 * (contract in openpete_disc_build.h). */

#include "openpete_disc_build.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_error_ids.h"
#include "openpete_fatal.h"
#include "openpete_log.h"
#include "openpete_osd.h"

#include <stdlib.h>
#include <string.h>

/** @brief Whether the disc holds the executable this game boots.
 *
 * @param disc_path The disc.
 * @param exe The ISO 9660 path of the executable.
 * @return 1 when the file is on the disc, else 0. */
static int disc_has_file(const char* disc_path, const char* exe) {
    disc_image_t* img = disc_image_open(disc_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_SERIAL);
    if (!img) return 0;
    disc_image_entry_t e;
    const int found = disc_image_find(img, exe, &e) == 0 && !e.is_dir;
    disc_image_close(img);
    return found;
}

int openpete_disc_build_boot_check(const char* disc_path, const char* game_exe_filename) {
    if (!disc_path || !*disc_path) return 0;
    openpete_disc_build_t b;
    if (openpete_disc_build_identify(disc_path, &b) != 0) return 0;
    OPL_INFO_F("disc", "build of %s: %s (serial %s, region %s, boot %s, exe sha256 %s%s%s)",
               disc_path, openpete_disc_build_class_name(b.cls), b.serial[0] ? b.serial : "-",
               openpete_disc_region_name(b.region), b.boot_path[0] ? b.boot_path : "-",
               b.exe_sha256[0] ? b.exe_sha256 : "-", b.revision ? ", version " : "",
               b.revision ? b.revision : "");
    if (b.cls == OPENPETE_DISC_BUILD_SUPPORTED) return 0;

    const char* refuse_env = getenv("OPENPETE_DISC_BUILD_REFUSE");
    const int cannot_boot = game_exe_filename && !disc_has_file(disc_path, game_exe_filename);
    const int refuse = cannot_boot || (refuse_env && *refuse_env && strcmp(refuse_env, "0") != 0);
    char msg[1024];
    openpete_disc_build_message(&b, refuse, msg, sizeof msg);
    if (refuse) {
        openpete_fatal_message(OP_E_DISC_BUILD, "Unsupported disc", msg);
        return -1;
    }
    OPL_WARN_F("disc", "%s %s", msg, openpete_error_ref(OP_E_DISC_BUILD));
    char toast[OPENPETE_OSD_TEXT_MAX];
    openpete_disc_build_toast(&b, toast, sizeof toast);
    openpete_osd_notify("host", "disc.build", OPENPETE_OSD_TIMEOUT_MAX_MS, "%s", toast);
    return 0;
}
