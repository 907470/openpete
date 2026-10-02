# Render channel census

Every feeder that emits triangles into the native scene, what it emits, and
what stable identity it holds at emit time.

A channel is a feeder path (source identity), not a scene category
(routing). One feeder emits into several categories, and several feeders
share a category. Material selectors key on the provenance tag, never on
the category; the two facts below say why.

Scene categories are the `OPENPETE_SCENE_*` defines in
`core/include/openpete_native_render.h:387-530`. Blend values are PSX ABR
plus one: 0 = opaque, 1 = ABR mode 0 (B+F)/2 alpha, 2 = ABR mode 1
additive, 3 = ABR mode 2 subtractive. ABR mode 3 (B+F/4) has no consumer
and no blend value (`openpete_native_render.h:368-374`).

## Two facts that shape everything downstream

1. For blended geometry, the category argument is decorative.
`openpete_native_scene_add_tri` routes `blend != 0` tris by blend and
ignores the category (`openpete_native_render.h:385`;
`games/spyro-1/renderer/tracer.c:108` passes `OPENPETE_SCENE_ACTOR`
with the comment "category ignored for blend!=0"). Every additive tri in
the game — flame ribbon, particle glows and flares, the dragon star, tracer
streaks — lands in one blend-routed bucket regardless of feeder. Splitting
that bucket needs the tag; the category cannot do it.

2. A tag is not an identity. Nine of the twelve feeders stamp a provenance
tag, but only moby and terrain carry a stable per-face identity inside it
(guest record pointer plus class plus packed face key; sector plus face
plus texture id plus tier plus env). Portal carries a stable per-group
identity, the destination skybox guest pointer, on the tri's group id
rather than in the tag. The remaining tags classify a channel and a
feeder-local kind without identifying which element was drawn.

## Feeder table

All paths under `games/spyro-1/features/`.

| Feeder | Categories | Tag | Blend | Textured | Identity at emit | Cross-tick caching |
|---|---|---|---|---|---|---|
| `moby.c` | ACTOR / HUD3D / HUD / ACTOR_FRONT (routed at :3202-3205) | `TAG_MOBY_SLOT` :3319, cleared :3323 | both | both | stable — guest record ptr, `m_Class`, packed face key | faceblock decode cache; Tier-2 speck hold banked per canonical epoch (:5171) |
| `terrain.c` | WORLD / WORLD_FILL (:6633); PORTAL_GUARD (:6651, env-gated, default off) | `TAG_TERRAIN*` :6569, cleared :6635 | both | both | stable — sector, face, texid, tier, env | sector decode cache; retained GPU sector-mesh cache; sub-tick lerp banks |
| `spyro.c` | PLAYER (:1288 walk) | `TAG_PLAYER` :1263 | both | both | singleton (`G_SPYRO`); no per-face key | seg prev/curr snapshots (sub-tick shape lerp) |
| `portal.c` | PORTAL_CYCLORAMA, PORTAL_MASK (via `add_tri_grouped`) | `TAG_PORTAL(kind)` :1030, :1485; the aperture mask tris at :1583 and :1612 are untagged | 0 | no | stable per group — destination skybox guest ptr; preview kind in the tag | sub-tick capture-and-replay |
| `cyclorama.c` | SKY (:485) | `TAG_SKY` :468, cleared :487 | 0 | no | loop index only, not emitted | canonical-drew replay gate |
| `flame.c` | WORLD (:597 ribbon, :612 caps) | `TAG_FLAME(part)` :581, :600 | 2 ribbon / 0 caps | ribbon yes, caps no | ribbon-vs-caps only | none |
| `particle.c` | WORLD (six sites) | `TAG_PARTICLE(kind, sel)` at six sites | 0/2/3, data-driven | quads yes, rest no | classifies only — feeder kind 0..5 plus the game's `sel` for quads and lines; no pool slot or pointer | snapshot + liveness gate |
| `tracer.c` | ACTOR (decorative — blend 2) | `TAG_TRACER` :125 | 2 | no | none — per-tick guest point arrays, drained each tick | hook-entry snapshot replay |
| `shadow.c` | SHADOW (:549 moby, :759 Spyro) | `TAG_SHADOW(kind)` :536, :746 | 3 (subtract) always | moby yes, Spyro no | caster kind only; the caster moby ptr is read at build time and not plumbed onto the tri | none |
| `transition.c` | WORLD (:196 backdrop grid, :213 faded dome) | none | 0 | no | none — dome re-derived from gamestate | polybuf interval recorded for liveness only |
| `ui_sprite.c` | HUD (carry=1), OVERLAY (:335), HUD_PANEL (:345) | none | 0/1/3 | mixed | none — per-tick polybuf allocation | core carry/re-apply across sub-ticks |

