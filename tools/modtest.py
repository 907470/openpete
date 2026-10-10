#!/usr/bin/env python3
"""modtest.py - the mod test runner ([test] block in modtest.toml).

The modtest harness:
a mod declares its own regression test in its manifest; this runner
replays it and checks the assertions. In-tree PoC rungs run through it
in CI (tests/harness/check_mods.sh); third-party modders run the *identical*
harness locally:

    tools/modtest.py mods/my-mod
    tools/modtest.py --game spyro-3 mods/my-mod

modtest.toml schema (CI-side, next to mod.toml):

    [test]
    args = "--skip-to-level artisans" # optional extra engine flags
    frames = 8000              # run window (--frame-limit)
    expect_oracle = "pass"     # "pass" | "fail" | "skip" (default skip)
                               #   pass: PSX-oracle byte-exact (observer
                               #         mods - the neutrality gate)
                               #   fail: oracle MUST diverge (gameplay
                               #         mods - expected divergence)
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

    [test.spyro-2]             # optional, per game slug: assertions
    expect_log = ["..."]       #   added when --game names that game

The fixture is composed automatically: the mod + its hard-dependency
closure (deps resolved as sibling directories), enabled in
dependency-first order.

--game SLUG (default spyro-1) picks the binary, the disc and the game's
boot flags through scripts/lib/game.sh, the resolver every harness uses;
OPENPETE_BIN and OPENPETE_DISC override the first two there.
--jobs N runs up to N mods at once; each mod's output is buffered and
printed whole in argument order, so the log reads as a serial run's.
Exit 0 = all executed [test] blocks green; 1 = any failure, or
no [test] block executed at all.
"""

