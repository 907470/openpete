/* tests/unit/s2_grid_relink_test.c - Spyro 2's moby grid relink
 * (func_80041538, asm/updatemobys.s) run as recompiled against the port's
 * bus with a moby moved to a negative X.
 *
 * The relink computes the bucket index with unsigned shifts,
 * (x >> 13) + (y >> 13) * 32, stores it in the moby as a halfword (+0x34)
 * and indexes the grid with the full-width value. At x < 0 the full index
 * exceeds the grid by 0x80000 entries, 2 MB, so the bucket head is read and
 * written through the RAM mirror; the next relink unlinks through the
 * halfword index, walking the bucket's chain (loop header 0x80041634) with
 * no end-of-list test until it finds the moby.
 *
 * The function body is copied verbatim from the generated PSX.EXE_full.c
 * (extract_recomp_function.py). Built with the mirror (psx_span_test's
 * cpu.c), the moby moves (271, 40154) -> (-181, 41045) -> (-588, 41958)
 * with flag 2 and ends in bucket 159. Built with OPENPETE_PSX_RAM_MIRROR_END
 * at 2 MB, the mirror-band store is dropped and the third relink's walk
 * never ends; the loop poll stub stops it after RELINK_POLL_CAP iterations,
 * and that build passes only when it does. */

#include "openpete_psx.h"
#include "openpete_log.h"
#include "psx_runtime_internal.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(OPENPETE_PSX_RAM_MIRROR_END) && OPENPETE_PSX_RAM_MIRROR_END <= 0x00200000u
#define EXPECT_MIRROR 0
#else
#define EXPECT_MIRROR 1
#endif

#define GRID_PTR_ADDR   0x80067174u   /* D_80067174: the grid pointer the relink loads */
#define GRID_BASE       0x80165768u   /* grid base of the Ripto's Arena hang session */
#define MOBY            0x80155444u   /* the class 0x2AF moby of that session */
#define GRID_BUCKETS    1024u
#define WALK_HEADER     0x80041634u
#define RELINK_POLL_CAP 100000u

void func_80041538(CPUState* cpu);

/** @brief Stub CD-ROM register read: every register is unmapped here. */
int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub CD-ROM register write: every register is unmapped here. */
int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value) { (void)phys; (void)value; return 0; }
/** @brief Stub BIOS ROM window: no byte of it is mapped here. */
int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub logger sink. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) { (void)level; (void)subsystem; (void)fmt; }

/** @brief Entry hook stub: no hook handles a call, so every recompiled body runs. */
int psx_override_dispatch(CPUState* cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }

static jmp_buf  s_trip;
static uint32_t s_polls;
static uint32_t s_trip_header;
static uint32_t s_mirror_band;
static int      s_callee_ran;
static int      s_fail;
static CPUState s_cpu;

/** @brief Loop poll stub: counts back-edges and leaves the walk by longjmp after RELINK_POLL_CAP of them. */
void psx_loop_poll(CPUState* cpu, uint32_t header, uint32_t insns, uint32_t pure) {
    (void)cpu; (void)insns; (void)pure;
    if (++s_polls >= RELINK_POLL_CAP) {
        s_trip_header = header;
        longjmp(s_trip, 1);
    }
}

/** @brief Stub for the flag-4 callee (func_8001B6C8): records that it ran, which no flag-2 relink does. */
void func_8001B6C8(CPUState* cpu) { (void)cpu; s_callee_ran = 1; }

static uint32_t (*s_read_word)(uint32_t);
static void     (*s_write_word)(uint32_t, uint32_t);
static uint16_t (*s_read_half)(uint32_t);
static void     (*s_write_half)(uint32_t, uint16_t);

/** @brief Count an access whose physical address lies in the RAM mirror band [2 MB, 8 MB). */
static void note_band(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x00200000u && phys < 0x00800000u) s_mirror_band++;
}

/** @brief Counting word load: notes a mirror-band address, then calls the bus. */
static uint32_t counted_read_word(uint32_t addr) { note_band(addr); return s_read_word(addr); }
/** @brief Counting word store: notes a mirror-band address, then calls the bus. */
static void counted_write_word(uint32_t addr, uint32_t v) { note_band(addr); s_write_word(addr, v); }
/** @brief Counting halfword load: notes a mirror-band address, then calls the bus. */
static uint16_t counted_read_half(uint32_t addr) { note_band(addr); return s_read_half(addr); }
/** @brief Counting halfword store: notes a mirror-band address, then calls the bus. */
static void counted_write_half(uint32_t addr, uint16_t v) { note_band(addr); s_write_half(addr, v); }

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); s_fail++; } } while (0)

