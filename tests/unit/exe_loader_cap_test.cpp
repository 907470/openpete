/* tests/unit/exe_loader_cap_test.cpp - the disc EXE loader refuses a
 * directory size no PSX EXE can have before it allocates.
 *
 * Writes an 18-sector 2048-byte-sector ISO9660 image whose root directory
 * lists SCUS_942.28 with size 0xFFFFFFF0; the loader must return NULL. A
 * file one byte over 2 MiB + 2048 whose bytes are all present is refused
 * (the previous loader read it whole); a 4-byte file loads. */
#include "openpete_psx.h"
#include "openpete_log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/** @brief Logger stub. */
extern "C" void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Store a both-endian ISO9660 733 field (little-endian half used by the reader). */
static void put733(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}

/** @brief Write one directory record at p for name with lba and size.
 * @return The record length. */
static int dir_record(uint8_t* p, const char* name, uint32_t lba, uint32_t size, uint8_t flags) {
    const int nl = (int)std::strlen(name);
    const int len = 33 + nl + ((33 + nl) & 1);
    p[0] = (uint8_t)len;
    put733(p + 2, lba);
    put733(p + 10, size);
    p[25] = flags;
    p[32] = (uint8_t)nl;
    std::memcpy(p + 33, name, (size_t)nl);
    return len;
}

/** @brief Write an image at path whose root holds SCUS_942.28;1 of the given directory size at sector 18. */
static void write_image(const std::string& path, uint32_t exe_size) {
    /* The body sector holds up to 2 MiB + 2049 bytes, so a file one byte over
     * the cap reads whole and only the cap refuses it. */
    static uint8_t img[18 * 2048 + OPENPETE_PSX_RAM_SIZE + 2048u + 2048u];
    std::memset(img, 0, sizeof img);
    uint8_t* pvd = img + 16 * 2048;
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "\0", 17, 2048, 2);
    uint8_t* root = img + 17 * 2048;
    int o = dir_record(root, "\0", 17, 2048, 2);
    o += dir_record(root + o, "\1", 17, 2048, 2);
    dir_record(root + o, "SCUS_942.28;1", 18, exe_size, 0);
    std::memcpy(img + 18 * 2048, "EXE!", 4);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(img, 1, sizeof img, f);
    std::fclose(f);
}

/** @brief Runs the oversize and valid cases.
 * @return 0 when both pass. */
int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/exe_loader_cap_test.iso";
    int fail = 0;
    size_t n = 0;

    write_image(path, 0xFFFFFFF0u);
    uint8_t* b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (b) { std::fprintf(stderr, "FAIL oversize EXE loaded (%zu bytes)\n", n); std::free(b); fail++; }

    write_image(path, OPENPETE_PSX_RAM_SIZE + 2048u + 1u);
    b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (b) { std::fprintf(stderr, "FAIL EXE one byte over the cap loaded\n"); std::free(b); fail++; }

    write_image(path, 4u);
    b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (!b || n != 4 || std::memcmp(b, "EXE!", 4) != 0) { std::fprintf(stderr, "FAIL valid EXE\n"); fail++; }
    std::free(b);
    std::remove(path.c_str());
    if (fail) return 1;
    std::printf("exe_loader_cap_test: OK\n");
    return 0;
}
