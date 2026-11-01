# Display

Settings for the window, its shape and the camera's view. Each section names
the settings menu row, the dev overlay row and the flag; the config key is
the flag without the leading dashes, with `-` as `_`.

## Window

The window opens at the game's drawn picture at twice its size: 640x448
<!-- id: knob:window --> for Spyro 1. `--window WxH`
<!-- id: knob:window --> sets another starting size, for example
`--window 1920x1080`, and `--window maximized` opens the window maximized.
Drag the window's edges to resize it at any time.

On Wayland and macOS, a display with scaling above 100% counts the size in
the desktop's scaled units, and the game draws at the display's full
resolution: at 200% scaling `--window 1280x800` opens a window of the same
apparent size as on an unscaled display, drawn at 2560x1600 pixels. On
Windows and X11 the size counts pixels.

`--background` <!-- id: flag:--background --> opens the window at its
normal size without taking focus, behind the other windows where the
platform allows it; clicking the window or its taskbar button focuses it.
It wins over `--fullscreen`. Default 0 <!-- id: knob:background -->.

## Fullscreen

Fullscreen is borderless, at the desktop's resolution.

| | |
| --- | --- |
| Toggle | ++alt+enter++, a double-click in the window, the settings menu's **DISPLAY** row, the dev overlay's **Fullscreen** row |
| Start fullscreen | `--fullscreen 1` <!-- id: knob:fullscreen home --> |
| Default | windowed <!-- id: knob:fullscreen -->; the Spyro 1 package's `openpete.toml` sets `fullscreen = true` <!-- id: knob:fullscreen @spyro-1 home --> |

## Launcher window

The launcher opens windowed at 1280x800 (in the units `--window` counts), or at the size `--window` sets when
one is set (maximized when that is maximized); a size set for one game in
`[game.<id>]` does not apply to it. A size that does not fit the display's
usable area opens maximized when it fits the whole display, and otherwise
at 90% of the usable area. When the game starts, the window changes to the
game's own size, or goes straight to fullscreen when the game starts
fullscreen. Its window resizes and maximizes like the game's, and
++alt+enter++ or a double-click outside its buttons and entries toggles
fullscreen.

A fullscreen toggle, resize or maximize made in the launcher carries to the
game started from it, over `--fullscreen`, `--window` and `openpete.toml`.
A launcher left as it opened leaves the game's own settings in force. The
carried state lasts for that start only; the next start of OpenPete uses the
settings again.

| | |
| --- | --- |
| Start the launcher fullscreen | `--launcher-fullscreen 1` <!-- id: knob:launcher_fullscreen home -->; default windowed <!-- id: knob:launcher_fullscreen --> |
| Launcher size | `--launcher-window WxH` or `--launcher-window maximized` <!-- id: knob:launcher_window home -->; default 1280x800, or the `--window` size <!-- id: knob:launcher_window --> |

Neither setting changes the game's window unless the window is changed in
the launcher.

## Aspect ratio

OpenPete renders widescreen as Hor+: the picture keeps the game's vertical
view and shows more at the sides.

| Value | Effect |
| --- | --- |
| `auto` (default <!-- id: knob:aspect home -->) | Follows the window's shape, and the screen's in fullscreen, as it is resized |
| `W:H`, for example `16:9`, `21:9` | Renders at that fixed shape. Choosing one in a window resizes the window to match; a window of another shape shows bars. |

Settings menu **ASPECT** and the dev overlay's **Aspect ratio** offer
**AUTO**, **4:3**, **16:10**, **16:9**, **21:9** and **32:9**; the flag
{{ flag("knob:aspect") }} <!-- id: knob:aspect --> takes any `W:H`.

## Field of view

`--fov DEG` <!-- id: knob:fov home --> sets the vertical field of view, from 30 to
120 degrees <!-- id: knob:fov -->. The default is the game's own projection,
55 degrees vertical <!-- id: knob:fov -->, which is 74 degrees horizontal at
4:3 and 90 degrees at 16:9. The horizontal angle always follows the aspect
ratio.

The settings menu's **FOV** row steps from **DEFAULT** through 30 to 120 in
5-degree steps and shows the vertical and horizontal angles, for example
**55V 90H**. The dev overlay's **Field of view** row has an **Original**
checkbox for the game's projection; unchecked, a slider beside it takes any
value in the range.

The field of view applies to the native renderer only.

## VSync

VSync on <!-- id: knob:vsync home --> waits for the display's refresh before
showing each picture; the [frame rate](../help/glossary.md#frame-rate) then
cannot exceed the refresh rate. VSync off shows pictures as soon as they are
ready: lower input latency and uncapped frame rate, with possible tearing.

Settings menu **VSYNC**, dev overlay **VSync**, `--vsync 0|1`
<!-- id: knob:vsync -->. A change rebuilds the display chain at the next
picture.

## On-screen notifications

Changing a setting raises a notification in the game's gold border and
lettering. Mods raise them too. Several stack.

| Setting | Default | Flag |
| --- | --- | --- |
| Notifications on or off | {{ default("knob:osd") }} <!-- id: knob:osd --> | `--osd on\|off` <!-- id: knob:osd --> |
| Position | {{ default("knob:osd_position") }} <!-- id: knob:osd_position --> | {{ flag("knob:osd_position") }} <!-- id: knob:osd_position -->: `top-left`, `top-center`, `top-right`, `bottom-left`, `bottom-center`, `bottom-right` |
| Seconds on screen | {{ default("knob:osd_timeout") }} <!-- id: knob:osd_timeout -->, at most 30 <!-- id: knob:osd_timeout --> | {{ flag("knob:osd_timeout") }} <!-- id: knob:osd_timeout --> |

The notifications and the performance readout
([Performance](performance.md#the-performance-readout)) can share a corner:
the readout keeps its place and the notifications stack clear of it. Both
move aside while the settings menu is open.

## Version line

A release build shows its version at the bottom left for two seconds after
start. `--version-osd off` <!-- id: knob:version_osd --> hides it; default on
<!-- id: knob:version_osd -->. A distributed build that is not a tagged
release shows its version for the whole session, and this setting does not
hide it.

## Renderer

`--renderer vk` <!-- id: knob:renderer --> (the default
<!-- id: knob:renderer -->) draws with OpenPete's native Vulkan renderer.
`--renderer psycross` draws with the PsyCross OpenGL reference renderer,
shown in the same window. The field of view, supersampling, level of detail,
terrain visibility, vertex precision and texture mapping settings apply to
the native renderer only. The renderer is chosen at start.

## Graphics device

On a computer with more than one Vulkan device, such as integrated graphics
beside a graphics card, OpenPete chooses one automatically: a discrete
graphics card first, then integrated graphics, then a software renderer
<!-- id: knob:gpu -->. The log file lists every device with its index at
start, then the one chosen and why.

`--gpu INDEX|NAME` <!-- id: knob:gpu home --> chooses another: its index in
that list, or part of its name, in any case (`--gpu 1`, `--gpu radeon`); a
value of digits only is always an index.
The variable `OPENPETE_VK_DEVICE` takes the same values; the flag wins over
it. A value that matches no device, or names one that cannot draw to the
window, is written to the log, and the automatic choice is used. The device
is chosen at start, before the launcher opens.
