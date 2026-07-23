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
result), so a material builds ON the faithful base rather than forking
it. Forks of `sdk/shaders/psx_scene.frag` still work but freeze the
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
shader handle keeps the stock stage. Write the fragment stage on
`openpete_psx_material.glsl` and start from `op_psx_shade()` — the
faithful stock result for textured AND untextured draws (`op_pc.bpp`
is `0` on untextured ones, where it returns the gouraud vertex
colour):

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"
layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c = op_psx_shade();
    if (c.a == 0.0) discard;              /* PSX transparent sentinel */
    outColor = vec4(c.rgb * tint.rgb, c.a);
}
```

Applies to plain opaque runs — textured and untextured (flat/gouraud)
alike — for moby classes and terrain (env-anim/tier refinements;
`OP_SEL_UNTEXTURED` scopes a terrain selector to the beyond-LOD
gouraud band, whose per-instance callback receives `moby_vaddr` 0).
Conflicts resolve by enabled-list priority, first match wins.

### Per-instance callback (state-aware)

Pass a callback as the last argument to shade *specific instances* by
their live gameplay state — the "only FLAMED metal chests" case:

```c
static int flamed_chest(CPUState* cpu, uint32_t moby, void* out, uint32_t cap) {
    if (op_read_u8(g_api, moby + 0x4E) == 0) return -1;  /* not flamed → decline */
    float* rgb = out;                                    /* → stock shading    */
    rgb[0] = 1.0f; rgb[1] = 0.3f; rgb[2] = 0.0f; rgb[3] = 1.0f;
    return 16;                                           /* bytes of uniform   */
}
openpete_mod_material_selector_t sel = { OP_RP_MOBY, /*class*/195, -1, 0 };
api->material_register(self, &sel, -1, frag, flamed_chest);
```

The callback runs at extraction per matching moby instance (guest RAM
readable through `cpu`/`op_read_*`); return the bytes of a per-draw
uniform block (bound at set 3 binding 1, `<= 256` bytes) or `-1` to
leave that instance stock. Distinct outcomes land in distinct batches
automatically. Read-only: the callback must not write guest state.

Cost: each distinct outcome is one extra draw (it splits the batched
scene). Scope the selector to a class + a *rare* state so the common
case stays cheap; the `[gpu-pass]` / draw-count instruments name the
cost. Uniforms are recomputed every present (the callback reads
tick-stable guest RAM, so results don't strobe across sub-tick
frames). See `mods/moby-instance-tint` for a working example.

Texture-id selectors (`sel.texture`) are not yet available — use `-1`.

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
