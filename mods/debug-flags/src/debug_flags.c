/**
 * @file debug_flags.c
 * @brief debug-flags: a table of named guest-RAM writes, applied from buttons in the Mods panel.
 *
 * Each row of k_actions names one progress flag (or any other guest word)
 * the game keeps on the memory card, the game it belongs to, and the value
 * that marks it done. The Mods panel draws one button per row of the
 * running game, plus "Apply all", and beside each button the value the
 * flag held at the last tick.
 *
 * A button runs inside a present, where guest access is refused, so a
 * press only sets the row's bit in s_pending. The per-tick hook, on the
 * sim thread, writes every pending row before the game update runs and
 * clears a row's bit on the first tick that reads the target value back.
 * A write lost to a rolled-back runahead tick is therefore written again
 * on the next committed tick. The same hook copies each row's current
 * value into s_value for the panel.
 *
 * Adding an action is one row. A row for a game the mod has not loaded on
 * before also adds that game to mod.toml; every game already has its tick
 * binding below.
 *
 * A routine row (apply set) makes several stores found by walking the
 * running level's mobys instead of one fixed store; it stays queued while
 * the level it serves is not in play, and clears once its stores read back.
 *
 * Test trigger: the config.toml key `test.apply_tick = N` (no `[[config]]`
 * row, so the panel never shows it) queues every row at the N-th tick the
 * hook sees, which is how a headless run presses "Apply all". With
 * `test.apply_only = "<prefix>"` it queues only the rows whose name starts
 * with the prefix.
 */

#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>

#include <stdint.h>
#include <string.h>

/** One named write: guest address, width, target value, owning game. */
typedef struct {
    const char* name;   /**< Button label; unique among one game's rows. */
    const char* game;   /**< Game id the address belongs to. */
    uint32_t    addr;   /**< Guest virtual address. */
    uint32_t    width;  /**< Access width in bytes: 1, 2 or 4. */
    uint32_t    value;  /**< Value written; the row reads as applied when the address holds it. */
    const char* note;   /**< Tooltip: what the game does with the value. */
    /** Routine row, when non-NULL: replaces the single store. Called with
     *  write 0 it returns 1 when the effect is in place, 0 when it is not,
     *  -1 when the running game state cannot take it (another level, a
     *  load); called with write 1 it also makes its stores and returns the
     *  number made. addr, width and value are unused. */
    int (*apply)(int write);
} action_t;

static int glimmer_gem_lamps(int write);
static int atlas_page_stress(int write);

/* Spyro 2 0x800646AE: the halfword of entry 15 of the progress table at
 * 0x80064670. Ripto's Arena (level_66 moby_update, sh at 0x8007F740)
 * stores 1 there when Ripto is beaten; the save writer copies the table
 * into the card record. Winter Tundra's portal-gate object (class 0x16D)
 * tests it every tick and, when nonzero, enables the Dragon Shores portal
 * record and frees itself (level_50 moby_update 0x80076830-0x800768B8).
 *
 * Spyro 2 0x80064682, 0x80064686, 0x8006468A: the halfwords of progress
 * entries 4, 5 and 6, the swim, climb and headbash purchases. An entry's
 * low halfword is Moneybags' price and its high halfword the paid flag; the
 * dialogue script's pay opcode stores 1 there once the gem count covers the
 * price (func_8003A568, sh at 0x8003A708). The player code reads each flag
 * at the gate, every time, so a write takes effect without a reload:
 *   - 0x80064682 in the swim code each overlay with water carries (level_10
 *     spyro, lh at 0x8006FB48 and 0x8006FCC0): Square at the surface
 *     (state 0x2C) dives (state 0x28) only while it is nonzero. The
 *     water-entry test (func_8002A4C8, lh at 0x8002A67C) reads it too: a
 *     fall (state 0x15) or headbash into deep water goes under (state
 *     0x2A) while it is nonzero and stops at the surface while it is zero;
 *   - 0x80064686 at the head of the ladder test (func_8002D328, lh at
 *     0x8002D32C), which returns 0 at once while it is zero;
 *   - 0x8006468A in the airborne triangle test (func_80035D38, lh at
 *     0x800366B8 and 0x80036F38), which enters the headbash state 0x2E.
 *
 * Spyro 2 0x800698BB: the permanent superflame byte. The powerup gate's
 * case 10 stores 1 (level_11 moby_update sb at 0x80076030; the gate's code
 * is repeated in each overlay that places one); the flame button takes
 * the superflame shot (0x8006A15C = 6, no flame ribbon) while it or the
 * superflame powerup timer 0x8006A1C8 is nonzero (func_8002FC20, lbu at
 * 0x8002FE40). The save writer copies it into the card record and the
 * loader restores it (titlescreen_and_loading 0x80076834, 0x80076A8C); the
 * new-game reset of the 0x18-byte block at 0x800698B8 keeps it
 * (func_8001AAA4).
 *
 * Spyro 3 0x8006FB93: the same byte, g_CheatFlags + 3, with the same
 * powerup-gate case 10 writer (level_10 moby sb at 0x8007A754), the same
 * superflame test beside the timer at g_Spyro + 0x1D0 (func_80045D70, lbu
 * at 0x80046058) and the same new-game reset that keeps it
 * (func_80054E5C). */
