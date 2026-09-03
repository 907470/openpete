# Publishing a mod

What a mod has to get right before it is handed to other players.

## Ship no game bytes

A mod must not contain data derived from a Spyro disc: no models,
textures, sounds, level payloads or code overlays ripped from the game,
and no diff whose plaintext is game data. Ship a generator instead: a
tool the player runs once against their own disc image, writing the
derived files into `assets/` on their machine. `recolored-artisans`
shows the pattern. Original
content you made, such as a synthesized sound, a painted texture or a
model you built, is yours to ship.

`.build/` is the engine's compile cache and never belongs in a release;
`data/` is per-player state.

## Carry a licence

Put a `LICENSE` file at the mod root and name the licence in `mod.toml`
comments. Code and assets can carry different licences; say which
applies to what. Content by someone else, such as a texture pack or a
level hack, ships only with that author's recorded permission, credited
in the README.

## Pin the API as a minimum

`api = N` in `mod.toml` is the lowest engine API version the mod needs.
The engine refuses to load a mod that asks for a newer API than it has,
before the entry point runs. The API struct is append-only, so a mod
built for api N loads on every later engine. Set N to the version that
introduced the last entry the mod calls, and never compare versions in
code.

## Prefer source

A mod that ships `src/*.c` compiles on the player's machine against the
engine's own `sdk/`, with the bundled toolchain on Windows and the
system compiler or the bundled one on Linux. That is the supported
release form: it needs no build on the author's side, works on every
platform the engine ships for, and re-compiles automatically when the
engine's SDK changes.

## If you ship a prebuilt object

A prebuilt `.build/<id>.so` or `.dll` is loaded only when no compiler
resolves, which happens on Windows with no toolchain next to the exe and
no `OPENPETE_MOD_CC`; on Linux the system `cc` always resolves. A
compile error disables the mod rather than falling back. Every such object must export the ABI
descriptor: compile `<sdk>/openpete_mod_abi.c` alongside the sources, or
put `OPENPETE_MOD_ABI_DEFINE()` in one of them. The engine refuses an
object with no descriptor, one built for a newer API than the engine,
or one whose API struct is larger than the engine's, and logs both
versions. An object built for an older API loads. The `state` and
`runahead` postures declared in `mod.toml` apply to a prebuilt object
exactly as to a compiled one.

## Test before release

`tools/modtest.py` runs the `[test]` blocks of a `modtest.toml` next
to `mod.toml` headlessly: a boot with the mod enabled, the log lines it
must emit, and the oracle comparison that proves a `sim_mutative =
false` mod leaves gameplay state byte-identical to the original game.
See [Manifest and config](manifest.md) for the schema.

## What to package

A `.zip` with `mod.toml` at its root, plus `src/`, `assets/`, `README`
and `LICENSE`. Players drop it into `mods/` unchanged; the engine unpacks
and caches it by content hash. Do not zip a wrapping folder.
