# Code mods

A code mod exports one function:

``` c
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self);
```

Return 0 to load. The mod reaches the engine only through `api`; it never
links against engine symbols. `api->api_version` is the engine's
`OPENPETE_MOD_API_VERSION`; compare it with `<`, never `!=`.

## The override chain

Every recompiled guest function dispatches through one chokepoint, so a
per-address chain intercepts every call to it, direct or indirect,
without patching code. A pre-hook, a post-hook, and a full replacement are
one mechanism, distinguished by where the override calls `api->base(cpu)`:

``` c
static void my_override(CPUState* cpu) {
    /* runs before the original: pre-hook */
    api->base(cpu);
    /* runs after the original: post-hook */
    /* omit base() entirely: full replacement */
}
```

`base()` runs the next override in the chain and bottoms out at the
recompiled original. It may be called any number of times; each call runs
the continuation once. Chain order follows the enabled list: the mod
listed later runs first.

There is no event system. Detect an event by diffing guest state around
`base()`: to detect a gem pickup, post-hook the collector and compare the
counter before and after.

`api->call(cpu, addr)` calls a guest function through the same dispatch
layer, so the callee's own override chain runs. Write `cpu->a0..a3`
before, read `cpu->v0` after. The register fields are declared in
`psx_runtime.h` (in `sdk/`); include it alongside the API header.
The register conventions an override relies on are in the
[CPUState reference](reference/cpustate.md).

## Tick context and present context

The game simulates at 29.913 ticks per second. The engine may present
many frames per tick, extracting scenes ahead of their display instant,
and the number of presents per tick depends on the player's machine.

**Tick context**: the entry point, overrides, toggle hooks, and refine
callbacks. Guest reads and writes, `call`, `guest_alloc`, and every
registration are legal here and nowhere else.

**Present context**: hooks installed with `register_present_hook`. Guest
access, `call`, `base`, and registration are refused with an error, since
anything done to the game from a present would happen a different number
of times on different machines. `ctx->alpha` is the sub-tick phase of the
scene being drawn, `ctx->dt` the spacing to the previous scene. Publish
state from tick hooks into your own variables and evaluate curves at
`ctx->alpha` for sub-tick smoothness; the engine never interpolates mod
visuals.

`draw_text` works from both contexts. A call from tick context shows for
all of that tick's presents; a call from a present hook shows for that
present.

## Guest memory

A mod works in two address spaces. The game's code and data use guest
addresses: 32-bit values that on the console named a byte of its 2 MB RAM.
Your C code uses host pointers. openpete keeps the guest RAM as a byte
array in the game process, so every guest address corresponds to a host
pointer, and the API converts between the two:

- `api->guest(vaddr)` takes a guest address and returns the host pointer
  to the same byte. It returns NULL for an address that maps nothing,
  including guest address 0.
- `api->guest_addr(ptr)` takes a host pointer obtained from `api->guest`,
  `guest_alloc`, or arithmetic on either, and returns its guest address.
  It returns 0 for a pointer outside guest memory.

Three guest address forms name the same byte: 0x00xxxxxx, 0x80xxxxxx and
0xA0xxxxxx. The game's own pointers use the 0x80 form. `api->guest`
accepts all three.

Guest memory is little-endian on every supported host, so reading a
`uint32_t` through the host pointer gives the value the game sees.

A pointer field inside a guest struct holds a guest address, not a host
pointer. Dereferencing it in C reads the wrong memory. Pass it through
`api->guest` first. The reverse holds when writing: a guest struct field
that the game will follow must receive a guest address from
`api->guest_addr`, never a raw host pointer.

Host pointers are valid for the current process only. Store guest
addresses in anything that persists: the arena, savestates, or a struct
the game reads.

Guest memory covers the 2 MB RAM, the 1 KB scratchpad at 0x1F800000, and
every block returned by `guest_alloc` (see The arena below). All three
resolve through `api->guest`.

