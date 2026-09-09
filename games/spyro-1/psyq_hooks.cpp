// games/spyro-1/psyq_hooks.c — what Spyro 1 supplies to the shared PSYQ thunk
// layer.
//
// The thunk layer is game-neutral; everything here is not. Three kinds of
// thing live in this file:
//
//  - PSYQ's own data structures, written into PSX RAM at Spyro 1's addresses
//    because gameplay code reads them back and those bytes have to
//    match hardware. The values were recovered by observation from emulator
//    RAM dumps, not derived, and they are specific to PSYQ 4.0 as this game
//    links it.
//  - The stack frame each real PSYQ function leaves behind. An empty C thunk
//    leaves those slots at whatever was there before, which reads as a RAM
//    difference at every call.
//  - Couplings to Spyro 1's own features: its asset overlay, its render-path
//    filter and moby ranking, its sound-effect name tables.

extern "C" {
#include "psyq_game_hooks.h"
#include "openpete_psx.h"
#include "openpete_fiber.h"
#include "openpete_mods.h"
#include "openpete_ticker.h"
}

#include "mod_wad_overlay.h"
#include "features/render_paths.h"
#include "features/moby_rank.h"
#include "sfx_pack.h"
#include "psx/libcd.h"

#include <string.h>

extern "C" void openpete_logf(int level, const char* subsystem, const char* fmt, ...);

#define PTR(a) ((void*)(openpete_psx_get_ram() + ((a) & 0x1FFFFFu)))

/* --- PSYQ prologue frames ----------------------------------------------
 *
 * Each entry is the frame the real PSYQ 4.0 function pushes: its size, where
 * it stores ra, and how many of s0..s4 it saves at 16, 20, 24, 28, 32. */
static const psyq_game_frame_t k_frames[PSYQ_FRAME__COUNT] = {
    [PSYQ_FRAME_MAIN] = { 16u, 12u, 0 },
    [PSYQ_FRAME_RESETGRAPH] = { 32u, 24u, 2 },
    [PSYQ_FRAME_SETDISPMASK] = { 32u, 24u, 2 },
    [PSYQ_FRAME_SETGRAPHDEBUG] = { 24u, 20u, 1 },
    [PSYQ_FRAME_DRAWSYNC] = { 24u, 20u, 1 },
    [PSYQ_FRAME_CLEARIMAGE] = { 40u, 32u, 4 },
    [PSYQ_FRAME_PUTDISPENV] = { 40u, 32u, 4 },
    [PSYQ_FRAME_PUTDRAWENV] = { 32u, 28u, 3 },
    [PSYQ_FRAME_SETDEFDRAWENV] = { 40u, 36u, 5 },
    [PSYQ_FRAME_MEMCARDINIT] = { 0x18u, 0x10u, 0 },
};

extern "C" int psyq_game_stack_frame(psyq_frame_site_t site, psyq_game_frame_t* out) {
    if (site < 0 || site >= PSYQ_FRAME__COUNT) return 0;
    if (k_frames[site].frame_size == 0u) return 0;
    *out = k_frames[site];
    return 1;
}

/* --- PSYQ-internal RAM mirrors ----------------------------------------- */

extern "C" void psyq_game_mirror_main(CPUState* cpu) {
    const uint32_t INIT_FLAG_ADDR = 0x800730BCu;
    if (cpu->read_word(INIT_FLAG_ADDR) == 0u) {
        cpu->write_word(INIT_FLAG_ADDR, 1u);
    }
}