static const action_t k_actions[] = {
    { "Ripto defeated (opens the Dragon Shores portal)", "spyro-2",
      0x800646AEu, 2u, 1u,
      "Spyro 2 0x800646AE = 1: Ripto's Arena sets it when Ripto is beaten; "
      "Winter Tundra's portal gate opens the Dragon Shores portal while it "
      "is nonzero. A save written afterwards keeps it.", NULL },
    { "Swim (dive underwater)", "spyro-2", 0x80064682u, 2u, 1u,
      "Spyro 2 0x80064682 = 1: the paid flag of Moneybags' swim lesson. "
      "While it is nonzero Square at the water's surface dives; at zero "
      "Spyro stays at the surface.", NULL },
    { "Climb (ladders)", "spyro-2", 0x80064686u, 2u, 1u,
      "Spyro 2 0x80064686 = 1: the paid flag of Moneybags' climb lesson. "
      "The ladder test returns at once while it is zero.", NULL },
    { "Headbash (triangle in the air)", "spyro-2", 0x8006468Au, 2u, 1u,
      "Spyro 2 0x8006468A = 1: the paid flag of Moneybags' headbash "
      "lesson. Triangle in the air starts the headbash while it is "
      "nonzero.", NULL },
    { "Permanent superflame", "spyro-2", 0x800698BBu, 1u, 1u,
      "Spyro 2 0x800698BB = 1: the powerup gate's permanent-superflame "
      "case sets it; the flame button fires the superflame shot while it "
      "is nonzero. The card record keeps it.", NULL },
    { "Permanent superflame", "spyro-3", 0x8006FB93u, 1u, 1u,
      "Spyro 3 0x8006FB93 = 1 (g_CheatFlags + 3): the powerup gate's "
      "permanent-superflame case sets it; the flame button fires the "
      "superflame shot while it is nonzero.", NULL },
    { "Glimmer gem lamps lit (both big gems emit)", "spyro-2", 0u, 0u, 1u,
      "Spyro 2 Glimmer: sets both big gems' six lamp bits and a flame hit "
      "on every unlit gem lamp; each gem then runs its own completion and "
      "the gem cutter's dialogue opens. Waits for Glimmer to be in play "
      "when pressed elsewhere.", glimmer_gem_lamps },
    { "Atlas page stress (synthetic progress)", "spyro-3", 0u, 0u, 1u,
      "Spyro 3 atlas: Sorceress defeated, 9999999 gems and every egg bit in "
      "every level. Synthetic values that lengthen every atlas row; applied "
      "only while the atlas overlay is resident. A save written afterwards "
      "keeps them.", atlas_page_stress },
};

#define N_ACTIONS ((int)(sizeof k_actions / sizeof k_actions[0]))

_Static_assert(N_ACTIONS <= 31, "s_pending holds one bit per row");

static const openpete_mod_api_t* g_api;   /**< Engine table, saved at entry. */
static openpete_mod_t*           g_self;  /**< This mod's handle, saved at entry. */

static int      s_rows[N_ACTIONS];  /**< Indices into k_actions for the running game. */
static int      s_n;                /**< Entries used in s_rows. */
static unsigned s_pending;          /**< Bit i: s_rows[i] queued; set by the panel, cleared by the tick hook. */
static uint32_t s_value[N_ACTIONS]; /**< Value s_rows[i]'s address held at the last tick. */
static unsigned s_seen;             /**< Nonzero once the tick hook has filled s_value. */
static int64_t  s_ticks;            /**< Ticks the hook has seen. */
static int64_t  s_test_tick;        /**< Tick at which every row is queued; 0 = never. */
static unsigned s_test_mask;        /**< Rows the test tick queues; 0 = every row. */

