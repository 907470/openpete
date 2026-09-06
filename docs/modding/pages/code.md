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

- Use the [typed structs](reference/structs.md). Their layouts are pinned
  with `_Static_assert`, so a layout change fails the mod's compile.
- Take addresses from the `OP_GADDR_*` constants in
  [Globals](reference/globals.md) and the `OP_FNADDR_*` constants in
  `openpete_sdk_symbols.h`.
- `op_read_u32(api, va)`, `op_write_u32(api, va, v)` and their siblings
  for every width and signedness, `op_read_{u,s}{8,16,32}` and
  `op_write_{u,s}{8,16,32}`, are inline helpers in the API header; each
  takes the API table as its first argument.

### The arena

`api->guest_alloc(self, size, align, flags, &host_view)` bump-allocates
guest-addressable bytes and returns a host view of the same block. The
bytes are timeline state: savestates, rewind, runahead, and process
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

## Toggling and reload

`register_toggle_hook` installs a callback that runs at the tick boundary
after the mod is enabled or disabled from the Mods section. The engine
freezes a disabled mod's registrations but never rewinds guest state, so a
mod that wrote guest bytes restores the stock bytes on `on = 0`.

## Services

| Goal | Service | Writes guest RAM |
|---|---|---|
| Draw HUD text | `draw_text` | No |
| Show status lines in the Mods section | `ui_status` | No |
| Draw a panel of your own | `openpete_mod_ui.h`, or `openpete_imgui.h` for the whole Dear ImGui API | No |
| Announce an event on screen | `notify` | No |
| Replace a game sound from code | `sfx_replace` | No |
| Play the mod's own WAV | `sfx_play` | No |
| Read player settings | `[[config]]` rows with `config_*` | No |
| Read a hotkey | `[[binding]]` rows with `binding_down` | No; acting on it in a tick hook diverges gameplay from a replay |
| Post-process the frame | `shader_register` with `postfx_*` | No |
| Re-shade what the engine draws | `[[material]]` rows or `material_register` | No |
| Read another level's data | `read_level_data` | No |
| Re-target texture-pack fog | `texpack_fog_shift` | No |
| Persistent files | `data_dir` | No |

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
— windows, tables, plots, tree nodes, tabs, popups, colour editors, draw
lists, fonts — `openpete_imgui.h` hands a mod the whole Dear ImGui C API:
see [Dear ImGui annex](reference/imgui.md).

**Always-on sections.** `OPENPETE_MOD_UI_SECTION_FLAGS(fn,
OPENPETE_MOD_UI_ALWAYS)` registers a body that runs on every present
instead, whatever the overlay is doing — a stats readout, a draw-list
overlay. It runs at top level, not inside the mod's node, so it owns its
windows: every widget it emits outside an `ImGui_Begin` lands in Dear
ImGui's default debug window. With the overlay closed its windows take no
mouse and no keyboard, so the game keeps both; `ui->overlay_open()` says
which state the present is in. `ui_status` lines still appear in the mod's
node. The pass is skipped while the in-game settings menu is open.

An always window is chrome, not content: it is never recorded and never
appears in screenshots or A/B composes. `draw_text` is the recorded-HUD
path.

Full contracts: [API reference](reference/api.md).
