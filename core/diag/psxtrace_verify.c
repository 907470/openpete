/* core/diag/psxtrace_verify.c - consumer for the .psxtrace v2 oracle. */

#include "openpete_psxtrace.h"
#include "openpete_cdrom_mmio.h"
#include "openpete_log.h"
#include "openpete_psx.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
extern SDL_AtomicInt openpete_vbl;

/* Provided by run.c (gameplay_hash_compute_ex). Mirrors the Lua dumper's
 * scripts/openpete_state_hash.lua so the value-equality check is
 * meaningful. */
extern uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);

#define PSXTRACE_VERSION        2u
#define PSXTRACE_FRMR_MAGIC     0x524D5246u  /* 'FRMR' */
#define PSXTRACE_FLAG_ZSTD       0x1u
#define PSXTRACE_FLAG_HAS_RAM    0x2u
#define PSXTRACE_FLAG_TABLE_HASH 0x4u
#define PSXTRACE_FLAG_SCM_HASH   0x8u
#define PSXTRACE_FLAG_VBL_STAMP  0x10u
#define PSXTRACE_FLAGS_KNOWN     (PSXTRACE_FLAG_ZSTD | PSXTRACE_FLAG_HAS_RAM | \
                                  PSXTRACE_FLAG_TABLE_HASH | PSXTRACE_FLAG_SCM_HASH | \
                                  PSXTRACE_FLAG_VBL_STAMP)

/** @brief Read four bytes from fp as a little-endian u32 into *out.
 *
 * @return 0 on success; -1 on a short read, leaving *out unchanged. */
static int read_u32_le(FILE* fp, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return -1;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] <<  8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 0;
}

