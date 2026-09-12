#!/usr/bin/env python3
"""modtest.py — the mod test runner ([test] block in modtest.toml).

The P1 "modtest harness" of notes/plans/openpete-modding-framework.md:
a mod declares its own regression test in its manifest; this runner
replays it and checks the assertions. In-tree PoC rungs run through it
in CI (scripts/check_mods.sh); third-party modders run the *identical*
harness locally:

    tools/modtest.py mods/my-mod

modtest.toml schema (CI-side, next to mod.toml):

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
    requires = ["path", ...]   # inputs the test cannot run without,
                               #   relative to the repo root. Any one
                               #   missing SKIPs the mod, naming the
                               #   absent path. For mods whose assets are
                               #   derived locally from a disc and are
                               #   never committed.

The fixture is composed automatically: the mod + its hard-dependency
closure (deps resolved as sibling directories), enabled in
dependency-first order.

Env: OPENPETE_BIN, OPENPETE_DISC — same defaults as check_mods.sh.
Exit 0 = all executed [test] blocks green; 1 = any failure, or
no [test] block executed at all.
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
    mt = mod_dir / "modtest.toml"
    if mt.exists():
        text += "\n" + mt.read_text(encoding="utf-8")
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
                             "expect_log": [], "input": None, "args": None,
                             "requires": []}
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
            elif key == "requires":
                m["test"]["requires"] = ARR_RE.findall(val)
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
    """Run one mod's [test] block. Returns ("pass"|"skip"|"fail", reason)."""
    man = parse_manifest(mod_dir)
    test = man["test"]
    mid = man["id"]
    if test is None:
        reason = "no [test] block"
        print(f"[modtest] {mid}: SKIP — {reason}")
        return "skip", reason

    # A test whose inputs are derived locally (converted from a disc) and
    # never committed cannot run on a fresh clone. Skip it by name rather
    # than failing on the absence or passing without the assets.
    for req in test["requires"]:
        if not (REPO / req).exists():
            reason = f"missing input {req}"
            print(f"[modtest] {mid}: SKIP — {reason}")
            return "skip", reason

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

        # The oracle reports 64 when the golden trace, the binary, or the
        # disc is absent. Nothing was compared, so the mod is skipped by
        # name rather than counted green.
        if mode in ("pass", "fail") and r.returncode == 64:
            reason = "psx oracle prerequisites missing (exit 64)"
            print(f"[modtest] {mid}: SKIP — {reason}")
            return "skip", reason

        ok = True
        # Engagement floor for a plain run: a [test] block with no
        # expect_log would otherwise report PASS for a run that crashed on
        # startup. A clean run exits 0 and prints its own exit line. The
        # oracle modes are excluded because their exit code IS the verdict
        # (2 = divergence, which "fail" mode requires); the verdict-string
        # checks below carry the engagement proof there.
        if mode not in ("pass", "fail"):
            if r.returncode != 0:
                print(f"[modtest] {mid}: FAIL — run exited {r.returncode}")
                ok = False
            elif "openpete-exit: reason=" not in out:
                print(f"[modtest] {mid}: FAIL — "
                      "run printed no openpete-exit line")
                ok = False
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
        return ("pass" if ok else "fail"), ""


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

    passed, failed, skipped = [], [], []
    for mod_dir in args.mods:
        status, reason = run_one(mod_dir, bin_path, disc, args.verbose)
        if status == "pass":
            passed.append(mod_dir.name)
        elif status == "skip":
            skipped.append((mod_dir.name, reason))
        else:
            failed.append(mod_dir.name)

    n = len(args.mods)
    print(f"[modtest] summary: {n} mod(s) — {len(passed)} passed, "
          f"{len(skipped)} skipped, {len(failed)} failed")
    for mid, reason in skipped:
        print(f"[modtest]   skipped: {mid} — {reason}")
    for mid in failed:
        print(f"[modtest]   failed: {mid}")

    # A run that executed nothing proves nothing: report it as a failure
    # rather than as a green gate.
    if not passed and not failed:
        print("[modtest] no [test] block executed — VACUOUS")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
