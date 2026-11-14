/* bugreport_redact_test.c - the bug-report redactor and redaction pass on a
 * made-up identity (user "zqxu", host "zqxhost", home "/home/zqxu"), given
 * through br_redactor_init_identity so no row depends on the machine's own
 * names.
 *
 * Text: the user and host names are replaced as whole words wherever they
 * occur (`user@host`, `host: name`, a bare word, a dotted host's first
 * label, any letter case); a longer word holding a name is unchanged; a
 * name shorter than three bytes is replaced only as a path component or a
 * `key=` value; the placeholders are not matched again.
 *
 * Pass: a text member whose redaction runs out of memory, a bk2 that does
 * not parse, a text member whose redacted text still holds the home
 * directory, a state image that does not parse and a member whose name holds
 * an identity are dropped and listed with a reason; a bk2's entries are
 * redacted; a binary member holding the home directory ships flagged with
 * its counts.
 *
 * The state-image functions are stubbed: an image starting "BADSTATE" does
 * not parse.
 *
 * Run: bugreport_redact_test (exit 0 = every row passes). */
#include "bugreport_internal.h"

#include "miniz.h"
#include "openpete_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            s_fail++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/** @brief Stub: list no sections, so the scan covers the whole image. */
int openpete_state_image_sections(const uint8_t* img, size_t len, openpete_state_section_t* out,
                                  int max)
{
    (void)img; (void)len; (void)out; (void)max;
    return -1;
}

/** @brief Stub: an image starting "BADSTATE" does not parse; any other rewrites nothing. */
int openpete_state_image_card_paths_set(uint8_t* img, size_t len, const char* port1,
                                        const char* port2)
{
    (void)port1; (void)port2;
    return len >= 8 && !memcmp(img, "BADSTATE", 8) ? -1 : 0;
}

/** @brief Fill a redactor with the test identity: home "/home/zqxu" and "/data/u9", user @p user, host @p host. */
static void ident(br_redactor_t* r, const char* user, const char* host)
{
    br_identity_t id = {{"/home/zqxu", "/data/u9", NULL, NULL}, user, {host, NULL, NULL}, 0};
    br_redactor_init_identity(r, &id);
}

/** @brief Redact @p in and compare with @p want. */
static void text_row(br_redactor_t* r, const char* in, const char* want)
{
    size_t n = 0;
    char* out = br_redact_text(r, in, strlen(in), &n);
    CHECK(out && !strcmp(out, want), "redact \"%s\": got \"%s\", want \"%s\"", in,
          out ? out : "(null)", want);
    free(out);
}

/** @brief The text rows. */
static void test_text(void)
{
    br_redactor_t r;
    ident(&r, "zqxu", "zqxhost");
    text_row(&r, "login zqxu@zqxhost ok", "login <user>@<host> ok");
    text_row(&r, "host: zqxhost", "host: <host>");
    text_row(&r, "user zqxu logged in", "user <user> logged in");
    text_row(&r, "zqxhost.lan up", "<host>.lan up");
    text_row(&r, "ZQXHOST Zqxu", "<host> <user>");
    text_row(&r, "zqxuser zqxhostname my-zqxu zqxu_x", "zqxuser zqxhostname my-zqxu zqxu_x");
    text_row(&r, "open /home/zqxu/file", "open ~/file");
    text_row(&r, "key=zqxu", "key=<user>");
    text_row(&r, "dir p/zqxu/q", "dir p/<user>/q");
    br_redactor_free(&r);

    ident(&r, "zqxu", "zqxhost.lan");
    text_row(&r, "on zqxhost and zqxhost.lan", "on <host> and <host>");
    br_redactor_free(&r);

    ident(&r, "ab", "zqxhost");
    text_row(&r, "ab cab p/ab/q key=ab", "ab cab p/<user>/q key=<user>");
    br_redactor_free(&r);

    ident(&r, "user", "host");
    text_row(&r, "user <user> host", "<user> <user> <host>");
    size_t n = 0;
    char* out = br_redact_text(&r, "user and host", 13, &n);
    CHECK(out && br_scan(&r, (const uint8_t*)out, n, NULL, NULL) == 0,
          "placeholders found by the scan: \"%s\"", out ? out : "(null)");
    free(out);
    br_redactor_free(&r);
}

/** @brief Return the member named @p name, or NULL. */
static const br_member_t* member(const br_members_t* m, const char* name)
{
    for (size_t i = 0; i < m->n; i++)
        if (!strcmp(m->v[i].name, name)) return &m->v[i];
    return NULL;
}

/** @brief Return 1 when the pass dropped @p name with a reason. */
static int dropped(const br_pass_t* s, const char* name)
{
    for (size_t i = 0; i < s->ndrop; i++)
        if (!strcmp(s->drop[i].member, name) && s->drop[i].reason[0]) return 1;
    return 0;
}

/** @brief Add a text member. */
static void add_text(br_members_t* m, const char* name, const char* txt, br_kind_t kind)
{
    br_members_add_copy(m, name, txt, strlen(txt), kind);
}