- Use the [typed structs](reference/spyro-1/structs.md). Their layouts are pinned
  with `_Static_assert`, so a layout change fails the mod's compile.
- Take addresses from the `OP_GADDR_*` constants in
  [Globals](reference/spyro-1/globals.md) and the `OP_FNADDR_*` constants in
  `openpete_sdk_symbols.h`.
- `op_read_u32(api, va)`, `op_write_u32(api, va, v)` and their siblings
  for every width and signedness, `op_read_{u,s}{8,16,32}` and
  `op_write_{u,s}{8,16,32}`, are inline helpers in the API header; each
  takes the API table as its first argument.

### The arena

`api->guest_alloc(self, size, align, flags, &host_view)` bump-allocates
guest-addressable bytes and returns a host view of the same block. The
bytes are [timeline state](glossary.md): savestates, rewind, runahead, and process
handoff carry and restore them like guest RAM. Mutable per-session state
(a cooldown, a spawned-set bitmap, a counter) belongs here. Host statics
do not roll back; keep them for caches, file handles, and UI drafts.

- There is no free. Allocate at entry and reuse; allocating per level or
  per toggle grows every future snapshot.
- Write through the host view from tick context only.
- Store guest addresses inside the block, never host pointers: the bytes
  cross a process boundary at handoff.
- `OP_GALLOC_IMMUTABLE` declares a block written once and never again
  (a parked model payload). The engine checksums it at each snapshot and
  logs a changed block with the mod's name.

On reload the allocation sequence is replayed: the i-th call with the
same size, alignment, and flags returns the same address and view with
its bytes intact. A savestate whose allocation ledger does not
prefix-match the live session (a config edit changed a size) is refused
with the mod and entry named.

**Vertex streams.** An animation frame word references its vertex stream
through a 21-bit field that cannot hold an arena address. Request a
handle with `api->vstream_handle(self, stream_vaddr)` and write the handle
into the field; the game's readers resolve it. Request handles at entry,
in a fixed order, once per stream: they are ledgered like allocations.
Spyro 1 only: Spyro 2 and 3 frame words hold a halfword offset from the
animation header, not a stream field, so a mod moves a stream there
through the header's pointers. A mod built for Spyro 2 or 3 does not
compile a use of `vstream_handle`; the error states the reason. A `"*"`
object gets 0 and one log line.

### Spyro 3's checked code

Spyro: Year of the Dragon hashes its own code while it runs: the EXE's
code, 0x8001200C up to 0x80064F9C, and the code of the resident overlay.
A hash over a changed byte fails, and the game then damages save and level
state. A store a mod makes into those ranges is reverted and logged with
the mod's id, unless the player runs with `--scan-guard 0`, under which it
persists. The check's cost counts in an override's reported time.

