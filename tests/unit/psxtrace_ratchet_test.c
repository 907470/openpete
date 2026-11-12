/* tests/unit/psxtrace_ratchet_test.c - the oracle's event ratchet and its
 * event classification on synthetic inputs.
 *
 * The verifier tolerates three event classes on a row only when the row's
 * table in oracle_ratchet.toml lists them: GTE events, VBL events and
 * gameplay bursts. This test writes a table, feeds events, and checks that
 * a listed event passes, an unlisted one is new, a listed one never fed is
 * stale, an unarmed ratchet fails nothing, a `strict` one fails everything,
 * and that the carry, VBL-event and load-span rules classify a synthetic
 * record sequence as the verifier does.
 */
#include "openpete_psxtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

/** @brief Print a failed check and mark the run failed.
 *
 * @param ok The check's result.
 * @param what What was checked. */
static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        g_fail = 1;
    }
}

/** @brief Check the list lookups: listed events pass, unlisted ones are new, unfed entries are stale.
 *
 * @param path Scratch file the table is written to. */
static void test_lists(const char* path) {
    FILE* fp = fopen(path, "w");
    if (!fp) { check(0, "write the table"); return; }
    fputs("[other]\ngte = [5]\n\n[row]\ngte = [10, 20]\nvbl = [7]\n"
          "gameplay_bursts = [\"g_Spu@100\", \"g_DeltaTime@200\"]\n", fp);
    fclose(fp);
    check(openpete_ratchet_load(path, "row") == 0, "load the row's table");
    check(openpete_ratchet_armed(), "a loaded table arms the ratchet");
    check(openpete_ratchet_note(OPENPETE_RATCHET_GTE, 10, NULL) == 1, "listed GTE event passes");
    check(openpete_ratchet_note(OPENPETE_RATCHET_GTE, 5, NULL) == 0, "another table's GTE event is new");
    check(openpete_ratchet_note(OPENPETE_RATCHET_VBL, 7, NULL) == 1, "listed VBL event passes");
    check(openpete_ratchet_note(OPENPETE_RATCHET_VBL, 10, NULL) == 0, "a GTE record is not a VBL entry");
    check(openpete_ratchet_note(OPENPETE_RATCHET_BURST, 100, "g_Spu") == 1, "listed burst passes");
    check(openpete_ratchet_note(OPENPETE_RATCHET_BURST, 100, "g_DeltaTime") == 0, "a burst in another region is new");
    check(openpete_ratchet_note(OPENPETE_RATCHET_BURST, 101, "g_Spu") == 0, "a burst at another record is new");
    /* gte 20 and g_DeltaTime@200 were never fed. */
    check(openpete_ratchet_report(1) == 2, "two stale entries on a complete run");
    check(openpete_ratchet_report(0) == 0, "no stale entry on a run cut short");

    check(openpete_ratchet_load(path, "absent") == 0, "a missing table loads");
    check(openpete_ratchet_note(OPENPETE_RATCHET_GTE, 10, NULL) == 0, "a missing table lists nothing");

    check(openpete_ratchet_load("strict", "row") == 0, "strict loads");
    check(openpete_ratchet_note(OPENPETE_RATCHET_VBL, 7, NULL) == 0, "strict lists nothing");

    check(openpete_ratchet_load(NULL, NULL) == 0 && !openpete_ratchet_armed(), "no file leaves it unarmed");
    check(openpete_ratchet_note(OPENPETE_RATCHET_GTE, 99, NULL) == 1, "unarmed fails nothing");

    fp = fopen(path, "w");
    if (fp) { fputs("[row]\ngameplay_bursts = [\"g_Spu\"]\n", fp); fclose(fp); }
    check(openpete_ratchet_load(path, "row") == -1, "a burst without @RECORD is refused");
    check(openpete_ratchet_load("/nonexistent/oracle_ratchet.toml", "row") == -1, "an unreadable file is refused");
    openpete_ratchet_reset();
}

