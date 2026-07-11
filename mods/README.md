# openpete mods

Every subdirectory here with a `mod.toml` is a mod; `<id>.zip` archives
load identically (unpacked + cached automatically). `enabled.txt` lists
enabled mods, one id per line — **the order IS the priority**: mods
later in the list run first on shared overrides, and `base()` descends
toward earlier mods, then the game's original code.

## Trust posture — read this first

**Mods are native programs.** A mod's C source is compiled and loaded
into the game process with the same privileges as the game itself.
There is no sandbox and (as yet) no signing. **Install only mods you
trust**, exactly as you would any other program. Crash reports name the
offending mod, the per-mod time instrument names slow ones, and the
save-safety tag warns when a card was played under a different modset —
but none of that is a security boundary.

Mods may not ship game-derived data (models, textures, audio ripped
from any Spyro disc). Ship a **generator** that derives assets from the
user's own disc instead — see `recolored-artisans` for the pattern.

## Anatomy

```
mods/<id>/
  mod.toml          manifest (below)
  src/*.c           compiled on load against <exe>/sdk/ (Linux; Windows
                    loads a prebuilt .build/<id>.dll)
  assets/           optional overlay root (textures/, music/, sounds/,
                    wad/) — pure-asset mods need no src/
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
above   = ["x"]           # advisory: must be listed AFTER x
tier    = "raw"           # declare when using raw func_* addresses
sim_neutral = true        # observer/presentation-only: excluded from
                          # the save modset tag

[[binding]]               # declared host keys (conflicts are reported)
name = "warp"
key  = "F6"
purpose = "warp home"

[[site]]                  # instruction-site hooks (the escape hatch);
name = "fade-bound"       # baked by scripts/regen-mod-sites.sh
addr = 0x800128EC
kind = "replace"

[test]                    # modtest harness (tools/modtest.py)
frames        = 8000
expect_oracle = "skip"    # pass | fail | skip
input         = "test/scenario.bk2"
args          = "--skip-to-level artisans"
expect_log    = ["my-mod: it worked"]   # single line — parser is line-based
```

### The API in one breath

`openpete_mod_entry(api, self)` runs at load. Override game functions
by name (`api->override_name`) or raw address; `api->base(cpu)` calls
down the chain. Read/write guest RAM via `api->guest` + the typed
structs in `openpete_game_structs.h` (layout-pinned at compile time).
Draw HUD text/rects and replay actors through the native renderer
(never guest prims). Services: savestate trigger + participation,
`play_sound`, `binding_down`, `bk2_status`, `guest_alloc`, content
registries (`register_moby_type`, `register_level_slot`,
`enter_level`), and instruction sites (`site_handler`). Present hooks
are render-pure — sim work belongs in tick hooks; the loader enforces
it. Full contracts live in `sdk/openpete_mod_api.h` next to the exe.

## The gallery is the documentation

Every mechanism ships with a working, CI-tested example in this
directory — start from the one nearest your idea (`hello-anchor` for
observers, `double-gems` for gameplay taps, `hud-stopwatch` for HUD,
`ghost-lite` for actor replay, `new-moby`/`mirror-level` for content,
`fade-skip` for instruction sites, `triangle-flutter` for the full
flagship treatment). `scripts/check_mods.sh` runs the whole ladder.

## Installing from a mod-sources manifest

`tools/modget.py` installs mods listed in a JSON manifest (a
"mod-source"): each entry names a zip URL and its sha256. Fetching
verifies the hash before the zip lands here. Anyone can host a
mod-source; there is no central registry — the hash pins what you get,
the trust posture above still applies to what you run.
