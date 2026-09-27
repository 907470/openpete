// games/spyro-1/features/credits_skip.c — `--skip-to-cutscene` for Spyro 1.
//
// Each target is entered through the game's own transition, from a hook
// that makes only the stores the game's trigger branch makes:
//
// intro (g_CutsceneIdx 1). New Game in the title's TSM_Menu calls
// func_8001277C (progress, lives and dragons reset), then sets m_Mode
// TSM_Loading, m_State 5, m_DemoType TSD_Cutscene and
// g_FairyCutscene.m_HasMemoryCard 0 (titlescreen.c, the no-save branch).
// The hook reproduces that branch from the GamestateDraw (0x8001ED5C) entry
// at the Press-Start wait (g_Gamestate 13, m_Mode TSM_Init, m_SubState 3),
// after the tick's whole update. It also stores m_SubTick 0: the title
// update's end-of-tick code stores 0 there when m_State changed, and on the
// hook tick that code already ran against the wait's state, so without the
// store state 5's sound and fade (keyed on m_SubTick 0) would start from the
// wait's count. From the next tick the title's state 5 fades, calls
// MemCardStop and enters TSM_Demo, whose TSS_Setup takes TSD_Cutscene
// (g_CutsceneIdx 1) and the cutscene transition runs as after New Game.
//
// ending / ending_loot (g_CutsceneIdx 2 / 3). The boss overlays of
// Gnasty's Lair (63) and Gnasty's Loot (64) enter GS_Cutscene inline in a
// moby update (func_level_63_8007AD64 at 0x8007F970, func_level_64_8007AD4C
// at 0x8008073C), so no guest function exists to call. After a
// --skip-to-level to that level, the hook makes the branch's stores from the
// GamestateDraw entry on the first live tick: level 63 stores
// g_LevelVortexExitFlags[g_LevelIndex] = 1, g_CreditsSequence 0,
// g_CutsceneIdx 2, g_Gamestate 14, g_StateSwitch 1 ($fp), g_ScreenBorderEnabled
// 0, g_IsSpyroHidden 0 and a zero word through g_CutsceneLayout; level 64
// the flag, g_CutsceneIdx 3, g_Gamestate 14, g_StateSwitch 1 and the zero
// word. main skips GamestateDraw on a tick that set g_StateSwitch, so the
// hook returns 1; the next tick's GamestateUpdate clears g_StateSwitch and
// runs GS_Cutscene tick 0, which loads the WAD slot inside the game fiber.
// The trigger predicates (boss word, gem total, Spyro state,
// func_80037F90) are not reproduced. ending_loot also stores D_80075818 =
// 10: credits stage 99 returns to that level when it is not -1, which is the
// Artisans return the game's path takes at 14000 gems; the skip leaves
// g_GemTotal at the fresh session's value. LoadLevel stage 13 resets
// D_80075818 to -1 on every load, so the store is made after arrival.
//
// credits / credits_100. The game reaches its credits through
// InitCreditsSequence (0x8002D228), which clears the framebuffer, kills
// sound, resets Spyro, sets g_Gamestate = GS_Credits and CDLoadSync's the
// credits overlay over the active one. Its cheat-code caller (cheats.c,
// CheatApplyEffect case 2) additionally stashes g_LevelId in D_80075818 so
// the roll's final stage (func_800333DC, stage 99) reloads the level it was
// entered from; -1 there sends the player to Gnorc Gnexus, or Artisans at
// 14000 gems. A credits skip is a --skip-to-level (the flag's own target,
// else Artisans) followed by that cheat effect on the first live tick. The
// call runs inside the game fiber (CDLoadSync yields on the CD), from a
// dispatch hook on GamestateUpdate (0x8003385C). The hook falls through, so
// the same tick's GamestateUpdate already runs the credits state, one tick
// earlier than the cheat, whose effect lands mid-update.
//
// Every hook is inert until armed (the attract demo never arms one) and
// does nothing on a speculative (runahead) tick: a request acted on there
// is rolled back with RAM while the host latch would read it as done.

#include "features/credits_skip.h"

