# Reporting a bug

## The bug report zip

{{ key("key:host.bug_report") }} <!-- id: key:host.bug_report home --> writes one
[bug report](glossary.md#bug-report) zip.
Nothing is uploaded: you send the file, with what happened (the level,
what you did, what you expected, and what you saw instead).

### What it holds

- the game's state at the press, and the game's state from 30 to 60 seconds
  before it (less in the first 30 seconds of a session or after a
  [savestate](glossary.md#savestate) load)
- every controller input between the two, so a developer replays those
  seconds on their own computer
- the memory cards as they were at the earlier state
- a screenshot of the picture at the press, and small screenshots of the 5
  seconds before it
- the end of this run's log and of the previous run's log
- your settings (the `--config-report` output and your `openpete.toml`
  files) and key bindings (the `--keys-report` output)
- the graphics card and driver, the displays, the processor, the
  controllers and the operating system
- every enabled mod with its files, and a list of every installed mod
- the game code built from your disc at the
  [first-run compile](glossary.md#first-run-compile); most of the zip's
  size is this file
- your note, when you typed one

It does not hold the disc image (only its file name and checksum) or any
file from your own folders besides the ones listed above. In its text
files and in the savestates' card paths, your home folder is replaced by
`~`, your user name by `<user>`, and your computer name by `<host>`. A
home folder, user name or computer name found in data that is not text
(the game's memory, a mod's memory, a compiled mod) is not changed; the log
names the file and offset on a `finding:` line. A mod setting marked
secret is listed by name, without its value.

### Where it is saved

The zip is named `bug-<game>-<date>-<time>.zip`, for example
`bug-spyro-1-20261103-143000.zip`, and goes in the `bugreports` folder of
the data folder; with the portable layout that is beside the program
([File locations](file-locations.md)). After the note, a notification
reads **BUG REPORT: SAVING...** and stays until the zip is written, then
changes to **BUG REPORT: SAVED**. The log names the file and the mods it
includes. A second press while the zip is being written writes nothing;
before **BUG REPORT: SAVING...** shows, it shows **BUG REPORT: STILL
WRITING THE LAST ONE**.
**BUG REPORT FAILED - SEE THE LOG** means no zip was written; the log
says why.

When the game stops responding ([`OP-E-HANG`](troubleshooting.md#op-e-hang)),
OpenPete writes a zip itself before it exits, named
`bug-<game>-<date>-<time>-hang.zip`. It holds no state at the press and no
screenshot.

### The note

After the press, the game pauses and a box asks **What went wrong?** Type
one line and press ++enter++ to save it in the zip, or ++esc++ to skip it.
On a controller, South (A, Cross) saves and East (B, Circle) skips. The
game resumes after either. The box is not shown when the
[dev overlay](glossary.md#dev-overlay) is not available, and it is
skipped when no keyboard is attached and no on-screen keyboard appears
within 2 seconds.

### Steam Deck

In Steam's controller settings for OpenPete, bind a back button to the
keyboard key ++f9++.

## Without the zip

When OpenPete does not start, or exits before a zip is written, send the
following instead.

### What to include

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
too, or the `-hang` zip, which holds it.

A problem that appears only with mods installed should say which mods:
retry with `--mods ""`<!-- id: flag:--mods --> (no mods) and report both
results.

### What the log contains

The log holds the paths OpenPete uses (your install and data folders, which
can include your user name), your graphics card and driver, and your
controllers. It holds nothing else that identifies you. Remove the user
name from the paths before posting if you prefer.

### Input recordings

A bug that depends on exact timing reproduces from a recording of the
controller input. {{ flag("flag:--bk2-record") }}<!-- id: flag:--bk2-record --> records one
into the `bk2` folder; attach the `.bk2` and the `.bk2.args` file beside
it ([Recording](../playing/recording.md)).
