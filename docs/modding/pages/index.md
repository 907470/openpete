# openpete modding

A mod is a directory or `.zip` under `mods/` holding a `mod.toml`
manifest and any of: replacement assets, GLSL shaders, and C sources that
the engine compiles on load against the SDK next to the executable.

!!! warning "Mods are native programs"

    C sources compile into the game process and run with its privileges.
    There is no sandbox and no signing. Install only mods you trust. A mod
    with no `src/` directory never loads code.

## Declarative mods

Everything a mod expresses through `mod.toml` and files:
replacement textures, music, sounds, custom levels, shader materials,
player settings, and hotkeys. Nothing compiles, so there is no ABI and no
API version to track. This mod is complete:

```
mods/my-music/
  mod.toml
  assets/music/track_20.wav
```

```toml
[mod]
id      = "my-music"
version = "1.0.0"
```

## Code mods

C under `src/` reaches the game's functions and state through
`openpete_mod_api.h`. The engine compiles the sources at boot with the
bundled toolchain and hooks game functions through a per-function
override chain:

```c
#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;

static void on_camera_update(CPUState* cpu) {
    g_api->log(g_self, OP_MOD_LOG_INFO, "CameraUpdate");
    g_api->base(cpu);              /* run the original */
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
```

A mod may use both tiers. `[[config]]` and `[[binding]]` rows give a code
mod engine-rendered settings and rebindable keys with no UI code.

## Pages

- [Getting started](getting-started.md): install location, enabling,
  directory layout, a first mod of each tier, iteration.
- [Manifest](manifest.md): every `mod.toml` key, `config.toml`,
  `enabled.txt`.
- [Code mods](code.md): the override chain, tick and present contexts,
  guest memory, services.
- Assets: [textures and music](assets.md), [sounds](sounds.md),
  [custom levels](levels.md).
- [Shaders](shaders.md): post-process passes and material selectors.
- [Publishing](publishing.md): what a mod must get right before release.
- Reference, generated from the SDK sources: [API](reference/api.md),
  [functions](reference/spyro-1/functions.md),
  [globals](reference/spyro-1/globals.md),
  [game structs](reference/spyro-1/structs.md),
  [CPUState](reference/cpustate.md),
  [examples](reference/examples.md).
