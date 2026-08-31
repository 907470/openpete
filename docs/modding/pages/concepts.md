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
with an error. Presents are non-deterministic in count and timing,
because the engine extracts scenes ahead of their display instant and
can produce fewer than one per display slot, so anything that affected
the simulation would desync replays. Publish state from your tick hooks
into your own variables, then evaluate your own curves at `ctx->alpha`
for sub-tick smoothness.

Draw calls work from both contexts. `draw_text` and `draw_rect`
submitted during a tick persist for that tick's presents; submitted from
a present hook, they live for one present.

## Sim-neutrality and the oracle

The project's floor is gameplay that stays bit-identical to real
PlayStation hardware, proven by a replay gate
(`scripts/check_psx_oracle.sh`). Mods fit into that world in two ways:

- **Observers and presentation mods**, such as HUD, shader, sound
  overlay, and texture mods, are built on services that never write
  guest RAM. Declare `sim_neutral = true`, and openpete excludes your
  mod from the save modset tag. The sound overlay, the draw service, and
  the texture service are sim-neutral by construction, because the
  substitution happens on the host side.
- **Gameplay mods**, such as rule changes, spawns, and model swaps,
  exist to write guest RAM. The save-safety system tags memory cards
  with the modset, and warns before you mix a card that was played under
  a different one.

In `modtest.toml`, `expect_oracle = "pass"` proves that an observer
stays byte-neutral with the mod enabled, `"fail"` asserts that a
gameplay mod really does diverge, and `"skip"` covers everything in
between.

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

| Goal | Service | Sim-neutral? |
|---|---|---|
| Draw HUD text or rectangles | `draw_text`, `draw_rect` | Yes |
| Replace a game sound | `sfx_replace` | Yes for voice names, no for def-shaped names |
| Play your own WAV file | `sfx_play` | Yes |
| Put custom textures on your models | `assets/textures/own/<hexid>.png` | Yes |
| Add post-process shaders | `shader_register` with `postfx_*` | Yes |
| Re-shade some of what the engine draws | `[[material]]` rows, or `material_register` | Yes |
| Add custom levels | `[[level]]` with `assets/levels/` | No |
| Expose player settings | A `[[config]]` schema with `config_*` | Not applicable |
| Show status lines | `ui_status` | Yes |
| Add hotkeys | `[[binding]]` with `binding_down` | Divergent if read during ticks |

For the full contracts, see the [API reference](reference/api.md).

## Ordering across mods

Hard dependencies, declared as `deps = [...]`, topologically sort ahead
of enabled-list order, so a library's entry point always runs before the
entry points of the mods that depend on it. Everything else follows
`enabled.txt` order: later mods run first on shared overrides, and
`base()` descends toward earlier ones.