Non-feeders in the directory (no `add_tri` call): `moby_rank.c`, `moby_rank_cal.c`,
`render_paths.c`, `render_decouple.c`, `polybuf_guard.c`,
`crash_demo_woah.c`, `vibration.c`, `text_moby.c` (glyphs go
to the separate overlay-glyph channel, not a scene category),
`text_moby_id.c`, `cheats.c`, `freecam.c` (with `games/common/freecam_game.c`),
`levelwarp.c`, `audio_volume.c`, `mod_level_ops.c`,
`mod_moby_ops.c`, `native_gte.c`. No `add_tri` callers exist outside
`games/spyro-1/features/`.

## Tag layouts

The tag is 32 bits with the path in `[31:28]`. Moby and terrain predate the
shared layout and use their own payloads
(`openpete_native_render.h:795-816`); every path from 3 upward uses
`OPENPETE_TAG_PATH(path, ident, slot)` — `[27:16]` material slot (12 bits),
`[15:0]` source identity (`openpete_native_render.h:833-836`).

| Path | Macro | Identity field |
|---|---|---|
| 1 | `OPENPETE_TAG_MOBY_SLOT(cls, slot)` | `m_Class` in `[15:0]`, slot in `[27:16]` |
| 2 | `OPENPETE_TAG_TERRAIN{,_TEX,_UNTEX}` | sector `[8:0]`, texid `[15:9]`, slot `[24:16]`, untex bit 25, LP tier bit 26, env bit 27 |
| 3 | `OPENPETE_TAG_PLAYER(slot)` | 0 — the walk keeps per-vertex indices, not a segment id |
| 4 | `OPENPETE_TAG_FLAME(part, slot)` | `FLAME_RIBBON` 0, `FLAME_CAPS` 1 |
| 5 | `OPENPETE_TAG_PARTICLE(kind, sel, slot)` | `(kind << 8) | sel`; kinds `PART_QUAD` 0, `PART_LINE` 1, `PART_GLOW` 2, `PART_FLARE` 3, `PART_SPARK` 4, `PART_STAR` 5; `sel` is the game's own class for quads and lines, 0 otherwise |
| 6 | `OPENPETE_TAG_SKY(slot)` | 0 — the feeder carries a loop index only |
| 7 | `OPENPETE_TAG_SHADOW(kind, slot)` | `SHADOW_MOBY` 0, `SHADOW_PLAYER` 1 |
| 8 | `OPENPETE_TAG_TRACER(slot)` | 0 |
| 9 | `OPENPETE_TAG_PORTAL(kind, slot)` | `PORTAL_FLAT` 0, `PORTAL_DOME` 1, `PORTAL_BLENDED` 2 |

The path numbers are the mod-facing `OP_CHAN_*` values
(`core/include/openpete_mod_api.h:127-136`), and the identity field is the
selector's `ident` key. The tag joins the batcher's material sort key only
while a split consumer is armed (a registered material selector, or
`OPENPETE_TAG_SPLIT=1`), so batching without mods is byte-identical.
`OPENPETE_TAG_TINT=1` tints vertex colours by tag hash as a coverage
instrument.

Untagged geometry (transition, 2D UI, the portal aperture mask) is
unreachable by a material selector.

