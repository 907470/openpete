# Spyro 1 BK2 corpus

This directory holds BizHawk movie (`.bk2`) inputs that drive
openpete's headless harness for regression testing. Each file is a
sealed reproduction of a gameplay scenario — same input every run,
forever.

The determinism matrix harness (`scripts/check_determinism_matrix.sh`,
ctest target `determinism_matrix`) iterates this corpus across the
presentation matrix (aspect × fov × ...) and asserts every cell is
bit-identical to its `(4:3, default-fov)` baseline. Adding a new BK2
here therefore extends regression coverage permanently.

The harness has three modes:

| Mode | Flag | Coverage | Frames/cell | Use when |
| ---- | ---- | -------- | ----------- | -------- |
| PR-tier | (default) | 3 scenarios × 3 aspects × 2 fovs | 2000 | quick local check |
| Full | `--full` | all *.bk2 + title × {4:3,16:9,21:9,32:9} × {default,50,90,120} | 2000 | broad sweep |
| Soak | `--soak` | all *.bk2 + title × {4:3,16:9} × {default} | 30000, `--bk2-stop-on-end` | hunt slow-burn / late-frame leaks |

Soak mode (ctest target `determinism_soak`, label `soak`) is the
mode for catching channel leaks that only surface deep into a
movie — drift accumulators, animation state that diverges slowly,
or a moby that wanders into a divergent sector after thousands of
frames. It excludes `tas` + `frame-time-test` by default
(`--exclude=tas,frame-time-test`); override the exclude list to
broaden or narrow.

## Corpus discipline

**Capture every gameplay scenario you exercise manually.** If you
boot the game to investigate something — a bug, a feature, a render
question — and you press buttons to reach the state you want, that
is a regression-fixture worth saving. Five seconds of pad input now
prevents a future "I can't reproduce that" hour.

Capture is a one-flag operation:

```sh
build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
    --run-recomp 0x80012204 \
    --bk2-record games/spyro-1/bk2/<descriptive-name>.bk2
```

Use kebab-case names that read as a goal, not a fix
(`stone-hill-entry.bk2` good; `fixes-issue-42.bk2` bad — the issue
will rot, the goal won't). Keep them small (< 30 sec gameplay; a
few KB on disk).

To extend an existing movie with new input, combine `--bk2` with
`--bk2-record` — replays the movie and appends live input from
where it ends.

## Per-BK2 args sidecar

A movie can ship with a sidecar file `<name>.bk2.args` that supplies
extra CLI flags whenever `--bk2 <name>.bk2` is passed. Useful for
scenarios that need a non-default startup flag to be reproducible —
e.g. an Artisans-only movie that should always boot via
`--skip-to-level 10` instead of relying on title-screen input timing.

Format: whitespace-separated tokens; `#` to end-of-line is a comment.
Example `skip-to-artisans.bk2.args`:

```
# Skip the title-screen demo and drop straight into Artisans Homeworld.
--skip-to-level 10
```

Sidecar tokens are spliced into argv **before** the user's flags, so
explicit CLI args still override (e.g. passing `--skip-to-level 20`
on the command line wins over a sidecar's `10`). Missing sidecar is
not an error; it's the common case.

## Current corpus

| BK2                          | What it exercises                                          |
| ---------------------------- | ---------------------------------------------------------- |
| `artisans-bridge.bk2`        | Artisans bridge→hill junction (terrain GT3 UV orientation) |
| `demo.bk2`                   | Full run through of demo mode (no inputs)                  |
| `frame-time-test.bk2`        | Frame-pacing under variable input                          |
| `inventory.bk2`              | Inventory open/close, gem/dragon/life HUD reads            |
| `missing-geometry.bk2`       | Sector load order                                          |
| `nestor.bk2`                 | Dragon-rescue cutscene (RESCUED text over moby; letterbox; f1875) |
| `missing-world.bk2`          | Level-overlay translation edge case                        |
| `moby-culling-16-9.bk2`      | Cone-cull at 16:9 (m_WasDrawn channel exemplar)            |
| `portal-cyclorama-issue.bk2` | Cyclorama clip-X widening (gated channel exemplar)         |
| `skip-to-artisans.bk2`       | Title → first level transition                             |
| `stone-hill-entry.bk2`       | Stone Hill entry sequence (longest deterministic baseline) |
| `tas.bk2`                    | Multi-minute TAS-style input (broad coverage)              |
| `titlescreen-menu.bk2`       | Title → memory-card menu (2D sprite mirror/flip; frame 450)|

## When the matrix harness fails on a BK2

Read the failure tag, then:

- **expected-red** — the BK2 surfaces an `open` channel; not a
  regression. Open channels are tracked in
  `manifest/render_channels.toml`.
- **REGRESSION** — a `gated` channel diverged. The classifier
  output names which channel and which state region; cross-
  reference with `code_generator.cpp` substitution sites for that
  channel.
- **BROKEN (no hash log)** — openpete crashed or hung; check
  `/tmp/determinism-matrix/log-*.txt` (rerun with `--keep-logs`).

See `notes/plans/openpete-determinism-enforcement.md` for the
full enforcement plan.
