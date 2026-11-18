/* probe.c - a PS-EXE that plays GP0 command lists and reads back the VRAM
 * each one wrote.
 *
 * The case list (cases.bin, linked in by crt0.S) is a sequence of records
 * of 32-bit words: n, x, y, w, h, then n GP0 words; n = 0xFFFFFFFF ends the
 * list. Per record the probe sends the n words to GP0, copies the VRAM rect
 * (x, y, w, h) to the CPU through GPUREAD, appends one output record, and
 * fills the rect with 0. A sparse output record is a word holding the count
 * of nonzero halfwords in the rect, then one word per such halfword: the
 * column in bits 0-7, the row in bits 8-15 (both relative to the rect) and
 * the halfword in bits 16-31. With bit 31 of h set (h in bits 0-15) the
 * record is dense: every halfword of the rect in row order, padded to a
 * word. A record with w = 0 reads nothing back and writes no output (a
 * setup record, such as a texture upload). probe_done(start, end) runs once
 * after the last record; the driver dumps main RAM at its entry. */
#include <stdint.h>

#define GP0 (*(volatile uint32_t*)0x1F801810u)
#define GP1 (*(volatile uint32_t*)0x1F801814u)

extern const uint32_t probe_cases[];
extern uint32_t probe_out[];

/** @brief Waits until the GPU accepts a new command word. */
static void wait_cmd(void) { while (!(GP1 & (1u << 26))) { } }

/** @brief Waits until the GPU has VRAM data ready to read. */
static void wait_read(void) { while (!(GP1 & (1u << 27))) { } }

/** @brief Fills a VRAM rect with 0.
 * @param x Left column, a multiple of 16.
 * @param y Top row.
 * @param w Width, a multiple of 16.
 * @param h Height. */
static void fill0(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    wait_cmd();
    GP0 = 0x02000000u;
    GP0 = (y << 16) | x;
    GP0 = (h << 16) | w;
}

/** @brief Marks the end of the run; the driver dumps main RAM at this function's entry.
 * @param start The first output word.
 * @param end One past the last output word. */
__attribute__((noinline)) void probe_done(uint32_t* start, uint32_t* end)
{
    __asm__ volatile("" : : "r"(start), "r"(end) : "memory");
}

/** @brief Resets the GPU, plays every case and reads back each one's rect. */
void probe_main(void)
{
    GP1 = 0x00000000u;              /* reset */
    GP1 = 0x01000000u;              /* reset command buffer */
    wait_cmd(); GP0 = 0xE1000400u;  /* draw mode: page 0, no dither, drawing to the display area allowed */
    wait_cmd(); GP0 = 0xE2000000u;  /* texture window: none */
    wait_cmd(); GP0 = 0xE3000000u;  /* drawing area top-left (0, 0) */
    wait_cmd(); GP0 = 0xE4000000u | (511u << 10) | 1023u; /* bottom-right (1023, 511) */
    wait_cmd(); GP0 = 0xE5000000u;  /* drawing offset (0, 0) */
    wait_cmd(); GP0 = 0xE6000000u;  /* mask: no set, no check */
    fill0(0, 0, 512, 512);
    const uint32_t* c = probe_cases;
    uint32_t* o = probe_out;
    for (;;) {
        const uint32_t n = c[0];
        if (n == 0xFFFFFFFFu) break;
        const uint32_t x = c[1], y = c[2], w = c[3], h = c[4] & 0xFFFFu, dense = c[4] >> 31;
        c += 5;
        wait_cmd();
        for (uint32_t i = 0; i < n; i++) GP0 = c[i];
        c += n;
        if (!w) continue;
        wait_cmd();
        GP0 = 0xC0000000u;
        GP0 = (y << 16) | x;
        GP0 = (h << 16) | w;
        wait_read();
        if (dense) {
            for (uint32_t i = 0; i < (w * h + 1) / 2; i++) *o++ = GP0;
            fill0(x, y, w, h);
            continue;
        }
        uint32_t* cnt = o++;
        uint32_t k = 0;
        const uint32_t total = w * h;
        uint32_t word = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (!(i & 1u)) word = GP0;
            const uint32_t hw = (i & 1u) ? (word >> 16) : (word & 0xFFFFu);
            if (hw) {
                const uint32_t rx = i % w, ry = i / w;
                *o++ = rx | (ry << 8) | (hw << 16);
                k++;
            }
        }
        *cnt = k;
        fill0(x, y, w, h);
    }
    probe_done(probe_out, o);
    for (;;) { }
}
