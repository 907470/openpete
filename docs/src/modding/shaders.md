# Shaders

Mods ship GLSL source. The engine compiles it at load with in-process
glslang in the `vulkan1.0` dialect, caches the SPIR-V by content hash
under `<mod>/.build/shaders/`, and writes compile errors to
`.build/shaders/<name>.log`. A broken shader yields an unavailable effect
and a log line; the mod still loads.

Shaders run on the GPU and cannot touch guest RAM. C code may read game
state to feed uniforms; nothing flows back.

| Kind | API | Effect |
|---|---|---|
| Post-process | `shader_register`, `postfx_register`, `postfx_enable`, `postfx_set_uniforms` | Fullscreen fragment passes over the composed frame: CRT simulation, grading, scanlines |
| Material | `[[material]]` rows, or `material_register` | Replaces the fragment shading of a subset of the engine's own draws: a moby class, a terrain tier, the flame plume. The manifest form needs no C |

A mod shades what the engine draws; it cannot add draws of its own.

## Glossary

| Term | Meaning |
|---|---|
| selector | The set of keys deciding which of the engine's draws a material repaints. Every key is optional and an omitted key matches everything, so a selector widens as keys are left out. |
| channel | The family of draws a selector names: `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, `portal`, or `any`. |
| ident | An identity within one channel, since a channel covers several kinds of draw: a flame's ribbon against its caps, a particle kind and class, a portal preview kind. |
| tier | Which terrain detail level a face belongs to, `hp` or `lp`. Omit the key to take both. |
| pass | One fullscreen post-process draw a mod registers. Passes run every present. |
| point | Where in the frame a pass runs: `OP_POSTFX_SCENE` before the HUD, `OP_POSTFX_COMPOSITE` after it. |
| refine callback | A callback run during extraction, once per matched instance, returning that instance's uniform block or declining the match. |

## Registering a shader

```c
int frag = api->shader_register(self, "shaders/crt.frag");
```

The path is relative to the mod and the extension selects the stage. The
call returns `-1` for a missing file or a compile error.

Every shader includes the binding contract:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1     /* post-process fragments only: defines op_uv, op_color */
#include "openpete_shader_api.glsl"
```

The include resolves to `<exe>/sdk/shaders/openpete_shader_api.glsl`,
and its bytes are hashed into every shader's build key, so an interface
change invalidates the cache. Material shaders include
`openpete_psx_material.glsl` or `openpete_psx_decal.glsl` instead; never
both, since each claims the same descriptor set.

To ship without source, place `<path>.spv` where no source sits beside
it, with a `<path>.spv.ihash` sidecar matching the current interface
hash. A stale sidecar refuses the shader.

## Post-process passes

```c
int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, /*order*/ 0);
api->postfx_set_uniforms(self, fx, &params, sizeof params);   /* copied; any context */
api->postfx_enable(self, fx, 1);
```

Injection points:

- `OP_POSTFX_COMPOSITE`: after the scene and the HUD are composed, before
  the present blit. Sees the whole frame including the HUD.
- `OP_POSTFX_SCENE`: after the 3D scene, before the HUD, so grading and
  fog tint the world while the HUD stays untouched. Receives scene depth
  at `op_scene_depth`.
- `OP_POSTFX_PRESENT`: reserved; registration is refused.

Passes run on every present. Order across mods
follows enabled-list priority; `order` breaks ties within one mod, lower
first.

Inputs: `op_prev_pass` (set 0), `op_scene_depth` (set 1; the opaque
terrain pre-pass depth, meaningful at the `SCENE` point, no mobys),
`op_prev_frame` (set 2, for temporal effects). The engine block
`op_engine` at set 3 binding 0 carries time, the tick, the interpolation
alpha (the value present hooks see), the resolutions, and the aspect
ratio. The mod's own block is set 3 binding 1: declare it `std140` and
mirror the layout in the C struct by hand. `postfx_set_uniforms` copies
the bytes verbatim, at most 1024.

Example: `crt-beans` (a three-pass CRT model, shipped under
`sdk/examples/`).

## Materials

A material replaces the fragment shading of a subset of the geometry the
engine draws. A *selector* names the subset; the engine decides per draw
whether the shader runs. The shader never receives game structs and never
decides whether it applies.

A material inherits its draw group's blend mode, depth policy, and draw
order and changes colour math only. It cannot move, reorder, hide, or
re-composite geometry. On an additive draw the output is still added.

