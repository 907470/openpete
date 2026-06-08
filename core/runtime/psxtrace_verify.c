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
    t->hard_fail_region = -1;
    /* CD-load transient tolerance window (ticks). Largest legitimate
     * level-load burst observed in the 20k demo is ~310 ticks; default
     * 600 leaves ~2x headroom while still catching a permanent leak. */
    t->heal_window = 600u;
    {
        const char* hw = getenv("OPENPETE_PSXTRACE_HEAL_WINDOW");
        if (hw && *hw) {
            unsigned long v = strtoul(hw, NULL, 0);
            if (v > 0) t->heal_window = (uint32_t)v;
        }
    }
    for (uint32_t i = 0; i < OPENPETE_PSXTRACE_MAX_REGIONS; i++)
        t->transient_open_frame[i] = -1;
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
    /* Peek the first record's pc field so callers can use it as the
     * dispatch-anchor default (the trace was captured at hits of this
     * PC; openpete should advance() at the same PC for epoch parity).
     * Layout: u32 magic + u32 frame_no + u32 pad1 + u32 pad2 + u32 pc.
     * Read pc, then rewind so read_records sees the record intact. */
    long save = ftell(t->fp);
    uint32_t peek_magic, peek_frame, peek_p1, peek_p2, peek_pc;
    if (read_u32_le(t->fp, &peek_magic) == 0 &&
        peek_magic == PSXTRACE_FRMR_MAGIC &&
        read_u32_le(t->fp, &peek_frame) == 0 &&
        read_u32_le(t->fp, &peek_p1)    == 0 &&
        read_u32_le(t->fp, &peek_p2)    == 0 &&
        read_u32_le(t->fp, &peek_pc)    == 0) {
        t->inferred_anchor_pc = peek_pc;
    }
    if (save >= 0) fseek(t->fp, save, SEEK_SET);
    OPL_INFO_F("psxtrace",
               "opened %s: v%u frames=%u regions=%u flags=0x%x has_ram=%d "
               "anchor_pc=0x%08X",
               path, t->version, t->frame_count, t->region_count,
               t->flags, !!(t->flags & PSXTRACE_FLAG_HAS_RAM),
               t->inferred_anchor_pc);
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
    /* Per-region compare with CD-load-transient heal tracking. We scan
     * every region every frame (not only when the total mismatches) so a
     * transient region that re-converges is detected even on a frame whose
     * total still differs due to another region, and so a heal that drops
     * the total back to matching is still observed. */
    static int s_dump = -1;
    if (s_dump < 0) s_dump = getenv("OPENPETE_PSXTRACE_DUMP") ? 1 : 0;

    int any_mism = 0;
    for (uint32_t r = 0; r < t->region_count; r++) {
        const int in_range = (r < (uint32_t)openpete_state_region_count);
        /* gameplay_neutral: render-side region audited as gameplay-neutral.
         * Treated as a transient with an INFINITE heal window — its
         * divergence is counted for diagnostics but never trips
         * hard_failed. Any real gameplay coupling surfaces in the tracked
         * downstream regions (psyq_rand_seed, g_Spyro, …). */
        const int neutral   = in_range ? openpete_state_regions[r].gameplay_neutral : 0;
        const int transient = neutral
                            || (in_range ? openpete_state_regions[r].cd_load_transient : 0);

        if (op_per[r] != oracle_per[r]) {
            any_mism = 1;
            t->hashes_diverged++;
            t->per_region_diverge_count[r]++;
            if (s_dump) {
                OPL_INFO_F("psxtrace",
                           "frame=%u region=%u (%s) op=0x%016llx oracle=0x%016llx",
                           frame_no, r, t->region_names[r],
                           (unsigned long long)op_per[r],
                           (unsigned long long)oracle_per[r]);
            }
            if (!t->diverged) {
                t->diverged = 1;
                t->first_divergent_frame  = frame_no;
                t->first_divergent_region = (int32_t)r;
                t->first_divergent_oracle = oracle_per[r];
                t->first_divergent_op     = op_per[r];
            }
            if (!transient) {
                /* A non-transient region differing is a hard regression. */
                if (!t->hard_failed) {
                    t->hard_failed     = 1;
                    t->hard_fail_frame = frame_no;
                    t->hard_fail_region = (int32_t)r;
                    t->hard_fail_oracle = oracle_per[r];
                    t->hard_fail_op     = op_per[r];
                    t->hard_fail_reason = "non-transient";
                }
            } else if (t->transient_open_frame[r] < 0) {
                /* Open a new CD-load transient burst. */
                t->transient_open_frame[r] = (int64_t)frame_no;
            } else if (!neutral && (int64_t)frame_no - t->transient_open_frame[r]
                       > (int64_t)t->heal_window) {
                /* Burst outlived the tolerance window: treat as a real
                 * (non-healing) divergence. */
                if (!t->hard_failed) {
                    t->hard_failed     = 1;
                    t->hard_fail_frame = frame_no;
                    t->hard_fail_region = (int32_t)r;
                    t->hard_fail_oracle = oracle_per[r];
                    t->hard_fail_op     = op_per[r];
                    t->hard_fail_reason = "unhealed";
                }
            }
        } else if (transient && t->transient_open_frame[r] >= 0) {
            /* Transient region re-converged: the burst healed. */
            int64_t start = t->transient_open_frame[r];
            t->transient_healed_bursts++;
            OPL_INFO_F("psxtrace",
                       "CD-load transient absorbed: region=%u (%s) "
                       "ticks %lld..%u (len=%lld, window=%u)",
                       r, t->region_names[r],
                       (long long)start, frame_no - 1u,
                       (long long)((int64_t)frame_no - start),
                       t->heal_window);
            t->transient_open_frame[r] = -1;
        }
    }
    if (total_op != total_oracle && !any_mism) {
        /* Total mismatched but no per-region did — region traversal
         * disagreement (manifest skew, NULL-ptr deref differences). Not a
         * CD-load transient; treat as a hard failure. */
        if (!t->diverged) {
            t->diverged = 1;
            t->first_divergent_frame  = frame_no;
            t->first_divergent_region = -1;
            t->first_divergent_oracle = total_oracle;
            t->first_divergent_op     = total_op;
        }
        if (!t->hard_failed) {
            t->hard_failed     = 1;
            t->hard_fail_frame = frame_no;
            t->hard_fail_region = -1;
            t->hard_fail_oracle = total_oracle;
            t->hard_fail_op     = total_op;
            t->hard_fail_reason = "total-only";
        }
    }
    t->frames_compared++;
    t->records_consumed++;
    return 1;
}