/** @brief Read eight bytes from fp as a little-endian u64 into *out, low word first.
 *
 * @return 0 on success; -1 on a short read, leaving *out unchanged. */
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
    t->first_strict_divergent_region = -1;
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
    {
        const char* vp = getenv("OPENPETE_PSXTRACE_VBL_POLICY");
        if (vp && strcmp(vp, "exact") == 0) t->vbl_policy_exact = 1;
        else if (vp && *vp && strcmp(vp, "log") != 0) {
            OPL_ERROR_F("psxtrace", "OPENPETE_PSXTRACE_VBL_POLICY=%s: wanted exact or log", vp);
            return -1;
        }
    }
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
                    "%s: unsupported version %u (wanted %u - rebuild trace)",
                    path, t->version, PSXTRACE_VERSION);
        goto fail;
    }
    if (ram_size != OPENPETE_PSX_RAM_SIZE) {
        OPL_ERROR_F("psxtrace", "%s: unexpected ram_size 0x%x",
                    path, ram_size);
        goto fail;
    }
    if (t->region_count == 0) {
        OPL_ERROR_F("psxtrace", "%s: region_count 0 - a trace over no regions verifies nothing",
                    path);
        goto fail;
    }
    if (t->region_count > OPENPETE_PSXTRACE_MAX_REGIONS) {
        OPL_ERROR_F("psxtrace", "%s: region_count %u > max %u",
                    path, t->region_count, OPENPETE_PSXTRACE_MAX_REGIONS);
        goto fail;
    }
    if (t->flags & ~PSXTRACE_FLAGS_KNOWN) {
        OPL_ERROR_F("psxtrace",
                    "%s: header flags 0x%x carry bits this reader does not "
                    "know (0x%x); its header blocks would misalign the records",
                    path, t->flags, t->flags & ~PSXTRACE_FLAGS_KNOWN);
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
            /* A name the buffer cannot hold whole cannot be compared whole. */
            if (copy != nlen) {
                free(blob);
                OPL_ERROR_F("psxtrace", "%s: region %u name is %u bytes, max %u",
                            path, r, nlen, OPENPETE_PSXTRACE_REGION_NAME_MAX - 1u);
                goto fail;
            }
            i += nlen;
        }
        free(blob);
    }
    /* Header blocks follow the names blob in flag-bit order. */
    if (t->flags & PSXTRACE_FLAG_TABLE_HASH) {
        uint32_t lo, hi;
        if (read_u32_le(t->fp, &lo) || read_u32_le(t->fp, &hi)) {
            OPL_ERROR_F("psxtrace", "%s: truncated table-hash block", path);
            goto fail;
        }
        t->table_hash = ((uint64_t)hi << 32) | lo;
    }
    if (t->flags & PSXTRACE_FLAG_SCM_HASH) {
        uint8_t n;
        if (fread(&n, 1, 1, t->fp) != 1) {
            OPL_ERROR_F("psxtrace", "%s: truncated scm-hash block", path);
            goto fail;
        }
        char tmp[256];
        if (fread(tmp, 1, n, t->fp) != n) {
            OPL_ERROR_F("psxtrace", "%s: truncated scm-hash block", path);
            goto fail;
        }
        uint32_t copy = n < sizeof(t->scm_hash) - 1u ? n : (uint32_t)sizeof(t->scm_hash) - 1u;
        memcpy(t->scm_hash, tmp, copy);
        t->scm_hash[copy] = 0;
    }
    t->has_vbl_stamps = !!(t->flags & PSXTRACE_FLAG_VBL_STAMP);
    /* The sidecars beside the golden are the reference's schedule; under
     * it openpete's VBLs are the reference's, so the per-record delta
     * compare is exact unless the policy was set to log by hand. */
    (void)openpete_cdrom_mmio_open(path);
    if (openpete_sched_open(path) && t->has_vbl_stamps) {
        const char* vp = getenv("OPENPETE_PSXTRACE_VBL_POLICY");
        if (!(vp && strcmp(vp, "log") == 0)) t->vbl_policy_exact = 1;
    }
    if (t->vbl_policy_exact && !t->has_vbl_stamps) {
        OPL_ERROR_F("psxtrace", "%s: OPENPETE_PSXTRACE_VBL_POLICY=exact but the trace "
                    "carries no VBL stamps - recapture", path);
        goto fail;
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
                    "%s: trace region_count=%u but openpete has %zu - "
                    "manifest mismatch (rebuild trace from same manifest)",
                    path, t->region_count, openpete_state_region_count);
        goto fail;
    }
    /* Row order and names must be the registered table's: a hash column
     * compared under another row's name is a wrong verdict, not a
     * divergence. */
    for (uint32_t r = 0; r < t->region_count; r++) {
        const char* want = openpete_state_regions[r].name ? openpete_state_regions[r].name : "";
        if (strcmp(t->region_names[r], want) != 0) {
            OPL_ERROR_F("psxtrace",
                        "%s: region %u is \"%s\" in the trace but \"%s\" in the "
                        "registered table - manifest differs from capture",
                        path, r, t->region_names[r], want);
            goto fail;
        }
    }
    /* Same names with edited geometry (start/length/mask) would fail at
     * record 0 in that row, indistinguishable from a game divergence; the
     * table hash separates the two. */
    if (!(t->flags & PSXTRACE_FLAG_TABLE_HASH)) {
        OPL_ERROR_F("psxtrace",
                    "%s: no table-hash block (captured before the module "
                    "read its table from the manifest) - recapture",
                    path);
        goto fail;
    }
    if (t->table_hash != openpete_state_regions_table_hash) {
        OPL_ERROR_F("psxtrace",
                    "%s: table hash 0x%016llx but the registered table is "
                    "0x%016llx - manifest differs from capture",
                    path, (unsigned long long)t->table_hash,
                    (unsigned long long)openpete_state_regions_table_hash);
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
    /* The VBL stamps of every record, read ahead for the schedule: the
     * reference's VBLANK at each anchor hit is charged before that
     * record's hash. Hash-only traces have fixed-size records. */
    if (t->has_vbl_stamps && !(t->flags & PSXTRACE_FLAG_HAS_RAM) && save >= 0) {
        const long rec_size = 5 * 4 + 8 + 8 * (long)t->region_count;
        t->stamps = (uint32_t*)calloc(t->frame_count ? t->frame_count : 1u, sizeof(uint32_t));
        uint32_t n = 0;
        for (; t->stamps && n < t->frame_count; n++) {
            uint32_t m, f, p1, p2;
            if (fseek(t->fp, save + rec_size * (long)n, SEEK_SET) != 0) break;
            if (read_u32_le(t->fp, &m) || m != PSXTRACE_FRMR_MAGIC || read_u32_le(t->fp, &f) ||
                read_u32_le(t->fp, &p1) || read_u32_le(t->fp, &p2)) break;
            t->stamps[n] = p2;
        }
        t->stamp_count = n;
        openpete_sched_set_stamps(t->stamps, n);
    }
    if (save >= 0) fseek(t->fp, save, SEEK_SET);
    OPL_INFO_F("psxtrace",
               "opened %s: v%u frames=%u regions=%u flags=0x%x has_ram=%d "
               "anchor_pc=0x%08X table=0x%016llx build=%s",
               path, t->version, t->frame_count, t->region_count,
               t->flags, !!(t->flags & PSXTRACE_FLAG_HAS_RAM),
               t->inferred_anchor_pc, (unsigned long long)t->table_hash,
               t->scm_hash[0] ? t->scm_hash : "-");
    return 0;
fail:
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
    free(t->ram_buf);
    t->ram_buf = NULL;
    return -1;
}