/** @brief Read the main-RAM word of a KSEG0 address without the bus or the counters. */
static uint32_t ram_word(uint32_t addr) {
    uint32_t v;
    memcpy(&v, openpete_psx_get_ram() + (addr & 0x1FFFFFu), 4);
    return v;
}

/** @brief Write the main-RAM word of a KSEG0 address without the bus or the counters. */
static void ram_set_word(uint32_t addr, uint32_t v) {
    memcpy(openpete_psx_get_ram() + (addr & 0x1FFFFFu), &v, 4);
}

/** @brief Address of grid bucket @p i in main RAM. */
static uint32_t bucket(uint32_t i) { return GRID_BASE + 4u * i; }

/** @brief Clear RAM and link the moby alone in the bucket of (271, 40154), bucket 128, as the game's grid holds it. */
static void setup(void) {
    memset(openpete_psx_get_ram(), 0, OPENPETE_PSX_RAM_SIZE);
    ram_set_word(GRID_PTR_ADDR, GRID_BASE);
    ram_set_word(bucket(128u), MOBY);
    ram_set_word(MOBY + 0x4u, 0u);
    ram_set_word(MOBY + 0x34u, 128u);   /* halfword +0x34; +0x36 stays 0 */
}

/** @brief Set the moby's X (+0xC) and Y (+0x10) and run the recompiled relink with flag 2.
 *
 * @return 0 when the function returned, 1 when the loop poll stub stopped the walk. */
static int relink(int32_t x, int32_t y) {
    ram_set_word(MOBY + 0xCu, (uint32_t)x);
    ram_set_word(MOBY + 0x10u, (uint32_t)y);
    s_cpu.a0 = MOBY;
    s_cpu.a1 = 2u;
    s_cpu.ra = 0u;
    if (setjmp(s_trip)) return 1;
    func_80041538(&s_cpu);
    return 0;
}

/** @brief Count the grid buckets whose head word in main RAM is nonzero. */
static uint32_t occupied_buckets(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < GRID_BUCKETS; i++) n += ram_word(bucket(i)) != 0u;
    return n;
}

/** @brief Run the three relinks and check the outcome the build's mirror setting predicts.
 *
 * @return 0 when every check holds, else 1. */
int main(void) {
    openpete_psx_runtime_init(&s_cpu);
    s_read_word = s_cpu.read_word;   s_cpu.read_word = counted_read_word;
    s_write_word = s_cpu.write_word; s_cpu.write_word = counted_write_word;
    s_read_half = s_cpu.read_half;   s_cpu.read_half = counted_read_half;
    s_write_half = s_cpu.write_half; s_cpu.write_half = counted_write_half;
    setup();

    const int32_t pos[3][2] = { { 271, 40154 }, { -181, 41045 }, { -588, 41958 } };
    int tripped = 0;
    for (int i = 0; i < 3 && !tripped; i++) tripped = relink(pos[i][0], pos[i][1]);
    const uint32_t head159 = ram_word(bucket(159u));
    const uint32_t index = ram_word(MOBY + 0x34u) & 0xFFFFu;
    printf("s2_grid_relink: mirror %s, walk %s, polls %u, header 0x%08X, mirror-band accesses %u, "
           "bucket 159 head 0x%08X, moby index %u, occupied buckets %u\n",
           EXPECT_MIRROR ? "on" : "off", tripped ? "stopped at the cap" : "ended", s_polls, s_trip_header,
           s_mirror_band, head159, index, occupied_buckets());

    CHECK(!s_callee_ran, "the flag-4 callee did not run");
    CHECK(index == 159u, "the moby records bucket 159");
    /* Each relink to x < 0 loads and stores one bucket head in the mirror band: the second and the
     * third with the mirror, the second alone without it (the third never leaves its walk). */
    CHECK(s_mirror_band == (EXPECT_MIRROR ? 4u : 2u), "mirror-band bucket accesses");
    if (EXPECT_MIRROR) {
        CHECK(!tripped && s_polls == 0u, "every walk ends with no back-edge (the moby heads its bucket)");
        CHECK(head159 == MOBY, "bucket 159 holds the moby");
        CHECK(ram_word(MOBY + 0x4u) == 0u, "the moby ends its chain");
        CHECK(ram_word(bucket(128u)) == 0u, "bucket 128 no longer holds the moby");
        CHECK(occupied_buckets() == 1u, "no other bucket is written");
    } else {
        CHECK(tripped && s_trip_header == WALK_HEADER && s_polls == RELINK_POLL_CAP,
              "without the mirror the third relink's unlink walk at 0x80041634 never ends");
        CHECK(head159 == 0u, "the mirror-band store was dropped, bucket 159 is empty");
    }
    if (s_fail) {
        fprintf(stderr, "s2_grid_relink: %d check(s) failed\n", s_fail);
        return 1;
    }
    printf("s2_grid_relink: PASS\n");
    return 0;
}
