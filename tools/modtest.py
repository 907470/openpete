#!/usr/bin/env python3
"""modtest.py — the mod test runner ([test] block in mod.toml).

The P1 "modtest harness" of notes/plans/openpete-modding-framework.md:
a mod declares its own regression test in its manifest; this runner
replays it and checks the assertions. In-tree PoC rungs run through it
in CI (scripts/check_mods.sh); third-party modders run the *identical*
harness locally:

    tools/modtest.py mods/my-mod

mod.toml schema:

    [test]
    args = "--skip-to-level artisans" # optional extra engine flags
    frames = 8000              # run window (--frame-limit)
    expect_oracle = "pass"     # "pass" | "fail" | "skip" (default skip)
                               #   pass: PSX-oracle byte-exact (observer
                               #         mods — the neutrality gate)
                               #   fail: oracle MUST diverge (gameplay
                               #         mods — expected divergence)
                               #   skip: plain headless run, log
                               #         assertions only
    expect_log = ["..."]       # substrings that must appear in the log
    input = "test/movie.bk2"   # optional bk2 replay, relative to mod dir

The fixture is composed automatically: the mod + its hard-dependency
closure (deps resolved as sibling directories), enabled in
dependency-first order.

Env: OPENPETE_BIN, OPENPETE_DISC — same defaults as check_mods.sh.
Exit 0 = all mods green; 1 = any failure.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ARR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def strip_comment(line: str) -> str:
    """Cut a trailing # comment, respecting quoted strings ('#' inside
    quotes is content — e.g. expect_log = ["call #1"])."""
    out = []
    in_str = False
    escape = False
    for ch in line:
        if escape:
            out.append(ch)
            escape = False
            continue
        if ch == "\\" and in_str:
            out.append(ch)
            escape = True
            continue
        if ch == '"':
            in_str = not in_str
        elif ch == "#" and not in_str:
            break
        out.append(ch)
    return "".join(out).strip()


def parse_manifest(mod_dir: Path):
    """Tiny targeted parse: [mod] id/deps + [test] block keys."""
    text = (mod_dir / "mod.toml").read_text(encoding="utf-8")
    section = None
    m = {"id": mod_dir.name, "deps": [], "test": None}
    for line in text.splitlines():
        line = strip_comment(line)
        if not line:
            continue
        if line.startswith("["):
            section = line.strip("[]").strip()
            if section == "test":
                m["test"] = {"frames": 8000, "expect_oracle": "skip",
                             "expect_log": [], "input": None, "args": None}
            continue
        if "=" not in line:
            continue
        key, val = [x.strip() for x in line.split("=", 1)]
        if section == "mod":
            if key == "id":
                m["id"] = val.strip('"')
            elif key == "deps":
                m["deps"] = ARR_RE.findall(val)
        elif section == "test" and m["test"] is not None:
            if key == "frames":
                m["test"]["frames"] = int(val)
            elif key == "expect_oracle":
                m["test"]["expect_oracle"] = val.strip('"')
            elif key == "expect_log":
                m["test"]["expect_log"] = ARR_RE.findall(val)
            elif key == "input":
                m["test"]["input"] = val.strip('"')
            elif key == "args":
                m["test"]["args"] = val.strip('"')
    return m


def dep_closure(mod_dir: Path):
    """[dep dirs...] + mod_dir, dependency-first (siblings by id)."""
    ordered, seen = [], set()

    def visit(d: Path):
        mid = d.name
        if mid in seen:
            return
        seen.add(mid)
        for dep in parse_manifest(d)["deps"]:
            sib = d.parent / dep
            if (sib / "mod.toml").exists():
                visit(sib)
        ordered.append(d)

    visit(mod_dir.resolve())
    return ordered


def run_one(mod_dir: Path, bin_path: str, disc: str, verbose: bool):
    man = parse_manifest(mod_dir)
    test = man["test"]
    mid = man["id"]
    if test is None:
        print(f"[modtest] {mid}: no [test] block — SKIP")
        return True

    with tempfile.TemporaryDirectory(prefix="openpete-modtest-") as tmp:
        fixture = Path(tmp)
        with (fixture / "enabled.txt").open("w") as en:
            for d in dep_closure(mod_dir):
                (fixture / d.name).symlink_to(d)
                en.write(d.name + "\n")

        frames = str(test["frames"])
        mode = test["expect_oracle"]
        env = dict(os.environ)
        if mode in ("pass", "fail"):
            env["OPENPETE_FRAME_LIMIT"] = frames
            cmd = [str(REPO / "scripts/check_psx_oracle.sh"),
                   "--", "--mods-dir", str(fixture)]
        else:
            cmd = [bin_path, "--headless", "--disc", disc,
                   "--run-recomp", "0x80012204",
                   "--frame-limit", frames, "--mods-dir", str(fixture)]
        if test["input"]:
            cmd += ["--bk2", str(mod_dir / test["input"]), "--bk2-stop-on-end"]
        if test["args"]:
            # engine flags; in oracle mode cmd already ends with the
            # post-`--` engine-arg tail, so appending works in both modes
            cmd += shlex.split(test["args"])


        r = subprocess.run(cmd, cwd=REPO, env=env, timeout=600,
                           capture_output=True, text=True)
        out = r.stdout + r.stderr

        ok = True
        if mode == "pass" and "verdict: PASS" not in out:
            print(f"[modtest] {mid}: FAIL — oracle did not PASS")
            ok = False
        if mode == "fail" and "verdict: FAIL" not in out:
            print(f"[modtest] {mid}: FAIL — oracle did not FAIL "
                  "(expected divergence missing)")
            ok = False
        for needle in test["expect_log"]:
            if needle not in out:
                print(f"[modtest] {mid}: FAIL — log missing: {needle!r}")
                ok = False
        if ok:
            print(f"[modtest] {mid}: PASS ({mode}, {frames} frames, "
                  f"{len(test['expect_log'])} log asserts)")
        elif verbose:
            print(out[-3000:])
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mods", nargs="+", type=Path)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    bin_path = os.environ.get(
        "OPENPETE_BIN",
        str(REPO / "build/linux-release/games/spyro-1/openpete-spyro1"))
    disc = os.environ.get(
        "OPENPETE_DISC",
        str(REPO / "games/spyro-1/spyro-1/build/SCUS-94228.cue"))

    ok = True
    for mod_dir in args.mods:
        ok &= run_one(mod_dir, bin_path, disc, args.verbose)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
