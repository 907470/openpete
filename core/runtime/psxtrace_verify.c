/* core/runtime/psxtrace_verify.c — consumer for the .psxtrace v2 oracle. */

#include "openpete_psxtrace.h"
#include "openpete_log.h"
#include "openpete_psx.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Provided by run.c (gameplay_hash_compute_ex). Mirrors the Lua dumper's
 * scripts/openpete_state_hash.lua so the value-equality check is
 * meaningful. */
extern uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);

#define PSXTRACE_VERSION        2u
#define PSXTRACE_FRMR_MAGIC     0x524D5246u  /* 'FRMR' */
#define PSXTRACE_FLAG_ZSTD      0x1u
#define PSXTRACE_FLAG_HAS_RAM   0x2u

static int read_u32_le(FILE* fp, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return -1;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] <<  8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

static int read_u64_le(FILE* fp, uint64_t* out) {
    uint32_t lo = 0, hi = 0;
    if (read_u32_le(fp, &lo) || read_u32_le(fp, &hi)) return -1;
    *out = ((uint64_t)hi << 32) | (uint64_t)lo;
    return 0;
}

int openpete_psxtrace_open(openpete_psxtrace_t* t, const char* path) {
    memset(t, 0, sizeof(*t));
    t->path = path;
    t->first_divergent_region = -1;
    t->fp = fopen(path, "rb");
    if (!t->fp) {
        OPL_ERROR_F("psxtrace", "open %s failed: %s", path, strerror(errno));
        return -1;
    }
    uint8_t magic[8];
    if (fread(magic, 1, 8, t->fp) != 8 ||
        memcmp(magic, "PSXTRACE", 8) != 0) {
        OPL_ERROR_F("psxtrace", "%s: bad magic", path);
        goto fail;
    }
    uint32_t ram_size = 0, names_len = 0, reserved = 0;
    if (read_u32_le(t->fp, &t->version)      ||
        read_u32_le(t->fp, &t->frame_count)  ||
        read_u32_le(t->fp, &ram_size)        ||
        read_u32_le(t->fp, &t->flags)        ||
        read_u32_le(t->fp, &t->region_count) ||
        read_u32_le(t->fp, &names_len)       ||
        read_u32_le(t->fp, &reserved)) {
        OPL_ERROR_F("psxtrace", "%s: short header", path);
        goto fail;
    }
    if (t->version != PSXTRACE_VERSION) {
        OPL_ERROR_F("psxtrace",
                    "%s: unsupported version %u (wanted %u — rebuild trace)",
                    path, t->version, PSXTRACE_VERSION);
        goto fail;
    }
    if (ram_size != OPENPETE_PSX_RAM_SIZE) {
        OPL_ERROR_F("psxtrace", "%s: unexpected ram_size 0x%x",
                    path, ram_size);
        goto fail;
    }
    if (t->region_count > OPENPETE_PSXTRACE_MAX_REGIONS) {
        OPL_ERROR_F("psxtrace", "%s: region_count %u > max %u",
                    path, t->region_count, OPENPETE_PSXTRACE_MAX_REGIONS);
        goto fail;
    }
    /* Read and parse names blob. */
    if (names_len > 0) {
        uint8_t* blob = (uint8_t*)malloc(names_len);
        if (!blob) goto fail;
        if (fread(blob, 1, names_len, t->fp) != names_len) {
            free(blob);
            OPL_ERROR_F("psxtrace", "%s: truncated names blob", path);
            goto fail;
        }
        uint32_t i = 0;
        for (uint32_t r = 0; r < t->region_count; r++) {
            if (i >= names_len) { free(blob); goto fail; }
            uint8_t nlen = blob[i++];
            if (i + nlen > names_len) { free(blob); goto fail; }
            uint32_t copy = nlen;
            if (copy >= OPENPETE_PSXTRACE_REGION_NAME_MAX) {
                copy = OPENPETE_PSXTRACE_REGION_NAME_MAX - 1u;
            }
            memcpy(t->region_names[r], blob + i, copy);
            t->region_names[r][copy] = 0;
            i += nlen;
        }
        free(blob);
    }
    if (t->flags & PSXTRACE_FLAG_HAS_RAM) {
        if (t->flags & PSXTRACE_FLAG_ZSTD) {
            OPL_ERROR_F("psxtrace",
                        "%s: zstd RAM payloads not yet supported in C reader",
                        path);
            goto fail;
        }
        t->ram_buf = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
        if (!t->ram_buf) goto fail;
    }
    /* Sanity: openpete's region count must match the trace's, otherwise
     * the per-region hash columns won't line up. */
    if ((uint32_t)openpete_state_region_count != t->region_count) {
        OPL_ERROR_F("psxtrace",
                    "%s: trace region_count=%u but openpete has %zu — "
                    "manifest mismatch (rebuild trace from same manifest)",
                    path, t->region_count, openpete_state_region_count);
        goto fail;
    }
    OPL_INFO_F("psxtrace",
               "opened %s: v%u frames=%u regions=%u flags=0x%x has_ram=%d",
               path, t->version, t->frame_count, t->region_count,
               t->flags, !!(t->flags & PSXTRACE_FLAG_HAS_RAM));
    return 0;
fail:
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
    free(t->ram_buf);
    t->ram_buf = NULL;
    return -1;
}

