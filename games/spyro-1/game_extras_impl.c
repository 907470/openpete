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
    int log_floor_query = 0;
    int log_pos_only = 0;
    switch (addr) {
        case 0x8002C85Cu: tag = "die";     break;
        case 0x8004A200u: tag = "physics"; break;
        case 0x80043FE4u: tag = "integ";   log_pos_only = 1; break;
        case 0x80047B60u: tag = "stateD";  log_pos_only = 1; break;
        case 0x8004888Cu: tag = "rotUpd";  log_pos_only = 1; break;
        /* State-0 (idle) handler subcalls — one of these teleports Spyro. */
        case 0x8003E628u: tag = "F8003E628"; log_pos_only = 1; break;
        case 0x8003FE40u: tag = "F8003FE40"; log_pos_only = 1; break;
        case 0x8003E318u: tag = "SlopeFC";   log_pos_only = 1; break;
        case 0x8003E758u: tag = "AdjAirC";   log_pos_only = 1; break;
        case 0x8004D5ECu: tag = "floorQ";  log_floor_query = 1; break;
        default: return 0;
    }
    if (log_pos_only) {
        uint32_t sx2=0, sy2=0, sz2=0;
        uint32_t mState=0, controlF=0;
        uint32_t collMx=0, collMy=0, collMz=0;
        uint32_t velX=0, velY=0, velZ=0;
        uint32_t trueVelX=0, trueVelY=0, trueVelZ=0;
        sr32_safe(0x80078A58u + 0u,    &sx2);
        sr32_safe(0x80078A58u + 4u,    &sy2);
        sr32_safe(0x80078A58u + 8u,    &sz2);
        sr32_safe(0x80078A58u + 0x78u, &mState);
        sr32_safe(0x80078A58u + 0x1F4u,&controlF);    /* m_ControlFlags */
        /* m_CollisionMovement at SpyroPhysics+0x64 = g_Spyro+0xC8+0x64 = g_Spyro+0x12C */
        sr32_safe(0x80078A58u + 0x12Cu,&collMx);
        sr32_safe(0x80078A58u + 0x130u,&collMy);
        sr32_safe(0x80078A58u + 0x134u,&collMz);
        /* m_Velocity @ +0x100, m_TrueVelocity @ +0x10C */
        sr32_safe(0x80078A58u + 0x100u,&velX);
        sr32_safe(0x80078A58u + 0x104u,&velY);
        sr32_safe(0x80078A58u + 0x108u,&velZ);
        sr32_safe(0x80078A58u + 0x10Cu,&trueVelX);
        sr32_safe(0x80078A58u + 0x110u,&trueVelY);
        sr32_safe(0x80078A58u + 0x114u,&trueVelZ);
        OPL_INFO_F("hook",
                   "%s f%u pos=(%d,%d,%d) state=%d ctrl=0x%X "
                   "vel=(%d,%d,%d) trueVel=(%d,%d,%d) "
                   "collMov=(%d,%d,%d) ra=0x%08X",
                   tag, openpete_frame,
                   (int)sx2, (int)sy2, (int)sz2, (int)mState, controlF,
                   (int)velX, (int)velY, (int)velZ,
                   (int)trueVelX, (int)trueVelY, (int)trueVelZ,
                   (int)collMx, (int)collMy, (int)collMz, cpu->ra);
        return 0;
    }
    if (log_floor_query) {
        /* a0 = Vector3D*, a1 = max search dist. Read cpu state directly. */
        uint32_t a0 = cpu->a0;
        uint32_t a1 = cpu->a1;
        uint32_t qx=0, qy=0, qz=0, lastRetPrev=0;
        sr32_safe(a0 +  0u, &qx);
        sr32_safe(a0 +  4u, &qy);
        sr32_safe(a0 +  8u, &qz);
        sr32_safe(0x80076B88u, &lastRetPrev);
        OPL_INFO_F("hook",
                   "floorQ f%u qPos=(%d,%d,%d) max=%d a0=0x%08X "
                   "lastPrevRet=%d ra=0x%08X",
                   openpete_frame,
                   (int)qx, (int)qy, (int)qz, (int)a1, a0,
                   (int)lastRetPrev, cpu->ra);
        return 0;
    }
    uint32_t gs=0, sx=0, sy=0, sz=0, surf=0, dt=0;
    uint32_t terrainColl=0, mobyChain=0;
    uint32_t triCount=0, flagCount=0, triPtr=0;
    sr32_safe(0x800757D8u,             &gs);
    sr32_safe(0x80078A58u +    0u,     &sx);
    sr32_safe(0x80078A58u +    4u,     &sy);
    sr32_safe(0x80078A58u +    8u,     &sz);
    sr32_safe(0x80078A58u + 0xA0u,     &surf);
    sr32_safe(0x800756CCu,             &dt);
    /* g_Environment.m_TerrainCollision @ 0x800785A8 + 0x2C = 0x800785D4 */
    sr32_safe(0x800785D4u,             &terrainColl);
    /* g_MobyCollisionChain @ 0x80075778 */
    sr32_safe(0x80075778u,             &mobyChain);
    /* If TerrainCollision pointer is valid, walk it: m_TriangleCount @ +0,
     * m_FlagCount @ +4, m_Triangles @ +0x10. */
    uint32_t blockTree=0, blocks=0;
    if (terrainColl >= 0x80000000u && terrainColl < 0x80200000u) {
        sr32_safe(terrainColl +  0u,    &triCount);
        sr32_safe(terrainColl +  4u,    &flagCount);
        sr32_safe(terrainColl +  8u,    &blockTree);
        sr32_safe(terrainColl + 0xCu,   &blocks);
        sr32_safe(terrainColl + 0x10u,  &triPtr);
    }
    /* Compute Spyro's collision-chain block index. The asm at func_8004D5EC
     * uses (pos.x>>13) + ((pos.y>>13)<<5) and looks up at
     * (g_MobyCollisionChain + 4096 + index*4). */
    int32_t bx = (int32_t)sx >> 13;
    int32_t by = (int32_t)sy >> 13;
    int32_t blockIdx = bx + (by << 5);
    uint32_t chainEntry = 0;
    if (mobyChain >= 0x80000000u && mobyChain < 0x80200000u) {
        uint32_t entryAddr = mobyChain + 4096u + (uint32_t)(blockIdx * 4);
        sr32_safe(entryAddr, &chainEntry);
    }
    /* The terrain collision lookup uses BlockTree. Read the first node — a
     * BlockTree is a linked list per (px>>16, py>>16) chunk; head pointers
     * live in BlockTree[ (px>>16) + (py>>16)*N ]. Without knowing N, just
     * sample BlockTree[0..3] to see if it's non-empty. */
    uint32_t btree0=0, btree1=0, btree2=0, btree3=0;
    if (blockTree >= 0x80000000u && blockTree < 0x80200000u) {
        sr32_safe(blockTree +  0u, &btree0);
        sr32_safe(blockTree +  4u, &btree1);
        sr32_safe(blockTree +  8u, &btree2);
        sr32_safe(blockTree + 12u, &btree3);
    }
    /* D_80076B88 holds the LAST floor-query return value — the function writes
     * v0 to (v1+8) where v1=0x80076B80, just before its `jr $ra`. */
    uint32_t lastRet = 0;
    sr32_safe(0x80076B88u, &lastRet);

    OPL_INFO_F("hook",
               "%s f%u gs=%d pos=(%d,%d,%d) surf=%d lastRet=%d "
               "TC tri#=%d btree=0x%08X[%08X %08X %08X %08X] "
               "block(%d,%d)=idx%d chain[%d]=0x%08X",
               tag, openpete_frame, (int)gs,
               (int)sx, (int)sy, (int)sz, (int)surf, (int)lastRet,
               (int)triCount, blockTree, btree0, btree1, btree2, btree3,
               bx, by, blockIdx, blockIdx, chainEntry);
    return 0;
}