#define S2_MOBY_ARRAY   0x80066F14u /* pointer to the 0x58-byte moby records */
#define S2_MOBY_STRIDE  0x58u
#define S2_LEVEL_ID     0x80066F90u /* level number, 11 = Glimmer */
#define S2_LEVEL_TICK   0x80067008u /* ticks since the level began running */
#define S2_GLIMMER      11u
#define GL_LAMP_CLASS   0x21Cu      /* gem lamp holder */
#define GL_GEM_CLASS    0x40Cu      /* big gem */
#define GL_HIT_BIT      0x10000u    /* moby +0x18: flame hit */

/** @brief Completes both Glimmer gem-lamp puzzles through the game's own lamp and gem updates.
 *
 * Glimmer holds two copies of the puzzle, each one big gem (class 0x40C)
 * and six gem lamps (class 0x21C); on the stock layout gem moby 25 owns
 * lamps 16-21 and gem 28 (32236, 103178, 12902) owns lamps 29-34. A lamp's
 * data block (moby +0x00) holds its bit number at +0x00 and its gem's moby
 * index at +0x04. The lamp's update (level_11 moby_update 0x80079A94-
 * 0x80079B50) tests the flame-hit bit 0x10000 of its moby +0x18 while its
 * state (+0x48) is below 3, and on a hit ORs 1 << bit into the gem's +0x49
 * byte (sb at 0x80079AF4) and goes from state 0 to state 1, lit (sb at
 * 0x80079B1C). A lit lamp whose gem is still in state 0 goes dark when its
 * timer runs out, clearing its bit and returning to state 0
 * (0x8007A060-0x8007A168). The gem in state 0 compares +0x49 with 0x3F
 * and on a match starts its camera sequence and enters state 1
 * (0x8007AA48-0x8007AAA8); once the sequence word 0x80067ED4 reads 2 it
 * spawns its six orbiting mobys (class 0x230) and enters state 2, and from
 * states 2 and 3 it emits class 0x231 mobys (0x8007AAF0-0x8007AD90). Lit
 * lamps of a completed gem enter state 3 while the sequence word
 * 0x80067ED0 reads 11 and stay lit (0x80079F44-0x80079FB4).
 *
 * The update loop runs a moby only while it is drawn or inside its update
 * radius (func_80040E94), so a lamp or gem away from the camera updates
 * later than the tick of the write. The routine therefore writes the gem's
 * end value 0x3F rather than relying on six lamps updating together, and
 * sets the hit bit of every lamp still in state 0, each store only when the
 * value is not already there. The game's own code then makes every other
 * store of the completion, including the camera sequence and the gem
 * cutter's dialogue, which takes Cross presses to close.
 * @param write Nonzero: make the stores.
 * @return With write 0: 1 when no gem and no lamp is in state 0, 0 when
 *         one is, -1 outside Glimmer play; with write 1: the stores made. */
static int glimmer_gem_lamps(int write) {
    uint32_t head, m;
    int i, todo = 0, stores = 0;

    if (op_read_u16(g_api, S2_LEVEL_ID) != S2_GLIMMER ||
        op_read_u32(g_api, S2_LEVEL_TICK) == 0u)
        return -1;
    head = op_read_u32(g_api, S2_MOBY_ARRAY);
    for (i = 0, m = head; i < 4096 && op_read_u8(g_api, m + 0x48u) != 0xFFu;
         i++, m += S2_MOBY_STRIDE) {
        uint32_t cls = op_read_u16(g_api, m + 0x36u);
        if (cls == GL_GEM_CLASS && op_read_u8(g_api, m + 0x48u) == 0u) {
            todo++;
            if (write && op_read_u8(g_api, m + 0x49u) != 0x3Fu) {
                op_write_u8(g_api, m + 0x49u, 0x3Fu);
                stores++;
            }
        } else if (cls == GL_LAMP_CLASS && op_read_u8(g_api, m + 0x48u) == 0u) {
            uint32_t hit = op_read_u32(g_api, m + 0x18u);
            todo++;
            if (write && !(hit & GL_HIT_BIT)) {
                op_write_u32(g_api, m + 0x18u, hit | GL_HIT_BIT);
                stores++;
            }
        }
    }
    return write ? stores : (todo == 0);
}

