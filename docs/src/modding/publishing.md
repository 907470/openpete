# Publishing recommendations

## Ship no game bytes

A mod must not contain data derived from a Spyro disc: no models,
textures, sounds, level payloads, or code overlays, and no patch whose
plaintext is game data. Ship a generator the player runs once against
their own disc image, writing the derived files into `assets/` on their
machine. State what the generator needs to run (a shell, python3) and
how a Windows player runs it, from Git Bash or WSL or by hand.
Original content (a synthesised sound, a painted texture, a model you
built) is yours to ship.

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

The pin also selects the overlay-address behaviour and, for a manifest
with no `game` key, the games the mod loads on: raising a pin to 13 for
any other api entry turns the mod's window `override_addr` and ambiguous
raw `override_name` calls into refusals, and a game-less manifest below 13
loads on Spyro 1 only ([Manifest](manifest.md)).

## Prefer source

A mod that ships `src/*.c` compiles on the player's machine against the
engine's `sdk/games/<game>/` (the running game's symbols, wrappers, structs
and `openpete_game.h`) and `sdk/` (the game-neutral headers), with the
bundled toolchain on Windows and the system compiler on Linux, into
`.build/<game>/<id>.so` or `.dll`. It needs no build on the author's side,
works on every platform the engine ships for, and recompiles when the
engine's SDK changes.

## Prebuilt objects

A mod with no `src/` and a prebuilt object loads that object as-is on any
platform. A mod with `src/` uses its prebuilt object only when no compiler
resolves; a compile error disables the mod rather than falling back.

Every object must export the ABI descriptor: compile
`<sdk>/openpete_mod_abi.c` alongside the sources, or put
`OPENPETE_MOD_ABI_DEFINE()` in one of them. The engine refuses an object
with no descriptor, one built for a newer API than the engine, one whose
API struct is larger than the engine's, or one built before the layout
freeze at api 11, and logs both versions.

The descriptor also records the game the object was built for, as
`openpete_mod_game`: the per-game SDK's game when the compile line has
`-I <sdk>/games/<game>` before `-I <sdk>`, `"*"` when it defines
`OPENPETE_MOD_ANY_GAME` (every per-game header then stops the compile
with `#error`), and nothing when the line has `-I <sdk>` alone. A Windows
object links with `-Wl,--export-all-symbols`: one `dllexport` in a source
otherwise turns off the export of every unmarked global, the descriptors
and `openpete_mod_entry` among them. The engine
looks for a prebuilt object in this order and loads the first it finds
only when its game fits:

| path | loads when the object records |
|---|---|
| `.build/<game>/<id>.so` or `.dll` | the running game |
| `.build/any/<id>.so` or `.dll` | `"*"` |
| `.build/<id>.so` or `.dll` | the running game or `"*"`; on Spyro 1 also no game, since objects built before per-game SDKs were built against Spyro 1's |

A `"*"` object reads no per-game header, so `override_addr` and a
`func_`-spelled `override_name` resolve through the running game's
addresses; each such call logs a warning.

## Build by hand

`<sdk>` is the engine's `sdk/` directory and `<game>` one of `spyro-1`,
`spyro-2`, `spyro-3`; build once per game the manifest selects. These are
the engine's own compile flags.

Linux:

``` sh
cc -shared -fPIC -O2 -fno-strict-aliasing -Wall -Wundef \
   -I <sdk>/games/<game> -I <sdk> \
   -o .build/<game>/<id>.so src/*.c <sdk>/openpete_mod_abi.c
```

Windows, with the bundled llvm-mingw or any x86_64 MinGW compiler:

``` sh
x86_64-w64-mingw32-gcc -shared -Wl,--export-all-symbols -O2 \
   -fno-strict-aliasing -Wall -Wundef -static-libgcc \
   -I <sdk>/games/<game> -I <sdk> \
   -o .build/<game>/<id>.dll src/*.c <sdk>/openpete_mod_abi.c
```

The root of `<sdk>` also holds `openpete_sdk_symbols.h`,
`openpete_sdk_wrappers.h` and `openpete_game_structs.h` as dispatchers: a
line with `-I <sdk>` alone reaches them, and each includes the header of
the same name under `<sdk>/games/spyro-1/` unless the line defines
`OPENPETE_GAME_ID` and sets `OPENPETE_GAME_SPYRO2` or
`OPENPETE_GAME_SPYRO3` to 1, which selects `games/spyro-2/` or
`games/spyro-3/`. A line with `-I <sdk>` alone therefore builds a Spyro 1
mod whose object records no game. A line with `-I <sdk>/games/<game>`
first never opens a dispatcher. A packaged SDK carries one game's
directory; a dispatcher that selects a game the package does not carry
stops the compile with `#error` naming the game. A compiler ignores an
`-I` directory that does not exist, so `-I <sdk>/games/spyro-2` against a
Spyro 1 package falls through to the dispatchers and builds for Spyro 1;
`-Wmissing-include-dirs` reports the missing directory.

A `"*"` object for every game (a source with no per-game header, see
[Code mods](code.md#one-mod-for-several-games)) drops the per-game path and
defines `OPENPETE_MOD_ANY_GAME`:

``` sh
cc -shared -fPIC -O2 -fno-strict-aliasing -Wall -Wundef \
   -DOPENPETE_MOD_ANY_GAME -I <sdk> \
   -o .build/any/<id>.so src/*.c <sdk>/openpete_mod_abi.c
```

## Package

A `.zip` with `mod.toml` at its root, plus `src/`, `shaders/`, `assets/`,
`README`, and `LICENSE`. Players drop it into `mods/` unchanged. Do not
zip a wrapping folder, and leave out `.build/` (compile cache, except a
prebuilt object a mod without `src/` ships) and `data/` (per-player
state).
