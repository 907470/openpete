// games/spyro-1/mod_wad_overlay.h — level-segment asset overlay (rung 9).
// See mod_wad_overlay.c. Consumers: main.cpp (configure), psyq_thunks
// (setloc tracking + apply at the CdRead chokepoint).

#ifndef SPYRO1_MOD_WAD_OVERLAY_H
#define SPYRO1_MOD_WAD_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void spyro1_mod_wad_configure(const char* disc_path);
void mod_wad_setloc(uint32_t sector);
/* Serve override bytes into the just-read guest buffer and advance the
 * tracked head position. host_buf = host pointer to the read dest;
 * sectors = data sectors read (2048 bytes each). */
void mod_wad_apply(uint8_t* host_buf, int sectors);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_MOD_WAD_OVERLAY_H */
