/* write_census_test - the write census range table (games/common/write_census.c)
 * against a fake CPU.
 *
 * `ranges` installs a spec of twelve one-word ranges and stores into each
 * range past the eighth, and once outside every range: each in-range store
 * adds a row and every store reaches the wrapped hook. `refuse` installs a
 * spec whose ninth range is malformed and checks that no hook is wrapped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not <assert.h>: the release presets define NDEBUG. */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "write_census_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#ifdef _WIN32
  #define setenv_portable(k, v) _putenv_s((k), (v))
#else
  #define setenv_portable(k, v) setenv((k), (v), 1)
#endif

#include "psx_runtime.h"
#include "spyro_overlay_table.h"
#include "write_census.h"

unsigned openpete_frame = 0;
/** @brief Stub: no overlay is resident. @return 0. */
uint32_t spyro_get_active_overlay_magic(void) { return 0; }
/** @brief Stub: no overlay table. @param magic Unused. @return NULL. */
const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic) { (void)magic; return NULL; }

static unsigned s_stores = 0;
/// @brief Fake word store hook that counts the stores reaching it.
static void fake_write_word(uint32_t a, uint32_t v) { (void)a; (void)v; s_stores++; }
/// @brief Fake halfword store hook that counts the stores reaching it.
static void fake_write_half(uint32_t a, uint16_t v) { (void)a; (void)v; s_stores++; }
/// @brief Fake byte store hook that counts the stores reaching it.
static void fake_write_byte(uint32_t a, uint8_t v)  { (void)a; (void)v; s_stores++; }
/// @brief Fake swl/swr hook that counts the stores reaching it.
static void fake_swlr(uint32_t a, uint32_t v)       { (void)a; (void)v; s_stores++; }

static CPUState s_cpu;

/** @brief Installs the census over the fake CPU with the given spec and output path.
 *
 * @param spec The OPENPETE_WRITE_CENSUS value.
 * @param out The OPENPETE_WRITE_CENSUS_OUT path. */
static void install(const char* spec, const char* out) {
    s_cpu.write_word = fake_write_word;
    s_cpu.write_half = fake_write_half;
    s_cpu.write_byte = fake_write_byte;
    s_cpu.swl = fake_swlr;
    s_cpu.swr = fake_swlr;
    setenv_portable("OPENPETE_WRITE_CENSUS", spec);
    setenv_portable("OPENPETE_WRITE_CENSUS_OUT", out);
    write_census_install(&s_cpu);
}

/** @brief Twelve ranges: stores into ranges 9 to 12 each log a row; an out-of-range store logs none. */
static void test_ranges(const char* out) {
    char spec[512] = "";
    for (int i = 0; i < 12; ++i) {
        char r[40];
        snprintf(r, sizeof r, "%s%08X..%08X", i ? "," : "", 0x80010000u + i * 0x100u, 0x80010003u + i * 0x100u);
        strcat(spec, r);
    }
    install(spec, out);
    CHECK(write_census_active());
    CHECK(s_cpu.write_word != fake_write_word);
    for (int i = 8; i < 12; ++i) s_cpu.write_word(0x80010000u + i * 0x100u, (uint32_t)i);
    s_cpu.write_byte(0x80010B03u, 1);   /* last byte of range 12 */
    s_cpu.write_half(0x80010B04u, 1);   /* one past range 12 */
    CHECK(write_census_rows() == 5);
    CHECK(s_stores == 6);
}

/** @brief A malformed ninth range refuses the whole spec: the hooks stay unwrapped. */
static void test_refuse(const char* out) {
    install("80010000..80010003,80010100..80010103,80010200..80010203,80010300..80010303,"
            "80010400..80010403,80010500..80010503,80010600..80010603,80010700..80010703,"
            "80010800..8001", out);
    CHECK(!write_census_active());
    CHECK(s_cpu.write_word == fake_write_word);
    s_cpu.write_word(0x80010000u, 1);
    CHECK(write_census_rows() == 0);
}

/** @brief Runs the case named by argv[1] with the CSV at argv[2].
 *
 * @param argc Argument count.
 * @param argv `ranges` or `refuse`, then the output path.
 * @return 0 on pass; a failed check exits 1. */
int main(int argc, char** argv) {
    CHECK(argc == 3);
    remove(argv[2]);
    if (strcmp(argv[1], "ranges") == 0) test_ranges(argv[2]);
    else if (strcmp(argv[1], "refuse") == 0) test_refuse(argv[2]);
    else CHECK(!"unknown case");
    printf("write_census_test %s: ok\n", argv[1]);
    return 0;
}
