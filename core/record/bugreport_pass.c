/* core/record/bugreport_pass.c - the bug-report redaction pass over the
 * bundle's members: rewrite each by its class, scan it, and drop or flag
 * it. See bugreport_internal.h. */
#include "bugreport_internal.h"

#include "miniz.h"
#include "openpete_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const k_what[BRF_N] = {"home directory", "user name", "host name"};

/** @brief Record one finding of the scan in progress. */
static void scan_hit(void* ud, size_t off, br_needle_kind_t what)
{
    br_pass_t* s = (br_pass_t*)ud;
    s->total++;
    s->hits[what]++;
    if (s->nf >= BR_FINDINGS_MAX) return;
    br_finding_t* f = &s->f[s->nf++];
    snprintf(f->member, sizeof f->member, "%s", s->member);
    snprintf(f->section, sizeof f->section, "%s", s->section ? s->section : "");
    f->offset = s->base + off;
    f->what = what;
}

/** @brief Count one match by kind into a size_t[BRF_N]. */
static void count_hit(void* ud, size_t off, br_needle_kind_t what)
{
    (void)off;
    ((size_t*)ud)[what]++;
}

/** @brief Return the kind of the first nonzero count, or BRF_N when every count is zero. */
static br_needle_kind_t first_kind(const size_t hits[BRF_N])
{
    for (int k = 0; k < BRF_N; k++)
        if (hits[k]) return (br_needle_kind_t)k;
    return BRF_N;
}

/** @brief Scan a byte range for identities and return the first kind found, or BRF_N. */
static br_needle_kind_t survivor(const br_redactor_t* R, const void* d, size_t n)
{
    size_t hits[BRF_N] = {0};
    br_scan(R, (const uint8_t*)d, n, count_hit, hits);
    return first_kind(hits);
}

/** @brief Scan a member's bytes, by section for a state file, into the findings and the member's counts.
 * @param R The redactor.
 * @param s The pass.
 * @param mb The member. */
static void scan_member(const br_redactor_t* R, br_pass_t* s, const br_member_t* mb)
{
    s->member = mb->name;
    s->section = NULL;
    s->base = 0;
    memset(s->hits, 0, sizeof s->hits);
    if (mb->kind == BRM_STATE) {
        openpete_state_section_t sec[16];
        const int n = openpete_state_image_sections(mb->data, mb->len, sec, 16);
        for (int i = 0; i < n; i++) {
            s->section = sec[i].name;
            s->base = sec[i].off;
            br_scan(R, mb->data + sec[i].off, sec[i].len, scan_hit, s);
        }
        if (n > 0) return;
    }
    br_scan(R, mb->data, mb->len, scan_hit, s);
}

/* redact_bk2's failures. */
enum { BK2_OK = 0, BK2_UNPARSED = -1, BK2_OOM = -2, BK2_SURVIVOR = -3 };

/** @brief Rebuild a bk2 zip with its entries redacted and each redacted entry scanned.
 * @param R The redactor.
 * @param mb The member, replaced in place on success.
 * @param what Receives the kind of a surviving identity.
 * @return BK2_OK, or the failure (the member left as it was). */
static int redact_bk2(br_redactor_t* R, br_member_t* mb, br_needle_kind_t* what)
{
    mz_zip_archive in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    if (!mz_zip_reader_init_mem(&in, mb->data, mb->len, 0)) return BK2_UNPARSED;
    if (!mz_zip_writer_init_heap(&out, 0, 0)) { mz_zip_reader_end(&in); return BK2_OOM; }
    int rc = BK2_OK;
    const mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n && rc == BK2_OK; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&in, i, &st)) { rc = BK2_UNPARSED; break; }
        size_t len = 0;
        void* d = mz_zip_reader_extract_to_heap(&in, i, &len, 0);
        if (!d) { rc = BK2_UNPARSED; break; }
        size_t rn = len;
        char* r = br_redact_text(R, (const char*)d, len, &rn);
        mz_free(d);
        if (!r) { rc = BK2_OOM; break; }
        *what = survivor(R, r, rn);
        if (*what == BRF_N) *what = survivor(R, st.m_filename, strlen(st.m_filename));
        if (*what != BRF_N) rc = BK2_SURVIVOR;
        else if (!mz_zip_writer_add_mem(&out, st.m_filename, r, rn, MZ_DEFAULT_LEVEL)) rc = BK2_OOM;
        free(r);
    }
    mz_zip_reader_end(&in);
    void* zb = NULL;
    size_t zl = 0;
    if (rc == BK2_OK && !mz_zip_writer_finalize_heap_archive(&out, &zb, &zl)) rc = BK2_OOM;
    mz_zip_writer_end(&out);
    if (rc != BK2_OK) { free(zb); return rc; }
    free(mb->data);
    mb->data = (uint8_t*)zb;
    mb->len = zl;
    return BK2_OK;
}

/** @brief Remove member @p i from the list, recording it as dropped with a reason.
 * @param m The members.
 * @param i The member's index.
 * @param s The pass.
 * @param fmt The reason's format. */
