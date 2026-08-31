# openpete modding

openpete is a PC port of Spyro the Dragon whose non-negotiable floor is
gameplay that stays bit-identical to original PlayStation hardware. Its
mod framework is designed to reach deeper than any other unofficial port
of the game.

## Mod with files, not code

The primary road needs no C at all. The following mod is complete and
working:

```
mods/my-music/
  mod.toml
  assets/music/track_20.wav
```

``` toml
[mod]
id      = "my-music"
name    = "My title theme"
version = "1.0.0"
api     = 1
```

Add `my-music` to `mods/enabled.txt`, and the title screen plays your
track. Texture replacements, sound packs, and whole custom levels
(`[[level]]`) work the same way: a manifest plus files under `assets/`,
picked up by convention. An asset-only mod has no ABI, so nothing about
it can break when the engine updates.

## Mod with code

When you need behavior rather than content, write plain C. The engine
compiles it on load against a generated SDK, and hooks the game through
a per-function override chain that intercepts every call without
patching any code:

``` c
#include <openpete_mod_api.h>

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

static void on_camera_update(CPUState* cpu) {
    A->log(SELF, OP_MOD_LOG_INFO, "hello from the override chain");
    A->base(cpu);              /* fall through to the original */
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; SELF = self;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
```

Save that as `mods/hello/src/hello.c`, add a three-line `mod.toml`, and
the engine compiles and loads it at boot with the bundled toolchain, on
every supported platform.

## What to read next

- [Getting started](getting-started.md): the anatomy of a mod, the build
  and load pipeline, testing, and hot reload.
- [Concepts](concepts.md): the override chain, tick and present
  contexts, sim-neutrality and the oracle, and guest memory.
- [Manifest and config](manifest.md): every `mod.toml` key, plus
  player-facing settings in `config.toml`.
- Reference, generated from the same sources the compiler sees: the
  [API](reference/api.md), the
  [overridable functions](reference/functions.md),
  [globals](reference/globals.md),
  [game structs](reference/structs.md), and the
  [example gallery](reference/gallery.md).

## Before you install a mod

!!! warning "Mods are native programs"
    A mod's C source compiles and loads into the game process with the
    same privileges as the game itself. There is no sandbox and no
    signing. Install only mods that you trust, exactly as you would any
    other program.

openpete names the offending mod in a crash report, names slow mods in
the per-mod time instrument, and warns you when a memory card was played
under a different set of mods. None of that is a security boundary.

Mods must not ship game-derived data, such as models, textures, or audio
ripped from a Spyro disc. Ship a generator that derives those assets
from the player's own disc instead. For the pattern, see
`recolored-artisans` and `spyro3` in the
[example gallery](reference/gallery.md).

## How the reference stays accurate

The build regenerates the reference section whenever the API header, the
symbol manifests, or the mod gallery change. `tools/gen_mod_docs.py`
performs the extraction, and the `mod_docs` CMake target runs it. The
prose in those pages comes from the same headers your mod compiles
against, so there is no second copy of the truth to fall out of date.