int openpete_psxtrace_advance(openpete_psxtrace_t* t) {
    if (!t->fp) return -1;
    uint32_t magic = 0, frame_no = 0, pad1 = 0, pad2 = 0, pc = 0;
    uint64_t total_oracle = 0;
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
        read_u64_le(t->fp, &total_oracle)) {
        OPL_ERROR_F("psxtrace", "record %u: short fixed header",
                    t->records_consumed);
        return -1;
    }
    /* Read oracle's per-region hashes. */
    uint64_t oracle_per[OPENPETE_PSXTRACE_MAX_REGIONS];
    for (uint32_t r = 0; r < t->region_count; r++) {
        if (read_u64_le(t->fp, &oracle_per[r])) {
            OPL_ERROR_F("psxtrace",
                        "record %u: short hash array at r=%u",
                        t->records_consumed, r);
            return -1;
        }
    }
    /* Optional RAM payload — read it before the diff so the file pointer
     * advances regardless of whether we use it. */
    if (t->flags & PSXTRACE_FLAG_HAS_RAM) {
        uint32_t n = 0;
        if (read_u32_le(t->fp, &n)) return -1;
        if (n != OPENPETE_PSX_RAM_SIZE) {
            OPL_ERROR_F("psxtrace",
                        "record %u: ram-payload-len %u != %u "
                        "(zstd unsupported)",
                        t->records_consumed, n, OPENPETE_PSX_RAM_SIZE);
            return -1;
        }
        if (fread(t->ram_buf, 1, n, t->fp) != n) {
            OPL_ERROR_F("psxtrace",
                        "record %u: short ram payload",
                        t->records_consumed);
            return -1;
        }
    }
    (void)pad2; (void)pc;
    /* Compute openpete's hashes for THIS frame. */
    uint64_t op_per[OPENPETE_PSXTRACE_MAX_REGIONS];
    uint64_t total_op = gameplay_hash_compute_ex(op_per);
    /* Compare. Latch first divergence. */
    if (total_op != total_oracle) {
        for (uint32_t r = 0; r < t->region_count; r++) {
            if (op_per[r] != oracle_per[r]) {
                t->hashes_diverged++;
                if (!t->diverged) {
                    t->diverged = 1;
                    t->first_divergent_frame  = frame_no;
                    t->first_divergent_region = (int32_t)r;
                    t->first_divergent_oracle = oracle_per[r];
                    t->first_divergent_op     = op_per[r];
                }
            }
        }
        if (!t->diverged) {
            /* Total mismatched but no per-region did — shouldn't happen
             * unless region traversal differs (manifest skew, NULL ptr
             * deref disagreement, etc). Latch as region=-1. */
            t->diverged = 1;
            t->first_divergent_frame  = frame_no;
            t->first_divergent_region = -1;
            t->first_divergent_oracle = total_oracle;
            t->first_divergent_op     = total_op;
        }
    }
    t->frames_compared++;
    t->records_consumed++;
    return 1;
}

void openpete_psxtrace_close(openpete_psxtrace_t* t) {
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
    free(t->ram_buf);
    t->ram_buf = NULL;
}
