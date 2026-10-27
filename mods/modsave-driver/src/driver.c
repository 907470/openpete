/* modsave-driver - fixture-driven Spyro 1 card saves and loads.
 *
 * Actions run from the GamestateUpdate override after the base, at the
 * override's own call count:
 *   save:<i>    SaveCreate(scratch), then MemCardWriteFile(port << 4,
 *               "BASCUS-94228SPYRO", scratch, 0x200 + i * 0x600, 0x600)
 *   load:<i>    MemCardReadFile of record i into the scratch, then
 *               SaveLoad(scratch)
 *   badload:<i> as load, with record byte 0x40 flipped after the read, so
 *               the stock checksum verdict fails
 *   newgame:<i> func_8001277C, the game's progress clear (a new
 *               playthrough); <i> is unused
 *   newkeep:<i> as newgame, then g_VisitedFlags[0] := 1, so a later
 *               SaveCreate record is non-empty
 *   leave:<i>   no count: save index i on the first update whose stock
 *               base moves g_Gamestate off 0 (gameplay to another state)
 * Each call goes through api->call, so every mod's override chain on the
 * callee runs. MemCardWriteFile's fifth argument is passed at sp+0x10 of a
 * 0x20-byte frame the driver pushes below the current stack pointer.
 * Log line per action: "modsave-driver: <op> <i> at <n> rc <v0>".
 */
#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define DRV_MAX_ACTIONS 16
#define DRV_REC_BYTES   0x600u
#define DRV_NAME        "BASCUS-94228SPYRO"

enum { DRV_SAVE, DRV_LOAD, DRV_BADLOAD, DRV_NEWGAME, DRV_NEWKEEP, DRV_LEAVE };

typedef struct { int op; uint32_t index; uint32_t at; } drv_action_t;

/** @brief Arena block: the call counter, the file name and the scratch record. */
typedef struct {
    uint32_t count;
    uint32_t left;           /* the leave action has run */
    char     name[32];
    uint8_t  rec[DRV_REC_BYTES];
} drv_state_t;

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static drv_state_t*              g_st;
static uint32_t                  g_st_va;
static drv_action_t              g_act[DRV_MAX_ACTIONS];
static int                       g_nact;
static uint32_t                  g_chan;

/** @brief Call MemCardWriteFile or MemCardReadFile on one record through the dispatch layer. */
static uint32_t drv_card_io(CPUState* cpu, uint32_t fn, uint32_t index) {
    const uint32_t sp = cpu->sp;
    cpu->sp = sp - 0x20u;
    op_write_u32(g_api, cpu->sp + 0x10u, DRV_REC_BYTES);
    cpu->a0 = g_chan;
    cpu->a1 = g_st_va + (uint32_t)offsetof(drv_state_t, name);
    cpu->a2 = g_st_va + (uint32_t)offsetof(drv_state_t, rec);
    cpu->a3 = 0x200u + index * DRV_REC_BYTES;
    g_api->call(cpu, fn);
    cpu->sp = sp;
    return cpu->v0;
}

/** @brief Run one action; returns the v0 of its last guest call. */
static uint32_t drv_run(CPUState* cpu, const drv_action_t* a) {
    const uint32_t rec = g_st_va + (uint32_t)offsetof(drv_state_t, rec);
    if (a->op == DRV_NEWGAME || a->op == DRV_NEWKEEP) {
        g_api->call(cpu, OP_FNADDR_func_8001277C);
        if (a->op == DRV_NEWKEEP) op_write_u8(g_api, OP_GADDR_g_VisitedFlags, 1u);
        return 0;
    }
    if (a->op == DRV_SAVE || a->op == DRV_LEAVE) {
        cpu->a0 = rec;
        g_api->call(cpu, OP_FNADDR_SaveCreate);
        return drv_card_io(cpu, OP_FNADDR_MemCardWriteFile, a->index);
    }
    const uint32_t rc = drv_card_io(cpu, OP_FNADDR_MemCardReadFile, a->index);
    if (rc != 0u) return rc;
    if (a->op == DRV_BADLOAD) g_st->rec[0x40] ^= 0x01u;
    cpu->a0 = rec;
    g_api->call(cpu, OP_FNADDR_SaveLoad);
    return cpu->v0;
}

/** @brief GamestateUpdate override: runs the base, then any action due at this count. */
static void on_update(CPUState* cpu) {
    static const char* names[] = { "save", "load", "badload", "newgame", "newkeep", "leave" };
    const uint32_t gs0 = op_read_u32(g_api, OP_GADDR_g_Gamestate);
    g_api->base(cpu);
    const uint32_t n = ++g_st->count;
    const int left = gs0 == 0u && op_read_u32(g_api, OP_GADDR_g_Gamestate) != 0u;
    for (int i = 0; i < g_nact; i++) {
        if (g_act[i].op == DRV_LEAVE ? (!left || g_st->left) : g_act[i].at != n) continue;
        if (g_act[i].op == DRV_LEAVE) g_st->left = 1;
        const uint32_t saved_ra = cpu->ra, saved_v0 = cpu->v0;
        const uint32_t rc = drv_run(cpu, &g_act[i]);
        cpu->ra = saved_ra;
        cpu->v0 = saved_v0;
        g_api->log(g_self, OP_MOD_LOG_INFO, "modsave-driver: %s %u at %u rc %u",
                   names[g_act[i].op], g_act[i].index, n, rc);
    }
}

/** @brief Parse "op:index@count" items; returns the number parsed or -1 on a malformed item. */
static int drv_parse(const char* s) {
    int n = 0;
    while (*s && n < DRV_MAX_ACTIONS) {
        char op[16];
        unsigned idx = 0, at = 0;
        int used = 0;
        if (sscanf(s, " %15[a-z]:%u@%u%n", op, &idx, &at, &used) != 3 || idx > 2u) return -1;
        if (!strcmp(op, "save")) g_act[n].op = DRV_SAVE;
        else if (!strcmp(op, "load")) g_act[n].op = DRV_LOAD;
        else if (!strcmp(op, "badload")) g_act[n].op = DRV_BADLOAD;
        else if (!strcmp(op, "newgame")) g_act[n].op = DRV_NEWGAME;
        else if (!strcmp(op, "newkeep")) g_act[n].op = DRV_NEWKEEP;
        else if (!strcmp(op, "leave")) g_act[n].op = DRV_LEAVE;
        else return -1;
        g_act[n].index = idx;
        g_act[n].at = at;
        n++;
        s += used;
        while (*s == ',' || *s == ' ') s++;
    }
    return n;
}

/**
 * @brief Mod entry: parses the action list and attaches the driver to GamestateUpdate.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, -1 on a malformed action list or a refused registration.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    char spec[512];
    void* view = 0;
    g_api = api; g_self = self;
    api->config_str(self, "actions", "", spec, (int)sizeof spec);
    g_nact = drv_parse(spec);
    if (g_nact < 0) {
        api->log(self, OP_MOD_LOG_ERROR, "modsave-driver: malformed actions \"%s\"", spec);
        return -1;
    }
    g_st_va = api->guest_alloc(self, sizeof(drv_state_t), 16, 0, &view);
    if (!g_st_va || !view) return -1;
    g_st = (drv_state_t*)view;
    memcpy(g_st->name, DRV_NAME, sizeof DRV_NAME);
    g_chan = (uint32_t)api->config_int(self, "port", 0) << 4;
    api->log(self, OP_MOD_LOG_INFO, "modsave-driver: %d actions", g_nact);
    return api->override_name(self, "GamestateUpdate", on_update) ? -1 : 0;
}