extern "C" void psyq_game_mirror_resetgraph(CPUState* cpu, int mode) {
    int sub = mode & 7;
    if (sub == 0 || sub == 3 || sub == 5) {
        const uint32_t ENV = 0x80074A64u;
        /* memset(ENV, 0, 128) */
        for (uint32_t a = ENV; a < ENV + 128u; a += 4u) cpu->write_word(a, 0u);
        /* sb gpu_type @ENV+0; sb 1 @ENV+1 */
        cpu->write_byte(ENV + 0u, 0u);   /* NTSC */
        cpu->write_byte(ENV + 1u, 1u);
        /* halfwords pulled from PSYQ tables at 0x80074AE4 / 0x80074AF8.
         * For NTSC (gpu_type=0) the table low-halfwords are 0x0400 and
         * 0x0200 — confirmed by xxd of pcsx-redux's RAM dump. */
        cpu->write_half(ENV + 4u, 0x0400u);
        cpu->write_half(ENV + 6u, 0x0200u);
        /* Observed pcsx-redux: byte at ENV+8 (0x80074A6C) = 1. Source not
         * yet localised in disassembly; matched by observation so the diff
         * stays clean. */
        cpu->write_byte(ENV + 8u, 1u);
        /* memset(ENV+16, 0xFF, 92) -> 0x80074A74..0x80074ACF */
        for (uint32_t a = ENV + 16u; a < ENV + 16u + 92u; a += 4u) {
            cpu->write_word(a, 0xFFFFFFFFu);
        }
        /* memset(ENV+108, 0xFF, 20) -> 0x80074AD0..0x80074AE3 */
        for (uint32_t a = ENV + 108u; a < ENV + 108u + 20u; a += 4u) {
            cpu->write_word(a, 0xFFFFFFFFu);
        }
    }

    /* func_80061DEC effects (called by ResetGraph): three word writes plus
     * the BIOS-side gpu-type cache. Hardcoded results for NTSC mode 0:
     * func_8005DF44 returns 0 ((sub==0 || sub==3 || sub==5) only). */
    cpu->write_word(0x80074B6Cu, 0u);
    cpu->write_word(0x80074B68u, 0u);

    /* Post-ResetGraph snapshot from pcsx-redux RAM at MemoryCardInit halt.
     * 60 EXE bytes scattered across libgpu's vtable / dispatch / GPU command
     * scratch areas. Real PSYQ libgpu writes these via a chain of helpers
     * (`func_8005DDC8` -> *(D_800749AC)+0xC, `func_8005DF60` walks the
     * 0x80073924 control struct, `func_80061DEC` updates the helper-table
     * trio at 0x80074B68/6C/78, and the 0x80075978 region holds queued GP0
     * env packets). openpete uses PsyCross stubs and never enters those
     * helpers, so we replay the observed post-init values directly. NTSC
     * mode 0; values verified against pcsx-redux dumps (gp=0x80075264,
     * sp=0x801FFFF8 at boot). */
    if (sub == 0 || sub == 3 || sub == 5) {
        /* libgpu callback control struct at 0x80073924+ */
        cpu->write_word(0x80073924u, 0x00000001u);
        cpu->write_word(0x80073928u, 0x8005E560u);  /* fnptr */
        cpu->write_word(0x80073934u, 0x8005E680u);  /* fnptr */
        cpu->write_word(0x80073954u, 0x00000009u);
        cpu->write_word(0x8007395Cu, 0x8005DFC8u);  /* fnptr */
        cpu->write_word(0x80073960u, 0x8007493Cu);
        cpu->write_word(0x80073964u, 0x801FFFF8u);  /* boot sp */
        cpu->write_word(0x80073968u, 0x80073924u);  /* self ref */
        cpu->write_word(0x80073970u, 0x00000001u);
        cpu->write_word(0x80073988u, 0x80075264u);  /* boot gp */
        /* libgpu vtable patch slots (indirected through D_800749AC) */
        cpu->write_word(0x80074990u, 0x8005E804u);
        cpu->write_word(0x800749A0u, 0x8005E5D8u);
        /* GP0 dispatch state at 0x80074B58+ (set by func_80061F48 in real
         * PSYQ; we cache the post-init values). */
        cpu->write_word(0x80074B58u, 0x80060FD8u);
        cpu->write_word(0x80074B5Cu, 0x801FFF78u);  /* inner sub-call sp */
        cpu->write_word(0x80074B70u, 0x00000009u);
        cpu->write_word(0x80074B78u, 0x00000009u);
        cpu->write_word(0x80074B7Cu, 0x000000F0u);
        /* Pre-built GP0 env packet block at 0x80075978 — pcsxr ends
         * GraphicsInit with these queued; openpete-PsyCross drains
         * commands a different way so the slot stays zero without help. */
        cpu->write_word(0x80075978u, 0x05FFFFFFu);
        cpu->write_word(0x8007597Cu, 0xE6000000u);
        cpu->write_word(0x80075980u, 0xE1000000u);
        cpu->write_word(0x80075984u, 0x02000000u);
        cpu->write_word(0x8007598Cu, 0x01FF0200u);
    }

}

