/* vstream_test — the moby vertex-stream resolver (core/psx/vstream.c).
 *
 * Pins the by-construction claims the stream-handle design rests on:
 *   - stock field values (>= the handle bound) pass through bit-identical,
 *     doubled on the halved path, exactly as the game's shift pair does;
 *   - a field value below the bound is a handle into the table, in BOTH
 *     encodings (a Spyro word with handle h must not resolve table[2h]);
 *   - handle 0 and an unbound handle resolve to 0 and are counted, never
 *     silently mapped;
 *   - a bound entry never rebinds to a different vaddr;
 *   - a KUSEG alias of a registered region (a top-bit-masked pointer, as
 *     the game's readers form them) resolves to the region's host bytes
 *     with the remaining length as the bound.
 */
#include "openpete_vstream.h"
#include "psx_runtime.h"
#include "openpete_psx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The resolver reads RAM through openpete_psx_get_ram; give it one. */
static uint8_t s_ram[OPENPETE_PSX_RAM_SIZE];
uint8_t* openpete_psx_get_ram(void) { return s_ram; }
#include "openpete_log.h"
void openpete_logf(openpete_log_level_t level, const char* tag, const char* fmt, ...) { (void)level; (void)tag; (void)fmt; }

int main(void) {
    openpete_vstream_census_t c;

    /* Stock pass-through: the idiom (w << 11) >> 11 and >> 10. */
    CHECK(openpete_vstream_addr(0x1A3000u, 0) == 0x1A3000u);
    CHECK(openpete_vstream_addr(0x0FD800u, 1) == 0x1FB000u);
    CHECK(openpete_vstream_addr(0x8000u, 1) == 0x10000u);   /* lowest halved stock */
    CHECK(openpete_vstream_addr(0x1FFFFFu, 1) == 0x3FFFFEu); /* top of the halved reach */
    CHECK(openpete_vstream_addr(0xFFFFFFFFu, 0) == 0x1FFFFFu); /* masks to 21 bits */
    openpete_vstream_census(&c);
    CHECK(c.stock == 5 && c.handle == 0 && c.unresolved == 0);

    /* Handle 0 and an unbound handle: loud, counted, resolve to 0. */
    CHECK(openpete_vstream_addr(0u, 0) == 0u);
    CHECK(openpete_vstream_addr(0x7FFFu, 1) == 0u);
    openpete_vstream_census(&c);
    CHECK(c.unresolved == 2 && c.first_unresolved == 0u);

    /* Binding: index 0 and the bound itself are refused; entries stick. */
    CHECK(openpete_vstream_table_set(0u, 0x80700000u) == 0);
    CHECK(openpete_vstream_table_set(VSTREAM_HANDLE_BOUND, 0x80700000u) == 0);
    CHECK(openpete_vstream_table_set(7u, 0x80700100u) == 1);
    CHECK(openpete_vstream_table_set(7u, 0x80700100u) == 1);   /* same vaddr: fine */
    CHECK(openpete_vstream_table_set(7u, 0x80700200u) == 0);   /* rebind: refused */
    CHECK(openpete_vstream_table_get(7u) == 0x80700100u);
    CHECK(openpete_vstream_table_find(0x80700100u) == 7u);
    CHECK(openpete_vstream_table_find(0x80700200u) == 0u);

    /* Both encodings index by the raw field value. */
    CHECK(openpete_vstream_addr(7u, 0) == 0x80700100u);
    CHECK(openpete_vstream_addr(7u, 1) == 0x80700100u);
    CHECK(openpete_vstream_addr(14u, 1) == 0u);               /* NOT table[2h] */

    /* Host alias: RAM-resident stock stream. */
    const uint8_t* host = NULL; uint32_t rem = 0;
    s_ram[0x1A3000u] = 0xAB;
    CHECK(openpete_vstream_resolve(0x1A3000u, 0, &host, &rem) == 0x1A3000u);
    CHECK(host == &s_ram[0x1A3000u] && rem == OPENPETE_PSX_RAM_SIZE - 0x1A3000u);

    /* A registered region past 2 MB, reached through a handle, and also
     * through the KUSEG alias a top-bit-masked pointer produces. */
    static uint8_t region[0x1000];
    region[0x10] = 0xCD;
    CHECK(psx_runtime_register_region(0x80700000u, sizeof region, region) == 0);
    CHECK(openpete_vstream_table_set(9u, 0x80700010u) == 1);
    CHECK(openpete_vstream_resolve(9u, 0, &host, &rem) == 0x80700010u);
    CHECK(host == &region[0x10] && rem == sizeof region - 0x10);
    CHECK(openpete_vstream_table_set(10u, 0x00700010u) == 1);   /* KUSEG alias */
    CHECK(openpete_vstream_resolve(10u, 1, &host, &rem) == 0x00700010u);
    CHECK(host == &region[0x10] && *host == 0xCD);
    /* Unmapped: no host alias, but the vaddr still comes back. */
    CHECK(openpete_vstream_table_set(11u, 0x80900000u) == 1);
    CHECK(openpete_vstream_resolve(11u, 0, &host, &rem) == 0x80900000u);
    CHECK(host == NULL && rem == 0);

    if (fails) { fprintf(stderr, "vstream_test: %d FAILED\n", fails); return 1; }
    puts("vstream_test: OK");
    return 0;
}