int openpete_psxtrace_skip_to(openpete_psxtrace_t* t, uint32_t rec) {
    if (!t->fp || (t->flags & PSXTRACE_FLAG_HAS_RAM)) return -1;
    const long rec_size = 5 * 4 + 8 + 8 * (long)t->region_count;
    while (t->records_consumed < rec && t->records_consumed < t->frame_count) {
        if (fseek(t->fp, rec_size, SEEK_CUR) != 0) return -1;
        /* The delta compare resumes from the skipped-to record's stamp. */
        if (t->stamps && t->records_consumed < t->stamp_count)
            t->prev_trace_vbl = t->stamps[t->records_consumed];
        t->records_consumed++;
        t->records_skipped++;
    }
    return 0;
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
    for (uint32_t r = 0; r < t->region_count; r++) {
        if (t->records_consumed > 0 && oracle_per[r] != t->prev_oracle_per[r])
            t->per_region_change_count[r]++;
        t->prev_oracle_per[r] = oracle_per[r];
    }
    /* Optional RAM payload - read it before the diff so the file pointer
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
    (void)pc;
    /* Per-record VBL stamp: compare this record's delta from the previous
     * with openpete's, so a load whose cadence differs, or a skipped state,
     * fails at its first record with both deltas named. */
    if (t->has_vbl_stamps) {
        const int32_t op_vbl = SDL_GetAtomicInt(&openpete_vbl);
        if (t->records_consumed > 0) {
            const int64_t d_trace = (int64_t)pad2 - (int64_t)t->prev_trace_vbl;
            const int64_t d_op    = (int64_t)op_vbl - (int64_t)t->prev_op_vbl;
            const int bt = (d_trace < 0) ? 16 : (d_trace > 16 ? 16 : (int)d_trace);
            const int bo = (d_op < 0)    ? 16 : (d_op > 16 ? 16 : (int)d_op);
            t->vbl_hist_trace[bt]++;
            t->vbl_hist_op[bo]++;
            /* Under the schedule a VBL the reference spent inside a tick's
             * work is charged by openpete at the next wait, so openpete
             * reaches that record one VBL behind and the next record one
             * ahead: a deficit that opens and closes within two records.
             * A deficit that grows negative (openpete ahead) or persists
             * is a desync. */
            int sched_ok = 0;
            if (openpete_sched_active()) {
                t->vbl_deficit += d_trace - d_op;
                if (t->vbl_deficit == 0) t->vbl_deficit_records = 0;
                else t->vbl_deficit_records++;
                sched_ok = (t->vbl_deficit >= 0 && t->vbl_deficit_records <= 2);
                if (d_trace != d_op && t->vbl_deficit > 0) t->vbl_long_frames++;
            }
            if (d_trace != d_op) {
                if (t->vbl_delta_mismatches == 0) {
                    t->first_vbl_mismatch_frame = frame_no;
                    t->first_vbl_mismatch_trace = (int32_t)d_trace;
                    t->first_vbl_mismatch_op    = (int32_t)d_op;
                }
                t->vbl_delta_mismatches++;
                if (t->vbl_policy_exact && !sched_ok && !t->hard_failed) {
                    t->hard_failed      = 1;
                    t->hard_fail_frame  = frame_no;
                    t->hard_fail_region = -1;
                    t->hard_fail_reason = "vbl-delta";
                    OPL_ERROR_F("psxtrace",
                                "record %u: VBL delta %lld on the reference, %lld in openpete",
                                frame_no, (long long)d_trace, (long long)d_op);
                }
            }
        }
        t->prev_trace_vbl = pad2;
        t->prev_op_vbl = op_vbl;
    }
    /* Negative control (harness): OPENPETE_PSXTRACE_POKE=ADDR:BYTE:FRAME
     * writes one byte into guest RAM at the anchor of that frame, before
     * the hashes are taken. A verify run under it must report a
     * non-transient divergence at that frame in the region holding ADDR;
     * a PASS means the gate is not looking at that byte. */
    {
        static int      s_poke_parsed = 0;
        static uint32_t s_poke_addr = 0, s_poke_frame = 0;
        static uint8_t  s_poke_val = 0;
        if (!s_poke_parsed) {
            s_poke_parsed = 1;
            const char* e = getenv("OPENPETE_PSXTRACE_POKE");
            if (e && *e) {
                unsigned a = 0, v = 0, f = 0;
                if (sscanf(e, "%i:%i:%u", &a, &v, &f) == 3) {
                    s_poke_addr = a; s_poke_val = (uint8_t)v; s_poke_frame = f;
                    s_poke_parsed = 2;
                }
            }
        }
        if (s_poke_parsed == 2 && frame_no == s_poke_frame) {
            uint8_t* ram = openpete_psx_get_ram();
            uint32_t off = s_poke_addr & (OPENPETE_PSX_RAM_SIZE - 1u);
            OPL_INFO_F("psxtrace", "poke: frame=%u 0x%08X: 0x%02X -> 0x%02X",
                       frame_no, s_poke_addr, ram[off], s_poke_val);
            ram[off] = s_poke_val;
        }
    }
    {
        const char* why = NULL;
        if (openpete_sched_failed(&why) && !t->hard_failed) {
            t->hard_failed      = 1;
            t->hard_fail_frame  = frame_no;
            t->hard_fail_region = -1;
            t->hard_fail_reason = "schedule";
            OPL_ERROR_F("psxtrace", "record %u: schedule failed: %s", frame_no, why);
        }
    }
    /* Inside a load window the loader's tick count is CPU time and the two
     * sides' records do not correspond; the record is consumed and not
     * compared, and the re-alignment at the window's end skips the rest. */
    if (openpete_sched_in_load()) {
        t->records_uncompared++;
        t->prev_trace_vbl = pad2;
        t->prev_op_vbl = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
        t->frames_compared++;
        t->records_consumed++;
        return 1;
    }
    /* OPENPETE_PSXTRACE_RAM_DUMP=FRAME:PATH writes openpete's RAM as it is
     * hashed for that record, for a byte diff against the reference's
     * dump at the same hit. */
    {
        static int s_rd_parsed = 0; static unsigned s_rd_frame = 0; static char s_rd_path[512];
        if (!s_rd_parsed) {
            s_rd_parsed = 1;
            const char* e = getenv("OPENPETE_PSXTRACE_RAM_DUMP");
            if (e && *e && sscanf(e, "%u:%511s", &s_rd_frame, s_rd_path) == 2) s_rd_parsed = 2;
        }
        if (s_rd_parsed == 2 && frame_no == s_rd_frame) {
            FILE* fp = fopen(s_rd_path, "wb");
            if (fp) { fwrite(openpete_psx_get_ram(), 1, OPENPETE_PSX_RAM_SIZE, fp); fclose(fp); }
            OPL_INFO_F("psxtrace", "ram dump: frame=%u -> %s%s", frame_no, s_rd_path, fp ? "" : " (open failed)");
        }
    }
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
        /* gameplay_neutral: render-side region audited as gameplay-neutral;
         * host_precision: the state of a host device model coarser than
         * the hardware. Both are treated as a transient with an INFINITE
         * heal window: the divergence is counted for diagnostics but never
         * trips hard_failed. Any real gameplay coupling surfaces in the
         * tracked downstream regions (psyq_rand_seed, g_Spyro, ...). */
        const int neutral   = in_range ? (openpete_state_regions[r].gameplay_neutral
                                          || openpete_state_regions[r].host_precision) : 0;
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
            if (!neutral && !t->strict_diverged) {
                t->strict_diverged = 1;
                t->first_strict_divergent_frame  = frame_no;
                t->first_strict_divergent_region = (int32_t)r;
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
            /* The region re-converged: the burst ended. A neutral row's
             * burst is tolerated by its class (bounded only by its
             * diverge_baseline), a cd_load_transient row's by the heal
             * window; the two are counted apart so the CD-load count
             * measures the load tolerance alone. */
            int64_t start = t->transient_open_frame[r];
            if (neutral) {
                t->neutral_bursts++;
                OPL_INFO_F("psxtrace",
                           "%s burst: region=%u (%s) ticks %lld..%u (len=%lld)",
                           openpete_state_regions[r].host_precision ? "host_precision"
                                                                     : "gameplay_neutral",
                           r, t->region_names[r],
                           (long long)start, frame_no - 1u,
                           (long long)((int64_t)frame_no - start));
            } else {
                t->transient_healed_bursts++;
                OPL_INFO_F("psxtrace",
                           "CD-load transient absorbed: region=%u (%s) "
                           "ticks %lld..%u (len=%lld, window=%u)",
                           r, t->region_names[r],
                           (long long)start, frame_no - 1u,
                           (long long)((int64_t)frame_no - start),
                           t->heal_window);
            }
            t->transient_open_frame[r] = -1;
        }
    }
    if (total_op != total_oracle && !any_mism) {
        /* Total mismatched but no per-region did - region traversal
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
     * re-converged -> hard fail. But if the run was cut short (frame-limit,
     * crash, fast smoke) before the trace's end, an open burst is merely
     * unverified, not a failure - only promote when we actually hit EOF. */
    int eof_reached = (t->records_consumed >= t->frame_count);
    /* Ratchet: a tolerated row's divergence count over the full run may
     * not exceed the baseline its manifest row records. */
    if (eof_reached) {
        for (uint32_t r = 0; r < t->region_count; r++) {
            if (r >= (uint32_t)openpete_state_region_count) break;
            int32_t base = openpete_state_regions[r].diverge_baseline;
            if (base < 0) continue;
            uint64_t n = t->per_region_diverge_count[r];
            if (n > (uint64_t)base) {
                OPL_ERROR_F("psxtrace",
                            "ratchet: region=%u (%s) diverged on %llu records, "
                            "baseline %d",
                            r, t->region_names[r], (unsigned long long)n, base);
                if (!t->hard_failed) {
                    t->hard_failed      = 1;
                    t->hard_fail_frame  = t->frame_count;
                    t->hard_fail_region = (int32_t)r;
                    t->hard_fail_reason = "ratchet";
                }
            }
        }
    }
    for (uint32_t r = 0; r < t->region_count; r++) {
        if (t->transient_open_frame[r] < 0) continue;
        const int neutral = (r < (uint32_t)openpete_state_region_count)
                          && (openpete_state_regions[r].gameplay_neutral
                              || openpete_state_regions[r].host_precision);
        if (neutral) {
            /* A gameplay_neutral or host_precision row has no heal window;
             * an open burst at EOF is counted, never promoted. */
            OPL_INFO_F("psxtrace",
                       "%s region=%u (%s) diverging at EOF "
                       "(burst opened tick %lld)",
                       openpete_state_regions[r].host_precision ? "host_precision" : "gameplay_neutral",
                       r, t->region_names[r],
                       (long long)t->transient_open_frame[r]);
            continue;
        }
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
                       "mid-transient (opened tick %lld) - unverified, not a failure",
                       t->records_consumed, t->frame_count, r, t->region_names[r],
                       (long long)t->transient_open_frame[r]);
        }
    }
    openpete_sched_summary();
    openpete_cdrom_mmio_summary();
    if (t->hard_failed) {
        int32_t r = t->hard_fail_region;
        OPL_INFO_F("psxtrace",
                   "  hard fail: %s at frame=%u region=%d (%s)",
                   t->hard_fail_reason ? t->hard_fail_reason : "?",
                   t->hard_fail_frame, (int)r,
                   (r >= 0 && r < (int32_t)t->region_count)
                       ? t->region_names[r] : "<total>");
    }
    OPL_INFO_F("psxtrace",
               "verdict: %s (hard_failed=%d, CD-load transients absorbed=%llu, "
               "heal_window=%u ticks, neutral bursts=%llu)",
               t->hard_failed ? "FAIL" : "PASS", t->hard_failed,
               (unsigned long long)t->transient_healed_bursts,
               t->heal_window,
               (unsigned long long)t->neutral_bursts);
}

void openpete_psxtrace_close(openpete_psxtrace_t* t) {
    if (t->fp) { fclose(t->fp); t->fp = NULL; }
    free(t->ram_buf);
    t->ram_buf = NULL;
    free(t->stamps);
    t->stamps = NULL;
}