void openpete_psxtrace_finalize(openpete_psxtrace_t* t) {
    /* A transient burst still open when the WHOLE trace was consumed never
     * re-converged → hard fail. But if the run was cut short (frame-limit,
     * crash, fast smoke) before the trace's end, an open burst is merely
     * unverified, not a failure — only promote when we actually hit EOF. */
    int eof_reached = (t->records_consumed >= t->frame_count);
    for (uint32_t r = 0; r < t->region_count; r++) {
        if (t->transient_open_frame[r] < 0) continue;
        if (eof_reached) {
            OPL_ERROR_F("psxtrace",
                        "CD-load transient in region=%u (%s) opened at tick "
                        "%lld never re-converged before EOF",
                        r, t->region_names[r],
                        (long long)t->transient_open_frame[r]);
            if (!t->hard_failed) {
                t->hard_failed     = 1;
                t->hard_fail_frame = (uint32_t)t->transient_open_frame[r];
                t->hard_fail_region = (int32_t)r;
                t->hard_fail_reason = "unhealed-eof";
            }
        } else {
            OPL_INFO_F("psxtrace",
                       "run ended early (%u/%u records) with region=%u (%s) "
                       "mid-transient (opened tick %lld) — unverified, not a failure",
                       t->records_consumed, t->frame_count, r, t->region_names[r],
                       (long long)t->transient_open_frame[r]);
        }
    }
    OPL_INFO_F("psxtrace",
               "verdict: %s (hard_failed=%d, CD-load transients absorbed=%llu, "
               "heal_window=%u ticks)",
               t->hard_failed ? "FAIL" : "PASS", t->hard_failed,
               (unsigned long long)t->transient_healed_bursts,
               t->heal_window);
}

void openpete_psxtrace_close(openpete_psxtrace_t* t) {
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
    free(t->ram_buf);
    t->ram_buf = NULL;
}
