# The dev overlay

The dev overlay is a panel titled **openpete config** listing every setting
OpenPete has, grouped by section, with each row's current value and where it
came from. It changes live settings without a restart, saves them to the
config file, and carries the cheat buttons and the level warp.

{{ key("key:host.devui") }} <!-- id: key:host.devui home --> opens and closes it. ++esc++ also closes it. It is drawn
by the native renderer in the game window.

The game keeps running behind the panel. Mouse use leaves the game playable;
while a text field in the panel has the keyboard, the keyboard's game and
hotkey bindings pause and the PlayStation pad is released.

## Reading the panel

| Part | Meaning |
| --- | --- |
| Search field | Shows the rows whose name, flag or environment variable contains the text, with every section holding a match opened. |
| Row value | Changes apply at once on a live row. A row that needs a restart is drawn greyed out. |
| Row title tooltip | The setting's description, its flag and variable spellings, "restart to apply" on a restart row, and where the value came from: the command line, the environment or the default, "edited this session" once changed, and the `openpete.toml` section that set it, for example `[saved]`. **pinned** marks a setting that a flag or variable of this launch overrides; saving it takes effect once that override is gone. |
| Button tooltip | A button's status, such as **not in a level**, above its description. |
| Right click on a row title | **Reset to boot value**, **Copy as CLI flag**, **Copy as env line**. |

**Save settings**, beside the search field, writes every row changed this
session into the `[saved]` block of the config file, as the settings menu's
**SETTINGS** row does; its tooltip shows the result of the last save. The
settings menu's **CLEAR SAVED** empties that block. Closing the panel writes
nothing. See
[Saving your settings](settings-menu.md#saving-your-settings).

## Sections

The graphics rows are described under [Graphics](../graphics/display.md);
each Graphics page names its settings. Sections marked collapsed start
closed; click a title to open it.

| Section | Rows |
| --- | --- |
| Display | Fullscreen, Aspect ratio, Render FPS, Field of view, Supersampling, VSync, Performance stats (level and position), Vertex precision, Raster quantize, Color depth, Texture mapping, LOD |
| Display, Advanced (collapsed) | Notifications (on, position, timeout), Fog (on and scale), Render mode, Terrain GPU cache, Terrain visibility, Render paths (absent from release packages) |
| Audio (collapsed) | Mute, SFX volume, Music volume |
| Gameplay | Freecam, Tick scale, Pause sim, Runahead, Rewind, Level warp |
| Gameplay, Input (collapsed) | Vibration, Mouse sensitivity |
| Gameplay, Cheats (collapsed) | one button per cheat, on games that have them |
| Info (collapsed) | the launch-only rows: Skip to level, Skip to cutscene, Skip to sub-area, Freecam pose, Player pose, bk2 replay, bk2 frame offset, bk2 recording, Renderer, Extraction threads, Window size |
| Info, Paths (collapsed) | every path and memory card row |
| Recording | the video capture rows, shown only while recording |
| Mods | one row per installed mod |
| Debug, Debug diagnostics (collapsed) | diagnostic settings |

Button groups (Vertex precision, Color depth, Texture mapping, LOD, Terrain
visibility) highlight the current choice. **Color depth** **15-bit** is the
dither setting on, **24-bit** off. **Runahead** has a checkbox; buttons for 1,
2 and 3 ticks appear beside it while runahead is on. The same pattern holds
elsewhere: a row's further controls show only while its first control is
on, as Render FPS's target (while **Interpolation** is on), Field of view's
slider (while **Original** is off), Performance
stats' position, Notifications' position and timeout, Fog's scale, and the
**SFX volume** and **Music volume** sliders beside their **Override**
checkbox.

**Render paths** draws only the chosen render paths (sky, Spyro, mobys,
terrain, shadows, flame, particles and the rest), one checkbox each, for
comparing one path in isolation; **All** returns to normal drawing and
**None** draws no path. It changes only the picture. **Rewind** has a checkbox; the window in seconds
appears beside it while rewind is on.

**Pause sim** <!-- id: knob:pause --> stops the game's ticks while the
picture keeps presenting, as the pause key ({{ key("key:host.pause") }} <!-- id: key:host.pause -->)
does; audio mutes while paused. Checked, it shows a **Step tick** button
<!-- id: knob:frame_step --> that advances the game by one tick, as the step
key ({{ key("key:host.frame_step") }} <!-- id: key:host.frame_step -->) does. It starts unchecked
<!-- id: knob:pause -->.

**Freecam pose** <!-- id: knob:freecam_pose --> engages the free camera
at a pose (`pos=(x, y, z) yaw=N pitch=N`, the format the free camera logs)
once the level is live. **Player pose** <!-- id: knob:player_pose --> places
Spyro at `x,y,z[,yaw]` once gameplay is live, with the camera re-seated
behind him. Both are empty by default, and both change the game's state.

**Render mode** offers **Textured**, **Untextured**
<!-- id: knob:notex --> and **Wireframe** <!-- id: knob:wireframe -->.
Untextured and Wireframe are inspection views: the first replaces textures
with grey to show the lighting alone, the second draws every triangle's
edges. {{ flag("flag:--notex") }}<!-- id: flag:--notex --> followed by `1`
turns the untextured view on at launch, as the variable `OPENPETE_NOTEX=1`
does; the flag wins over the variable. The view is off by default.

The camera lock <!-- id: knob:camlock --> holds the picture on the current
view, and lets it fly with the free camera keys, while the game keeps running
underneath: cutscenes play on. It changes only the picture, not the game.
Turning it on turns the free camera off. It has no row in the panel; its
key, `camlock`, is unbound unless configured <!-- id: key:host.camlock -->.

## Cheat buttons

Gameplay's **Cheats** subsection has one row per cheat, **99 lives** and
**Unlock all levels** on Spyro the Dragon, each with an **Apply** button. A
press applies the cheat on the next tick in a level, during play or on the
pause and inventory screens; it can be pressed again, for example to top up
lives. The button's tooltip reads **queued**, **applied**, or **not in a
level**; outside a level the button is disabled. The cheats are the ones
`--cheats` applies at start; see
[Practice tools](practice-tools.md#cheats).

## Level warp

The **Level warp** row moves to any level mid-session. Its list
<!-- id: knob:warp_level --> picks the destination and follows the level you
are in until you pick one; the **Warp** button <!-- id: knob:warp_go -->
starts the warp.

In a level, the warp runs the game's own exit-level sequence and then loads
the chosen level; progress is kept. From the title screen it starts a new
session in the chosen level, with progress and lives reset as with
`--skip-to-level`; the button's tooltip reads **from title (resets
progress)**. The button is disabled while the free camera is on (**freecam
active**) or when no route exists (**not in a level**).

## Mods

The Mods section lists each installed mod with a checkbox that enables or
disables it at the next tick. A mod that registers its own controls shows
them under its name. A mod that adds levels lists each with a **Warp**
button. See [Mods](../customizing/mods.md).

## The I and L keys

Two more keys toggle renderer settings.

| Key | Action | Notification |
| --- | --- | --- |
| {{ key("key:host.interp_toggle") }} <!-- id: key:host.interp_toggle --> | Toggle [interpolation](../graphics/smoothness.md#interpolation) on and off | **INTERPOLATION: ON** / **OFF** |
| {{ key("key:host.lod_toggle") }} <!-- id: key:host.lod_toggle --> | Toggle [LOD](../graphics/quality.md#level-of-detail) between original and extended | **LOD: ORIGINAL** / **EXTENDED** |

Interpolation off presents each tick's picture unchanged between ticks,
which shows what interpolation adds.
