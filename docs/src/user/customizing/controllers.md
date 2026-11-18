# Controllers

A connected gamepad plays the game with its own buttons; no setup and no
keyboard are needed. The first connected controller is port 1. The layout
follows the controller's positions: the bottom face button is
**Cross**, the right one **Circle**, the left one **Square**, the top one
**Triangle**. [Key bindings](key-bindings.md) moves any of them.

## A controller that is not recognised

OpenPete reads controllers through SDL, which needs a mapping line for each
controller model. A controller without one is ignored, or has its buttons
in the wrong places. A mapping can be added without changing anything the
release ships.

1. Get the line for your controller from
   [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB),
   which also documents how to generate a line for a controller it lacks.
2. Put the line in a file named `gamecontrollerdb.txt` in your config
   directory:

=== "Windows"

    `%APPDATA%\openpete\gamecontrollerdb.txt`

=== "Linux"

    `~/.config/openpete/gamecontrollerdb.txt`
    (`$XDG_CONFIG_HOME/openpete/` when `XDG_CONFIG_HOME` is set)

One line is enough. The file is merged with the shipped mappings, and your
line wins where both describe the same controller. It survives upgrades.
A mapping added while the game runs takes effect on the next start.

The shipped `gamecontrollerdb.txt` next to the program can be edited too;
extracting a new release over the folder replaces it.

### Order of precedence

Lowest first; a later source replaces an earlier one's line for the same
controller:

1. SDL's built-in mappings.
2. The file named by the environment variable
   `SDL_GAMECONTROLLERCONFIG_FILE`, when set. The shipped file is then
   skipped.
3. The shipped `gamecontrollerdb.txt` next to the program.
4. Your `gamecontrollerdb.txt` in the config directory.
5. The file named by `--controller-db PATH`<!-- id: flag:--controller-db -->
   (config key `controller_db`, variable `OPENPETE_CONTROLLER_DB`).

Mappings in the environment variable `SDL_GAMECONTROLLERCONFIG` outrank
every file. Steam sets it for its virtual controller.

`openpete.log` records each mapping file looked for, and for each connected
controller the mapping used and where it came from.

## Vibration

`--vibration N`<!-- id: flag:--vibration --> sets the rumble strength from
`0`<!-- id: knob:vibration --> (off) to `10`<!-- id: knob:vibration --> (full
strength). The default, `{{ default("knob:vibration") }}`<!-- id: knob:vibration -->,
leaves the game's own vibration setting alone, which is on. `on` and `off`
are also accepted. The motors are silent while the window is not focused,
unless `--pad-unfocused` is on (below). Turning vibration off in the game's
own options menu takes effect as usual.

## Stick response

`--stick-response MODE`<!-- id: flag:--stick-response --> sets how far the
game thinks the analog stick is pushed for a given position of your stick.
`console`<!-- id: knob:stick_response --> reaches the full value at about
three quarters of the stick's travel, so a light push moves Spyro further
and the last quarter of the travel changes nothing. The game is verified
against this response. `linear`<!-- id: knob:stick_response --> spreads the
stick's travel evenly over the PlayStation's range, so the game sees exactly
as much of a push as you make. The default is
`{{ default("knob:stick_response") }}`<!-- id: knob:stick_response home -->.
The config key is `stick_response`; the
[dev overlay](../help/glossary.md#dev-overlay)'s Input section switches it
while the game runs. A keyboard key bound to the stick gives the full value
under either response, and movie playback is not affected.

## Controller in the background

`--pad-unfocused`<!-- id: flag:--pad-unfocused --> keeps reading every
controller while another window has focus, so the pad plays the game behind
a split timer, a stream window or a chat. Rumble follows the same setting.
The default, `{{ default("knob:pad_unfocused") }}`<!-- id: knob:pad_unfocused -->,
ignores a controller while the window is in the background. `--pad-unfocused 0`
turns it off again from the command line; the config key is
`pad_unfocused`, the variable `OPENPETE_PAD_UNFOCUSED`. The
[dev overlay](../help/glossary.md#dev-overlay)'s Input section switches it
while the game runs. The keyboard and the mouse still need the window
focused.

## Mouse

The mouse moves the [free camera](../help/glossary.md#free-camera)'s view and drives the [settings menu](../help/glossary.md#settings-menu) and the
[dev overlay](../help/glossary.md#dev-overlay). `--mouse-sensitivity S`<!-- id: flag:--mouse-sensitivity -->
scales mouse look: `{{ default("knob:mouse_sensitivity") }}`<!-- id: knob:mouse_sensitivity home --> (the default) is
one full turn per 8192 counts; the range is
`0`<!-- id: knob:mouse_sensitivity --> to
`100`<!-- id: knob:mouse_sensitivity -->. `--mouse 0`<!-- id: flag:--mouse home -->
ignores the mouse and leaves the pointer free over the window. To invert
the vertical look, bind `look_axis_y = "pad:-righty,mouse:y"` under
`[keys.host]`.
