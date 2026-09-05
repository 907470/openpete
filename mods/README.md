# openpete mods

Every subdirectory here with a `mod.toml` is a mod; an `<id>.zip` with
`mod.toml` at its root loads the same way. `enabled.txt` lists the
enabled mods, one ID per line, and order is priority: a mod listed later
runs first on a shared override and `base()` descends toward earlier
mods, then the original game code. `--mods a,b` or `OPENPETE_MODS`
replaces the list for one run; `--mods ""` enables none.

Mods are native programs. A mod's C compiles into the game process and
runs with its privileges; there is no sandbox and no signing. Install
only mods you trust. A mod with no `src/` directory never loads code.

Mods must not ship data derived from a Spyro disc. Ship a generator that
derives the assets from the player's own disc; `recolored-artisans`
(source tree only) shows the pattern.

## Documentation

The mod-author documentation (guides plus the generated API, function,
global, and struct reference) builds with the game:

```
cmake --build --preset linux-release --target mod_docs
```

The site lands in `build/<preset>/games/spyro-1/moddocs/site/index.html`.
`scripts/build-mod-docs.sh --serve` runs a live preview (needs
`pip install mkdocs-material`). Sources: `docs/modding/`.

## In-tree mods

`examples.txt` names the mods `scripts/package-dist.sh --with-mod-sdk`
stages into `<exe>/sdk/examples/`; the documentation's Examples page
lists exactly those. Every entry ships only original content.

In-tree mods: `declarative-example`, `music-swap`,
`hd-single-texture`, `sfx-single-sound`, `declarative-reshade`,
`recolored-artisans`, `grim-garden`, and `spyro15-cliff-town` are
declarative; `hello-anchor`, `double-gems`, `natural-gems`,
`hud-stopwatch`, `crt-filter`, `crt-beans`, `scene-sepia`,
`channel-gallery`, `moby-instance-tint`, `egg-hud-3d`, `teapot-dragons`,
`crash3-models`, `crystalfissure-dark-artisans`, `supercharge-anywhere`,
and `spyro3` ship code.

CI fixtures, not examples: `arena-canary`, `arena-probe`, `chain-duo-a`,
`chain-duo-b`, `crash-dummy`, `demo-lib`, `lib-and-client`, `raw-probe`,
`sfx-replace-probe`, `vstream-rehandle`. `scripts/check_mods.sh` runs
every mod's `modtest.toml`; `python3 tools/modtest.py mods/<id>` runs
one. A mod whose assets are derived locally from a disc and never
committed lists them in `requires`, and the runner skips it by name when
they are absent instead of passing or failing.

`tools/modget.py` installs mods from a JSON mod-source that names a zip
URL and a mandatory sha256 per entry.
