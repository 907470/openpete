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
  (a parked model payload). The engine checksums it at each rewind
  keyframe and file savestate and logs a changed block once, with the
  mod's name and call number.

On reload the allocation sequence is replayed: the i-th call with the
same size, alignment, and flags returns the same address and view with
its bytes intact. A call of another shape takes a fresh block; the old
block stays allocated. A savestate whose allocation ledger does not
prefix-match the live session (a config edit changed a size) is refused
with the mod and entry named.

#### Two pools

The arena is two pools above the 2 MB of RAM. The `flags`
argument picks the pool; the address is never chosen by the mod.

| Pool | Guest addresses | Capacity | Largest block | Taken by |
|---|---|---|---|---|
| [DMA-reachable](glossary.md) | 0x80200000 to 0x80FFFFFF | 14 MiB | 9 MiB | every call without `OP_GALLOC_NO_DMA` |
| [general](glossary.md) | 0x81000000 to 0x9EFFFFFF | 480 MiB | 480 MiB | calls with `OP_GALLOC_NO_DMA` (api 13) |

The PSX GPU follows packet and ordering-table links through 24-bit
addresses, so a block the GPU or a libgpu call links into a chain must
sit below physical 16 MiB. The DMA-reachable pool is every byte below
that bound that RAM does not use. It has two segments: 9 MiB at
0x80700000, filled first, then 5 MiB at 0x80200000. A block goes to the
first segment with room for it at that segment's cursor, so a block
larger than 5 MiB fits only in the 9 MiB segment.

`OP_GALLOC_NO_DMA` states that no address inside the block is truncated
to 24 bits: no ordering-table link, no libgpu call that links or walks
it, no pointer to it in a 24-bit field. Pass it for everything you do
not link: lookup tables, recordings, decoded assets, per-session state.
Leave it off for GPU packets, primitives, ordering tables, and any block
whose address the game stores in a narrow field. The flag combines with
`OP_GALLOC_IMMUTABLE`. A mod built against api 12 or earlier never
passes it and gets the DMA-reachable pool: a call sequence that fit the
single 8 MiB arena of api 12 gets the same addresses.

#### Errors

`guest_alloc` returns 0 and sets `*host_view` to NULL when:

- `size` is 0, or above the pool's largest block (silent);
- `align` is not 0 or a power of two (silent; 0 means 8);
- `flags` holds a bit other than the two above (logged);
- the call comes from a present hook (logged);
- no segment of the pool has room. The log line names the caller, the
  request and each segment's use, then lists every mod's bytes in that
  pool:

```text
guest arena exhausted: my-mod wanted 1048576 B (align 16) in the DMA-reachable pool, upper 9437184/9437184 used, lower 5000000/5242880 used
guest arena exhausted: DMA-reachable pool held by other-mod: 9437184 B
guest arena exhausted: DMA-reachable pool held by third-mod: 5000000 B
```

A full DMA-reachable pool does not spill into the general pool: move the
blocks you do not link to `OP_GALLOC_NO_DMA`.

#### Where use is shown

- The log holds one line per mod per session, after its entry returns
  (or after its first block, when that comes later):
  `guest arena: my-mod holds 65536 B DMA-reachable (0 B IMMUTABLE), 0 B general (0 B IMMUTABLE)`.
- The Mods section of the dev overlay shows
  **Mod memory: X of 14 MiB DMA-reachable, Y MiB general** for all mods,
  and **Runahead snapshot: Z ms per tick** once runahead has run.
- Hovering a mod's row shows its own
  **Guest memory: ... KiB DMA-reachable, ... KiB general (... KiB immutable)**.

#### Snapshot cost

Each pool contributes its used range: from its base to the end of its
last block, alignment padding included. The two pools cost the same per
byte, and `OP_GALLOC_IMMUTABLE` blocks are copied like the rest.

| Feature | Cost of the arena |
|---|---|
| File savestate | The used ranges and the allocation ledger, written to the file. |
| Runahead | The used ranges copied out and back once per tick. |
| Rewind | Each keyframe (one per second) copies the used ranges, then over the following ticks shares every 4 KiB page equal to the previous keyframe's. The window keeps the arena once plus the 4 KiB pages that change each second. |

Bytes a mod rewrites every tick cost rewind their pages per second of
window; bytes it writes once cost them once. A restore puts back the
bytes the snapshot held; a block allocated after the snapshot keeps its
contents, and no allocation moves.

Savestates written by engines with the single 8 MiB arena (api 12 and
earlier) still load: their blocks restore into the 9 MiB segment at the
addresses they had.

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
| `game_disc_path`, `game_installed` | yes | yes |
| `shader_register`, `postfx_*` | yes | yes |
| `sfx_replace`, `sfx_play` | yes | yes; slot names per game ([Sounds](sounds.md)) |
| `draw_text` | yes | yes |
| `[[material]]` rows | every render channel | every render channel |
| `material_register`, `material_register_refine`, `material_enable`, `material_set_params`, `material_set_selector` | yes | yes |
| `moby_classes` | yes | yes |
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
| Read a sector of a game's disc | `read_sector` (2352 raw bytes by LBA; the booted disc through the engine's disc cache, another game's from its library image) | No |
| Find another game's disc image | `game_disc_path` | No |
| Ask whether a game is installed | `game_installed` | No |

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

### Other games' discs

A mod reads the disc of another Spyro game, such as Spyro 2's models in a
Spyro 1 mod, by naming that game in its manifest
([`requires_games` or `optional_games`](manifest.md#modtoml)) and passing
its id to the disc calls. The ids are strings; the SDK names them:

| Constant | Value |
|---|---|
| `OPENPETE_MOD_GAME_SPYRO1` | `"spyro-1"` |
| `OPENPETE_MOD_GAME_SPYRO2` | `"spyro-2"` |
| `OPENPETE_MOD_GAME_SPYRO3` | `"spyro-3"` |

`api->game_id(self)` returns the running game's id, and a raw string is
accepted in place of a constant.

``` c
uint8_t sec[2352];
if (api->game_installed(self, OPENPETE_MOD_GAME_SPYRO2) &&
    api->read_sector(self, OPENPETE_MOD_GAME_SPYRO2, 16, sec) == 0) {
    /* sec + 24 holds the primary volume descriptor of Spyro 2's disc */
}
api->read_sector(self, api->game_id(self), 16, sec);   /* the booted disc */
```

| Call | Running game | Listed game, installed | Listed game, not installed | Game in neither list |
|---|---|---|---|---|
| `read_sector(self, game, lba, buf)` | the booted disc, through the disc cache | the library's disc, read from its image file | -1 | -1, one log line per mod |
| `game_disc_path(self, game, want_bin)` | as `disc_path` | the library entry's `.cue`, or its `.bin` | NULL | NULL, one log line per mod |
| `game_installed(self, game)` | 1 | 1 | 0 | 1 or 0, never refused |

A game is installed when the player's library holds a disc of it, any
release; the library is read once, when mods load. A read of another
game's disc goes to that disc's image file alone: it never touches the
booted disc's cache or the order in which the cache loads. No call
writes a disc. All three calls are legal from any context. A mod refused at load for a missing `requires_games` game never
runs, so its calls need no check for that game; an `optional_games` game
needs `game_installed` or the -1.

Full contracts: [API reference](reference/api.md).
