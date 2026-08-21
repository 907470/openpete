# Getting started

## Anatomy of a mod

```
mods/<id>/
  mod.toml          manifest — see [Manifest & config](manifest.md)
  config.toml       optional user-editable settings (api->config_*)
  src/*.c           compiled on load against <exe>/sdk/
  assets/           overlay root (textures/replacements/, music/,
                    sounds/, levels/) — pure-asset mods need no src/
  data/             per-mod persistent dir (created on demand)
  test/             self-contained inputs for the [test] block (bk2s)
  .build/           the engine's compile cache (never commit)
```

A `<id>.zip` with `mod.toml` at the archive root loads identically —
it is unpacked and content-cached automatically.

### Where `mods/` is

The mods folder lives wherever the install keeps the files it writes,
because the engine compiles mods in place:

| Install                  | `mods/`                          |
| ------------------------ | -------------------------------- |
| Windows                  | next to `openpete-spyro1.exe`    |
| Linux `.tar.xz`          | `~/.local/share/openpete/mods/`  |
| Linux AppImage           | `~/.local/share/openpete/mods/`  |
| any, `portable = true`   | next to the executable           |

An AppImage is a read-only file, so it is always the per-user path
there. `--mods-dir DIR` (or `$OPENPETE_MODS_DIR`) overrides all of
this — handy for developing against a checkout.

`mods/enabled.txt` lists enabled mods, one id per line — **the order IS
the priority**: mods later in the list run first on shared overrides,
and `base()` descends toward earlier mods, then the game's original
code. To try a different set without editing the install, pass
`--mods hello,other-mod` (same ordering rule; `--mods ""` runs vanilla).

## Your first mod: files, not code

Start declarative. A manifest plus an `assets/` tree is a complete
mod, and it is how most mods should ship — no compile step, no API
version to pin, nothing that can break across engine updates:

- `assets/music/track_20.wav` replaces the title theme
  (see `music-swap`).
- `assets/textures/replacements/<name>.png` replaces a texture in the
  native renderer (see `hd-single-texture`; the texture-dump tooling
  names the files).
- `assets/levels/<stem>.data.bin` + `.ovl.bin` plus a `[[level]]`
  manifest block is a playable custom level — slot claim, warp row and
  disc serving are all engine-side (see `grim-garden`).

Reach for `src/*.c` when you need *behavior*, not content.

## Hello, override chain

`mods/hello/mod.toml`:

``` toml
[mod]
id      = "hello"
name    = "Hello"
version = "0.1.0"
api     = 2
sim_neutral = true   # log-only observer: never touches guest state
```

`mods/hello/src/hello.c`:

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
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
```

Add `hello` to `mods/enabled.txt` and launch. The engine hashes your
sources, compiles them with the bundled toolchain (a splash progress
stage on cold boots, a no-op on warm ones), `dlopen`s the result and
calls your entry. A compile failure disables the mod with the error in
the log — it never bricks the launch.

## The three tiers of reach

1. **Named tier** — `api->override_name(self, "CameraUpdate", fn)`.
   Stable across engine updates; names come from the
   [function reference](reference/functions.md).
2. **Prototyped** — functions with natural signatures get generated
   wrappers: `OP_OVERRIDE_IMPL_Foo`, `op_base_Foo(cpu, ...)`,
   `op_call_Foo(cpu, ...)` with typed args and guest↔host pointer
   marshalling (`openpete_sdk_wrappers.h`).
3. **Raw tier** — `api->override_addr` on any address; args in
   `cpu->a0..a3`, return in `cpu->v0`. Raw addresses are region-pinned
   and can drift across decomp updates — the loader warns once per mod
   on the first raw registration.

## Iterating

- **Hot reload** (dev mode): launch with `OPENPETE_MOD_HOT_RELOAD=1`
  and edits to your sources *or* `config.toml` go live mid-session —
  the loader re-compiles, swaps the chain at a tick boundary, and
  carries your mod's state across via the savestate-participation
  callbacks if you registered them.
- **Testing**: declare a `[test]` block in `mod.toml` and run
  `python3 tools/modtest.py mods/<id>` — the same harness CI uses
  (`scripts/check_mods.sh` runs the whole ladder).
- **Logs**: `api->log` lines are tagged `mod:<id>`. Crashes are
  attributed to the innermost mod on the override stack.

## Rules of the road

- **Sim-thread only**: guest reads/writes, `call`, registrations —
  all belong in tick context (inside overrides or entry). Present hooks
  are render-pure and the loader *enforces* it.
- **Divergence is a choice, not an accident.** If your mod perturbs
  gameplay-readable state, that's fine (say so: omit `sim_neutral`) —
  but observers should stay byte-neutral, and the PSX oracle gate can
  prove either way (`expect_oracle = "pass"` in your `[test]`).
- **No game data in the repo/zip.** Ship generators, not rips.