#include "feature_registry.h"
#include "named_target.h"
#include "openpete_fiber.h" /* openpete_speculative, openpete_frame */
#include "openpete_log.h"
#include "psx_runtime_internal.h" /* CPUState + its read/write callbacks */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ADDR_GAMESTATE_UPDATE      0x8003385Cu
#define ADDR_GAMESTATE_DRAW        0x8001ED5Cu
#define ADDR_INIT_CREDITS_SEQUENCE 0x8002D228u
#define ADDR_FUNC_8001277C         0x8001277Cu /* progress/lives/dragons reset */

#define ADDR_G_GAMESTATE           0x800757D8u /* GS_Playing 0, GS_TitleScreen 13,
                                                  GS_Cutscene 14, GS_Credits 15 */
#define ADDR_G_LEVELID             0x8007596Cu
#define ADDR_G_DEMOMODE            0x80075714u /* DEMO_MODE_NONE = 0 */
#define ADDR_G_CREDITSSEQUENCE     0x800757ACu /* 0 = short roll, 10 = full */
#define ADDR_D_80075818            0x80075818u /* level to reload after the roll */
#define ADDR_D_800756C4            0x800756C4u /* overlay delta time */
#define ADDR_G_CUTSCENEIDX         0x8007566Cu
#define ADDR_G_CUTSCENELAYOUT      0x80075680u /* pointer */
#define ADDR_G_STATESWITCH         0x8007579Cu
#define ADDR_G_LEVELINDEX          0x80075964u
#define ADDR_G_LEVELVORTEXEXITFLAGS 0x8007A6A8u /* byte per level index */
#define ADDR_G_SCREENBORDERENABLED 0x8007570Cu
#define ADDR_G_ISSPYROHIDDEN       0x80075814u
#define ADDR_G_TITLESCREENSTATE    0x80078D78u
#define TS_MODE                    0x00u /* TSM_Init 0, TSM_Loading 2 */
#define TS_STATE                   0x04u
#define TS_SUBTICK                 0x0Cu
#define TS_SUBSTATE                0x10u /* TSM_Init 3: the Press-Start wait */
#define TS_DEMOTYPE                0x1Cu /* TSD_Cutscene 0 */
#define ADDR_FAIRY_HAS_MEMORY_CARD (0x80078D00u + 0x14u) /* g_FairyCutscene */

#define GS_PLAYING     0u
#define GS_TITLESCREEN 13u
#define GS_CUTSCENE    14u
#define GS_CREDITS     15u

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime.h */

/* One row per name. `source`: the level the row enters from (0 = the title,
 * -1 = any live level). `cutscene_idx`: g_CutsceneIdx at release (0 for the
 * credits rows, which release on GS_Credits). */
typedef struct {
    const char* name;
    int         id;
    int         source;
    uint32_t    cutscene_idx;
} cutscene_t;

static const cutscene_t k_cutscenes[] = {
    { "intro",        SPYRO1_CUTSCENE_INTRO,         0, 1u },
    { "ending",       SPYRO1_CUTSCENE_ENDING,       63, 2u },
    { "ending_loot",  SPYRO1_CUTSCENE_ENDING_LOOT,  64, 3u },
    { "credits",      SPYRO1_CUTSCENE_CREDITS,      -1, 0u },
    { "credits_100",  SPYRO1_CUTSCENE_CREDITS_FULL, -1, 0u },
    { "full_credits", SPYRO1_CUTSCENE_CREDITS_FULL, -1, 0u },
};

static const cutscene_t* row_of(int id) {
    for (size_t i = 0; i < sizeof(k_cutscenes) / sizeof(k_cutscenes[0]); i++)
        if (k_cutscenes[i].id == id) return &k_cutscenes[i];
    return NULL;
}

int spyro1_cutscene_resolve(const char* arg) {
    const named_target_table_t cutscenes =
        NAMED_TARGET_TABLE(k_cutscenes, cutscene_t, name, id);
    return named_target_resolve(arg, "cutscene", &cutscenes, NULL, 0u);
}

int spyro1_cutscene_source_level(int id) {
    const cutscene_t* r = row_of(id);
    return r ? r->source : -1;
}

static const cutscene_t* s_row      = NULL; /* armed row, NULL when idle */
static int               s_induced  = 0;    /* the row's hook has acted */
static int               s_watching = 0;

void spyro1_cutscene_skip_arm(int id) {
    s_row      = row_of(id);
    s_induced  = 0;
    s_watching = s_row != NULL;
}

