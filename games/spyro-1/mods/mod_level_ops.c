// games/spyro-1/mods/mod_level_ops.c - Spyro 1 game ops for the mod
// level-slot registry (core/mods/mod_registry.c).
//
// Ground truth (loaders.c / overlay_pointers.c / wad.h /
// initialization.c):
//   - g_LevelIndex = (id/10 - 1)*6 + id%10 (loaders.c:1396). Retail ids
//     10..64 fill indices 0..34; ID 65 -> INDEX 35 is the one free
//     in-bounds slot - every level-indexed table is sized >= 36
//     (D_80077908[36][8], g_VisitedFlags[36], g_LevelNames[37], ...),
//     so index 35 reads/writes safely (fresh-slot zeros).
//   - The WAD TOC lives in guest RAM after a ONE-SHOT boot read:
//     g_WadHeader @ 0x8007A6D0, m_LevelEntry[36] at +72, stride 16
//     ({overlay off,len, data off,len}). Every load reads the RAM copy,
//     so cloning a slot = one 16-byte in-RAM copy of the donor's entry;
//     the stock async loader then streams the donor's REAL disc sectors.
//   - SetOverlayPointers (0x8005A470) is a switch(g_LevelId) with NO
//     default: an unknown id silently keeps the PREVIOUS level's overlay
//     fn-pointers and disc-scratch buffers - the hard breakage. Fixed by
//     the DONOR ALIAS: temp-set g_LevelId = donor around the call so the
//     game's own case body runs (byte-exact slot values), then restore.
//   - LoadLevelScene (0x8001364C) keys behavior on g_LevelId too -
//     id%10==5 marks a FLIGHT level (65%10==5!), plus per-id special
//     cases. The same donor alias makes the clone hub-faithful
//     (g_IsFlightLevel stays 0 for a hub donor).
//   - Entry trigger = the level-select cheat's writes (cheats.c:225):
//     g_NextLevelId, g_PortalLevelId=0, g_HasLevelTransition=0,
//     g_Gamestate=GS_LevelTransition, g_LoadStage=1. The stock guards
//     rejecting ids > 64 live in the callers we bypass.

#include "openpete_mods.h"
#include "openpete_log.h"

#include "psx_runtime_internal.h" /* CPUState */
#include "mods/mod_wad_overlay.h" /* spyro1_mod_wad_read_level_data */
#include "game_extras.h"     /* game_set_skip_to_level (title-screen route) */

#include <stdio.h>
#include <string.h>

#define ADDR_G_WADHEADER   0x8007A6D0u
#define WAD_LEVELENTRY_OFF 72u
#define WAD_LEVELENTRY_SZ  16u
#define SLOT_INDEX         35
#define SLOT_LEVEL_ID      65

/* Target totals: three EXE tables, all indexed world * 6 + level. The
 * inventory rows read the dragon and gem tables and the HUD's all-gems
 * test reads the gem table; nothing in the game writes any of them, so
 * the engine is their only writer.
 *
 * g_TargetEggCounts covers the first three worlds only (0x8006F480 to
 * 0x8006F491). Two padding bytes follow it and g_MemCardSaveHeader begins
 * at 0x8006F494, so an egg index of 20 or more corrupts the header the game
 * writes to a memory card. Every egg write is gated on the index having a
 * row. */
#define ADDR_G_TARGETDRAGONS 0x8006F414u
#define ADDR_G_TARGETGEMS    0x8006F438u
#define ADDR_G_TARGETEGGS    0x8006F480u
#define COUNT_TABLE_ROWS     36
#define EGG_TABLE_ROWS       18

#define ADDR_G_GAMESTATE   0x800757D8u
#define ADDR_G_LEVELID     0x8007596Cu
#define ADDR_G_NEXTLEVELID 0x800758B4u
#define ADDR_G_PORTALLVLID 0x800758ACu
#define ADDR_G_HASLVLTRANS 0x800756D0u
#define ADDR_G_LOADSTAGE   0x80075864u
#define ADDR_G_DEMOMODE    0x80075714u /* DEMO_MODE_NONE = 0 */
/* D_8006EF9C - per-level-index XA track (int[48]). The free slot's row
 * (index 35) ships as track 19 = Gnasty's Loot; a claimed slot plays
 * the DONOR's music instead. */
