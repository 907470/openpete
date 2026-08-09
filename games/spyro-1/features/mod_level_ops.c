// mod_level_ops.c — Spyro 1 game ops for the mod level-slot registry
// (core/mods/mod_registry.c; modding framework PoC rung 12).
//
// Ground truth (recon 2026-07-11, loaders.c / overlay_pointers.c /
// wad.h / initialization.c):
//   - g_LevelIndex = (id/10 - 1)*6 + id%10 (loaders.c:1396). Retail ids
//     10..64 fill indices 0..34; ID 65 -> INDEX 35 is the one free
//     in-bounds slot — every level-indexed table is sized >= 36
//     (D_80077908[36][8], g_VisitedFlags[36], g_LevelNames[37], ...),
//     so index 35 reads/writes safely (fresh-slot zeros).
//   - The WAD TOC lives in guest RAM after a ONE-SHOT boot read:
//     g_WadHeader @ 0x8007A6D0, m_LevelEntry[36] at +72, stride 16
//     ({overlay off,len, data off,len}). Every load reads the RAM copy,
//     so cloning a slot = one 16-byte in-RAM copy of the donor's entry;
//     the stock async loader then streams the donor's REAL disc sectors.
//   - SetOverlayPointers (0x8005A470) is a switch(g_LevelId) with NO
//     default: an unknown id silently keeps the PREVIOUS level's overlay
//     fn-pointers and disc-scratch buffers — the hard breakage. Fixed by
//     the DONOR ALIAS: temp-set g_LevelId = donor around the call so the
//     game's own case body runs (byte-exact slot values), then restore.
//   - LoadLevelScene (0x8001364C) keys behavior on g_LevelId too —
//     id%10==5 marks a FLIGHT level (65%10==5!), plus per-id special
//     cases. The same donor alias makes the clone hub-faithful
//     (g_IsFlightLevel stays 0 for a hub donor).
//   - Entry trigger = the level-select cheat's writes (cheats.c:225):
//     g_NextLevelId, g_PortalLevelId=0, g_HasLevelTransition=0,
//     g_Gamestate=GS_LevelTransition, g_LoadStage=1. The stock guards
//     rejecting ids > 64 live in the callers we bypass.

#include "openpete_mods.h"

#include "psx_runtime.h" /* CPUState */
#include "mod_wad_overlay.h" /* spyro1_mod_wad_read_level_data */

#define ADDR_G_WADHEADER   0x8007A6D0u
#define WAD_LEVELENTRY_OFF 72u
#define WAD_LEVELENTRY_SZ  16u
#define SLOT_INDEX         35
#define SLOT_LEVEL_ID      65

#define ADDR_G_GAMESTATE   0x800757D8u
#define ADDR_G_LEVELID     0x8007596Cu
#define ADDR_G_NEXTLEVELID 0x800758B4u
#define ADDR_G_PORTALLVLID 0x800758ACu
#define ADDR_G_HASLVLTRANS 0x800756D0u
#define ADDR_G_LOADSTAGE   0x80075864u

#define ADDR_SETOVERLAYPTRS 0x8005A470u
#define ADDR_LOADLEVELSCENE 0x8001364Cu

/* Chain plumbing (mod_chain.c) — same private-decl pattern as
 * core/mods/mod_registry.c. */