#define S3_SORCERESS_DEFEATED 0x8007158Fu /* progressFlags + 0x1F (lvl47_SorceressDefeated) */
#define S3_LEVEL_GEMS     0x80071A10u /* int[40] gems collected per level */
#define S3_LEVEL_EGGS     0x80070300u /* u8[40] eggs collected bitmask per level */
#define S3_ATLAS_LEVELS   39u         /* entry 39 of the gem array is Moneybags' refund */
#define S3_STRESS_GEMS    9999999u
#define S3_ATLAS_CODE_A   0x8007839Cu /* func_atlas_8007839C: lui $v1, %hi(D_atlas_8007ADE0) */
#define S3_ATLAS_WORD_A   0x3C038008u
#define S3_ATLAS_CODE_B   0x800774FCu /* func_atlas_80076B64: lh $v0, %lo(D_atlas_8007AE0E)($v0) */
#define S3_ATLAS_WORD_B   0x8442AE0Eu

/** @brief Writes synthetic Spyro 3 progress that lengthens every atlas row, while the atlas overlay is resident.
 *
 * The atlas draws one sprite per glyph and reads its numbers each tick:
 * gems per level from the int array at 0x80071A10 (printed "%d/%d" on the
 * contents and world pages), eggs as the popcount of each level's bitmask
 * at 0x80070300, and the Sorceress-defeated byte 0x8007158F, which adds the
 * Super Bonus Worlds contents row and the "Press ... for skill points"
 * prompt (func_atlas_800765F8). 9999999 gems and 0xFF egg masks are values
 * the game never reaches; they widen every number so a page turn, which
 * draws both pages, links more 2D primitives than a real save does. The
 * routine waits until two atlas code words read back at their addresses,
 * so a level overlay occupying the same range is never written, and a run
 * outside the atlas keeps the game's own values.
 * @param write Nonzero: make the stores.
 * @return With write 0: 1 when every value is in place, 0 when one is not,
 *         -1 while the atlas overlay is not resident; with write 1: the
 *         stores made. */
static int atlas_page_stress(int write) {
    uint32_t i;
    int todo = 0, stores = 0;

    if (op_read_u32(g_api, S3_ATLAS_CODE_A) != S3_ATLAS_WORD_A ||
        op_read_u32(g_api, S3_ATLAS_CODE_B) != S3_ATLAS_WORD_B)
        return -1;
    if (op_read_u8(g_api, S3_SORCERESS_DEFEATED) != 1u) {
        todo++;
        if (write) { op_write_u8(g_api, S3_SORCERESS_DEFEATED, 1u); stores++; }
    }
    for (i = 0; i < S3_ATLAS_LEVELS; i++) {
        if (op_read_u32(g_api, S3_LEVEL_GEMS + 4u * i) != S3_STRESS_GEMS) {
            todo++;
            if (write) { op_write_u32(g_api, S3_LEVEL_GEMS + 4u * i, S3_STRESS_GEMS); stores++; }
        }
        if (op_read_u8(g_api, S3_LEVEL_EGGS + i) != 0xFFu) {
            todo++;
            if (write) { op_write_u8(g_api, S3_LEVEL_EGGS + i, 0xFFu); stores++; }
        }
    }
    return write ? stores : (todo == 0);
}

/** @brief Reads an action's address at its width.
 *
 * @param a The action.
 * @return The value the address holds. */
static uint32_t action_read(const action_t* a) {
    switch (a->width) {
    case 1u: return op_read_u8(g_api, a->addr);
    case 2u: return op_read_u16(g_api, a->addr);
    default: return op_read_u32(g_api, a->addr);
    }
}

/** @brief Writes an action's value to its address at its width.
 *
 * @param a The action. */
static void action_write(const action_t* a) {
    switch (a->width) {
    case 1u: op_write_u8(g_api, a->addr, (uint8_t)a->value); break;
    case 2u: op_write_u16(g_api, a->addr, (uint16_t)a->value); break;
    default: op_write_u32(g_api, a->addr, a->value); break;
    }
}

/** @brief Queues rows for the next tick; legal from any thread.
 *
 * @param mask Bit i queues s_rows[i]. */
static void queue_rows(unsigned mask) {
    __atomic_fetch_or(&s_pending, mask, __ATOMIC_RELEASE);
}

/** @brief Writes every queued row, refreshes the panel's copy of each row's value, then runs the game update.
 *
 * @param cpu The guest CPU state. */
