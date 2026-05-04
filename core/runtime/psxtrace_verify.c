/* core/runtime/psxtrace_verify.c — consumer for the .psxtrace oracle.
 *
 * See core/include/openpete_psxtrace.h for the API + format reference. */

#include "openpete_psxtrace.h"
#include "openpete_log.h"
#include "openpete_psx.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PSXTRACE_FRMR_MAGIC     0x524D5246u  /* 'FRMR' */
#define PSXTRACE_FLAG_ZSTD      0x1u

static int read_u32_le(FILE* fp, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return -1;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] <<  8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

int openpete_psxtrace_open(openpete_psxtrace_t* t, const char* path) {
    memset(t, 0, sizeof(*t));
    t->path = path;
    t->fp = fopen(path, "rb");
    if (!t->fp) {
        OPL_ERROR_F("psxtrace", "open %s failed: %s", path, strerror(errno));
        return -1;
    }
    uint8_t magic[8];
    if (fread(magic, 1, 8, t->fp) != 8 ||
        memcmp(magic, "PSXTRACE", 8) != 0) {
        OPL_ERROR_F("psxtrace", "%s: bad magic", path);
        fclose(t->fp); t->fp = NULL; return -1;
    }
    uint32_t ram_size = 0, hint = 0, reserved = 0;
    if (read_u32_le(t->fp, &t->version)     ||
        read_u32_le(t->fp, &t->frame_count) ||
        read_u32_le(t->fp, &ram_size)       ||
        read_u32_le(t->fp, &t->flags)       ||
        read_u32_le(t->fp, &hint)           ||
        read_u32_le(t->fp, &reserved)) {
        OPL_ERROR_F("psxtrace", "%s: short header", path);
        fclose(t->fp); t->fp = NULL; return -1;
    }
    if (t->version != 1) {
        OPL_ERROR_F("psxtrace", "%s: unsupported version %u",
                    path, t->version);
        fclose(t->fp); t->fp = NULL; return -1;
    }
    if (ram_size != OPENPETE_PSX_RAM_SIZE) {
        OPL_ERROR_F("psxtrace", "%s: unexpected ram_size 0x%x",
                    path, ram_size);
        fclose(t->fp); t->fp = NULL; return -1;
    }
    if (t->flags & PSXTRACE_FLAG_ZSTD) {
        OPL_ERROR_F("psxtrace",
                    "%s: zstd records not yet supported in C reader; "
                    "decompress with `python -m tools.psxtrace_format`",
                    path);
        fclose(t->fp); t->fp = NULL; return -1;
    }
    OPL_INFO_F("psxtrace",
               "opened %s: version=%u frames=%u flags=0x%x",
               path, t->version, t->frame_count, t->flags);
    return 0;
}

int openpete_psxtrace_advance(openpete_psxtrace_t* t,
                              const uint8_t* live_ram,
                              size_t live_ram_size) {
    if (!t->fp) return -1;
    if (live_ram_size < OPENPETE_PSX_RAM_SIZE) {
        OPL_ERROR_F("psxtrace", "live_ram %zu < %u",
                    live_ram_size, OPENPETE_PSX_RAM_SIZE);
        return -1;
    }
    uint32_t magic = 0, frame_no = 0, pad1 = 0, pad2 = 0, pc = 0, n = 0;
    if (read_u32_le(t->fp, &magic)) {
        if (feof(t->fp)) return 0;
        return -1;
    }
    if (magic != PSXTRACE_FRMR_MAGIC) {
        OPL_ERROR_F("psxtrace",
                    "record %u: bad magic 0x%08x (expected 'FRMR')",
                    t->records_consumed, magic);
        return -1;
    }
    if (read_u32_le(t->fp, &frame_no) ||
        read_u32_le(t->fp, &pad1)     ||
        read_u32_le(t->fp, &pad2)     ||
        read_u32_le(t->fp, &pc)       ||
        read_u32_le(t->fp, &n)) {
        OPL_ERROR_F("psxtrace", "record %u: short header",
                    t->records_consumed);
        return -1;
    }
    if (n != OPENPETE_PSX_RAM_SIZE) {
        OPL_ERROR_F("psxtrace",
                    "record %u: payload %u != raw 2MB (zstd unsupported)",
                    t->records_consumed, n);
        return -1;
    }
    /* Stream-compare 64 KB at a time. */
    enum { CHUNK = 65536 };
    uint8_t buf[CHUNK];
    uint32_t off = 0;
    while (off < n) {
        size_t want = (n - off) < CHUNK ? (n - off) : CHUNK;
        size_t got = fread(buf, 1, want, t->fp);
        if (got != want) {
            OPL_ERROR_F("psxtrace",
                        "record %u: payload truncated at %u/%u",
                        t->records_consumed, off + (uint32_t)got, n);
            return -1;
        }
        for (size_t i = 0; i < want; i++) {
            if (buf[i] != live_ram[off + i]) {
                t->bytes_diverged++;
                if (!t->diverged) {
                    t->diverged = 1;
                    t->first_divergent_frame  = frame_no;
                    t->first_divergent_addr   =
                        OPENPETE_PSX_KSEG0_BASE + off + (uint32_t)i;
                    t->first_divergent_op     = live_ram[off + i];
                    t->first_divergent_oracle = buf[i];
                }
            }
        }
        off += (uint32_t)want;
    }
    t->bytes_compared += n;
    t->records_consumed++;
    return 1;
}

void openpete_psxtrace_close(openpete_psxtrace_t* t) {
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
}
