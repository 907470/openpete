#!/usr/bin/env python3
"""strip-comments.py - remove C comments from the shipped recomp source kit.

The recomp/src kit is compiled on the user's machine at first boot, so it
ships as readable C. Its comments, however, are internal engineering notes
(hook PCs, host-side file paths, design rationale) that have no meaning to
someone holding only the archive. Strip them at packaging time; the source
tree keeps its comments.

Preprocessor structure, string/char literals and line continuations are
preserved, so the stripped output compiles identically. Lines that held
nothing but a comment are dropped, and the blank runs that leaves are
collapsed to one.

    strip-comments.py FILE...        (rewrites each file in place)
"""

import sys


def strip(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            # The comment vanishes entirely, newlines included: the newline
            # that terminated the line it sat on is outside the comment, so
            # line structure (and any backslash continuation) is unaffected.
            i = end
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            end = src.find("\n", i)
            i = n if end < 0 else end
            continue
        out.append(c)
        i += 1
    return "".join(out)


def tidy(text):
    lines = [ln.rstrip() for ln in text.split("\n")]
    out, blank = [], False
    for ln in lines:
        if ln:
            out.append(ln)
            blank = False
        elif not blank:
            out.append(ln)
            blank = True
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return "\n".join(out) + "\n"


def main(paths):
    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
        with open(path, "w", encoding="utf-8") as f:
            f.write(tidy(strip(src)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(64)
    sys.exit(main(sys.argv[1:]))
