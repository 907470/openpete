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
  multi-install-extract  The multi-install check with each other game's asset extraction job (domes, glyph
            models, logo) run inline in the `ingest` role after its install, on the repository's
            image of that game (OPENPETE_INSTALL_OTHERS_FIRST=extract, OPENPETE_ASSET_DISCS): every
            job completes with domes, a ready glyph set and a logo, and the dump equals the single
            install's; Spyro 3's image given as Spyro 2's disc fails Spyro 2's job.
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
  input     T11, the game page's commands (windowed, three-game library): arrow keys, the d-pad and
            the left stick's X move the focus one box without wrap (the stick re-centres between
            moves); up and down do nothing; Escape quits at once; East asks once ("quit ask chip
            shown"), the ask lapses after 2 s, and a second East inside 2 s quits; the pointer takes
            the focus only after it moves, so a key move holds against a still pointer; Space and
            West open the file picker for the focused box's game; Enter and South start the focused
            box's game; a disc of another game dropped on the page focuses that game's box when the
            page returns.
  ingest-held  An image of a game the library already holds is refused before the copy, with a
            message naming the library's entry, and the library is left as it was: by a headless
            --ingest (non-zero exit), and by a drop on the selector (the selector returns and a
            pick of the held entry plays).
  bind-repair   The pick path's bind-failure rebuild: garbage at A's module path is rebuilt at the bind
            ("rebuilding it from your disc") and the game plays; the same rebuild forced to fail
            writes the failed-build marker, and the next launch exits 3 without a rebuild.
  cache     The launcher asset cache (state/openpete/cache/<game>/: domes.bin, glyphs.txt, logo.rgba,
            manifest.toml) over a three-game library, headless with no display: the cold launch
            writes every game's cache through the launch-time rerun (each output decoded back:
            domes, glyphs, logo); a warm launch writes no file and logs no rerun; a deleted output,
            an older and a newer manifest format and a changed disc hash in meta.toml each rerun
            that game alone; an extraction forced to fail (OPENPETE_ASSET_FAIL) logs the failure
            and writes the failed marker, keeping the previous outputs, and the same build skips
            it; a copy of the executable (another build id) reruns every game once, clearing the
            marker, and its second launch reruns none. After every step each manifest lists
            outputs of its sizes and no temporary is left.
  cache-ingest  A headless --ingest writes the game's cache before the entry boots (the
            assets-ready line, before the module build's end on a build with the first-boot
            pipeline) and logs no launch-time rerun.
  multi-install-launch  The multi-install check through the launcher's launch-time path: a
            headless --game pick over a library of the three games with no cache, whose
            launch-time jobs install and extract every game, dumps the installed globals equal to
            a fresh single install's, for each game as target.
  job-ingest-stop-assets  T8 (a) on the asset extraction (windowed): A's launch-time asset job
            (held by OPENPETE_ASSET_JOB_DELAY_MS) is stopped by a drop of B's disc at B's copy
            end, B's asset job runs at the ingest, A's job is requeued and completes after it.
  job-pick-other-assets  T8 (c) on the asset extraction (windowed): a pick of B during A's held
            launch-time asset job stops A, which leaves no cache file, and B's cache is written
            at the pick.
  shader-step-*  T8's shader step on the launcher's ahead compile (windowed, a cold pipeline
            cache, OPENPETE_PRECOMPILE_FORCE=1 so bring-up's compile stage runs on lavapipe):
            shader-step-during: a pick at the selector's first frame stops the ahead walk with
            create infos not returned, and the compile stage compiles pipelines and draws its
            progress screen (OPENPETE_PRECOMPILE_PANEL_MS=0).
            shader-step-after: a pick held until the ahead walk has returned every create info
            (OPENPETE_LAUNCHER_PICK_DELAY_MS) starts the game with every bring-up create held by
            the cache (bring-up compiles no pipeline); the progress screens drawn are reported.
            shader-step-order: with the terrain cache described (OPENPETE_TERRAIN_CACHE_FORCE=1),
            the walk's terrain group at the first texture mapping returns before the group of
            the other built-in pipelines.

Usage: tests/harness/check_launcher_flow.py CASE [--build-dir DIR] [--out DIR] [--measure]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run or a disc image missing).
"""
import argparse
import fcntl
import glob
import hashlib
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
        r"stopping the launch-time job of spyro-1 \(requeued\)", r"module build stopped between commands",
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
        r"the pick of spyro-1 waits for its running launch-time job", r"module rebuild of spyro-1 done",
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
        r"stopping the launch-time job of spyro-1", r"game picked: spyro-2")
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
    # The pipeline cache lies in the leaf itself; the per-game directories hold the first process's
    # launcher asset cache, which its ingest writes.
    cache_files = [f for f in glob.glob(os.path.join(cache, "*")) if os.path.isfile(f)]
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


def install_dump(a, d, tag, game, others, extra=None):
    """One process's install dump for a target game; with `extra`, also its log text."""
    out = os.path.join(d, tag + ".dump")
    log = os.path.join(d, tag + ".log")
    for f in (out, log):
        if os.path.exists(f):
            os.remove(f)
    env = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
    env.update({"OPENPETE_INSTALL_DUMP": out, "XDG_DATA_HOME": os.path.join(d, "data"),
                "XDG_STATE_HOME": os.path.join(d, "state"), "XDG_CONFIG_HOME": os.path.join(d, "config")})
    if others:
        env["OPENPETE_INSTALL_OTHERS_FIRST"] = "1"
    env.update(extra or {})
    with open(log, "w") as f:
        subprocess.run(["setarch", os.uname().machine, "-R", a.binary, "--no-config", "--config-report",
                        "--game", game], env=env, stdout=subprocess.DEVNULL, stderr=f, timeout=300)
    if extra is None:
        return report_lines(out)
    with open(log, errors="replace") as f:
        return report_lines(out), f.read()


