# 0011 - Make the PsyCross log file opt-in

## Symptom

Every launch of `openpete-spyro1` drops a `Spyro the Dragon.log` file into
the current working directory. PsyCross's `PsyX_Log_Initialise()` (called
unconditionally from `PsyX_Initialise`) opens `"<appname>.log"` for writing,
and `g_appNameStr` is the window/app title. The host never reads this file;
it just clutters whatever directory the game is run from (and was already in
`openpete/.gitignore`).

## Fix

Gate the log-file creation behind an env var. `PsyX_Log_Initialise()` now
early-returns (leaving `g_logStream == NULL`) unless `OPENPETE_PSYX_LOG` is
set. This is safe because the write path already guards the stream:
`PrintMessageToOutput()` only does `fprintf(g_logStream, ...)` when
`g_logStream != NULL`, and `PsyX_Log_Finalise` / `PsyX_Log_Flush` already
null-check it. Console (stdout) logging via `printf` is unaffected in both
modes.

- Default: no file is created.
- `OPENPETE_PSYX_LOG=1`: the `<appname>.log` file is created as before, for
  debugging.

Mirrors the existing `OPENPETE_*` env-var gating convention already used in
PsyCross (e.g. `OPENPETE_RENDER_PROBE`). `getenv` is available via the
`<stdlib.h>` already included at the top of `PsyX_main.cpp`.

## Verification

Spyro 1, NTSC, openpete linux-release. Ran headless from a clean scratch
directory:

- No env var -> no `.log` file appears in the cwd.
- `OPENPETE_PSYX_LOG=1` -> `Spyro the Dragon.log` is created (533 bytes) and
  contains the usual PsyX init spew.