extern "C" void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb) {
    /* Real PSYQ VSyncCallback also stashes the cb pointer in libgpu's
     * vtable at 0x800749D0 (the slot func_8005DE58 reads on each VSync
     * IRQ). PsyCross dispatches the cb host-side, so we mirror the write
     * for byte-faithful diffs. */
    cpu->write_word(0x800749D0u, cpu->a0);
}

extern "C" void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0, uint32_t buf1) {
    /* Real PSYQ PadInitDirect (0x8006A4xx region) sets up libpad's pad-state
     * machine: a 10-entry dispatch table at 0x800751BC (one fnptr per
     * PadStateXxx), plus per-slot bookkeeping at 0x80075C58+ and 0x80075D40+
     * (pointers to actuator buffers, byte masks, status caches). PsyCross's
     * libpad implementation tracks state host-side and never writes those
     * PSX-side bytes, leaving 60+ EXE/data bytes zero on our side. Replay
     * the observed values verbatim — function pointers are PSYQ code (not
     * relocated), the buffer addresses come from a0/a1. */

    /* Pad-state dispatch table @ 0x800751BC (10 PSYQ fnptrs). */
    static const struct { uint32_t off; uint32_t val; } pad_dispatch[] = {
        {0x800751BCu, 0x8006B1D8u},
        {0x800751C0u, 0x8006B170u},
        {0x800751C4u, 0x8006B2DCu},
        {0x800751C8u, 0x8006B398u},
        {0x800751CCu, 0x8006B614u},
        {0x800751D0u, 0x8006B64Cu},
        {0x800751D4u, 0x8006B6D4u},
        {0x800751D8u, 0x8006BAE8u},
        {0x800751DCu, 0x8006B7A8u},
        {0x800751E0u, 0x8006B2CCu},
        {0x800751F0u, 0x80075D18u},
        {0x800751F4u, 0x00000001u},
    };
    for (size_t i = 0; i < sizeof(pad_dispatch)/sizeof(pad_dispatch[0]); i++) {
        cpu->write_word(pad_dispatch[i].off, pad_dispatch[i].val);
    }

    /* Per-pad slot tables. Buffer pointers depend on a0/a1; the rest are
     * PSYQ data. Slot 0 ("g_PadBuffer") writes through 0x80075D40+; slot 1
     * ("g_PadBufferSecondController") through 0x80075E38+. Inner pointers
     * (0x80075CAB / 0x80075CF3 / 0x80075C88 / 0x80075CD0) are PSYQ
     * scratch-buffer locations inside the same data block. */
    cpu->write_word(0x80075C58u, 0x0000B058u);
    cpu->write_word(0x80075C5Cu, 0x8006969Cu);
    cpu->write_word(0x80075C60u, 0x80069634u);
    cpu->write_word(0x80075D28u, 0x80075D18u);
    cpu->write_word(0x80075D40u, buf0);
    cpu->write_word(0x80075D48u, buf1);
    cpu->write_word(0x80075D4Cu, 0x00000002u);
    cpu->write_word(0x80075D54u, 0x80075C88u);
    cpu->write_word(0x80075D58u, 0x80075CD0u);
    cpu->write_word(0x80075D74u, 0xFFFFFF00u);
    cpu->write_word(0x80075D78u, 0x00FFFFFFu);
    cpu->write_word(0x80075E18u, 0x80075E08u);
    cpu->write_word(0x80075E38u, buf1);
    cpu->write_word(0x80075E44u, 0x80075CABu);
    cpu->write_word(0x80075E48u, 0x80075CF3u);
    cpu->write_word(0x80075E64u, 0xFFFFFF00u);
    cpu->write_word(0x80075E68u, 0x00FFFFFFu);
}

extern "C" void psyq_game_mirror_pad_slot2(CPUState* cpu) {
    /* slot-2 buffer: 8 bytes packed as two LE u32 writes. */
    cpu->write_word(0x80078E50u, 0x000000FFu);  /* FF 00 00 00 */
    cpu->write_word(0x80078E54u, 0x00000000u);  /* 00 00 00 00 */
    cpu->write_word(0x80078E9Cu, 0x00000190u);
}

