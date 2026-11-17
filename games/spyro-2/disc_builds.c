/* games/spyro-2/disc_builds.c - the builds of Spyro 2: Ripto's Rage! a disc
 * can hold, for the disc build classifier (openpete_disc_build.h). A row with no
 * serial ends the table.
 *
 * Sources:
 *  SCUS-94425 1.0: spyro-2/sha256sum.txt's PSX.EXE pin, equal to
 *    SCUS_944.25 read off the image whose .bin SHA-256 is b3fd9fdb...
 *    bin_sha1: the Redump .bin SHA-1 the User docs give
 *    (docs/src/user/games/spyro-2.md).
 *  SCES-02104 (PAL, "Gateway to Glimmer"), SCPS-10128 (NTSC-J): PSX
 *    Datacenter's entries. */
#include "openpete_disc_build.h"

const openpete_disc_build_row_t spyro2_disc_builds[] = {
    { "SCUS-94425", "7b54002789ab379e0d3a6b49d7b50078b9c9c2fed589f41ff9878ebb68fe0bc1", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-2", "Spyro 2: Ripto's Rage!", "1.0",
      "b3a28f8c5ec02dfbd1f9bd47adbe1d75186a5d3f" },
    { "SCES-02104", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-2", "Spyro 2: Gateway to Glimmer", NULL, NULL },
    { "SCPS-10128", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-2", "Spyro x Sparx: Tondemo Tours", NULL, NULL },
    { NULL, NULL, 0, OPENPETE_DISC_REGION_UNKNOWN, NULL, NULL, NULL, NULL },
};