## The portal preview is a three-rung LOD ladder

A portal shows the destination level's sky, and the game renders it three
ways across the approach (`portal.c:2018-2020`, dispatched by the
leaf hooks at :2210-2212):

| Kind | Leaf | What it draws |
|---|---|---|
| `PORTAL_FLAT` (0) | `func_8004FEA0` | a flat colour quad — the far preview, and the close backstop behind the dome |
| `PORTAL_BLENDED` (2) | `func_80050240` | the dome with DPCS fog toward `D_800757D4` — the medium-distance cross-dissolve |
| `PORTAL_DOME` (1) | `func_8004F4BC` | the full gouraud dome, unfogged — the close-up |

The leaf sets `s_portal_kind` (`portal.c:2045-2047`) and the emit
stamps it into the tag, so a material can key one rung. A material that
reshades one rung and not the others changes appearance as the player walks
toward the portal, which is why `ident` -1 (all three) is the usual choice.

A material on the SKY channel does not reach portal previews. The cyclorama
feeder draws the level's own sky; portal previews are a different feeder
drawing a different level's sky.

## Reshadeable does not follow from taggable

A tri carrying a provenance tag is not automatically reshadeable.

The stock textured scene pipelines bind `core/rhi/shaders/psx_scene_key.frag`,
not a plain colour shader, and the untextured ones bind `quad_key.frag`.
Those stages carry the og OT-key contest (terrain keys, actor keys, the prop
EMBED rule) and the same-moby rank painter select — per-pixel visibility
verdicts expressed as `discard`. Moby drop-shadows go further: their stage
reconstructs terrain height from a depth pre-pass to reject walls and steep
faces. Portal previews are stencil-tested against the aperture mask. A
material supplying its own `main()` would delete whatever it did not
reproduce.

The resolution is the contract's shape, not a per-path exemption. The engine
owns `main()`; a material supplies `vec4 op_material(vec4 base)`
(`core/include/openpete_psx_material.glsl:136,146`). The engine runs the
verdicts, decodes the texel, applies the modulation, and writes the result,
so a material cannot skip engine behaviour it never gets to run.

| Path | Reshadeable | Via |
|---|---|---|
| opaque plus the alpha/additive/subtract-world groups | yes | `openpete_psx_material.glsl` — the engine half runs the key and rank contests from sets 2, 4, 5 and 6 (`openpete_psx_material.glsl:163-166`) |
| portal sky previews | yes | same include; the override inherits the aperture stencil test |
| retained terrain (the GPU sector-mesh cache) | yes | same include, compiled a second time under `OP_PROFILE_TERRAIN` — flat per-face varyings instead of a push block, the fold atlas instead of a per-draw pack image, and `core/rhi/shaders/terrain_shade.glsl` shared with the stock stage so the two cannot drift |
| moby shadows (`scene_tex_sub_decal_pipe`) | yes | `core/include/openpete_psx_decal.glsl` — a different draw path (own vertex stage, push block, samplers) so its declarations differ, but the same `op_material()` and the same split; `main()` applies the occlusion verdict before `op_material()` runs (`openpete_psx_decal.glsl:194`) |
| Spyro's shadow (`gouraud_sub_pipe`, untextured depth-aware decal) | no — known gap | a third fragment stage whose vertex interface is neither the scene contract's nor the textured decal's: `vColor` at location 0, `vUV` at 1, plus `vQ` at 3 (`core/rhi/shaders/shadow_decal.frag:68-71`), against the decal profile's `vUV` at 0 and `vColor` at 1 (`openpete_psx_decal.glsl:53-54`). It owns per-pixel occlusion, so it needs an engine half of its own; none exists. The channel-gallery's "Shadows (player)" row is routable and paints nothing (`mods/channel-gallery/src/channel_gallery.c:113`) |
| portal sky mask (colour-write off) | n/a | writes no colour, so a material could change nothing |

With `OPENPETE_MOBY_SHADOW_DECAL=0` the fallback (plain subtract plus
hardware bias) takes the override too, so a mod's reach does not depend on
whether the host can sample depth.

