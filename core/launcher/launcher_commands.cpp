/* launcher_commands.cpp - the game page's command state: the focus over its
 * boxes and the Mods chip (moved by keys and the pad, taken by the pointer
 * only after it moves), the ask-once quit of the east button, and the game whose box takes
 * the focus when the page next shows. The event-to-command map is ask_of in
 * launcher.cpp; this file holds no SDL or imgui state. */
#include "launcher_internal.h"

#include "openpete_log.h"

#include <cstdio>

namespace {

/* The game id launcher_focus_game_set named, "" for none. */
char s_focus_game[32] = {};

}  // namespace

int launcher_focus_move(launcher_focus* f, int dir) {
    const int from = f->index;
    int to = from + (dir < 0 ? -1 : dir > 0 ? 1 : 0);
    if (to < 0) to = 0;
    if (to > f->count - 1) to = f->count - 1;
    if (to < 0) to = 0;
    f->index = to;
    f->held = true;
    OPL_INFO_F("launcher", "command: focus %s from box %d to box %d%s", dir < 0 ? "left" : "right",
               from, to, to == from ? " (edge, no wrap)" : "");
    return to != from;
}

int launcher_focus_mods(launcher_focus* f, bool on) {
    f->held = true;
    if (f->mods == on) return 0;
    f->mods = on;
    if (on) OPL_INFO_F("launcher", "command: focus up from box %d to the Mods chip", f->index);
    else OPL_INFO_F("launcher", "command: focus from the Mods chip back to box %d", f->index);
    return 1;
}

void launcher_focus_pointer_moved(launcher_focus* f) { f->held = false; }

bool launcher_focus_hover(launcher_focus* f, int hovered) {
    if (f->held || hovered < 0 || hovered >= f->count || (hovered == f->index && !f->mods)) return false;
    OPL_INFO_F("launcher", "command: focus follows the pointer from %s %d to box %d", f->mods ? "the Mods chip, box" : "box",
               f->index, hovered);
    f->index = hovered;
    f->mods = false;
    return true;
}

bool launcher_quit_ask_press(launcher_quit_ask* q, unsigned long long now_ms) {
    if (launcher_quit_ask_shown(q, now_ms)) {
        OPL_INFO_F("launcher", "command: quit (east again %llu ms after the ask)", now_ms - q->asked_ms);
        q->asked_ms = 0;
        return true;
    }
    q->asked_ms = now_ms ? now_ms : 1;
    OPL_INFO_F("launcher", "command: quit asked (press again within %d ms)", LAUNCHER_QUIT_ASK_MS);
    return false;
}

bool launcher_quit_ask_shown(const launcher_quit_ask* q, unsigned long long now_ms) {
    return q->asked_ms != 0 && now_ms >= q->asked_ms && now_ms - q->asked_ms < LAUNCHER_QUIT_ASK_MS;
}

void launcher_focus_game_set(const char* game) {
    std::snprintf(s_focus_game, sizeof s_focus_game, "%s", game ? game : "");
}

const char* launcher_focus_game(void) { return s_focus_game; }
