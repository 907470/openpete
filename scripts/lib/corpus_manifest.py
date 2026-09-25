"""Corpus manifest loading for the Python harnesses and census tools.

The Python counterpart of pg_manifest_load in scripts/presentation-game.sh,
with the same contract:
  - each game's corpus manifests sit in games/<slug>/corpus/
    (games/<slug>/manifest/ holds the recompiler's state manifests);
  - a missing manifest is FAIL: the message names the path and the process
    exits 1;
  - the row count is printed as `rows=N path=<manifest>`, the path relative
    to the repository root;
  - OPENPETE_MANIFEST_DRY_RUN=1 makes dry_run_exit() exit 0, so a tool run
    under it loads and reports its manifests and runs nothing.

load() returns the manifest's rows as stripped lines. The caller's row rule
decides which lines are rows: `line` strips from the first `#` and keeps a
line left with a non-blank character; `pipe` keeps a line whose trimmed first
`|` field is non-empty and does not start with `#`.
"""
import os
import sys

DRY_RUN_ENV = "OPENPETE_MANIFEST_DRY_RUN"


def corpus_manifest(root, game, name):
    """Absolute path of games/<game>/corpus/<name> under the repository root."""
    return os.path.join(root, "games", game, "corpus", name)


def load(root, path, form):
    """Rows of the manifest at `path` under the row rule `form`."""
    rel = os.path.relpath(path, root)
    if not os.path.isfile(path):
        sys.stderr.write("FAIL: corpus manifest missing: %s\n" % rel)
        sys.exit(1)
    rows = []
    with open(path) as f:
        for line in f:
            if form == "line":
                line = line.split("#", 1)[0].strip()
                if line:
                    rows.append(line)
            elif form == "pipe":
                first = line.split("|", 1)[0].strip()
                if first and not first.startswith("#"):
                    rows.append(line.strip())
            else:
                sys.stderr.write("FAIL: unknown row form %r for %s\n" % (form, rel))
                sys.exit(1)
    print("rows=%d path=%s" % (len(rows), rel), flush=True)
    return rows


def dry_run_exit():
    if os.environ.get(DRY_RUN_ENV, "0") == "1":
        sys.exit(0)
