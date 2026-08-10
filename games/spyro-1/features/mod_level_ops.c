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
#include "openpete_log.h"

#include "psx_runtime.h" /* CPUState */
#include "mod_wad_overlay.h" /* spyro1_mod_wad_read_level_data */
#include "game_extras.h"     /* game_set_skip_to_level (title-screen route) */

#include <stdio.h>
#include <string.h>

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
#define ADDR_G_DEMOMODE    0x80075714u /* DEMO_MODE_NONE = 0 */
/* D_8006EF9C — per-level-index XA track (int[48]). The free slot's row
 * (index 35) ships as track 19 = Gnasty's Loot; a claimed slot plays
 * the DONOR's music instead. */
#define ADDR_LEVEL_TRACK_TAB 0x8006EF9Cu

static void inherit_donor_music(CPUState* cpu, int donor_index) {
    cpu->write_word(ADDR_LEVEL_TRACK_TAB + 4u * SLOT_INDEX,
                    cpu->read_word(ADDR_LEVEL_TRACK_TAB +
                                   4u * (uint32_t)donor_index));
}

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
    inherit_donor_music(cpu, donor_index);
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
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    /* Title screen: the --skip-to-level machinery owns this route (same
     * hand-off the dev levelwarp uses) — it induces the demo-timeout
     * transition at the next Press-Start window WITH the demo-replay
     * downgrade. The cheat-path writes below would instead load the
     * level in attract-demo mode, whose desynced input replay strobes
     * the pause menu. */
    if (gs == 13u) {
        game_set_skip_to_level(level_id);
        return 0;
    }
    /* Otherwise only from live NON-DEMO gameplay — a mid-load
     * re-trigger would corrupt the load state machine, and the attract
     * demo (GS_Playing with g_DemoMode set) would keep its input
     * replay running inside the new level. Refusal leaves the warp
     * queued; it fires once the demo hands back to the title screen. */
    if (gs != 0u) return -1;
    if (cpu->read_word(ADDR_G_DEMOMODE) != 0u) return -1;
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
    inherit_donor_music(cpu, donor_index);
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

/* ── In-game level titles ─────────────────────────────────────────────
 * g_LevelNames (char*[37] @0x8006F7F0) is the single name source: the
 * inventory rows and entry title cards (func_800181AC: A-Z 0-9 !,?.),
 * and the homeworld portal name arc (A-Z only — anything else renders
 * as an apostrophe there). Custom names live in the mods low-park
 * window (real low guest RAM, savestate-carried). */
#define ADDR_G_LEVELNAMES 0x8006F7F0u

static int level_index_of(int level_id) {
    if (level_id == SLOT_LEVEL_ID) return SLOT_INDEX;
    if (level_id < 10 || level_id > 64) return -1;
    return (level_id / 10 - 1) * 6 + level_id % 10;
}

static int ops_set_level_name(CPUState* cpu, int level_id,
                              const char* name) {
    static uint32_t s_stock[37];
    static uint8_t  s_known[37];
    static struct { char text[40]; uint32_t gaddr; } s_park[8];
    static int s_park_n = 0;

    int idx = level_index_of(level_id);
    if (idx < 0 || idx > 36) return -1;
    uint32_t entry = ADDR_G_LEVELNAMES + 4u * (uint32_t)idx;
    if (!s_known[idx]) {
        s_stock[idx] = cpu->read_word(entry);
        s_known[idx] = 1;
    }
    if (!name) {
        cpu->write_word(entry, s_stock[idx]);
        return 0;
    }
    /* Uppercase; keep the glyphs the text renderers map; drop the rest.
     * Collapses to nothing -> keep the stock name. */
    char clean[40];
    int n = 0;
    for (const char* p = name; *p && n < (int)sizeof clean - 1; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
            c == '!' || c == ',' || c == '?' || c == '.' || c == '\'')
            clean[n++] = c;
    }
    clean[n] = '\0';
    if (!n) return -1;

    uint32_t gaddr = 0;
    for (int i = 0; i < s_park_n && !gaddr; i++)
        if (strcmp(s_park[i].text, clean) == 0) gaddr = s_park[i].gaddr;
    if (!gaddr) {
        void* host = NULL;
        gaddr = openpete_mods_guest_alloc_low((uint32_t)n + 1u, 4u, &host,
                                              "level-names");
        if (!gaddr || !host) return -1;
        memcpy(host, clean, (size_t)n + 1u);
        if (s_park_n < (int)(sizeof s_park / sizeof s_park[0])) {
            snprintf(s_park[s_park_n].text, sizeof s_park[0].text, "%s",
                     clean);
            s_park[s_park_n].gaddr = gaddr;
            s_park_n++;
        }
    }
    cpu->write_word(entry, gaddr);
    OPL_INFO_F("mods", "level title: index %d -> \"%s\" (@0x%08X)", idx,
               clean, gaddr);
    return 0;
}

