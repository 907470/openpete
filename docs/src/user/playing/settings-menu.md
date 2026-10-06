# The settings menu

The settings menu is a panel drawn over the game in the game's own lettering.
It opens and closes with {{ key("key:host.menu") }} <!-- id: key:host.menu home -->; see
[Controls](controls.md#openpete-keys).

!!! note
    The game keeps running while the menu is open. The PlayStation pad is
    released for as long as the menu is up, so the game receives no input,
    and every change shows live behind the panel. Pause first (++f7++) to stop the game
    underneath.

## Moving through the menu

| | Keyboard | Gamepad | Mouse |
| --- | --- | --- | --- |
| Previous / next row | ++up++ / ++down++ <!-- id: key:host.menu_up home --> | D-pad, left stick | Point at the row |
| Change the value | ++left++ / {{ key("key:host.menu_right") }} <!-- id: key:host.menu_right --> | D-pad, left stick; Cross also steps forward | Left click steps forward, right click back, wheel either way |
| Activate **EXIT GAME** | {{ key("key:host.menu_select") }} <!-- id: key:host.menu_select home --> | Cross | Left click |
| Close | the menu key, ++esc++ | Start or Circle, on release <!-- id: key:host.menu_close home --> | The close mark in the corner |

Values wrap around at either end. Each change raises an on-screen
notification naming the row and its new value; see
[Display](../graphics/display.md#on-screen-notifications).

## Rows

| Row | Values | Setting | Page |
| --- | --- | --- | --- |
| **DISPLAY** | **WINDOWED**, **FULLSCREEN** | {{ config_key("knob:fullscreen") }} <!-- id: knob:fullscreen --> | [Display](../graphics/display.md#fullscreen) |
| **ASPECT** | **AUTO**, **4:3**, **16:10**, **16:9**, **21:9**, **32:9** | {{ config_key("knob:aspect") }} <!-- id: knob:aspect --> | [Display](../graphics/display.md#aspect-ratio) |
| **FPS** | 30, 60, then steps of 60 below your display's refresh rate, the refresh rate, **UNLIMITED** | {{ config_key("knob:render_fps") }} <!-- id: knob:render_fps --> | [Smoothness](../graphics/smoothness.md#frame-rate) |
| **FOV** | **DEFAULT**, then 30° to 120° vertical in 5° steps, shown as vertical and horizontal angle | {{ config_key("knob:fov") }} <!-- id: knob:fov --> | [Display](../graphics/display.md#field-of-view) |
| **SHOW FPS** | **ON**, **OFF** | {{ config_key("knob:perf_stats") }} <!-- id: knob:perf_stats --> | [Performance](../graphics/performance.md#the-performance-readout) |
| **VSYNC** | **ON**, **OFF** | {{ config_key("knob:vsync") }} <!-- id: knob:vsync --> | [Display](../graphics/display.md#vsync) |
| **SSAA** | **OFF**, **2X**, **3X**, **4X** | {{ config_key("knob:ssaa") }} <!-- id: knob:ssaa --> | [Quality](../graphics/quality.md#supersampling) |
| **WOBBLE** | **OFF**, **ON** | {{ config_key("knob:vertex_precision") }} <!-- id: knob:vertex_precision --> | [PlayStation look](../graphics/retro-look.md#wobble) |
| **PIXELATION** | **OFF**, **1X**, **2X**, **4X**, **8X**, **16X** | {{ config_key("knob:raster_quantize") }} <!-- id: knob:raster_quantize --> | [PlayStation look](../graphics/retro-look.md#pixelation) |
| **TEXTURES** | **CORRECT**, **AFFINE** | {{ config_key("knob:texture_mapping") }} <!-- id: knob:texture_mapping --> | [PlayStation look](../graphics/retro-look.md#texture-warp) |
| **DITHER** | **OFF**, **15-BIT** | {{ config_key("knob:dither") }} <!-- id: knob:dither --> | [PlayStation look](../graphics/retro-look.md#dither) |
| **LOD** | **ORIGINAL**, **EXTENDED** | {{ config_key("knob:lod") }} <!-- id: knob:lod --> | [Quality](../graphics/quality.md#level-of-detail) |
| **FREECAM** | **OFF**, **ON** | the free camera | [Practice tools](practice-tools.md#free-camera) |
| **SETTINGS** | **SAVE**, **CLEAR SAVED** | | [below](#saving-your-settings) |
| **EXIT GAME** | | | [below](#exit-game) |

**SHOW FPS** **OFF** hides the performance readout and **ON** shows it. The
detailed pacing readout is chosen with `--perf-stats detailed` or the dev
overlay.

**PIXELATION** shows the nearest step when the current value is off the
ladder (for example 3 set from the dev overlay) and changes it only when
you step the row.

## Saving your settings

Changes made in the menu last for the session. To keep them:

- ++right++ (or Cross, or a left click) on **SETTINGS** writes every setting
  changed this session into the `[saved]` block of your `openpete.toml`. The
  row reports **SAVED** and the count, or **FAILED**.
- ++left++ on **SETTINGS** empties the `[saved]` block and reports
  **CLEARED**. The current session keeps its values; the next start uses the
  rest of the file.

Closing the menu never writes the file. Hand-written lines above the
`[saved]` block stay as they are. Saved settings beat the rest of the file,
but a flag typed on the command line or an exported environment variable
beats them for that launch; see [The config file](../customizing/config-file.md).

## EXIT GAME

The first press changes the row to **REALLY EXIT?**; the second quits.
Moving to another row disarms it. The memory cards are written before the
program exits, as on any other quit.

## Settings the menu does not show

Everything else is a command-line flag, an `openpete.toml` key, or a row of
the [dev overlay](dev-overlay.md). The config key is the flag name without
the leading dashes and with `-` as `_` (`--fog-scale` is `fog_scale`).
`--help` lists the flags; {{ flag("flag:--config-report") }} <!-- id: flag:--config-report -->
prints which setting won and what it overrode.