import argparse
import concurrent.futures
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
    quotes is content - e.g. expect_log = ["call #1"])."""
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
    m = {"id": mod_dir.name, "deps": [], "test": None, "test_game": {}}
    lines = []
    for line in text.splitlines():
        line = strip_comment(line)
        # An array value left open on its line continues on the next ones.
        if lines and lines[-1].count("[") > lines[-1].count("]") \
                and "=" in lines[-1]:
            lines[-1] += " " + line
        else:
            lines.append(line)
    for line in lines:
        if not line:
            continue
        if line.startswith("["):
            section = line.strip("[]").strip()
            if section.startswith("test."):
                m["test_game"].setdefault(section[5:], {"expect_log": []})
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
        elif section and section.startswith("test.") and key == "expect_log":
            m["test_game"][section[5:]]["expect_log"] = ARR_RE.findall(val)
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


AUDIO_RE = re.compile(r"alcOpenDevice|ALC_|AL lib:|OpenAL|PsyX_SPUAL|"
                      r"sound device|wav override")


def print_audio_device_lines(mid: str, out: str, emit):
    """Emit the run's audio-device and wav-override log lines.

    Lines the engine logs after the OpenAL check (the wav override's
    streaming and active lines) are absent when the device failed to open,
    for example alcOpenDevice returning ALC_OUT_OF_MEMORY; these lines show
    the device state beside the missing-line failure."""
    lines = [ln for ln in out.splitlines() if AUDIO_RE.search(ln)]
    emit(f"[modtest] {mid}: audio-device lines ({len(lines)}):")
    for ln in lines[:20]:
        emit(f"[modtest]   {ln.strip()}")


def resolve_game(game: str):
    """(binary, disc, boot flags) for a game slug, from scripts/lib/game.sh's
    pg_resolve, which honours OPENPETE_BIN and OPENPETE_DISC."""
    r = subprocess.run(
        ["bash", "-c", 'source scripts/lib/game.sh && '
         'GAME="$(pg_parse_game --game "$1")" && pg_resolve "$GAME" && '
         'printf "%s\\n%s\\n%s\\n" "$PG_BIN" "$PG_DISC" "${PG_RUN_ARGS[*]-}"',
         "_", game], cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"modtest: cannot resolve game {game!r}: {r.stderr.strip()}")
    bin_path, disc, run_args = (r.stdout.split("\n") + ["", "", ""])[:3]
    if not Path(bin_path).is_absolute():
        bin_path = str(REPO / bin_path)
    if disc and not Path(disc).is_absolute():
        disc = str(REPO / disc)
    return bin_path, disc, shlex.split(run_args)


def run_one(mod_dir: Path, game, verbose: bool, emit,
            concurrent_run: bool = False):
    """Run one mod's [test] block. Returns ("pass"|"skip"|"fail", reason).

    Every report line goes through emit (one line per call, as print would
    write it), so a caller running mods concurrently can buffer each mod's
    lines and print them in order. With concurrent_run set, an inherited
    OPENPETE_ORACLE_LOG path gains a ".<mod id>" suffix so two oracle runs
    never write one file. `game` is (slug, binary, disc, boot flags)."""
    slug, bin_path, disc, run_args = game
    man = parse_manifest(mod_dir)
    test = man["test"]
    mid = man["id"]
    if test is None:
        reason = "no [test] block"
        emit(f"[modtest] {mid}: SKIP - {reason}")
        return "skip", reason
    expect_log = test["expect_log"] + \
        man["test_game"].get(slug, {}).get("expect_log", [])

    # A test whose inputs are derived locally (converted from a disc) and
    # never committed cannot run on a fresh clone. Skip it by name rather
    # than failing on the absence or passing without the assets.
    for req in test["requires"]:
        if not (REPO / req).exists():
            reason = f"missing input {req}"
            emit(f"[modtest] {mid}: SKIP - {reason}")
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
        env["OPENPETE_VIBRATION"] = "0"
        if mode in ("pass", "fail"):
            env["OPENPETE_FRAME_LIMIT"] = frames
            if concurrent_run and env.get("OPENPETE_ORACLE_LOG"):
                env["OPENPETE_ORACLE_LOG"] += f".{mid}"
            cmd = [str(REPO / "tests/harness/check_psx_oracle.sh"),
                   "--game", slug, "--", "--mods-dir", str(fixture)]
        else:
            cmd = [bin_path, "--headless", "--disc", disc, *run_args,
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
            emit(f"[modtest] {mid}: SKIP - {reason}")
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
                emit(f"[modtest] {mid}: FAIL - run exited {r.returncode}")
                ok = False
            elif "openpete-exit: reason=" not in out:
                emit(f"[modtest] {mid}: FAIL - "
                     "run printed no openpete-exit line")
                ok = False
        if mode == "pass" and "verdict: PASS" not in out:
            emit(f"[modtest] {mid}: FAIL - oracle did not PASS")
            ok = False
        if mode == "fail" and "verdict: FAIL" not in out:
            emit(f"[modtest] {mid}: FAIL - oracle did not FAIL "
                 "(expected divergence missing)")
            ok = False
        missing = [n for n in expect_log if n not in out]
        for needle in missing:
            emit(f"[modtest] {mid}: FAIL - log missing: {needle!r}")
            ok = False
        if missing:
            print_audio_device_lines(mid, out, emit)
        if ok:
            emit(f"[modtest] {mid}: PASS ({mode}, {frames} frames, "
                 f"{len(expect_log)} log asserts)")
        elif verbose:
            emit(out[-3000:])
        return ("pass" if ok else "fail"), ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mods", nargs="+", type=Path)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="mods run at once (default 1)")
    ap.add_argument("--game", default="spyro-1",
                    help="game slug (scripts/lib/game.sh); default spyro-1")
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")

    game = (args.game, *resolve_game(args.game))

    def run_buffered(mod_dir: Path):
        """Run one mod, returning its result, its buffered report lines and
        the exception that ended it (None when it returned)."""
        lines = []
        try:
            status, reason = run_one(mod_dir, game, args.verbose,
                                     lines.append, args.jobs > 1)
        except Exception as exc:  # re-raised in order, after the lines
            return None, None, lines, exc
        return status, reason, lines, None

    passed, failed, skipped = [], [], []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        # Results are consumed in submission order: a mod's lines print once
        # it and every mod before it have finished.
        futures = [pool.submit(run_buffered, d) for d in args.mods]
        results = []
        for fut in futures:
            status, reason, lines, exc = fut.result()
            for line in lines:
                print(line)
            sys.stdout.flush()
            if exc is not None:
                for other in futures:
                    other.cancel()
                raise exc
            results.append((status, reason))
    for mod_dir, (status, reason) in zip(args.mods, results):
        if status == "pass":
            passed.append(mod_dir.name)
        elif status == "skip":
            skipped.append((mod_dir.name, reason))
        else:
            failed.append(mod_dir.name)

    n = len(args.mods)
    print(f"[modtest] summary: {n} mod(s) - {len(passed)} passed, "
          f"{len(skipped)} skipped, {len(failed)} failed")
    for mid, reason in skipped:
        print(f"[modtest]   skipped: {mid} - {reason}")
    for mid in failed:
        print(f"[modtest]   failed: {mid}")

    # A run that executed nothing proves nothing: report it as a failure
    # rather than as a green gate.
    if not passed and not failed:
        print("[modtest] no [test] block executed - VACUOUS")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
