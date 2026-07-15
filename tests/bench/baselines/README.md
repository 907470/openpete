# Performance baselines (bench-corpus)

Blessed `rollup.json` files, one per platform tag, for regression detection:

```
scripts/bench-corpus.py --exe <exe> --baseline tests/bench/baselines/<platform>.json
```

The platform tag is `<build-preset>/<gpu>` (e.g. `windows-mingw-opt/NVIDIA...`,
or `linux-release/SOFTWARE` for llvmpipe). A compare against a baseline from a
different platform tag refuses (exit 64) — CPU-only llvmpipe numbers are not
comparable to a real-GPU run.

To (re)bless a baseline after an intentional perf change, run the full corpus on
the target platform and copy its `out/bench/<run-id>/rollup.json` here, reviewing
the diff first. GPU numbers are only meaningful from a real-GPU (Windows) run;
the `linux-release/SOFTWARE` baseline gates CPU scopes only.

No baseline is committed yet — capture one once the corpus sweep is run on a
stable machine.
```
