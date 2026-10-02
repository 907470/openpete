# 0002 - Stub stateless CD commands (CdlSetmode + state mgmt + CdlReadS in CdControlB)

Spyro the Dragon stalls during the title-screen load sequence because
PsyCross's `CdControl` / `CdControlB` only handle a small subset of the
PSX CD command set:

- `CdControl` (synchronous) only knew `CdlSetloc` and `CdlReadS`;
  everything else returned 0 (failure). Spyro issues `CdlSetmode` (0x0E)
  before every read; the failed return prevented the read from being
  initiated and the boot sequence stalled before audio.
- `CdControlB` (blocking) handled `CdlSetmode` only with an
  `eprinterr("Unimplemented...")` log line, and didn't handle `CdlReadS`
  / `CdlReadN` at all.

Both endpoints also returned 0 even on success (PSX hardware returns 1
when a command is accepted by the CD controller). Spyro's command-issued
checks rely on the 1-on-success convention.

## Changes

In `src/psx/LIBCD.C`:

1. `CdControl` now also accepts `CdlSetmode` (silently - sector-size mode
   bits aren't honoured but the command no longer returns failure) and
   stateless commands `CdlNop / CdlPause / CdlStandby / CdlStop /
   CdlMute / CdlDemute`. The function now returns 1 from the success
   paths (was 0).
2. `CdControlB` now also accepts `CdlReadS` / `CdlReadN` (seek + flag
   the read mode, same as `CdControl`'s `CdlReadS`) and the stateless
   commands listed above. The `CdlSetmode` `eprinterr` was removed (the
   no-op is intentional, not an error worth logging every time).
3. `CdControlB`'s `CdlSetfilter` arm now also returns 1 (was 0 - the
   command is accepted-as-no-op, the existing TODO covers the actual
   XA/CDDA work).

## Why a no-op is acceptable for `CdlSetmode`

PsyCross only emulates 2048-byte data-sector reads from a backing
.bin/.cue. The mode bits the game tries to set (sector size, double-
speed, ADPCM) don't change anything we model - accepting the command
silently lets the game's command pipeline advance.

When PsyCross gains real CD-XA / variable-sector support, this case
should be expanded to honour `CdlModeSize0` / `CdlModeSize1` / the
ADPCM bits.

## Files changed

- `src/psx/LIBCD.C`
