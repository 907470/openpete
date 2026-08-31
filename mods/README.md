# openpete mods

**Full mod-author documentation** (guides + generated API/function/
global/struct reference) builds with the game: `cmake --build --preset
linux-release --target mod_docs` →
`build/<preset>/games/spyro-1/moddocs/site/index.html`, or
`scripts/build-mod-docs.sh --serve` for a live-preview server
(needs `pip install mkdocs-material`; sources in `docs/modding/`).

Every subdirectory here with a `mod.toml` is a mod; `<id>.zip` archives
load identically (unpacked + cached automatically). `enabled.txt` lists
enabled mods, one id per line — **the order IS the priority**: mods
later in the list run first on shared overrides, and `base()` descends
toward earlier mods, then the game's original code. `--mods a,b` (env
`OPENPETE_MODS`) overrides the file for a single run; `--mods ""`
enables none.

## Trust posture — read this first

**Mods are native programs.** A mod's C source is compiled and loaded
into the game process with the same privileges as the game itself.
There is no sandbox and (as yet) no signing. **Install only mods you
trust**, exactly as you would any other program. Crash reports name the
offending mod, the per-mod time instrument names slow ones, and the
save-safety tag warns when a card was played under a different modset —
but none of that is a security boundary. (A pure-asset mod with no
`src/` never loads code, which shrinks what trust has to cover to the
assets themselves.)

Mods may not ship game-derived data (models, textures, audio ripped
from any Spyro disc). Ship a **generator** that derives assets from the
user's own disc instead — see `recolored-artisans` for the pattern.

## Most mods are files, not programs

The primary way to mod the game is **declarative**: a `mod.toml` plus
files under `assets/`. No C, no compile step, no API to version — a
pure-asset mod cannot break across engine updates because it has no
ABI at all. The engine picks the files up by convention:

```
assets/textures/replacements/*.png    HD texture replacement (native renderer)
assets/music/*.wav                    per-track music replacement
assets/sounds/*.wav                   mod-local one-shots (sfx_play / [[binding]])
assets/levels/<stem>.WAD
assets/levels/<stem>.OVL              custom level payloads, wired by [[level]]
```

`[[level]]` in the manifest turns a pair of level payloads into a
playable custom level (slot claim, TOC serving, the M-overlay warp row
— all engine-side; see `grim-garden`). `[[binding]]` declares
user-rebindable keys. Five in-tree mods ship exactly this way:
`music-swap`, `hd-single-texture`, `recolored-artisans`, `grim-garden`,
`spyro15-cliff-town` — start there, not at the C API.

## Anatomy

```
mods/<id>/
  mod.toml          manifest (below)
  assets/           overlay root (see conventions above) — pure-asset
                    mods need nothing else
  config.toml       optional user-editable settings
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
name = "warp"             # Users rebind these in openpete.toml via
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

### The code tier in one breath

`openpete_mod_entry(api, self)` runs at load. Override game functions
by name (`api->override_name`) or raw address; `api->base(cpu)` calls
down the chain, `api->call` invokes game code. Read/write guest RAM
via `api->guest` + the typed structs in `openpete_game_structs.h`
(layout-pinned at compile time). Draw HUD text/rects through the
native renderer (never guest prims). Services: `sfx_replace`/
`sfx_play`, `guest_alloc`, post-process shaders (`shader_register` +
`postfx_*`), settings ([[config]] schema + `config_*`), `ui_status`
lines, and `binding_down`. Present hooks are render-pure — sim work
belongs in tick hooks; the loader enforces it. The api struct is **v1
and append-only** — with one deliberate exception on record: the
moby-type registry was removed at api 7, before any mod outside this
tree used it. Full contracts live in `sdk/openpete_mod_api.h` next to
the exe; the widget annex (`openpete_mod_ui.h`) is separately versioned
and carries no stability promise.

## The gallery is the documentation

Every mechanism ships with a working, CI-tested example in this
directory — start from the one nearest your idea. Declarative:
`music-swap` (one WAV), `hd-single-texture` (one PNG),
`recolored-artisans` (generated texture pack), `grim-garden` /
`spyro15-cliff-town` (custom levels). Code tier: `hello-anchor` for
observers, `double-gems` for gameplay taps, `hud-stopwatch` for HUD
draws and bindings, the `crt-*` mods for shaders, and `spyro3` for the
full flagship treatment.
`scripts/check_mods.sh` runs the whole ladder.

## Installing from a mod-sources manifest

`tools/modget.py` installs mods listed in a JSON manifest (a
"mod-source"): each entry names a zip URL and its sha256. Fetching
verifies the hash before the zip lands here. Anyone can host a
mod-source; there is no central registry — the hash pins what you get,
the trust posture above still applies to what you run.
