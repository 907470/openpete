# Controls

Any gamepad SDL recognises plays the game with its own buttons; no keyboard
is needed. The keyboard stands in for the PlayStation pad and also carries
OpenPete's own keys. Every binding below can be changed; see
[Key bindings](../customizing/key-bindings.md). `--keys-report`
<!-- id: flag:--keys-report --> prints every action, its binding and which
config section set it.

## The PlayStation pad

| PlayStation | Keyboard | Gamepad |
| --- | --- | --- |
| Cross | ++c++ <!-- id: key:pad.cross home --> | South face button (Xbox A) |
| Circle | ++v++ <!-- id: key:pad.circle home --> | East face button (Xbox B) |
| Square | ++x++ <!-- id: key:pad.square home --> | West face button (Xbox X) |
| Triangle | ++z++ <!-- id: key:pad.triangle home --> | North face button (Xbox Y) |
| D-pad | ++up++ ++down++ ++left++ ++right++ <!-- id: key:pad.dpad_up home --> | D-pad |
| Left stick | ++w++ ++a++ ++s++ ++d++ <!-- id: key:pad.stick_up home --> | Left stick |
| Start | ++enter++ <!-- id: key:pad.start home --> | Start |
| Select | ++space++ <!-- id: key:pad.select home --> | Back |
| L1 | ++shift++ (left) <!-- id: key:pad.l1 home --> | Left shoulder |
| L2 | ++ctrl++ (left) <!-- id: key:pad.l2 home --> | Left trigger |
| L3 | ++bracket-left++ <!-- id: key:pad.l3 home --> | Left stick click |
| R1 | ++shift++ (right) <!-- id: key:pad.r1 home --> | Right shoulder |
| R2 | ++ctrl++ (right) <!-- id: key:pad.r2 home --> | Right trigger |
| R3 | ++bracket-right++ <!-- id: key:pad.r3 home --> | Right stick click |

Keyboard keys are positional: on a non-US layout the binding is the key in
the position the US layout names.

Only the first pad has default bindings. Pads 2 to 4 (`[keys.pad2]` to
`[keys.pad4]`) start unbound.

## OpenPete keys

| Action | Key | Page |
| --- | --- | --- |
| Open or close the settings menu | ++escape++ <!-- id: key:host.menu --> | [Settings menu](settings-menu.md) |
| Quit | none <!-- id: key:host.quit home --> | |
| Open or close the dev overlay | ++m++ <!-- id: key:host.devui --> | [Dev overlay](dev-overlay.md) |
| Toggle fullscreen | ++alt+enter++ | [Display](../graphics/display.md#fullscreen) |
| Screenshot | ++f12++ <!-- id: key:host.screenshot --> | [Recording](recording.md#screenshots) |
| Savestate: save to the active slot | ++1++ <!-- id: key:host.savestate_save --> | [Saves](saves.md#savestates) |
| Savestate: load from the active slot | ++2++ <!-- id: key:host.savestate_load --> | [Saves](saves.md#savestates) |
| Savestate: next slot | ++3++ <!-- id: key:host.savestate_cycle --> | [Saves](saves.md#savestates) |
| Rewind (hold) | ++backspace++ <!-- id: key:host.rewind --> | [Saves](saves.md#rewind) |
| Pause / resume | ++f7++ <!-- id: key:host.pause --> | [Practice tools](practice-tools.md#pause-and-tick-step) |
| Advance one tick while paused | ++f8++ <!-- id: key:host.frame_step --> | [Practice tools](practice-tools.md#pause-and-tick-step) |
| Fast-forward (hold) | ++tab++ <!-- id: key:host.turbo --> | [Practice tools](practice-tools.md#fast-forward) |
| Free camera on / off | ++f++ <!-- id: key:host.freecam --> | [Practice tools](practice-tools.md#free-camera) |
| Drop Spyro at the free camera | ++t++ <!-- id: key:host.teleport --> | [Practice tools](practice-tools.md#teleport) |
| Toggle interpolation | ++i++ <!-- id: key:host.interp_toggle --> | [Smoothness](../graphics/smoothness.md#interpolation) |
| Toggle LOD original / extended | ++l++ <!-- id: key:host.lod_toggle --> | [Quality](../graphics/quality.md#level-of-detail) |

++alt+enter++ is fixed. The rebindable `fullscreen` action is a second,
single-key route to the same toggle and is unbound by default
<!-- id: key:host.fullscreen home -->.

++esc++ closes an open settings menu or dev overlay whatever it is bound to.
No key quits by default; the window's close button always quits, and
[Key bindings](../customizing/key-bindings.md) binds `quit` to a key.

## Free camera keys

While the free camera is on, these keys fly it. Keyboard and pad inputs add
together.

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Forward / back | ++w++ ++s++ <!-- id: key:host.fly_forward home --> | Left stick |
| Strafe left / right | ++a++ ++d++ <!-- id: key:host.fly_left --> | Left stick |
| Rise | ++space++ <!-- id: key:host.fly_up home --> | Right shoulder |
| Descend | ++shift++ (left) <!-- id: key:host.fly_down --> | Left shoulder |
| Look | ++up++ ++down++ ++left++ ++right++ <!-- id: key:host.look_up home -->, mouse | Right stick |
| Faster / slower | ++bracket-right++ ++bracket-left++ <!-- id: key:host.speed_up home --> | Right trigger / left trigger |

Mouse look is scaled by `--mouse-sensitivity` <!-- id: knob:mouse_sensitivity -->
(default 1.0 <!-- id: knob:mouse_sensitivity -->, one full turn per 8192
mouse counts). Binding `look_axis_y` to `"pad:-righty,mouse:y"` inverts the
mouse's vertical look; see [Key bindings](../customizing/key-bindings.md).

## The mouse

The mouse drives the settings menu, the dev overlay and free camera look.
`--mouse 0` <!-- id: knob:mouse --> ignores every mouse event and leaves the
pointer a normal desktop cursor over the window; default on
<!-- id: knob:mouse -->.
