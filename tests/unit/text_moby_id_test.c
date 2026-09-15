/* text_moby_id_test — the provenance table's generation lifecycle
 * (games/spyro-1/renderer/text_moby_id.c) against a fake CPU and RAM.
 *
 * The emitters are driven through the module's own dispatch table, exactly
 * as psx_override_dispatch would: an entry hook on func_80017FE4 /
 * func_800181AC records the glyphs a call is about to allocate, an entry
 * hook on func_80018880 (the pool drain that ends every draw) closes the
 * generation and, on a second drain with no emit between, clears it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not <assert.h>: the release presets define NDEBUG. */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "text_moby_id_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#include "feature_registry.h"
#include "openpete_subtick_view.h"
#include "psx_runtime.h"

#include "renderer/text_moby_id.h"

extern const feature_module_t text_moby_id_module;

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x00200000u
static uint8_t s_ram[RAM_SIZE];

static uint8_t* ram(uint32_t addr) {
    CHECK(addr >= RAM_BASE && addr - RAM_BASE < RAM_SIZE);
    return s_ram + (addr - RAM_BASE);
}
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ram(a), 4); return v; }
static void     wr32(uint32_t a, uint32_t v) { memcpy(ram(a), &v, 4); }
static void     wr16(uint32_t a, uint16_t v) { memcpy(ram(a), &v, 2); }

static uint32_t fake_read_word(uint32_t a) { return rd32(a); }
static uint8_t  fake_read_byte(uint32_t a) { return *ram(a); }

/* text_moby_id reads the live m_Class through the sub-tick view. */
void* openpete_subtick_host_ptr(uint32_t addr) { return ram(addr); }

#define HUD_MOBYS_PTR 0x80075710u
#define HUD_POOL_BASE 0x800756FCu
#define MOBY_STRIDE   0x58u
#define CLS_OFF       0x36u
#define CLS_LETTER_A  426u

#define EMIT_PLAIN  0x80017FE4u
#define DRAIN       0x80018880u

static feature_dispatch_fn hook(uint32_t addr) {
    for (const feature_dispatch_entry_t* e = text_moby_id_module.dispatch_table; e->addr; e++)
        if (e->addr == addr) return e->fn;
    return NULL;
}

/* Emulate one emitter call: run the entry hook, then do what the emitter
 * does — bump the cursor down one moby per non-space character and write
 * the class the emitter would write. */
static void emit(CPUState* cpu, uint32_t ra, const char* text, uint32_t str_addr) {
    memcpy(ram(str_addr), text, strlen(text) + 1);
    cpu->ra = ra; cpu->a0 = str_addr;
    int r = hook(EMIT_PLAIN)(cpu, EMIT_PLAIN);
    CHECK(r == 0);   /* entry hook: the recompiled body still runs */
    uint32_t cur = rd32(HUD_MOBYS_PTR);
    for (const char* p = text; *p; p++) {
        if (*p == ' ') continue;
        cur -= MOBY_STRIDE;
        memset(ram(cur), 0, MOBY_STRIDE);
        wr16(cur + CLS_OFF, (uint16_t)(CLS_LETTER_A + (*p - 'A')));
    }
    wr32(HUD_MOBYS_PTR, cur);
}

/* Emulate the drain: hook, then the cursor walks back to the pool base. */
static void drain(CPUState* cpu) {
    int r = hook(DRAIN)(cpu, DRAIN);
    CHECK(r == 0);
    wr32(HUD_MOBYS_PTR, rd32(HUD_POOL_BASE));
}

static void new_tick(uint32_t pool_base) {
    wr32(HUD_POOL_BASE, pool_base);
    wr32(HUD_MOBYS_PTR, pool_base);
}

int main(void) {
    CPUState cpu; memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = fake_read_word; cpu.read_byte = fake_read_byte;
    CHECK(hook(EMIT_PLAIN) && hook(DRAIN));

    const uint32_t POOL_A = 0x801A3BB0u, POOL_B = 0x801BFBB0u, STR = 0x80100000u;
    uint32_t key, k0, k1;

    /* Tick 1: one call emits "AB" — glyph j lands at cursor - (j+1)*stride. */
    new_tick(POOL_A);
    emit(&cpu, 0x80010000u, "AB", STR);
    uint32_t a1 = POOL_A - 1u * MOBY_STRIDE, b1 = POOL_A - 2u * MOBY_STRIDE;
    CHECK(text_moby_id_lookup(a1, &k0));
    CHECK(text_moby_id_lookup(b1, &k1));
    CHECK(k0 != k1 && (k0 & 0xF0000000u) == 0x50000000u);
    drain(&cpu);
    /* After the draw's drain the records stay valid: this tick's capture and
     * every present until the next tick read them. */
    CHECK(text_moby_id_lookup(a1, &key) && key == k0);

    /* Tick 2, other polybuf half: the same layout gives the same keys at the
     * new addresses (provenance, not position). */
    new_tick(POOL_B);
    emit(&cpu, 0x80010000u, "AB", STR);
    uint32_t a2 = POOL_B - 1u * MOBY_STRIDE;
    CHECK(text_moby_id_lookup(a2, &key) && key == k0);
    CHECK(!text_moby_id_lookup(a1, &key));   /* previous generation cleared */
    drain(&cpu);

    /* Tick 3: a draw that emits no text but allocates pool mobys itself with
     * a matching class (the credits roll). The class guard alone cannot tell
     * this moby from the stale record at its address; the second drain with
     * no emit between must have cleared the generation. */
    new_tick(POOL_A);
    uint32_t cur = POOL_A - MOBY_STRIDE;
    memset(ram(cur), 0, MOBY_STRIDE);
    wr16(cur + CLS_OFF, CLS_LETTER_A);
    wr32(HUD_MOBYS_PTR, cur);
    drain(&cpu);
    CHECK(!text_moby_id_lookup(a2, &key));
    CHECK(!text_moby_id_lookup(cur, &key));

    /* Tick 4: text returns; a second callsite and a second call from the
     * first callsite get distinct keys. The class guard still rejects a
     * record whose moby now holds another class. */
    new_tick(POOL_B);
    emit(&cpu, 0x80010000u, "A", STR);
    emit(&cpu, 0x80020000u, "A", STR);
    emit(&cpu, 0x80010000u, "A", STR);
    uint32_t g0 = POOL_B - 1u * MOBY_STRIDE, g1 = POOL_B - 2u * MOBY_STRIDE, g2 = POOL_B - 3u * MOBY_STRIDE;
    uint32_t q0, q1, q2;
    CHECK(text_moby_id_lookup(g0, &q0) && text_moby_id_lookup(g1, &q1) && text_moby_id_lookup(g2, &q2));
    CHECK(q0 == k0 && q1 != q0 && q2 != q0 && q2 != q1);
    wr16(g1 + CLS_OFF, CLS_LETTER_A + 1u);
    CHECK(!text_moby_id_lookup(g1, &key));
    drain(&cpu);
    CHECK(text_moby_id_lookup(g0, &key) && key == q0);

    puts("text_moby_id_test: PASS");
    return 0;
}
