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

/* --- task #9: physics+die hook (level-1 fall-through diagnosis) -------- */
#include "openpete_log.h"
#include "psx_runtime.h"
extern uint32_t openpete_frame;
extern void* openpete_psx_to_host_ptr(uint32_t);

static int sr32_safe(uint32_t addr, uint32_t* out) {
    if (addr < 0x80000000u || addr >= 0x80200000u) return 0;
    void* p = openpete_psx_to_host_ptr(addr); if (!p) return 0;
    *out = *(uint32_t*)p; return 1;
}

int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    if (!cpu) return 0;
    /* Bracket: only between f1500 and f2500 to avoid noise. The transition
     * from cutscene (gs=14) to death loop (gs=4) reliably happens at f2338. */
    if (openpete_frame < 1500u || openpete_frame > 2500u) return 0;
    const char* tag = NULL;
    switch (addr) {
        case 0x8002C85Cu: tag = "die";     break;
        case 0x8004A200u: tag = "physics"; break;
        default: return 0;
    }
    uint32_t gs=0, sx=0, sy=0, sz=0, surf=0, dt=0, cpx=0, cpy=0, cpz=0;
    sr32_safe(0x800757D8u,             &gs);
    sr32_safe(0x80078A58u +    0u,     &sx);
    sr32_safe(0x80078A58u +    4u,     &sy);
    sr32_safe(0x80078A58u +    8u,     &sz);
    sr32_safe(0x80078A58u + 0xA0u,     &surf);  /* m_surfaceBelowSpyro */
    sr32_safe(0x800756CCu,             &dt);    /* g_DeltaTime */
    sr32_safe(0x800778D8u +    0u,     &cpx);
    sr32_safe(0x800778D8u +    4u,     &cpy);
    sr32_safe(0x800778D8u +    8u,     &cpz);
    OPL_INFO_F("hook",
               "%s f%u gs=%d dt=%d pos=(%d,%d,%d) surfBelow=%d cp=(%d,%d,%d) "
               "ra=0x%08X",
               tag, openpete_frame, (int)gs, (int)dt,
               (int)sx, (int)sy, (int)sz, (int)surf,
               (int)cpx, (int)cpy, (int)cpz, cpu->ra);
    return 0;
}
