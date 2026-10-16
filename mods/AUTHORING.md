# openpete mod authoring

Self-contained reference for writing openpete (Spyro the Dragon, SCUS-94228)
mods against the shipped SDK. Assumes no access to engine source. Paths are
relative to `<exe>`, the directory holding `openpete` /
`openpete.exe`. Authoritative sources when this file and the SDK
disagree: `sdk/openpete_mod_api.h` (contract), the Modding section of
`docs/index.html` (generated reference, offline), `sdk/examples/<id>/`
(working mods).

Game internals beyond the SDK (what a function does, struct fields the
SDK does not type, `func_XXXXXXXX` bodies, data formats) come from the
matching decompilation: `git clone https://github.com/TheMobyCollective/spyro-1`.
`src/` and `include/` are the C; `asm/` holds unmatched functions and
data; `src/overlays/level_NN/` is per-level moby code. The SDK's symbol
names and addresses are generated from it, so a name in
`sdk/games/spyro-1/openpete_sdk_symbols.h` is greppable there verbatim.

## 1. Model

- The game is the original PSX binary recompiled to C. Guest RAM (2 MB at
  0x80000000, 1 KB scratchpad at 0x1F800000) is a byte array in the process.
  Gameplay state is bit-identical to hardware unless a mod writes guest RAM.
- Sim runs at 29.913 ticks/s (NTSC 1125000/37609). VBL = 59.826 Hz. The CLI
  word "frame" means VBL. Presents per tick depend on the machine.
- Every recompiled function dispatches through one chokepoint, so a per-
  function override chain intercepts every call, direct or indirect.
