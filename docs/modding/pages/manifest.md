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
state    = "none"         # host-state posture: "none" (no host state
                          # derived from guest history) or
                          # "rebuildable" (re-derives from guest RAM) —
                          # gates savestate/rewind with mods enabled
runahead = "pure"         # "pure" (tick hooks touch guest state only)
                          # or "host-fx" (host side effects, engine-
                          # suppressed during speculative ticks) —
                          # gates runahead
sim_neutral = true        # observer/presentation-only: never diverges
                          # gameplay state; excluded from the save
                          # modset tag

[[binding]]               # declared host keys — conflicts across the
name    = "warp"          # enabled set are reported at load; resolve
key     = "F6"            # with api->binding_down(self, "warp").
purpose = "warp home"     # Users rebind via [keys.mod.<id>] in
                          # openpete.toml without editing the mod.

# CI declarations live in a SIBLING file this repo owns (modtest.toml,
# same [test] schema) — they are not part of the manifest format.
[test]                    # in modtest.toml, not mod.toml
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
block plus two files. The engine gives the mod a section in the
M-overlay's **Mods** list with a row per level — Warp button,
slot/replace mode switch, and a tooltip carrying its "Custom World"
number (levels from all mods sort alphabetically, five per world) —
and handles all the guest-side plumbing: level-slot claim, WAD TOC
declaration, serving your files when the game streams the level,
restore on disable.

``` toml
[[level]]
name  = "Grim Garden"      # shown in the menu; also the sort key
files = "grim_garden"      # stem: assets/levels/grim_garden.WAD
                           #     + assets/levels/grim_garden.OVL
donor = "dark_hollow"      # whose loader/code the level runs under —
                           # lower_snake name ("cliff_town") or id (22)
replace = true             # optional: serve over the donor's own level
                           # instead of the shared free slot
music = "grim_garden.wav"  # optional: the level's own song, from
                           # assets/music/ (PCM 16-bit, 44100 Hz)
title = "GRIM GARDEN"      # optional: in-game title if it should differ
                           # from name (e.g. a portal-safe A-Z spelling)
```

- `<stem>.WAD` is the level's WAD data segment (any size);
  `<stem>.OVL` is the donor's code overlay (must stay the donor's
  stock size) — omit it to run the stock overlay.
  These are the extensions the level editors emit; the case doesn't
  matter (`.wad`, `.WAD` and `.Wad` all resolve).
- Pick the level your hack was authored against as `donor`.
- Without `music`, the level plays its **donor's** track. With it, the
  engine plays your WAV whenever the level starts its music, and sizes
  the game's loop bookkeeping to the WAV's length. The file loads whole
  into RAM — a few minutes of 44.1 kHz stereo is ~50 MB.
- The level's `name` is also its **in-game title**: the inventory rows,
  the entry title card, and the homeworld portal's floating letters all
  show it (in `replace` mode, in place of the donor's name). Supported
  characters everywhere, portal arc included: `A-Z`, `0-9`, space, and
  `. '` (other punctuation — `! , ?` — works in the inventory and title
  card but shows as an apostrophe at the portal). Set `title` to show a
  different in-game string than the menu `name`.
- In `replace` mode the donor's homeworld **portal preview** also shows
  your level's own skybox (parsed from your `.WAD`) instead of the
  stock preview baked into the homeworld.
- Multiple `[[level]]` blocks per mod are legal.
- Custom levels share the game's one free level slot (id 65) until the
  capacity work lands, so per-level progress (gems/dragons) is shared
  between them — `replace` mode sidesteps that by taking over the
  donor's own slot.
- Don't commit game-derived data: ship a derivation script (an xdelta
  applied to the user's own disc — see `grim-garden` or
  `spyro15-cliff-town` for the pattern) that writes `assets/levels/`.

## `[[material]]` — re-shading, zero code

The other no-C block: a `[[material]]` row points a fragment shader at a
subset of the geometry the engine already draws.

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "flame"
ident   = 0                          # the additive plume, not the base caps
params  = [0.30, 0.55, 1.00, 0.60]   # arrives as the shader's std140 block
```

A mod that is one manifest plus one `.frag` is a complete, working mod —
see `mods/declarative-reshade`. Selector keys, precedence against C-side
registrations, and what is *not* reachable are documented in
[Mod shaders](shaders.md#materials--re-shading-the-engines-own-draws).

Rows are display-only (they change colour maths, never compositing or
gameplay), and an unrecognised key is refused rather than ignored — a
typo must not silently widen what you selected.

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

## `[[config]]` — settings the engine renders for you

Declare your settings as schema rows in `mod.toml` and the engine
draws them in the Mods panel (M key) — checkbox, slider, or combo per
row, grouped and tooltipped. The user's committed edits are written
back into `config.toml` **with their comments preserved**, and the mod
reloads at the next tick boundary so entry re-reads the fresh values
through the same `config_*` getters. The mod declares *what* is
configurable; the engine owns *how it looks*.

``` toml
[[config]]
key     = "gems.sfx"      # the config.toml path this row edits
type    = "bool"          # bool | int | float | string
default = true            # shown when config.toml has no value
label   = "S3 collect chime"
group   = "Gems"          # section header within the mod's block
help    = "Spyro 3 collect chime (canonical flat pitch)"

[[config]]
key   = "spawn.scale"
type  = "float"
default = 1.0
min   = 0.25              # min < max renders a slider;
max   = 4.0               # omit for a free input box

[[config]]
key   = "render.mode"
type  = "string"
default = "fast"
items = ["fast", "fancy"] # string + items renders a combo
```

A mod whose whole UI is settings plus status lines
(`api->ui_status`) needs no widget code at all. For a genuinely
custom panel — live graphs, tables — see the UI annex
(`openpete_mod_ui.h`), which is separately versioned and carries no
stability promise.

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
