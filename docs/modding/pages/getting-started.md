# Getting started

## Anatomy of a mod

A mod is a directory under `mods/` named for the mod's ID:

| Path | Contents |
|---|---|
| `mod.toml` | The manifest. See [Manifest and config](manifest.md). |
| `config.toml` | Optional player-editable settings, read through `api->config_*`. |
| `src/*.c` | C sources, compiled on load against `<exe>/sdk/`. |
| `assets/` | Overlay root: `textures/`, `music/`, `sounds/`, and `levels/`. An asset-only mod needs no `src/`. |
| `data/` | Per-mod persistent directory, created on demand. |
| `.build/` | The engine's compile cache. Don't commit it. |

A `<id>.zip` archive with `mod.toml` at its root loads identically. The
engine unpacks it and caches it by content hash.

### Where the mods directory lives

The engine compiles mods in place, so `mods/` sits wherever the install
keeps the files it writes:

| Install | Location of `mods/` |
|---|---|
| Windows | Next to `openpete-spyro1.exe` |
| Linux `.tar.xz` | `~/.local/share/openpete/mods/` |
| Linux AppImage | `~/.local/share/openpete/mods/` |
| Any install with `portable = true` | Next to the executable |

An AppImage is a read-only file, so it always uses the per-user path. To
point the engine somewhere else, such as a development checkout, pass
`--mods-dir DIR` or set `OPENPETE_MODS_DIR`. Either one overrides all of
the preceding locations.

### How mods are enabled and ordered

`mods/enabled.txt` lists the enabled mods, one ID per line. Order
determines priority: mods later in the list run first on shared
overrides, and `base()` descends toward earlier mods and then to the
game's original code.

To run a different set for a single session without editing the install,
pass `--mods hello,other-mod`. The same ordering rule applies, and
`--mods ""` runs the game without mods.

## Create an asset-only mod

Start declarative. A manifest plus an `assets/` tree is a complete mod,
and it's how most mods should ship: no compile step, no API version to
pin, and nothing that can break across engine updates.

The engine picks up assets by convention:

- `assets/music/track_20.wav` replaces the title theme. See
  `music-swap`.
- `assets/textures/<name>.png` replaces a texture in the
  native renderer. See `hd-single-texture`. The texture-dump tooling
  names these files for you.
- `assets/levels/<stem>.WAD` and `<stem>.OVL`, plus a `[[level]]`
  manifest block, make a playable custom level. The engine handles the
  slot claim, the warp row, and disc serving. See `grim-garden`.

Reach for `src/*.c` only when you need behavior.

## Create a code mod

The following mod logs a line every few seconds from inside the override
chain.

1.  Write the manifest to `mods/hello/mod.toml`:

    ``` toml
    [mod]
    id      = "hello"
    name    = "Hello"
    version = "0.1.0"
    api     = 2
    sim_neutral = true   # log-only observer: never touches guest state
    ```

2.  Write the source to `mods/hello/src/hello.c`:

    ``` c
    #include <openpete_mod_api.h>

    static const openpete_mod_api_t* A;
    static openpete_mod_t* SELF;
    static unsigned calls;

    static void on_camera_update(CPUState* cpu) {
        if ((++calls % 300u) == 0u)   /* ~every 10 s at 29.913 ticks/s */
            A->log(SELF, OP_MOD_LOG_INFO, "CameraUpdate call #%u", calls);
        A->base(cpu);                 /* pre-hook: us first, original last */
    }

    int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
        A = api; SELF = self;
        return api->override_name(self, "CameraUpdate", on_camera_update);
    }
    ```

3.  Add `hello` to `mods/enabled.txt`.

4.  Launch the game.

The engine hashes your sources, compiles them with the bundled toolchain
(a splash progress stage on cold boots, and a no-op on warm ones), calls
`dlopen` on the result, and runs your entry point. A compile failure
disables the mod and logs the error. It never blocks the launch.

!!! note "Don't check the API version in code"
    The `api` key in `mod.toml` is the *minimum* version your mod needs,
    and the loader refuses an older engine before your entry point runs.
    Never compare versions with `!=`: the API struct is append-only, so
    an engine newer than your pin is exactly what you want. If a single
    call needs a later version than the rest of the mod, guard that call
    alone with `if (api->api_version >= N)`.

## Choose a way to hook game functions

Three tiers reach the game's code, from most stable to most precise:

1.  **By name.** `api->override_name(self, "CameraUpdate", fn)` is
    stable across engine updates. The names come from the
    [function reference](reference/functions.md).
2.  **Through a prototype.** Functions with natural signatures get
    generated wrappers in `openpete_sdk_wrappers.h`:
    `OP_OVERRIDE_IMPL_Foo`, `op_base_Foo(cpu, ...)`, and
    `op_call_Foo(cpu, ...)`, with typed arguments and guest-to-host
    pointer marshalling.
3.  **By address.** `api->override_addr` reaches any address. Arguments
    arrive in `cpu->a0` through `cpu->a3`, and the return value goes in
    `cpu->v0`. Raw addresses are region-pinned and can drift across
    decompilation updates, so the loader warns once per mod on the first
    raw registration.

## Iterate on a mod

**Hot reload.** Launch with `OPENPETE_MOD_HOT_RELOAD=1`, and edits to
your sources or to `config.toml` go live mid-session: the loader
recompiles and swaps the chain at a tick boundary. Your mod's own
variables don't survive the swap, because your entry point re-runs on
the fresh object. Re-derive whatever you need from guest state there.

**Testing.** `python3 tools/modtest.py mods/<id>` runs a mod against a
declared scenario, using the same harness that CI uses.
`scripts/check_mods.sh` runs the whole ladder. The declarations live in
a `modtest.toml` file beside `mod.toml`. See
[Manifest and config](manifest.md).

**Logs.** Lines from `api->log` carry the tag `mod:<id>`. Crashes are
attributed to the innermost mod on the override stack.

## Follow these rules

- **Do sim work on the sim thread only.** Guest reads and writes,
  `call`, and registrations all belong in tick context, which means
  inside overrides or your entry point. Present hooks are render-pure,
  and the loader enforces it.
- **Make divergence a choice, not an accident.** Perturbing
  gameplay-readable state is fine, as long as you declare it by omitting
  `sim_neutral`. Observers should stay byte-neutral, and the PSX oracle
  gate can prove it either way through `expect_oracle = "pass"` in your
  `modtest.toml`.
- **Keep game data out of your repository and your zip.** Ship
  generators, not rips.
