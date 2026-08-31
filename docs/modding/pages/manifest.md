# Manifest and config

## mod.toml

The manifest describes what the mod *is*. Everything except `id`,
`version`, and `api` in the `[mod]` table is optional.

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
purpose = "warp home"     # Players rebind via [keys.mod.<id>] in
                          # openpete.toml without editing the mod.
```

### Test declarations

CI declarations use the same `[test]` schema, but they live in a sibling
`modtest.toml` file that this repository owns. They aren't part of the
manifest format.

``` toml
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

!!! note "`args` takes a string"
    The modtest harness parses line by line and is simple by design.
    Write `args = "--skip-to-level stone_hill"`, not
    `args = ["--skip-to-level", "stone_hill"]`.

## `[[level]]`: custom levels without code

A mod can ship a whole custom level with no C at all: one manifest block
plus two files. openpete gives the mod a section in the **Mods** list of
the M overlay, with one row per level. Each row has a Warp button, a
switch between slot mode and replace mode, and a tooltip carrying the
level's "Custom World" number. Levels from all mods sort alphabetically,
five per world. The engine handles the guest-side plumbing: the
level-slot claim, the WAD table-of-contents declaration, serving your
files when the game streams the level, and restoring everything when the
mod is disabled.

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
title = "GRIM GARDEN"      # optional: in-game title when it differs
                           # from name (a portal-safe A-Z spelling, say)
```

Points to keep in mind:

- `<stem>.WAD` is the level's WAD data segment and can be any size.
  `<stem>.OVL` is the donor's code overlay and must stay the donor's
  stock size. Omit the overlay to run the stock one. These are the
  extensions that level editors emit, and case doesn't matter: `.wad`,
  `.WAD`, and `.Wad` all resolve.
- Set `donor` to the level your hack was authored against.
- Without `music`, the level plays its donor's track. With it, the
  engine plays your WAV file whenever the level starts its music, and
  sizes the game's loop bookkeeping to the file's length. The file loads
  whole into RAM, so a few minutes of 44.1 kHz stereo takes about 50 MB.
- The level's `name` is also its in-game title, shown in the inventory
  rows, on the entry title card, and in the homeworld portal's floating
  letters. In replace mode, it appears in place of the donor's name.
  Every surface, including the portal arc, supports `A-Z`, `0-9`, space,
  and the `.` and `'` characters. Other punctuation, such as `!`, `,`,
  and `?`, works in the inventory and on the title card but renders as
  an apostrophe at the portal. Set `title` to show an in-game string
  that differs from the menu `name`.
- In replace mode, the donor's homeworld portal preview shows your
  level's own skybox, parsed from your `.WAD` file, instead of the stock
  preview baked into the homeworld.
- A mod can declare more than one `[[level]]` block.
- Custom levels share the game's single free level slot, ID 65, until
  the capacity work lands, so per-level progress such as gems and
  dragons is shared between them. Replace mode sidesteps this by taking
  over the donor's own slot.
- Don't commit game-derived data. Ship a derivation script that writes
  `assets/levels/`, such as an xdelta patch applied to the player's own
  disc. See `grim-garden` or `spyro15-cliff-town` for the pattern.

## `[[material]]`: re-shading without code

The other block that needs no C is `[[material]]`, which points a
fragment shader at a subset of the geometry the engine already draws:

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "flame"
ident   = 0                          # the additive plume, not the base caps
params  = [0.30, 0.55, 1.00, 0.60]   # arrives as the shader's std140 block
```

A mod that consists of one manifest and one `.frag` file is a complete,
working mod. See `mods/declarative-reshade`. For the selector keys, the
precedence against C-side registrations, and what these rows can't
reach, see
[Materials](shaders.md#materials-re-shading-the-engines-own-draws).

Rows are display-only: they change color math, never compositing or
gameplay. An unrecognized key is refused rather than ignored, so a typo
can't silently widen what you selected.

## config.toml

`config.toml` sits next to `mod.toml` and is optional. It describes what
the player *wants* from the mod. Players edit it, the engine reads it
immediately before your entry point runs, and typed getters expose it with
defaults from your code:

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

Keys are dotted paths: `"gems.models"` is the key `models` in the table
`[gems]`, and a bare `"verbose"` is a root-level key. A missing file, a
missing key, or a type mismatch yields the default you passed. Ship a
`config.toml` that documents your defaults, but keep the defaults in
your code authoritative. Parse errors are logged and fall back to the
code defaults; they never disable the mod.

Read the config in your entry point and branch there. In development
mode (`OPENPETE_MOD_HOT_RELOAD=1`), the loader watches the content hash
of `config.toml` alongside your sources: an edit hot-reloads the mod
against the cached object, so your entry point re-runs with fresh values
and nothing recompiles.

!!! note "Why not environment variables?"
    Environment variables don't cross process and interop boundaries,
    notably from WSL to Windows. They also aren't discoverable, and they
    can't ship defaults. `config.toml` does all three.

## `[[config]]`: settings the engine renders for you

Declare your settings as schema rows in `mod.toml`, and the engine draws
them in the Mods panel, which opens with the M key. Each row becomes a
checkbox, a slider, or a combo box, grouped and tooltipped. When the
player commits an edit, the engine writes it back into `config.toml`
with the file's comments preserved, then reloads the mod at the next
tick boundary so that your entry point re-reads the fresh values through
the same `config_*` getters. Your mod declares *what* is configurable,
and the engine owns *how it looks*.

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

A mod whose entire UI is settings plus status lines from
`api->ui_status` needs no widget code at all. For a genuinely custom
panel, such as live graphs or tables, use the UI annex in
`openpete_mod_ui.h`, which is versioned separately and carries no
stability promise.

## enabled.txt

`enabled.txt` holds one mod ID per line, and order determines priority:
later mods run first on shared overrides, and `base()` descends toward
earlier ones. The in-game mod menu (++o++) toggles loaded mods live at
tick boundaries. To reorder mods, edit `enabled.txt`.

To replace the file for a single run, pass `--mods ID[,ID...]` or set
`OPENPETE_MODS`. The same order-is-priority rule applies, and you don't
have to edit the install:

```
openpete-spyro1 --mods hello-anchor,spyro-logo-hd   # exactly these two
openpete-spyro1 --mods ""                           # vanilla, no mods
```

## Distribution

Zip the mod folder with `mod.toml` at the archive root. An `<id>.zip`
file in `mods/` loads identically to a folder, because the engine
unpacks it and caches it by content hash.

`tools/modget.py` installs mods from a mod-source JSON manifest that
pins a mandatory sha256 hash per entry. Anyone can host a mod-source,
and the hash pins what you get.
