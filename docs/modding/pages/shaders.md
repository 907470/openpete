# Mod shaders

Mods ship **GLSL source**; the engine compiles it at load with the same
compiler and dialect (`vulkan1.0`) that builds its own shaders — no
shader toolchain to install, on any platform. Compiles are content-hash
cached under `<mod>/.build/shaders/`; a broken shader is a log line
plus an unavailable effect, never a failed boot. Shaders are GPU-only:
they **cannot touch guest RAM**, so every tier below is sim-neutral by
construction — your C side may *read* gameplay state to feed uniforms,
and nothing flows back.

Three tiers, cheapest coupling first:

| Tier | API | What it does |
|---|---|---|
| Post-process | `shader_register` + `postfx_register`/`postfx_enable`/`postfx_set_uniforms` | Fullscreen fragment passes over the composed frame (CRT, grading, scanlines) |
| Custom draws | `pipeline_register` + `draw_mesh` | Your geometry, your vert+frag, drawn into the scene from a present hook |
| Material selectors | `material_register` | Re-shade a *subset* of the engine's own draws (a moby class, the env-animated tiles) |

## Registering shaders

```c
int frag = api->shader_register(self, "shaders/crt.frag");   /* stage from extension */
```

Paths are mod-relative; stage comes from `.vert`/`.frag`. Keep shaders
under `shaders/` — that directory joins the dev hot-reload watch, so an
edit recompiles and reloads in-session (`OPENPETE_MOD_HOT_RELOAD=1`).
Compile errors land in `.build/shaders/<name>.log` and return -1 —
degrade gracefully, don't fail entry.

Include the binding contract from any shader:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1     /* postfx frags only: brings op_uv/op_color */
#include "openpete_shader_api.glsl"
```

The engine resolves it to `sdk/shaders/openpete_shader_api.glsl` and
hashes those bytes into every shader's build key — an interface change
auto-invalidates your cache. **Material** fragment shaders include
`openpete_psx_material.glsl` instead (never both — they claim the same
descriptor set differently): it carries the material-tier interface plus
the stock PSX decode as functions (`op_psx_shade()` = faithful stock
result) and HD texture-pack access (`op_pack_shade()` = the scene with
the installed pack applied), so a material builds ON the faithful base
rather than forking it. Forks of `sdk/shaders/psx_scene.frag` still work but freeze the
decode they copied; include-based materials are rebuilt by the engine
whenever the contract evolves.

Shipping without source: a prebuilt `<path>.spv` loads when no source
sits beside it, but requires a `<path>.spv.ihash` sidecar matching the
current interface hash — a stale prebuilt refuses cleanly instead of
misrendering.

## Post-process passes

```c
int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, /*order*/0);
api->postfx_set_uniforms(self, fx, &params, sizeof params);   /* copied; any context */
api->postfx_enable(self, fx, 1);
```

Two injection points:

- `OP_POSTFX_COMPOSITE` runs after the scene **and HUD** are composed,
  before the present blit — the whole frame, including the HUD.
- `OP_POSTFX_SCENE` runs after the 3D scene but **before the HUD**, so
  effects (grading, fog) tint the world while the HUD stays crisp on
  top. It also gets real scene depth at `op_scene_depth`. (`PRESENT`,
  post-blit, is future — identical to COMPOSITE until an upscaler
  exists.)

Both run on **every present** (canonical and sub-tick). Pass order
across mods = enabled-list priority; `order` breaks ties within a mod.
Inputs per the contract: `op_prev_pass` (set 0), `op_scene_depth`
(set 1 — the WORLD-opaque pre-pass depth, most meaningful at the SCENE
point; note it covers opaque terrain, not mobys), `op_prev_frame`
(set 2, temporal). The engine block (`op_engine`, set 3 binding 0)
carries time, canonical tick, **interp alpha** (the same value your
present hooks see), resolutions, aspect. Your own block is set 3
binding 1 — declare it `std140` and mirror the layout in your C struct
by hand (`postfx_set_uniforms` bytes arrive verbatim; 1 KiB cap). See
`mods/crt-filter` (COMPOSITE, config-driven) and `mods/scene-sepia`
(SCENE, under-HUD) for the whole pattern.

## Deeper tiers (parked)

Custom-pipeline draws and material selectors — mod geometry through
the native renderer, and re-shading subsets of the engine's own draws
— existed as api-v0 experiments and are parked while the renderer's
material and depth model settle. Their engine machinery survives; the
surfaces return as append-only pillars once stable. The postfx chain
above is the supported shader tier.
