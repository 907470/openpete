/* core/diag/psxtrace_ratchet.c - the oracle's per-row event ratchet.
 *
 * Three event classes the verifier tolerates on a row only by name: GTE
 * hash events (a compared record interval whose GTE row differs from the
 * reference's), VBL-delta events (a record that opens a VBL deficit
 * against the reference), and gameplay bursts (a divergence in a
 * cd_load_transient region that opens outside a load span). The committed
 * list per row is a table of games/<game>/manifest/oracle_ratchet.toml,
 * keyed by the golden's file name without `.bin`:
 *
 *   [psxtrace-demo-20k]
 *   gte = [1941, 2022]                 # record indices
 *   vbl = [1555]                       # record indices
 *   gameplay_bursts = ["g_Spu@1980"]   # region name @ opening record
 *
 * An event not on the list is new and fails the row; a listed event the run
 * never produced is stale and is reported so the list can be tightened.
 * SDL-free so tests/unit compiles it directly. */

#include "openpete_psxtrace.h"
#include "openpete_log.h"

#include "../../third_party/tomlc99/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int      cls;
    uint32_t rec;
    char     region[OPENPETE_PSXTRACE_REGION_NAME_MAX];
    int      seen;
} ratchet_entry_t;

static const char* const k_class_key[OPENPETE_RATCHET_CLASSES] = {"gte", "vbl", "gameplay_bursts"};

static struct {
    int              armed;
    char             key[128];
    ratchet_entry_t* e;
    uint32_t         n, cap;
    uint32_t         events[OPENPETE_RATCHET_CLASSES];
    uint32_t         fresh[OPENPETE_RATCHET_CLASSES];
} s_rt;

/** @brief Append one listed entry to the ratchet table.
 *
 * @param cls Event class.
 * @param rec Record index.
 * @param region Region name for a gameplay burst, else NULL.
 * @return 0 on success, -1 when the table cannot grow. */
static int ratchet_add(int cls, uint32_t rec, const char* region) {
    if (s_rt.n == s_rt.cap) {
        const uint32_t cap = s_rt.cap ? s_rt.cap * 2u : 64u;
        ratchet_entry_t* e = (ratchet_entry_t*)realloc(s_rt.e, cap * sizeof(*e));
        if (!e) return -1;
        s_rt.e = e;
        s_rt.cap = cap;
    }
    ratchet_entry_t* x = &s_rt.e[s_rt.n++];
    memset(x, 0, sizeof(*x));
    x->cls = cls;
    x->rec = rec;
    if (region) snprintf(x->region, sizeof(x->region), "%s", region);
    return 0;
}

/** @brief Parse one class's array of a ratchet table into entries.
 *
 * `gte` and `vbl` hold integers; `gameplay_bursts` holds "REGION@RECORD"
 * strings. A missing array is an empty list.
 * @param tab The row's table.
 * @param cls Event class whose array is read.
 * @return 0 on success, -1 on a malformed element. */
static int ratchet_parse_class(const toml_table_t* tab, int cls) {
    const toml_array_t* arr = toml_array_in(tab, k_class_key[cls]);
    if (!arr) return 0;
    for (int i = 0; i < toml_array_nelem(arr); i++) {
        if (cls != OPENPETE_RATCHET_BURST) {
            const toml_datum_t d = toml_int_at(arr, i);
            if (!d.ok || d.u.i < 0) return -1;
            if (ratchet_add(cls, (uint32_t)d.u.i, NULL)) return -1;
            continue;
        }
        toml_datum_t d = toml_string_at(arr, i);
        if (!d.ok) return -1;
        char* at = strrchr(d.u.s, '@');
        int bad = !at || at == d.u.s || at[1] == 0;
        if (!bad) {
            *at = 0;
            bad = ratchet_add(cls, (uint32_t)strtoul(at + 1, NULL, 10), d.u.s) != 0;
        }
        free(d.u.s);
        if (bad) return -1;
    }
    return 0;
}

void openpete_ratchet_reset(void) {
    free(s_rt.e);
    memset(&s_rt, 0, sizeof(s_rt));
}

