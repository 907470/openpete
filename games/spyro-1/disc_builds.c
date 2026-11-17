/* games/spyro-1/disc_builds.c - the builds of Spyro the Dragon a disc can
 * hold, for the disc build classifier (openpete_disc_build.h). A row with no
 * serial ends the table.
 *
 * Sources:
 *  SCUS-94228 1.0: the decomp's build/PSX.EXE pin (spyro-1/sha256sum.txt,
 *    readme "PSX.EXE SHA-256"), equal to SCUS_942.28 read off the NTSC-U
 *    image. Original, Greatest Hits and Collectors' Edition pressings share
 *    the image. bin_sha1: the Redump .bin SHA-1 the User docs give
 *    (docs/src/user/getting-started/dumping-your-disc.md).
 *  SCES-01438 (PAL); SCPS-10085, SCPS-10083 (disc 1 of the limited
 *    edition), SCPS-45395 (NTSC-J): PSX Datacenter's entries. */
#include "openpete_disc_build.h"

const openpete_disc_build_row_t spyro1_disc_builds[] = {
    { "SCUS-94228", "a533d75cab8afaae6107ec35a02a9a5fe979a92c7c955f9cf1ee50f693a1b998", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-1", "Spyro the Dragon", "1.0",
      "cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f" },
    { "SCES-01438", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { "SCPS-10085", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { "SCPS-10083", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", "Limited Edition", NULL },
    { "SCPS-45395", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { NULL, NULL, 0, OPENPETE_DISC_REGION_UNKNOWN, NULL, NULL, NULL, NULL },
};