static void on_tick(CPUState* cpu) {
    unsigned pend;
    int i;

    if (++s_ticks == s_test_tick)
        queue_rows(s_test_mask ? s_test_mask : (1u << s_n) - 1u);
    pend = __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE);
    for (i = 0; i < s_n; i++) {
        const action_t* a = &k_actions[s_rows[i]];
        uint32_t cur;
        if (a->apply) {
            int st = a->apply(0);
            if ((pend & (1u << i)) && st == 1) {
                __atomic_fetch_and(&s_pending, ~(1u << i), __ATOMIC_RELAXED);
            } else if ((pend & (1u << i)) && st == 0) {
                int n = a->apply(1);
                if (n > 0)
                    g_api->log(g_self, OP_MOD_LOG_INFO,
                               "%s: %d store(s), tick %lld", a->name, n,
                               (long long)s_ticks);
            }
            __atomic_store_n(&s_value[i], (uint32_t)st, __ATOMIC_RELAXED);
            continue;
        }
        cur = action_read(a);
        if (pend & (1u << i)) {
            if (cur == a->value) {
                __atomic_fetch_and(&s_pending, ~(1u << i), __ATOMIC_RELAXED);
            } else {
                action_write(a);
                g_api->log(g_self, OP_MOD_LOG_INFO,
                           "%s: 0x%08X = %u (was %u), tick %lld", a->name,
                           a->addr, a->value, cur, (long long)s_ticks);
                cur = a->value;
            }
        }
        __atomic_store_n(&s_value[i], cur, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_seen, 1u, __ATOMIC_RELEASE);
    g_api->base(cpu);
}

/** @brief Draws one button per row, each row's state, and an "Apply all" button.
 *
 * @param ui The engine's widget table. */
static void ui_draw(const openpete_mod_ui_t* ui) {
    const unsigned seen = __atomic_load_n(&s_seen, __ATOMIC_ACQUIRE);
    const unsigned pend = __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE);
    int i;

    if (s_n == 0) {
        ui->text_disabled("no actions for this game");
        return;
    }
    ui->text_disabled("Writes apply on the next tick; a save keeps them.");
    for (i = 0; i < s_n; i++) {
        const action_t* a = &k_actions[s_rows[i]];
        const uint32_t v = __atomic_load_n(&s_value[i], __ATOMIC_RELAXED);
        if (ui->button(a->name))
            queue_rows(1u << i);
        ui->tooltip(a->note);
        ui->same_line();
        if (pend & (1u << i))
            ui->text("queued");
        else if (!seen)
            ui->text_disabled("no tick yet");
        else if (a->apply)
            ui->text(v == 1u ? "set" : v == 0u ? "not set" : "not in play here");
        else if (v == a->value)
            ui->text("set (0x%08X = %u)", a->addr, v);
        else
            ui->text_disabled("not set (0x%08X = %u)", a->addr, v);
    }
    ui->separator();
    if (ui->button("Apply all"))
        queue_rows((1u << s_n) - 1u);
}
OPENPETE_MOD_UI_SECTION(ui_draw)

/**
 * @brief Mod entry: selects the running game's rows and attaches the tick hook to its per-tick update.
 *
 * The hook runs once per tick from the title screen on: Spyro 1
 * GamestateUpdate, Spyro 2 func_8001B140, Spyro 3 func_80055400.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, the override registration's failure otherwise.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    const char* game = api->game_id(self);
    char only[96];
    int i;

    g_api = api; g_self = self;
    for (i = 0; i < N_ACTIONS; i++)
        if (!strcmp(k_actions[i].game, game))
            s_rows[s_n++] = i;
    s_test_tick = api->config_int(self, "test.apply_tick", 0);
    if (api->config_str(self, "test.apply_only", "", only, (int)sizeof only))
        for (i = 0; i < s_n; i++)
            if (!strncmp(k_actions[s_rows[i]].name, only, strlen(only)))
                s_test_mask |= 1u << i;
    api->log(self, OP_MOD_LOG_INFO, "debug-flags: %d action(s) for %s", s_n,
             game);
#if OPENPETE_GAME_SPYRO1
    return api->override_name(self, "GamestateUpdate", on_tick);
#elif OPENPETE_GAME_SPYRO2
    return api->override_addr(self, 0x8001B140u, on_tick);
#elif OPENPETE_GAME_SPYRO3
    return api->override_addr(self, 0x80055400u, on_tick);
#else
#error "debug-flags: no tick binding for this game"
#endif
}