extern "C" void psyq_game_mirror_memcard_init(CPUState* cpu) {
    /* Real PSYQ MemCardInit (0x8006626C) clears two state words then
     * jumps into func_80067E70, which OpenEvent-registers a chain of
     * BIOS events and threads handlers through PSYQ's libgpu-style
     * control struct. The card-side struct lives at 0x800748F0+ and
     * shadows the same ABI as libgpu's at 0x80073924. PsyCross stubs
     * skip the entire chain, so 15 EXE bytes remain zero on our side.
     * Replay the observed post-init values directly. */
    cpu->write_byte(0x80074904u, 0x01u);          /* "card init done" */
    cpu->write_word(0x8007490Cu, 0x8005E124u);    /* fnptr */
    cpu->write_word(0x80074924u, 0x80073924u);    /* libgpu struct backref */
    cpu->write_byte(0x8007492Cu, 0x01u);
    cpu->write_word(0x80074938u, 0x8005DFD8u);    /* fnptr */
    cpu->write_byte(0x800749E0u, 0x05u);          /* registered handler count */


}

/* PSYQ's rand seed, in the library's own data at D_80075AC0. */
extern "C" uint32_t psyq_game_rand_seed_addr(void) { return 0x80075AC0u; }

/* --- Feature couplings -------------------------------------------------- */

extern "C" void psyq_game_cd_setloc(const void* cdl_loc) {
    mod_wad_setloc((uint32_t)CdPosToInt((CdlLOC*)cdl_loc));
}

extern "C" void psyq_game_cd_apply_buf(unsigned char* buf, int sectors) {
    mod_wad_apply((uint8_t*)buf, sectors);
}

/* Every overlay loads at this one vaddr; the id is its first word. */
extern "C" uint32_t psyq_game_overlay_vaddr(void) { return 0x8007AA38u; }

/* Spyro 1 waits on VSync(0), which advances the counter directly, so its poll
 * limit is zero: the poll path advances nothing. */
extern "C" unsigned psyq_game_vsync_poll_advance(void) { return 0u; }

/* Spyro 1 registers its read-completion handler through psyq_CdReadCallback,
 * so the thunk layer holds it and no guest RAM slot names it. */
extern "C" uint32_t psyq_game_cd_read_cb_addr(void) { return 0u; }

/* g_CdState.m_IsReading must be clear once the completion callback has run. */
extern "C" void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb) {
    (void)cpu;
    const uint8_t* ram = (const uint8_t*)openpete_psx_get_ram();
    uint32_t after = 0;
    memcpy(&after, ram + 0x76BB8, 4);
    if (after != 0u) {
        openpete_logf(3, "thunks",
                      "CdRead: m_IsReading still set after callback (after=%u cb=0x%08X)",
                      after, read_cb);
    }
}

extern "C" int  psyq_game_render_paths_active(void) { return render_paths_active(); }
extern "C" int  psyq_game_render_path_addr_visible(uint32_t addr) {
    return render_path_addr_visible(addr);
}
extern "C" void psyq_game_ot_begin(void) { openpete_mobyrank_ot_begin(openpete_frame); }
extern "C" void psyq_game_ot_prim_full(unsigned code_byte, const void* payload,
                            unsigned payload_bytes) {
    openpete_mobyrank_ot_prim(code_byte, (const uint8_t*)payload, payload_bytes);
}
extern "C" void psyq_game_ot_end(void) { openpete_mobyrank_ot_end(); }

#include "spyro1_sfx_names.h" /* generated: all 256 slot names */

/* Mod-facing aliases that predate the generated table; they resolve to
 * the slot name and keep working forever. */
static const struct { const char* alias; uint8_t slot; } k_sfx_aliases[] = {
    {"gem_pickup", 0x00},   {"chest_break", 0x03},
    {"spyro_charge", 0x04}, {"super_charge", 0x05},
    {"exit_vortex", 0x1A},  {"spyro_stars", 0x1B},
    {"water_splash", 0x1D}, {"spyro_walk", 0x1E},
    {"spyro_unsquish", 0x20}, {"flight_pling", 0x21},
    {"flight_collected", 0x23}, {"moby_poof", 0x26},
    {"spyro_flame", 0x28},  {"menu_sound", 0x2C},
    {"menu_cursor", 0x2D},  {"menu_confirm", 0x2E},
    {"pickup_ding", 0x2F},
};
static const char* spyro_sfx_canon(const char* name) {
    for (const auto& a : k_sfx_aliases)
        if (strcmp(a.alias, name) == 0) return spyro1_sfx_slot_names[a.slot];
    return name;
}

