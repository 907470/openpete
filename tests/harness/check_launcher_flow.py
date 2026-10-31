#!/usr/bin/env python3
"""check_launcher_flow.py - the launcher's flow checks, one case per ctest test of the label
`launcher`, beside check_launcher.py (whose launch helper, virtual display and scratch data root
every case here uses).

Cases:
  startup   Window-open to first present (the launcher's stamps window_shown and first_present)
            with a library of three games (one entry per game, each disc.bin a link to the
            repository's image) and a warm pipeline cache: one launch warms the cache, then
            STARTUP_RUNS launches each read both stamps. On a build with the first-boot pipeline
            each entry holds its built module, so the launch-time jobs compute every game's stamp
            and build nothing. PASS when the median delta is at most
            STARTUP_BOUND_MS, the bound set from the figure measured on the build before the
            launch-time stamps moved off the launcher thread. --measure prints the figures and
            sets no verdict.
  resolution       The resolved options after a scripted launcher pick (OPENPETE_LAUNCHER_SELECT_GAME) of
            a library entry, per game, against a direct --disc launch of the same entry's disc.cue:
            each run writes its resolution (OPENPETE_CONFIG_REPORT_OUT, the --config-report text and
            the disc path) and any differing option fails: the scope, the directories, each knob's
            value and the disc path (provenance lines are printed, not compared). Windowed, --frame-limit 5, no window
            change, so no window state is carried.
  multi-install       The installed globals after the final install (OPENPETE_INSTALL_DUMP: the build record,
            the eight host tables, each orchestrator slot and the interp registration, the
            registered runtime and its scalars, the glyph set's flags) of a process that installed
            every other linked game first (OPENPETE_INSTALL_OTHERS_FIRST) against a fresh process's
            single install, for each game as target, both under setarch -R so addresses match
            between processes; any differing line or a non-zero install refusal count fails. The
            single-install dumps of two games must differ (the instrument sees the install).
  job-*     The launcher's job runner on the module rebuild, on a build with the first-boot
            pipeline (recomp/ staged beside the binary; SKIP otherwise). A launch-time job of game
            A is made by moving A's module off its stamped path. Every windowed case runs with
            OPENPETE_PRECOMPILE_AHEAD=1 and asserts a non-zero queued count.
            job-ingest-stop: A's rebuild is stopped by a drop of game B's disc at B's copy end, B's module is
            built, the selector returns, A's rebuild completes after B's build, and the pick of B
            (OPENPETE_LAUNCHER_SELECT_IDLE) starts B; no install is refused.
            job-pick-same: a pick of A during A's rebuild waits for it and starts A.
            job-pick-other: a pick of B during A's rebuild stops A, whose stamped module path then holds no
            file or a whole module; B starts.
            lock: a second process started during a headless --ingest of the first logs the lock
            message, queues no ahead compile, writes no pipeline cache and exits 3, and the first
            process's ingest completes with the whole disc copied and its module built.
            failed-marker: a build forced to fail (OPENPETE_RECOMP_BUILD_FAIL) writes the entry's
            failed-build marker and exits 3; the next launch starts no rebuild and exits 3; a launch
            of a copy of the executable (another build id) rebuilds, removes the marker and plays.
            headless-no-disc: headless, a launch naming no disc boots the most recently played entry after the
            inline launch-time check; an empty library, and --game naming a game with no entry,
            exit 3 with the missing-disc diagnostic.
  ingest-held  An image of a game the library already holds is refused before the copy, with a
            message naming the library's entry, and the library is left as it was: by a headless
            --ingest (non-zero exit), and by a drop on the selector (the selector returns and a
            pick of the held entry plays).
  bind-repair   The pick path's bind-failure rebuild: garbage at A's module path is rebuilt at the bind
            ("rebuilding it from your disc") and the game plays; the same rebuild forced to fail
            writes the failed-build marker, and the next launch exits 3 without a rebuild.

Usage: tests/harness/check_launcher_flow.py CASE [--build-dir DIR] [--out DIR] [--measure]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run or a disc image missing).
"""
import argparse
import fcntl
import glob
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402

