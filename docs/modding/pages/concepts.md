# Concepts

## The override chain

The prologue of every recompiled guest function passes through a single
dispatch chokepoint, so a per-address chain intercepts every call,
whether direct, indirect, or dispatched, without patching any code.
Pre-hooks, post-hooks, and full replacement are all the same mechanism.
What distinguishes them is where you call `api->base(cpu)`:

``` c
static void my_override(CPUState* cpu) {
    /* code here runs BEFORE the original        (pre-hook)  */
    api->base(cpu);
    /* code here runs AFTER the original         (post-hook) */
    /* never call base() at all: full replacement            */
}
```

You can call `base()` zero, one, or many times, and each call runs the
continuation once. Enabled-list order sets the priority: later mods run
first, and `base()` walks toward earlier mods and then to the recompiled
original.

There is no separate event system, because an event is a post-hook plus
a state diff around `base()`. To detect a collected gem, for example,
post-hook the collector and compare the counter before and after.

## Tick context and present context

openpete simulates at the PlayStation's canonical rate of 29.913 ticks
per second, but it can present many frames per tick for high-framerate
interpolation. That split is the most important contract in the API.

**Tick context** covers overrides, your entry point, and level hooks.
The simulation is live. Guest reads and writes, `call`, and registration
are legal here and nowhere else.

**Present context** covers hooks you install with
`register_present_hook`. It's render-pure both by contract and by
enforcement: guest access, game calls, and registrations are refused
with an error.

The reason is that presents aren't countable. How many run per tick
depends on the player's framerate, and the engine extracts scenes ahead
of their display instant and can produce fewer than one per display
slot. Anything you did to the game from a present hook would therefore
happen a different number of times on different machines, which is not
something you can write a working mod against.

Publish state from your tick hooks into your own variables, then
evaluate your own curves at `ctx->alpha` for sub-tick smoothness.

Draw calls work from both contexts. `draw_text` and `draw_rect`
submitted during a tick persist for that tick's presents; submitted from
a present hook, they live for one present.

## Whether your mod changes gameplay

Some mods change what the game does; others only change what the player
sees. openpete needs to know which yours is, so that it can keep the
player's saves straight.

Most of the time it works this out on its own. A mod that ships no C and
claims no `[[level]]` has no way to reach guest RAM, so the engine knows
it can't change gameplay and you declare nothing. The draw service, the
sound overlay, and the texture service can't write guest RAM either,
because each substitutes on the host side.

Once your mod ships C, the engine can't tell what that C does, so it
assumes the mod changes gameplay. If yours doesn't, say so:

``` toml
[mod]
sim_mutative = false   # this mod never writes guest RAM
```

Get it wrong in the cautious direction and the player sees a save
warning they didn't need. Get it wrong the other way and a card quietly
records progress it can't reproduce, which is why the assumption runs
the way it does.

### What the answer is used for

openpete tags a memory card with the gameplay mods that were loaded when
it was first written, and warns the player before mixing that card with
a different set. Progress is never blocked, and the tag only changes
when the player passes `--adopt-saves`. A mod that writes no guest RAM
stays out of the tag, so a purely visual mod never makes a card look
incompatible.

## Guest memory

Guest RAM is a 2 MB little-endian mirror. `api->guest(vaddr)` returns a
host pointer, accepting kuseg, kseg0, and kseg1 addresses, and
`api->guest_addr` converts back. Three rules apply:

1.  Pointers stored inside guest structs are guest virtual addresses.
    Translate them again before you follow them.
2.  Use the [typed structs](reference/structs.md) wherever they exist.
    Their layouts are pinned with `_Static_assert`, so drift fails
    *your* compile rather than a player's session.
3.  Take named addresses from the `OP_GADDR_*` constants documented in
    [Globals](reference/globals.md), and from the `OP_FNADDR_*`
    constants. Never hardcode a number that you can name.

For guest-addressable scratch space, `api->guest_alloc` bump-allocates
in the expansion window above 2 MB and gives you a host view of it.

!!! note "One caveat inherited from the moby format"
    Animated-model vertex streams use 21-bit offsets, so any payload for
    one of those must live in the low 2 MB. Coordinate a fixed park
    below the executable base at `0x80010000`.

## Services

| Goal | Service | Writes guest RAM? |
|---|---|---|
| Draw HUD text or rectangles | `draw_text`, `draw_rect` | No |
| Replace a game sound | `sfx_replace` | No for voice names, yes for def-shaped names |
| Play your own WAV file | `sfx_play` | No |
| Put custom textures on your models | `assets/textures/own/<hexid>.png` | No |
| Add post-process shaders | `shader_register` with `postfx_*` | No |
| Re-shade some of what the engine draws | `[[material]]` rows, or `material_register` | No |
| Add custom levels | `[[level]]` with `assets/levels/` | Yes |
| Expose player settings | A `[[config]]` schema with `config_*` | No |
| Show status lines | `ui_status` | No |
| Announce something that just happened | `notify` | No |
| Add hotkeys | `[[binding]]` with `binding_down` | Only if you act on the key in a tick hook |

### `ui_status` or `notify`?

They answer different questions. `ui_status` is **state** — a block of
lines in your mod's section of the Mods panel, repainted whenever you
print again, visible only while the player has that overlay open.
`notify` is an **event** — a toast in the game's own gold chrome, on
screen for a few seconds whether or not any panel is open.

A counter, a per-level readout, the current mode: `ui_status`. "The key
you just pressed registered", "your level finished loading", "the thing
you were waiting for happened": `notify`.

Two things about `notify` are easy to get wrong:

- **Write inside the game's glyph set.** Text renders with Spyro's own
  3D letter models: `0-9`, `A-Z` (lowercase folds up), space, and
  `% / ? + ^ . ' :`. Everything else renders as a space, so a hyphen,
  comma or exclamation mark silently disappears.
- **Pass a key for anything repeating.** A key coalesces: a second
  notification with the same key repaints the live toast instead of
  stacking a new one. A value the player is scrubbing through wants a
  key; a one-off event wants `NULL`.

It is also a no-op on runahead and rewind ticks, which are rolled back
or re-executed — so a mod that counts its own `notify` calls will see
fewer toasts than calls. That is the design, not a dropped message.

For the full contracts, see the [API reference](reference/api.md).

## Ordering across mods

Hard dependencies, declared as `deps = [...]`, topologically sort ahead
of enabled-list order, so a library's entry point always runs before the
entry points of the mods that depend on it. Everything else follows
`enabled.txt` order: later mods run first on shared overrides, and
`base()` descends toward earlier ones.
