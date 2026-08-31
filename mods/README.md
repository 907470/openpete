# openpete mods

Every subdirectory here that contains a `mod.toml` file is a mod, and an
`<id>.zip` archive loads identically because the engine unpacks and
caches it automatically. `enabled.txt` lists the enabled mods, one ID per
line, and order determines priority: mods later in the list run first on
shared overrides, and `base()` descends toward earlier mods and then to
the game's original code. To override the file for a single run, pass
`--mods a,b` or set `OPENPETE_MODS`; `--mods ""` enables none.

## Build the full documentation

The complete mod-author documentation, comprising the guides plus the
generated API, function, global, and struct reference, builds with the
game:

```
cmake --build --preset linux-release --target mod_docs
```

The result lands in
`build/<preset>/games/spyro-1/moddocs/site/index.html`. For a
live-preview server, run `scripts/build-mod-docs.sh --serve`, which
needs `pip install mkdocs-material`. The sources live in
`docs/modding/`.

## Before you install a mod

Mods are native programs. A mod's C source compiles and loads into the
game process with the same privileges as the game itself. There is no
sandbox and, so far, no signing. Install only mods that you trust,
exactly as you would any other program.

openpete names the offending mod in a crash report, names slow mods in
the per-mod time instrument, and warns when a card was played under a
different set of mods. None of that is a security boundary. An
asset-only mod with no `src/` directory never loads code, which narrows
what you have to trust down to the assets themselves.

Mods must not ship game-derived data, such as models, textures, or audio
ripped from a Spyro disc. Ship a generator that derives those assets
from the player's own disc instead. See `recolored-artisans` for the
pattern.

## Most mods are files, not programs

The primary way to mod the game is declarative: a `mod.toml` file plus
files under `assets/`. There's no C, no compile step, and no API version
to pin, so an asset-only mod can't break across engine updates because
it has no ABI at all. The engine picks the files up by convention:

```
assets/textures/replacements/*.png    HD texture replacement (native renderer)
assets/music/*.wav                    per-track music replacement
assets/sounds/*.wav                   mod-local one-shots (sfx_play / [[binding]])
assets/levels/<stem>.WAD
assets/levels/<stem>.OVL              custom level payloads, wired by [[level]]
```

A `[[level]]` block in the manifest turns a pair of level payloads into
a playable custom level. The engine handles the slot claim, the
table-of-contents serving, and the warp row in the M overlay. See
`grim-garden`. A `[[binding]]` block declares keys that players can
rebind.

Five in-tree mods ship exactly this way: `music-swap`,
`hd-single-texture`, `recolored-artisans`, `grim-garden`, and
`spyro15-cliff-town`. Start there rather than at the C API.

## Anatomy of a mod

```
mods/<id>/
  mod.toml          manifest (documented in the next section)
  assets/           overlay root (conventions listed earlier) - asset-only
                    mods need nothing else
  config.toml       optional player-editable settings
  src/*.c           optional code tier: compiled on load against
                    <exe>/sdk/ by the bundled toolchain (all platforms;
                    a prebuilt .build/<id>.dll is only the fallback
                    when no toolchain is available)
  data/             per-mod persistent dir (created on demand)
  test/             self-contained inputs for the [test] block (bk2s)
```

### mod.toml

```toml
[mod]
id      = "my-mod"        # must match the directory name
name    = "My Mod"
version = "0.1.0"
api     = 1               # SDK api-version pin
deps    = ["other-mod"]   # hard deps (topo-sorted, cycles rejected)
state   = "none"          # none | rebuildable — host-state posture;
runahead = "pure"         # pure | host-fx — both gate savestate/
                          # rewind/runahead with mods enabled
sim_neutral = true        # observer/presentation-only: excluded from
                          # the save modset tag

[[binding]]               # declared host keys (conflicts are reported).
name = "warp"             # Players rebind these in openpete.toml via
key  = "F6"               # [keys.mod.<id>] without editing the mod.
purpose = "warp home"

[[config]]                # settings schema: the ENGINE renders these in
key     = "gems.sfx"      # the Mods panel and writes committed edits
type    = "bool"          # back into config.toml (comments preserved);
default = true            # entry re-reads them via api->config_* after
label   = "S3 collect chime"   # the queued reload. types: bool | int |
group   = "Gems"          # float | string; min/max = slider, items =
help    = "tooltip text"  # combo.

[test]                    # modtest harness — lives in a SIBLING
                          # modtest.toml (this repo's CI concern, not
                          # part of the manifest format)
frames        = 8000
expect_oracle = "skip"    # pass | fail | skip
input         = "test/scenario.bk2"
args          = "--skip-to-level artisans"
expect_log    = ["my-mod: it worked"]   # single line — parser is line-based
```

### The code tier at a glance

`openpete_mod_entry(api, self)` runs at load. Override game functions by
name with `api->override_name`, or by raw address. `api->base(cpu)`
calls down the chain, and `api->call` invokes game code. Read and write
guest RAM through `api->guest` plus the typed structs in
`openpete_game_structs.h`, whose layouts are pinned at compile time.
Draw HUD text and rectangles through the native renderer, never through
guest prims.

The services are `sfx_replace` and `sfx_play`, `guest_alloc`,
post-process shaders through `shader_register` and `postfx_*`, settings
through a `[[config]]` schema and `config_*`, `ui_status` lines, and
`binding_down`. Present hooks are render-pure, so simulation work
belongs in tick hooks, and the loader enforces the split.

The API struct is at v1 and is append-only, with one deliberate
exception on record: the moby-type registry was removed at api 7, before
any mod outside this tree used it. The full contracts live in
`sdk/openpete_mod_api.h` next to the executable. The widget annex,
`openpete_mod_ui.h`, is versioned separately and carries no stability
promise.

## Start from an example

Every mechanism ships with a working, CI-tested example in this
directory, so start from the one nearest to your idea.

Declarative examples: `music-swap` (one WAV file), `hd-single-texture`
(one PNG file), `recolored-artisans` (a generated texture pack), and
`grim-garden` with `spyro15-cliff-town` (custom levels).

Code-tier examples: `hello-anchor` for observers, `double-gems` for
gameplay taps, `hud-stopwatch` for HUD draws and bindings, the `crt-*`
mods for shaders, and `spyro3` for the full flagship treatment.

`scripts/check_mods.sh` runs the whole ladder.

## Install from a mod-source manifest

`tools/modget.py` installs mods listed in a JSON manifest called a
mod-source. Each entry names a zip URL and its sha256 hash, and fetching
verifies the hash before the zip lands here. Anyone can host a
mod-source, and there is no central registry: the hash pins what you
get, and the trust guidance earlier in this file still applies to what
you run.