int  spyro1_cutscene_skip_watching(void) { return s_watching; }
void spyro1_cutscene_skip_finish(void)   { s_watching = 0; }

static int row_is_credits(const cutscene_t* r) {
    return r->id == SPYRO1_CUTSCENE_CREDITS ||
           r->id == SPYRO1_CUTSCENE_CREDITS_FULL;
}

int spyro1_cutscene_skip_released(CPUState* cpu, char* eng, size_t cap) {
    if (!s_row || !s_induced) return 0;
    uint32_t gs = cpu->read_word(ADDR_G_GAMESTATE);
    if (row_is_credits(s_row)) {
        if (gs != GS_CREDITS) return 0;
        snprintf(eng, cap, " cutscene=%s credits_sequence=%u return_level=%u",
                 s_row->name, (unsigned)cpu->read_word(ADDR_G_CREDITSSEQUENCE),
                 (unsigned)cpu->read_word(ADDR_D_80075818));
        return 1;
    }
    uint32_t idx = cpu->read_word(ADDR_G_CUTSCENEIDX);
    if (gs != GS_CUTSCENE || idx != s_row->cutscene_idx) return 0;
    snprintf(eng, cap, " cutscene=%s g_CutsceneIdx=%u return_level=%d",
             s_row->name, (unsigned)idx,
             (int32_t)cpu->read_word(ADDR_D_80075818));
    return 1;
}

/** @brief Reports whether the hook for a cutscene id may act on this tick.
 *
 * A row's hook acts once, outside runahead: the row must be armed, not yet
 * induced, and the tick not speculative.
 * @param id A SPYRO1_CUTSCENE_* id.
 * @return 1 when the hook may act, else 0. */
static int hook_live(int id) {
    return s_row && s_row->id == id && !s_induced && !openpete_speculative;
}

/** @brief Reports whether this is a live tick of the row's source level with the overlay delta time set.
 *
 * Live means g_Gamestate GS_Playing, a nonzero g_LevelId equal to `source`
 * when `source` is positive, no attract demo, and a nonzero D_800756C4.
 * @param cpu The guest CPU state.
 * @param source The required level id, or -1 for any level.
 * @return 1 when every condition holds, else 0. */
static int first_live_tick(CPUState* cpu, int source) {
    if (cpu->read_word(ADDR_G_GAMESTATE) != GS_PLAYING) return 0;
    uint32_t level = cpu->read_word(ADDR_G_LEVELID);
    if (level == 0u || (source > 0 && level != (uint32_t)source)) return 0;
    if (cpu->read_word(ADDR_G_DEMOMODE) != 0u) return 0;
    /* The credits overlay ticks its entry timers by D_800756C4, the delta
     * time the level overlays' moby update loop copies from g_DeltaTime; the
     * credits never write it. Fresh from boot it is 0 and every timer
     * stalls, so wait for a gameplay tick to have set it, as the pad code
     * (entered mid-session) always finds it set. */
    return cpu->read_word(ADDR_D_800756C4) != 0u;
}

static int credits_hook(CPUState* cpu) {
    int full = s_row->id == SPYRO1_CUTSCENE_CREDITS_FULL;
    uint32_t level = cpu->read_word(ADDR_G_LEVELID);
    s_induced = 1;
    cpu->write_word(ADDR_D_80075818, level);
    if (full) cpu->write_word(ADDR_G_CREDITSSEQUENCE, 10u);

    /* GamestateUpdate takes no arguments and this is its call boundary, so
     * only the return address needs preserving across the nested call. */
    uint32_t ra = cpu->ra;
    cpu->a0 = full ? 1u : 0u;
    call_by_address(cpu, ADDR_INIT_CREDITS_SEQUENCE);
    cpu->ra = ra;

    OPL_INFO_F("skip-to-cutscene", "%s credits started from level %u; the "
               "roll returns there", full ? "full (100%)" : "short",
               (unsigned)level);
    return 0;
}

static int on_gamestate_update(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!(hook_live(SPYRO1_CUTSCENE_CREDITS) ||
          hook_live(SPYRO1_CUTSCENE_CREDITS_FULL)))
        return 0;
    if (!first_live_tick(cpu, -1)) return 0;
    return credits_hook(cpu);
}