/* ── Replace-mode portal skybox ───────────────────────────────────────
 * A homeworld portal's preview dome is a copy of the destination's sky
 * BAKED INTO THE HOMEWORLD'S OWN LEVEL DATA (loaders.c: each Portal
 * carries copyBgColor/copySectorCount/copySectors, pointer-patched at
 * load) — replacing the destination level never touches it. While a
 * replace is declared, this driver parses the custom level's own
 * cyclorama component out of the mod's data.bin (LevelHeader
 * m_DataOffset @+8 -> skip 5 self-sizing components -> cyclorama:
 * [size][bg word][sector count][payload-relative sector ptrs]...),
 * parks the payload in the guest arena with the pointers absolutized,
 * and repoints the donor portal's three preview fields at it. The
 * native portal feeder re-derives from these same guest structures, so
 * the substitution flows through both render paths. */
#define ADDR_G_PORTALS     0x80078640u
#define ADDR_G_PORTALCOUNT 0x800758BCu
#define PORTAL_M_SKYBOX    0u
#define PORTAL_M_LEVELID   28u
#define SKY_SECTORCOUNT    0u
#define SKY_SECTORS        4u
#define SKY_BGCOLOR        16u

typedef struct {
    char     stem[64];
    uint32_t gaddr;   /* arena copy of the cyclorama payload; 0 = bad */
    uint32_t count;
    uint32_t bgword;
} sky_park_t;

static const sky_park_t* sky_park_get(const char* dir, const char* stem) {
    static sky_park_t s_parks[8];
    static int s_n = 0;
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_parks[i].stem, stem) == 0)
            return s_parks[i].gaddr ? &s_parks[i] : NULL;
    if (s_n >= (int)(sizeof s_parks / sizeof s_parks[0])) return NULL;
    sky_park_t* pk = &s_parks[s_n++];
    snprintf(pk->stem, sizeof pk->stem, "%s", stem);
    pk->gaddr = 0;

    char path[1152];
    snprintf(path, sizeof path, "%s/levels/%s.data.bin", dir, stem);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    uint32_t data_off = 0;
    int ok = fsz > 16 && fseek(f, 8, SEEK_SET) == 0 &&
             fread(&data_off, 4, 1, f) == 1;
    uint32_t pos = data_off;
    for (int c = 0; ok && c < 5; c++) { /* tex, sectors, occl, surf, coll */
        uint32_t csz = 0;
        ok = fseek(f, (long)pos, SEEK_SET) == 0 &&
             fread(&csz, 4, 1, f) == 1 && csz > 0 &&
             pos + csz <= (uint32_t)fsz;
        pos += csz;
    }
    uint32_t csz = 0;
    ok = ok && fseek(f, (long)pos, SEEK_SET) == 0 &&
         fread(&csz, 4, 1, f) == 1 && csz > 8 &&
         pos + csz <= (uint32_t)fsz;
    if (!ok) { fclose(f); return NULL; }
    uint32_t paylen = csz - 4u;
    void* host = NULL;
    uint32_t gaddr =
        openpete_mods_guest_alloc(paylen, 16u, &host, "portal-sky");
    if (!gaddr || !host ||
        fread(host, 1, paylen, f) != paylen) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    uint32_t* w = (uint32_t*)host;
    pk->bgword = w[0];
    pk->count  = w[1];
    if (pk->count > (paylen - 8u) / 4u) return NULL; /* corrupt */
    for (uint32_t i = 0; i < pk->count; i++)
        w[2 + i] += gaddr; /* payload-relative -> absolute guest */
    pk->gaddr = gaddr;
    OPL_INFO_F("mods", "portal sky parked: %s (%u sectors, %u B @0x%08X)",
               stem, pk->count, paylen, gaddr);
    return pk;
}

