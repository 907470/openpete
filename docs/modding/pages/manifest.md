# Manifest & config

## mod.toml

The manifest describes what the mod **is**. Everything except
`[mod] id/version/api` is optional.

``` toml
[mod]
id      = "my-mod"        # must match the directory (or zip) name
name    = "My Mod"
version = "0.1.0"
api     = 2               # SDK api-version pin (OPENPETE_MOD_API_VERSION)
deps    = ["other-mod"]   # hard deps: topo-sorted, cycles rejected,
                          # missing dep = actionable error
above   = ["x"]           # advisory: must be listed AFTER x in
below   = ["y"]           # enabled.txt / BEFORE y (order = priority)
tier    = "raw"           # declare when poking raw func_* addresses —
                          # region-pinned, may drift across updates
sim_neutral = true        # observer/presentation-only: never diverges
                          # gameplay state; excluded from the save
                          # modset tag

[[binding]]               # declared host keys — conflicts across the
name    = "warp"          # enabled set are reported at load; resolve
key     = "F6"            # with api->binding_down(self, "warp")
purpose = "warp home"

[[site]]                  # instruction-site hooks (the escape hatch):
name = "fade-bound"       # one guest instruction, replaced or appended.
addr = 0x800128EC         # Sites are BAKED at recomp-regen time —
kind = "replace"          # scripts/regen-mod-sites.sh; inert until then

[test]                    # the modtest harness (tools/modtest.py)
frames        = 8000
expect_oracle = "skip"    # "pass"  = must stay byte-identical to PSX
                          # "fail"  = must diverge (gameplay mods)
                          # "skip"  = don't assert the oracle
input         = "test/scenario.bk2"          # optional input movie
args          = "--skip-to-level artisans"   # extra engine flags —
                                             # a STRING, not an array
expect_log    = ["my-mod: it worked"]        # substring asserts
```

!!! note "`args` is a string"
    The modtest harness's parser is line-based and simple by design:
    `args = "--skip-to-level stone_hill"`, **not**
    `args = ["--skip-to-level", "stone_hill"]`.

## `[[level]]` — custom levels, zero code

A mod can ship a whole custom level with **no C at all**: one manifest
block plus two files. The engine registers the level in the M-overlay's
**Custom Levels** panel (sorted alphabetically, five per "Custom World"),
gives it a Warp button and a slot/replace mode switch, and handles all
the guest-side plumbing — level-slot claim, WAD TOC declaration, serving
your files when the game streams the level, restore on disable.

``` toml
[[level]]
name  = "Grim Garden"      # shown in the menu; also the sort key
files = "grim_garden"      # stem: assets/levels/grim_garden.data.bin
                           #     + assets/levels/grim_garden.ovl.bin
donor = "dark_hollow"      # whose loader/code the level runs under —
                           # lower_snake name ("cliff_town") or id (22)
replace = true             # optional: serve over the donor's own level
                           # instead of the shared free slot
```

- `<stem>.data.bin` is the level's WAD data segment (any size);
  `<stem>.ovl.bin` is the donor's code overlay (must stay the donor's
  stock size) — omit it to run the stock overlay.
- Pick the level your hack was authored against as `donor`.
- Multiple `[[level]]` blocks per mod are legal.
- Custom levels share the game's one free level slot (id 65) until the
  capacity work lands, so per-level progress (gems/dragons) is shared
  between them — `replace` mode sidesteps that by taking over the
  donor's own slot.
- Don't commit game-derived data: ship a derivation script (an xdelta
  applied to the user's own disc — see `grim-garden` or
  `spyro15-cliff-town` for the pattern) that writes `assets/levels/`.

## config.toml

`config.toml` (optional, next to `mod.toml`) describes what the user
**wants** from the mod. It is user-editable, read by the engine right
before your entry runs, and exposed through typed getters with
code-side defaults:

``` toml
# spyro3's config.toml, for example:
[gems]
models = true
sfx    = true

[chests]
models    = true
sfx       = true
collision = true   # false = stock S1 collision under S3 looks

[debug]
force_flame = false
```

``` c
int models_on = api->config_bool(self, "gems.models", 1);
int64_t count = api->config_int(self, "spawn.count", 4);
double  scale = api->config_float(self, "spawn.scale", 1.0);
char who[64];  api->config_str(self, "greet.name", "world", who, sizeof who);
```

Keys are dotted paths (`"gems.models"` = key `models` in table
`[gems]`; bare `"verbose"` is a root-level key). Absent file, absent
key, or a type mismatch ⇒ the default you passed — ship a `config.toml`
documenting the defaults, but keep the **code** defaults authoritative.
Parse errors log and fall back; they never disable the mod.

Read config at entry and branch there. In dev mode
(`OPENPETE_MOD_HOT_RELOAD=1`) the loader watches `config.toml`'s
content hash alongside your sources: an edit hot-reloads the mod
against the cached object — entry re-runs with fresh values, no
recompile.

Why not env vars? They don't cross process/interop boundaries (notably
WSL→Windows), they aren't discoverable, and they can't ship defaults.
`config.toml` does all three.

## enabled.txt

One mod id per line. **Order is priority**: later mods run first on
shared overrides and `base()` descends toward earlier ones. The
in-game mod menu (++o++) can toggle loaded mods live at tick
boundaries; reordering is an `enabled.txt` edit.

`--mods ID[,ID...]` (env `OPENPETE_MODS`) replaces the file for one
run — same order-is-priority rule, no install to edit:

```
openpete-spyro1 --mods hello-anchor,spyro-logo-hd   # exactly these two
openpete-spyro1 --mods ""                           # vanilla, no mods
```

## Distribution

Zip the mod folder with `mod.toml` at the archive root; `<id>.zip` in
`mods/` loads identically to a folder (unpacked and content-cached).
`tools/modget.py` installs from a mod-source JSON manifest with a
mandatory sha256 pin per entry — anyone can host a mod-source; the
hash pins what you get.