/** @brief Check the GTE carry rule on a synthetic run of record mismatches. */
static void test_gte_carry(void) {
    check(openpete_ratchet_gte_carry(1, 0, 1, 0) == 1, "hash-only after op-count is a carry");
    check(openpete_ratchet_gte_carry(0, 0, 1, 0) == 0, "hash-only with no op-count before is an event");
    check(openpete_ratchet_gte_carry(1, 1, 1, 0) == 0, "op-count after op-count is its own event");
    check(openpete_ratchet_gte_carry(1, 0, 1, 1) == 0, "a written-mask difference is its own event");
    /* Records 1..6 as the verifier sees them: {d_ops, d_hash, d_written}.
     * 1 op-count, 2 its carry, 3 hash-only (no op-count at 2: an event),
     * 4 match, 5 op-count, 6 hash-only (a carry). Events: 1, 3, 5. */
    static const int rows[6][3] = {{1, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    int prev_ops = 0, events = 0, carries = 0;
    for (int i = 0; i < 6; i++) {
        const int* d = rows[i];
        const int mism = d[0] || d[1] || d[2];
        if (mism && openpete_ratchet_gte_carry(prev_ops, d[0], d[1], d[2])) carries++;
        else if (mism) events++;
        prev_ops = d[0];
    }
    check(events == 3 && carries == 2, "synthetic GTE run: 3 events, 2 carries");
}

/** @brief Check the VBL-event rule on a long tick charged at the next wait. */
static void test_vbl_events(void) {
    /* Reference 3,1 against openpete 2,2, then 2,2: one event at the first record. */
    static const int64_t tr[4] = {2, 3, 1, 2}, op[4] = {2, 2, 2, 2};
    int64_t deficit = 0;
    int events = 0, mism = 0;
    for (int i = 0; i < 4; i++) {
        events += openpete_ratchet_vbl_event(1, deficit, tr[i], op[i]);
        mism += tr[i] != op[i];
        deficit += tr[i] - op[i];
    }
    check(mism == 2 && events == 1, "a long tick is two mismatching records and one event");
    check(openpete_ratchet_vbl_event(0, 5, 3, 2) == 1, "unscheduled: every mismatch is an event");
}

/** @brief Check the load-span rule over a synthetic record sequence with bursts. */
static void test_span(void) {
    /* Records 0..9: the reference issues a read at 2; a load burst opens at
     * 3 and heals at 6; another region opens at 5 (inside: the load burst
     * is open) and at 8 (outside: no read, no load burst open). */
    uint32_t open_load = 0;
    int inside_at_5 = 0, inside_at_8 = 1, inside_at_3 = 0;
    for (uint32_t rec = 0; rec < 10; rec++) {
        const int span = openpete_ratchet_span_open(rec == 2, open_load);
        if (rec == 2 && span) open_load++;      /* a burst opening with the read */
        if (rec == 3) { inside_at_3 = span; if (span) open_load++; }
        if (rec == 5) { inside_at_5 = span; if (span) open_load++; }
        if (rec == 6 && open_load) open_load -= 2u;  /* the read's and record 3's bursts heal */
        if (rec == 7 && open_load) open_load--;      /* record 5's heals */
        if (rec == 8) inside_at_8 = span;
    }
    check(inside_at_3 && inside_at_5, "bursts while a load burst is open lie inside the span");
    check(!inside_at_8, "a burst after every load burst healed lies outside");
    check(openpete_ratchet_span_open(0, 0) == 0, "no read, no open burst: outside");
}

/** @brief Run every case; exit 0 when all pass.
 *
 * @param argc Unused.
 * @param argv argv[1], when given, is the scratch file for the table.
 * @return 0 on success, 1 on any failed check. */
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "psxtrace_ratchet_test.toml";
    test_lists(path);
    test_gte_carry();
    test_vbl_events();
    test_span();
    remove(path);
    printf("%s\n", g_fail ? "psxtrace_ratchet_test: FAIL" : "psxtrace_ratchet_test: PASS");
    return g_fail;
}