int openpete_ratchet_load(const char* file, const char* key) {
    openpete_ratchet_reset();
    if (!file || !*file) return 0;
    s_rt.armed = 1;
    snprintf(s_rt.key, sizeof(s_rt.key), "%s", key && *key ? key : "-");
    if (strcmp(file, "strict") == 0) {
        OPL_INFO_F("psxtrace", "ratchet: strict, every event fails");
        return 0;
    }
    FILE* fp = fopen(file, "r");
    if (!fp) {
        OPL_ERROR_F("psxtrace", "ratchet: %s is unreadable", file);
        return -1;
    }
    char err[200];
    toml_table_t* root = toml_parse_file(fp, err, sizeof(err));
    fclose(fp);
    if (!root) {
        OPL_ERROR_F("psxtrace", "ratchet: %s: %s", file, err);
        return -1;
    }
    const toml_table_t* tab = key ? toml_table_in(root, key) : NULL;
    int rc = 0;
    for (int c = 0; tab && c < OPENPETE_RATCHET_CLASSES && rc == 0; c++) rc = ratchet_parse_class(tab, c);
    toml_free(root);
    if (rc) {
        OPL_ERROR_F("psxtrace", "ratchet: %s [%s]: malformed list", file, s_rt.key);
        return -1;
    }
    OPL_INFO_F("psxtrace", "ratchet: %s [%s]: %u listed event(s)%s", file, s_rt.key, s_rt.n,
               tab ? "" : " (no table: every event fails)");
    return 0;
}

int openpete_ratchet_armed(void) { return s_rt.armed; }

int openpete_ratchet_note(int cls, uint32_t rec, const char* region) {
    if (cls < 0 || cls >= OPENPETE_RATCHET_CLASSES) return 0;
    s_rt.events[cls]++;
    for (uint32_t i = 0; i < s_rt.n; i++) {
        ratchet_entry_t* x = &s_rt.e[i];
        if (x->cls != cls || x->rec != rec) continue;
        if (cls == OPENPETE_RATCHET_BURST && strcmp(x->region, region ? region : "") != 0) continue;
        x->seen = 1;
        return 1;
    }
    if (!s_rt.armed) return 1;
    s_rt.fresh[cls]++;
    return 0;
}

uint32_t openpete_ratchet_report(int complete) {
    uint32_t stale = 0;
    for (uint32_t i = 0; complete && i < s_rt.n; i++) {
        const ratchet_entry_t* x = &s_rt.e[i];
        if (x->seen) continue;
        stale++;
        if (x->cls == OPENPETE_RATCHET_BURST)
            OPL_WARN_F("psxtrace", "ratchet: stale %s entry \"%s@%u\": no such burst on this run; tighten the list",
                       k_class_key[x->cls], x->region, x->rec);
        else
            OPL_WARN_F("psxtrace", "ratchet: stale %s entry %u: no such event on this run; tighten the list",
                       k_class_key[x->cls], x->rec);
    }
    char st[16] = "-";
    if (complete) snprintf(st, sizeof(st), "%u", stale);
    OPL_INFO_F("psxtrace", "ratchet: key=%s armed=%d gte events=%u vbl events=%u gameplay bursts=%u; new=%u stale=%s",
               s_rt.armed ? s_rt.key : "-", s_rt.armed, s_rt.events[OPENPETE_RATCHET_GTE],
               s_rt.events[OPENPETE_RATCHET_VBL], s_rt.events[OPENPETE_RATCHET_BURST],
               s_rt.fresh[OPENPETE_RATCHET_GTE] + s_rt.fresh[OPENPETE_RATCHET_VBL] + s_rt.fresh[OPENPETE_RATCHET_BURST],
               st);
    return stale;
}

int openpete_ratchet_gte_carry(int prev_ops_mismatch, int d_ops, int d_hash, int d_written) {
    return prev_ops_mismatch && !d_ops && !d_written && d_hash;
}

int openpete_ratchet_vbl_event(int scheduled, int64_t deficit_before, int64_t d_trace, int64_t d_op) {
    if (d_trace == d_op) return 0;
    return !scheduled || deficit_before == 0;
}

int openpete_ratchet_span_open(int read_issued, uint32_t load_bursts_open) {
    return read_issued || load_bursts_open > 0u;
}
