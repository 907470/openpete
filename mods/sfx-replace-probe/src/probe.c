/* sfx-replace-probe - registers a two-second synthesized buzz over one
 * game sound through the sfx_replace verb. The body is built here as
 * raw SPU-ADPCM: filter 0, shift 0, so every 4-bit nibble decodes to
 * nibble << 12 - a coarse square wave is exactly representable, and no
 * disc-derived audio is involved. Log-only otherwise: sim-neutral. */

#include <openpete_mod_api.h>
#include <string.h>

#define BLOCKS_MAX 7875               /* 5.0 s at 28 samples per 16-byte block */

static unsigned char g_body[BLOCKS_MAX * 16];

/* ~551 Hz square wave: 40 samples at +7, 40 at -7 (nibble 0x9). Two
 * samples per byte, low nibble first. Returns the body length. */
static unsigned build_body(double seconds) {
    int blocks = (int)(seconds * 44100.0 / 28.0 + 0.5);
    if (blocks < 1) blocks = 1;
    if (blocks > BLOCKS_MAX) blocks = BLOCKS_MAX;
    unsigned sample = 0;
    for (int b = 0; b < blocks; b++) {
        unsigned char* blk = g_body + b * 16;
        blk[0] = 0x00;                            /* shift 0, filter 0 */
        blk[1] = (b == blocks - 1) ? 0x01 : 0x00; /* END flag on the last block */
        for (int i = 0; i < 14; i++) {
            unsigned char lo = ((sample / 40u) & 1u) ? 0x9 : 0x7; sample++;
            unsigned char hi = ((sample / 40u) & 1u) ? 0x9 : 0x7; sample++;
            blk[2 + i] = (unsigned char)(lo | (hi << 4));
        }
    }
    return (unsigned)blocks * 16u;
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    char name[64];
    api->config_str(self, "probe.sound", "pickupDing", name, sizeof name);
    double seconds = api->config_float(self, "probe.seconds", 2.0);
    unsigned len = build_body(seconds);
    int rc = api->sfx_replace(self, name, g_body, len, 0);
    if (rc != 0) {
        api->log(self, OP_MOD_LOG_ERROR, "sfx-replace-probe: sfx_replace(\"%s\") refused", name);
        return -1;
    }
    api->log(self, OP_MOD_LOG_INFO, "sfx-replace-probe: registered %u-byte body over \"%s\" (%.2f s)",
             len, name, seconds);
    api->ui_status(self, "replacing \"%s\" with a %.1f s buzz", name, seconds);
    return 0;
}
