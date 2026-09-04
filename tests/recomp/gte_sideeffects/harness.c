/* Drives the recompiled probe (asm_probe.py) and checks the GTE data-
 * register side effects the recompiler must emit inline:
 *   - a write to $30 (LZCS) recomputes $31 (LZCR): the count of leading
 *     bits equal to the sign bit, 32 for 0 and 0xFFFFFFFF;
 *   - a write to $28 (IRGB) unpacks the 5-bit fields into IR1-3 ($9-$11)
 *     as value << 7 and keeps only the low 15 bits in $28;
 *   - a plain data register write touches nothing else.
 * Both mtc2 and lwc2 forms are covered. A direct `gte_data[rd] = value`
 * emission passes the control case and fails the other four. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "psx_runtime_internal.h"

static uint8_t ram[0x10000];
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0xFFFF), 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0xFFFF), &v, 4); }

int psx_override_dispatch(CPUState* cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }

void gteprobe_80010000(CPUState*);
void gteprobe_80010040(CPUState*);
void gteprobe_80010080(CPUState*);
void gteprobe_800100C0(CPUState*);
void gteprobe_80010100(CPUState*);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t ref_lzcr(uint32_t v)
{
    uint32_t sign = ((int32_t)v < 0) ? 1u : 0u;
    uint32_t n = 0;
    for (int b = 31; b >= 0 && ((v >> b) & 1u) == sign; b--) n++;
    return n;
}

#define FILL 0xAAAAAAAAu

int main(void)
{
    static CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.read_word = rd32;
    cpu.write_word = wr32;
    cpu.sp = 0x8000F000u;

    static const uint32_t lz[] = { 0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFF0000u,
                                   0x00010000u, 0x40000000u, 0xC0000000u, 0xFFFFFFFEu, 0x0000FFFFu };
    for (size_t i = 0; i < sizeof lz / sizeof *lz; i++) {
        const uint32_t v = lz[i], want = ref_lzcr(v);
        memset(cpu.gte_data, 0xAA, sizeof cpu.gte_data);
        cpu.a0 = v;
        gteprobe_80010000(&cpu);
        CHECK(cpu.gte_data[30] == v, "mtc2 $30: LZCS %08X", v);
        CHECK(cpu.gte_data[31] == want, "mtc2 $30: LZCR for %08X = %u, want %u", v, cpu.gte_data[31], want);
        CHECK(cpu.v0 == want, "mtc2 $30 / mfc2 $31 round trip for %08X", v);
        memset(cpu.gte_data, 0xAA, sizeof cpu.gte_data);
        wr32(0x80001000u, v);
        cpu.a0 = 0x80001000u;
        gteprobe_80010080(&cpu);
        CHECK(cpu.gte_data[30] == v, "lwc2 $30: LZCS %08X", v);
        CHECK(cpu.gte_data[31] == want, "lwc2 $30: LZCR for %08X = %u, want %u", v, cpu.gte_data[31], want);
        CHECK(cpu.v0 == want, "lwc2 $30 / mfc2 $31 round trip for %08X", v);
    }

    static const uint32_t rgb[] = { 0, 0x7FFF, 0x001F, 0x03E0, 0x7C00, 0x1234, 0xFFFF8000u, 0x12345678u };
    for (size_t i = 0; i < sizeof rgb / sizeof *rgb; i++) {
        const uint32_t v = rgb[i];
        const uint32_t r = (v & 0x1Fu) << 7, g = ((v >> 5) & 0x1Fu) << 7, b = ((v >> 10) & 0x1Fu) << 7;
        memset(cpu.gte_data, 0xAA, sizeof cpu.gte_data);
        cpu.a0 = v;
        gteprobe_80010040(&cpu);
        CHECK(cpu.gte_data[28] == (v & 0x7FFFu), "mtc2 $28: IRGB for %08X = %08X", v, cpu.gte_data[28]);
        CHECK(cpu.gte_data[9] == r && cpu.gte_data[10] == g && cpu.gte_data[11] == b,
              "mtc2 $28: IR1-3 for %08X = %X %X %X, want %X %X %X",
              v, cpu.gte_data[9], cpu.gte_data[10], cpu.gte_data[11], r, g, b);
        CHECK(cpu.v0 == r && cpu.v1 == g && cpu.a1 == b, "mtc2 $28 / mfc2 $9-$11 round trip for %08X", v);
        memset(cpu.gte_data, 0xAA, sizeof cpu.gte_data);
        wr32(0x80001004u, v);
        cpu.a0 = 0x80001000u;
        gteprobe_800100C0(&cpu);
        CHECK(cpu.gte_data[28] == (v & 0x7FFFu), "lwc2 $28: IRGB for %08X = %08X", v, cpu.gte_data[28]);
        CHECK(cpu.gte_data[9] == r && cpu.gte_data[10] == g && cpu.gte_data[11] == b,
              "lwc2 $28: IR1-3 for %08X = %X %X %X, want %X %X %X",
              v, cpu.gte_data[9], cpu.gte_data[10], cpu.gte_data[11], r, g, b);
        CHECK(cpu.v0 == r && cpu.v1 == g && cpu.a1 == b, "lwc2 $28 / mfc2 $9-$11 round trip for %08X", v);
    }

    memset(cpu.gte_data, 0xAA, sizeof cpu.gte_data);
    cpu.a0 = 0x12345678u;
    gteprobe_80010100(&cpu);
    CHECK(cpu.gte_data[9] == 0x12345678u && cpu.v0 == 0x12345678u, "mtc2 $9: plain store");
    for (int r = 0; r < 32; r++)
        if (r != 9) CHECK(cpu.gte_data[r] == FILL, "mtc2 $9: wrote neighbour $%d", r);

    printf("recomp_gte_sideeffects: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
