# Manifest

## mod.toml

`id` and `version` are required. Every other key is optional.

``` toml
[mod]
id      = "my-mod"        # the directory or zip name; enabled.txt and --mods use that name
name    = "My Mod"        # shown in the Mods section; defaults to id
game    = "spyro-1"       # or ["spyro-1", "spyro-3"]; omit = all
version = "0.1.0"
api     = 1               # minimum OPENPETE_MOD_API_VERSION; omit for a mod with no src/
deps    = ["other-mod"]   # hard dependencies: their entry points run first
sim_mutative = false      # see below
state    = "none"         # "none" | "rebuildable": permits rewind with this mod loaded
runahead = "pure"         # "pure" | "host-fx": permits runahead with this mod loaded
```

**`game`.** Which game the mod is for: one id (`"spyro-1"`, `"spyro-2"`,
`"spyro-3"`) or an array of them. `mods/` is shared across the
openpete ports, so a mod that only makes sense under one of them names it
here and the others never see it: a mod naming no game this exe runs is dropped
during discovery, before it is compiled, listed in the Mods panel, or
counted by `enabled.txt`. Omitting the key targets every title, which is
right for a mod built entirely from portable engine surfaces (e.g. shaders).
Ids are matched exactly against the running game's id, so a misspelling
matches nothing and the mod loads nowhere.

**`deps`.** Dependencies are topologically sorted ahead of enabled-list
order, so a library's entry point runs before its clients' regardless of
where each sits in `enabled.txt`. A cycle or a missing dependency is
logged and disables the dependent.

**`sim_mutative`.** Whether the mod writes guest RAM. The engine tags a
memory card with the mutative mods loaded at its first write and warns
the player before playing that card under a different set; progress is
never blocked, and `--adopt-saves` re-tags. A mod with no `src/` and no
`[[level]]` is derived non-mutative. A mod with `src/` is assumed mutative
unless it sets `sim_mutative = false`. Asset overlays, shaders, `draw_text`,
`ui_status`, and `notify` never write guest RAM.

**`state`, `runahead`.** Both default to undeclared. An enabled code mod
with `runahead` undeclared disables runahead; with `state` undeclared it
refuses rewind and timeline jumps, with a one-shot warning. `runahead =
"host-fx"` declares tick-hook side effects outside guest RAM (sounds,
files), which the engine suppresses during speculative ticks. `state =
"rebuildable"` declares that every host-side value derived from guest
history can be re-derived from guest RAM after a jump.

### `[[binding]]`

``` toml
[[binding]]
name    = "warp"          # read with api->binding_down(self, "warp")
key     = "F6"            # default source(s), keymap spelling
purpose = "warp home"     # shown to the player
```

`key` takes the keymap's spelling: a keyboard name, a gamepad source
(`pad:north`, `pad:-lefty`), a mouse button (`mouse:right`), or a
comma-separated list (`"F6,pad:north"`). Wheel
notches and mouse motion are not bindable (the read is a level); a key
that fails to parse is logged at load and reads 0. Conflicts across the
enabled set are reported at load. Players rebind under `[keys.mod.<id>]`
in `openpete.toml` with the same spellings, or `none`. The read is inert
while the dev overlay owns the keyboard, like every host action.

### `[[config]]`

Rows declare the mod's settings. The engine renders them in the M
overlay's Mods section as checkboxes, sliders, inputs, and combo boxes,
grouped and tooltipped. Apply writes the edit into `config.toml` with
comments preserved and reloads the mod at the next tick boundary, so the
entry point re-reads the values through `api->config_*`.

``` toml
[[config]]
key     = "gems.sfx"      # config.toml path this row edits
type    = "bool"          # bool | int | float | string
default = true            # shown when config.toml has no value
label   = "S3 collect chime"
group   = "Gems"          # section header within the mod's block
help    = "tooltip text"

[[config]]
key     = "spawn.scale"
type    = "float"
default = 1.0
min     = 0.25            # min < max renders a slider; omit for an input box
max     = 4.0

[[config]]
key     = "render.mode"
type    = "string"
default = "fast"
items   = ["fast", "fancy"]   # string + items renders a combo
```

A mod whose UI is settings plus `api->ui_status` lines needs no widget
code. Custom panels use `openpete_mod_ui.h`, which is versioned
separately and carries no stability promise.

### `[[level]]` and `[[material]]`

See [Custom levels](levels.md) and
[Shaders](shaders.md#declare-a-material-in-the-manifest).

## config.toml

Optional, beside `mod.toml`. The engine parses it once, immediately before
the entry point runs. Keys are dotted paths: `"gems.models"` is key
`models` in table `[gems]`; a bare `"verbose"` is a root key.

``` toml
[gems]
models = true
sfx    = true

[chests]
collision = true
```

``` c
int     on    = api->config_bool (self, "gems.models", 1);
int64_t count = api->config_int  (self, "spawn.count", 4);
double  scale = api->config_float(self, "spawn.scale", 1.0);
char who[64];   api->config_str  (self, "greet.name", "world", who, sizeof who);
```

A missing file, a missing key, or a type mismatch returns the default
passed in. A parse error is logged and every getter returns its default;
the mod stays enabled. Bools read as 0/1 through `config_int` and
`config_float`; ints read through `config_float`; floats truncate through
`config_int`; strings never coerce.

Read the config in the entry point. An edit to `config.toml` takes effect
on the next launch.

## enabled.txt and --mods

See [Enabling mods](getting-started.md#enabling-mods).
