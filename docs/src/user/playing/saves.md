# Saves, savestates and rewind

OpenPete keeps two kinds of save. Memory cards hold the game's own saves,
made from the game's menus as on a PlayStation. Savestates are OpenPete's
snapshots of the whole running game, made and loaded with a key at any
moment.

## Memory cards

Two formatted memory cards are inserted at every start, as `card1.mcr` and
`card2.mcr` in the cards folder ([File locations](../help/file-locations.md)).
Every write the game makes goes to the file at once. The files are standard
128 KiB PlayStation card images.

| Setting | Effect |
| --- | --- |
| `--card-dir PATH` <!-- id: knob:card_dir --> | Use another folder for both cards |
| `--card1 PATH` <!-- id: flag:--card1 home -->, `--card2 PATH` <!-- id: flag:--card2 home --> | Use a specific image for port A or port B |
| `--no-card` <!-- id: flag:--no-card --> | Insert no card; the game cannot save |
| `--card-unformatted` <!-- id: flag:--card-unformatted --> | Provision a blank unformatted card, so the game's own format screen runs |

### Mods and memory cards

A mod that changes the game can change what a save means. OpenPete records
which mods a card's progress was made under in a file beside the card
(`card1.mcr.modset`), counting only mods that change game state. The record
is written at the card's first save under mods.

Each start compares that record with the mods running now. On a mismatch
OpenPete writes a warning to the log and lets the game load and save as
usual; the record stays as it was, so the warning repeats. The cases:

- A card with progress from an unmodded game, loaded with mods running.
- A card made under mods, loaded with no mods running.
- A card made under a different set or version of mods.

`--adopt-saves` <!-- id: flag:--adopt-saves --> rewrites the record to the
mods running now and ends the warnings.

## Savestates

| Key | Action |
| --- | --- |
| ++1++ <!-- id: key:host.savestate_save home --> | Save to the active slot |
| ++2++ <!-- id: key:host.savestate_load home --> | Load from the active slot |
| ++3++ <!-- id: key:host.savestate_cycle home --> | Make the next slot active |

There are 10 slots, numbered 0 to 9; the active slot starts at 0 each run and
the slot key wraps from 9 to 0. Each slot is a file, `slot0.state` to
`slot9.state`, in the states folder
([File locations](../help/file-locations.md)). Saving overwrites the slot.
The log records each save, load and slot change; there is no on-screen
message.

The savestate keys do nothing while the settings menu is open.

A load is refused, and the running game left untouched, when:

- the state was made by a build whose compiled game differs from this one,
  which includes most upgrades;
- the state was made under a different set of game-changing mods (the log
  says **MODSET MISMATCH**; enable the same mods and versions).

!!! warning
    A savestate restores the whole machine, memory card data excepted: a
    game save made after the savestate stays on the card when you load the
    state.

## Rewind

Holding ++backspace++ <!-- id: key:host.rewind home --> steps the game back one
[tick](../help/glossary.md#tick) at a time. Rewind is off by default <!-- id: knob:rewind -->; turn it
on with `--rewind N` <!-- id: knob:rewind -->, where N is how many seconds
of play to keep, or with the **Rewind window** row of the
[dev overlay](dev-overlay.md) (0 to 600 seconds <!-- id: knob:rewind -->).

- Each second of window costs about 2 MiB of memory.
- Rewind stays within the current level: loading a level clears the history.
- Changing the window clears the history.
- A code mod that does not declare how it keeps its own state blocks rewind;
  the log names the mod.
- With rewind off, pressing ++backspace++ writes one line to the log saying
  how to turn it on.
