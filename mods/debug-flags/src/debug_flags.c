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
 * Test trigger: the config.toml key `test.apply_tick = N` (no `[[config]]`
 * row, so the panel never shows it) queues every row at the N-th tick the
 * hook sees, which is how a headless run presses "Apply all".
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
} action_t;

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
      "is nonzero. A save written afterwards keeps it." },
    { "Swim (dive underwater)", "spyro-2", 0x80064682u, 2u, 1u,
      "Spyro 2 0x80064682 = 1: the paid flag of Moneybags' swim lesson. "
      "While it is nonzero Square at the water's surface dives; at zero "
      "Spyro stays at the surface." },
    { "Climb (ladders)", "spyro-2", 0x80064686u, 2u, 1u,
      "Spyro 2 0x80064686 = 1: the paid flag of Moneybags' climb lesson. "
      "The ladder test returns at once while it is zero." },
    { "Headbash (triangle in the air)", "spyro-2", 0x8006468Au, 2u, 1u,
      "Spyro 2 0x8006468A = 1: the paid flag of Moneybags' headbash "
      "lesson. Triangle in the air starts the headbash while it is "
      "nonzero." },
    { "Permanent superflame", "spyro-2", 0x800698BBu, 1u, 1u,
      "Spyro 2 0x800698BB = 1: the powerup gate's permanent-superflame "
      "case sets it; the flame button fires the superflame shot while it "
      "is nonzero. The card record keeps it." },
    { "Permanent superflame", "spyro-3", 0x8006FB93u, 1u, 1u,
      "Spyro 3 0x8006FB93 = 1 (g_CheatFlags + 3): the powerup gate's "
      "permanent-superflame case sets it; the flame button fires the "
      "superflame shot while it is nonzero." },
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
        queue_rows((1u << s_n) - 1u);
    pend = __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE);
    for (i = 0; i < s_n; i++) {
        const action_t* a = &k_actions[s_rows[i]];
        uint32_t cur = action_read(a);
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
    int i;

    g_api = api; g_self = self;
    for (i = 0; i < N_ACTIONS; i++)
        if (!strcmp(k_actions[i].game, game))
            s_rows[s_n++] = i;
    s_test_tick = api->config_int(self, "test.apply_tick", 0);
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
