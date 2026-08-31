# Concepts

## The override chain

Every recompiled guest function's prologue passes through one dispatch
chokepoint, so a per-address chain intercepts **every** call — direct,
indirect, or dispatched — with zero code patching. Pre-hook, post-hook
and full replacement are one mechanism, distinguished by where you call
`api->base(cpu)`:

``` c
static void my_override(CPUState* cpu) {
    /* code here runs BEFORE the original        (pre-hook)  */
    api->base(cpu);
    /* code here runs AFTER the original         (post-hook) */
    /* never call base() at all: full replacement            */
}
```

`base()` may be called 0, 1, or N times; each call runs the
continuation once. The enabled-list order is the priority: later mods
run first, `base()` walks toward earlier mods, then the recompiled
original.

There is no separate event system — **events are post-hooks plus a
state diff** around `base()`. Want "gem collected"? Post-hook the
collector and compare the counter before/after.

## Tick vs present

openpete simulates at the PSX's canonical 29.913 ticks/s but can present
many frames per tick (high-fps interpolation). That split is the most
important contract in the API:

- **Tick context** (overrides, entry, registry update fns): the sim is
  live. Guest reads/writes, `call`, spawning, registration — all
  legal here, and only here.
- **Present context** (`register_present_hook`): render-pure by
  contract *and enforcement*. Guest access, game calls and
  registrations are refused with an error — presents are
  non-deterministic in count and timing (the engine extracts scenes
  ahead of their display instant and may produce fewer than one per
  display slot), so anything sim-affecting would desync replays. Publish state from tick hooks into your own
  variables; evaluate your own curves at `ctx->alpha` for sub-tick
  smoothness.

Draw calls (`draw_text`, `draw_rect`, `draw_actor`) work from both
contexts: submitted in a tick they persist for that tick's presents;
submitted in a present hook they live one present.

## Sim-neutrality and the oracle

The project's floor is bit-identical gameplay with real PSX hardware,
proven by a replay gate (`scripts/check_psx_oracle.sh`). Mods slot into
that world in two ways:

- **Observers / presentation mods** (HUD, ghosts, SFX overlay,
  textures): built on services that never write guest RAM — declare
  `sim_neutral = true` and your mod is excluded from the save modset
  tag. The SFX overlay, draw service and texture service are
  *sim-neutral by construction*: the substitution happens host-side.
- **Gameplay mods** (new mobys, collision, model swaps): guest writes
  are the point. The save-safety system tags memory cards with the
  modset; a card played under a different modset warns before mixing.

The `[test]` block's `expect_oracle = "pass"` proves an observer stays
byte-neutral *with the mod enabled*; `"fail"` asserts a gameplay mod
really diverges; `"skip"` for everything in between.

## Guest memory

Guest RAM is a 2 MB little-endian mirror. `api->guest(vaddr)` returns a
host pointer (kuseg/kseg0/kseg1 all accepted); `api->guest_addr` goes
the other way. Three rules:

1. Pointers stored *inside* guest structs are guest vaddrs —
   re-translate before chasing.
2. Use the [typed structs](reference/structs.md) where they exist:
   layouts are `_Static_assert`-pinned, so drift fails **your** compile,
   never a player's session.
3. Named addresses come from `OP_GADDR_*` ([globals](reference/globals.md))
   and `OP_FNADDR_*` constants — never hardcode a number you can name.

Need guest-addressable scratch? `api->guest_alloc` bump-allocates in
the >2 MB expansion window with a host view. (One caveat inherited from
the moby format: animated-model vertex streams use 21-bit offsets, so
payloads for those must live in the low 2 MB — coordinate a fixed park
below the EXE base at `0x80010000`.)

## Services map

| Need | Service | Neutral? |
|---|---|---|
| Draw HUD text/rects | `draw_text` / `draw_rect` | yes |
| Replace a game sound | `sfx_replace` | voice names yes; def-shaped names no |
| Play your own WAV | `sfx_play` | yes |
| Custom textures on your models | `assets/textures/own/<hexid>.png` | yes |
| Post-process shaders | `shader_register` + `postfx_*` | yes |
| Custom levels | `[[level]]` + `assets/levels/` | no |
| User settings | `[[config]]` schema + `config_*` | n/a |
| Status lines | `ui_status` | yes |
| Hotkeys | `[[binding]]` + `binding_down` | divergent if read in ticks |

Full contracts: [API reference](reference/api.md).

## Ordering across mods

Hard dependencies (`deps = [...]`) topo-sort ahead of the enabled-list
order, so a library's entry always runs before its dependents'. For
everything else, `enabled.txt` order IS the priority — later mods run
first on shared overrides and `base()` descends toward earlier ones.
