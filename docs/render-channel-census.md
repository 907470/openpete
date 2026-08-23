# Render channel census

Every feeder that emits triangles into the native scene, what it emits,
and — the question that drives most design work here — **what stable
identity it has in hand at emit time**.

A *channel* is a feeder path (source identity), not a scene category
(routing). One feeder can emit into several categories, and several
feeders share a category. Selection systems key on the provenance
**tag**, never on the category; the two tables below say why.

Scene categories are `OPENPETE_SCENE_*` in
`core/include/openpete_native_render.h`. Blend values are PSX ABR+1:
0 = opaque, 1 = average (ABR0), 2 = additive (ABR1), 3 = subtract
(ABR2), 4 = quarter (ABR3).

## Two facts that shape everything downstream

**1. For blended geometry, the category argument is decorative.**
`add_tri` routes `blend != 0` tris *by blend*, ignoring the category
("Ignored for non-opaque tris (blend!=0 routes by blend)",
`openpete_native_render.h:265-266`; `tracer_native.c:107` passes
`OPENPETE_SCENE_ACTOR` with the comment "category ignored for
blend!=0"). So every additive tri in the game — flame ribbon, particle
glows and flares, the dragon star, tracer streaks — lands in one shared
blend-routed bucket regardless of which feeder produced it. Anything
that wants to act on "the flame" specifically must split that bucket by
tag; category cannot do it.

**2. Only two feeders carry a stable per-face identity today.** Moby
(guest record pointer + class + packed face key) and terrain (sector +
face + texture id + tier + env). Portal has a stable *per-group*
identity (the destination skybox guest pointer). Everything else emits
with at best a loop ordinal — which is exactly the identity that does
not survive geometry being cached across frames.

## Feeder table

All paths under `games/spyro-1/features/`.

| Feeder | Categories | Tagged | Blend | Textured | Identity at emit | Cross-frame caching present |
|---|---|---|---|---|---|---|
| `moby_native.c` | ACTOR / HUD3D / HUD / ACTOR_FRONT (routed at :3233-3236) | **YES** — `TAG_MOBY_SLOT` :3334, cleared :3338 | both | both | **STABLE** — guest record ptr, `m_Class`, packed face key | faceblock decode cache; Tier-2 speck hold banked per canonical epoch |
| `terrain_native.c` | WORLD / WORLD_FILL (:6756-6757); PORTAL_GUARD (:6774, env-gated, default off) | **YES** — `TAG_TERRAIN*` :6692-6699, cleared :6758 | both | both | **STABLE** — sector, face, texid, tier, env | sector decode cache; retained GPU sector-mesh cache; sub-tick lerp banks |
| `spyro_native.c` | PLAYER (:1440, :1558) | no | both | both | singleton (`G_SPYRO`); no per-face key | pose capture/replay |
| `portal_native.c` | PORTAL_CYCLORAMA, PORTAL_MASK (via `add_tri_grouped`) | no | 0 | no | **STABLE per group** — destination skybox guest ptr (:1096); plus a per-leaf KIND (see below) | sub-tick capture-and-replay |
| `cyclorama_native.c` | SKY (:471) | no | 0 | no | loop index only | canonical-drew replay gate |
| `flame_native.c` | WORLD (:637 ribbon, :651 caps) | no | **2 ribbon / 0 caps** | ribbon yes, caps no | singleton | none |
| `particle_native.c` | WORLD (six sites) | no | 0/2/3, data-driven | quads yes, rest no | **UNSTABLE** — `sel` class 0..6 only; no pool slot or pointer | snapshot + liveness gate |
| `tracer_native.c` | ACTOR (decorative — blend 2) | no | 2 | no | **UNSTABLE** — per-frame guest point arrays, drained each frame | hook-entry snapshot replay |
| `shadow_native.c` | SHADOW (:545 moby, :752 Spyro) | no | **3 (subtract) always** | moby yes, Spyro no | **UNSTABLE at emit** — but the moby ptr *is* read at build time and simply not plumbed onto the tri | none |
| `transition_native.c` | WORLD (:195 backdrop grid, :212 faded dome) | no | 0 | no | none — dome re-derived from gamestate | polybuf interval recorded for liveness only |
| `ui_sprite_native.c` | HUD (carry=1), OVERLAY, HUD_PANEL | no | 0/1/**3** | mixed | **UNSTABLE** — per-frame polybuf allocation | core carry/re-apply across sub-ticks |
| `s3_actor_native.c` | ACTOR (:417) | no | 0/1 | no | host-side model + pose | none |

Non-feeders in the directory (no `add_tri`): `moby_rank.c`,
`render_paths.c`, `render_decouple.c`, `text_moby_native.c` (glyphs go
to the separate overlay-glyph channel, not a scene category),
`text_moby_id.c`, `cheats_native.c`, `freecam_native.c`,
`levelwarp_native.c`, `audio_volume_native.c`, `mod_level_ops.c`,
`mod_moby_ops.c`, `native_gte.c`. No `add_tri` callers exist outside
`games/spyro-1/features/`.

### The portal preview is a three-rung LOD ladder

A portal shows the destination level's sky, and the game renders it three
different ways across the approach (`portal_native.c:2004-2008`):

| Kind | Leaf | What it draws |
|---|---|---|
| `PORTAL_FLAT` (0) | `func_8004FEA0` | a flat colour quad — the far preview, and the close backstop behind the dome |
| `PORTAL_BLENDED` (2) | `func_80050240` | the dome with DPCS fog toward `D_800757D4` — the medium-distance cross-dissolve |
| `PORTAL_DOME` (1) | `func_8004F4BC` | the full gouraud dome, unfogged — the close-up |

The leaf records `is_dome = (kind != PORTAL_FLAT)`, so the kind is in hand
at emit and would make a natural selector key: "restyle the close-up
portal sky" and "restyle the far colour chip" are different asks, and a
mod that reshades one and not the others will otherwise see its effect
pop as the player walks toward the portal.

Worth stating plainly because it is easy to miss: a material on the SKY
channel does **not** reach portal previews. The cyclorama feeder draws
the level's own sky; portal previews are a different feeder drawing a
*different level's* sky, and the two are only related conceptually.

### Reshadeable does not follow from taggable

A tri carrying a provenance tag is not automatically reshadeable. A
material replaces the FRAGMENT stage, so a channel is reachable only if
its draw path's fragment stage is doing nothing but colour math. Three
outcomes exist today:

| Path | Reshadeable | Why |
|---|---|---|
| opaque + the alpha/additive/subtract-world groups | yes, but **not yet faithfully** — see below | the override inherits the group's blend, depth-write and painter slot, and builds against psx_scene.vert/.frag |
| moby shadows (`scene_tex_sub_decal_pipe`) | yes, via the **decal profile** | `shadow_decal_tex.frag` OWNS per-pixel occlusion (samples the WORLD-opaque pre-pass depth at set 0, rejects walls/steep faces). A plain material would delete that, so this path takes a second contract — `openpete_psx_decal.glsl` — whose engine half exposes the verdict (`op_decal_visible()`) for the mod's fragment to apply |
| portal sky mask (colour-write off) | n/a | writes no colour; deliberately untagged, since a material could not change anything |

**The first row's caveat is the important one.** The stock textured
scene pipelines do NOT bind `psx_scene.frag`; they bind
`psx_scene_key.frag`, and the untextured ones bind `quad_key.frag`.
Those stages carry the og OT-key contest (terrain keys, actor keys, the
prop EMBED rule) and the same-moby rank painter select — per-pixel
*visibility verdicts* expressed as `discard`. Both override sites
(`draw_tex_runs`, `draw_ut_mat_spans`) build against the contest-free
twin, so a material on those groups changes not only a face's colour but
which surface exists at a pixel.

Measured (`cliff-town-vista` f2000): a material selecting the terrain LP
tier and returning stock shading unchanged moves 3105 px, 300 of them at
≥64 sum-delta, with adjacent pixels swapping between two surfaces'
colour families in both directions. It survives `--terrain-cache 0`, and
a probe painting the audience by `vKoff` presence shows that audience is
100% keyed. Separately, `openpete_psx_material.glsl` never divides
`vUV`/`vColor` by the perspective ratio `vQ` the way every stock stage
does — worth ~1-3 LSB on far geometry and more near the camera (moby
3958 px → 1 px when a probe does the divide by hand).

So the ordinary groups belong in the same category as the shadow row,
not opposite it. `notes/plans/openpete-terrain-cache-materials.md`
owns the fix: the engine owns `main()` and the mod owns the colour slot,
which retires the decal profile as a separate *contract* (it stays a
separate pipeline variant) and makes both defects impossible to
reintroduce.

Probe worth keeping for the shadow row: with
`OPENPETE_MOBY_SHADOW_DECAL=0` the fallback (plain subtract + hardware
bias) DOES take the override and the shadows recolour. That is precisely
why the plain override stays refused on both paths — accepting it only on
the fallback would make a mod's reach depend on whether the host can
sample depth.

**The decal profile is the resolution** (`core/include/openpete_psx_decal.glsl`):
a second shading contract that splits the stock stage into the engine's
occlusion verdict and the mod's colour. Its pipeline reuses the
`mod_pipeline` set layout unchanged — sets 0/1 samplers (pre-pass depth,
VRAM), set 3 the engine/mod UBO pair — with `shadow_decal_tex.vert` as
the vertex stage and the decal push block. The whole subtract range draws
as ONE call because every moby shadow shares one sprite AND one tag; a
non-uniform tag falls back to stock rather than painting the range with
the first triangle's material.

The generalisation to carry forward, in the form the measurements above
force: **almost every draw path's fragment stage does something besides
colour maths, so the split belongs in the contract rather than in a
per-path profile.** A mod that writes its own `main()` leaves the engine
nowhere to stand — and every occlusion rule, perspective term or fog
model the engine later adds silently makes every shipped material wrong.

The instrument that catches it in one number: **an identity material —
one whose fragment returns the stock shading unchanged — must be
pixel-identical to no material at all.** No colour classifier, no eyes,
no judgement about what looks right; any nonzero result is contract
leakage by definition, because the mod asked for nothing.

## Retention identity

What key would survive this channel's geometry being banked across
presents or baked onto the GPU. A channel cannot be cached until this
column has an answer, and the answer must be designed *before* the
caching arc, not after.

| Channel | Retention identity | State |
|---|---|---|
| moby | record ptr + class (+ face key) | **proven** — the Tier-2 hold replay already re-resolves per present from exactly this |
| terrain | sector + face (+ texid, tier, env) | available |
| portal | destination skybox guest ptr (per group) | available |
| player | singleton | trivial |
| flame | singleton | trivial |
| sky/cyclorama | none per-tri — only a loop index; a derived key (fan region from vertex run) must be invented | **needs design** |
| shadow | recoverable — the caster moby ptr is in hand at build time; plumbing it onto the tri is a small change | **needs a small change** |
| particle | none — `sel` (0..6) classifies but does not identify; pool ordinal is explicitly not identity | **unsolved** |
| tracer | none — source arrays are drained per frame | **unsolved** |
| 2D UI | none — per-frame polybuf allocation | not applicable (carried, not cached) |

## Pattern breaks worth knowing

1. **`OPENPETE_SCENE_TERRAIN_KEY` (14) is retired** — the number is
   reserved and no feeder emits it.
2. **`PORTAL_GUARD` (9) has no portal producer.** Its only emitter is
   the *terrain* feeder (`terrain_native.c:6774`), behind
   `OPENPETE_PORTAL_GUARD`, default off. Those tris are untagged
   despite the identical face being terrain-tagged nine lines earlier —
   the emit falls after `set_tag(0)` at :6758.
3. **The moby feeder emits into HUD**, a 2D-UI category, with
   re-derived 3D billboards. It shares HUD with `ui_sprite_native.c`'s
   genuinely-2D tris; only the `carry` flag tells them apart.
4. **The dragon star lives in WORLD but behaves like HUD** — additive,
   emitted at z = 0 on the near plane to mirror the PSX's on-top
   ordering.
5. **`ACTOR_FRONT` is selected by guest-pointer match** against
   `g_DragonCutscene`, not by class.
6. **Two different "second player" paths**: the S1 ghost renders as
   PLAYER, the S3 ghost as ACTOR.
7. **`HUD_PANEL` can be subtractive** (blend 3), keyed off a tracked
   `DR_MODE` ABR state rather than the prim's own code.
8. **The transition backdrop is re-derived, not replayed.** It calls
   `cyclorama_native_extract_faded` and synthesizes a 3×3 edge-extend
   grid; the recorded polybuf interval is used only for liveness
   masking and diagnostics, never decoded into tris.

## Slot and tag capacity

The provenance tag is 32 bits: `[31:28]` path (moby 1, terrain 2 — 13
values free), and a per-path payload. The moby layout —
`[27:16]` material slot (12 bits), `[15:0]` class (16 bits) — fills the
word exactly and is the template a new path should copy.

Per-present material slots are interned into **one global table**,
`MOD_MATERIAL_SLOTS = 128` (`core/mods/mod_shader.c:927`), index 0
reserved. That software constant — not the tag — is the binding
capacity today: the moby slot field would hold 4094 outcomes and
terrain's 9-bit field 510.

**Latent hazard:** nothing couples the table size to the *narrowest*
slot field. Terrain decodes its slot as `(tag >> 16) & 0x1FF`, so if
the table were ever raised past 511, a terrain tri carrying a global
slot id ≥ 512 would silently alias to slot 0 — "defer to declarative
resolve", i.e. the wrong material, with no crash and no log. Any change
to `MOD_MATERIAL_SLOTS` needs a compile-time assertion against the
narrowest field in use.
