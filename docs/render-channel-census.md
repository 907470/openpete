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
| `portal_native.c` | PORTAL_CYCLORAMA, PORTAL_MASK (via `add_tri_grouped`) | no | 0 | no | **STABLE per group** — destination skybox guest ptr (:1096) | sub-tick capture-and-replay |
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
