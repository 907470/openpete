#!/usr/bin/env python3
"""check_game_includes.py - refuse a host TU whose include path leads into a
game's directory.

The one executable links every game's host code, and the code shared by the
games compiles once. A header found through an include directory inside one
game's directory (games/<id>/ in the source tree, games/<id>/ in the build
tree) would let a TU's meaning depend on which game's directory is on its
path. The invariant: no host TU resolves a game-local header by bare name.
A game's header is reached relative to the including file, or by its
game-qualified path under a game root (games/ in either tree), which names
the game.

Reads the compile database and checks every TU whose object belongs to one
of --targets (an object under CMakeFiles/<target>.dir/): each -I, -iquote,
-isystem and -idirafter directory, lexically and with links resolved, must
lie outside every --games directory of both trees.

Exit 0 clean, 1 refused (each violation on stderr), 2 usage or input error.
"""
import argparse
import json
import os
import shlex
import sys

FLAGS = ("-I", "-iquote", "-isystem", "-idirafter")


def include_dirs(args, cwd):
    """Yield every include directory a compile command names, absolute."""
    i = 0
    while i < len(args):
        a = args[i]
        for f in FLAGS:
            if a == f and i + 1 < len(args):
                i += 1
                yield os.path.normpath(os.path.join(cwd, args[i]))
                break
            if a.startswith(f) and len(a) > len(f):
                yield os.path.normpath(os.path.join(cwd, a[len(f):]))
                break
        i += 1


def output_of(args):
    """The object a compile command writes, or ''."""
    for i, a in enumerate(args):
        if a == "-o" and i + 1 < len(args):
            return args[i + 1]
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--compile-db", required=True)
    ap.add_argument("--source-dir", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--games", nargs="+", required=True)
    ap.add_argument("--targets", nargs="+", required=True)
    ap.add_argument("--stamp")
    a = ap.parse_args()

    if a.stamp and os.path.exists(a.stamp):
        os.remove(a.stamp)
    try:
        with open(a.compile_db) as f:
            db = json.load(f)
    except (OSError, ValueError) as e:
        print(f"check_game_includes: {a.compile_db}: {e}", file=sys.stderr)
        return 2

    roots = []
    for tree in (a.source_dir, a.build_dir):
        for g in a.games:
            d = os.path.normpath(os.path.join(tree, "games", g))
            roots.append(d)
            roots.append(os.path.realpath(d))
    roots = sorted(set(roots))
    markers = [f"/CMakeFiles/{t}.dir/" for t in a.targets]

    errors = []
    checked = 0
    for e in db:
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        out = e.get("output") or output_of(args)
        if not any(m in "/" + out for m in markers):
            continue
        checked += 1
        for d in include_dirs(args, e["directory"]):
            for cand in {d, os.path.realpath(d)}:
                hit = next((r for r in roots
                            if cand == r or cand.startswith(r + os.sep)), None)
                if hit:
                    errors.append(f"{os.path.relpath(e['file'], a.source_dir)}: "
                                  f"include directory {d} lies in {hit}")
                    break
    if checked == 0:
        print("check_game_includes: no TU of the named targets in the compile "
              "database", file=sys.stderr)
        return 2
    if errors:
        print(f"check_game_includes: REFUSED ({len(errors)} violation(s)): a "
              "host TU resolves headers through a game's directory; name the "
              "header relative to the including file or by its game-qualified "
              "path (games/common/game_build.h)", file=sys.stderr)
        for line in sorted(set(errors)):
            print(f"  {line}", file=sys.stderr)
        return 1
    print(f"check_game_includes: {checked} host TUs, no include directory "
          "inside a game's directory")
    if a.stamp:
        with open(a.stamp, "w") as f:
            f.write(f"{checked}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
