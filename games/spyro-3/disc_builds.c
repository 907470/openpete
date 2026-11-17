/* games/spyro-3/disc_builds.c - the builds of Spyro: Year of the Dragon a
 * disc can hold, for the disc build classifier (openpete_disc_build.h). A row with no
 * serial ends the table.
 *
 * Sources:
 *  SCUS-94467 1.0, disc: SCUS_944.67 read off the image whose .bin SHA-256
 *    is a313c9b1... (Redump "Spyro - Year of the Dragon (USA) (v1.0)").
 *    bin_sha1: the Redump .bin SHA-1 the User docs give
 *    (docs/src/user/games/spyro-3.md).
 *  SCUS-94467 1.0, decomp build: spyro-3/sha256sum.txt's PSX.EXE pin, the
 *    recompiler manifest's input. It differs from the disc's executable by
 *    the decomp build's two-byte patch of its executable check (exe_check,
 *    asm/stdutil.s). Both are supported; the disc's row carries the .bin
 *    SHA-1 the messages name.
 *  SCUS-94467 1.1: SCUS_944.67 (380928 bytes) read off an image whose .bin
 *    SHA-1 is 81eae747fbd9b2ff75ae5b3e24287bb25fcb516a, Redump's "Spyro -
 *    Year of the Dragon (USA) (v1.1)". Same serial and boot file name as
 *    1.0, so only the executable's hash tells the two apart.
 *  SCES-02835 (PAL): PSX Datacenter's entry. The game had no NTSC-J
 *    release. */
#include "openpete_disc_build.h"

const openpete_disc_build_row_t spyro3_disc_builds[] = {
    { "SCUS-94467", "cb819ee78c556d403779309859cb08a7111331f624759bc1bc380946261bb26e", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.0",
      "9df489e2420988494691c9193fe74dec4b1dbfcd" },
    { "SCUS-94467", "e5406997dccc7300c8198498c20b9d6c4c0a547813be1010446b6c4e5d50e39f", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.0", NULL },
    { "SCUS-94467", "006363e485559dcd9a203fe71fe4267cb254d476ca59f04ee841048831ad969e", 0,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.1", NULL },
    { "SCES-02835", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-3", "Spyro: Year of the Dragon", NULL, NULL },
    { NULL, NULL, 0, OPENPETE_DISC_REGION_UNKNOWN, NULL, NULL, NULL, NULL },
};
