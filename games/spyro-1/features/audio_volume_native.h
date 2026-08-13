#pragma once
/* audio_volume_native.h — --sfx / --music: preset Spyro 1's own in-game
 * volume sliders, and make the music slider audible.
 *
 * Two jobs, both driven from one tick hook (see audio_volume_native.c):
 *  - stamp the requested slider levels into the game's volume globals,
 *    re-stamping over the clobbers the game does on its own, until the
 *    player moves that slider in the options menu;
 *  - mirror the game's music slider onto the XA / music-WAV OpenAL gain,
 *    which is otherwise never applied (SpuSetCommonAttr is a no-op in
 *    PsyCross, so the slider would only act as an on/off switch).
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

typedef struct CPUState CPUState;

/* Apply the pending presets and refresh the music gain. Call once per tick;
 * inert (no guest writes) while both presets are unset. */
void spyro1_audio_volume_on_tick(CPUState* cpu);
