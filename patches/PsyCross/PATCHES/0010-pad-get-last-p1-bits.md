# 0010 — Pad GetLastP1Bits accessor

Adds `PsyX_Pad_GetLastP1Bits()` so the host can sample the merged P1 pad
bitfield each frame. Used by `--bk2-record` (`core/runtime/bk2_record.c`)
to write a BizHawk-format movie of whatever the player did, including
SDL keyboard, gamepad, and any `--bk2` replay override — all already
folded together by `PsyX_Pad_InternalPadUpdates` into `pad->buttons`.

## Why a getter, not just reading `pad->buttons` directly

`g_controllers` is a private translation unit symbol in `PsyX_pad.cpp`.
Exposing the controller table publicly drags in `PsyXController` and
`LPPADRAW` (libpad-internal types) — instead this returns a single
`u_short` in the same released-set / pressed-cleared convention as
`g_PsyX_Pad_InputOverride` (patch 0008), so the host has symmetric
read/write hooks against PsyCross's pad state without depending on the
controller-record layout.

Returns `0xFFFF` (no buttons pressed) when:

- the game hasn't yet called `PadInitDirect`/`PadStartCom` (no
  `padData` registered), or
- it's been called this iteration but `PsyX_Pad_InternalPadUpdates`
  hasn't run yet on the host fiber (early boot before the first
  game-fiber yield).

Both states map naturally to "nothing pressed yet" for recording
purposes.