ROOT = cl.ROOT
DISCS = {
    "spyro-1": ("SCUS-94228", "Spyro the Dragon"),
    "spyro-2": ("SCUS-94425", "Spyro 2: Ripto's Rage!"),
    "spyro-3": ("SCUS-94467", "Spyro: Year of the Dragon"),
}
STARTUP_RUNS = 7
# Set from the --measure figure on the build before the launch-time stamps moved off the launcher
# thread (7 runs on the dev box under Xvfb and lavapipe: median 45.7 ms, min 45.1, max 46.6): the
# baseline's median plus 10%.
STARTUP_BOUND_MS = 50.0


def disc_bin(game):
    """The repository's disc image of a game."""
    serial = DISCS[game][0]
    return os.path.join(ROOT, "games", game, game, "build", serial + ".bin")


def disc_cue(game):
    """The repository's cue of a game."""
    serial = DISCS[game][0]
    return os.path.join(ROOT, "games", game, game, "build", serial + ".cue")


def seed_entry(d, game, last_played, tag=None):
    """Add a library entry of a game whose disc.bin links the repository's image; return its directory."""
    b = disc_bin(game)
    if not os.path.isfile(b):
        raise cl.Skip(f"{b} missing")
    e = os.path.join(d, "data", "openpete", "library", tag or ("0000000000" + game))
    os.makedirs(e)
    os.symlink(b, os.path.join(e, "disc.bin"))
    with open(os.path.join(e, "disc.cue"), "w") as f:
        f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    with open(os.path.join(e, "meta.toml"), "w") as f:
        f.write('label = "%s (launcher test)"\nsource = "%s.bin"\nsha256 = "%s"\nlast_played = %d\n'
                'game = "%s"\n' % (DISCS[game][1], DISCS[game][0], "0" * 64, last_played, game))
    return e


def stamps(log):
    """The launcher's start-up stamps in ms after process start, by name."""
    return {m.group(1): float(m.group(2))
            for m in re.finditer(r"stamp (\w+) ([\d.]+) ms after process start", log)}


def case_startup(a):
    """Window-open to first present over a three-game library with a warm pipeline cache."""
    d = cl.case_dir(a.out, "startup")
    for i, g in enumerate(DISCS):
        # With the first-boot pipeline each entry holds its built module, so the launch-time jobs
        # compute the stamps and build nothing.
        if has_first_boot(a):
            seed_built(a, d, g, i + 1)
        else:
            seed_entry(d, g, i + 1)
    env = {"OPENPETE_LAUNCHER_INPUT": "2500:quit"}
    deltas = []
    for i in range(STARTUP_RUNS + 1):
        rc, log = cl.launch(a.binary, d, f"run{i}", ["--launcher-window", "1280x800"], env)
        s = stamps(log)
        if rc != 0 or "window_shown" not in s or "first_present" not in s:
            print(f"[launcher-flow] startup run {i}: exit {rc}, stamps {s}: FAIL")
            return 1
        if i == 0:
            continue   # warms the pipeline cache
        deltas.append(s["first_present"] - s["window_shown"])
        print(f"[launcher-flow] startup run {i}: window_shown {s['window_shown']:.1f} ms, "
              f"first_present {s['first_present']:.1f} ms, delta {deltas[-1]:.1f} ms")
    med = statistics.median(deltas)
    print(f"[launcher-flow] startup: {len(deltas)} runs, delta median {med:.1f} ms, "
          f"min {min(deltas):.1f}, max {max(deltas):.1f}")
    if a.measure or STARTUP_BOUND_MS is None:
        print("[launcher-flow] startup: measured (no bound set)")
        return 0 if a.measure else 3
    ok = med <= STARTUP_BOUND_MS
    print(f"[launcher-flow] startup: median {med:.1f} ms against bound {STARTUP_BOUND_MS} ms: "
          f"{'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def module_name(game):
    """The game's module file name: spyro-1 -> spyro1-recomp.so."""
    return game.replace("-", "") + "-recomp.so"


def has_first_boot(a):
    """Whether the build stages the first-boot pipeline's inputs beside the binary."""
    base = os.path.join(a.build_dir, "recomp")
    return os.path.isfile(os.path.join(base, "tools", "PSXRecomp")) and \
        all(os.path.isfile(os.path.join(base, "manifest", g, "manifest.toml")) for g in DISCS)


def need_first_boot(a):
    """Skip a case that needs the first-boot pipeline on a build without it."""
    if not has_first_boot(a):
        raise cl.Skip(f"{a.build_dir} stages no first-boot pipeline (a *-launcher preset)")


def stamped(d, game):
    """The stamped module paths under a game's seeded entry."""
    e = os.path.join(d, "data", "openpete", "library", "0000000000" + game)
    return glob.glob(os.path.join(e, "modules", "*", module_name(game)))


def headless(a, d, tag, args, env=None, timeout=900, binary=None):
    """One headless run under a virtual display (PsyCross needs one) with the case's data root."""
    return cl.launch(binary or a.binary, d, tag, ["--headless", "--mute", "1"] + args, env, timeout=timeout)


def prime(a, game):
    """Build a game's module once per out directory and return (stamp directory name, module path).

    A lock file serialises the cases that prime the same game in parallel (ctest -j)."""
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "prime-" + game + ".lock"), "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        return prime_locked(a, game)


