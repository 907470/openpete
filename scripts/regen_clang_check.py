#!/usr/bin/env python3
"""Compile one game's generated C with the bundled clang, as first boot does.

The shipped recompiled module is compiled on the player's machine at first
boot by the bundled clang 18 with `-w` (games/common/recomp_build.cpp).
clang 16 and later refuse some diagnostics even under `-w` (an int/pointer
conversion among them), so generated C the project's GCC build accepts can
still fail first boot. This check runs at the end of
scripts/regen-recomp.sh: every entry of the linux-release compile database
whose source lies under games/<game>/generated/ is re-run `-fsyntax-only -w`
with its compiler replaced by <checkout root>/.tools/clang/bin/clang. The
checkout root is the parent of the git common directory, so a worktree
uses the main checkout's clang.

The kit's include layout (`recomp/src`, `-nostdlibinc`) exists only in
launcher builds and is not reproduced; the database's include set stands in
for it.

Run:  python3 scripts/regen_clang_check.py <game> [--jobs N]
Exit: 0 every TU compiled, or SKIP (no bundled clang, no database, no
      entry for the game); 1 a TU failed, each named with its output.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
DATABASE = os.path.join(REPO, "build", "linux-release", "compile_commands.json")


def checkout_root():
    """Return the main checkout's root: the parent of the git common directory."""
    common = subprocess.run(
        ["git", "-C", REPO, "rev-parse", "--path-format=absolute",
         "--git-common-dir"],
        check=True, capture_output=True, text=True).stdout.strip()
    return os.path.dirname(os.path.realpath(common))


def entry_args(entry):
    """Return an entry's argument list, from `arguments` or a split `command`."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def syntax_args(args, clang):
    """Return args with the compiler replaced by clang, `-c` and `-o <obj>` removed, and `-fsyntax-only -w` appended."""
    out = [clang]
    skip = False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c":
            continue
        out.append(a)
    return out + ["-fsyntax-only", "-w"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--jobs", type=int, default=4)
    opt = ap.parse_args()

    clang = os.path.join(checkout_root(), ".tools", "clang", "bin", "clang")
    if not os.access(clang, os.X_OK):
        print(f"[regen] clang check: SKIP - no bundled clang at {clang}")
        return 0
    if not os.path.isfile(DATABASE):
        print(f"[regen] clang check: SKIP - no compile database at {DATABASE}")
        return 0

    gen = os.path.realpath(os.path.join(REPO, "games", opt.game, "generated")) + os.sep
    with open(DATABASE) as f:
        entries = [e for e in json.load(f)
                   if e["file"].endswith(".c")
                   and os.path.realpath(e["file"]).startswith(gen)]
    if not entries:
        print(f"[regen] clang check: SKIP - {DATABASE} has no entry under {gen}")
        return 0

    def run(entry):
        args = syntax_args(entry_args(entry), clang)
        r = subprocess.run(args, cwd=entry["directory"], capture_output=True,
                           text=True)
        return entry["file"], r.returncode, r.stdout + r.stderr

    with ThreadPoolExecutor(max_workers=max(1, opt.jobs)) as pool:
        results = list(pool.map(run, entries))
    failed = [(f, out) for f, rc, out in results if rc != 0]
    for f, out in failed:
        print(f"[regen] clang check: FAIL {f}\n{out}", file=sys.stderr)
    print(f"[regen] clang check: {len(results) - len(failed)}/{len(results)} "
          f"generated TU(s) of {opt.game} compile under {clang}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
