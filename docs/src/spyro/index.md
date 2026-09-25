# Spyro

Notes on how Spyro the Dragon (SCUS-94228) stores, loads and runs its
levels, reverse-engineered from the matching decomp's loader and
cross-checked against SpyroEdit.

- [Level format](level-format.md) — on-disc WAD layout, the length-prefixed
  component container, and the decoded interiors of environment sectors,
  faces, textures, collision and moby instances.
- [Level overlays](level-overlays.md) — per-level code overlays: the shared
  `0x8007AA38` window, the id word, the `SetOverlayPointers` callback
  slots, and how the 37 same-address overlays are recompiled into
  co-resident C.
