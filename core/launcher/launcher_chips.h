/* launcher_chips.h - the launcher's corner chips, anchored to the window's
 * bottom corners: bottom left the background shader compile's progress
 * ("Compiling shaders 42%") with a spinner while the launcher's precompile
 * runs, bottom right the name of the level whose dome is on screen, hidden
 * with no dome; a click on the level chip shows the next level's dome
 * (launcher_sky_next). The game page adds the Mods chip at the top-left
 * corner (launcher_mods_chip_draw). Each is one line of the body font at 10 layout px on
 * a chip (padding 8 x 6 layout px, 12 layout px from the window's edges),
 * drawn through the shape pass and recorded in the element manifest as
 * status.shaders and status.skybox (their boxes <id>.panel, the spinner
 * status.shaders.spinner). Launcher thread, inside a present's imgui
 * callback. */
#ifndef OPENPETE_LAUNCHER_CHIPS_H
#define OPENPETE_LAUNCHER_CHIPS_H

/* The corner chips' metrics, layout px: the text size (as ProggyClean,
 * launcher_push_body_font), the padding around the line, the margin to the
 * window's edges, and the spinner's gap before the text. */
constexpr float kCornerPx = 10.0f;
constexpr float kCornerPadX = 8.0f, kCornerPadY = 6.0f;
constexpr float kCornerMargin = 12.0f;
constexpr float kSpinGap = 6.0f;

/** @brief Draw the corner chips for one present: the shader compile's chip while the launcher's precompile runs and @p shader_step is false, and the level chip while a dome shows; a click on the level chip starts the crossfade to the next dome.
 *
 * The level chip is a button: the pointer over it is the hand cursor, and
 * a left click (on press) calls launcher_sky_next. The level's name comes
 * from its game's launcher entry (openpete_launcher_game.level_name); a
 * level the entry does not name shows its id. The spinner is a 270 degree
 * arc turning once per second on the sky clock (launcher_sky_clock),
 * round caps, stroke 1.5 layout px, its diameter the text height, in the
 * chip's text colour.
 * @param scale Window pixels per layout pixel.
 * @param shader_step true while the view is the shader step, which shows
 *        the compile's progress as its own bar. */
void launcher_corner_chips_draw(float scale, bool shader_step);

/** @brief Draw the Mods chip at the window's top-left corner, the corner chips' metrics mirrored to the top edge; a button that opens the mods folder.
 *
 * One line of the body font at kCornerPx on a chip padded by kCornerPadX x
 * kCornerPadY, kCornerMargin from the top and left edges, recorded in the
 * element manifest as page.mods (its box page.mods.panel). The keyboard and
 * pad focus draws the text in the title's orange; the pointer over it is the
 * hand cursor. The caller acts on a press (openpete_mods_open_folder).
 * @param scale Window pixels per layout pixel.
 * @param focused true while the keyboard or pad focus is on the chip.
 * @return true when a left click pressed it this present. */
bool launcher_mods_chip_draw(float scale, bool focused);

#endif /* OPENPETE_LAUNCHER_CHIPS_H */