def prime_locked(a, game):
    """prime's body, run under its lock."""
    d = os.path.join(a.out, "prime-" + game)
    # The cached module serves only the binary that built it: its stamp names that binary's inputs.
    key = os.path.join(d, "binary.mtime")
    mtime = str(os.stat(a.binary).st_mtime_ns)
    got = glob.glob(os.path.join(d, "data", "openpete", "library", "*", "modules", "*", module_name(game)))
    if got and os.path.isfile(key) and open(key).read() == mtime:
        return os.path.basename(os.path.dirname(got[0])), got[0]
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    seed_entry(d, game, 1)
    rc, log = headless(a, d, "prime", ["--game", game, "--frame-limit", "1"])
    got = stamped(d, game)
    if not got:
        raise RuntimeError(f"priming {game} built no module (exit {rc}); see {d}/prime.log")
    with open(key, "w") as f:
        f.write(mtime)
    return os.path.basename(os.path.dirname(got[0])), got[0]


def seed_built(a, d, game, last_played):
    """Seed an entry of a game holding its built module at the stamped path; return the module path."""
    stamp, src = prime(a, game)
    e = seed_entry(d, game, last_played)
    dst = os.path.join(e, "modules", stamp, module_name(game))
    os.makedirs(os.path.dirname(dst))
    shutil.copy2(src, dst)
    return dst


def ordered(log, *pats):
    """Problems with the order of first matches of patterns in a log; each must occur after the one before."""
    bad, at = [], -1
    for p in pats:
        m = re.search(p, log[at + 1:])
        if not m:
            bad.append(f"no {p!r} after offset {at + 1}")
            return bad
        at = at + 1 + m.start()
    return bad


def whole_module(path):
    """Whether a file is a module the loader can bind: an ELF exporting openpete_game_recomp_module."""
    if not os.path.isfile(path):
        return False
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return "openpete_game_recomp_module" in out


def common_flow(log):
    """Problems every windowed flow run shares: the ahead compile queued, no install refused."""
    bad = []
    m = re.search(r"launcher precompile: \d+ pipeline descriptions, (\d+) create infos queued", log)
    if not m or int(m.group(1)) == 0:
        bad.append("VACUOUS: the ahead compile queued nothing (OPENPETE_PRECOMPILE_AHEAD)")
    if "install of" in log and "refused" in log:
        bad.append("an install was refused while a job was in flight")
    return bad


def verdict(name, bad, detail=""):
    """Print and return a case's verdict."""
    print(f"[launcher-flow] {name}: {'FAIL ' + '; '.join(bad) if bad else 'PASS'}{(' (' + detail + ')') if detail else ''}")
    return 1 if bad else 0


