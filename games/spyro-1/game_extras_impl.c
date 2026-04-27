// games/spyro-1/game_extras_impl.c — Spyro 1's per-game hooks.
//
// Phase 0b fills in only the identity hooks needed for the empty binary's
// window title + future disc validation. Memory layout, overlays, and
// runtime quirks are TODO until Phase 1b.
//
// Disc image: <spyro>/roms/SCUS-94228/SCUS-94228.cue
// Disc BIN SHA256: 95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f
// PSX EXE: SCUS_942.28 (CRC32 over full header+body)

#include "game_extras.h"

#include <stdio.h>

/* --- Identity --- */

uint32_t game_get_entry_point(void) {
    /* From spyro-1/build/PSX.EXE header (initial_pc field). */
    return 0x8005B8E0u;
}

const char* game_get_exe_filename(void) {
    return "SCUS_942.28";
}

uint32_t game_get_expected_crc32(void) {
    /* CRC32 of spyro-1/build/PSX.EXE (full file, 2 KB header + body).
     * Re-compute if the matching decomp's binary changes:
     *   python3 -c "import zlib; print(hex(zlib.crc32(open('build/PSX.EXE','rb').read())))"
     */
    return 0xC56EB4EEu;
}

const char* game_get_display_name(void) {
    return "Spyro the Dragon";
}

/* --- Memory layout (PSX defaults; overrides come in Phase 1b) --- */

uint32_t game_get_ram_size(void)             { return 2u * 1024u * 1024u; }
uint32_t game_get_compiled_range_start(void) { return 0x80010000u; }
uint32_t game_get_compiled_range_end(void)   { return 0x80075800u; } /* load_addr + load_size */
uint32_t game_get_overlay_range_start(void)  { return 0x80100000u; } /* TODO Phase 1b */
uint32_t game_get_overlay_range_end(void)    { return 0x801F0000u; } /* TODO Phase 1b */

/* --- Overlays / quirks / per-frame --- */

int  game_on_overlay_load(uint32_t overlay_id) {
    (void)overlay_id;
    /* TODO Phase 1b — copy overlay data into g_ram, swap dispatcher table. */
    return 0;
}

void game_on_overlay_init(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1b — runs once after WadInit. */
}

void game_apply_runtime_quirks(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1c — preamble fixes, known-bad-instruction workarounds. */
}

void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
    /* TODO — only needed if a per-frame DMA timing hack surfaces. */
}

/* --- Disc layout --- */

static const game_disc_layout_t k_disc_layout = {
    .wad_sector      = 37,            /* Spyro 1 stores WAD.WAD at sector 37 */
    .wad_header_addr = 0x80100000u,   /* TODO Phase 1c — confirm against decomp */
};

const game_disc_layout_t* game_get_disc_layout(void) {
    return &k_disc_layout;
}

/* See docs/missing-geometry-bug.md — Option B (recomp self-instrumentation).
 * On entry to func_80023AC4 (Spyro's renderer, hand-written asm), wrap
 * cpu->write_word with a frame-gated tracer to capture the prim-buffer
 * write stream and diff bad vs good frames. The depth flag prevents the
 * recursive call into the function body from re-entering this branch. */
extern void func_80023AC4(CPUState* cpu);
extern void openpete_trace_writes_begin(CPUState* cpu);
extern void openpete_trace_writes_end(CPUState* cpu);

int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    static int s_in_traced_call = 0;
    if (addr == 0x80023AC4u && !s_in_traced_call) {
        s_in_traced_call = 1;
        openpete_trace_writes_begin(cpu);
        func_80023AC4(cpu);
        openpete_trace_writes_end(cpu);
        s_in_traced_call = 0;
        return 1;
    }
    return 0;
}