static int ops_replace_sky_tick(CPUState* cpu, int donor_id,
                                const char* dir, const char* stem,
                                int on) {
    /* One donor portal patched at a time (mirrors the one-active-custom
     * -level rule). Saved fields are per level-instance: a level load
     * rebuilds portals from fresh data, so the patch state resets when
     * g_LevelId changes. */
    static int      s_patched = 0;
    static uint32_t s_level, s_sky_addr;
    static uint32_t s_saved[3]; /* count, sectors, bg */

    uint32_t lvl = cpu->read_word(ADDR_G_LEVELID);
    if (s_patched && lvl != s_level) s_patched = 0;

    uint32_t n = cpu->read_word(ADDR_G_PORTALCOUNT);
    if (n > 6u) return 0; /* portals not live (mid-load) */
    uint32_t sky = 0;
    for (uint32_t i = 0; i < n && !sky; i++) {
        uint32_t portal = cpu->read_word(ADDR_G_PORTALS + 4u * i);
        if (!portal) continue;
        if (cpu->read_word(portal + PORTAL_M_LEVELID) == (uint32_t)donor_id)
            sky = cpu->read_word(portal + PORTAL_M_SKYBOX);
    }

    if (!on) {
        if (s_patched && sky == s_sky_addr) {
            cpu->write_word(sky + SKY_SECTORCOUNT, s_saved[0]);
            cpu->write_word(sky + SKY_SECTORS, s_saved[1]);
            cpu->write_word(sky + SKY_BGCOLOR, s_saved[2]);
            s_patched = 0;
        }
        return 0;
    }
    if (!sky) return 0;
    const sky_park_t* pk = sky_park_get(dir, stem);
    if (!pk) return -1;
    if (cpu->read_word(sky + SKY_SECTORS) == pk->gaddr + 8u) return 0;
    if (!s_patched) {
        s_saved[0] = cpu->read_word(sky + SKY_SECTORCOUNT);
        s_saved[1] = cpu->read_word(sky + SKY_SECTORS);
        s_saved[2] = cpu->read_word(sky + SKY_BGCOLOR);
        s_patched = 1;
        s_level = lvl;
        s_sky_addr = sky;
    }
    cpu->write_word(sky + SKY_SECTORCOUNT, pk->count);
    cpu->write_word(sky + SKY_SECTORS, pk->gaddr + 8u);
    cpu->write_word(sky + SKY_BGCOLOR, pk->bgword);
    OPL_INFO_F("mods", "portal sky: donor %d portal now renders %s "
               "(%u sectors)", donor_id, stem, pk->count);
    return 0;
}

static const openpete_level_ops_t s_spyro1_level_ops = {
    .claim_slot       = ops_claim_slot,
    .retarget_slot    = ops_retarget_slot,
    .declare_data_len = ops_declare_data_len,
    .resolve_name     = ops_resolve_name,
    .enter_level      = ops_enter_level,
    .read_level_data  = ops_read_level_data,
    .set_level_name   = ops_set_level_name,
    .replace_sky_tick = ops_replace_sky_tick,
};

void spyro1_level_ops_register(void) {
    openpete_mods_set_level_ops(&s_spyro1_level_ops);
}
