#!/usr/bin/env python3
"""Fence: the DuckStation series carries no upstream text.

The series in third_party/duckstation-patches/ is files of openpete's own
(`files/`) and the lines it inserts into upstream files (the `+` lines of
`hooks.txt`). This fence compares each such line with every line of the
pinned upstream tree (the commit third_party/duckstation's gitlink names,
read from the submodule's object store) and refuses a line that occurs
there when it holds at least MIN_CHARS characters after stripping
whitespace and is neither an `#include` line nor a row of ALLOW. Shorter
matches are braces, keywords and idioms any C++ file shares.

Run:
  python3 scripts/fences/upstream_text_census.py tree [root]   the tracked series files (build target)
  python3 scripts/fences/upstream_text_census.py hook [root]   the staged series files, when any is staged
Options: --ds DIR and --pin REV name the upstream repository and commit
(default: root/third_party/duckstation, else the main checkout's module
repository for it, and its gitlink).
Exit: 0 clean (or SKIP: the pin's objects are not available, said on
stdout); 66 a refused line.
rumble-guard: no-launch, wslenv-guard: no-launch
"""
import os
import subprocess
import sys

SERIES = "third_party/duckstation-patches/"
MIN_CHARS = 30

# (stripped line, reason): series lines that match an upstream line and are
# not upstream expression.
ALLOW = (
    ("while (std::fgets(line, sizeof(line), fp))", "the C library's line-reading idiom"),
    ("const auto lock = Core::GetSettingsLock();", "a call of the core's settings-lock interface"),
    ("MemoryCardImage::DataArray data;", "a declaration of the card-image interface type"),
    ("for (size_t i = 0; i < n; i++) {", "a counting loop"),
    ("std::vector<std::string> lines;", "a declaration"),
    ("SPU::GeneratePendingSamples();", "a call of the SPU interface"),
    ("// " + "-" * 75, "a separator rule"),
)
ALLOWED = {a for a, _ in ALLOW}


def git(cwd, *args, data=None):
    """Run git in cwd and return stdout as bytes; None on failure."""
    p = subprocess.run(["git", "-C", cwd] + list(args), input=data, capture_output=True)
    return p.stdout if p.returncode == 0 else None


def upstream_lines(ds, pin):
    """Set of stripped non-blank lines of every text blob at pin, or None when the objects are absent."""
    listing = git(ds, "ls-tree", "-r", pin)
    if listing is None:
        return None
    oids = [r.split()[2] for r in listing.decode().splitlines() if r.split()[1] == "blob"]
    data = git(ds, "cat-file", "--batch", data=("\n".join(oids) + "\n").encode())
    out, pos = set(), 0
    while data and pos < len(data):
        nl = data.index(b"\n", pos)
        size = int(data[pos:nl].split()[2])
        body = data[nl + 1:nl + 1 + size]
        pos = nl + 2 + size
        if b"\0" in body[:8192]:
            continue
        out.update(s for s in (l.strip() for l in body.split(b"\n")) if s)
    return out


def series_lines(rel, data):
    """(line number, stripped text) of the lines the series contributes in file rel."""
    hooks = rel.endswith("hooks.txt")
    for n, l in enumerate(data.split(b"\n"), 1):
        if hooks:
            if not l.startswith(b"+"):
                continue
            l = l[1:]
        yield n, l.strip()


def scan(files, upstream):
    """Refusals over {rel: bytes}."""
    refused = []
    for rel, data in sorted(files.items()):
        for n, s in series_lines(rel, data):
            if len(s) < MIN_CHARS or s.startswith(b"#include") or s not in upstream:
                continue
            text = s.decode("utf-8", "replace")
            if text not in ALLOWED:
                refused.append("%s:%d: %s" % (rel, n, text))
    return refused


def in_scope(rel):
    return rel.startswith(SERIES + "files/") or rel == SERIES + "hooks.txt"


def collect(root, mode):
    """{rel: bytes} of the series files: tracked work-tree files, or the staged blobs in hook mode."""
    if mode == "hook":
        names = git(root, "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
        staged = [r for r in (names or b"").decode().split("\0") if r and in_scope(r)]
        if not staged:
            return {}
        rels = [r for r in git(root, "ls-files", "-z", "--", SERIES).decode().split("\0") if r and in_scope(r)]
        return {r: git(root, "show", ":" + r) or b"" for r in rels}
    out = {}
    for rel in (git(root, "ls-files", "-z", "--", SERIES) or b"").decode().split("\0"):
        if rel and in_scope(rel) and os.path.isfile(os.path.join(root, rel)):
            with open(os.path.join(root, rel), "rb") as fh:
                out[rel] = fh.read()
    return out


def main(argv):
    """Run the fence in tree or hook mode."""
    args = argv[1:]
    opts = {}
    for k in ("--ds", "--pin"):
        if k in args:
            i = args.index(k)
            opts[k] = args[i + 1]
            del args[i:i + 2]
    mode = args[0] if args else "tree"
    if mode not in ("tree", "hook"):
        print(__doc__)
        return 2
    root = os.path.abspath(args[1]) if len(args) > 1 else \
        os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    files = collect(root, mode)
    if not files:
        print("upstream-text: PASS (no series file %s)" % ("staged" if mode == "hook" else "tracked"))
        return 0
    ds = opts.get("--ds", os.path.join(root, "third_party", "duckstation"))
    pin = opts.get("--pin")
    if pin is None:
        ls = (git(root, "ls-files", "-s", "--", "third_party/duckstation") or b"").split()
        pin = ls[1].decode() if len(ls) > 1 else None
    upstream = upstream_lines(ds, pin) if pin and os.path.isdir(ds) else None
    if upstream is None and pin and "--ds" not in opts:
        # A linked worktree's submodule may be uninitialised; the main checkout's module repository holds the pin.
        common = (git(root, "rev-parse", "--path-format=absolute", "--git-common-dir") or b"").decode().strip()
        ds = os.path.join(common, "modules", "third_party", "duckstation")
        upstream = upstream_lines(ds, pin) if os.path.isdir(ds) else None
    if upstream is None:
        print("upstream-text: SKIP (the pin %s is not in %s; pass --ds DIR naming a DuckStation repository that "
              "holds it)" % (pin, ds))
        return 0
    refused = scan(files, upstream)
    for r in refused:
        print("upstream-text: REFUSED %s" % r)
    print("upstream-text: %s (%d series files, %d refused, pin %s)"
          % ("FAIL" if refused else "PASS", len(files), len(refused), pin[:12]))
    if refused:
        print("upstream-text: write the line in the series' own words, or add an ALLOW row with the reason it "
              "is not upstream expression")
    return 66 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
