# Audio

Spyro the Dragon has its own **SOUND EFFECTS** and **MUSIC** sliders in its
options menu, stored in the game's save. OpenPete can preset them at launch
and mute everything.

| Flag | Config key | Values | Default |
| --- | --- | --- | --- |
| `--sfx N`<!-- id: flag:--sfx --> | `sfx` | `0` (off) to `10` (max), `on` (= 10), `off` (= 0), `-1` | `-1`<!-- id: knob:sfx -->: the slider stays as the game has it |
| `--music N`<!-- id: flag:--music --> | `music` | same as `sfx` | `-1`<!-- id: knob:music --> |
| `--mute 0\|1`<!-- id: flag:--mute --> | `mute` | `0`, `1`, `on`, `off` | `0`<!-- id: knob:mute -->: audio on |

## Presetting the sliders

`--sfx` and `--music` set the game's own slider, so the game plays at that
level exactly as if you had moved the slider. The preset is applied again
over the title screen and after loading a save, so a save made at another
volume does not undo it. Moving the slider in the game's options menu ends
the preset for the rest of the session.

```toml
[game.spyro-1]
music = 0     # music off, sound effects as saved
sfx   = 7
```

## Muting

`--mute 1` silences all output: sound effects, music and
[replacement music](music-and-sound-packs.md). It does not change the
game's sliders. The [dev overlay](../help/glossary.md#dev-overlay)'s **Audio** section toggles mute and moves
both presets while the game runs.

Audio is also silent while the game is paused (++f7++) and while
`--skip-to-level` [fast-forwards](../help/glossary.md#fast-forward) to a level, and returns to the mute setting
afterwards.

## Replacing music and sounds

See [Music and sound packs](music-and-sound-packs.md).
