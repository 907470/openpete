// core/launcher/launcher_title.cpp - the launcher's 3D title: the pixel
// title OPENPETE replaced, at once, by one game's glyph models read from that
// game's launcher asset cache, drawn by the title pass on the launcher's
// device (openpete_native_launcher_set_title).
//
// The game is the most recently played library entry's at launch, else the
// first whose assets become ready; an ingest or a pick switches to its game
// when its assets are ready. The glyphs are fitted to the pixel title's width
// (a gap of 0.004 of the layout height after each but the last) and centred
// on the ink of its capitals; they wobble as the settings menu's title,
// stepping once per game tick on the present clock from the switch, and are
// lit as Spyro 2's and Spyro 3's performance readout (text_glyph_set_emit_title).
//
// Harness variables: OPENPETE_LAUNCHER_TITLE_TICK=<n> pins the tick;
// OPENPETE_LAUNCHER_TITLE_DUMP=<path> writes the first 3D title drawn (its
// placement, cap height, and per glyph its posed rect and front face colour)
// as JSON.
#include "launcher_internal.h"

#include "openpete_interp.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kText = "OPENPETE";
constexpr int kMaxVerts = 8192;
constexpr int kMaxGlyphs = 16;

/* The title's state, on the launcher's thread. */
struct Title {
    const openpete_launcher_game* shown = nullptr;   /* the game whose glyphs draw */
    const openpete_launcher_game* pref = nullptr;    /* the game the title belongs to */
    op_time_t t0 = 0;           /* present-clock instant of the switch */
    bool drawn = false;         /* the present being built draws the 3D title */
    bool logged = false;        /* the first draw of the shown game was logged */
    bool dumped = false;        /* OPENPETE_LAUNCHER_TITLE_DUMP was written */
    float rect[4] = {};         /* the drawn title's rect: x0, cy - cap/2, x1, cy + cap/2 */
    openpete_hud_vert verts[kMaxVerts];
};
Title t;

/** @brief Return the per-game launcher entry of a game id: the launcher's own descriptor when it names that game, else the neutral descriptor's entry.
 *
 * @param g A launcher entry naming the game.
 * @return The entry carrying the title callbacks, or NULL. */
const openpete_launcher_game* title_entry(const openpete_launcher_game* g) {
    if (!g || !g->game_id) return nullptr;
    if (g->title_load && g->title_emit) return g;
    const openpete_launcher_game* n = launcher_game();
    for (int i = 0; n && i < n->game_count; i++)
        if (n->games[i] && n->games[i]->game_id && !strcmp(n->games[i]->game_id, g->game_id) &&
            n->games[i]->title_load && n->games[i]->title_emit)
            return n->games[i];
    return nullptr;
}

/** @brief Return the title's tick: OPENPETE_LAUNCHER_TITLE_TICK when set, else the game ticks on the present clock since the switch.
 *
 * @return The tick. */
int title_tick() {
    if (const char* e = getenv("OPENPETE_LAUNCHER_TITLE_TICK"); e && *e) return atoi(e);
    const double s = (double)(openpete_interp_present_now() - t.t0) / (double)OP_NS_PER_SEC;
    return s > 0.0 ? (int)std::floor(s * OP_TICK_HZ) : 0;
}

/** @brief Write the drawn title as JSON to OPENPETE_LAUNCHER_TITLE_DUMP, once per process.
 *
 * @param q The placement.
 * @param w The window width.
 * @param h The window height.
 * @param cap_h The cap height.
 * @param n The vertex count.
 * @param gl The posed glyphs.
 * @param gn Their count. */
void title_dump(const openpete_launcher_title& q, float w, float h, float cap_h, int n,
                const openpete_launcher_title_glyph* gl, int gn) {
    const char* path = getenv("OPENPETE_LAUNCHER_TITLE_DUMP");
    if (t.dumped || !path || !*path) return;
    t.dumped = true;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\"game\": \"%s\", \"window\": [%g, %g], \"text\": \"%s\", \"tick\": %d,\n", t.shown->game_id, w, h,
            kText, q.tick);
    fprintf(f, " \"x0\": %.4f, \"cy\": %.4f, \"width\": %.4f, \"gap\": %.4f, \"cap\": %.4f, \"verts\": %d,\n", q.x0,
            q.cy, q.width, q.gap, cap_h, n);
    fprintf(f, " \"rect\": [%.2f, %.2f, %.2f, %.2f],\n \"glyphs\": [", t.rect[0], t.rect[1], t.rect[2], t.rect[3]);
    for (int i = 0; i < gn; i++)
        fprintf(f, "%s\n  {\"rect\": [%.4f, %.4f, %.4f, %.4f], \"face\": %d, \"rgba\": \"%08x\"}", i ? "," : "",
                gl[i].x0, gl[i].y0, gl[i].x1, gl[i].y1, gl[i].face, gl[i].rgba);
    fprintf(f, "]}\n");
    fclose(f);
}

}  // namespace

void launcher_title_offer(const openpete_launcher_game* g, bool prefer, const char* why) {
    const openpete_launcher_game* e = title_entry(g);
    if (!e) return;
    if (prefer) t.pref = e;
    if (t.shown && e != t.pref) return;   /* the title belongs to another game */
    if (e->title_load(e) != 0) return;
    if (t.shown != e) t.t0 = openpete_interp_present_now();
    t.shown = e;
    t.logged = false;
    OPL_INFO_F("launcher", "launcher title: the 3D title of %s (%s)", e->game_id, why);
}

bool launcher_title_prepare(float win_w, float win_h, float x0, float cy, float width, float gap) {
    t.drawn = false;
    if (!t.shown) {
        (void)openpete_native_launcher_set_title(nullptr, 0, 0, 0);
        return false;
    }
    openpete_launcher_title q = {x0, cy, width, gap, title_tick()};
    openpete_launcher_title_glyph gl[kMaxGlyphs] = {};
    float cap_h = 0.0f;
    const int n = t.shown->title_emit(t.shown, kText, &q, t.verts, kMaxVerts, &cap_h, gl, kMaxGlyphs);
    t.drawn = n >= 3 && openpete_native_launcher_set_title(t.verts, n, (int)win_w, (int)win_h) == 1;
    if (!t.drawn) return false;
    t.rect[0] = x0;
    t.rect[1] = cy - cap_h * 0.5f;
    t.rect[2] = x0 + width;
    t.rect[3] = cy + cap_h * 0.5f;
    if (!t.logged) {
        t.logged = true;
        OPL_INFO_F("launcher", "launcher title: 3D title of %s drawn at tick %d (%d vertices, cap %.2f px)",
                   t.shown->game_id, q.tick, n, cap_h);
    }
    int gn = 0;
    while (gn < kMaxGlyphs && gl[gn].x1 > gl[gn].x0) gn++;
    title_dump(q, win_w, win_h, cap_h, n, gl, gn);
    return true;
}

bool launcher_title_chosen(void) { return t.shown != nullptr; }

bool launcher_title_drawn(float rect[4]) {
    if (t.drawn && rect) memcpy(rect, t.rect, sizeof t.rect);
    return t.drawn;
}
