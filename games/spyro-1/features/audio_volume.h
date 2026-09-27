#pragma once
/* audio_volume.h — --sfx / --music: preset Spyro 1's own in-game
 * volume sliders, and make the music slider audible.
 *
 * Two jobs, both driven from one tick hook (see audio_volume.c):
 *  - stamp the requested slider levels into the game's volume globals,
 *    re-stamping over the clobbers the game does on its own, until the
 *    player moves that slider in the options menu;
 *  - mirror the game's music slider onto the XA / music-WAV OpenAL gain,
 *    which is otherwise never applied (SpuSetCommonAttr is a no-op in
 *    PsyCross, so the slider would only act as an on/off switch).
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/** @brief Applies the pending --sfx and --music presets and refreshes the music gain.
 *
 * Called once per tick. Writes no guest RAM while both presets are unset,
 * and does nothing on a runahead speculative tick, whose guest writes roll
 * back while this module's latches and a cleared host request would not.
 * With the sound-options submenu open it stamps nothing and drops a preset
 * whose slider the player moved.
 * @param cpu The guest CPU state. */
void spyro1_audio_volume_on_tick(CPUState* cpu);
