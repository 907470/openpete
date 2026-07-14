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
auto-invalidates your cache. `sdk/shaders/` also carries the **stock
material shaders** (`psx_scene.vert/.frag`, `quad.vert/.frag`) to fork
for material overrides.

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

`OP_POSTFX_COMPOSITE` runs after the scene+HUD compose, before the
present blit, on **every present** (canonical and sub-tick). Pass order
across mods = enabled-list priority; `order` breaks ties within a mod.
Inputs per the contract: `op_prev_pass` (set 0), `op_scene_depth`
(set 1 — WORLD-opaque pre-pass depth), `op_prev_frame` (set 2,
temporal). The engine block (`op_engine`, set 3 binding 0) carries
time, canonical tick, **interp alpha** (the same value your present
hooks see), resolutions, aspect. Your own block is set 3 binding 1 —
declare it `std140` and mirror the layout in your C struct by hand
(`postfx_set_uniforms` bytes arrive verbatim; 1 KiB cap). See
`mods/crt-filter` for the whole pattern including config.toml-driven
parameters.

## Custom-pipeline draws

```c
openpete_mod_pipeline_desc_t d = {0};
d.vert_shader = vs;  d.frag_shader = fs;
d.vertex_format = OP_VTX_POS_UV_RGBA;
d.blend = OP_BLEND_OPAQUE;  d.depth_test = 1;  d.depth_write = 0;
int pipe = api->pipeline_register(self, &d);
/* ...from a present hook: */
api->draw_mesh(self, pipe, verts, n_verts, texture_id);
```

Vertices are `openpete_mod_vertex_t` (pos/uv/rgba, 24 bytes); your
vertex shader owns the interpretation of `pos` (attributes arrive
untransformed at locations 0..2). Draws land inside the scene pass
after the world — with `depth_test` they z-test against it.
`texture_id` is a `texture_register` id (0 = untextured). Meshes are
per-present: submit from your present hook every frame you want them
visible. `mods/mesh-probe` is the minimal example.

## Material selectors

```c
openpete_mod_material_selector_t sel = { OP_RP_MOBY, /*class*/195, -1, 0 };
api->material_register(self, &sel, /*vert*/-1, frag, /*per_instance*/NULL);
```

Overrides the **shading** of matching engine draws; scheduling (pass
position, depth, stencil, category) stays engine-owned. `-1` on a
shader handle keeps the stock stage — fork
`sdk/shaders/psx_scene.frag` as your starting point (it samples PSX
VRAM through the material push constants; keep that interface).
Current scope: declarative selectors on plain opaque textured runs
(moby class, terrain env-anim/tier); texture-id selectors and the
per-instance callback (state-aware overrides, e.g. only FLAMED chests)
are the next engine milestone and refuse with a log line today.
Conflicts: enabled-list priority, first match wins.

## Verifying your effect (read this before debugging "it doesn't draw")

Composed mod content (postfx, meshes, overlays) exists **only in the
live present**. Two capture paths will lie to you:

- `--screenshot-on-frame`'s `-vk` PNG re-composes the raw scene
  *without* mod compose content;
- `--screenshot-presents` captures the PSX *reference* frame.

Use `--record out.mkv` (reads the true composite; decode a frame with
ffmpeg) or your own eyes on the window. `OPENPETE_GPU_TIMERS=1` prints
a `[gpu-pass]` line where **your postfx passes appear under your mod
id** — if your shader is slow, that's where it gets named; on a GPU
hang (device lost) the log names the last pass begun.