### Declare a material in the manifest

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "player"
params  = [1.0, 0.55, 0.15, 0.45]    # the shader's std140 block
```

Every selector key is optional; an omitted key matches everything:

| Key | Meaning |
|---|---|
| `frag`, `vert` | Shader paths relative to the mod |
| `channel` | `any`, `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, `portal` |
| `ident` | Channel-local identity: flame ribbon (0) or caps (1); particle `(kind << 8) \| sel` with kinds 0 quads, 1 lines, 2 glow rings, 3 sparkle flares, 4 sparkle lines, 5 dragon star (`OP_PART_*`); shadow caster kind; portal preview kind |
| `class` | Moby class |
| `texture` | Terrain texture-record ID, 0 to 127 |
| `level` | Level ID. Texture IDs are per level, so a `texture` row usually needs this |
| `tier` | `"hp"` or `"lp"`; omit for both |
| `textured` | `true` or `false`; omit for both |
| `env_anim` | `true` selects only animated terrain env faces |
| `params` | Floats handed to the shader as its uniform block |
| `enabled` | `false` ships the row switched off |
| `game` | One game id or an array of them; the row registers on those games only. Omit for every game the mod loads on |

A key the engine does not recognise is refused with an error, since a
typo that silently widened a selector would repaint geometry the row
never named. A row whose channel the running game does not serve, or
with channel `any` on a game that serves none, is logged and skipped,
and `material_register` refuses the same channel with one log line and
-1; today every game serves every channel.
`declarative-example` is a complete example: one manifest,
asset files and one `.frag`.

Terrain is drawn two ways: near and changing geometry is walked per
present; the rest is retained in GPU buffers. The engine compiles a
material's `.frag` once per path and gates both with the same occlusion
verdicts, so one shader covers both, and a terrain material does not
reduce the retained set.

### Register a material from C

```c
openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
sel.channel    = OP_CHAN_MOBY;
sel.moby_class = 42;
int mat = api->material_register(self, &sel, frag);
api->material_set_params(self, mat, &block, sizeof block);   /* std140, at most 256 bytes */
api->material_enable(self, mat, 1);
```

Build the selector with `openpete_mod_material_sel_any()` and assign
fields; never zero-initialise it. The struct carries its own size and
flag mask, and the engine refuses a selector it cannot honour exactly.

Manifest rows arm before the entry point runs, so C registrations land
later and win where two materials claim the same pixels.

`material_register` is refused inside a present, and a UI section runs
inside one. A material that follows a value the player is changing
retargets with `material_set_selector` instead:

```c
sel.moby_class = picked;
api->material_set_selector(self, mat, &sel);
```

The handle, shader, refine callback, and enabled state survive. From a
present hook or UI section the change applies at the next tick, since
selection is constant across a present; the material keeps its old key
until then.

`api->moby_classes(self, buf, cap)` returns the moby classes the current
level's load placed in the game's model table, ascending, from an
engine-side snapshot refreshed once per tick, so it is safe from a UI
section and gives a class picker no dead entries. On Spyro 3, whose
model table keeps the rows of earlier levels, the set is the classes of
the load's own model lists, not every filled row. Class ids run to 767
on Spyro 3; the call copies at most `cap` entries, so a buffer of any
size is safe, and `moby_classes(self, NULL, 0)` returns the count.

### Write a material shader

```glsl
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };

vec4 op_material(vec4 base)          // base = the engine's own result
{
    return vec4(mix(base.rgb, tint.rgb, tint.a), base.a);
}
```

`base` is the face as the engine would draw it: CLUT decode, perspective
divide, PSX modulation, and any installed HD texture replacement applied.
For untextured draws it is the interpolated vertex colour. Return
`base.a` unchanged unless the material means to change transparency; the
engine discards the PSX transparent-texel sentinel before the call.

The engine owns `main()`. The stock fragment stage also runs the
per-pixel surface verdicts that decide which surface exists at a pixel; a
shader that owned `main()` would replace those verdicts and break
whenever the engine changed them.

Moby drop-shadows are drawn on their own path (flat fans, depth test off,
per-pixel occlusion decided in the fragment stage). A material on that
path includes `openpete_psx_decal.glsl` instead; the function and body
are unchanged and the engine picks the include from the draw path. The
occlusion verdict is applied before `op_material` runs. Shadows blend
subtractively, so a brighter tint darkens the ground more.

`mods/channel-gallery` registers one highlight shader once per channel
with a different selector each time, toggled live; it shows what each
selector claims.

### Refine callbacks

A selector prunes cheaply; a refine callback decides per instance. It
runs during extraction in tick context and returns the instance's uniform
block, or `-1` to leave the instance stock:

```c
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    const Moby* m = (const Moby*)api->guest(key->u.moby.vaddr);
    if (!m || m->m_State != BURNING) return -1;
    float* v = out; v[0] = 1.0f; v[1] = 0.4f; v[2] = 0.0f; v[3] = 1.0f;
    return 16;
}
...
api->material_register_refine(self, &sel, frag, refine);
```

Available on the moby channel (keyed per instance) and on terrain (keyed
per texture ID or the untextured band, asked once per present). Scope the
selector as tightly as possible first; the callback is the expensive
half.

Results are memoised per instance and the call count is not part of the
contract, so the callback must be a pure function of its key and
tick-stable guest state. Instances whose blocks are byte-identical share
one slot and one batch, so cost follows the number of distinct outcomes,
not the number of matches; one distinct block per instance is the
worst case. `channel-gallery` (shipped under `sdk/examples/`) registers
materials from C over every selectable surface.