static void drop_member(br_members_t* m, size_t i, br_pass_t* s, const char* fmt, ...)
{
    s->drop_total++;
    if (s->ndrop < BR_DROPS_MAX) {
        br_drop_t* d = &s->drop[s->ndrop++];
        snprintf(d->member, sizeof d->member, "%s", m->v[i].name);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d->reason, sizeof d->reason, fmt, ap);
        va_end(ap);
    }
    free(m->v[i].name);
    free(m->v[i].data);
    memmove(&m->v[i], &m->v[i + 1], (m->n - i - 1) * sizeof m->v[0]);
    m->n--;
}

/** @brief Redact one text member in place and scan the result.
 * @return NULL when the member ships, else the reason it is dropped. */
static const char* redact_text_member(br_redactor_t* R, br_member_t* mb, char* why, size_t cap)
{
    size_t rn = 0;
    char* r = br_redact_text(R, (const char*)mb->data, mb->len, &rn);
    if (!r) return "redaction ran out of memory";
    free(mb->data);
    mb->data = (uint8_t*)r;
    mb->len = rn;
    const br_needle_kind_t k = survivor(R, mb->data, mb->len);
    if (k == BRF_N) return NULL;
    snprintf(why, cap, "the %s survives redaction", k_what[k]);
    return why;
}

/** @brief Record a scanned member's counts as a flag when it has findings. */
static void flag_member(br_pass_t* s, const br_member_t* mb)
{
    if (first_kind(s->hits) == BRF_N) return;
    s->flag_total++;
    if (s->nflag >= BR_FLAGS_MAX) return;
    br_flag_t* f = &s->flag[s->nflag++];
    snprintf(f->member, sizeof f->member, "%s", mb->name);
    memcpy(f->hits, s->hits, sizeof f->hits);
}

void br_redact_members(br_redactor_t* R, br_members_t* m, br_pass_t* s)
{
    for (size_t i = 0; i < m->n;) {
        br_member_t* mb = &m->v[i];
        const br_needle_kind_t nk = survivor(R, mb->name, strlen(mb->name));
        if (nk != BRF_N) {
            drop_member(m, i, s, "its name holds the %s", k_what[nk]);
            continue;
        }
        char why[96];
        const char* drop = NULL;
        switch (mb->kind) {
        case BRM_TEXT:
            drop = redact_text_member(R, mb, why, sizeof why);
            break;
        case BRM_BK2: {
            br_needle_kind_t k = BRF_N;
            const int rc = redact_bk2(R, mb, &k);
            if (rc == BK2_UNPARSED) drop = "the bk2 does not parse, so its text cannot be redacted";
            else if (rc == BK2_OOM) drop = "redaction ran out of memory";
            else if (rc == BK2_SURVIVOR) {
                snprintf(why, sizeof why, "the %s survives redaction in an entry", k_what[k]);
                drop = why;
            }
            break;
        }
        case BRM_DUMP: {
            const br_needle_kind_t k = survivor(R, mb->data, mb->len);
            if (k != BRF_N) {
                snprintf(why, sizeof why, "the %s occurs in the crash dump, which cannot be rewritten",
                         k_what[k]);
                drop = why;
            }
            break;
        }
        case BRM_STATE: {
            const int n = openpete_state_image_card_paths_set(mb->data, mb->len, "card1.mcd", "card2.mcd");
            if (n < 0) {
                drop = "the state image does not parse, so its card paths cannot be rewritten";
                break;
            }
            R->replacements += (uint64_t)n;
            scan_member(R, s, mb);
            flag_member(s, mb);
            break;
        }
        default:
            scan_member(R, s, mb);
            flag_member(s, mb);
            break;
        }
        if (drop) drop_member(m, i, s, "%s", drop);
        else i++;
    }
}

size_t br_flag_line(const br_pass_t* s, char* out, size_t cap)
{
    static const char lead[] = "Some files in this report contain your user folder or computer name: ";
    out[0] = '\0';
    if (s->flag_total == 0 || cap < 128) return 0;
    size_t n = (size_t)snprintf(out, cap, "%s", lead);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < s->nflag; i++) {
        const size_t rest = s->flag_total - i - 1;
        /* Room for this name, its separator and a worst-case ", and N more". */
        char more[40];
        const int ml = rest ? snprintf(more, sizeof more, ", and %zu more", rest) : 0;
        const size_t need = strlen(s->flag[i].member) + (i ? 2 : 0) + (size_t)ml;
        if (n + need >= cap) {
            n += (size_t)snprintf(out + n, cap - n, "%sand %zu more", i ? ", " : "", s->flag_total - i);
            return n < cap ? n : cap - 1;
        }
        n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "", s->flag[i].member);
    }
    if (s->flag_total > s->nflag)
        n += (size_t)snprintf(out + n, cap - n, ", and %zu more", s->flag_total - s->nflag);
    return n < cap ? n : cap - 1;
}
