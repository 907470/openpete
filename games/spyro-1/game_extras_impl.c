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
#include "openpete_psx.h"

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

/* --sfx CLI override. -1 = leave alone; 0..10 = forced value (0=OFF). */
static int s_sfx_volume_override = -1;
static int s_sfx_applied = 0;

void game_set_sfx_volume(int value) {
    s_sfx_volume_override = value;
    s_sfx_applied = 0;
}

/* Spyro's "SOUND EFFECTS" slider lives at:
 *   D_80075754             0..10  (0 = OFF as shown in the menu)
 *   g_Spu @ 0x80075F08
 *     +0x320  unk_0x320    intermediate: D_80075754 * 0x3FFF / 10
 *     +0x324  m_SoundVolume intermediate: unk_0x320 * 0x1000 / 0x3FFF
 * SpuInitialize (spu.c:25) writes m_SoundVolume = 0x1000 *before* any
 * sound can be queued (no SPU init = no PlaySound). That's our true
 * gate: as soon as the game has set m_SoundVolume non-zero, override
 * it. This catches the title-screen intro cutscene wind SFX, which
 * keys voices well before gamestates/init.c:549 sets D_80075754 = 10.
 * After the one-shot write the user can still adjust from the options
 * menu — that path also rewrites m_SoundVolume, which we don't fight. */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;
    if (s_sfx_applied || s_sfx_volume_override < 0) return;

    uint32_t cur_sound_volume = cpu->read_word(0x8007622Cu);
    if (cur_sound_volume == 0) return;  /* SpuInitialize hasn't run yet */

    int v = s_sfx_volume_override;
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    uint32_t unk_0x320     = (uint32_t)((v * 0x3FFF) / 10);
    uint32_t m_SoundVolume = (unk_0x320 * 0x1000u) / 0x3FFFu;

    cpu->write_word(0x80075754u, (uint32_t)v);
    cpu->write_word(0x80076228u, unk_0x320);      /* g_Spu.unk_0x320 */
    cpu->write_word(0x8007622Cu, m_SoundVolume);  /* g_Spu.m_SoundVolume */
    s_sfx_applied = 1;
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

/* SpuInitialize at 0x8005595c (spyro-1/build/psx.map). It writes
 * g_Spu.m_SoundVolume = 0x1000 inline (spu.c:25). Run it normally,
 * then stamp our --sfx override immediately on return — that closes
 * the window between m_SoundVolume becoming non-zero and the very
 * next PlaySound (which the title-screen intro fires before any host-
 * side per-frame hook gets a chance to run). */
extern void func_8005595C(CPUState* cpu);  /* SpuInitialize */

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
    static int s_in_spuinit = 0;
    if (addr == 0x8005595Cu && !s_in_spuinit
        && !s_sfx_applied && s_sfx_volume_override >= 0) {
        s_in_spuinit = 1;
        func_8005595C(cpu);
        s_in_spuinit = 0;
        int v = s_sfx_volume_override;
        if (v < 0) v = 0;
        if (v > 10) v = 10;
        uint32_t unk_0x320     = (uint32_t)((v * 0x3FFF) / 10);
        uint32_t m_SoundVolume = (unk_0x320 * 0x1000u) / 0x3FFFu;
        cpu->write_word(0x80075754u, (uint32_t)v);
        cpu->write_word(0x80076228u, unk_0x320);
        cpu->write_word(0x8007622Cu, m_SoundVolume);
        s_sfx_applied = 1;
        return 1;
    }
    return 0;
}