The instrument that keeps this honest is the identity material: one whose
`op_material` returns `base` unchanged must be pixel-identical to no
material at all. Any nonzero pixel delta is contract leakage by definition,
because the mod asked for nothing. It is a rung in
`tests/harness/check_mod_channels.sh:152-196` for every reshadeable channel.

## Retention identity

The key that would survive a channel's geometry being banked across presents
or baked onto the GPU. A channel cannot be cached until this column has an
answer.

| Channel | Retention identity | State |
|---|---|---|
| moby | record ptr + class (+ face key) | proven — the Tier-2 hold replay re-resolves per present from exactly this |
| terrain | sector + face (+ texid, tier, env) | proven — the retained GPU sector-mesh cache uses it |
| portal | destination skybox guest ptr (per group) | available |
| player | singleton | trivial |
| flame | singleton (ribbon/caps split) | trivial |
| sky/cyclorama | none per-tri — only a loop index; a derived key (fan region from vertex run) must be invented | needs design |
| shadow | recoverable — the caster moby ptr is in hand at build time; plumbing it onto the tri is a small change | needs a small change |
| particle | none — kind and `sel` classify but do not identify; pool ordinal is explicitly not identity | unsolved |
| tracer | none — source arrays are drained per tick | unsolved |
| 2D UI | none — per-tick polybuf allocation | not applicable (carried, not cached) |

## Pattern breaks worth knowing

1. `OPENPETE_SCENE_TERRAIN_KEY` (14) is retired — the number is reserved and
   no feeder emits it (`openpete_native_render.h:529-530`).
2. `PORTAL_GUARD` (9) has no portal producer. Its only emitter is the
   terrain feeder (`terrain.c:6651`), behind `OPENPETE_PORTAL_GUARD`,
   default off. Those tris are untagged despite the identical face being
   terrain-tagged: the emit falls after `set_tag(0)` at :6635.
3. The moby feeder emits into HUD, a 2D-UI category, with re-derived 3D
   billboards. It shares HUD with `ui_sprite.c`'s genuinely-2D tris;
   only the `carry` flag tells them apart.
4. The dragon star lives in WORLD but behaves like HUD — additive, emitted
   at z = 0 on the near plane to mirror the PSX's on-top ordering.
5. `ACTOR_FRONT` is selected by guest-pointer match against
   `g_DragonCutscene` (`moby.c:1522-1534`), not by class.
6. `HUD_PANEL` can be subtractive (blend 3), keyed off the ABR of the last
   tracked `DR_MODE` rather than the prim's own code
   (`ui_sprite.c:333,344-346`).
7. The transition backdrop is re-derived, not replayed. It calls
   `cyclorama_native_extract_faded` (`transition.c:107`) and
   synthesizes a 3x3 edge-extend grid; the recorded polybuf interval is used
   only for liveness masking and diagnostics, never decoded into tris.

## Slot and tag capacity

Per-present material slots are interned into one global table,
`MOD_MATERIAL_SLOTS = 128` (`core/mods/mod_shader.c:749`), index 0 reserved
for "defer to declarative resolve". That software constant, not the tag, is
the binding capacity: the 12-bit slot field of the shared layout holds 4094
outcomes and terrain's 9-bit field 510.

Slot ids are minted from one counter but stamped into fields of differing
width, so the table can never outgrow the narrowest field. Terrain's is the
narrowest at 9 bits and its decode masks `(tag >> 16) & 0x1FF`
(`mod_shader.c:1390`), so an id of 512 would come back as 0 — the wrong
material, with no crash and no log — and 0x1FF is already the
declined-to-stock sentinel `OPENPETE_MATERIAL_TERRAIN_STOCK`
(`core/include/openpete_mods.h:648`), leaving 510 as the last usable id.
A compile-time assertion at `mod_shader.c:775-776` refuses a
`MOD_MATERIAL_SLOTS` above that bound; raising it means widening the terrain
field first.