FLOW_ENV = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_PICK_DELAY_MS": "1500"}


def case_job_ingest_stop(a):
    """A's rebuild stopped by B's ingest at B's copy end, completed after B's build."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-ingest-stop")
    mod = seed_built(a, d, "spyro-1", 1)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_DROP=disc_cue("spyro-2"),
               OPENPETE_LAUNCHER_SELECT_GAME="spyro-2", OPENPETE_LAUNCHER_SELECT_IDLE="1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted drop", r"\[library\] added",
        r"stopping the module rebuild of spyro-1 \(requeued\)", r"module build stopped between commands",
        r"launcher-side module build of spyro-2", r"first-boot module build of spyro-2",
        r"launch-time job started: spyro-1", r"module rebuild of spyro-1 done",
        r"scripted pick: entry \d+ \(spyro-2\)", r"game picked: spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    if not whole_module(mod):
        bad.append(f"A's module {mod} is not whole after its rebuild")
    return verdict("job-ingest-stop", bad)


def case_job_pick_same(a):
    """A pick of A during A's rebuild shows its progress and continues to the game."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-pick-same")
    mod = seed_built(a, d, "spyro-1", 1)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_SELECT_GAME="spyro-1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-1\)",
        r"the pick of spyro-1 waits for its running module rebuild", r"module rebuild of spyro-1 done",
        r"game picked: spyro-1", r"shared recomp module attached")
    if "first-boot module build of spyro-1" in log.split("module rebuild of spyro-1 done", 1)[-1]:
        bad.append("the module was built again after the joined rebuild")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-same", bad)


