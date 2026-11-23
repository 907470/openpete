#!/usr/bin/env python3
"""Apply one negative-control mutant (a unified diff) to a copy of a protocol file for the present-thread interleaving harness.

usage: apply_mutant.py <name> <source> <patch> <out>

The patch's `+++` path must name the source (its path relative to the
repository root, after the `b/` prefix). Each hunk's context and removed
lines must occur exactly in the source; the hunk may sit at a different
line than its header states (the nearest occurrence to the stated line is
taken), but never with a changed line of context. A hunk that does not
match refuses (exit 1) naming the mutant and the hunk, so a protocol-file
change that breaks a mutant fails the harness build, and the change
refreshes the mutant (docs/src/developer/present-thread-harness.md). Text
before the first `---` line is the mutant's description and is ignored.
"""

import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@")


def parse(patch_text):
    """Return (target path, hunks); each hunk is (stated old line, old lines, new lines)."""
    lines = patch_text.splitlines(keepends=True)
    target = None
    hunks = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("+++ "):
            target = ln[4:].strip()
            if target.startswith("b/"):
                target = target[2:]
        m = HUNK.match(ln)
        if m:
            old, new = [], []
            i += 1
            while i < len(lines) and lines[i][:1] in (" ", "-", "+", "\\"):
                body = lines[i]
                tag, text = body[:1], body[1:]
                if tag == "\\":
                    i += 1
                    continue
                if tag in (" ", "-"):
                    old.append(text)
                if tag in (" ", "+"):
                    new.append(text)
                i += 1
            hunks.append((int(m.group(1)), old, new))
            continue
        i += 1
    return target, hunks


def find_all(src, block):
    """Return every 0-based line index at which the list of lines block occurs in src."""
    n = len(block)
    return [k for k in range(len(src) - n + 1) if src[k:k + n] == block]


def main(argv):
    """Apply the patch and write the mutated copy; return the exit status."""
    if len(argv) != 5:
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    name, source, patch, out = argv[1:]
    with open(patch, encoding="utf-8") as f:
        target, hunks = parse(f.read())
    norm = source.replace("\\", "/")
    if not target or not norm.endswith("/" + target):
        print(f"mutant {name}: patch targets {target!r}, not {source}", file=sys.stderr)
        return 1
    if not hunks:
        print(f"mutant {name}: patch has no hunk", file=sys.stderr)
        return 1
    with open(source, encoding="utf-8") as f:
        src = f.read().splitlines(keepends=True)
    shift = 0
    for h, (stated, old, new) in enumerate(hunks, 1):
        at = find_all(src, old)
        if not at:
            print(f"mutant {name}: hunk {h} (stated line {stated}) no longer applies to {target}; "
                  f"refresh tests/sched/present/mutants/{name}.patch", file=sys.stderr)
            return 1
        want = stated - 1 + shift
        k = min(at, key=lambda x: abs(x - want))
        src[k:k + len(old)] = new
        shift += len(new) - len(old)
    with open(out, "w", encoding="utf-8") as f:
        f.writelines(src)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