int  openpete_mods_chain_add(uint32_t addr,
                             void (*fn)(CPUState* cpu),
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

static int s_slot_donor = 0; /* donor level id; 0 = slot unclaimed */
static int s_hooks_installed = 0;

static uint32_t entry_addr(int index) {
    return ADDR_G_WADHEADER + WAD_LEVELENTRY_OFF
         + WAD_LEVELENTRY_SZ * (uint32_t)index;
}

/* Donor alias: run the wrapped guest function with g_LevelId swapped to
 * the donor so its switch/branch cases key like the donor level, then
 * restore the slot id. Per-INDEX state (g_LevelIndex = 35) is computed
 * outside these functions and stays the slot's own. */
static void alias_call(CPUState* cpu) {
    uint32_t id = cpu->read_word(ADDR_G_LEVELID);
    int aliased = (s_slot_donor > 0 && id == SLOT_LEVEL_ID);
    if (aliased) cpu->write_word(ADDR_G_LEVELID, (uint32_t)s_slot_donor);
    openpete_mods_base(cpu);
    if (aliased) cpu->write_word(ADDR_G_LEVELID, id);
}

static int ops_claim_slot(CPUState* cpu, int donor_level_id) {
    if (donor_level_id < 10 || donor_level_id > 64) return 0;
    if (s_slot_donor) return 0; /* v1 capacity: exactly one free slot */
    /* WadInitialize done? The boot read fills the TOC; entry 0
     * (Artisans) has a nonzero data length once loaded. */
    if (cpu->read_word(entry_addr(0) + 12u) == 0u) return 0;
    int donor_index = (donor_level_id / 10 - 1) * 6 + donor_level_id % 10;
    /* Seed the slot's TOC entry from the donor's — the loader then
     * streams the donor's real disc sectors for both overlay and data. */
    for (uint32_t w = 0; w < WAD_LEVELENTRY_SZ; w += 4)
        cpu->write_word(entry_addr(SLOT_INDEX) + w,
                        cpu->read_word(entry_addr(donor_index) + w));
    if (!s_hooks_installed) {
        if (openpete_mods_chain_add(ADDR_SETOVERLAYPTRS, alias_call,
                                    "level-registry") != 0 ||
            openpete_mods_chain_add(ADDR_LOADLEVELSCENE, alias_call,
                                    "level-registry") != 0)
            return 0;
        s_hooks_installed = 1;
    }
    s_slot_donor = donor_level_id;
    return SLOT_LEVEL_ID;
}

static int ops_enter_level(CPUState* cpu, int level_id) {
    /* Only from live gameplay — a mid-load re-trigger would corrupt the
     * load state machine. */
    if (cpu->read_word(ADDR_G_GAMESTATE) != 0u) return -1;
    cpu->write_word(ADDR_G_NEXTLEVELID, (uint32_t)level_id);
    cpu->write_word(ADDR_G_PORTALLVLID, 0u);
    cpu->write_word(ADDR_G_HASLVLTRANS, 0u);
    cpu->write_word(ADDR_G_LOADSTAGE, 1u);
    cpu->write_word(ADDR_G_GAMESTATE, 1u); /* GS_LevelTransition */
    return 0;
}

static int ops_read_level_data(int level_id, void* buf, uint32_t cap) {
    return spyro1_mod_wad_read_level_data(level_id, buf, cap);
}

/* Re-aim an already-claimed slot at a new donor: re-copy the donor's
 * TOC entry over the slot's and swap the alias donor. The custom-level
 * registry calls this when a different level takes the shared slot. */
static int ops_retarget_slot(CPUState* cpu, int donor_level_id) {
    if (!s_slot_donor) return -1;
    if (donor_level_id < 10 || donor_level_id > 64) return -1;
    int donor_index = (donor_level_id / 10 - 1) * 6 + donor_level_id % 10;
    for (uint32_t w = 0; w < WAD_LEVELENTRY_SZ; w += 4)
        cpu->write_word(entry_addr(SLOT_INDEX) + w,
                        cpu->read_word(entry_addr(donor_index) + w));
    s_slot_donor = donor_level_id;
    return 0;
}

/* Declare a Data length on a level's guest TOC entry (0 = restore the
 * stock length read from the user's own disc TOC). Validated against a
 * live table: entry 0 must be populated. */
static int ops_declare_data_len(CPUState* cpu, int level_id, uint32_t len) {
    if (level_id < 10 || level_id > SLOT_LEVEL_ID) return -1;
    int index = (level_id / 10 - 1) * 6 + level_id % 10;
    if (index < 0 || index > SLOT_INDEX) return -1;
    if (cpu->read_word(entry_addr(0) + 12u) == 0u) return -1;
    if (len == 0) {
        int n = spyro1_mod_wad_read_level_data(
            level_id == SLOT_LEVEL_ID && s_slot_donor ? s_slot_donor
                                                      : level_id,
            NULL, 0);
        if (n < 0) return -1;
        len = (uint32_t)n;
    }
    cpu->write_word(entry_addr(index) + 12u, len);
    return 0;
}

int spyro1_level_resolve(const char* arg); /* levelwarp_native.c */
static int ops_resolve_name(const char* name) {
    return name && name[0] ? spyro1_level_resolve(name) : -1;
}

static const openpete_level_ops_t s_spyro1_level_ops = {
    .claim_slot       = ops_claim_slot,
    .retarget_slot    = ops_retarget_slot,
    .declare_data_len = ops_declare_data_len,
    .resolve_name     = ops_resolve_name,
    .enter_level      = ops_enter_level,
    .read_level_data  = ops_read_level_data,
};

void spyro1_level_ops_register(void) {
    openpete_mods_set_level_ops(&s_spyro1_level_ops);
}
