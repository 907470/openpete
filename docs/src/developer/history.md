# History

Milestones kept for the record. Each entry names the commit and the
artifact as it was made; the artifact is stored unmodified.

## 2026-04-24: the first triangle

![The first triangle](history/2026-04-24-first-triangle.png)

The first image the port drew: a hardcoded `POLY_F3` injected into the
PsyCross frame each cycle by `core/runtime/debug_render.cpp` under
`--debug-triangle`, before any game code rendered. The OT was hand-built
around a PsyCross `ClearOTag` quirk whose init loop overwrote the
terminator. Commit 62c7060f (main loop, overlay dispatch, first
triangle). The original screenshot is
[`history/2026-04-24-first-triangle.bmp`](history/2026-04-24-first-triangle.bmp)
(640x480, the bytes committed that day); the PNG above is a lossless
conversion of it.