The game's code bytes are not what runs: openpete runs the recompiled
functions, so a store into code changes no behaviour. Change behaviour with
an [override](#the-override-chain) instead.

A store through a kept host pointer from a present hook, a UI body or a
thread the mod starts is not reverted.

## Toggling and reload

`register_toggle_hook` installs a callback that runs at the tick boundary
after the mod is enabled or disabled from the Mods section. The engine
freezes a disabled mod's registrations but never rewinds guest state, so a
mod that wrote guest bytes restores the stock bytes on `on = 0`.

## One mod for several games

A mod with `src/` compiles once for each game its manifest selects
([`game`](manifest.md)), against that game's SDK: the compile line puts
`-I <sdk>/games/<game>` (the game's `openpete_sdk_symbols.h`,
`openpete_sdk_wrappers.h`, `openpete_game_structs.h` and
`openpete_game.h`) before `-I <sdk>` (the game-neutral headers), and the
object lands in `.build/<game>/`. `openpete_game.h` defines
`OPENPETE_GAME_ID` (`"spyro-1"`, ...) and all three of
`OPENPETE_GAME_SPYRO1`, `OPENPETE_GAME_SPYRO2` and
`OPENPETE_GAME_SPYRO3`, the compiled game's as 1 and the others as 0.
Function names, addresses and struct layouts differ per game, so a mod
selects them with one arm per game and closes the chain with
`#else #error`, so that a game with no arm fails the compile instead of
building a mod that registers nothing:

``` c
#if OPENPETE_GAME_SPYRO1
    rc = api->override_name(self, "CameraUpdate", on_clock);
#elif OPENPETE_GAME_SPYRO2
    rc = api->override_addr(self, 0x8001D87Cu, on_clock);
#elif OPENPETE_GAME_SPYRO3
    rc = api->override_addr(self, 0x8001204Cu, on_clock);
#else
#error "no arm for this game"
#endif
```

The compile line has `-Wundef`, so a misspelt game macro is a warning
rather than a silent 0. The `game-arms` example is this pattern across
three overrides.

**Game-neutral services** (api 13): `game_id` names the running game,
`gameplay_live` reports whether a level is being played, `player_pose`
fills the player's position, facing and level, and `level_id` returns the
level. Called from a tick hook with its `cpu` they read live; from a
present hook or a UI section, with `cpu` NULL, they read the record the
engine sampled at the last tick boundary.

**`"*"` objects.** A source that includes `openpete_mod_api.h` alone and
names no game function, address or struct can be built once as an object
that loads on every game: compile with `-DOPENPETE_MOD_ANY_GAME` and
`-I <sdk>` only, and ship it as `.build/any/<id>.so` or `.dll` (see
[Publishing](publishing.md#build-by-hand)). Under that define every
per-game header stops the compile with `#error`. Limits:

- No compile-on-boot. The engine compiles a source mod per game with the
  per-game include path, where `openpete_game.h` stops an
  `OPENPETE_MOD_ANY_GAME` compile; a `"*"` object is built by hand and
  shipped.
- No tick hook. The api has no game-neutral tick hook: tick context comes
  from overrides, whose targets differ per game. A `"*"` mod works from
  present hooks and the sampled game services; a mod that needs tick
  context uses per-game arms.
- `override_addr` and every `func_`-spelled `override_name` from a `"*"`
  object resolve through the running game's addresses, a different
  function on another game; each such call logs a warning.

The `pose-report` example builds both ways: per game by compile-on-boot,
or as a `"*"` object by hand.

### Per-game support

Every api entry exists on every game. An entry whose game surface is
missing on a game returns the value the table names.
`OPENPETE_MOD_ERR_UNSUPPORTED` (-2, in `openpete_mod_api.h`) is the return
of an entry whose surface the running game does not serve; the entry
logs one line naming the game the first time each mod calls it. Every
other failure returns -1, so `rc < 0` tests for any failure and
`rc == OPENPETE_MOD_ERR_UNSUPPORTED` for the missing surface. A material
on a render channel the running game does not serve is refused the same
way on both paths: a `[[material]]` row is logged and skipped, and
`material_register` or `material_register_refine` logs one line and
returns -1.

| Entry | Spyro 1 | Spyro 2 and Spyro 3 |
|---|---|---|
| `override_name`, `override_ovl`, `base`, `call` | yes | yes |
| `override_addr` | yes | main-executable addresses; an address inside the overlay window is refused, use `override_ovl` |
| `guest`, `guest_addr`, `guest_alloc`, `log`, `data_dir`, `register_toggle_hook`, `config_*`, `ui_status`, `register_present_hook`, `binding_down`, `notify`, `mouse_*`, `pad_*`, `disc_path`, `read_sector` | yes | yes |
| `game_id`, `gameplay_live`, `player_pose`, `level_id` | yes | yes |
| `shader_register`, `postfx_*` | yes | yes |
| `sfx_replace`, `sfx_play` | yes | yes; slot names per game ([Sounds](sounds.md)) |
| `draw_text` | yes | yes |
| `[[material]]` rows | every render channel | every render channel |
| `material_register`, `material_register_refine`, `material_enable`, `material_set_params`, `material_set_selector` | yes | yes |
| `moby_classes` | yes | Spyro 3: yes. Spyro 2: no moby census, returns `OPENPETE_MOD_ERR_UNSUPPORTED` |
| `read_level_data` | yes | returns `OPENPETE_MOD_ERR_UNSUPPORTED` |
| `[[level]]` rows | yes | refused with one log line |
| `vstream_handle` | yes | no vertex-stream field: a per-game build is refused at compile time; a `"*"` object gets 0 and one log line |
| `texpack_fog_shift` | yes | applies to the terrain the native renderer draws |
| music replacement ([Music](music.md)) | yes | no |

## Services

| Goal | Service | Writes guest RAM |
|---|---|---|
| Draw HUD text | `draw_text` | No |
| Show status lines in the Mods section | `ui_status` | No |
| Draw a panel of your own | `openpete_mod_ui.h`, or `openpete_imgui.h` for the whole ImGui API | No |
| Announce an event on screen | `notify` | No |
| Replace a game sound from code | `sfx_replace` | No |
| Play the mod's own WAV | `sfx_play` | No |
| Read player settings | `[[config]]` rows with `config_*` | No |
| Read a hotkey | `[[binding]]` rows with `binding_down` | No; acting on it in a tick hook diverges gameplay from a replay |
| Read the mouse | `mouse_read` (position, buttons, per-hook-kind deltas), `mouse_capture` for relative mode | No; same replay caveat in a tick hook |
| Read another player's pad | `pad_read` | No; the record is the tick's own, so a tick-hook read replays |
| Drive another player's rumble | `pad_rumble` (libpad actuator bytes; slot 0 refused) | No; inert while speculating or replaying |
| Post-process the frame | `shader_register` with `postfx_*` | No |
| Re-shade what the engine draws | `[[material]]` rows or `material_register` | No |
| Read another level's data | `read_level_data` | No |
| Re-target texture-pack fog | `texpack_fog_shift` | No |
| Persistent files | `data_dir` | No |
| Find the booted disc image | `disc_path` (the path the engine opened, in any layout) | No |
| Read a sector of the booted disc | `read_sector` (2352 raw bytes by LBA, through the engine's disc cache) | No |

**`ui_status` or `notify`.** `ui_status` reports state: lines in the mod's
block of the Mods section, visible while that overlay is open, repainted
whenever the mod prints again. `notify` reports an event: a toast in the
game's gold chrome, shown for a few seconds regardless of any overlay.
`notify` renders with the game's glyph set (`0-9`, `A-Z`, space, and
`% / ? + ^ . ' :`; other characters render as a space), coalesces toasts
that share a key, is rate-limited per mod, and is a no-op on runahead
and rewind ticks.

**Panels.** `openpete_mod_ui.h` mirrors eleven widgets (text, button,
checkbox, sliders, combo, input text, separator, same-line, tooltip) over
the engine's overlay; register a body with `OPENPETE_MOD_UI_SECTION(fn)` at
file scope in one source file and it draws inside the mod's node of the
Mods section, while that overlay is open. For anything beyond those eleven
- windows, tables, plots, tree nodes, tabs, popups, colour editors, draw
lists, fonts - `openpete_imgui.h` hands a mod the whole ImGui C API:
see [ImGui](imgui.md).

**Always-on sections.** `OPENPETE_MOD_UI_SECTION_FLAGS(fn,
OPENPETE_MOD_UI_ALWAYS)` registers a body that runs on every present
instead, whatever the overlay is doing - a stats readout, a draw-list
overlay. It runs at top level, not inside the mod's node, so it owns its
windows: every widget it emits outside an `ImGui_Begin` lands in
ImGui's default debug window. With the overlay closed its windows take no
mouse and no keyboard, so the game keeps both; `ui->overlay_open()` says
which state the present is in. `ui_status` lines still appear in the mod's
node. The pass is skipped while the in-game settings menu is open.

Full contracts: [API reference](reference/api.md).
