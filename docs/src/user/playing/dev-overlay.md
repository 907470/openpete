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
| **live editable only** | Checked by default: shows only the rows that apply at once. Uncheck it to see the rows that need a restart, drawn greyed out. |
| **filter** | Shows the rows whose name, flag or environment variable contains the text. |
| Row value | Changes apply at once on a live row. |
| Source column | `cli` (a command-line flag), `env` (an environment variable), `def` (the default), plus the `openpete.toml` section that set it, for example `[saved]`. **pinned** marks a row that a flag or variable of this launch overrides; saving it takes effect once that override is gone. |
| Tooltip | The setting's description, its flag and variable spellings, and "restart to apply" on a restart row. |
| Right click on a row | **Reset to boot value**, **Copy as CLI flag**, **Copy as env line**. |

**Save settings** writes every row changed this session into the `[saved]`
block of the config file, as the settings menu's **SETTINGS** row does.
**Reset saved** empties that block; the effect shows at the next start.
Closing the panel writes nothing. See
[Saving your settings](settings-menu.md#saving-your-settings).

## Sections

The graphics rows are described under [Graphics](../graphics/display.md);
each Graphics page names its settings.

| Section | Live rows |
| --- | --- |
| Display | Fullscreen, Aspect ratio, Field of view, Supersampling, VSync, Performance stats, Stats position, Notifications, Version line, Notification position, Notification timeout, Vertex precision, Raster quantize, Terrain visibility, Untextured (lighting), Wireframe (lighting), Dither (15-bit), Texture mapping, LOD, Fog, Fog scale |
| Pacing | Render FPS, Tick scale, Runahead, Rewind window |
| Audio | Mute, SFX volume, Music volume |
| Input | Vibration, Mouse, Mouse sensitivity |
| Paths | none; every path row needs a restart |
| Gameplay | Free camera, Camera lock (render), the cheat buttons, the level warp |
| Mods | one row per installed mod |

Restart rows include Renderer, Window size, Extraction threads, Terrain GPU
cache, Input lock, every path, and the launch-only gameplay rows (Skip to
level, Skip to cutscene, Cheats at launch, bk2 replay).

**Untextured (lighting)** <!-- id: knob:notex --> and **Wireframe
(lighting)** <!-- id: knob:wireframe --> are inspection views: the first
replaces textures with grey to show the lighting alone, the second draws
every triangle's edges.

**Camera lock (render)** <!-- id: knob:camlock --> holds the picture on the
current view, and lets it fly with the free camera keys, while the game keeps
running underneath: cutscenes play on. It changes only the picture, not the
game. Turning it on turns the free camera off. Its key, `camlock`, is unbound
unless configured <!-- id: key:host.camlock -->.

## Cheat buttons

The Gameplay section has one row per cheat, **Cheat: 99 lives** and **Cheat:
unlock all levels**, each with an **Apply** button. A press applies the
cheat on the next tick in a level, during play or on the pause and inventory
screens; it can be pressed again, for example to top up lives. The row's
status reads **queued**, **applied**, or **not in a level**. The cheats are
the ones `--cheats` applies at start; see
[Practice tools](practice-tools.md#cheats).

## Level warp

Two Gameplay rows move to any level mid-session:

- **Warp: level** picks the destination. It follows the level you are in
  until you pick one.
- **Warp: jump to level** has the **Jump** button.

In a level, the jump runs the game's own exit-level sequence and then loads
the chosen level; progress is kept. From the title screen it starts a new
session in the chosen level, with progress and lives reset as with
`--skip-to-level`; the status reads **from title (resets progress)**. The
button is disabled while the free camera is on (**freecam active**) or when
no route exists (**not in a level**).

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
