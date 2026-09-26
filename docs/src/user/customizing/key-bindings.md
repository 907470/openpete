# Key bindings

Every key and pad button OpenPete reads is rebindable in
[`openpete.toml`](config-file.md), in four tables:

| Table | Holds |
| --- | --- |
| `[keys.pad]` | the PlayStation controller in port 1 |
| `[keys.pad2]` … `[keys.pad4]` | further controllers; unbound by default |
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

`openpete-spyro1 --keys-report`<!-- id: flag:--keys-report home --> prints every
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
controller; `pad2:` … `pad4:` name the others. Inside `[keys.pad2]`, a bare
`pad:` reads that player's own controller.

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
changes some of them for Spyro 1: ++escape++ <!-- id: key:host.menu @spyro-1 --> opens the [settings menu](../help/glossary.md#settings-menu) and does
not quit, and ++m++ <!-- id: key:host.devui @spyro-1 -->, ++i++ <!-- id: key:host.interp_toggle @spyro-1 --> and ++l++ <!-- id: key:host.lod_toggle @spyro-1 --> are bound.

### Controller (`[keys.pad]`)

| Action | Keyboard | Controller |
| --- | --- | --- |
| `cross` | ++c++<!-- id: key:pad.cross --> | `pad:south` |
| `circle` | ++v++<!-- id: key:pad.circle --> | `pad:east` |
| `square` | ++x++<!-- id: key:pad.square --> | `pad:west` |
| `triangle` | ++z++<!-- id: key:pad.triangle --> | `pad:north` |
| `l1` / `r1` | ++left-shift++<!-- id: key:pad.l1 --> / ++right-shift++<!-- id: key:pad.r1 --> | shoulders |
| `l2` / `r2` | ++left-ctrl++<!-- id: key:pad.l2 --> / ++right-ctrl++<!-- id: key:pad.r2 --> | triggers |
| `l3` / `r3` | ++bracket-left++<!-- id: key:pad.l3 --> / ++bracket-right++<!-- id: key:pad.r3 --> | stick clicks |
| `start` | ++enter++<!-- id: key:pad.start --> | `pad:start` |
| `select` | ++space++<!-- id: key:pad.select --> | `pad:back` |
| `dpad_up` `dpad_down` `dpad_left` `dpad_right` | arrow keys<!-- id: key:pad.dpad_up --> | d-pad |
| `stick_up` `stick_down` `stick_left` `stick_right` | ++w++ ++s++ ++a++ ++d++<!-- id: key:pad.stick_up --> | (keyboard only) |
| `left_x` `left_y` `right_x` `right_y` | none | stick axes |

### OpenPete keys (`[keys.host]`)

| Action | Default | Does |
| --- | --- | --- |
| `menu` | ++o++<!-- id: key:host.menu --> | open or close the settings menu |
| `menu_up` `menu_down` `menu_left` `menu_right` | arrow keys, d-pad, left stick<!-- id: key:host.menu_up --> | move in the settings menu; `menu_right` is also `pad:south` |
| `menu_select` | ++enter++, `pad:south`<!-- id: key:host.menu_select --> | activate the selected row |
| `menu_close` | `pad:start`, `pad:east`<!-- id: key:host.menu_close --> | close the settings menu |
| `quit` | ++escape++<!-- id: key:host.quit --> | quit the game |
| `fullscreen` | none<!-- id: key:host.fullscreen --> | toggle fullscreen |
| `screenshot` | ++f12++<!-- id: key:host.screenshot --> | save a PNG of the image on screen |
| `savestate_save` | ++1++<!-- id: key:host.savestate_save --> | quick-save to the active slot |
| `savestate_load` | ++2++<!-- id: key:host.savestate_load --> | quick-load from the active slot |
| `savestate_cycle` | ++3++<!-- id: key:host.savestate_cycle --> | select the next savestate slot |
| `rewind` | ++backspace++<!-- id: key:host.rewind --> | hold to rewind (needs `--rewind`) |
| `pause` | ++f7++<!-- id: key:host.pause --> | pause or resume |
| `frame_step` | ++f8++<!-- id: key:host.frame_step --> | advance one [tick](../help/glossary.md#tick) while paused |
| `turbo` | ++tab++<!-- id: key:host.turbo --> | hold to fast-forward |
| `freecam` | ++f++, `pad:leftstick`<!-- id: key:host.freecam --> | engage or leave the free camera |
| `teleport` | ++t++<!-- id: key:host.teleport --> | drop Spyro at the free camera |
| `fly_forward` `fly_back` `fly_left` `fly_right` | ++w++ ++s++ ++a++ ++d++<!-- id: key:host.fly_forward --> | free camera: move |
| `fly_up` / `fly_down` | ++space++, `pad:rightshoulder` / ++left-shift++, `pad:leftshoulder`<!-- id: key:host.fly_up --> | free camera: rise / descend |
| `look_up` `look_down` `look_left` `look_right` | arrow keys<!-- id: key:host.look_up --> | free camera: turn |
| `speed_up` / `speed_down` | ++bracket-right++, `pad:righttrigger` / ++bracket-left++, `pad:lefttrigger`<!-- id: key:host.speed_up --> | free camera: faster / slower |
| `fly_axis_x` `fly_axis_y` | `pad:leftx`, `pad:-lefty`<!-- id: key:host.fly_axis_x --> | free camera: move axes |
| `look_axis_x` `look_axis_y` | `pad:rightx,mouse:x`, `pad:-righty,mouse:-y`<!-- id: key:host.look_axis_x --> | free camera: look axes |

++alt+enter++ always toggles fullscreen, whatever `fullscreen` is bound to.
++escape++ always closes an open settings menu or [dev overlay](../help/glossary.md#dev-overlay) first. The
window's close button always quits.

### Development actions

These actions have no built-in binding. `--keys-report` shows each as
`dev (default X)`, where X is the key in brackets below; bind one in your
own file to use it.

| Action | Does |
| --- | --- |
| `devui` (++m++) | open or close the [dev overlay](../playing/dev-overlay.md); bound by the shipped file |
| `interp_toggle` (++i++) | toggle [frame interpolation](../help/glossary.md#frame-interpolation); bound by the shipped file |
| `lod_toggle` (++l++) | toggle [extended detail](../help/glossary.md#extended-detail); bound by the shipped file |
| `camlock` (++g++) | lock the displayed view while the game keeps running |
| `glitch_mark` (++f9++)<!-- id: key:host.glitch_mark --> | write the last presents and terrain walks to a file; bound in a development build only |
| `fillpick` (++p++), `fillpick_preview` (++h++), `fillpick_fill` (++y++), `fillpick_nofill` (++n++), `fillpick_none` (++b++), `fillpick_undo` (++u++) | the fill-tag inspector, a renderer development tool |

## Shared keys

Two actions may share a key when they are never live at the same moment:
the arrow keys drive both the d-pad and the settings menu, and ++w++ ++a++
++s++ ++d++ drive both the left stick and the free camera. `--keys-report`
lists intended overlaps under "shared inputs". The log warns about a source
bound to two actions that can be live at the same moment, and about a
required action, such as `menu`, left with no binding.
