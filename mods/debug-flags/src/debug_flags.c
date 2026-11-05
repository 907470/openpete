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
    const char* name;   /**< Button label; unique across the table. */
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
 * record and frees itself (level_50 moby_update 0x80076830-0x800768B8). */
static const action_t k_actions[] = {
    { "Ripto defeated (opens the Dragon Shores portal)", "spyro-2",
      0x800646AEu, 2u, 1u,
      "Spyro 2 0x800646AE = 1: Ripto's Arena sets it when Ripto is beaten; "
      "Winter Tundra's portal gate opens the Dragon Shores portal while it "
      "is nonzero. A save written afterwards keeps it." },
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
