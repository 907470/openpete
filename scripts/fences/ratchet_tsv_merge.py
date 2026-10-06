#!/usr/bin/env python3
"""Git merge driver for the ratchet baselines (baselines/*/*.tsv).

Registered by .gitattributes (`merge=ratchet-tsv`) and armed per clone:
  git config merge.ratchet-tsv.driver 'python3 scripts/fences/ratchet_tsv_merge.py %O %A %B %P'

A baseline is a set of rows keyed by their first two columns (file,
function) under a header of `#` lines and one column line. Every
hook-written file records the HEAD it was computed against in `# head:`,
so a text merge of two branches cut from different commits conflicts on
that line even when no row differs. This driver merges three ways by key
over base (%O), ours (%A) and theirs (%B):

  - a row equal on both sides is kept;
  - a row changed, added or deleted on one side only takes that side;
  - a row changed differently on both sides (or added on both with
    different values) is a conflict.

A row deleted on either side stays deleted: a union of rows would
resurrect a row the other side retired. The header is ours (the branch
being merged into, the default branch under a cherry-pick or rebase): its
`# head:` names a commit in the result's history, and the hook recomputes
the file on the next commit that stages a scope path. Headers whose lines
other than `written-by:` and `head:` differ (another tool version or
options: the rows are not comparable) conflict as a whole.

On a conflict the driver writes ours with conflict markers around each
conflicting row and exits 1; git leaves the path unmerged.

The driver overwrites %A, so it refuses, writing nothing, any invocation
that is not git's: git passes exactly four arguments, %O %A %B are
untracked temporary files (`.merge_file_XXXXXX` at the worktree top, the
driver's working directory) and %P is a baseline path. Each of %O %A %B
must hold a baseline (a `#` header over a `file\tfunction...` column line);
%O alone may be empty (both sides added the path).
"""
import fnmatch
import re
import subprocess
import sys

DRIVER_NAME = "ratchet-tsv"
DRIVER_CONFIG = "python3 scripts/fences/ratchet_tsv_merge.py %O %A %B %P"
ARM_MERGE = "git config merge.%s.driver '%s'" % (DRIVER_NAME, DRIVER_CONFIG)
VOLATILE = re.compile(r"#\s*(written-by|head):")


def split(text):
    """-> (header lines, {key: row line}, key order). The header is every
    line up to and including the first non-`#` line (the column line)."""
    header, rows, order = [], {}, []
    lines = text.splitlines()
    i = 0
    while i < len(lines) and lines[i].startswith("#"):
        header.append(lines[i])
        i += 1
    if i < len(lines):
        header.append(lines[i])
        i += 1
    for line in lines[i:]:
        if not line:
            continue
        cols = line.split("\t")
        key = "\t".join(cols[:2])
        if key not in rows:
            order.append(key)
        rows[key] = line
    return header, rows, order


def stable(header):
    return [h for h in header if not VOLATILE.match(h)]


def merge(base, ours, theirs):
    """-> (text, conflicts). Pure; the three inputs are file contents."""
    bh, brows, _ = split(base)
    oh, orows, _ = split(ours)
    th, trows, _ = split(theirs)
    if stable(oh) != stable(th):
        return ("<<<<<<< ours\n" + ours + "=======\n" + theirs
                + ">>>>>>> theirs\n"), 1
    out, conflicts = [], 0
    # The fences render rows sorted by `file:function`.
    for key in sorted(set(brows) | set(orows) | set(trows),
                      key=lambda k: k.replace("\t", ":", 1)):
        b, o, t = brows.get(key), orows.get(key), trows.get(key)
        if o == t:
            pick = o
        elif o == b:
            pick = t
        elif t == b:
            pick = o
        else:
            conflicts += 1
            out.append("<<<<<<< ours")
            if o is not None:
                out.append(o)
            out.append("=======")
            if t is not None:
                out.append(t)
            out.append(">>>>>>> theirs")
            continue
        if pick is not None:
            out.append(pick)
    return "\n".join(oh + out) + "\n", conflicts


USAGE = ("usage: ratchet_tsv_merge.py %O %A %B %P -- git merge driver for "
         "baselines/*/*.tsv, run by git only (arm: " + ARM_MERGE + ")\n")


def is_baseline(text):
    """True when text has a baseline's shape: `#` header lines, then a
    column line whose first two columns are `file` and `function`."""
    lines = text.splitlines()
    i = 0
    while i < len(lines) and lines[i].startswith("#"):
        i += 1
    return i > 0 and i < len(lines) and lines[i].split("\t")[:2] == [
        "file", "function"]


def tracked(path):
    """True when path is a file git tracks in the current repository."""
    try:
        p = subprocess.run(["git", "ls-files", "--error-unmatch", "--", path],
                           capture_output=True)
    except OSError:
        return False
    return p.returncode == 0


def read_inputs(argv):
    """-> (texts of %O %A %B, None) for git's driver invocation, else
    (None, the reason to refuse). Writes nothing."""
    if len(argv) != 4:
        return None, "expected 4 arguments, got %d" % len(argv)
    if not fnmatch.fnmatch(argv[3], "baselines/*/*.tsv"):
        return None, "%%P %r is not a baseline path" % argv[3]
    roles = list(zip(("%O", "%A", "%B"), argv[:3]))
    for name, path in roles:
        if tracked(path):
            return None, "%s %r is a tracked file, not a merge temporary" % (
                name, path)
    texts = []
    for name, path in roles:
        try:
            with open(path, encoding="utf-8", errors="surrogateescape") as fh:
                texts.append(fh.read())
        except OSError as e:
            return None, "%s %r: %s" % (name, path, e.strerror)
        if not is_baseline(texts[-1]) and not (name == "%O" and not texts[-1]):
            return None, "%s %r is not a ratchet baseline TSV" % (name, path)
    return texts, None


def main(argv):
    texts, why = read_inputs(argv)
    if why:
        sys.stderr.write("ratchet-tsv merge: refused, nothing written: %s\n%s"
                         % (why, USAGE))
        return 2
    text, conflicts = merge(*texts)
    with open(argv[1], "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)
    if conflicts:
        sys.stderr.write("ratchet-tsv merge: %s: %d row(s) changed on both "
                         "sides; resolve the marked rows.\n"
                         % (argv[3], conflicts))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
