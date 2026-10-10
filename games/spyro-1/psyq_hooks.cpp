// games/spyro-1/psyq_hooks.cpp - what Spyro 1 supplies to the shared PSYQ thunk
// layer.
//
// The thunk layer is game-neutral; everything here is not. Three kinds of
// thing live in this file:
//
//  - PSYQ's own data structures, written into PSX RAM at Spyro 1's addresses
//    because gameplay code reads them back and those bytes must match what
//    hardware leaves. The values are replayed verbatim, not computed, and
//    are specific to PSYQ 4.0 as this game links it.
//  - The stack frame each real PSYQ function leaves behind. An empty C thunk
//    leaves those slots at whatever was there before, which reads as a RAM
//    difference at every call.
//  - Couplings to Spyro 1's own features: its asset overlay, its music route
//    and its sound-effect name tables.

extern "C" {
#include "psyq/psyq_game_hooks.h"
#include "openpete_psx.h"
#include "openpete_fiber.h"
#include "openpete_mods.h"
#include "openpete_ticker.h"
}

#include "mods/mod_wad_overlay.h"
#include "psx/libcd.h"

#include <string.h>

extern "C" void openpete_logf(int level, const char* subsystem, const char* fmt, ...);

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
         * 0x0200. */
        cpu->write_half(ENV + 4u, 0x0400u);
        cpu->write_half(ENV + 6u, 0x0200u);
        /* Byte at ENV+8 (0x80074A6C) is 1 after ResetGraph; the libgpu
         * store that writes it is not identified. */
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

    /* func_80061DEC (called by ResetGraph) also zeroes the callback ring's
     * head and tail at 0x80074B68/6C; they are zero from BSS already and
     * advance with hardware's interrupt dispatch afterwards, so they are
     * not written here. */

    /* The post-ResetGraph state of libgpu's control structures, as the
     * SCPH-1001 BIOS leaves them: libgpu writes these through helpers that
     * reach the BIOS jump tables (`func_8005DDC8` -> *(D_800749AC)+0xC,
     * `func_8005DF60` walks the 0x80073924 control struct, `func_80061DEC`
     * updates the trio at 0x80074B68/6C/78), so the values are replayed. NTSC
     * mode 0, gp=0x80075264, sp=0x801FFFF8 at boot. Not written here: the
     * interrupt-dispatch context at 0x80074B58 (handler, sp, argument of the
     * callback in flight) and the counters at 0x80074B7C/0x80074B80, which
     * hardware's interrupt timing decides. */
    if (sub == 0 || sub == 3 || sub == 5) {
        /* libgpu callback control struct at 0x80073924+ */
        cpu->write_word(0x80073924u, 0x00000001u);
        cpu->write_word(0x80073928u, 0x8005E560u);  /* fnptr */
        cpu->write_word(0x80073934u, 0x8005E680u);  /* fnptr */
        cpu->write_word(0x80073954u, 0x0000000Du);  /* enabled-source mask */
        cpu->write_word(0x8007395Cu, 0x8005DFC8u);  /* fnptr */
        cpu->write_word(0x80073960u, 0x8007493Cu);
        cpu->write_word(0x80073964u, 0x801FFFF8u);  /* boot sp */
        cpu->write_word(0x80073968u, 0x80073924u);  /* self ref */
        cpu->write_word(0x80073970u, 0x00000001u);
        cpu->write_word(0x80073988u, 0x80075264u);  /* boot gp */
        /* libgpu vtable patch slots (indirected through D_800749AC) */
        cpu->write_word(0x80074990u, 0x8005E804u);
        cpu->write_word(0x800749A0u, 0x8005E5D8u);
        cpu->write_word(0x80074B70u, 0x0000000Du);  /* enabled-source mask */
        cpu->write_word(0x80074B78u, 0x00000009u);
        /* The fill-packet block after ResetGraph's own clear of VRAM
         * (512x511 at the origin, black); ClearImage rewrites it. */
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
     * PSX-side bytes, so they are written here. The values are replayed
     * verbatim - function pointers are PSYQ code (not
     * relocated), the buffer addresses come from a0/a1. */

    /* Pad-state dispatch table @ 0x800751BC (10 PSYQ fnptrs), then the two
     * words at 0x800751F0/0x800751F4. */
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
    /* 0x80075C58 holds the kernel work address the BIOS's pad driver
     * returns to libpad; SCPH-1001 answers 0x74A8. */
    cpu->write_word(0x80075C58u, 0x000074A8u);
    cpu->write_word(0x80075C5Cu, 0x8006969Cu);
    cpu->write_word(0x80075C60u, 0x80069634u);
    /* libpad's control block at 0x80075D18: pointers into itself and into
     * libpad's own routines, the slot-0 receive buffer at +0x30, and the
     * port control bytes. */
    static const uint32_t k_pad_block[25] = {
        0x80075D7Cu, 0x80075D80u, 0x80075D8Cu, 0x00000000u, 0x80075D18u,
        0x8006A74Cu, 0x8006A768u, 0x00000000u, 0x8006FCCCu, 0x00000000u,
        0x80075800u, 0x80075D3Cu, 0u /* buf0 */, 0x00000102u, 0x00000000u,
        0x80075C88u, 0x80075CD0u, 0x00FF0908u, 0x00000600u, 0x00000000u,
        0x01000100u, 0x00000000u, 0x00000000u, 0xFF010000u, 0x00FFFFFFu,
    };
    for (uint32_t i = 0; i < 25u; i++)
        cpu->write_word(0x80075D18u + 4u * i, i == 12u ? buf0 : k_pad_block[i]);
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
     * skip the entire chain, so the post-init values are written here. */
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

/* libgpu's environment block and fill-packet block (psx.map D_80074A64,
 * D_80075978). */
extern "C" uint32_t psyq_game_libgpu_env_addr(void) { return 0x80074A64u; }
extern "C" uint32_t psyq_game_libgpu_clear_block_addr(void) { return 0x80075978u; }

/* --- Music route -------------------------------------------------------- */

#define SPYRO_MUSIC_TRACKS 48

/** @brief Reads the game's current music track index from g_CdMusic.m_CurrentTrack (0x800774B0).
 *
 * The CdlSetfilter args cannot identify a music track (file is always 1;
 * chan is only track-within-XA-file), so the game's own index is read.
 * Every play path writes m_CurrentTrack before issuing CdlReadS (music.c:28,
 * music.c:51-68, loaders.c:776).
 * @return The track (0 to 47), or -1 when the word is out of range. */
static int spyro_current_music_track(void) {
    int32_t track = 0;
    memcpy(&track, openpete_psx_get_ram() + 0x774B0, sizeof(track));
    return (track >= 0 && track < SPYRO_MUSIC_TRACKS) ? (int)track : -1;
}

extern "C" void psyq_game_music_route(int sector, psyq_music_route_t* out) {
    (void)sector;
    const int track = spyro_current_music_track();
    out->track = track;

    /* Custom-level song ([[level]] music): resolved against the guest's
     * level id at the moment it starts the stream. Prefer g_NextLevelId
     * (same rule as the WAD overlay's latch): the level's first start
     * fires inside LoadLevelScene, where the slot's donor alias has
     * g_LevelId temporarily swapped to the DONOR - g_NextLevelId still
     * names the slot. Loop restarts fire outside the alias, where both
     * agree. */
    int32_t level_id = 0;
    memcpy(&level_id, openpete_psx_get_ram() + 0x758B4, sizeof(level_id));
    if (level_id < 10 || level_id > 65)
        memcpy(&level_id, openpete_psx_get_ram() + 0x7596C,
               sizeof(level_id));
    double mdur = 0.0;
    const char* mfile =
        openpete_mods_custom_music_for((int)level_id, &mdur);
    out->custom_path = mfile;

    /* Guest-deterministic half (snapshotted; runs during speculation):
     * keep the guest's loop bookkeeping sane for a custom song. The
     * track keeps the DONOR's number (inherited at slot claim), but its
     * m_MusicTable end is stretched to the WAV's length so the guest
     * re-issues its loop ReadS when the song actually ends - 150
     * group-sectors per second (37.8 kHz stereo XA = 18.75 audio
     * sectors/s under the 8-sector interleave). Restored to stock the
     * next time the track starts without a custom song. The stock end is
     * host state that decides a guest RAM write and is not snapshotted. */
    if (track >= 0) {
        uint8_t* ram = openpete_psx_get_ram();
        const uint32_t ent = 0x774B0u + 8u + 8u * (uint32_t)track;
        int32_t tstart = 0, tend = 0;
        memcpy(&tstart, ram + ent, 4);
        memcpy(&tend, ram + ent + 4u, 4);
        static int32_t s_stock_end[SPYRO_MUSIC_TRACKS];
        static uint8_t s_stock_known[SPYRO_MUSIC_TRACKS];
        if (!s_stock_known[track]) {
            s_stock_end[track] = tend;
            s_stock_known[track] = 1;
        }
        if (mfile && mdur > 0.0) {
            int32_t want = tstart + (int32_t)(mdur * 150.0 + 0.5);
            if (tend != want) memcpy(ram + ent + 4u, &want, 4);
        } else if (s_stock_known[track] && tend != s_stock_end[track]) {
            memcpy(ram + ent + 4u, &s_stock_end[track], 4);
        }
        if (mfile) {
            /* Pin the easter-egg swap away: the game's easter table has
             * no row for the free slot's index 35 (an OOB read into the
             * XA offset table), and a custom level shouldn't swap to
             * retail songs mid-play regardless. */
            const int32_t never = 0x7FFFFFFF;
            memcpy(ram + 0x774B0u + 540u, &never, 4);
        }
    }
    if (mfile && !openpete_speculative)
        openpete_logf(2, "music", "custom-level song for level %d: %s (%.1fs "
                      "loop)", (int)level_id, mfile, mdur);
}


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
/** @brief Maps a mod-facing sound alias to the sound slot name the sfx pack resolves.
 *
 * The game mod descriptor's sfx_canon member (mods/mod_game_desc.c).
 * @param name A sound name from a mod; not NULL.
 * @return The slot name for an alias in k_sfx_aliases, else name itself. */
extern "C" const char* spyro1_sfx_canon(const char* name) {
    for (const auto& a : k_sfx_aliases)
        if (strcmp(a.alias, name) == 0) return spyro1_sfx_slot_names[a.slot];
    return name;
}

/* The level sound tables (SetNewSoundTable, loaders.c): g_Spu.m_SoundTable
 * and g_Spu.m_SoundDefinitions. g_Spu.m_ActiveSounds[24] follows
 * SpuCommonAttr (0x28 bytes) in g_Spu; ActiveSound is 0x1C bytes with
 * m_Pitch at +8. The level SPU upload is 0x80000 - 0x1010 bytes. */
static const psyq_sfx_slots_t k_sfx_slots = {
    0x800761D4u,                 /* g_Spu.m_SoundTable */
    0x800761D0u,                 /* g_Spu.m_SoundDefinitions */
    0x80075F08u + 0x28u,         /* g_Spu.m_ActiveSounds */
    0x1Cu,
    8u,                          /* m_Pitch */
    0x80000u - 0x1010u,
    spyro1_sfx_slot_names,
};

extern "C" const psyq_sfx_slots_t* psyq_game_sfx_slots(void) { return &k_sfx_slots; }
