# Mod shaders

Mods ship GLSL source, and the engine compiles it at load with the same
compiler and dialect (`vulkan1.0`) that builds its own shaders. There is
no shader toolchain to install on any platform. Compiles are cached by
content hash under `<mod>/.build/shaders/`, and a broken shader produces
a log line and an unavailable effect rather than a failed boot.

Shaders run on the GPU only, so they can't touch guest RAM. Every tier
described on this page is therefore sim-neutral by construction: your C
code may read gameplay state to feed uniforms, and nothing flows back.

Two tiers are available, listed here from the loosest coupling to the
tightest:

| Tier | API | What it does |
|---|---|---|
| Post-process | `shader_register` with `postfx_register`, `postfx_enable`, and `postfx_set_uniforms` | Runs fullscreen fragment passes over the composed frame, such as CRT effects, grading, and scanlines |
| Material selectors | `[[material]]` rows, or `material_register` | Re-shades a subset of the engine's own draws, such as a moby class, the LP terrain tier, or the flame plume. The manifest form needs no C at all |

Drawing your own geometry isn't among them. A mod shades what the engine
already draws, and can't add draws of its own.

## Register a shader

```c
int frag = api->shader_register(self, "shaders/crt.frag");   /* stage from extension */
```

Paths are relative to the mod, and the stage comes from the `.vert` or
`.frag` extension. Keep your shaders under `shaders/`, because that
directory joins the development hot-reload watch: with
`OPENPETE_MOD_HOT_RELOAD=1`, an edit recompiles and reloads in-session.
Compile errors land in `.build/shaders/<name>.log` and return `-1`.
Degrade gracefully rather than failing your entry point.

Include the binding contract from any shader:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1     /* postfx frags only: brings op_uv/op_color */
#include "openpete_shader_api.glsl"
```

The engine resolves that include to `sdk/shaders/openpete_shader_api.glsl`
and hashes those bytes into every shader's build key, so an interface
change invalidates your cache automatically.

Material fragment shaders include `openpete_psx_material.glsl` instead.
Never include both, because they claim the same descriptor set in
different ways. Forks of `sdk/shaders/psx_scene.frag` still work, but
they freeze the decode they copied, whereas the engine rebuilds
include-based materials whenever the contract evolves.

To ship without source, put a prebuilt `<path>.spv` file where no source
sits beside it. It also needs a `<path>.spv.ihash` sidecar file matching
the current interface hash, so a stale prebuilt refuses cleanly instead
of rendering incorrectly.

## Add a post-process pass

```c
int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, /*order*/0);
api->postfx_set_uniforms(self, fx, &params, sizeof params);   /* copied; any context */
api->postfx_enable(self, fx, 1);
```

Two injection points are available:

- `OP_POSTFX_COMPOSITE` runs after the scene and the HUD are composed,
  before the present blit, so it sees the whole frame including the HUD.
- `OP_POSTFX_SCENE` runs after the 3D scene but before the HUD, so
  effects such as grading and fog tint the world while the HUD stays
  crisp on top. This point also receives real scene depth at
  `op_scene_depth`.

A third point, `PRESENT`, runs after the blit. It's reserved for future
use and is identical to `COMPOSITE` until an upscaler exists.

Both available points run on every present, canonical and sub-tick. Pass
order across mods follows enabled-list priority, and `order` breaks ties
within a single mod.

The contract provides three inputs: `op_prev_pass` in set 0,
`op_scene_depth` in set 1, and `op_prev_frame` in set 2 for temporal
effects. `op_scene_depth` is the world-opaque pre-pass depth and is most
meaningful at the `SCENE` point; it covers opaque terrain, not mobys.
The engine block, `op_engine` at set 3 binding 0, carries time, the
canonical tick, the interpolation alpha (the same value your present
hooks see), the resolutions, and the aspect ratio.

Your own block is set 3 binding 1. Declare it `std140` and mirror the
layout in your C struct by hand: the bytes you pass to
`postfx_set_uniforms` arrive verbatim, with a 1 KiB cap. For the whole
pattern, see `mods/crt-filter` (a config-driven `COMPOSITE` pass) and
`mods/scene-sepia` (a `SCENE` pass under the HUD).

## Materials: re-shading the engine's own draws

A post-process pass sees the finished frame. A material goes a layer
deeper and replaces the fragment shading of a subset of the geometry the
engine already draws: one moby class, the LP terrain tier, the flame
plume, or the sky behind a portal.

The seam is that the CPU chooses the audience and the shader performs.
You describe the geometry you mean with a *selector*, and the engine
determines per draw whether your shader runs. Your shader never receives
game structs, and never determines whether it applies.

!!! note "Materials re-shade only"
    A material inherits its group's blend mode, depth policy, and
    painter order, and changes color math only. It can't move, reorder,
    or re-composite anything, which is why materials are display-only
    and can't affect gameplay. On an additive draw, your output is still
    added, so a bright tint adds more light rather than replacing it.

### Declare a material in the manifest

If all you want is to shade one thing with one shader, the manifest is
the whole mod: no `src/` directory, no entry point, and nothing
compiled.

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "player"
params  = [1.0, 0.55, 0.15, 0.45]    # arrives as your std140 block
```