/** @brief Makes New Game's no-save branch stores at the title's Press-Start wait.
 *
 * Reproduces titlescreen.c's TSM_Menu sub-state 4 branch: calls
 * func_8001277C, then stores m_Mode TSM_Loading, m_State 5, m_DemoType
 * TSD_Cutscene, g_FairyCutscene.m_HasMemoryCard 0, and m_SubTick 0 (see the
 * file header). Acts only at g_Gamestate 13, m_Mode TSM_Init, m_SubState 3.
 * @param cpu The guest CPU state.
 * @return 0, so the guest GamestateDraw runs. */
static int intro_hook(CPUState* cpu) {
    const uint32_t ts = ADDR_G_TITLESCREENSTATE;
    if (cpu->read_word(ADDR_G_GAMESTATE) != GS_TITLESCREEN ||
        cpu->read_word(ts + TS_MODE) != 0u ||
        cpu->read_word(ts + TS_SUBSTATE) != 3u)
        return 0;
    s_induced = 1;
    uint32_t ra = cpu->ra;
    call_by_address(cpu, ADDR_FUNC_8001277C);
    cpu->ra = ra;
    cpu->write_word(ts + TS_MODE, 2u);     /* m_Mode = TSM_Loading */
    cpu->write_word(ts + TS_STATE, 5u);    /* m_State = 5 */
    cpu->write_word(ts + TS_DEMOTYPE, 0u); /* m_DemoType = TSD_Cutscene */
    cpu->write_word(ADDR_FAIRY_HAS_MEMORY_CARD, 0u);
    cpu->write_word(ts + TS_SUBTICK, 0u);  /* m_SubTick = 0 */
    OPL_INFO_F("skip-to-cutscene", "intro: New Game induced at the "
               "Press-Start wait at frame=%u", openpete_frame);
    return 0;
}

/** @brief Makes the boss branch's GS_Cutscene stores on the first live tick of the row's level.
 *
 * The store list per level is in the file header.
 * @param cpu The guest CPU state.
 * @return 1 after the stores (g_StateSwitch 1: main skips this tick's
 *     GamestateDraw), 0 when the tick is not yet live. */
static int ending_hook(CPUState* cpu) {
    int loot = s_row->id == SPYRO1_CUTSCENE_ENDING_LOOT;
    if (!first_live_tick(cpu, s_row->source)) return 0;
    s_induced = 1;
    uint32_t layout = cpu->read_word(ADDR_G_CUTSCENELAYOUT);
    uint32_t index  = cpu->read_word(ADDR_G_LEVELINDEX);
    cpu->write_byte(ADDR_G_LEVELVORTEXEXITFLAGS + index, 1u);
    if (!loot) cpu->write_word(ADDR_G_CREDITSSEQUENCE, 0u);
    cpu->write_word(ADDR_G_CUTSCENEIDX, s_row->cutscene_idx);
    cpu->write_word(ADDR_G_GAMESTATE, GS_CUTSCENE);
    cpu->write_word(ADDR_G_STATESWITCH, 1u);
    if (!loot) {
        cpu->write_word(ADDR_G_SCREENBORDERENABLED, 0u);
        cpu->write_word(ADDR_G_ISSPYROHIDDEN, 0u);
    }
    cpu->write_word(layout, 0u);
    if (loot) cpu->write_word(ADDR_D_80075818, 10u); /* the Artisans return */
    OPL_INFO_F("skip-to-cutscene", "%s: GS_Cutscene %u entered from level "
               "%d at frame=%u", s_row->name, (unsigned)s_row->cutscene_idx,
               s_row->source, openpete_frame);
    /* g_StateSwitch 1: main skips this tick's GamestateDraw. */
    return 1;
}

static int on_gamestate_draw(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (hook_live(SPYRO1_CUTSCENE_INTRO)) return intro_hook(cpu);
    if (hook_live(SPYRO1_CUTSCENE_ENDING) ||
        hook_live(SPYRO1_CUTSCENE_ENDING_LOOT))
        return ending_hook(cpu);
    return 0;
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { ADDR_GAMESTATE_UPDATE, on_gamestate_update },
    { ADDR_GAMESTATE_DRAW,   on_gamestate_draw },
    { 0, NULL },
};

const feature_module_t credits_skip_module = {
    .name           = "credits-skip",
    .apply_options  = NULL,
    .dispatch_table = k_dispatch,
};