- No sandbox, no signing: mod C runs with the game's privileges.
- Two tiers, freely mixed: declarative (manifest + files, no ABI) and code
  (`src/*.c` compiled on the player's machine at boot against `sdk/`).
- A mod must not ship disc-derived bytes (models, textures, sounds, WADs,
  overlays, patches whose plaintext is game data). Ship a generator that
  writes them into `assets/` from the player's own disc.

## 2. Layout and enabling

`mods/` location: Windows next to the exe; Linux tar/AppImage
`~/.local/share/openpete/mods/` (`$XDG_DATA_HOME/openpete/mods/`), or next to
the exe if that dir exists and is writable, or `portable = true` in
`openpete.toml`. `--mods-dir DIR` / `OPENPETE_MODS_DIR` overrides all.

| Path | Role |
|---|---|
| `mod.toml` | Manifest, required |
| `config.toml` | Player settings, read via `api->config_*`; written by the Mods panel Apply |
| `src/*.c` | Compiled on load. No `src/` = never loads code |
| `shaders/*.vert,*.frag` | GLSL, compiled on load; the hot-reload watch dir |
| `assets/{textures,music,sfx,wad,levels}/` | Overlay roots, matched by filename |
| `data/` | Per-mod persistent dir, created on first `api->data_dir()` |
| `.build/` | Engine compile cache: `<game>/` holds `<id>.so|.dll`, `compile.log` and the `.hash` stamp per game; `shaders/<name>.log`. Not shipped |

`<id>.zip` with `mod.toml` at root (or under one wrapping folder) loads
identically; unpacked to `mods/.unpacked/<id>/`, re-unpacked only when the
zip hash changes. Folder beats zip with the same id.

`mods/enabled.txt`: one id per line, `#` comments. Missing file = every mod,
lexicographic. ORDER IS PRIORITY: later line = runs first on a shared
override, wins asset/material/shader collisions; `base()` descends toward
earlier mods, then the original. `--mods a,b` / `OPENPETE_MODS=a,b` replaces
the list for one run; `--mods ""` = vanilla. `deps` topo-sort ahead of list
order (library entry runs before clients). M-key overlay Mods section toggles
a mod at the next tick boundary and edits `[[config]]` rows; it cannot
reorder.

Compiler: Windows archive and Linux AppImage bundle one; Linux tar uses
`cc` on PATH or `OPENPETE_CC`. `OPENPETE_MOD_CC` overrides first. Flags:
`-shared -fPIC -O2 -g -fno-strict-aliasing -Wall -Wundef
-I sdk/games/<game>/ -I sdk/ src/*.c sdk/openpete_mod_abi.c`, output
`.build/<game>/<id>.so|.dll`; Windows adds `-Wl,--export-all-symbols`, so
a `dllexport` in a source does not drop the unmarked entry point and
descriptors from the DLL. `sdk/games/<game>/` holds the game's
symbols, wrappers, structs and `openpete_game.h` (`OPENPETE_GAME_ID`,
`OPENPETE_GAME_SPYRO1/2/3` as 1 or 0). Compile failure disables the mod,
writes `.build/<game>/compile.log`, game still boots. Non-zero entry return
disables the mod and sweeps its registrations. A mod with no `src/` and a
prebuilt object loads as-is; it must export the ABI descriptor
(`openpete_mod_abi.c` compiled in, or `OPENPETE_MOD_ABI_DEFINE()` in one
TU); the loader refuses no descriptor, newer api, larger struct, or a
descriptor from before the api-11 layout freeze. The descriptor records
the game (`openpete_mod_game`): prebuilt lookup is `.build/<game>/` (must
record the running game), `.build/any/` (must record `"*"`, built with
`-DOPENPETE_MOD_ANY_GAME`, which makes every per-game header `#error`),
then legacy `.build/<id>.so|.dll` (running game or `"*"`; no game loads on
Spyro 1 only).

## 3. mod.toml

```toml
[mod]
id      = "my-mod"      # required; = directory/zip name
version = "0.1.0"       # required
name    = "My Mod"      # Mods panel label, default id
game    = "spyro-1"     # or a list; omitted = every title at api >= 13,
                        # spyro-1 alone below 13 or unpinned. Exact match, so
                        # an unknown id loads nowhere. Filtered at discovery:
                        # a mod for another title never compiles here
api     = 13            # minimum OPENPETE_MOD_API_VERSION; omit with no src/.
                        # >= 13 refuses window override_addr and ambiguous raw
                        # override_name (use override_ovl); below 13 they bind
                        # every overlay on Spyro 1, refused on Spyro 2 and 3
deps    = ["lib-mod"]   # hard deps; cycle or missing dep disables this mod
sim_mutative = false    # writes guest RAM? see §7
state    = "none"       # "none" | "rebuildable"; undeclared = rewind refused
runahead = "pure"       # "pure" | "host-fx"; undeclared = runahead disabled

[[binding]]             # api->binding_down(self, "warp")
name = "warp"; key = "F6"; purpose = "warp home"
# player rebinds under [keys.mod.<id>] in openpete.toml: key names,
# "pad:north" etc., or "none"; conflicts across the enabled set logged at load

[[config]]              # engine renders them in the Mods panel
key = "spawn.scale"     # config.toml dotted path: [spawn] scale
type = "float"          # bool | int | float | string
default = 1.0
min = 0.25; max = 4.0   # min<max on int/float = slider, else input box
label = "Scale"; group = "Spawn"; help = "tooltip"
# type="string" + items=["a","b"] = combo

[[material]]            # see §5
frag = "shaders/tint.frag"; channel = "player"; params = [1.0, 0.5, 0.1, 0.4]

[[level]]               # see §6
name = "Grim Garden"; files = "grim_garden"; donor = "dark_hollow"
```

`api = N`: the version that introduced the last entry the mod calls. Loader
refuses a pin above the engine before compiling. Struct is append-only from
api 11; guard late entries with `if (api->api_version >= N)`, never `!=`.

config.toml is parsed once before entry runs. Read config in entry. Panel
Apply rewrites the file (comments preserved) and reloads the mod at the next
tick boundary; entry re-runs. Coercion: bool->0/1 via int/float; int via
float; float truncates via int; string never coerces. Missing file/key/type
mismatch returns the passed default; parse error logs and returns defaults.

## 4. Code mods

```c
#include <openpete_mod_api.h>       // api table, present ctx, selectors, op_read_*/op_write_*
#include <openpete_sdk_symbols.h>   // OP_GADDR_<global>, OP_FNADDR_<func> (815 globals, 1176 funcs)
#include <openpete_sdk_wrappers.h>  // typed override/base/call for 198 prototyped funcs
#include <openpete_game_structs.h>  // Moby, Vector3D, SHORTMATRIX, AnimationState; _Static_assert-pinned
#include <psx_runtime.h>            // CPUState fields (api header only forward-declares it)

static const openpete_mod_api_t* g_api; static openpete_mod_t* g_self;
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;                 // return 0 = loaded
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
```

### Override chain

One mechanism, three shapes, by where `api->base(cpu)` sits: after body =
pre-hook, before body = post-hook, absent = replacement. `base()` runs the
next override then the original; callable any number of times; valid only
while this mod's override is on the stack. No event system: diff guest state
around `base()` (gem pickup = post-hook `CollectItem`, compare
`g_GemTotal`; level ready = `g_LevelId` changed while `g_Gamestate == 0`
in a `GamestateUpdate` post-hook).

Three registration tiers:
1. `api->override_name(self, "Name", fn)`: names from
   the Modding reference's Functions page; `func_XXXXXXXX` names are unnamed decomp
   functions, resolve forever, loader warns once a stable name exists.
2. Typed wrappers (prototyped functions only):
   `OP_OVERRIDE_IMPL_CollectItem(my_fn) { ...pMoby... }` declares the body
   with unpacked typed args (guest pointers marshalled to host `Moby*`);
   `OP_REGISTER_OVERRIDE_CollectItem(api, self, my_fn)`;
   `op_base_CollectItem(cpu, pMoby)`; `op_call_Sin(cpu, 1024)` saves and
   restores a0-a3/v0/v1/ra around the nested call. A wrapper body returning
   a value writes `v0` itself. Aliases `OP_..._func_8003B9D4` exist.
3. `api->override_addr(self, 0x8003B9D4, fn)`: raw tier; args
   `cpu->a0..a3`, 5th+ at `cpu->read_word(cpu->sp + 16 + 4*i)`, result
   `cpu->v0` (`v1` = high word). Loader warns once per mod.

`api->call(cpu, addr)`: call a guest function through dispatch (its chain
runs). Set a0-a3 first, read v0 after. Only inside an override. Across
`base()` or `call()`, expect a0-a3, t0-t9, v0, v1, ra clobbered; s0-s7, sp,
fp, gp preserved. Read args before `base()`, results after. A raw override
that nests `call()` and then returns into the original must restore the
argument registers it overwrote. `cpu->pc` is stale in stock builds. GTE
arrays are live. `CPUState*` is valid only for the override's duration, sim
thread only.

### Contexts

| Context | Where | Guest RAM, `call`, `guest_alloc`, registration |
|---|---|---|
| Tick | entry, overrides, toggle hook, refine callbacks | legal |
| Present | `register_present_hook` bodies, `OPENPETE_MOD_UI_SECTION` bodies | refused with an error, returns -1 |

Present hooks run once per extracted scene (canonical per tick + sub-tick
scenes at high `--render-fps`), on the sim thread ahead of display; count
and timing are not a contract. `ctx->tick`, `ctx->alpha` [0,1) (0 on
canonical), `ctx->dt` seconds, `ctx->aspect`. Publish state from tick hooks
into statics, evaluate curves at `ctx->alpha`; the engine never interpolates
mod visuals. Plain headless never presents; headless with a screenshot flag
does, and the hook fires then.

Reading `binding_down` in a tick hook diverges gameplay from a replay by
design; display-only reads belong in present hooks.

### Guest memory

- `api->guest(vaddr)` -> host pointer (NULL for unmapped, including 0 and
  physical < 0x10000). Accepts 0x00/0x80/0xA0 forms; game pointers use 0x80.
- `api->guest_addr(host)` -> vaddr (0 if outside guest memory).
- Little-endian on every host: typed reads through the pointer are the
  game's values. Pointer fields in guest structs are vaddrs: translate before
  deref; write vaddrs from `guest_addr`, never host pointers.
- `op_read_{u,s}{8,16,32}(api, va)`, `op_write_*(api, va, v)`: inline in
  the API header.
- Persist vaddrs, never host pointers (arena, savestates, structs the game
  reads).
- `g_Models` (0x80076378) = 512 × u32 rows indexed by `m_Class`; a row with
  the top bit clear is a SimpleModel marker, not garbage. Class 250 = the
  crystal-dragon statue, the LAST model of every level, and its row bounds
  the model buffer for `LoadDragonCutscene`: a mod that moves it must show
  the stock row to that loader or the rescue cutscene hangs. Dragon egg =
  class 34, a shared PETE.WAD model absent from level headers.

### Arena (`api->guest_alloc(self, size, align, flags, &host_view)`)

Bump allocator, no free, zeroed on first allocation, reachable through every
guest access path. Timeline state: savestates, rewind, runahead, handoff
carry and restore it like RAM. Put mutable per-session state here (host
statics do not roll back; keep them for caches, file handles, UI drafts).
Allocate at entry; per-level allocation grows every snapshot. Write through
the view in tick context only. On reload the i-th call with the same
(size, align, flags) returns the same vaddr and bytes; a savestate whose
ledger does not prefix-match is refused naming the mod. Never hardcode the
arena base. `OP_GALLOC_IMMUTABLE`: written once after the allocating tick,
checksummed per snapshot, a changed block logs the mod (`guest arena: ...
marked IMMUTABLE but its bytes changed`). Only truly park-once payloads
qualify; anything the game rewrites (model headers patched per level) does
not.

Two pools, picked by flags, never by address. Default: DMA-reachable, 14 MiB
below physical 16 MiB (9 MiB segment at 0x80700000 first, then 5 MiB at
0x80200000; largest block 9 MiB); GPU packets, primitives, OTs and anything
stored in a 24-bit field go here. `OP_GALLOC_NO_DMA` (api 13): general pool,
480 MiB at 0x81000000, for every block nothing truncates to 24 bits. A full
pool fails the call (0, `guest arena exhausted: ...` plus every mod's use of
that pool) and never spills. One `guest arena: <id> holds ...` line per mod
per session; the dev overlay's Mods section shows the totals. Snapshot cost
is per used byte in both pools, IMMUTABLE included: savestate files and
runahead copy the used ranges; rewind keyframes share unchanged 4 KiB pages,
so only pages rewritten each second cost rewind per second of window.

Animation frame words hold a 21-bit vertex-stream field that cannot address
the arena: `api->vstream_handle(self, stream_vaddr)` returns a handle
(< 0x8000, never 0) to write into the field as-is in both encodings; readers
resolve it. Request at entry, fixed order, once per stream (ledgered like
allocations; a route-dependent order makes savestates from other routes
refuse). `guest()` on a handle-bearing field returns NULL. Spyro 1 only:
S2/S3 frame words carry a halfword offset from the animation header, so move
a stream through the header's pointers; a per-game S2/S3 build using the
member does not compile (the error states why), a "*" object gets 0 + one
log line.

S1 animated-model ceilings: 255 verts per tier (count byte at header +8,
+9 = low-detail count); ~1600 faces overflows the game's polygon buffer and
hangs it regardless of the native renderer.

### Toggle and reload

`api->register_toggle_hook(self, fn(cpu, on))` runs at the tick boundary
after a Mods-panel flip. Disabling freezes registrations but never rewinds
guest state: restore stock bytes on `on = 0`. `OPENPETE_MOD_HOT_RELOAD=1`:
`config.toml` / `shaders/` / `src/` edits reload the mod (source edits
recompile; others re-enter against the cached object). A broken reload
keeps the running instance.

### Services

| Need | Call | Context | Guest RAM |
|---|---|---|---|
| Log | `log(self, OP_MOD_LOG_{DEBUG,INFO,WARN,ERROR}, fmt, ...)`; stderr tag `mod:<id>` | any | no |
| Mods-panel lines | `ui_status(self, fmt, ...)`; accumulate within a tick, reset on the mod's first call in a new tick, ~24-line cap | any | no |
| Toast | `notify(self, key, timeout_ms, fmt, ...)`; same-key coalesces, per-mod rate limit, no-op on runahead/rewind ticks | entry, tick, present | no |
| HUD text | `draw_text(x, y, cap_h, rgb, str)`; overlay space: y 0..1 top-down, x 0..aspect, units = window height; tick call shows all presents of the tick, present call shows that present; 0x605010 = HUD gold | both | no |
| Glyphs for text/notify | `0-9 A-Z space % / ? + ^ . ' :`; lowercase folds up; others = space; nothing until the game loads its font | | |
| Settings | `config_bool/int/float(self, key, def)`, `config_str(self, key, def, buf, cap)` | any | no |
| Hotkey | `binding_down(self, name)` = 1 while held | any | see above |
| Own sound | `sfx_play(self, "name", gain 0..4)` = `assets/sfx/own/<name>.wav`, host-side, never SPU | any | no |
| Replace game sound | `sfx_replace(self, "soundName", adpcm_body, len, pitch)`; raw SPU-ADPCM, no VAG header; pitch 0x1000 = 44.1 kHz, 0 = game's | tick | no |
| Other level's data | `read_level_data(self, level_id, buf, cap)`; NULL buf sizes | tick | no |
| Fog retarget | `texpack_fog_shift(self, scale_rgb, offset_rgb 0..255)`; NULL,NULL = stock | tick | no |
| Files | `data_dir(self)` -> absolute path | any | no |
| Custom panel | `openpete_mod_ui.h` widget vtable via `OPENPETE_MOD_UI_SECTION(fn)` at file scope in one TU; separately versioned (`OPENPETE_MOD_UI_ABI`), no stability promise; body runs in present context | present | no |
| Beyond the eleven widgets | `openpete_imgui.h`: the whole ImGui C API, opted into with `OPENPETE_MOD_IMGUI()` at file scope in one TU. Calls are legal only inside a section body - anywhere else they are dropped and logged; lifecycle and input-injection calls are absent from the table and fail the mod's compile. See pages/imgui.md | present | no |

### Timing landmarks (cold boot into the attract demo)

CameraUpdate first runs at VBL ~3495. Canonical tick 2700 ≈ VBL 4950.
`--skip-to-level`: the level renders from VBL ~1700; use `--frame-limit
≥ 2300` and capture at ≥ 1700.

## 5. Shaders

GLSL compiled in-process (glslang, `vulkan1.0`), cached by content hash in
`.build/shaders/`, errors in `.build/shaders/<name>.log`; a broken shader =
unavailable effect, mod still loads. Shaders cannot touch guest RAM; C feeds
uniforms, nothing flows back. A mod shades what the engine draws; it cannot
add draws. To ship without source: `<path>.spv` + `<path>.spv.ihash`
sidecar matching the current interface hash (a stale sidecar refuses).

`api->shader_register(self, "shaders/x.frag")` -> handle or -1; extension
picks the stage. Reserved for the engine: push constants, every descriptor
set except set 3 binding 1, input locations.

### Post-process

```c
int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE /*or _SCENE*/, order);
api->postfx_set_uniforms(self, fx, &params, sizeof params);  // ≤ 1024 B, copied, any context
api->postfx_enable(self, fx, 1);                              // registered disabled
```

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1      // declares in vec2 op_uv, out vec4 op_color
#include "openpete_shader_api.glsl"     // sdk/shaders/, hashed into the build key
layout(set = 3, binding = 1, std140) uniform op_mod { vec4 p; };  // mirror the C struct by hand
void main() { op_color = texture(op_prev_pass, op_uv); }
```

Sets: 0 `op_prev_pass` (first pass = point input), 1 `op_scene_depth`
(opaque terrain pre-pass, no mobys; 1.0 where absent; encoded
d = 1 − 1/viewZ, decode viewZ = 1/(1−d)), 2 `op_prev_frame`, 3.0
`op_engine` UBO: `op_time, op_alpha, op_tick, op_present, op_canonical,
op_aspect, op_resolution, op_window, op_scene_rect (uv rect of the 4:3
content), op_scene_grid (512, 240, 224 visible rows, widen factor)`.
Points: `OP_POSTFX_COMPOSITE` (after HUD, before blit), `OP_POSTFX_SCENE`
(before HUD), `OP_POSTFX_PRESENT` refused. Passes run every present.
Cross-mod order = enabled-list priority; `order` breaks ties within a mod,
lower first. 64 passes engine-wide. `op_uv` (0,0) = top-left;
`texture(op_prev_pass, op_uv)` is identity.

### Materials

Replace fragment shading of a subset of the engine's draws. Inherit blend,
depth, stencil, order; colour math only; cannot hide or move geometry;
unmatched or failed = stock. 255 material slots engine-wide.

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"   // NOT openpete_shader_api.glsl (set-0 conflict)
layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
vec4 op_material(vec4 base) { return vec4(mix(base.rgb, tint.rgb, tint.a), base.a); }
// no main(), no output var, no discard; return base.a unchanged to keep holes
```

`base` = CLUT decode + perspective divide + PSX modulate + active HD texture
replacement + fog; untextured = interpolated vertex colour. Helpers:
`op_psx_shade()`, `op_pack_shade()` (samples the HD pack, falls back to
stock; prefer it or the material seams against HD neighbours),
`op_psx_texel(ivec2)`, `op_uv()`, `op_color()`. Moby drop shadows use
`openpete_psx_decal.glsl` (engine picks it per path; body unchanged;
subtractive blend, brighter tint = darker ground). Terrain is compiled twice
(per-present walk and retained GPU arenas); colour must be a function of the
face alone.

Selector keys (manifest row or `openpete_mod_material_selector_t`; omitted
= any; unknown manifest key refused):

| Key | Values |
|---|---|
| `channel` | `any moby terrain player flame particle sky shadow tracer portal` / `OP_CHAN_*` (1..9) |
| `ident` | flame: 0 ribbon, 1 caps; particle: `OP_PART_IDENT(kind, sel)` = `(kind<<8)\|sel`, kinds QUAD 0 (sel = particle class), LINE 1 (sel = class), GLOW 2, FLARE 3, SPARK 4, STAR 5; shadow caster kind; portal `OP_PORTAL_FLAT 0 / DOME 1 / BLENDED 2` (use -1: the portal pops through all three on approach); player/sky/tracer emit 0 |
| `class` / `moby_class` | moby class |
| `texture` | terrain texture record 0..127, per level: pair with `level` |
| `level` | `g_LevelId` |
| `tier` / flags | `"hp"` `"lp"` / `OP_SEL_HP_TIER`, `OP_SEL_LP_TIER`; neither = both |
| `textured` / flags | `true` `false` / `OP_SEL_TEXTURED`, `OP_SEL_UNTEXTURED` |
| `env_anim` / flags | `true` / `OP_SEL_ENV_ANIM` |
| `params` | floats = the std140 block |
| `enabled` | `false` ships off |

C tier (api 3+): `sel = openpete_mod_material_sel_any();` then assign
(never zero-init: that means channel 0 class 0). `material_register(self,
&sel, frag)` (tick context; refused in present/UI section and on a channel
the game does not serve, as `[[material]]` rows are) ->
`material_set_params(self, mat, &blk, len ≤ 256)`, `material_enable`,
`material_set_selector` (re-key a live material; from present/UI applies at
next tick), `moby_classes(self, out, cap)` (classes the current level's
load placed, ids to 767 on Spyro 3, at most cap copied, tick-boundary
snapshot, safe from UI; NULL,0 sizes). Manifest rows
arm before entry, so C registrations win overlapping pixels.
`material_register_refine(self, &sel, frag, cb)`: `cb(key, out, cap)`
returns bytes written (0 = claim, no params) or -1 = stock; runs at
extraction in tick context, memoised per instance, must be pure over the key
and tick-stable guest RAM; moby channel keyed per instance
(`key->u.moby.vaddr`, `.moby_class`), terrain per texture id / untextured
band (`key->u.terrain.texture`, `.flags`); identical blocks share a batch,
distinct blocks cost a draw each. `sdk/examples/channel-gallery` lights each
selector live and is the map of what is selectable.

## 6. Assets

Matched by filename; all host-side, never guest RAM. Later enabled mod wins;
mod roots outrank `<exe>/assets/` and the user data dir.

**Textures** `assets/textures/`: `<label>-<thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png`
(content identity: upload hash, texel box, palette hash, depth; label
ignored) or `<name>.png` symbolic (`title_ui`). Get names with
`OPENPETE_TEXPACK_DUMP_DIR=<dir>`: every drawn texture is written once under
its content name (2D draws at square-pixel aspect, world 1:1). Any
resolution = HD. `assets/textures/own/<hexid>.png` registers a mod-chosen
id for mod-installed models. `--texpack-dir` / `OPENPETE_TEXPACK_DIR` = base
pack outside mods.

**Music** `assets/music/track_NN.wav` (NN = zero-padded `m_MusicTable`
index; 20 = title) or `manifest.txt` lines `<index> <relative path>` (later
wins, tried first). PCM16 mono/stereo at 11025/22050/32000/44100/48000 Hz,
loaded whole.

**Sounds** `assets/sfx/`: substitution at the SPU voice's PCM handoff; sound
tables, SPU RAM, addresses, pitch, and the tick the game sees the sound end
stay stock. Census: `openpete --disc X.cue --dump-sfx-pack dump/`
(+ `--dump-sfx-wads a.wad`) writes `<label>-<hash>-p<pitch>.wav` + `index.txt`.
Forms: `<label>-<hash>-p<pitch>.wav` (that sample at that base pitch),
`<label>-<hash>.wav` (raw 44.1 kHz, game pitches), `<slot>.wav` (role, e.g.
`gemPickup.wav`, every level). Precedence: exact tag > untagged hash >
other-definition tag > symbolic; code `sfx_replace` > symbolic file. Any
rate/depth; stereo downmixed. Game still decides duration (longer rings out
on a side channel, shorter = silence), pitch/variance (`-flat` suffix pins
the file's rate), volume, pan, loop (`smpl` chunk = loop window). Rescanned
at each level load. `OPENPETE_SFXPACK_LOG=1` logs uncovered samples;
`OPENPETE_SFXPACK_VERIFY=1` compares PCM (`verify_bad` = 0 on a clean dump);
`[sfxpack] keyed/resolved/replaced/rings_out` at exit. Own sounds under
`assets/sfx/own/` are not scanned. `--sfxpack-dir` = base pack.

**Level data** `assets/wad/level<ID>.WAD` (any size) / `.OVL` (stock size;
loads into a fixed buffer), ID = logical level id. Disc-derived: generate on
the player's machine.

**Custom levels** `[[level]]`: `files` stem -> `assets/levels/<stem>.WAD`
(+ optional `.OVL`, donor's stock size; omit = stock overlay), `donor` =
lower_snake name or id 10..64 whose code the level targets, `replace = true`
serves over the donor slot (portal preview shows the custom sky), `music =
"x.wav"` from `assets/music/` (engine sizes loop bookkeeping to the file),
`title` overrides the in-game string. All custom levels share slot 65
(shared per-level progress) unless replacing. Names filter to `A-Z 0-9
space ! , ? . '`; portal letters render `A-Z 0-9 .`. Custom overlays are
loaded but not yet recompiled/executed.

## 7. Save, rewind, runahead declarations

- `sim_mutative`: derived false for a mod with no `src/` and no `[[level]]`;
  assumed true for `src/` unless declared false. Memory cards are tagged
  with the mutative modset at first write; a mismatch warns, never blocks;
  `--adopt-saves` re-tags.
- `state`: undeclared = rewind and timeline jumps refused (one warning);
  `"none"` = nothing derived from guest history kept (a counter restarting
  at zero is fine); `"rebuildable"` = every host value re-derivable from
  guest RAM after a jump.
- `runahead`: undeclared = runahead disabled while the mod is enabled;
  `"pure"` = tick hooks have no host effects; `"host-fx"` = host effects
  (log, sound, files) the engine may suppress on speculative ticks.

## 8. Iteration workflow

Run from `<exe>`. Every unattended launch: `--vibration 0 --mute 1`
(`--mute 0` when auditing audio). Log = stderr, mirrored to
`<state>/logs/openpete.log` (`--log-file PATH`, `--no-log-file`); mod lines
carry `mod:<id>`; `--log-level 'mods:debug,*:info'`.

```
openpete --disc <game>.cue --mods-dir mods --mods my-mod \
  --headless --frame-limit 6000 --vibration 0 --mute 1 2>&1 | tee run.log
```

- **Isolate**: `--mods-dir <scratch>` with only the mod under test symlinked
  or copied, plus `--mods <id>`. `--mods ""` = stock baseline for A/B.
- **Skip the boot**: `--skip-to-level stone_hill` (names: artisans 10,
  stone_hill 11, dark_hollow 12, town_square 13, toasty 14, sunny_flight 15,
  peace_keepers 20, dry_canyon 21, cliff_town 22, ice_cavern 23,
  doctor_shemp 24, night_flight 25, magic_crafters 30, alpine_ridge 31,
  high_caves 32, wizard_peak 33, blowhard 34, crystal_flight 35,
  beast_makers 40, terrace_village 41, misty_bog 42, tree_tops 43,
  metalhead 44, wild_flight 45, dream_weavers 50, dark_passage 51,
  lofty_castle 52, haunted_towers 53, jacques 54, icy_flight 55,
  gnorc_gnexus 60, gnorc_cove 61, twilight_harbor 62, gnasty_gnorc 63,
  gnastys_loot 64). Level live from VBL ~1700. `--cheats 99_lives,unlock_all_levels`.
- **Screenshots** (headless-capable, native renderer): `--screenshot-on-tick
  N[,N2]` (same sim moment every run; prefer over `--screenshot-on-frame`,
  whose VBL<->tick mapping drifts) `--screenshot-native-only
  --screenshot-output shot_{n}.png` -> `shot_<n>-vk.png`. Present hooks and
  postfx run on these presents. `--screenshot-intermediate-on-frame N` =
  alpha 0.5 sub-tick captures. `--screenshot-freeze-backdrop` for
  pause/results screens. `--screenshot-presents-start N
  --screenshot-presents-count K` = K consecutive presents. Windowed `-vk.png`
  is a readback of the presented frame; headless recomposes the canonical
  scene.
- **Pose the camera**: `--freecam-pose "x,y,z[,yaw,pitch]"` (also
  `"topdown"`) engages the debug free camera at that pose once the level is
  live (gameplay freezes: UpdateSpyro stops). Get coordinates interactively:
  press F (or left-stick click), fly with WASD / Space / LShift / arrows /
  `[` `]` speed, T drops Spyro at the camera; the pose is logged as
  `pos=(x, y, z) yaw=N pitch=N`; feed it back as the flag. `--freecam-path
  "[ticks=N;] x,y,z[,yaw,pitch];..."` flies a loop. `--window WxH`,
  `--aspect 16:9`, `--render-fps N` shape the capture.
- **Place Spyro**: `--player-pose "x,y,z[,yaw]"` (yaw = facing, 4096 per
  turn) places him once gameplay is live under pad control, clears momentum
  and re-seats the follow camera behind him. Every run prints its end state
  after `openpete-exit:` as `openpete-pose: level=<id> pos=(x, y, z) yaw=N`
  (or `none reason=...`); paste it back with `--skip-to-level <id>`. Fires
  before `--freecam-pose` in the same run; rides the bk2 `.args` sidecar.
- **Reproducible input**: `--bk2 movie.bk2` replays a BizHawk movie
  (`--bk2-stop-on-end`); `--bk2-record [PATH]` records live play plus an
  `.args` sidecar. Same movie + same flags = same gameplay bytes unless the
  mod writes guest RAM.
- **Pixel A/B**: capture the same tick with `--mods ""` and `--mods <id>`,
  diff the PNGs; count changed pixels in the band the change should touch.
  Zero changed pixels = the path never ran (vacuous), not success.
- **Neutrality check** for a mod claiming `sim_mutative = false`: compare
  `--gameplay-hash-log a.csv` runs with and without the mod on the same
  input; identical = no guest write. `--position-log` = Spyro position CSV.
  `--probe HEX` logs 4 bytes at a vaddr; `--ram-dump-on-frame N` dumps RAM.
- **Savestate / rewind / runahead posture**: `--state-save-on-frame N
  --state-save-output f.state`, `--state-load-on-frame N --state-load-input
  f.state`, `--state-stress N`, `--state-stress-rewind K`, `--rewind-hold
  A:N`, `--runahead 1`. Watch for `refused`, `DESYNC`, `IMMUTABLE ... bytes
  changed`, and the mod's name in the log.
- **Keys in a window**: O settings menu; M Mods overlay (dev; toggles mods,
  edits `[[config]]`, shows `ui_status`); F7 pause, F8 tick-step, Tab
  turbo, 1/2/3 savestate save/load/slot, Backspace rewind (`--rewind N`),
  G camlock (dev), Esc quit. `--keys-report` prints the live map. PSX pad
  on keyboard: C cross, V circle, X square, Z triangle, arrows d-pad,
  LShift/LCtrl L1/L2, RShift/RCtrl R1/R2.
- **Hot loop**: `OPENPETE_MOD_HOT_RELOAD=1` windowed; edit `src/`, `shaders/`
  or `config.toml`; the splash shows COMPILING MODS on a source change.
  Check `.build/compile.log` and `.build/shaders/*.log` on silence. Force a
  full compile of every `src/` mod without booting: `--compile-mods
  --mods-dir mods` (fails on any error). Env knobs do not cross WSL->Windows;
  use the CLI twins or `WSLENV`.
- **Headless test recipe** (what a `modtest` declares): run N VBLs with
  `--frame-limit`, optional `--bk2`, assert expected substrings in the log,
  and for a non-mutative mod assert the gameplay hash matches stock.

Failure signatures: `no compiler and no prebuilt` = mod silently disabled
(Windows exe cannot run a Linux cross-compiler; hand-build the `.dll` with
the flags in §2 plus `sdk/openpete_mod_abi.c`); `carries no ABI descriptor`
= add that file; `-1` from `material_register` inside a widget/present =
wrong context, use `material_set_selector`; a material on an untextured moby
that never lights = selector needs `textured = false`/both; refine callback
output that strobes = impure over non-tick-stable state; blank present-hook
output headless = no screenshot flag, so no present; `OPENPETE_FRAME_LIMIT`
is not an engine knob, only `--frame-limit`.

## 9. Symbols worth knowing

Globals (`OP_GADDR_*`; all 0x80 form): `g_Spyro` 0x80078A58 (Moby; position
at +0x0C), `g_Camera` 0x80076DD0, `g_GemTotal` 0x80075860, `g_LevelId`
0x8007596C, `g_Gamestate` 0x800757D8 (0 playing, 1 level transition, 2 pause
menu, 3 inventory, 4 respawn, 5 game over, 7 flight results, 8 dragon,
9 entrance animation, 10 exit level, 11 fairy), `g_GameTick` 0x8007572C, `g_LevelMobys` 0x80075828,
`g_DynMobys` 0x80075890, `g_Models` 0x80076378, `g_Environment`
0x800785A8, `g_Portals` 0x80078640, `g_Sparx` 0x80075898,
`g_SpyroLifeCount` 0x8007582C, `g_Pad` 0x80077378, `g_WadHeader`
0x8007A6D0, `g_LevelHeader` 0x80076C00, `g_Buffers` 0x800785D8.

Prototyped functions (typed wrappers): `CameraUpdate`, `GamestateUpdate`,
`GamestateDraw`, `CollectItem(Moby*)`, `GenerateGemCollectMobys(int, Moby*)`,
`PlaySound(int, Moby*, int, u_char*)`, `StopSound`, `SetMusicState(int,int)`,
`LoadLevel(int)`, `LoadLevelData`, `LoadLevelScene`, `PatchMobyModelPointers
(Model*)` (runs several times per level over the same models),
`LoadDragonCutscene`, `HudTick`, `HudPrint`, `HudGemUpdate`, `SpawnMobySparkle`,
`RandRange`, `Sin`/`Cos` (4096-unit circle; Sin(1024) = 4096), `Atan2`,
`Vec*`, `OctDistance`, `Memcpy/Memset`, `SaveLoad/SaveCreate/SaveChecksum`,
`Titlescreen*`, `Pad*`, `CheatsProcess`, `func_80022A2C` (runs every
screen), `func_80019300` (HUD list builder, gameplay only). Full list:
`sdk/games/spyro-1/openpete_sdk_wrappers.h`.

`Moby` (0x58): `m_Position` +0x0C (Vector3D int), `m_RotationMatrix` +0x20,
`m_Class` +0x36 (short), `m_AnimationState` +0x3C, `m_Rotation` +0x44
(3 bytes), `m_DepthOffset` +0x47, `m_State` +0x48, `m_Substate` +0x49,
`m_SectorIndex` +0x4A, `m_Renderer` +0x4B (7-bit distance | shiny bit),
`m_SpecularMetalColor` +0x4C, `m_SpecularMetalType` +0x4F, `m_RenderRadius`
+0x50, `m_WasDrawn` +0x51 (gameplay reads it), `m_UpdateDistance` +0x52,
`m_ScaleOverride` +0x57. Screen-space billboard recipe: Memset 0, class,
position x,y = screen coords, z = depth, `m_DepthOffset = 127`,
`m_RenderRadius = 0xFF`, `m_SpecularMetalType = 0`.
