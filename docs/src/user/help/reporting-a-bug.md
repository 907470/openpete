# Reporting a bug

## What to include

1. **The log.** Every run writes its output to `openpete.log` in the `logs`
   folder ([File locations](file-locations.md)): the version, the startup
   sequence, what it decided about your disc, controllers and graphics
   card, driver errors, and the crash report when it crashed. The previous
   run's log is kept as `openpete.log.1`, and earlier runs' as
   `openpete.log.2` through `openpete.log.15`; attach the run that failed
   when a relaunch made the problem go away.
2. **The settings.** The output of

    ```
    openpete --config-report
    ```
    <!-- id: flag:--config-report -->

    lists every setting, where it came from, and every folder in use.
    For a controls problem, add the output of {{ flag("flag:--keys-report") }}<!-- id: flag:--keys-report -->.
3. **What happened.** The level, what you did, what you expected, and what
   you saw instead. For a visual problem, an ++f12++ [screenshot](glossary.md#screenshot)
   ([File locations](file-locations.md) names the folder).
4. **The package.** Windows zip, Linux AppImage or Linux tar.xz, and its
   version: the file name of the download, or the version line shown at
   the bottom left of the window at startup.

When the game hangs (its code runs on without waiting for the next
[VBL](glossary.md#vbl)), OpenPete exits with status 2 and writes
`hang-trace.bin` into the folder it was started from; attach that file
too.

A problem that appears only with mods installed should say which mods:
retry with `--mods ""`<!-- id: flag:--mods --> (no mods) and report both
results.

## What the log contains

The log holds the paths OpenPete uses (your install and data folders, which
can include your user name), your graphics card and driver, and your
controllers. It holds nothing else that identifies you. Remove the user
name from the paths before posting if you prefer.

## Input recordings

A bug that depends on exact timing reproduces from a recording of the
controller input. {{ flag("flag:--bk2-record") }}<!-- id: flag:--bk2-record --> records one
into the `bk2` folder; attach the `.bk2` and the `.bk2.args` file beside
it ([Recording](../playing/recording.md)).
