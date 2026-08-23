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
| Material selectors | `[[material]]` rows, or `material_register` | Re-shade a *subset* of the engine's own draws (a moby class, the LP terrain tier, the flame plume). The manifest form needs no C at all |

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

## Materials — re-shading the engine's own draws

A post-process pass sees the finished frame. A **material** goes a layer
deeper: it replaces the fragment shading of a *subset* of the geometry
the engine already draws — one moby class, the LP terrain tier, the
flame plume, the sky behind a portal.

The seam is: **the CPU chooses the audience, the shader performs.** You
describe which geometry you mean with a *selector*; the engine decides
per draw whether your shader runs. Your shader never receives game
structs and never decides whether it applies.

**Reshade-only.** A material inherits its group's blend mode, depth
policy and painter order, and changes colour maths only. It cannot move,
reorder or re-composite anything — which is why materials are display-only
and cannot affect gameplay. On an additive draw your output is still
*added*, so a bright tint adds more light rather than replacing it.

### The zero-C tier

If all you want is "shade this with that", the manifest is the whole mod
— no `src/`, no entry point, nothing compiled:

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "player"
params  = [1.0, 0.55, 0.15, 0.45]    # arrives as your std140 block
```

Selector keys, all optional — omit one and it matches everything:

| Key | Meaning |
|---|---|
| `channel` | `any`, `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, `portal` |
| `ident` | channel-local identity: flame ribbon/caps, particle kind, shadow caster kind, portal preview rung |
| `class` | moby class |
| `texture` | terrain Tiledef id, 0..127 |
| `level` | level id — sector and texture ids are per-level, so a keyed row usually wants this too |
| `tier` | `"hp"` or `"lp"`; omit for both |
| `textured` | `true` or `false`; omit for both |
| `env_anim` | `true` to take only animated terrain env faces |
| `params` | floats handed to your shader as its uniform block |
| `enabled` | `false` ships the row switched off |

Narrowing keys are **narrowing**: leaving `tier` out means *both* tiers,
not neither. A key the engine does not recognise is refused with an error
rather than ignored — a typo that silently widened your selector would
repaint geometry you never asked for.

`mods/declarative-reshade` is a complete working example: a manifest and
one `.frag`.

### The C tier

For selection that depends on runtime state, register from `entry`:

```c
openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
sel.channel    = OP_CHAN_MOBY;
sel.moby_class = 42;
int mat = api->material_register(self, &sel, frag);
api->material_set_params(self, mat, &block, sizeof block);   /* std140 */
api->material_enable(self, mat, 1);
```

Always build the selector with `openpete_mod_material_sel_any()` and
assign fields — never zero-initialise it. The struct carries its own size
and flag mask, and the engine refuses a selector it cannot honour exactly
rather than matching wider than you asked.

Manifest rows arm *before* `openpete_mod_entry`, so a mod that uses both
tiers has its C registrations land later — and later wins. Where two
materials claim the same pixels, the last registration takes them.

Material fragment shaders include `openpete_psx_material.glsl` and read
their own block at set 3 binding 1. `op_pack_shade()` is the usual base
layer: it decodes the face the way the engine would, honours an installed
HD texture pack, and returns the interpolated vertex colour for
untextured draws — so one shader serves both paths.

`mods/channel-gallery` registers the same shader once per channel with a
different selector each time, with live toggles. It is the fastest way to
see what a given selector actually claims — including which things are
*not* separately addressable yet.

### Refine — deciding per instance

A selector prunes cheaply; a refine callback decides the rest. It runs
during extraction, in tick context, and returns that instance's uniform
block or `-1` to leave it stock:

```c
static int refine(const openpete_mod_material_key_t* key,
                  void* out, uint32_t cap) {
    const Moby* m = (const Moby*)api->guest(key->u.moby.vaddr);
    if (!m || m->m_State != BURNING) return -1;      /* decline */
    float* v = out; v[0] = 1.0f; v[1] = 0.4f; v[2] = 0.0f; v[3] = 1.0f;
    return 16;
}
...
api->material_register_refine(self, &sel, frag, refine);
```

That is how "class 42, but only while it is burning" is expressed: the
selector narrows to class 42, the callback declines the rest. Scope the
selector as tightly as you can first — the callback is the expensive
half.

**The engine promises SEMANTICS, never CADENCE.** Your verdict is the one
the material carries; how often you are asked for it is the engine's
business. Results are interned, and a channel that later banks geometry
across presents re-resolves from identity rather than calling back. So a
callback must be a pure function of its key plus tick-stable guest state,
and a counter inside one measures the engine's memoization, not the
scene.

Instances whose blocks are byte-identical share a slot and stay in one
batch, so cost tracks *distinct outcomes*, not matches.

> **Current limitation:** a callback returning many distinct blocks
> currently loses most of its instances — they fall back to stock
> shading. Callbacks that return a small set of shared blocks are
> unaffected. `mods/moby-instance-tint` reproduces it and its manifest
> carries the diagnosis.

### What is not reachable

Being tagged does not make geometry reshadeable. A material replaces the
fragment stage, so a draw path whose fragment stage does something other
than colour maths cannot accept one. Moby drop-shadows are the current
example: they draw through a depth-aware decal pass whose fragment shader
owns per-pixel occlusion against the world, so it is not yours to replace.
The gallery marks such cases as known gaps rather than hiding them.