#define SPYRO_SPU_SOUNDDEFS_VA  0x800761D0u /* g_Spu.m_SoundDefinitions */
#define SPYRO_SPU_SOUNDTABLE_VA 0x800761D4u /* g_Spu.m_SoundTable */
#define SPYRO_CUTSCENE_DEF_VA   0x800778F0u /* g_CutsceneSoundDef: a ONE-entry
                                             * def the cutscene/title/speech
                                             * code points m_SoundDefinitions
                                             * at while the level TABLE stays */
#define SPYRO_SOUNDDEF_STRIDE   0x14u
#define SPYRO_SPU_BANK_BLAST    (0x80000u - 0x1010u) /* level SPU upload */

/* Slot names whose def owns `spu_addr` in the CURRENT tables. When the
 * game has pointed m_SoundDefinitions at the one-entry cutscene def the
 * level table is meaningless for it: no slot names then. */
static int spyro_sfx_slot_names(uint32_t spu_addr, const char** out, int max) {
    uint32_t defs_va  = *(uint32_t*)PTR(SPYRO_SPU_SOUNDDEFS_VA);
    uint32_t table_va = *(uint32_t*)PTR(SPYRO_SPU_SOUNDTABLE_VA);
    if (!defs_va || !table_va || defs_va == SPYRO_CUTSCENE_DEF_VA) return 0;
    const uint8_t* table = (const uint8_t*)PTR(table_va);
    const uint8_t* defs  = (const uint8_t*)PTR(defs_va);
    int count = *(const int32_t*)PTR(table_va + 256);
    if (!table || !defs || count <= 0 || count > 256) return 0;
    int n = 0;
    for (int slot = 0; slot < SPYRO1_SFX_SLOT_COUNT && n < max; slot++) {
        uint8_t id = table[slot];
        if (id >= count) continue;
        uint32_t addr;
        memcpy(&addr, defs + id * SPYRO_SOUNDDEF_STRIDE, 4);
        if (addr == spu_addr) out[n++] = spyro1_sfx_slot_names[slot];
    }
    return n;
}

/* ── sfx pack glue ──
 * The pack folds a replacement's pitch into its buffer rate using the
 * base pitch the game will keep refreshing the voice with: the
 * ActiveSound's m_Pitch (the def's, or the per-play override), read
 * from guest RAM. g_Spu.m_ActiveSounds[24] follows SpuCommonAttr (0x28
 * bytes); ActiveSound is 0x1C bytes with m_Pitch at +8. */
#define SPYRO_SPU_VA            0x80075F08u /* g_Spu */
#define SPYRO_ACTIVESOUND_OFF   0x28u
#define SPYRO_ACTIVESOUND_SIZE  0x1Cu
static int spyro_sfx_base_pitch(int voice) {
    if (voice < 0 || voice >= 24) return 0;
    uint32_t p = *(uint32_t*)PTR(SPYRO_SPU_VA + SPYRO_ACTIVESOUND_OFF +
                                 (uint32_t)voice * SPYRO_ACTIVESOUND_SIZE + 8u);
    return (p > 0 && p < 0x4000u) ? (int)p : 0;
}

static void spyro_sfx_pack_arm(void) {
    static int s_armed = 0;
    if (s_armed) return;
    s_armed = 1;
    sfx_pack_set_pitch_reader(spyro_sfx_base_pitch);
    sfx_pack_set_name_resolver(spyro_sfx_slot_names);
}

extern "C" void psyq_game_sfx_arm(void) {
    spyro_sfx_pack_arm();
    openpete_mods_sfx_set_canon(spyro_sfx_canon);
}

extern "C" unsigned psyq_game_spu_bank_blast_len(void) { return SPYRO_SPU_BANK_BLAST; }

extern "C" int psyq_game_sfx_slot_names(unsigned spu_addr, const char** out, int max) {
    return spyro_sfx_slot_names(spu_addr, out, max);
}
