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

## Current corpus

| BK2                          | What it exercises |
|------------------------------|-------------------|
| `frame-time-test.bk2`        | Frame-pacing under variable input |
| `inventory.bk2`              | Inventory open/close, gem/dragon/life HUD reads |
| `missing-geometry.bk2`       | Sector load order |
| `missing-world.bk2`          | Level-overlay translation edge case |
| `moby-culling-16-9.bk2`      | Cone-cull at 16:9 (m_WasDrawn channel exemplar) |
| `portal-cyclorama-issue.bk2` | Cyclorama clip-X widening (gated channel exemplar) |
| `skip-to-artisans.bk2`       | Title → first level transition |
| `stone-hill-entry.bk2`       | Stone Hill entry sequence (longest deterministic baseline) |
| `tas.bk2`                    | Multi-minute TAS-style input (broad coverage) |

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
