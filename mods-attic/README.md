# mods-attic

Mods parked when the mod ABI was cut to api v1. Each one's core entry
points left the kernel because they projected engine internals that are
still moving; the FEATURES survive engine-side, and each mod returns
when its surface re-lands as an append-only pillar.

- `ghost-lite`, `triangle-flutter` — actor render service
  (capture_actor_pose / draw_actor / suppress_player_draw): pinned to
  the vk renderer's actor path, which is being reworked. Re-add as a
  pillar once that path settles.
- `moby-instance-tint`, `crystalfissure-dark-artisans` —
  material_register (+ read_level_data / texpack_fog_shift): renderer
  material internals; the dark-artisans needs belong in the texpack
  layer proper.
- `mirror-level` — the level-slot C API; `[[level]]` is the surviving
  declarative road, and the slot-capacity work will change the id
  time-share this API baked in.
- `fade-skip` — instruction-site hooks ([[site]]): build-time coupling
  to the recomp regen step; the boot-fade skip itself is covered by
  --skip-to-level for development.

These directories are NOT scanned by the loader. Their sources predate
the v1 header and will need mechanical updates when they return.
