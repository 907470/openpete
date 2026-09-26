#!/usr/bin/env python3
"""Fence: no image is committed under tests/golden outside the tracked
exception set.

The presentation golden's verdict is a digest manifest (blessed.tsv); its
tiles are generated and live in a per-user store, never in the repository,
because committed PNGs do not delta and grow the history by every promoted
tile. The files committed under tests/golden are `.tsv`, `.md` and the PNGs
of the tracked exception set: golden directories no recipe reproduces, whose
PNGs stay tracked as named exceptions.

Subject: the paths this commit stages as added, copied, modified or renamed
(`git diff --cached --name-only --diff-filter=ACMR`), never the tree and
never the whole index, so PNGs already tracked do not refuse a commit that
does not stage them. `R` and `C` are in the filter because git detects
renames and copies by default and prints the destination path; a `git mv`
of a PNG or a directory into tests/golden stages as `R`. The scan covers
tests/golden at any depth, not one directory name.

Residual: a golden corpus committed outside tests/golden escapes the scan;
a new golden directory outside tests/golden extends GOLDEN_ROOTS in the
change that adds it.

Wiring: githooks/pre-commit runs `hook`.

Escape: OPENPETE_GOLDEN_PNG_ACCEPT="<reason>" on the commit. The reason is
recorded with the staged tree and the refused paths in
<git-common-dir>/golden-png-guard.log, and admits one commit: the same
reason on a different staged tree is refused. A script line carrying
`golden-png-guard: intentional` is exempt from the write-time check.

Run: python3 scripts/fences/golden-png-guard.py hook
     python3 scripts/fences/golden-png-guard.py paths <path>...
Exit: 0 clean, 1 refused.
"""
import datetime
import os
import re
import shlex
import subprocess
import sys

GOLDEN_ROOTS = ("tests/golden/",)
# Directories whose PNGs stay tracked: the native renderer's tolerance golden
# (tests/harness/check_native_visual.sh), which no recipe reproduces.
EXCEPTIONS = ("tests/golden/spyro-1/native-vk/",)
ALLOWED_SUFFIXES = (".tsv", ".md")
ENV = "OPENPETE_GOLDEN_PNG_ACCEPT"
MARKER = "golden-png-guard: intentional"
LOG = "golden-png-guard.log"


def normalize(path):
    """Repo-relative form of a path naming tests/golden: an absolute or
    ./-prefixed path is cut at its first `tests/golden/`."""
    p = path.replace(os.sep, "/")
    while p.startswith("./"):
        p = p[2:]
    for root in GOLDEN_ROOTS:
        i = p.find("/" + root)
        if not p.startswith(root) and i >= 0:
            p = p[i + 1:]
    return p


def refused(path):
    """True when a path under a golden root is neither a .tsv, a .md, nor a
    PNG of the tracked exception set."""
    p = normalize(path)
    if not p.startswith(GOLDEN_ROOTS):
        return False
    if p.endswith(ALLOWED_SUFFIXES):
        return False
    return not (p.endswith(".png") and p.startswith(EXCEPTIONS))


def staged_paths():
    """Paths the index stages as added, copied, modified or renamed."""
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
        capture_output=True, text=True, check=True).stdout
    return [p for p in out.split("\0") if p]


def reason_ok(reason):
    """A bypass reason carries a letter and at least eight characters."""
    return bool(reason) and len(reason.strip()) >= 8 and \
        re.search(r"[A-Za-z]", reason) is not None


def log_path():
    """The bypass log in the repository's common git directory."""
    d = subprocess.run(["git", "rev-parse", "--git-common-dir"],
                       capture_output=True, text=True, check=True).stdout.strip()
    return os.path.join(os.path.abspath(d), LOG)


def bypass(bad, reason):
    """Admit one commit under a declared reason. Returns an error message,
    or None when admitted (and recorded)."""
    if not reason_ok(reason):
        return ("%s=%r is not a reason: write why these paths are committed "
                "(8+ characters, words)." % (ENV, reason))
    tree = subprocess.run(["git", "write-tree"], capture_output=True,
                          text=True, check=True).stdout.strip()
    lp = log_path()
    entries = []
    if os.path.isfile(lp):
        with open(lp) as fh:
            entries = [ln.rstrip("\n").split("\t") for ln in fh]
    for e in entries:
        if len(e) >= 3 and e[2] == reason:
            if e[1] == tree:
                return None
            return ("%s reason %r already admitted the commit of tree %s; a "
                    "bypass admits one commit. Write a new reason for this one."
                    % (ENV, reason, e[1][:12]))
    with open(lp, "a") as fh:
        fh.write("%s\t%s\t%s\t%s\n" % (
            datetime.datetime.now().isoformat(timespec="seconds"), tree,
            reason, " ".join(bad)))
    return None


def hook():
    """Pre-commit entry: refuse a staged path under a golden root outside the
    allowed set, unless the declared escape admits this commit."""
    bad = [p for p in staged_paths() if refused(p)]
    if not bad:
        return 0
    reason = os.environ.get(ENV)
    if reason is not None:
        err = bypass(bad, reason)
        if err is None:
            sys.stderr.write("golden-png-guard: BYPASSED (%s) for %d path(s): "
                             "%s\n" % (reason, len(bad), log_path()))
            return 0
        sys.stderr.write("golden-png-guard: REFUSED  %s\n" % err)
        return 1
    sys.stderr.write(
        "golden-png-guard: REFUSED  this commit stages %d file(s) under "
        "tests/golden that are not .tsv, .md or a PNG of the tracked exception "
        "set (%s):\n" % (len(bad), ", ".join(EXCEPTIONS)))
    for p in bad[:20]:
        sys.stderr.write("    %s\n" % p)
    if len(bad) > 20:
        sys.stderr.write("    ... %d more\n" % (len(bad) - 20))
    sys.stderr.write(
        "  Golden tiles are verdict digests in blessed.tsv; the images live in\n"
        "  the store (scripts/presentation-blessed.py). Unstage them with\n"
        "  `git restore --staged <path>`. A deliberate exception: set\n"
        "  %s=\"<reason>\" on this one commit.\n" % ENV)
    return 1


def offends(text):
    """Lines of a script whose `git add` names a refused path, as
    (line number, line); a line carrying the marker is exempt."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if MARKER in line or not re.search(r"\bgit\b.*\badd\b", line):
            continue
        try:
            toks = shlex.split(line, comments=True)
        except ValueError:
            toks = line.split()
        for i, t in enumerate(toks):
            if t == "git" and "add" in toks[i + 1:]:
                j = toks.index("add", i + 1)
                if any(refused(a) for a in toks[j + 1:]
                       if not a.startswith("-")):
                    out.append((n, line.strip()))
                break
    return out


def main():
    """CLI: `hook` (pre-commit) or `paths <path>...` (print refused ones)."""
    mode = sys.argv[1] if len(sys.argv) > 1 else "hook"
    if mode == "hook":
        return hook()
    if mode == "paths":
        bad = [p for p in sys.argv[2:] if refused(p)]
        for p in bad:
            print(p)
        return 1 if bad else 0
    sys.stderr.write("usage: golden-png-guard.py hook | paths <path>...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
