# Practice tools

These tools serve practice and exploration: stopping and stepping the game,
running it faster or slower, starting in any level, and flying the camera.

!!! warning "Tools that change the game"
    The cheats, the level warp, the free camera and the teleport change the
    game's state, which the unmodified game would not reach from the same
    inputs. Pause, tick-step, fast-forward and game speed change only when
    the game runs, not what it computes.

## Pause and tick-step

{{ key("key:host.pause") }} <!-- id: key:host.pause home --> pauses and resumes the game. Pausing mutes
the audio and stops controller vibration; resuming restores both.

While paused, {{ key("key:host.frame_step") }} <!-- id: key:host.frame_step home --> advances the game by one
[tick](../help/glossary.md#tick), the game's own update step: 29.913 per
second. Holding ++f8++ keeps stepping.

## Fast-forward

Holding {{ key("key:host.turbo") }} <!-- id: key:host.turbo home --> runs the game as fast as the
computer can, with no wait between ticks. Releasing it returns to normal
speed.

## Game speed

`--tick-scale F` <!-- id: knob:tick_scale --> runs the whole game at F times
normal speed: 0.5 is half-speed slow motion. Default 1.0
<!-- id: knob:tick_scale -->; the dev overlay's **Tick scale** row takes 0.05
to 8 <!-- id: knob:tick_scale --> and applies at once. The game computes the
same ticks at any speed; only the time between them changes.

## Starting in a level

`--skip-to-level L` <!-- id: knob:skip_to_level --> starts a new game directly
in level L, given as the game's level number or its name in
`lower_snake_case`. `--skip-to-level` with no value, or an unknown name,
prints the list; [Spyro the Dragon](../games/spyro-1.md) lists the Spyro 1
names. In a window, the logos, title screen and loading run fast-forwarded
behind a black window with the audio muted until the level is playable.

```
openpete-spyro1 --skip-to-level stone_hill
```

To change level during a session, use the level warp in the
[dev overlay](dev-overlay.md#level-warp).

`--skip-to-cutscene C` <!-- id: knob:skip_to_cutscene --> plays a named
cutscene. The Spyro 1 names:

| Name | Plays |
| --- | --- |
| `intro` | The opening, from the title screen through New Game; not combinable with `--skip-to-level` |
| `ending` | Gnasty defeated, from level 63 |
| `ending_loot` | Gnasty's Loot, from level 64; the credits return to Artisans |
| `credits` | The short credits roll, from the `--skip-to-level` target or Artisans; returns to the level it started from |
| `credits_100` | The full 100% credits roll, same rules as `credits` |

## Cheats

`--cheats A,B` <!-- id: knob:cheats --> applies the game's own built-in cheat
codes without entering them on the pad. Spyro 1 has two:

| Name | Effect | The game's code |
| --- | --- | --- |
| `99_lives` | Sets Spyro's extra lives to 99 | "99 LIVES" |
| `unlock_all_levels` | Marks every level visited and every homeworld unlocked | "LEVELS OPEN" |

A cheat is applied once per session, on the first tick of play in a level,
and again for each new session started from the title screen. Dying after
`99_lives` still costs a life. An unknown name prints the list and the
program does not start.

The dev overlay has an **Apply** button per cheat that fires it during play
and can be pressed again; see [Dev overlay](dev-overlay.md#cheat-buttons).

## Free camera

{{ key("key:host.freecam") }} <!-- id: key:host.freecam home -->, or the menu's
**FREECAM** row, detaches the camera from Spyro. While it is on, the game's
camera and Spyro's own update are frozen and the camera flies:
[Free camera keys](controls.md#free-camera-keys). The speed keys step
through five speeds; the camera starts at the middle one and keeps the
chosen speed for the rest of the session.

Turning the free camera off returns the camera to the game.

The free camera cannot be turned on while a bk2 movie replays or records
([Recording](recording.md)).

## Teleport

With the free camera on, {{ key("key:host.teleport") }} <!-- id: key:host.teleport home --> moves Spyro to
the camera's position, turns the free camera off and returns the camera to
the game, on the next tick. With the free camera off, ++t++ does nothing.