#define ADDR_LEVEL_TRACK_TAB 0x8006EF9Cu

/** @brief Copies a donor level's XA track number into the custom slot's row of the track table.
 *
 * Reads the word at D_8006EF9C + 4 * donor_index and stores it at the slot's
 * row (index 35), so the slot plays the donor's music.
 * @param cpu The guest CPU state.
 * @param donor_index The donor's level index (0 to 34). */
static void inherit_donor_music(CPUState* cpu, int donor_index) {
    cpu->write_word(ADDR_LEVEL_TRACK_TAB + 4u * SLOT_INDEX,
                    cpu->read_word(ADDR_LEVEL_TRACK_TAB +
                                   4u * (uint32_t)donor_index));
}

#define ADDR_SETOVERLAYPTRS 0x8005A470u
#define ADDR_LOADLEVELSCENE 0x8001364Cu

/* Chain plumbing (mod_chain.c) - same private-decl pattern as
 * core/mods/mod_registry.c. */
int  openpete_mods_chain_add(uint32_t addr,
                             void (*fn)(CPUState* cpu),
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

static int s_slot_donor = 0; /* donor level id; 0 = slot unclaimed */
static int s_hooks_installed = 0;

/** @brief Returns the guest address of a level's 16-byte entry in the WAD TOC.
 *
 * The TOC is g_WadHeader (0x8007A6D0) + 72, stride 16.
 * @param index The level index. */
static uint32_t entry_addr(int index) {
    return ADDR_G_WADHEADER + WAD_LEVELENTRY_OFF
         + WAD_LEVELENTRY_SZ * (uint32_t)index;
}

/** @brief Runs the wrapped guest function with g_LevelId aliased to the claimed slot's donor.
 *
 * Installed as a mod override chain link on SetOverlayPointers and
 * LoadLevelScene. When a slot is claimed and g_LevelId is the slot id (65),
 * g_LevelId is swapped to the donor id around the base call so the
 * function's switch and branch cases key like the donor level, then
 * restored. Per-index state (g_LevelIndex 35) is computed outside these
 * functions and stays the slot's own.
 * @param cpu The guest CPU state. */
static void alias_call(CPUState* cpu) {
    uint32_t id = cpu->read_word(ADDR_G_LEVELID);
    int aliased = (s_slot_donor > 0 && id == SLOT_LEVEL_ID);
    if (aliased) cpu->write_word(ADDR_G_LEVELID, (uint32_t)s_slot_donor);
    openpete_mods_base(cpu);
    if (aliased) cpu->write_word(ADDR_G_LEVELID, id);
}

/** @brief Claims the free level slot (index 35, id 65) for a donor level and installs the alias hooks.
 *
 * Copies the donor's 16-byte WAD TOC entry over the slot's, copies the
 * donor's XA track, and on the first claim chains alias_call onto
 * SetOverlayPointers and LoadLevelScene. Refuses until the boot TOC read
 * has filled entry 0, and while the slot is already claimed.
 * @param cpu The guest CPU state.
 * @param donor_level_id The donor's level id, 10 to 64.
 * @return The claimed level id (65), or 0 when the id is out of range, the
 *     slot is taken, the TOC is not loaded, or a hook install failed. */
static int ops_claim_slot(CPUState* cpu, int donor_level_id) {
    if (donor_level_id < 10 || donor_level_id > 64) return 0;
    if (s_slot_donor) return 0; /* v1 capacity: exactly one free slot */
    /* WadInitialize done? The boot read fills the TOC; entry 0
     * (Artisans) has a nonzero data length once loaded. */
    if (cpu->read_word(entry_addr(0) + 12u) == 0u) return 0;
    int donor_index = (donor_level_id / 10 - 1) * 6 + donor_level_id % 10;
    /* Seed the slot's TOC entry from the donor's - the loader then
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

/** @brief Starts a transition into a level from live gameplay, or hands off to the title-screen skip route.
 *
 * At the title screen (g_Gamestate 13) it calls game_set_skip_to_level. In
 * non-demo gameplay (g_Gamestate 0, g_DemoMode 0) it stores the
 * level-select cheat's words: g_NextLevelId = level_id, g_PortalLevelId 0,
 * g_HasLevelTransition 0, g_LoadStage 1, then g_Gamestate 1
 * (GS_LevelTransition). Any other state is refused so a load in progress is
 * not restarted.
 * @param cpu The guest CPU state.
 * @param level_id The destination level id; not range-checked.
 * @return 0 when queued, -1 when not in gameplay or in the attract demo. */
static int ops_enter_level(CPUState* cpu, int level_id) {
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    /* Title screen: the --skip-to-level machinery owns this route (same
     * hand-off the dev levelwarp uses) - it induces the demo-timeout
     * transition at the next Press-Start window WITH the demo-replay
     * downgrade. The cheat-path writes below would instead load the
     * level in attract-demo mode, whose desynced input replay strobes
     * the pause menu. */
    if (gs == 13u) {
        game_set_skip_to_level(level_id);
        return 0;
    }
    /* Otherwise only from live NON-DEMO gameplay - a mid-load
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

/** @brief Reads a level's stock data entry from the disc image through the WAD overlay reader.
 *
 * @param level_id The level id.
 * @param buf Destination, or NULL to return only the length.
 * @param cap Capacity of buf in bytes.
 * @return The byte length, or -1 on failure. */
static int ops_read_level_data(int level_id, void* buf, uint32_t cap) {
    return spyro1_mod_wad_read_level_data(level_id, buf, cap);
}

/** @brief Re-aims the claimed custom-level slot at a new donor level.
 *
 * Copies the donor's guest WAD TOC entry over the slot's (index 35), copies
 * the donor's XA track into the slot's row of the per-level track table
 * (D_8006EF9C), and makes the donor the id that alias_call swaps into
 * g_LevelId. The mod registry calls it when a different custom level takes
 * the shared slot.
 * @param cpu The guest CPU state.
 * @param donor_level_id The donor's level id, 10 to 64.
 * @return 0 on success, -1 when no slot is claimed or the id is out of
 *     range. */
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

/** @brief Writes the data length of a level's guest WAD TOC entry.
 *
 * Accepts level ids 10 to 65 whose index is in 0..35, and only once the
 * boot TOC read has populated entry 0. A zero `len` restores the stock
 * length read from the user's disc TOC, the donor's for the claimed slot.
 * @param cpu The guest CPU state.
 * @param level_id The level id.
 * @param len The data length in bytes, or 0 for the stock length.
 * @return 0 on success, -1 on a rejected id, an unpopulated TOC, or a
 *     failed disc read. */
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

int spyro1_level_resolve(const char* arg); /* levelwarp.c */
/** @brief Resolves a level name or numeric string to a level id for the mod registry.
 *
 * @param name A lower_snake level name, an alias or a decimal id.
 * @return The level id, or -1 when name is NULL, empty or unknown. */
static int ops_resolve_name(const char* name) {
    return name && name[0] ? spyro1_level_resolve(name) : -1;
}

/* ── In-game level titles ─────────────────────────────────────────────
 * g_LevelNames (char*[37] @0x8006F7F0) is the single name source: the
 * inventory rows and entry title cards (func_800181AC: A-Z 0-9 !,?.),
 * and the homeworld portal name arc (A-Z only - anything else renders
 * as an apostrophe there). Custom names live in the mods low-park
 * window (real low guest RAM, savestate-carried). */
#define ADDR_G_LEVELNAMES 0x8006F7F0u

/** @brief Maps a level id to its index in the per-level guest tables.
 *
 * Id 65 maps to the free slot index 35; ids 10 to 64 map to
 * (id / 10 - 1) * 6 + id % 10, without checking that the units digit is 0
 * to 5.
 * @param level_id The level id.
 * @return The index, or -1 for any other id. */
static int level_index_of(int level_id) {
    if (level_id == SLOT_LEVEL_ID) return SLOT_INDEX;
    if (level_id < 10 || level_id > 64) return -1;
    return (level_id / 10 - 1) * 6 + level_id % 10;
}

/* The count tables as the EXE shipped them, snapshotted before any guest
 * instruction runs. Capturing lazily on first write instead would read
 * whatever is in RAM at that moment, which a savestate load can have
 * replaced with a previously-declared value - the stock row would then be
 * lost for the rest of the session. */
static uint8_t  s_stock_dragons[COUNT_TABLE_ROWS];
static uint16_t s_stock_gems[COUNT_TABLE_ROWS];
static uint8_t  s_stock_eggs[EGG_TABLE_ROWS];
static int      s_stock_captured;

/** @brief Snapshots the shipped dragon, gem and egg target tables before any guest code runs.
 *
 * Copies 36 dragon bytes, 36 gem halfwords and 18 egg bytes into host
 * arrays that ops_set_level_counts restores from, and marks them captured.
 * @param cpu The guest CPU state. */
void spyro1_mod_level_capture_stock_counts(CPUState* cpu) {
    for (int i = 0; i < COUNT_TABLE_ROWS; i++) {
        s_stock_dragons[i] =
            (uint8_t)cpu->read_byte(ADDR_G_TARGETDRAGONS + (uint32_t)i);
        s_stock_gems[i] =
            (uint16_t)cpu->read_half(ADDR_G_TARGETGEMS + 2u * (uint32_t)i);
    }
    for (int i = 0; i < EGG_TABLE_ROWS; i++)
        s_stock_eggs[i] =
            (uint8_t)cpu->read_byte(ADDR_G_TARGETEGGS + (uint32_t)i);
    s_stock_captured = 1;
}

/** @brief Reconciles one level's dragon, gem, and egg target totals in guest RAM.
 *
 * A non-negative value is written; a negative one restores the value the
 * table held at boot, which is how a row that stops being live gives its
 * index back. `eggs` is ignored for an index with no row in the egg table.
 * Only differing bytes are written, so a call on tables that already agree
 * costs three reads and no write.
 * @param cpu The guest CPU state.
 * @param level_id The level id (10 to 64, or the custom slot's 65).
 * @param dragons The dragon total, or negative for the stock value.
 * @param gems The gem total, or negative for the stock value.
 * @param eggs The egg total, or negative for the stock value.
 * @return The number of bytes written, or -1 for an id with no count row
 *     or before the stock tables were captured. */
static int ops_set_level_counts(CPUState* cpu, int level_id,
                                int dragons, int gems, int eggs) {
    int idx = level_index_of(level_id);
    if (idx < 0 || idx >= COUNT_TABLE_ROWS || !s_stock_captured) return -1;

    int wrote = 0;
    uint32_t a = ADDR_G_TARGETDRAGONS + (uint32_t)idx;
    uint8_t want8 = dragons >= 0 ? (uint8_t)dragons : s_stock_dragons[idx];
    if ((uint8_t)cpu->read_byte(a) != want8) {
        cpu->write_byte(a, want8);
        wrote++;
    }

    a = ADDR_G_TARGETGEMS + 2u * (uint32_t)idx;
    uint16_t want16 = gems >= 0 ? (uint16_t)gems : s_stock_gems[idx];
    if ((uint16_t)cpu->read_half(a) != want16) {
        cpu->write_half(a, want16);
        wrote += 2;
    }

    if (idx < EGG_TABLE_ROWS) {
        a = ADDR_G_TARGETEGGS + (uint32_t)idx;
        uint8_t wantE = eggs >= 0 ? (uint8_t)eggs : s_stock_eggs[idx];
        if ((uint8_t)cpu->read_byte(a) != wantE) {
            cpu->write_byte(a, wantE);
            wrote++;
        }
    }
    return wrote;
}

static uint8_t s_name_override[37]; /* indices our titles occupy */
static void leveltext_hook_install(void);

/** @brief Shows a custom title for a level in the guest name table, or restores the stock name.
 *
 * Uppercases the name, keeps A-Z, 0-9, space and !,?.' and drops the rest
 * (at most 39 characters). The string is parked once in the guest arena
 * (the first 8 distinct titles are cached and reused; any later title is
 * allocated on every call) and its pointer stored in g_LevelNames; the
 * stock pointer is saved on the first write per index. A title installs the
 * portal-text glyph hook.
 * @param cpu The guest CPU state.
 * @param level_id The level id (10 to 64, or 65 for the slot).
 * @param name The title, or NULL to restore the stock name.
 * @return 0 on success, -1 for an unknown id, a title empty after
 *     cleaning, or an arena allocation failure. */
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
        s_name_override[idx] = 0;
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
        /* The title table holds full pointers, so the string can live in
         * the arena; parked once and never rewritten. */
        gaddr = openpete_mods_guest_alloc((uint32_t)n + 1u, 4u,
                                          OP_GALLOC_IMMUTABLE, &host,
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
    s_name_override[idx] = 1;
    leveltext_hook_install();
    OPL_INFO_F("mods", "level title: index %d -> \"%s\" (@0x%08X)", idx,
               clean, gaddr);
    return 0;
}

/* ── Portal-text glyphs beyond A-Z ────────────────────────────────────
 * The portal name arc (func_8003C358) maps only 'A'-'Z' to glyph mobys;
 * every other character spawns the apostrophe glyph. The digit/period
 * glyph MODELS exist (the inventory/title-card renderer uses them), so
 * for names WE installed this post-pass rebrands the apostrophe mobys
 * the stock function just spawned to the right glyph class - matched by
 * MobyLetterProps {m_Parent @+0, m_Index @+6} - and undoes the
 * apostrophe's +256 z lift. Stock names never enter the pass. */
#define ADDR_LEVELTEXT_FN 0x8003C358u
#define ADDR_G_LEVELMOBYS 0x80075828u
#define ADDR_G_MODELS     0x80076378u
#define ADDR_INIT_NORMAL  0x800526A8u
#define ADDR_INIT_SHADED  0x800529CCu
#define MOBY_STRIDE       0x58u
#define MOBY_OFF_PROPS    0x00u
#define MOBY_OFF_POS_Z    0x14u
#define MOBY_OFF_CLASS    0x36u
#define MOBY_OFF_STATE    0x48u
#define MOBY_POOL_CAP     512
#define CLASS_APOSTROPHE  76
#define CLASS_DIGIT_0     260
#define CLASS_PERIOD      327
#define CLASS_LETTER_A    426

#define ADDR_G_PORTALS     0x80078640u
#define ADDR_G_PORTALCOUNT 0x800758BCu
#define PORTAL_M_SKYBOX    0u
#define PORTAL_M_LEVELID   28u

/** @brief Maps a title character to the moby class of its glyph model, for digits and the period.
 *
 * @param c A character of a level title.
 * @return CLASS_DIGIT_0 + digit for '0' to '9', CLASS_PERIOD for '.',
 *     else 0. */
static int glyph_class_for(char c) {
    if (c >= '0' && c <= '9') return CLASS_DIGIT_0 + (c - '0');
    if (c == '.') return CLASS_PERIOD;
    return 0;
}

/** @brief Rebrands the portal-name apostrophe mobys of an installed title to digit and period glyphs.
 *
 * Mod override chain link on func_8003C358 (the portal name arc; a0 = parent
 * moby, a1 nonzero for a name). After the base call, for a level index whose
 * title ops_set_level_name replaced, each apostrophe moby the arc spawned
 * for that parent whose character is a digit or period is moved to an
 * unused letter class: the glyph model is copied into that class's g_Models
 * row and the moby's class is set to it, with the apostrophe's +256 z lift
 * undone. Letters used by the level's portal names and by earlier claims
 * are skipped; claims reset when g_LevelId changes.
 * @param cpu The guest CPU state, carrying the call's arguments. */
static void leveltext_hook(CPUState* cpu) {
    uint32_t parent  = cpu->a0;
    uint32_t is_name = cpu->a1;
    openpete_mods_base(cpu);
    if (!is_name || !parent) return;
    uint32_t pprops = cpu->read_word(parent + MOBY_OFF_PROPS);
    if (!pprops) return;
    uint32_t idx = cpu->read_word(pprops);
    if (idx > 36u || !s_name_override[idx]) return;
    uint32_t str = cpu->read_word(ADDR_G_LEVELNAMES + 4u * idx);
    if (!str) return;
    char name[40];
    int len = 0;
    while (len < (int)sizeof name - 1) {
        char c = (char)cpu->read_byte(str + (uint32_t)len);
        if (!c) break;
        name[len++] = c;
    }
    /* Letter-class transplant. Rebranding straight to the HUD digit
     * classes (260+) KILLS the moby - their update cases in the homeworld
     * overlays are HUD-only and self-destruct as level mobys (m_State
     * 0xFE) - and classes outside the letter family miss the letter pose
     * update (the period rendered flat). So: claim letter classes UNUSED
     * by any portal name in this level, transplant the digit/period
     * glyph MODEL onto their g_Models rows, and rebrand to those - full
     * letter behavior (alive, posed) with the digit's look. Rows and the
     * claim map are level-scoped. */
    static uint32_t s_claim_level = 0xFFFFFFFFu;
    static char     s_claim_ch[12];
    static int      s_claim_cls[12];
    static int      s_claim_n = 0;
    uint32_t cur_level = cpu->read_word(ADDR_G_LEVELID);
    if (cur_level != s_claim_level) {
        s_claim_level = cur_level;
        s_claim_n = 0;
    }

    /* Letters any portal name here (or RETURN HOME) actually uses. */
    uint8_t used[26];
    memset(used, 0, sizeof used);
    uint32_t np = cpu->read_word(ADDR_G_PORTALCOUNT);
    for (uint32_t p = 0; p <= 6u; p++) {
        uint32_t nstr;
        if (p < np && np <= 6u) {
            uint32_t portal = cpu->read_word(ADDR_G_PORTALS + 4u * p);
            if (!portal) continue;
            int lid = (int)cpu->read_word(portal + PORTAL_M_LEVELID);
            int li = level_index_of(lid);
            if (li < 0) continue;
            nstr = cpu->read_word(ADDR_G_LEVELNAMES + 4u * (uint32_t)li);
        } else if (p == 6u) {
            nstr = cpu->read_word(ADDR_G_LEVELNAMES + 4u * 36u); /* RETURN HOME */
        } else {
            continue;
        }
        for (int k = 0; k < 40 && nstr; k++) {
            char c = (char)cpu->read_byte(nstr + (uint32_t)k);
            if (!c) break;
            if (c >= 'A' && c <= 'Z') used[c - 'A'] = 1;
        }
    }
    for (int k = 0; k < s_claim_n; k++)
        used[s_claim_cls[k] - CLASS_LETTER_A] = 1;

    uint32_t pool = cpu->read_word(ADDR_G_LEVELMOBYS);
    if (!pool) return;
    int rebrands = 0;
    for (int s = 0; s < MOBY_POOL_CAP; s++, pool += MOBY_STRIDE) {
        uint8_t state = cpu->read_byte(pool + MOBY_OFF_STATE);
        if (state == 0xFFu) break;
        if (state >= 0x80u) continue;
        if (cpu->read_half(pool + MOBY_OFF_CLASS) != CLASS_APOSTROPHE)
            continue;
        uint32_t lp = cpu->read_word(pool + MOBY_OFF_PROPS);
        if (!lp || cpu->read_word(lp) != parent) continue;
        int ci = (int)(int16_t)cpu->read_half(lp + 6u);
        if (ci < 0 || ci >= len) continue;
        int src = glyph_class_for(name[ci]);
        if (!src) continue; /* a genuine apostrophe (or unmapped) */

        int cls = 0;
        for (int k = 0; k < s_claim_n && !cls; k++)
            if (s_claim_ch[k] == name[ci]) cls = s_claim_cls[k];
        if (!cls) {
            uint32_t mdl = cpu->read_word(ADDR_G_MODELS + 4u * (uint32_t)src);
            if (!mdl) continue; /* glyph model not resident here */
            /* Rarity-ordered free-letter scan. */
            static const char order[] = "QZXJVKWYGPBFUMDHCLNRTOAISE";
            for (int k = 0; order[k] && !cls; k++) {
                int L = order[k] - 'A';
                if (!used[L]) {
                    cls = CLASS_LETTER_A + L;
                    used[L] = 1;
                }
            }
            if (!cls) continue; /* no free letter class (unlikely) */
            cpu->write_word(ADDR_G_MODELS + 4u * (uint32_t)cls, mdl);
            if (s_claim_n < (int)sizeof s_claim_ch) {
                s_claim_ch[s_claim_n] = name[ci];
                s_claim_cls[s_claim_n] = cls;
                s_claim_n++;
            }
        }
        cpu->write_half(pool + MOBY_OFF_CLASS, (uint16_t)cls);
        cpu->write_word(pool + MOBY_OFF_POS_Z,
                        cpu->read_word(pool + MOBY_OFF_POS_Z) - 256u);
        rebrands++;
    }
    if (rebrands)
        OPL_INFO_F("mods", "portal text: %d glyph%s rebranded for level "
                   "index %u (%d letter row%s claimed)", rebrands,
                   rebrands == 1 ? "" : "s", idx, s_claim_n,
                   s_claim_n == 1 ? "" : "s");
}

/** @brief Chains leveltext_hook onto func_8003C358 once.
 *
 * A failed chain add is retried on the next call. */
static void leveltext_hook_install(void) {
    static int s_installed = 0;
    if (s_installed) return;
    if (openpete_mods_chain_add(ADDR_LEVELTEXT_FN, leveltext_hook,
                                "level-registry") == 0)
        s_installed = 1;
}

/* ── Replace-mode portal skybox ───────────────────────────────────────
 * A homeworld portal's preview dome is a copy of the destination's sky
 * BAKED INTO THE HOMEWORLD'S OWN LEVEL DATA (loaders.c: each Portal
 * carries copyBgColor/copySectorCount/copySectors, pointer-patched at
 * load) - replacing the destination level never touches it. While a
 * replace is declared, this driver parses the custom level's own
 * cyclorama component out of the mod's .WAD data segment (LevelHeader
 * m_DataOffset @+8 -> skip 5 self-sizing components -> cyclorama:
 * [size][bg word][sector count][payload-relative sector ptrs]...),
 * parks the payload in the guest arena with the pointers absolutized,
 * and repoints the donor portal's three preview fields at it. The
 * native portal feeder re-derives from these same guest structures, so
 * the substitution flows through both render paths. */
#define SKY_SECTORCOUNT    0u
#define SKY_SECTORS        4u
#define SKY_BGCOLOR        16u

typedef struct {
    char     stem[64];
    uint32_t gaddr;   /* arena copy of the cyclorama payload; 0 = bad */
    uint32_t count;
    uint32_t bgword;
} sky_park_t;

/** @brief Loads a custom level's cyclorama payload into the guest arena and returns its parked record.
 *
 * Reads <dir>/<stem> from the mod, walks the level data header past five
 * components to the cyclorama, copies its payload to an immutable guest
 * arena block, and rewrites the payload-relative sector pointers to
 * absolute guest addresses. At most 8 stems are cached, by stem; a failure
 * is cached too, so it is not retried.
 * @param dir The mod directory.
 * @param stem The custom level's file stem.
 * @return The parked record, or NULL when the file is missing or malformed,
 *     the arena allocation fails, or the cache is full. */
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
    openpete_mods_custom_asset_path(dir, stem, 0, path, sizeof path);
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
        openpete_mods_guest_alloc(paylen, 16u, OP_GALLOC_IMMUTABLE, &host,
                                  "portal-sky");
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

static void replace_transition_sky(CPUState* cpu, int donor_id,
                                   const char* dir, const char* stem);

/** @brief Points the donor level's homeworld portal preview at a custom level's sky, or restores it.
 *
 * Per-tick driver. While on, it also runs replace_transition_sky, finds the
 * portal whose level id is the donor, saves that portal's sector count,
 * sector pointer and background word once, and stores the parked
 * cyclorama's values. When off it restores the saved values. Patch state
 * resets when g_LevelId changes (the portals are rebuilt on load). No
 * portal is patched or restored while g_PortalCount exceeds 6.
 * @param cpu The guest CPU state.
 * @param donor_id The replaced retail level's id.
 * @param dir The mod directory.
 * @param stem The custom level's file stem.
 * @param on Nonzero to patch, zero to restore.
 * @return 0, or -1 when the sky payload cannot be parked. */
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

    if (on) replace_transition_sky(cpu, donor_id, dir, stem);

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

#define ADDR_G_CYCLORAMA     0x80078A40u
#define ADDR_ENTERED_PORTAL  0x8007576Cu /* D_8007576C: portal idx, -1 = none */

/** @brief Re-points g_Cyclorama at the custom sky while a portal entry transitions into the donor level.
 *
 * Load stage 0 of a portal entry Memcpy's the portal's raw baked blob into
 * scratch and points g_Cyclorama at the blob's tail copy of the preview
 * (bg word, count, inline pointer array), a second unpatched copy of the
 * stock sky that fills the screen for about a second. Patching the blob's
 * inline pointers is unsafe (stage 0 rebases them assuming they point
 * inside the blob), so the parked copy is stored into g_Cyclorama every
 * tick instead. Acts only in g_Gamestate 1 with g_NextLevelId == donor_id
 * and a portal entry (D_8007576C >= 0); once the loaded level's own
 * cyclorama takes over, the writes are content-identical.
 * @param cpu The guest CPU state.
 * @param donor_id The replaced retail level's id.
 * @param dir The mod directory.
 * @param stem The custom level's file stem. */
static void replace_transition_sky(CPUState* cpu, int donor_id,
                                   const char* dir, const char* stem) {
    if (cpu->read_word(ADDR_G_GAMESTATE) != 1u) return; /* transition? */
    if (cpu->read_word(ADDR_G_NEXTLEVELID) != (uint32_t)donor_id) return;
    /* Portal entries only (D_8007576C = entered-portal index, -1 when
     * the transition came from a menu/cheat fade - those correctly show
     * the PREVIOUS level's sky, not the destination's). */
    if ((int32_t)cpu->read_word(ADDR_ENTERED_PORTAL) < 0) return;
    const sky_park_t* pk = sky_park_get(dir, stem);
    if (!pk) return;
    if (cpu->read_word(ADDR_G_CYCLORAMA + 4u) == pk->gaddr + 8u) return;
    cpu->write_word(ADDR_G_CYCLORAMA + 0u, pk->count);
    cpu->write_word(ADDR_G_CYCLORAMA + 4u, pk->gaddr + 8u);
    cpu->write_word(ADDR_G_CYCLORAMA + 16u, pk->bgword);
    OPL_INFO_F("mods", "portal sky: transition into %d riding %s",
               donor_id, stem);
}

static const openpete_level_ops_t s_spyro1_level_ops = {
    .claim_slot       = ops_claim_slot,
    .retarget_slot    = ops_retarget_slot,
    .declare_data_len = ops_declare_data_len,
    .resolve_name     = ops_resolve_name,
    .enter_level      = ops_enter_level,
    .read_level_data  = ops_read_level_data,
    .set_level_name   = ops_set_level_name,
    .set_level_counts = ops_set_level_counts,
    .replace_sky_tick = ops_replace_sky_tick,
};

/** @brief Returns Spyro 1's level ops, the game descriptor's level_ops member.
 *
 * @return The static ops table. */
const openpete_level_ops_t* spyro1_level_ops(void) {
    return &s_spyro1_level_ops;
}
