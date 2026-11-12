/* ram-mirror-probe - a live moby relinked by the game at a negative X or Y.
 *
 * The game's grid relink (flag 2) reads the moby's X (+0x0C) and Y (+0x10),
 * forms the bucket index (x >> 13) + ((y >> 13) << 5) with unsigned shifts
 * (Spyro 1 also keeps bit 0x400 of the old index), stores the index's low
 * halfword at +0x34 and pushes the moby on the list whose head word is
 * grid + 4 * index. Its next relink unlinks the moby by walking the list
 * that starts at grid + 4 * (int16)halfword.
 *
 * With X negative and Y in sectors 1-31 the index is 0x80000 + (halfword),
 * so the head word lies 2 MB past the bucket the halfword names: the same
 * RAM byte on hardware, where physical RAM repeats through 8 MB. With Y
 * negative the head word lies about 64 MB past the grid, where hardware has
 * no device, and the halfword reads negative, so the next relink skips the
 * unlink.
 *
 * The probe lets `skip` live flag-2 relinks through, takes the next moby
 * relinked with its other coordinate in sectors 0-31 (1-31 for case x),
 * and for that moby's next `force` relinks writes the coordinate as -588
 * before the game's relink and restores it after. Each forced relink is
 * checked: the halfword equals the index's low 16 bits and, for case x,
 * the bucket the halfword names holds the moby at its head, read through
 * the base address and through the mirror address the game stored to. The
 * first relink after the forced ones moves the moby back to the bucket of
 * its own position, where it must be the head (case x); for case y the
 * negative halfword makes the game skip the relink, and the moby stays in
 * no bucket. With `observe` set the probe forces nothing and logs the grid's
 * base address at each relink where it or the level differs from the last
 * one logged. Logs
 * "ram-mirror-probe: PASS ..." or "ram-mirror-probe: FAIL ...".
 */
#include <openpete_mod_api.h>
#include <psx_runtime.h>
#include <string.h>

static const openpete_mod_api_t* A;
static openpete_mod_t*           S;

typedef struct {
    const char* game;
    uint32_t    relink;     /* the grid relink function */
    uint32_t    grid_ptr;   /* the global holding the grid's base vaddr */
    int         keep_400;   /* Spyro 1 ORs bit 0x400 of the old index into the new one */
    int         zero_flag8; /* flag 8 files the moby in bucket 0 (Spyro 2 and 3) */
} game_row_t;

static const game_row_t k_games[] = {
    { "spyro-1", 0x800529E4u, 0x80075778u, 1, 0 },
    { "spyro-2", 0x80041538u, 0x80067174u, 0, 1 },
    { "spyro-3", 0x80055D24u, 0x8006C7E0u, 0, 1 },
};

#define NEG_COORD (-588)
#define SECTOR_SHIFT 13

static const game_row_t* G;
static int      s_case_y;
static uint32_t s_skip, s_force, s_seen, s_forced, s_ok, s_target;
static int      s_done;
static int      s_observe, s_last_level = -1;
static uint32_t s_last_grid;

/** @brief Read a guest word through the host view; 0 for an address that names nothing. */
static uint32_t rd32(uint32_t va) { return op_read_u32(A, va); }

/** @brief The bucket index the game's relink forms for a moby at (x, y) whose stored halfword is @p half. */
static uint32_t bucket_index(uint32_t x, uint32_t y, uint16_t half) {
    uint32_t idx = (x >> SECTOR_SHIFT) + ((y >> SECTOR_SHIFT) << 5);
    if (G->keep_400) idx |= (uint32_t)half & 0x400u;
    return idx;
}

/** @brief Whether a coordinate lies in grid sectors [lo, 31]. */
static int in_sectors(uint32_t c, uint32_t lo) {
    return c >> SECTOR_SHIFT >= lo && c >> SECTOR_SHIFT < 32u;
}

/** @brief Report the verdict once. */
static void verdict(int pass, const char* why) {
    if (s_done) return;
    s_done = 1;
    A->log(S, pass ? OP_MOD_LOG_INFO : OP_MOD_LOG_ERROR,
           "ram-mirror-probe: %s case=%s game=%s moby=0x%08X forced=%u/%u bucket_ok=%u (%s)",
           pass ? "PASS" : "FAIL", s_case_y ? "y" : "x", G->game, s_target, s_forced, s_force, s_ok, why);
}

/** @brief Run one forced relink of the target with its coordinate at NEG_COORD, and check the bucket. */
static void forced_relink(CPUState* cpu) {
    const uint32_t m = s_target, grid = rd32(G->grid_ptr);
    const uint32_t off = s_case_y ? 0x10u : 0x0Cu;
    const uint32_t orig = rd32(m + off);
    op_write_s32(A, m + off, NEG_COORD);
    const uint32_t x = rd32(m + 0x0Cu), y = rd32(m + 0x10u);
    const uint32_t idx = bucket_index(x, y, op_read_u16(A, m + 0x34u));
    A->base(cpu);
    op_write_u32(A, m + off, orig);
    s_forced++;
    const uint16_t half = op_read_u16(A, m + 0x34u);
    const uint32_t store = grid + 4u * idx;          /* the head word the game wrote */
    const uint32_t named = grid + 4u * (uint32_t)(int32_t)(int16_t)half;
    int ok = half == (uint16_t)idx;
    if (!s_case_y) {
        const uint32_t base_alias = 0x80000000u | (store & 0x1FFFFFu);
        ok = ok && named == base_alias && rd32(named) == m && rd32(store) == m;
    } else {
        ok = ok && (int16_t)half < 0;
    }
    if (ok) s_ok++;
    if (s_forced == 1u || !ok)
        A->log(S, ok ? OP_MOD_LOG_INFO : OP_MOD_LOG_ERROR,
               "ram-mirror-probe: forced relink %u: index 0x%X, halfword 0x%04X, head word 0x%08X, "
               "bucket named 0x%08X holds 0x%08X, head word holds 0x%08X: %s",
               s_forced, idx, half, store, named, rd32(named), rd32(store), ok ? "ok" : "WRONG");
}