def case_job_pick_other(a):
    """A pick of B during A's rebuild stops A, which leaves no partial module."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-pick-other")
    mod = seed_built(a, d, "spyro-1", 1)
    seed_built(a, d, "spyro-2", 2)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_SELECT_GAME="spyro-2")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-2\)",
        r"stopping the module rebuild of spyro-1", r"game picked: spyro-2")
    stopped = "module build stopped between commands" in log
    partial = os.path.exists(mod) and not whole_module(mod)
    if partial:
        bad.append(f"A's module path holds a partial file {mod}")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-other", bad, f"A {'stopped between commands' if stopped else 'had finished'}; module "
                               f"{'absent' if not os.path.exists(mod) else 'whole'}")


def case_lock(a):
    """A second process during an ingest is refused by the library lock."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "lock")
    res = {}

    def first():
        res["first"] = headless(a, d, "first", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"])

    t = threading.Thread(target=first)
    t.start()
    tmp = os.path.join(d, "data", "openpete", "library")
    deadline = time.time() + 60
    while time.time() < deadline and not glob.glob(os.path.join(tmp, "ingest-tmp-*", "disc.bin")):
        time.sleep(0.05)
    copying = bool(glob.glob(os.path.join(tmp, "ingest-tmp-*", "disc.bin")))
    rc2, log2 = cl.launch(a.binary, d, "second", ["--launcher-window", "1280x800"],
                          {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_INPUT": "3000:quit"})
    cache = os.path.join(d, "state", "openpete", "cache")
    cache_files = [f for f in glob.glob(os.path.join(cache, "**"), recursive=True) if os.path.isfile(f)]
    t.join()
    rc1, log1 = res["first"]
    bad = []
    if not copying:
        bad.append("VACUOUS: the first process's copy was not seen in progress")
    if rc2 != 3:
        bad.append(f"second process exit {rc2}, not 3")
    if "another OpenPete is preparing a game" not in log2:
        bad.append("second process did not log the lock message")
    if "launcher precompile:" in log2:
        bad.append("second process started the ahead compile")
    if "launch-time job" in log2 or "first-boot module build" in log2:
        bad.append("second process ran a job")
    if cache_files:
        bad.append(f"pipeline cache written: {cache_files}")
    if rc1 != 0:
        bad.append(f"first process exit {rc1}")
    entries = [e for e in glob.glob(os.path.join(tmp, "*", "disc.bin"))]
    if len(entries) != 1 or os.path.getsize(entries[0]) != os.path.getsize(disc_bin("spyro-1")):
        bad.append(f"the first process's copy is not whole: {entries}")
    if not stamped_any(tmp, "spyro-1"):
        bad.append("the first process built no module")
    return verdict("lock", bad)


def stamped_any(lib, game):
    """The stamped module paths of a game under any entry of a library."""
    return glob.glob(os.path.join(lib, "*", "modules", "*", module_name(game)))


def exe_copy(a, d):
    """A copy of the binary (another build id: its own modification time) with the build's recomp/ inputs linked beside it."""
    x = os.path.join(d, "exe-copy")
    os.makedirs(x)
    shutil.copy(a.binary, os.path.join(x, "openpete"))
    src = os.path.abspath(a.build_dir)
    for name in os.listdir(src):
        if name in ("recomp", "sdk", "assets", "openpete_xvfb_wm") or name.endswith(".so"):
            os.symlink(os.path.join(src, name), os.path.join(x, name))
    return os.path.join(x, "openpete")


def case_failed_marker(a):
    """A forced build failure writes the marker; the same build skips it; another build retries."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "failed-marker")
    e = seed_entry(d, "spyro-1", 1)
    marker = os.path.join(e, "build-failed.toml")
    args = ["--game", "spyro-1", "--frame-limit", "1"]
    rc1, log1 = headless(a, d, "fail", args, {"OPENPETE_RECOMP_BUILD_FAIL": "1"})
    bad = []
    if rc1 != 3 or not os.path.isfile(marker):
        bad.append(f"forced failure: exit {rc1}, marker {'present' if os.path.isfile(marker) else 'absent'}")
    rc2, log2 = headless(a, d, "same-build", args)
    if rc2 != 3 or "first-boot module build" in log2 or "launch-time job skipped" not in log2:
        bad.append(f"same build: exit {rc2}, rebuild {'ran' if 'first-boot module build' in log2 else 'not run'}")
    other = exe_copy(a, d)
    rc3, log3 = headless(a, d, "other-build", args, binary=other)
    if rc3 != 0 or "launch-time module rebuild of spyro-1" not in log3 or os.path.isfile(marker):
        bad.append(f"another build: exit {rc3}, marker {'present' if os.path.isfile(marker) else 'removed'}")
    return verdict("failed-marker", bad)


def case_headless_no_disc(a):
    """Headless launches naming no disc."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "headless-no-disc")
    seed_built(a, d, "spyro-1", 5)
    seed_built(a, d, "spyro-2", 1)
    rc, log = headless(a, d, "no-disc", ["--frame-limit", "1"])
    bad = ordered(log, r"launch-time check: the module of spyro-1 is current",
                  r"booting library entry 0000000000spyro-1", r"game picked: spyro-1")
    if rc != 0:
        bad.append(f"no-disc exit {rc}")
    diag = "no game found - pass --disc PATH"
    e = os.path.join(d, "empty")
    os.makedirs(e)
    rc, log = headless(a, e, "empty", ["--frame-limit", "1"])
    if rc != 3 or diag not in log:
        bad.append(f"empty library: exit {rc}, diagnostic {'present' if diag in log else 'absent'}")
    g = os.path.join(d, "game-no-entry")
    os.makedirs(g)
    seed_built(a, g, "spyro-1", 1)
    rc, log = headless(a, g, "game", ["--game", "spyro-3", "--frame-limit", "1"])
    if rc != 3 or diag not in log:
        bad.append(f"--game with no entry: exit {rc}, diagnostic {'present' if diag in log else 'absent'}")
    return verdict("headless-no-disc", bad)


def case_bind_repair(a):
    """A module that fails to bind is rebuilt behind the pick; a failed rebuild leaves the marker."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "bind-repair")
    mod = seed_built(a, d, "spyro-1", 1)
    marker = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(mod))), "build-failed.toml")
    with open(mod, "wb") as f:
        f.write(b"\x7fELF not a recomp module")
    rc, log = headless(a, d, "repair", ["--frame-limit", "1"])
    bad = []
    if rc != 0 or "rebuilding it from your disc" not in log or not whole_module(mod):
        bad.append(f"repair: exit {rc}, module {'whole' if whole_module(mod) else 'not whole'}")
    with open(mod, "wb") as f:
        f.write(b"\x7fELF not a recomp module")
    rc, log = headless(a, d, "repair-fail", ["--frame-limit", "1"], {"OPENPETE_RECOMP_BUILD_FAIL": "1"})
    if rc == 0 or "rebuilding it from your disc" not in log or not os.path.isfile(marker):
        bad.append(f"forced failure: exit {rc}, marker {'present' if os.path.isfile(marker) else 'absent'}")
    rc, log = headless(a, d, "after", ["--frame-limit", "1"])
    if rc != 3 or "rebuilding it from your disc" in log or "first-boot module build" in log:
        bad.append(f"next launch: exit {rc}, rebuild {'ran' if 'first-boot module build' in log else 'not run'}")
    return verdict("bind-repair", bad)


def report_lines(path):
    """A resolution report's lines, or None when it was not written."""
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return f.read().splitlines()


def resolved_options(lines):
    """The options a resolution report states: the scope, the directories, each knob's value (by knob)
    and the disc the run starts with. A knob's provenance ("won at", "shadowed") and the
    identification source are not options: the --disc launch names its disc on the command line
    (a `disc` knob row from cli) and identifies by disc, the pick identifies by its library entry;
    the disc row's value is compared through the disc path."""
    out, in_table = {}, False
    for l in lines:
        t = l.split()
        if not t:
            in_table = False
            continue
        if t[0] == "knob" and "won" in t:
            in_table = True
            continue
        if in_table:
            if t[0] != "disc":
                out["knob " + t[0]] = t[1] if len(t) > 1 else ""
        elif l.startswith("disc_path ") or l.strip().startswith(("scope:", "files:", "save target:", "layout:")):
            out[l.split(":")[0].strip() if ":" in l.split()[0] else t[0]] = l.strip()
        elif l.startswith("  ") and len(t) == 2:
            out["dir " + t[0]] = t[1]
    return out


def case_resolution(a):
    """The launcher pick's resolved options against a direct --disc launch of the same entry."""
    d = cl.case_dir(a.out, "resolution")
    bad, compared = [], 0
    for i, g in enumerate(DISCS):
        e = seed_entry(d, g, i + 1)
        pick = os.path.join(d, g + ".pick.txt")
        direct = os.path.join(d, g + ".disc.txt")
        rc1, _ = cl.launch(a.binary, d, g + "-pick", ["--frame-limit", "5"],
                           {"OPENPETE_LAUNCHER_SELECT_GAME": g, "OPENPETE_CONFIG_REPORT_OUT": pick}, timeout=300)
        rc2, _ = cl.launch(a.binary, d, g + "-disc", ["--disc", os.path.join(e, "disc.cue"), "--frame-limit", "5"],
                           {"OPENPETE_CONFIG_REPORT_OUT": direct}, timeout=300)
        p, q = report_lines(pick), report_lines(direct)
        if rc1 != 0 or rc2 != 0 or p is None or q is None:
            bad.append(f"{g}: exits {rc1}/{rc2}, reports {p is not None}/{q is not None}")
            continue
        po, qo = resolved_options(p), resolved_options(q)
        keys = sorted(set(po) | set(qo))
        diff = [k for k in keys if po.get(k) != qo.get(k)]
        compared += len(keys)
        for k in diff:
            bad.append(f"{g}: {k}: pick {po.get(k)!r} vs disc {qo.get(k)!r}")
        prov = [x for x in set(p) ^ set(q) if "identified by" in x or x.split()[:1] == ["disc"]]
        print(f"[launcher-flow] resolution {g}: {len(keys)} options, {len(diff)} differ; provenance lines "
              f"differing (not options): {sorted(x.strip()[:60] for x in prov)}")
    if compared == 0:
        bad.append("VACUOUS: no line compared")
    return verdict("resolution", bad, f"{compared} lines compared")


def install_dump(a, d, tag, game, others):
    """One process's install dump for a target game."""
    out = os.path.join(d, tag + ".dump")
    env = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
    env.update({"OPENPETE_INSTALL_DUMP": out, "XDG_DATA_HOME": os.path.join(d, "data"),
                "XDG_STATE_HOME": os.path.join(d, "state"), "XDG_CONFIG_HOME": os.path.join(d, "config")})
    if others:
        env["OPENPETE_INSTALL_OTHERS_FIRST"] = "1"
    subprocess.run(["setarch", os.uname().machine, "-R", a.binary, "--no-config", "--config-report",
                    "--game", game], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    return report_lines(out)


def case_multi_install(a):
    """Multi-install against single install, for each game as target."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    d = cl.case_dir(a.out, "multi-install")
    bad, compared, singles = [], 0, {}
    for g in DISCS:
        one = install_dump(a, d, g + ".single", g, False)
        many = install_dump(a, d, g + ".multi", g, True)
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        singles[g] = one
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs multi {str(y)[:90]!r}" for x, y in diff]
        refused = [l for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        bad += [f"{g}: {l}" for l in refused]
        print(f"[launcher-flow] multi-install {g}: {len(one)} lines, {len(diff)} differ")
    if len(singles) >= 2:
        a1, a2 = list(singles.values())[:2]
        control = sum(1 for x, y in zip(a1, a2) if x != y)
        print(f"[launcher-flow] multi-install control: two games' single installs differ on {control} lines")
        if control == 0:
            bad.append("VACUOUS: two games' single-install dumps are identical")
    return verdict("multi-install", bad, f"{compared} lines compared")


def library_state(d):
    """The library's entries (any ingest-tmp directory included) and each entry's meta.toml lines but last_played."""
    lib = os.path.join(d, "data", "openpete", "library")
    out = {}
    for e in sorted(os.listdir(lib)):
        if e.startswith("."):
            continue   # the lock file the run takes
        m = os.path.join(lib, e, "meta.toml")
        # last_played is the pick's stamp of a play, not a change the ingest made.
        out[e] = [l for l in open(m).read().splitlines() if not l.startswith("last_played")] \
            if os.path.isfile(m) else None
    return out


def case_ingest_held(a):
    """An ingest of a game the library already holds is refused, by --ingest and by a drop."""
    d = cl.case_dir(a.out, "ingest-held")
    seed_entry(d, "spyro-1", 1)
    before = library_state(d)
    bad = []
    rc, log = headless(a, d, "ingest", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"])
    if rc == 0 or "the library already holds a disc of spyro-1" not in log:
        bad.append(f"--ingest: exit {rc}, refusal {'logged' if 'already holds' in log else 'absent'}")
    if library_state(d) != before:
        bad.append(f"--ingest changed the library: {sorted(library_state(d))}")
    env = {"OPENPETE_LAUNCHER_DROP": disc_cue("spyro-1"), "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1"}
    rc, log = cl.launch(a.binary, d, "drop", ["--launcher-window", "1280x800", "--frame-limit", "5"], env,
                        timeout=600)
    bad += ordered(log, r"scripted drop", r"ingest of .* refused", r"scripted pick: entry \d+ \(spyro-1\)",
                   r"game picked: spyro-1")
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    if library_state(d) != before:
        bad.append(f"the drop changed the library: {sorted(library_state(d))}")
    return verdict("ingest-held", bad)


CASES = {"startup": case_startup, "resolution": case_resolution, "multi-install": case_multi_install,
         "job-ingest-stop": case_job_ingest_stop, "job-pick-same": case_job_pick_same,
         "job-pick-other": case_job_pick_other, "lock": case_lock, "failed-marker": case_failed_marker,
         "headless-no-disc": case_headless_no_disc, "bind-repair": case_bind_repair,
         "ingest-held": case_ingest_held}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=sorted(CASES))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--measure", action="store_true")
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher-flow] SKIP: {a.binary} missing")
        return 64
    try:
        return CASES[a.case](a)
    except cl.Skip as e:
        print(f"[launcher-flow] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
