/* core/runtime/bugknobs_class.c - the bug-report replay class of a knob
 * registry row. The recorder writes it into knobs.txt for the reader; the
 * replay classifies this build's own row with the same function, so a
 * bundle's class text never decides what the replay applies. See
 * openpete_bugreport.h. */
#include "openpete_bugreport.h"

#include "openpete_knobs.h"

#include <string.h>

/* Rows the replay does not apply, by id, with their class: `host` rows set
 * window, pacing, audio, input, logging or menu state that the replay's own
 * options decide (headless, --mute, flat-out pacing, no devices); `sim` rows
 * act on the sim's timeline (the free camera freezes the player update, the
 * camera lock writes the camera, pause stops the ticks); a `follow` row is a
 * selection that tracks game state (the level warp's target follows the
 * player's level), so it is not sampled after boot. Every other live row
 * writes renderer state (the registry's contract) and is `render`. */
static const struct { const char* id; const char* cls; } k_bk_class[] = {
    {"fullscreen", "host"}, {"vsync", "host"}, {"render_fps", "host"}, {"tick_scale", "host"},
    {"runahead", "host"}, {"rewind", "host"}, {"mute", "host"}, {"sfx", "host"},
    {"music", "host"}, {"vibration", "host"}, {"mouse", "host"}, {"pad_unfocused", "host"},
    {"mouse_sensitivity", "host"}, {"log_level", "host"}, {"log_format", "host"},
    {"save_mismatch_toast", "host"}, {"record_fps_overlay", "host"}, {"state_slot", "host"},
    {"warp_level", "follow"}, {"freecam", "sim"}, {"camlock", "sim"}, {"pause", "sim"},
};

int openpete_bugknobs_is_env(const struct knob_desc* k)
{
    return k->env && !k->cli && k->type == KNOB_STRING;
}

const char* openpete_bugknobs_class(const struct knob_desc* k)
{
    if (k->type == KNOB_ACTION) return "action";
    if (k->mut != KNOB_LIVE) return "latched";
    if (openpete_bugknobs_is_env(k)) return "env";
    for (size_t i = 0; i < sizeof k_bk_class / sizeof k_bk_class[0]; i++)
        if (!strcmp(k->id, k_bk_class[i].id)) return k_bk_class[i].cls;
    return "render";
}
