// games/common/game_host_desc.c - the shared host TUs' table globals and
// their binding from the running game's descriptor. See game_host_desc.h.

#include "game_host_desc.h"

#include <stdio.h>

/* The running game's tables, copied from its game_host_desc_t. */
ui_game_t                ui_game;
terrain_game_t           terrain_game;
moby_game_t              moby_game;
player_game_t            player_game;
moby_game_s23_consts_t   moby_game_s23;
player_game_s23_consts_t player_game_s23;
portal_game_s23_consts_t portal_game_s23;
freecam_game_desc        freecam_desc;

/** @brief Report no portal sky drawn: the inert terrain provider. */
static int none_recent(void) { return 0; }
/** @brief Report no portal opening plane: the inert terrain provider. */
static int none_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) {
    (void)cpu; (void)v; (void)out; (void)max; return 0;
}
/** @brief Return the inert terrain provider's plane-cover margin, 256 world units. */
static float none_margin(void) { return 256.0f; }
/** @brief Report no LP face fill-tagged: the inert terrain provider. */
static int none_fill_tag(int level, int sec, int face) { (void)level; (void)sec; (void)face; return 0; }
/** @brief Spell a level as "level<id>": the inert terrain provider. */
static void none_level_name(int level, char* out, size_t cap) { snprintf(out, cap, "level%d", level); }
/** @brief Report no art correction: the inert terrain provider. */
static int none_artfix(int level, int sec, int face, uint32_t* rgb) {
    (void)level; (void)sec; (void)face; (void)rgb; return 0;
}

/* The terrain provider of a game with no portal-sky feeder, no fill-tag
 * list and no art corrections. */
static const terrain_game_t k_terrain_none = {
    .portal_sky_recent   = none_recent,
    .portal_sky_planes   = none_planes,
    .portal_plane_margin = none_margin,
    .fill_tag            = none_fill_tag,
    .level_name          = none_level_name,
    .lp_artfix           = none_artfix,
};

void game_runtime_install(const openpete_game_runtime_t* rt) {
    static const game_host_desc_t k_none = { 0 };
    openpete_game_runtime_register(rt);
    const game_host_desc_t* h = rt && rt->host ? rt->host : &k_none;
    const ui_game_t                k_ui = { 0 };
    const moby_game_t              k_moby = { 0 };
    const player_game_t            k_player = { 0 };
    const moby_game_s23_consts_t   k_moby_s23 = { 0 };
    const player_game_s23_consts_t k_player_s23 = { 0 };
    const portal_game_s23_consts_t k_portal_s23 = { 0 };
    const freecam_game_desc        k_freecam = { 0 };
    ui_game         = h->ui ? *h->ui : k_ui;
    terrain_game    = h->terrain ? *h->terrain : k_terrain_none;
    moby_game       = h->moby ? *h->moby : k_moby;
    player_game     = h->player ? *h->player : k_player;
    moby_game_s23   = h->moby_s23 ? *h->moby_s23 : k_moby_s23;
    player_game_s23 = h->player_s23 ? *h->player_s23 : k_player_s23;
    portal_game_s23 = h->portal_s23 ? *h->portal_s23 : k_portal_s23;
    freecam_desc    = h->freecam ? *h->freecam : k_freecam;
    game_orchestrator_bind(h->orchestrator, h->interp_register);
}

/** @brief Write one table as a `host.NAME HEX` line.
 *
 * @param out The stream.
 * @param name The table's name.
 * @param p The table.
 * @param n Its size in bytes. */
static void dump_table(FILE* out, const char* name, const void* p, size_t n) {
    fprintf(out, "host.%s ", name);
    for (size_t i = 0; i < n; i++) fprintf(out, "%02x", ((const unsigned char*)p)[i]);
    fputc('\n', out);
}

void game_host_tables_dump(FILE* out) {
    dump_table(out, "ui_game", &ui_game, sizeof ui_game);
    dump_table(out, "terrain_game", &terrain_game, sizeof terrain_game);
    dump_table(out, "moby_game", &moby_game, sizeof moby_game);
    dump_table(out, "player_game", &player_game, sizeof player_game);
    dump_table(out, "moby_game_s23", &moby_game_s23, sizeof moby_game_s23);
    dump_table(out, "player_game_s23", &player_game_s23, sizeof player_game_s23);
    dump_table(out, "portal_game_s23", &portal_game_s23, sizeof portal_game_s23);
    dump_table(out, "freecam_desc", &freecam_desc, sizeof freecam_desc);
}
