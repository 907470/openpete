# Publishing

## Ship no game bytes

A mod must not contain data derived from a Spyro disc: no models,
textures, sounds, level payloads, or code overlays, and no patch whose
plaintext is game data. Ship a generator the player runs once against
their own disc image, writing the derived files into `assets/` on their
machine. `mods/recolored-artisans` and `mods/grim-garden` show the
pattern. Original content (a synthesised sound, a painted texture, a
model you built) is yours to ship.

## Carry a licence

Put a `LICENSE` file at the mod root. Code and assets may carry different
licences; say which applies to what. Content by someone else ships only
with that author's recorded permission, credited in the README.

## Pin the API as a minimum

`api = N` is the lowest engine API version the mod needs: the version
that introduced the last entry the mod calls. The engine refuses a mod
that asks for a newer API than it has, before the entry point runs. The
API struct is append-only, so a mod built for api N loads on every later
engine.

## Prefer source

A mod that ships `src/*.c` compiles on the player's machine against the
engine's `sdk/`, with the bundled toolchain on Windows and the system
compiler on Linux. It needs no build on the author's side, works on every
platform the engine ships for, and recompiles when the engine's SDK
changes.

## Prebuilt objects

A mod with no `src/` and a `.build/<id>.so` or `.dll` loads that object
as-is on any platform. A mod with `src/` uses its prebuilt object only
when no compiler resolves; a compile error disables the mod rather than
falling back.

Every object must export the ABI descriptor: compile
`<sdk>/openpete_mod_abi.c` alongside the sources, or put
`OPENPETE_MOD_ABI_DEFINE()` in one of them. The engine refuses an object
with no descriptor, one built for a newer API than the engine, one whose
API struct is larger than the engine's, or one built before the layout
freeze at api 11, and logs both versions.

## Package

A `.zip` with `mod.toml` at its root, plus `src/`, `shaders/`, `assets/`,
`README`, and `LICENSE`. Players drop it into `mods/` unchanged. Do not
zip a wrapping folder, and leave out `.build/` (compile cache) and
`data/` (per-player state).

`tools/modget.py` in the openpete source tree installs mods from a JSON
mod-source listing a zip URL and a mandatory sha256 per entry.
