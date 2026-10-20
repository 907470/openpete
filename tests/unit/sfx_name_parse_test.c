/* tests/unit/sfx_name_parse_test.c - op_sfx_name_parse classifies sound
 * pack file names: the ".wav" and "-flat" suffixes in any case, the hash
 * form with and without a pitch tag and with a label, a 16-hex word inside
 * a label, the symbolic form and its 64-byte limit, and a round trip
 * through op_sfx_name_format.
 *
 * sfx_ident.c is compiled with PsyCross's include/psx directory on the
 * include path, as in openpete_core, where that directory's strings.h is
 * found for <strings.h>; the case-insensitive suffix matches go through
 * strcasecmp and strncasecmp. */
#include "sfx_ident.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do {                                                \
    if (!(cond)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++;                                                   \
    }                                                                   \
} while (0)

/** @brief Parse name and check kind, hash, pitch and flat; hash and pitch are checked only for the hash form. */
static void expect(const char* name, int kind, uint64_t hash, int pitch, int flat)
{
    op_sfx_name n;
    const int r = op_sfx_name_parse(name, &n);
    if (r != kind || n.kind != kind || n.flat != flat ||
        (kind == OP_SFX_NAME_HASH && (n.hash != hash || n.pitch != pitch))) {
        fprintf(stderr, "FAIL %s: kind %d/%d hash %016llx pitch %d flat %d\n",
                name, r, n.kind, (unsigned long long)n.hash, n.pitch, n.flat);
        g_failures++;
    }
}

/** @brief Run every case and return 1 when any failed. */
int main(void)
{
    const uint64_t h = 0x0123456789abcdefull;

    /* Not a .wav, or too short to hold one. */
    expect("", OP_SFX_NAME_NONE, 0, -1, 0);
    expect(".wav", OP_SFX_NAME_NONE, 0, -1, 0);
    expect("chime.ogg", OP_SFX_NAME_NONE, 0, -1, 0);
    expect("chime.wav.bak", OP_SFX_NAME_NONE, 0, -1, 0);

    /* The extension and the qualifier match in any case. */
    expect("0123456789abcdef.wav", OP_SFX_NAME_HASH, h, -1, 0);
    expect("0123456789abcdef.WAV", OP_SFX_NAME_HASH, h, -1, 0);
    expect("0123456789abcdef.WaV", OP_SFX_NAME_HASH, h, -1, 0);
    expect("0123456789abcdef-flat.wav", OP_SFX_NAME_HASH, h, -1, 1);
    expect("0123456789abcdef-FLAT.Wav", OP_SFX_NAME_HASH, h, -1, 1);

    /* Hash form: upper-case hex, a pitch tag, a label, both. */
    expect("0123456789ABCDEF.wav", OP_SFX_NAME_HASH, h, -1, 0);
    expect("0123456789abcdef-p1000.wav", OP_SFX_NAME_HASH, h, 0x1000, 0);
    expect("chime-0123456789abcdef.wav", OP_SFX_NAME_HASH, h, -1, 0);
    expect("gem-chime-0123456789abcdef-p0a3F-flat.wav", OP_SFX_NAME_HASH, h, 0xa3f, 1);

    /* A 16-hex word followed by more label text is skipped; the later run
     * that ends the stem is taken. */
    expect("fedcba9876543210-x-0123456789abcdef.wav", OP_SFX_NAME_HASH, h, -1, 0);
    /* A run not preceded by '-' is not a hash. */
    expect("x0123456789abcdef.wav", OP_SFX_NAME_SYMBOLIC, 0, -1, 0);
    /* A malformed pitch tag leaves the name symbolic. */
    expect("0123456789abcdef-p10.wav", OP_SFX_NAME_SYMBOLIC, 0, -1, 0);
    expect("0123456789abcdef-pzzzz.wav", OP_SFX_NAME_SYMBOLIC, 0, -1, 0);

    /* Symbolic form: the stem without ".wav" and "-flat". */
    {
        op_sfx_name n;
        CHECK(op_sfx_name_parse("SND_GEM_CHIME-Flat.WAV", &n) == OP_SFX_NAME_SYMBOLIC);
        CHECK(strcmp(n.name, "SND_GEM_CHIME") == 0);
        CHECK(n.flat == 1);
        CHECK(n.pitch == -1);
    }

    /* A symbolic stem of 63 bytes fits; 64 does not. */
    {
        char name[80];
        op_sfx_name n;
        memset(name, 'a', 63);
        strcpy(name + 63, ".wav");
        CHECK(op_sfx_name_parse(name, &n) == OP_SFX_NAME_SYMBOLIC);
        CHECK(strlen(n.name) == 63);
        memset(name, 'a', 64);
        strcpy(name + 64, ".wav");
        CHECK(op_sfx_name_parse(name, &n) == OP_SFX_NAME_NONE);
    }

    /* op_sfx_name_format output parses back to the same hash and pitch. */
    {
        char buf[96];
        op_sfx_name n;
        CHECK(op_sfx_name_format(buf, sizeof buf, "gem", h, 0x0800) == 0);
        CHECK(strcmp(buf, "gem-0123456789abcdef-p0800.wav") == 0);
        CHECK(op_sfx_name_parse(buf, &n) == OP_SFX_NAME_HASH);
        CHECK(n.hash == h && n.pitch == 0x0800 && n.flat == 0);
        CHECK(op_sfx_name_format(buf, sizeof buf, NULL, h, -1) == 0);
        CHECK(strcmp(buf, "0123456789abcdef.wav") == 0);
        CHECK(op_sfx_name_format(buf, 20, NULL, h, -1) == -1);
    }

    if (g_failures) {
        fprintf(stderr, "sfx_name_parse_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("sfx_name_parse_test: PASS\n");
    return 0;
}