/** @brief Build a zip holding one entry. */
static void* zip_one(const char* entry, const char* txt, size_t* len)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    void* b = NULL;
    *len = 0;
    if (mz_zip_writer_init_heap(&z, 0, 0) &&
        mz_zip_writer_add_mem(&z, entry, txt, strlen(txt), MZ_DEFAULT_LEVEL))
        mz_zip_writer_finalize_heap_archive(&z, &b, len);
    mz_zip_writer_end(&z);
    return b;
}

/** @brief Report whether a buffer holds the test identity in any form. */
static int holds_identity(const void* d, size_t n)
{
    return br_memmem(d, n, "zqxu", 4) || br_memmem(d, n, "zqxhost", 7) ||
           br_memmem(d, n, "/data/u9", 8);
}

/** @brief The pass rows. */
static void test_pass(void)
{
    br_redactor_t r;
    ident(&r, "zqxu", "zqxhost");
    br_members_t m = {0};
    add_text(&m, "oom.txt", "path /home/zqxu/x\n", BRM_TEXT);
    br_pass_t* s = (br_pass_t*)calloc(1, sizeof *s);
    r.fault_after = 0;
    br_redact_members(&r, &m, s);
    r.fault_after = -1;
    const br_member_t* mb = member(&m, "oom.txt");
    CHECK(!mb || !holds_identity(mb->data, mb->len), "oom.txt shipped unredacted");
    CHECK(dropped(s, "oom.txt"), "oom.txt not listed as dropped");
    br_members_free(&m);
    free(s);

    s = (br_pass_t*)calloc(1, sizeof *s);
    add_text(&m, "bad.bk2", "not a zip /home/zqxu/a zqxu@zqxhost", BRM_BK2);
    size_t zl = 0;
    void* z = zip_one("Header.txt", "Author zqxu@zqxhost\nRom /home/zqxu/x\n", &zl);
    br_members_add_copy(&m, "segment.bk2", z, zl, BRM_BK2);
    free(z);
    add_text(&m, "survivor.txt", "rel x/data/u9/f\n", BRM_TEXT);
    add_text(&m, "log.txt", "zqxu@zqxhost: /home/zqxu/a\n", BRM_TEXT);
    add_text(&m, "press.state", "BADSTATE /home/zqxu/cards/card1.mcd", BRM_STATE);
    add_text(&m, "host-trace.bin", "\x01\x02/home/zqxu/\x03", BRM_BINARY);
    add_text(&m, "mods/x/zqxu.txt", "plain", BRM_AUTHOR);
    br_redact_members(&r, &m, s);

    mb = member(&m, "bad.bk2");
    CHECK(!mb, "bad.bk2 shipped unredacted");
    CHECK(dropped(s, "bad.bk2"), "bad.bk2 not listed as dropped");

    mb = member(&m, "segment.bk2");
    CHECK(mb != NULL, "segment.bk2 dropped");
    if (mb) {
        mz_zip_archive in;
        memset(&in, 0, sizeof in);
        size_t n = 0;
        void* d = mz_zip_reader_init_mem(&in, mb->data, mb->len, 0)
                      ? mz_zip_reader_extract_file_to_heap(&in, "Header.txt", &n, 0) : NULL;
        CHECK(d && !holds_identity(d, n), "segment.bk2 Header.txt: \"%.*s\"", (int)n,
              d ? (const char*)d : "(absent)");
        mz_free(d);
        mz_zip_reader_end(&in);
    }

    mb = member(&m, "survivor.txt");
    CHECK(!mb || !holds_identity(mb->data, mb->len), "survivor.txt shipped holding the home form");
    CHECK(dropped(s, "survivor.txt"), "survivor.txt not listed as dropped");

    mb = member(&m, "log.txt");
    CHECK(mb && !holds_identity(mb->data, mb->len), "log.txt: \"%.*s\"", mb ? (int)mb->len : 0,
          mb ? (const char*)mb->data : "(dropped)");

    CHECK(!member(&m, "press.state"), "press.state that does not parse shipped");
    CHECK(dropped(s, "press.state"), "press.state not listed as dropped");

    CHECK(!member(&m, "mods/x/zqxu.txt"), "member named after the user shipped");
    CHECK(dropped(s, "mods/x/zqxu.txt"), "mods/x/zqxu.txt not listed as dropped");

    CHECK(member(&m, "host-trace.bin") != NULL, "host-trace.bin dropped");
    int flagged = 0;
    for (size_t i = 0; i < s->nflag; i++)
        if (!strcmp(s->flag[i].member, "host-trace.bin") && s->flag[i].hits[BRF_HOME] == 1) flagged = 1;
    CHECK(flagged, "host-trace.bin not flagged with one home finding");
    CHECK(s->drop_total == 4, "drop_total %zu, want 4", s->drop_total);

    br_members_free(&m);
    free(s);
    br_redactor_free(&r);
}

/** @brief Run every row. */
int main(void)
{
    test_text();
    test_pass();
    if (s_fail) {
        printf("bugreport_redact_test: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("bugreport_redact_test: PASS\n");
    return 0;
}
