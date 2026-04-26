# 0006: DRAWENV struct PSYQ-alignment

## Symptom

Spyro's loading transitions and level-intro cutscenes show heavy
horizontal smearing — multiple Spyros / dragons stacked side by side
with prior frames' pixels never being cleared. Title screen and
gameplay render correctly. Real PSX hardware shows no smear.

## Root cause

`include/psx/libgpu.h:715-726` defined `DRAWENV` with a stray `drt`
field between `dfe` and `isbg`:

```c
typedef struct {
    RECT16  clip;
    short   ofs[2];
    RECT16  tw;
    u_short tpage;
    u_char  dtd;
    u_char  dfe;
    u_char  drt;        /* ← extra! not in PSYQ 4.0 */
    u_char  isbg;
    u_char  r0, g0, b0;
    DR_ENV  dr_env;
} DRAWENV;
```

Sony's PSYQ 4.0 header (`/PSX/INCLUDE/LIBGPU.H` from
`Programmer Tools - Run-time Library 4.0 (USA) (Release 2.0)
_DTL-S2002_`, the redump-verified release that ships with the
toolchain Spyro was built against):

```c
typedef struct {
    RECT     clip;
    short    ofs[2];
    RECT     tw;
    u_short  tpage;
    u_char   dtd;
    u_char   dfe;
    u_char   isbg;       /* directly after dfe */
    u_char   r0, g0, b0;
    DR_ENV   dr_env;
} DRAWENV;
```

`drt` is not part of the PSYQ surface — it appears to have been added
to upstream PsyCross at some point and was never wired to anything
(grep finds the sole reference at the field declaration itself).

The shifted layout corrupts every `PutDrawEnv` call from any game
compiled against PSYQ headers:

| field | PSYQ offset | PsyCross offset (with `drt`) |
| --- | --- | --- |
| `dfe` | 23 | 23 |
| **`isbg`** | **24** | **25** |
| `r0` | 25 | 26 |
| `g0` | 26 | 27 |
| `b0` | 27 | 28 |

`PutDrawEnv` is a byte-level `memcpy` (`src/psx/LIBGPU.C:311-315`).
The game writes `isbg=1` at PSYQ offset 24; PsyCross reads
`activeDrawEnv.isbg` from offset 25, which holds the game's `r0` —
typically `0`. So `if (activeDrawEnv.isbg) GR_Clear(...)` in
`PsyX_BeginScene` (`src/PsyX_main.cpp:810-820`) never fires for
`isbg=1` drawenvs, the auto-clear silently no-ops, and the GL
backbuffer accumulates draws across frames.

The bug only became visible during loading transitions and
level-intro cutscenes because gameplay happens to draw enough
geometry per frame to overwrite the smear; the slow, sparse-draw
loading screens leave it on screen long enough to see.

## Fix

Remove the unused `drt` field so `DRAWENV`'s layout matches PSYQ
exactly. One-line change.

## Verification

Spyro 1, NTSC, openpete linux-release. Boot via the standard
`scripts/run-windows-mingw.sh` defaults (or the equivalent direct
linux invocation):

- Frame 1250 (boot intro / level-load animation, `isbg=1` per the
  init at `spyro-1/src/initialization.c:67-68`): single Spyro
  silhouette on a black background. Pre-fix: a dozen-plus accumulated
  Spyros stacked horizontally.
- Frame 4500 (title screen, `isbg=1`): unchanged — Spyro on a
  moonlit hill, no regression.

Cross-checked instrumentation: at frame 1240+, `activeDrawEnv.isbg`
now reads `1` (matching what `initialization.c` writes); pre-fix it
read `0` because we were sampling the game's `r0` byte.

## Notes

- This patch supersedes the original 0006 ("display-rect clear
  shadow") which was based on an incomplete diagnosis — that fix's
  shadow-replay path only fired when `isbg=1` or `ClearImage` had
  been called, but the underlying bug was that *isbg never read 1
  in the first place*. With the struct layout fixed, the existing
  `if (activeDrawEnv.isbg) GR_Clear(...)` path in `PsyX_BeginScene`
  works as intended and no shadow infra is needed.
- Other PSX games compiled against PSYQ headers (which is to say,
  every Sony-licensed PSX title) likely also benefit — but Spyro is
  the one we have a regression test for.
- Cross-link: `notes/plans/psycross-loading-smear.md` documents
  the diagnostic journey and bisects.