All selector keys are optional. Omit one, and it matches everything:

| Key | Meaning |
|---|---|
| `channel` | One of `any`, `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, or `portal` |
| `ident` | Channel-local identity: flame ribbon or caps, particle kind, shadow caster kind, or portal preview rung |
| `class` | Moby class |
| `texture` | Terrain Tiledef ID, from 0 to 127 |
| `level` | Level ID. Sector and texture IDs are per-level, so a keyed row usually needs this too |
| `tier` | `"hp"` or `"lp"`. Omit for both |
| `textured` | `true` or `false`. Omit for both |
| `env_anim` | `true` to take only animated terrain env faces |
| `params` | Floats handed to your shader as its uniform block |
| `enabled` | `false` ships the row switched off |

Narrowing keys narrow: leaving out `tier` means *both* tiers, not
neither. A key the engine doesn't recognize is refused with an error
rather than ignored, because a typo that silently widened your selector
would repaint geometry you never asked for.

`mods/declarative-reshade` is a complete working example: one manifest
and one `.frag` file.

#### Terrain and the retained cache

Terrain is the one channel the engine draws two ways. It walks near
geometry, and anything that changes, per present; the rest is baked into
GPU buffers once and redrawn without the CPU touching it. A material
reaches both paths identically, because the engine compiles your `.frag`
file twice, once per path, and gates each with the same occlusion
verdicts. You write one shader and never learn which path a face took.

A declarative row costs the cache nothing. Across the whole level
corpus, arming an all-terrain selector leaves the retained sector count
and every claim reason exactly where they were with no mods at all.

A `material_register_refine` callback costs the cache nothing either. A
terrain refine selector is keyed on one thing, either a Tiledef ID or
the untextured band, so the engine asks your callback once per present
rather than once per face, and hands the same answer to both paths. None
of that requires a face to be walked.

### Register a material from C

For selection that depends on runtime state, register from your entry
point:

```c
openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
sel.channel    = OP_CHAN_MOBY;
sel.moby_class = 42;
int mat = api->material_register(self, &sel, frag);
api->material_set_params(self, mat, &block, sizeof block);   /* std140 */
api->material_enable(self, mat, 1);
```

Always build the selector with `openpete_mod_material_sel_any()` and
then assign fields. Never zero-initialize it. The struct carries its own
size and flag mask, and the engine refuses a selector it can't honor
exactly rather than matching wider than you asked.

Manifest rows arm before `openpete_mod_entry` runs, so a mod that uses
both tiers has its C registrations land later, and later wins. Where two
materials claim the same pixels, the last registration takes them.

### Follow a value by retuning, not re-registering

`material_register` is an entry-time call. It's refused inside a
present, and a UI section runs inside one, so a material that has to
follow something the player is changing can't re-register itself. Point
it at a new selector instead:

```c
sel.moby_class = picked;
api->material_set_selector(self, mat, &sel);
```

The handle, the shader, the refine callback, and the enabled state all
survive; only the key changes. Called from a present hook or a UI
section, the change takes effect at the next tick rather than
immediately, because selection has to stay constant for a whole present.
The material keeps shading under its old key until then, so it never
blinks out in between.

Offer the player only keys that can match.
`api->moby_classes(self, buf, cap)` returns the moby classes the current
level actually loaded, typically a few dozen out of the 768 the field
allows, so a class picker built from it has no dead entries. It reads an
engine-side snapshot taken once per tick, so unlike `guest()` it's safe
to call from a UI section.

### Write a material shader

Material fragment shaders include `openpete_psx_material.glsl` and read
their own block at set 3 binding 1. The whole shader is one function:

```glsl
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };

vec4 op_material(vec4 base)          // base = the engine's own result
{
    return vec4(mix(base.rgb, tint.rgb, tint.a), base.a);
}
```

`base` is the face decoded the way the engine would draw it, including
CLUT decode, perspective divide, PSX modulation, and the replacement
from any installed HD texture pack. For untextured draws it's the
interpolated vertex color, so one function serves both paths. Return
`base.a` unchanged unless you mean to change transparency: the engine
discards the PSX transparent-texel sentinel for you.

!!! note "Why you don't write `main()`"
    The stock fragment stage does more than color math. It also runs
    og's per-pixel painter verdicts, which decide *which surface exists*
    at a pixel. A shader that owned `main()` replaced those verdicts
    silently, so every rule the engine added later broke every material
    already shipped. That happened three times before the split. With
    `main()` on the engine side, you can't lose behavior you never knew
    about.

`mods/channel-gallery` registers the same shader once per channel with a
different selector each time, with live toggles. It's the fastest way to
see what a given selector actually claims, including which things aren't
separately addressable yet.

### Decide per instance with a refine callback

A selector prunes cheaply, and a refine callback decides the rest. It
runs during extraction, in tick context, and returns that instance's
uniform block, or `-1` to leave the instance stock:

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

That's how you express "class 42, but only while it's burning": the
selector narrows to class 42, and the callback declines the rest. Scope
the selector as tightly as you can first, because the callback is the
expensive half.

!!! warning "The engine guarantees semantics, never cadence"
    Your verdict is the one the material carries, but how often the
    engine asks for it is the engine's business. Results are interned,
    and a channel that later banks geometry across presents re-resolves
    from identity rather than calling back. A callback must therefore be
    a pure function of its key plus tick-stable guest state, and a
    counter inside one measures the engine's memoization rather than the
    scene.

Instances whose blocks are byte-identical share a slot and stay in one
batch, so cost tracks *distinct outcomes*, not matches.

### Shade a draw path that has its own include

Some draw paths carry their own vertex stage, push block, and samplers,
so their declarations can't be the ones described earlier on this page.
Moby drop-shadows are the worked example: flat fans drawn with the depth
test off, because their fragment stage decides per pixel whether the
shadow is occluded, reconstructing terrain height from a depth pre-pass
and rejecting walls, steep faces, and raised platforms.

Those paths get their own include, and nothing else changes:

```glsl
#include "openpete_psx_decal.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };

vec4 op_material(vec4 base)
{
    return vec4(mix(base.rgb, tint.rgb, tint.a), base.a);
}
```

Same function and same body: only the include differs, and the engine
picks it from the draw path. The occlusion verdict is applied before
your function runs, so a material on this path can't paint shadows
through a wall even by accident. `mods/channel-gallery` carries a row on
it, `shaders/highlight_decal.frag`. Compare it with
`shaders/highlight.frag`, and the only difference is the `#include`
line.

Shadows blend subtractively, so a brighter tint darkens the ground
*more*. The row reads inverted next to the opaque channels, which is the
material inheriting its group's compositing exactly as it should.

A row that can't be reshaded yet stays in the gallery labeled as a known
gap. There are none at present.