def disc_sha256(a, game):
    """The content hash of a game's repository image, cached beside the case outputs by the image's mtime."""
    b = disc_bin(game)
    if not os.path.isfile(b):
        raise cl.Skip(f"{b} missing")
    os.makedirs(a.out, exist_ok=True)
    key = os.path.join(a.out, f"sha256-{game}.txt")
    stamp = str(os.stat(b).st_mtime_ns)
    if os.path.isfile(key):
        got = open(key).read().split()
        if len(got) == 2 and got[0] == stamp:
            return got[1]
    h = hashlib.sha256()
    with open(b, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    with open(key, "w") as f:
        f.write(stamp + " " + h.hexdigest() + "\n")
    return h.hexdigest()


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


def case_multi_install_extract(a):
    """The multi-install check with each other game's asset extraction job run after its install, and an unknown disc refused."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    d = cl.case_dir(a.out, "multi-install-extract")
    shas = {g: disc_sha256(a, g) for g in DISCS}
    discs = ";".join(f"{g}={shas[g]}={disc_cue(g)}" for g in DISCS)
    bad, compared = [], 0
    for g in DISCS:
        one = install_dump(a, d, g + ".single", g, False)
        many, log = install_dump(a, d, g + ".extract", g, True,
                                 {"OPENPETE_INSTALL_OTHERS_FIRST": "extract", "OPENPETE_ASSET_DISCS": discs})
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        for o in DISCS:
            if o == g:
                continue
            m = re.search(r"asset extraction of %s: (\d+) domes \((\d+) vertices, (\d+) triangles\), "
                          r"glyphs (\w+), logo (\d+)x(\d+)" % re.escape(o), log)
            if "asset extraction of %s: ok" % o not in log or not m or int(m.group(1)) == 0 \
                    or m.group(4) != "ready" or int(m.group(5)) == 0:
                bad.append(f"{g}: extraction of {o} did not complete ({m.group(0) if m else 'no result line'})")
            else:
                print(f"[launcher-flow] multi-install-extract {g}: {m.group(0)}")
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs extract {str(y)[:90]!r}" for x, y in diff]
        bad += [f"{g}: {l}" for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        print(f"[launcher-flow] multi-install-extract {g}: {len(one)} lines, {len(diff)} differ")
    # A disc whose asset inputs are not the known disc's fails the job: Spyro 3's image as Spyro 2's.
    wrong = discs.replace(f"spyro-2={shas['spyro-2']}={disc_cue('spyro-2')}",
                          f"spyro-2={shas['spyro-3']}={disc_cue('spyro-3')}")
    _, log = install_dump(a, d, "wrong-disc", "spyro-1", True,
                          {"OPENPETE_INSTALL_OTHERS_FIRST": "extract", "OPENPETE_ASSET_DISCS": wrong})
    m = re.search(r"asset extraction of spyro-2 from .* failed: (.*)", log)
    if "asset extraction of spyro-2: failed" not in log or not m:
        bad.append("Spyro 3's image as Spyro 2's disc did not fail Spyro 2's job")
    else:
        print(f"[launcher-flow] multi-install-extract: Spyro 3's image as Spyro 2's disc fails: {m.group(1)}")
    return verdict("multi-install-extract", bad, f"{compared} lines compared")


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


# ---- the launcher asset cache (cache-*, multi-install-launch, job-*-assets) ----

CACHE_FILES = ("domes.bin", "glyphs.txt", "logo.rgba", "manifest.toml")


def nox(a, d, tag, args, env=None, timeout=600, binary=None):
    """One headless run with no display, the case's data root and no inherited OPENPETE_* knob; returns (rc, log)."""
    e = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
    e.update({"XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
              "XDG_CONFIG_HOME": os.path.join(d, "config"), "OPENPETE_MSGBOX_SKIP": "1"})
    e.update(env or {})
    log = os.path.join(d, tag + ".log")
    if os.path.exists(log):
        os.remove(log)
    cmd = [binary or a.binary, "--no-config", "--no-card", "--vibration", "0", "--mute", "1", "--headless"] + args
    with open(log, "w") as f:
        try:
            rc = subprocess.run(cmd, env=e, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = -9
    with open(log, errors="replace") as f:
        return rc, f.read()


def cache_dir(d, game):
    """A game's launcher asset cache directory under the case's state root."""
    return os.path.join(d, "state", "openpete", "cache", game)


def manifest(d, game):
    """A game's cache manifest as (keys, files), or None when it does not exist."""
    p = os.path.join(cache_dir(d, game), "manifest.toml")
    if not os.path.isfile(p):
        return None
    keys, files, in_files = {}, {}, False
    for l in open(p).read().splitlines():
        if l == "[files]":
            in_files = True
        elif in_files and " = " in l:
            k, v = l.split(" = ", 1)
            files[k.strip('"')] = int(v)
        elif " = " in l:
            k, v = l.split(" = ", 1)
            keys[k] = v.strip('"')
    return keys, files


def cache_snapshot(d):
    """Every file under the cache leaf's game directories: path -> (inode, mtime_ns, size)."""
    out = {}
    for g in DISCS:
        for f in glob.glob(os.path.join(cache_dir(d, g), "*")):
            st = os.stat(f)
            out[f] = (st.st_ino, st.st_mtime_ns, st.st_size)
    return out


def written(before, after):
    """The cache files a run created or replaced."""
    return sorted(f for f, v in after.items() if before.get(f) != v)


def cache_problems(d, game):
    """Problems with a game's cache: a missing output, a size differing from the manifest, a temporary left."""
    bad = []
    m = manifest(d, game)
    if m is None:
        return [f"{game}: no manifest"]
    keys, files = m
    for f in CACHE_FILES[:-1]:
        p = os.path.join(cache_dir(d, game), f)
        if not os.path.isfile(p) or files.get(f) != os.path.getsize(p):
            bad.append(f"{game}: {f} {'missing' if not os.path.isfile(p) else 'size differs from the manifest'}")
    tmp = [f for f in os.listdir(cache_dir(d, game)) if ".tmp" in f]
    if tmp:
        bad.append(f"{game}: temporaries left {tmp}")
    return bad


def domes_count(d, game):
    """The dome count and total vertex count domes.bin holds."""
    b = open(os.path.join(cache_dir(d, game), "domes.bin"), "rb").read()
    if b[:8] != b"OPDOMES1":
        return 0, 0
    n = int.from_bytes(b[8:12], "little")
    at, nv_sum = 12, 0
    for _ in range(n):
        ln = b[at]
        at += 1 + ln
        nv, nt = int.from_bytes(b[at:at + 4], "little"), int.from_bytes(b[at + 4:at + 8], "little")
        at += 12 + 3 + nv * 9 + nt * 6
        nv_sum += nv
    return (n, nv_sum) if at == len(b) else (0, 0)


def logo_size(d, game):
    """The logo's width and height logo.rgba holds, (0, 0) when malformed."""
    b = open(os.path.join(cache_dir(d, game), "logo.rgba"), "rb").read()
    w, h = int.from_bytes(b[8:12], "little"), int.from_bytes(b[12:16], "little")
    return (w, h) if b[:8] == b"OPLOGO01" and len(b) == 16 + w * h * 4 else (0, 0)


def reruns(log):
    """The launch-time rerun lines of a log: (game, trigger)."""
    return re.findall(r"launch-time rerun: asset extraction of (\S+) \((.*)\)", log)


def seed_library(d):
    """A library of one entry per game (the repository's images); return {game: entry dir}."""
    return {g: seed_entry(d, g, i + 1) for i, g in enumerate(DISCS)}


def rewrite(path, fn):
    """Replace a file's text with fn(text)."""
    with open(path) as f:
        text = f.read()
    with open(path, "w") as f:
        f.write(fn(text))


def run_step(a, d, tag, bad, env=None, binary=None, args=None):
    """One headless pick of Spyro 1 over the case's library; returns (log, files written, reruns)."""
    before = cache_snapshot(d)
    rc, log = nox(a, d, tag, args or ["--game", "spyro-1", "--frame-limit", "1"], env, binary=binary)
    w, r = written(before, cache_snapshot(d)), reruns(log)
    if rc != 0:
        bad.append(f"{tag}: exit {rc}")
    for g in DISCS:
        bad += [f"{tag}: {x}" for x in cache_problems(d, g) if manifest(d, g) is not None]
    print(f"[launcher-flow] cache {tag}: exit {rc}, {len(w)} cache files written, {len(r)} reruns {r}")
    return log, w, r


def expect_reruns(bad, tag, r, want):
    """Check a step's reruns against {game: trigger substring}."""
    got = {g: t for g, t in r}
    if len(r) != len(want) or any(g not in got or s not in got[g] for g, s in want.items()):
        bad.append(f"{tag}: reruns {r}, expected {want}")


def cache_cold(a, d, bad):
    """The cold launch: every library game's cache written, each output decoded back."""
    log, w, r = run_step(a, d, "cold", bad)
    expect_reruns(bad, "cold", r, {g: "no cache manifest" for g in DISCS})
    for g in DISCS:
        n, nv = domes_count(d, g)
        lw, lh = logo_size(d, g)
        glyph = os.path.getsize(os.path.join(cache_dir(d, g), "glyphs.txt"))
        print(f"[launcher-flow] cache {g}: {n} domes ({nv} vertices), glyphs.txt {glyph} bytes, logo {lw}x{lh}")
        if n == 0 or nv == 0 or glyph == 0 or lw == 0:
            bad.append(f"cold: {g}: outputs empty or malformed ({n} domes, logo {lw}x{lh})")
        if f"launcher assets of {g} ready (launch-time)" not in log:
            bad.append(f"cold: {g}: no assets-ready line")
    if len(w) != 4 * len(DISCS):
        bad.append(f"cold: {len(w)} cache files written, expected {4 * len(DISCS)}")


def cache_rewrites(a, d, entries, bad):
    """A deleted output, an older and a newer format and a changed disc hash each rerun that game alone."""
    os.remove(os.path.join(cache_dir(d, "spyro-1"), "glyphs.txt"))
    _, w, r = run_step(a, d, "deleted", bad)
    expect_reruns(bad, "deleted", r, {"spyro-1": "glyphs.txt missing"})
    for g, fmt, why in (("spyro-2", "0", "manifest format 0, this build writes"),
                        ("spyro-3", "99", "newer than this build's")):
        p = os.path.join(cache_dir(d, g), "manifest.toml")
        rewrite(p, lambda t: re.sub(r"^format = \d+", "format = " + fmt, t, flags=re.M))
        _, w, r = run_step(a, d, "format-" + fmt, bad)
        expect_reruns(bad, "format-" + fmt, r, {g: why})
        if manifest(d, g)[0].get("format") != "1":
            bad.append(f"format-{fmt}: {g}'s manifest not rewritten in format 1")
    meta = os.path.join(entries["spyro-1"], "meta.toml")
    rewrite(meta, lambda t: t.replace('sha256 = "' + "0" * 64, 'sha256 = "' + "1" * 64))
    _, w, r = run_step(a, d, "hash", bad)
    expect_reruns(bad, "hash", r, {"spyro-1": "hash changed"})
    if manifest(d, "spyro-1")[0].get("disc_sha256") != "1" * 64:
        bad.append("hash: spyro-1's manifest does not name the new hash")


def cache_failed(a, d, bad):
    """A forced extraction failure writes the failed marker; this build skips it; another build reruns."""
    # A rerun is due (outputs of another build) so the forced failure is reached with the outputs recorded.
    p = os.path.join(cache_dir(d, "spyro-2"), "manifest.toml")
    rewrite(p, lambda t: re.sub(r'^build = ".*"$', 'build = "an older build"', t, flags=re.M))
    before = {f: os.path.getsize(os.path.join(cache_dir(d, "spyro-2"), f)) for f in CACHE_FILES[:-1]}
    log, w, r = run_step(a, d, "fail", bad, {"OPENPETE_ASSET_FAIL": "spyro-2"})
    m = manifest(d, "spyro-2")
    fails = re.findall(r"asset extraction of spyro-2 failed: (.*?);", log)
    if not fails or not m or not m[0].get("failed_build"):
        bad.append(f"fail: log lines {len(fails)}, marker {'present' if m and m[0].get('failed_build') else 'absent'}")
    else:
        print(f"[launcher-flow] cache fail: 1 failure line ({fails[0]}), marker build {m[0]['failed_build']}")
    if any(os.path.getsize(os.path.join(cache_dir(d, "spyro-2"), f)) != n for f, n in before.items()):
        bad.append("fail: the failure changed spyro-2's previous outputs")
    log, w, r = run_step(a, d, "fail-same-build", bad)
    if r or w or "asset extraction of spyro-2 skipped" not in log:
        bad.append(f"fail-same-build: reruns {r}, {len(w)} files written, skip "
                   f"{'logged' if 'asset extraction of spyro-2 skipped' in log else 'absent'}")


def case_cache(a):
    """The launcher asset cache over a three-game library, headless: cold, warm, each rewrite trigger, the failed marker, another build id."""
    d = cl.case_dir(a.out, "cache")
    entries = seed_library(d)
    bad = []
    cache_cold(a, d, bad)
    log, w, r = run_step(a, d, "warm", bad)
    current = len(set(re.findall(r"asset cache of (\S+) is current", log)))
    if w or r or current != len(DISCS):
        bad.append(f"warm: {len(w)} files written, {len(r)} reruns, {current} games current")
    cache_rewrites(a, d, entries, bad)
    cache_failed(a, d, bad)
    other = exe_copy(a, d)
    _, w, r = run_step(a, d, "other-build", bad, binary=other)
    expect_reruns(bad, "other-build", r, {"spyro-1": "outputs written by build", "spyro-3": "outputs written by build",
                                          "spyro-2": "a failed marker of build"})
    m = manifest(d, "spyro-2")
    if not m or m[0].get("failed_build"):
        bad.append("other-build: spyro-2's failed marker not cleared")
    _, w, r = run_step(a, d, "other-build-warm", bad, binary=other)
    if w or r:
        bad.append(f"other-build-warm: {len(w)} files written, {len(r)} reruns")
    return verdict("cache", bad)


def case_cache_ingest(a):
    """An ingest writes the game's cache before it boots; the assets-ready line precedes the module build's end."""
    d = cl.case_dir(a.out, "cache-ingest")
    rc, log = nox(a, d, "ingest", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"], timeout=900)
    bad = cache_problems(d, "spyro-1")
    if rc != 0:
        bad.append(f"exit {rc}")
    bad += ordered(log, r"\[library\] added", r"asset extraction of spyro-1 at ingest \(no cache manifest\)",
                   r"asset cache of spyro-1 written", r"launcher assets of spyro-1 ready \(ingest\)",
                   r"booting library entry")
    if reruns(log):
        bad.append(f"an ingest logged launch-time reruns {reruns(log)}")
    detail = "no first-boot pipeline: the build-end order is not exercised"
    if "first-boot module build" in log:
        # A headless ingest holds the launcher thread on the build, so its reap
        # (the ready line) follows the build; the cache write must not.
        bad += ordered(log, r"asset cache of spyro-1 written", r"recomp-build: module built: ")
        detail = "cache written before the module build's end"
    n, _ = domes_count(d, "spyro-1") if not bad else (0, 0)
    return verdict("cache-ingest", bad, f"{n} domes; {detail}")


def case_multi_install_launch(a):
    """T18 through the launch-time path: the dump after a pick whose launch-time jobs installed and extracted every library game, against a fresh single install."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    bad, compared, launches = [], 0, {}
    for g in DISCS:
        d = cl.case_dir(a.out, "multi-install-launch-" + g)
        seed_library(d)
        one = install_dump(a, d, g + ".single", g, False)
        out = os.path.join(d, g + ".launch.dump")
        env = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
        env.update({"OPENPETE_INSTALL_DUMP": out, "XDG_DATA_HOME": os.path.join(d, "data"),
                    "XDG_STATE_HOME": os.path.join(d, "state"), "XDG_CONFIG_HOME": os.path.join(d, "config")})
        logp = os.path.join(d, g + ".launch.log")
        with open(logp, "w") as f:
            subprocess.run(["setarch", os.uname().machine, "-R", a.binary, "--no-config", "--no-card", "--mute", "1",
                            "--vibration", "0", "--headless", "--game", g, "--frame-limit", "1"],
                           env=env, stdout=subprocess.DEVNULL, stderr=f, timeout=600)
        log = open(logp, errors="replace").read()
        many = report_lines(out)
        r = dict(reruns(log))
        others = [o for o in DISCS if o != g]
        if any(o not in r for o in others):
            bad.append(f"{g}: launch-time reruns {sorted(r)}, expected {others}")
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs launch {str(y)[:90]!r}" for x, y in diff]
        bad += [f"{g}: {l}" for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        launches[g] = many
        print(f"[launcher-flow] multi-install-launch {g}: {len(r)} launch-time reruns, {len(one)} lines, "
              f"{len(diff)} differ")
    if len(launches) >= 2:
        a1, a2 = list(launches.values())[:2]
        control = sum(1 for x, y in zip(a1, a2) if x != y)
        print(f"[launcher-flow] multi-install-launch control: two targets' launch dumps differ on {control} lines")
        if control == 0:
            bad.append("VACUOUS: two targets' launch dumps are identical")
    if compared == 0:
        bad.append("VACUOUS: no line compared")
    return verdict("multi-install-launch", bad, f"{compared} lines compared")


SLOW_ASSETS = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_ASSET_JOB_DELAY_MS": "20000"}


def case_job_ingest_stop_assets(a):
    """T8 (a) on the extraction rerun: A's launch-time asset job stopped at B's copy end, then completed after B's jobs (windowed)."""
    d = cl.case_dir(a.out, "job-ingest-stop-assets")
    seed_entry(d, "spyro-1", 1)
    env = dict(SLOW_ASSETS, OPENPETE_LAUNCHER_PICK_DELAY_MS="1500", OPENPETE_LAUNCHER_DROP=disc_cue("spyro-2"),
               OPENPETE_LAUNCHER_SELECT_GAME="spyro-2", OPENPETE_LAUNCHER_SELECT_IDLE="1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"launch-time rerun: asset extraction of spyro-1",
        r"scripted drop", r"\[library\] added", r"stopping the launch-time job of spyro-1 \(requeued\)",
        r"asset extraction of spyro-1 stopped", r"asset extraction of spyro-2 at ingest",
        r"launcher assets of spyro-2 ready \(ingest\)", r"launch-time job started: spyro-1",
        r"launcher assets of spyro-1 ready \(launch-time\)", r"scripted pick: entry \d+ \(spyro-2\)",
        r"game picked: spyro-2")
    bad += cache_problems(d, "spyro-1") + cache_problems(d, "spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-ingest-stop-assets", bad)


def case_job_pick_other_assets(a):
    """T8 (c) on the extraction rerun: a pick of B during A's launch-time asset job stops A, which leaves no partial cache file (windowed)."""
    d = cl.case_dir(a.out, "job-pick-other-assets")
    seed_entry(d, "spyro-1", 1)
    seed_entry(d, "spyro-2", 2)
    env = dict(SLOW_ASSETS, OPENPETE_LAUNCHER_PICK_DELAY_MS="1500", OPENPETE_LAUNCHER_SELECT_GAME="spyro-2")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-2\)",
        r"stopping the launch-time job of spyro-1", r"asset extraction of spyro-1 stopped; the cache is unchanged",
        r"launcher assets of spyro-2 ready \(pick\)", r"game picked: spyro-2")
    a_dir = cache_dir(d, "spyro-1")
    left = os.listdir(a_dir) if os.path.isdir(a_dir) else []
    if left:
        bad.append(f"A's cache holds files after its stopped job: {left}")
    bad += cache_problems(d, "spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-other-assets", bad)


# ---- the shader step (shader-step-*) ----

SHADER_ENV = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_PRECOMPILE_FORCE": "1", "OPENPETE_LAUNCHER_SELECT": "0"}
STAGE = re.compile(r"compile stage: ([\d.]+) ms from its open, \d+ creates queued, \d+ done; pipelines compiled "
                   r"(\d+) \(bring-up (\d+) of (\d+) creates, precompile (\d+) of (\d+);.*?progress screen (\d+) "
                   r"present\(s\), the first at (\d+) of (\d+)")
WALK_END = re.compile(r"launcher walk: keys (\d+), created (\d+), probe hits (\d+), .*?([\d.]+) ms \((.*)\)")


def shader_run(a, name, env):
    """One scripted pick of a Spyro 1 entry on a cold cache; returns (rc, log, stage match, walk-end match, problems)."""
    d = cl.case_dir(a.out, name)
    seed_entry(d, "spyro-1", 1)
    rc, log = cl.launch(a.binary, d, "run", ["--frame-limit", "20"], dict(SHADER_ENV, **env), timeout=900)
    st, wl = STAGE.search(log), WALK_END.search(log)
    bad = common_flow(log)
    if rc != 0:
        bad.append(f"exit {rc}")
    if not st:
        bad.append("no compile stage line")
    if not wl:
        bad.append("no launcher walk end line")
    return rc, log, st, wl, bad


def case_shader_step_during(a):
    """A pick during the ahead walk: the walk stops short and the compile stage compiles and draws (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-during", {"OPENPETE_PRECOMPILE_PANEL_MS": "0"})
    if wl and "stopped at the pick" not in wl.group(5):
        bad.append("VACUOUS: the ahead walk returned every create info before the pick")
    if st and int(st.group(3)) == 0:
        bad.append("bring-up compiled no pipeline: the pick came after the walk's useful part")
    if st and int(st.group(7)) == 0:
        bad.append("the compile stage drew no progress screen")
    detail = (f"walk created {wl.group(2)} of {wl.group(1)}; bring-up compiled {st.group(3)} of {st.group(4)}, "
              f"stage {st.group(1)} ms, {st.group(7)} screen(s), first at {st.group(8)} of {st.group(9)}") if st and wl else ""
    return verdict("shader-step-during", bad, detail)


def case_shader_step_after(a):
    """A pick after the ahead walk ends: no bring-up compile, no progress screen (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-after", {"OPENPETE_LAUNCHER_PICK_DELAY_MS": "5000"})
    if wl and "stopped at the pick" in wl.group(5):
        bad.append("the ahead walk was still running at the pick (raise the pick delay)")
    if wl and int(wl.group(2)) + int(wl.group(3)) == 0:
        bad.append("VACUOUS: the ahead walk created nothing")
    if st and int(st.group(4)) == 0:
        bad.append("VACUOUS: bring-up made no create")
    if st and int(st.group(3)) != 0:
        bad.append(f"bring-up compiled {st.group(3)} pipelines the walk had compiled")
    # The progress screen is timed (NR_PRECOMPILE_PANEL_NS): on lavapipe a creation the cache holds
    # still compiles, so a loaded host can pass the wait with nothing to compile; the count of
    # pipelines compiled decides, the screens are reported.
    detail = (f"walk created {wl.group(2)} of {wl.group(1)} in {wl.group(4)} ms; bring-up {st.group(4)} creates, "
              f"{st.group(3)} compiled, stage {st.group(1)} ms, {st.group(7)} screen(s)") if st and wl else ""
    return verdict("shader-step-after", bad, detail)


def case_shader_step_order(a):
    """The ahead walk's terrain group returns before the other built-in pipelines at the first texture mapping (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-order", {"OPENPETE_LAUNCHER_PICK_DELAY_MS": "8000",
                                                                "OPENPETE_TERRAIN_CACHE_FORCE": "1"})
    groups = re.search(r"per group at the first mapping (\d+) (\d+) (\d+) (\d+)", log)
    if not groups or int(groups.group(2)) == 0 or int(groups.group(3)) == 0:
        bad.append("VACUOUS: the walk queued no terrain or no other built-in create info")
    bad += ordered(log, r"launcher walk: \w+ group 1 returned", r"launcher walk: \w+ group 2 returned")
    back = {m.group(1): m.group(2) for m in re.finditer(r"launcher walk: \w+ group (\d) returned: \d+ create infos, ([\d.]+) ms", log)}
    detail = (f"first mapping: terrain {groups.group(2)} at {back.get('1', '-')} ms, other {groups.group(3)} "
              f"at {back.get('2', '-')} ms") if groups else ""
    return verdict("shader-step-order", bad, detail)


# ---- the game page's commands (input) ----

# A selector entry's centre in layout px: entry i's line top is 168 + 22 i below the 360 px band,
# which starts 44 px down the 448 px layout.
def entry_point(i):
    """The mouse= action pointing at selector entry i's centre."""
    return f"mouse=320/{168 + 44 + 22 * i + 8}"


def inputs(*acts, start=1500, step=300):
    """An OPENPETE_LAUNCHER_INPUT value: each action `step` ms after the one before, the first at `start`;
    an int in the list adds that many ms to the gap before the next action."""
    out, t = [], start
    for x in acts:
        if isinstance(x, int):
            t += x
            continue
        out.append(f"{t}:{x}")
        t += step
    return ",".join(out)


def focus_moves(log):
    """The focus moves the log names, in order: (kind, from, to)."""
    out = []
    for m in re.finditer(r"command: focus (left|right) from box (\d+) to box (\d+)|"
                         r"command: focus follows the pointer from box (\d+) to box (\d+)", log):
        if m.group(1):
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
        else:
            out.append(("pointer", int(m.group(4)), int(m.group(5))))
    return out


def input_run(a, d, tag, acts, extra=None, args=None, timeout=300):
    """One windowed launcher run over the case's library with the scripted actions; returns (rc, log)."""
    env = {"OPENPETE_LAUNCHER_INPUT": acts}
    env.update(extra or {})
    return cl.launch(a.binary, d, tag, ["--launcher-window", "1280x800"] + (args or []), env, timeout=timeout)


def case_input(a):
    """T11: keys, pad and pointer drive the game page's focus, activation, file picker and quit."""
    d = cl.case_dir(a.out, "input")
    for i, g in enumerate(DISCS):
        seed_entry(d, g, len(DISCS) - i)
    bad, counts = [], {}

    def expect_moves(tag, log, want):
        got = focus_moves(log)
        counts[tag + " moves"] = len(got)
        if got != want:
            bad.append(f"{tag}: focus moves {got}, want {want}")

    # Keys: no wrap at either edge, up/down inert, Escape quits at once.
    rc, log = input_run(a, d, "keys", inputs("key=left", "key=right", "key=right", "key=right", "key=up",
                                             "key=down", "key=left", "key=escape"))
    expect_moves("keys", log, [("left", 0, 0), ("right", 0, 1), ("right", 1, 2), ("right", 2, 2),
                               ("left", 2, 1)])
    counts["keys inert"] = len(re.findall(r"command: (up|down) does nothing", log))
    if counts["keys inert"] != 2:
        bad.append(f"keys: {counts['keys inert']} up/down lines, want 2")
    if rc != 0 or "command: quit (Escape or the window closed)" not in log or "game picked" in log:
        bad.append(f"keys: exit {rc}, Escape quit {'logged' if 'Escape or the window' in log else 'absent'}")

    # Pad and stick: the d-pad and the left stick's X move one box per push (the stick re-centres before
    # it moves again); East asks once, the ask lapses after 2 s, a second East inside 2 s quits.
    rc, log = input_run(a, d, "pad", inputs("pad=left", "pad=right", "stick=x:20000", "stick=x:30000",
                                            "stick=x:0", "stick=x:20000", "stick=x:0", "stick=x:-20000",
                                            "stick=x:0", "pad=up", "pad=down", "stick=y:-20000",
                                            "pad=east", 2400, "pad=east", 700, "pad=east"))
    expect_moves("pad", log, [("left", 0, 0), ("right", 0, 1), ("right", 1, 2), ("right", 2, 2),
                              ("left", 2, 1)])
    asks = re.findall(r"command: quit asked", log)
    chip = re.findall(r"command: quit ask chip (shown|hidden)", log)
    counts["pad asks"], counts["pad chip"] = len(asks), len(chip)
    if len(asks) != 2 or chip[:3] != ["shown", "hidden", "shown"]:
        bad.append(f"pad: {len(asks)} asks (want 2), chip {chip} (want shown, hidden, shown)")
    if rc != 0 or not re.search(r"command: quit \(east again \d+ ms after the ask\)", log) or "game picked" in log:
        bad.append(f"pad: exit {rc}, second-East quit {'logged' if 'east again' in log else 'absent'}")
    if len(re.findall(r"command: (up|down) does nothing", log)) != 3:
        bad.append("pad: d-pad up/down and stick Y did not each log one inert command")

    # The pointer takes the focus only after it moves: a key move holds against a still pointer.
    rc, log = input_run(a, d, "pointer", inputs(entry_point(2), "key=left", 900, entry_point(2).replace(
        "320/", "330/"), "key=escape"))
    expect_moves("pointer", log, [("pointer", 0, 2), ("left", 2, 1), ("pointer", 1, 2)])
    if rc != 0:
        bad.append(f"pointer: exit {rc}")

    # Space and West open the file picker for the focused box's game.
    rc, log = input_run(a, d, "browse", inputs("key=right", "key=space", 1500, "pad=west", 1500, "key=escape"))
    pick = re.findall(r"command: file picker for box (\d+) \(([\w-]+)\)", log)
    counts["browse"] = len(pick)
    if len(pick) != 2 or any(b != "1" for b, _ in pick) or rc != 0:
        bad.append(f"browse: exit {rc}, picker lines {pick} (want box 1 twice)")

    # Enter and South activate the focused box: that box's game starts.
    for tag, acts, box in (("enter", ("key=right", "key=enter"), 1), ("south", ("pad=right", "pad=right", "pad=south"), 2)):
        rc, log = input_run(a, d, tag, inputs(*acts), args=["--frame-limit", "5"], timeout=600)
        m = re.search(r"command: activate box (\d+) \(([\w-]+)\)", log)
        counts[tag] = 1 if m else 0
        if not m or int(m.group(1)) != box or rc != 0:
            bad.append(f"{tag}: exit {rc}, activation {m.group(0) if m else 'absent'} (want box {box})")
        else:
            bad += [f"{tag}: {x}" for x in ordered(log, re.escape(m.group(0)), r"game picked: " + m.group(2))]

    # A disc of another game added from the page focuses that game's box when the page returns. The
    # seeded entries' last_played lie past the ingest's time stamp, so the new entry is not the most
    # recently played (box 0), where the focus would start without the added disc's.
    d2 = cl.case_dir(a.out, "input-drop")
    seed_entry(d2, "spyro-1", 4000000001)
    seed_entry(d2, "spyro-2", 4000000000)
    rc, log = cl.launch(a.binary, d2, "drop", ["--launcher-window", "1280x800", "--frame-limit", "5"],
                        {"OPENPETE_LAUNCHER_DROP": disc_cue("spyro-3"), "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
                         "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-3"}, timeout=900)
    found = re.search(r"command: focus starts on box (\d+) \(spyro-3, the disc just added\)", log)
    counts["drop focus"] = 1 if found else 0
    if found and found.group(1) == "0":
        bad.append("drop: the new entry is box 0, so the focus start does not discriminate")
    bad += [f"drop: {x}" for x in ordered(log, r"scripted drop", r"command: focus starts on box \d+ \(spyro-3",
                                          r"game picked: spyro-3")]
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    return verdict("input", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


CASES = {"startup": case_startup, "resolution": case_resolution, "multi-install": case_multi_install,
         "multi-install-extract": case_multi_install_extract,
         "job-ingest-stop": case_job_ingest_stop, "job-pick-same": case_job_pick_same,
         "job-pick-other": case_job_pick_other, "lock": case_lock, "failed-marker": case_failed_marker,
         "headless-no-disc": case_headless_no_disc, "bind-repair": case_bind_repair,
         "ingest-held": case_ingest_held, "cache": case_cache, "cache-ingest": case_cache_ingest,
         "multi-install-launch": case_multi_install_launch,
         "job-ingest-stop-assets": case_job_ingest_stop_assets,
         "job-pick-other-assets": case_job_pick_other_assets,
         "shader-step-during": case_shader_step_during, "shader-step-after": case_shader_step_after,
         "shader-step-order": case_shader_step_order, "input": case_input}


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
