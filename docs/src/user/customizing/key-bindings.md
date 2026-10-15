# Key bindings

Every key and pad button OpenPete reads is rebindable in
[`openpete.toml`](config-file.md), in four tables, or, for the first
controller and OpenPete's keys, on the [dev overlay](../playing/dev-overlay.md#controls)'s
**Controls** section, which writes the file's `[saved.keys]` block:

| Table | Holds |
| --- | --- |
| `[keys.pad]` | the PlayStation controller in port 1 |
| `[keys.pad2]` ... `[keys.pad4]` | further controllers; unbound by default |
| `[keys.host]` | OpenPete's own keys: menus, [savestates](../help/glossary.md#savestate), pause, [free camera](../help/glossary.md#free-camera), [screenshots](../help/glossary.md#screenshot) |
| `[keys.mod.<id>]` | keys a mod declares (the mod's documentation names them) |

Each table works in any section of the file, so a binding can be global or
limited to one game or disc:

```toml
[keys.pad]
cross = ["C", "pad:south"]      # either source presses cross
l2    = "Left Ctrl"

[keys.host]
savestate_save = "F5"
savestate_load = "F9"
rewind         = "none"         # unbind

[game.spyro-1.keys.host]
screenshot = "F11"              # Spyro 1 only
```

`openpete --keys-report`<!-- id: flag:--keys-report home --> prints every
action, its binding, the section that set it and the action's purpose, then
exits.

## Writing a binding

A binding is a string or a list of strings. A list, or one string with
commas (`"C,pad:south"`), binds several sources to one action. `"none"` or
`[]` unbinds the action.

A section that names an action replaces that action's whole list and leaves
every other action alone. To add a key to a default, write the default
sources and the new one.

### Keyboard

A bare name is a keyboard key, spelled as SDL names it: `"F5"`,
`"Left Shift"`, `"Keypad 5"`, `"Return"`, `"Escape"`. Keys are positional:
on an AZERTY keyboard `"A"` is the key where a US layout has A. The
`--keys-report` output shows the label your layout gives each bound key.

### Controller

A controller source takes the `pad:` prefix.

| Source | Spellings |
| --- | --- |
| face buttons, by position | `pad:south` `pad:east` `pad:west` `pad:north` |
| face buttons, PlayStation names | `pad:cross` `pad:circle` `pad:square` `pad:triangle` |
| shoulders and stick clicks | `pad:l1` `pad:r1` `pad:l3` `pad:r3` (also `pad:leftshoulder`, `pad:rightshoulder`, `pad:leftstick`, `pad:rightstick`) |
| triggers | `pad:l2` `pad:r2` (also `pad:lefttrigger`, `pad:righttrigger`) |
| other buttons | `pad:start` `pad:select` (also `pad:back`), `pad:dpup` `pad:dpdown` `pad:dpleft` `pad:dpright` |
| stick axes | `pad:leftx` `pad:lefty` `pad:rightx` `pad:righty` |

A `-` before an axis inverts it: `pad:-lefty`. `pad:` means the first
controller; `pad2:` ... `pad4:` name the others. Inside `[keys.pad2]`, a bare
`pad:` reads that player's own controller.

### Holding

`hold:` before a key or button source makes it act only once it has been
held for half a second: `devui = ["M", "hold:pad:back"]`. It binds the
actions of `[keys.host]` that a press triggers; the PlayStation pad tables,
the axes and the mouse wheel do not take it.

### Mouse

`mouse:` sources bind in `[keys.host]` and `[keys.mod.<id>]` only:

| Source | Binds to |
| --- | --- |
| `mouse:left` `mouse:middle` `mouse:right` `mouse:x1` `mouse:x2` | any button action |
| `mouse:wheel_up` `mouse:wheel_down` | press actions; not actions held down, such as [fast-forward](../help/glossary.md#fast-forward) |
| `mouse:x` `mouse:y` (`mouse:-y` inverts) | the look axes `look_axis_x` and `look_axis_y` only |

`--mouse 0`<!-- id: flag:--mouse --> ignores the mouse entirely.

## Default bindings

The built-in defaults. The [shipped `openpete.toml`](config-file.md#the-shipped-file)
changes none of them.

### Controller (`[keys.pad]`)

| Action | Keyboard | Controller |
| --- | --- | --- |
| `cross` | {{ key("key:pad.cross") }}<!-- id: key:pad.cross --> | `pad:south` |
| `circle` | {{ key("key:pad.circle") }}<!-- id: key:pad.circle --> | `pad:east` |
| `square` | {{ key("key:pad.square") }}<!-- id: key:pad.square --> | `pad:west` |
| `triangle` | {{ key("key:pad.triangle") }}<!-- id: key:pad.triangle --> | `pad:north` |
| `l1` / `r1` | {{ key("key:pad.l1") }}<!-- id: key:pad.l1 --> / {{ key("key:pad.r1") }}<!-- id: key:pad.r1 --> | shoulders |
| `l2` / `r2` | {{ key("key:pad.l2") }}<!-- id: key:pad.l2 --> / {{ key("key:pad.r2") }}<!-- id: key:pad.r2 --> | triggers |
| `l3` / `r3` | {{ key("key:pad.l3") }}<!-- id: key:pad.l3 --> / {{ key("key:pad.r3") }}<!-- id: key:pad.r3 --> | stick clicks |
| `start` | {{ key("key:pad.start") }}<!-- id: key:pad.start --> | `pad:start` |
| `select` | {{ key("key:pad.select") }}<!-- id: key:pad.select --> | `pad:back` |
| `dpad_up` `dpad_down` `dpad_left` `dpad_right` | arrow keys<!-- id: key:pad.dpad_up --> | d-pad |
| `stick_up` `stick_down` `stick_left` `stick_right` | ++w++ ++s++ ++a++ ++d++<!-- id: key:pad.stick_up --> | (keyboard only) |
| `left_x` `left_y` `right_x` `right_y` | none | `pad:leftx` <!-- id: key:pad.left_x -->, `pad:lefty` <!-- id: key:pad.left_y -->, `pad:rightx` <!-- id: key:pad.right_x -->, `pad:righty` <!-- id: key:pad.right_y --> |

### OpenPete keys (`[keys.host]`)

| Action | Default | Does |
| --- | --- | --- |
| `menu` | {{ key("key:host.menu") }}<!-- id: key:host.menu --> | open or close the [settings menu](../help/glossary.md#settings-menu) |
| `devui` | {{ key("key:host.devui") }}<!-- id: key:host.devui --> | open the [dev overlay](../playing/dev-overlay.md); close it on release |
| `devui_up` `devui_down` `devui_left` `devui_right` | d-pad, left stick<!-- id: key:host.devui_up --> | move in the dev overlay |
| `devui_activate` / `devui_cancel` | `pad:south` / `pad:east`<!-- id: key:host.devui_activate --> | activate or leave the focused item in the dev overlay |
| `devui_menu` | `pad:west`<!-- id: key:host.devui_menu --> | dev overlay: change a value with the d-pad; hold to switch windows |
| `devui_slower` / `devui_faster` | `pad:leftshoulder` / `pad:rightshoulder`<!-- id: key:host.devui_slower --> | dev overlay: smaller or larger steps; previous or next window |
| `menu_up` `menu_down` `menu_left` `menu_right` | arrow keys, d-pad, left stick<!-- id: key:host.menu_up --> | move in the settings menu; `menu_right` is also `pad:south` |
| `menu_select` | ++enter++, `pad:south`<!-- id: key:host.menu_select --> | activate the selected row |
| `menu_close` | `pad:start`, `pad:east`<!-- id: key:host.menu_close --> | close the settings menu |
| `quit` | {{ key("key:host.quit") }}<!-- id: key:host.quit --> | quit the game |
| `fullscreen` | {{ key("key:host.fullscreen") }}<!-- id: key:host.fullscreen --> | toggle fullscreen |
| `screenshot` | {{ key("key:host.screenshot") }}<!-- id: key:host.screenshot --> | save a PNG of the image on screen |
| `savestate_save` | {{ key("key:host.savestate_save") }}<!-- id: key:host.savestate_save --> | quick-save to the active slot |
| `savestate_load` | {{ key("key:host.savestate_load") }}<!-- id: key:host.savestate_load --> | quick-load from the active slot |
| `savestate_cycle` | {{ key("key:host.savestate_cycle") }}<!-- id: key:host.savestate_cycle --> | select the next savestate slot |
| `rewind` | {{ key("key:host.rewind") }}<!-- id: key:host.rewind --> | hold to rewind (needs `--rewind`) |
| `pause` | {{ key("key:host.pause") }}<!-- id: key:host.pause --> | pause or resume |
| `frame_step` | {{ key("key:host.frame_step") }}<!-- id: key:host.frame_step --> | advance one [tick](../help/glossary.md#tick) while paused |
| `turbo` | {{ key("key:host.turbo") }}<!-- id: key:host.turbo --> | hold to fast-forward |
| `freecam` | {{ key("key:host.freecam") }}<!-- id: key:host.freecam --> | engage or leave the free camera |
| `teleport` | {{ key("key:host.teleport") }}<!-- id: key:host.teleport --> | drop Spyro at the free camera |
| `fly_forward` `fly_back` `fly_left` `fly_right` | ++w++ ++s++ ++a++ ++d++<!-- id: key:host.fly_forward --> | free camera: move |
| `fly_up` / `fly_down` | ++space++, `pad:rightshoulder` / ++left-shift++, `pad:leftshoulder`<!-- id: key:host.fly_up --> | free camera: rise / descend |
| `look_up` `look_down` `look_left` `look_right` | arrow keys<!-- id: key:host.look_up --> | free camera: turn |
| `speed_up` / `speed_down` | ++bracket-right++, `pad:righttrigger` / ++bracket-left++, `pad:lefttrigger`<!-- id: key:host.speed_up --> | free camera: faster / slower |
| `fly_axis_x` `fly_axis_y` | `pad:leftx`<!-- id: key:host.fly_axis_x -->, `pad:-lefty`<!-- id: key:host.fly_axis_y --> | free camera: move axes |
| `look_axis_x` `look_axis_y` | `pad:rightx,mouse:x`<!-- id: key:host.look_axis_x -->, `pad:-righty,mouse:-y`<!-- id: key:host.look_axis_y --> | free camera: look axes |
| `interp_toggle` | {{ key("key:host.interp_toggle") }}<!-- id: key:host.interp_toggle --> | toggle [frame interpolation](../help/glossary.md#frame-interpolation) |
| `lod_toggle` | {{ key("key:host.lod_toggle") }}<!-- id: key:host.lod_toggle --> | toggle [extended detail](../help/glossary.md#extended-detail) |

++alt+enter++ always toggles fullscreen, whatever `fullscreen` is bound to.
++escape++ always closes an open settings menu or [dev overlay](../help/glossary.md#dev-overlay) first. The
window's close button always quits.

### Development actions

These actions have no built-in binding. `--keys-report` shows each as
`dev (default X)`, where X is the key in brackets below; bind one in your
own file to use it.

| Action | Does |
| --- | --- |
| `camlock` (++g++) | lock the displayed view while the game keeps running |
| `glitch_mark` (++f9++)<!-- id: key:host.glitch_mark --> | write the last presents and terrain walks to a file; bound in a development build only |
| `fillpick` (++p++), `fillpick_preview` (++h++), `fillpick_fill` (++y++), `fillpick_nofill` (++n++), `fillpick_none` (++b++), `fillpick_undo` (++u++) | the fill-tag inspector, a renderer development tool |

## Shared keys

Two actions may share a key when they are never live at the same moment:
the arrow keys drive both the d-pad and the settings menu, and ++w++ ++a++
++s++ ++d++ drive both the left stick and the free camera. A `hold:`
controller source on one of OpenPete's keys may share its button with the
PlayStation pad (a tap reaches the game) and with the settings menu's and
dev overlay's navigation (a held button acts only as navigation while
either is open); `menu` and `devui` share Start and Back this way. `--keys-report`
lists intended overlaps under "shared inputs". The log warns about a source
bound to two actions that can be live at the same moment, and about a
required action, such as `menu`, left with no binding.