/** @brief The relink after the forced ones: the moby must head the bucket of its own position. */
static void released_relink(CPUState* cpu) {
    const uint32_t m = s_target;
    A->base(cpu);
    const uint32_t grid = rd32(G->grid_ptr);
    const uint16_t half = op_read_u16(A, m + 0x34u);
    const uint32_t idx = bucket_index(rd32(m + 0x0Cu), rd32(m + 0x10u), half);
    /* Case x: the unlink found the moby through the bucket its halfword
     * names, and the link filed it at the head of its own bucket. Case y:
     * the halfword reads negative, which the game's relink takes as "in no
     * bucket" and skips the unlink and the link both, so the moby stays out
     * of the grid with the halfword unchanged. */
    const int back = s_case_y ? (int16_t)half < 0
                              : half == (uint16_t)idx && idx < 0x800u && rd32(grid + 4u * idx) == m;
    A->log(S, OP_MOD_LOG_INFO, "ram-mirror-probe: released: index 0x%X, bucket head 0x%08X", idx,
           rd32(grid + 4u * idx));
    verdict(back && s_ok == s_force, back ? (s_ok == s_force ? "every bucket held the moby" : "a forced bucket was wrong")
                                          : "the moby is not at the head of its own bucket after release");
}

/** @brief Relink override: chooses the target among the game's own flag-2 relinks, then forces and checks its relinks. */
static void on_relink(CPUState* cpu) {
    const uint32_t m = cpu->a0, flags = cpu->a1;
    if (s_observe) {
        const uint32_t grid = rd32(G->grid_ptr);
        const int level = A->level_id(S, cpu);
        if (grid != s_last_grid || level != s_last_level)
            A->log(S, OP_MOD_LOG_INFO, "ram-mirror-probe: level %d grid base 0x%08X", level, grid);
        s_last_grid = grid;
        s_last_level = level;
        A->base(cpu);
        return;
    }
    if (s_done || !(flags & 2u) || (G->zero_flag8 && (flags & 8u)) || !A->gameplay_live(S, cpu)) {
        A->base(cpu);
        return;
    }
    if (!s_target) {
        const uint32_t other = s_case_y ? rd32(m + 0x0Cu) : rd32(m + 0x10u);
        if (s_seen++ < s_skip || !in_sectors(other, s_case_y ? 0u : 1u)) { A->base(cpu); return; }
        s_target = m;
        A->log(S, OP_MOD_LOG_INFO, "ram-mirror-probe: target moby 0x%08X type 0x%04X at (%d, %d), grid 0x%08X",
               m, op_read_u16(A, m + 0x36u), (int)rd32(m + 0x0Cu), (int)rd32(m + 0x10u), rd32(G->grid_ptr));
    }
    if (m != s_target) { A->base(cpu); return; }
    if (s_forced < s_force) forced_relink(cpu);
    else released_relink(cpu);
}

/**
 * @brief Mod entry: picks the game's row, reads the config, optionally maps the arena over the mirror, and hooks the relink.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, -1 on an unknown game or a refused hook or allocation.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; S = self;
    const char* id = api->game_id(self);
    for (unsigned i = 0; i < sizeof k_games / sizeof k_games[0]; i++)
        if (strcmp(k_games[i].game, id) == 0) G = &k_games[i];
    if (!G) return -1;
    char c[8];
    api->config_str(self, "case", "x", c, (int)sizeof c);
    s_case_y = c[0] == 'y';
    s_skip = (uint32_t)api->config_int(self, "skip", 200);
    s_force = (uint32_t)api->config_int(self, "force", 30);
    s_observe = api->config_bool(self, "observe", 0);
    if (api->config_bool(self, "conflict", 0)) {
        void* h1 = 0;
        void* h2 = 0;
        const uint32_t v1 = api->guest_alloc(self, 9u << 20, 0, 0, &h1);
        const uint32_t v2 = api->guest_alloc(self, 2u << 20, 0, 0, &h2);
        api->log(self, OP_MOD_LOG_INFO, "ram-mirror-probe: arena blocks 0x%08X (9 MiB), 0x%08X (2 MiB)", v1, v2);
        if (!v1 || !v2) return -1;
    }
    api->log(self, OP_MOD_LOG_INFO, "ram-mirror-probe: hooking 0x%08X on %s, case %s, skip %u, force %u",
             G->relink, id, s_case_y ? "y" : "x", s_skip, s_force);
    return api->override_addr(self, G->relink, on_relink) ? -1 : 0;
}
