#!/usr/bin/env python3
"""Tests for scripts/bench/timing_lock.py, scripts/lib/timing_gate.sh and the
gate's use in scripts/golden-capture-container.sh, scripts/container.sh and
scripts/release-container.sh.

Every test runs against a private OPENPETE_BENCH_DIR, so no real timing
window is read, created or removed."""
import hashlib
import json
import multiprocessing
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
ROOT = os.path.normpath(os.path.join(BENCH, "..", ".."))
sys.path.insert(0, BENCH)
import timing_lock                                          # noqa: E402


def _contend(bench_dir, barrier, results, rounds):
    os.environ["OPENPETE_BENCH_DIR"] = bench_dir
    for r in range(rounds):
        barrier.wait()
        h = timing_lock.try_acquire("contender-%d" % os.getpid())
        results.put((r, os.getpid(), h is not None))
        barrier.wait()          # both have tried before the winner releases
        if h:
            h.release()


class TimingLockTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="timing-lock-test.")
        self.old = {k: os.environ.get(k) for k in ("OPENPETE_BENCH_DIR", timing_lock.TOKEN_ENV)}
        os.environ["OPENPETE_BENCH_DIR"] = self.dir
        os.environ.pop(timing_lock.TOKEN_ENV, None)

    def tearDown(self):
        timing_lock.release_shared()
        for k, v in self.old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_two_contenders_one_winner_per_round(self):
        rounds = 40
        ctx = multiprocessing.get_context("fork")
        barrier, results = ctx.Barrier(2), ctx.Queue()
        procs = [ctx.Process(target=_contend, args=(self.dir, barrier, results, rounds))
                 for _ in range(2)]
        for p in procs:
            p.start()
        rows = [results.get(timeout=30) for _ in range(2 * rounds)]
        for p in procs:
            p.join(30)
        for r in range(rounds):
            wins = [w for rr, _, w in rows if rr == r]
            self.assertEqual(sorted(wins), [False, True], "round %d: %s" % (r, wins))
        self.assertFalse(os.path.exists(os.path.join(self.dir, "TIMING-WINDOW")))

    def test_holder_writes_and_removes_indicator(self):
        h = timing_lock.try_acquire("unit")
        line = open(os.path.join(self.dir, "TIMING-WINDOW")).read()
        self.assertIn("unit ", line)
        self.assertIn("lock-token=" + h.token, line)
        st = timing_lock.state()
        self.assertTrue(st["held"])
        self.assertFalse(st["mine"])
        os.environ[timing_lock.TOKEN_ENV] = h.token
        self.assertTrue(timing_lock.state()["mine"])
        self.assertTrue(timing_lock.wait_free(timeout=0))
        h.release()
        self.assertFalse(os.path.exists(os.path.join(self.dir, "TIMING-WINDOW")))
        self.assertFalse(timing_lock.state()["held"])

    def test_stale_lock_after_owner_killed(self):
        p = subprocess.Popen([sys.executable, os.path.join(BENCH, "timing_lock.py"), "run",
                              "--purpose", "doomed", "--", "sleep", "3"],
                             stderr=subprocess.DEVNULL)
        for _ in range(100):
            if timing_lock.state()["lock"]:
                break
            time.sleep(0.05)
        self.assertEqual(timing_lock.state()["lock"]["purpose"], "doomed")
        p.send_signal(signal.SIGKILL)       # no release path runs
        p.wait()
        ind = os.path.join(self.dir, "TIMING-WINDOW")
        self.assertTrue(os.path.exists(ind), "SIGKILL leaves the indicator behind")
        st = timing_lock.state()
        self.assertFalse(st["held"])
        self.assertFalse(os.path.exists(ind), "state() removes the stale indicator")
        h = timing_lock.try_acquire("next")
        self.assertIsNotNone(h)
        h.release()

    def test_handwritten_marker_holds_the_window(self):
        ind = os.path.join(self.dir, "TIMING-WINDOW")
        with open(ind, "w") as f:
            f.write("someitem 2026-11-20T07:00:00-06:00\n")
        st = timing_lock.state()
        self.assertTrue(st["held"])
        self.assertIn("hand-written", timing_lock.describe(st))
        self.assertIsNone(timing_lock.try_acquire("blocked"))
        self.assertFalse(timing_lock.wait_free(timeout=0.1))
        self.assertTrue(os.path.exists(ind), "a hand-written marker is never removed")

    def test_fleet_host_lock_is_separate(self):
        h = timing_lock.try_acquire("ptah leg", host="ptah")
        self.assertTrue(timing_lock.state("ptah")["held"])
        self.assertFalse(timing_lock.state()["held"])
        self.assertFalse(os.path.exists(os.path.join(self.dir, "TIMING-WINDOW")))
        h.release()

    def test_run_exports_token_and_releases(self):
        r = subprocess.run([sys.executable, os.path.join(BENCH, "timing_lock.py"), "run",
                            "--purpose", "child", "--", sys.executable, "-c",
                            "import os,sys; sys.path.insert(0, %r); import timing_lock;"
                            "st = timing_lock.state(); print(st['mine'], st['lock']['purpose'])"
                            % BENCH], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "True child")
        self.assertFalse(timing_lock.state()["held"])

    def test_acquire_release_detached(self):
        tl = os.path.join(BENCH, "timing_lock.py")
        r = subprocess.run([sys.executable, tl, "acquire", "--purpose", "multi", "--ttl", "60"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        token = r.stdout.strip().split("=", 1)[1]
        self.assertTrue(timing_lock.state()["held"])
        bad = subprocess.run([sys.executable, tl, "release", "--token", "0.0"],
                             capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertTrue(timing_lock.state()["held"])
        ok = subprocess.run([sys.executable, tl, "release", "--token", token],
                            capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertFalse(timing_lock.state()["held"])

    def test_fleet_local_host_paused(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "golden_fleet", os.path.join(ROOT, "scripts", "golden-fleet.py"))
        gf = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gf)
        owner = next(v for v in vars(gf).values()
                     if isinstance(v, type) and "local_paused" in vars(v))

        class Host:
            def __init__(self, local):
                self.local = local

        class HS:
            def __init__(self, local):
                self.host, self.name = Host(local), "local" if local else "ptah"
        fleet = owner.__new__(owner)
        local, remote = HS(True), HS(False)
        jobs = [type("J", (), {"pinned": None})()]
        self.assertIs(owner.next_job(fleet, jobs, local), jobs[0])
        h = timing_lock.try_acquire("windows leg")
        try:
            self.assertIsNone(owner.next_job(fleet, jobs, local))
            self.assertIs(owner.next_job(fleet, jobs, remote), jobs[0])
            os.environ[timing_lock.TOKEN_ENV] = h.token
            self.assertIs(owner.next_job(fleet, jobs, local), jobs[0])
        finally:
            os.environ.pop(timing_lock.TOKEN_ENV, None)
            h.release()

    def test_bash_gate(self):
        gate = os.path.join(ROOT, "scripts", "lib", "timing_gate.sh")
        sh = 'source %s; op_timing_gate unit-test; echo rc=$?' % gate
        env = dict(os.environ, OPENPETE_TIMING_GATE_TIMEOUT="1")
        free = subprocess.run(["bash", "-c", sh], env=env, capture_output=True, text=True)
        self.assertIn("rc=0", free.stdout)
        h = timing_lock.try_acquire("other job")
        held = subprocess.run(["bash", "-c", sh], env=env, capture_output=True, text=True)
        self.assertIn("rc=75", held.stdout, held.stderr)
        self.assertIn("other job", held.stderr)
        env[timing_lock.TOKEN_ENV] = h.token
        inside = subprocess.run(["bash", "-c", sh], env=env, capture_output=True, text=True)
        self.assertIn("rc=0", inside.stdout, inside.stderr)
        h.release()

    def _fake_engine(self, label=""):
        """Write a stub container engine that answers `image inspect` (with
        `label` for a Labels query) and logs every other call; its path."""
        engine = os.path.join(self.dir, "fake-engine")
        self.engine_log = os.path.join(self.dir, "engine.log")
        with open(engine, "w") as f:
            f.write('#!/bin/bash\n'
                    'if [[ "$1 $2" == "image inspect" ]]; then\n'
                    '  [[ "$*" == *Labels* ]] && echo %s || echo sha256:0\n'
                    'else echo "$*" >>%s; fi\n' % (label, self.engine_log))
        os.chmod(engine, 0o755)
        return engine

    def _capture_container(self, *args, env_extra=None):
        """Start scripts/golden-capture-container.sh against a stub engine
        that logs each `run` and a throwaway tree, fleet root and store."""
        d = self.dir
        tree, fleet, store = (os.path.join(d, n) for n in ("tree", "fleet", "store"))
        for p in (tree, fleet, store):
            os.makedirs(p, exist_ok=True)
        recipe = os.path.join(ROOT, "tests", "harness", "linux-capture.Dockerfile")
        with open(recipe, "rb") as f:
            recipe_hash = hashlib.sha256(f.read()).hexdigest()
        engine = self._fake_engine(recipe_hash)
        env = dict(os.environ, OPENPETE_CONTAINER_ENGINE=engine, OPENPETE_CAPTURE_TREE=tree,
                   OPENPETE_FLEET_ROOT=fleet, OPENPETE_GOLDEN_REF_DIR=store)
        env.pop("OPENPETE_GOLDEN_LOCK_HELD", None)
        env.update(env_extra or {})
        script = os.path.join(ROOT, "scripts", "golden-capture-container.sh")
        self.golden_lock = os.path.join(fleet, "golden.lock")
        return subprocess.Popen(["bash", script, *args], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def _launches(self):
        if not os.path.exists(self.engine_log):
            return []
        with open(self.engine_log) as f:
            return [line for line in f if line.startswith("run ")]

    def test_capture_container_launch_waits_for_window(self):
        free = self._capture_container("run", "--golden", "--", "true")
        out, err = free.communicate(timeout=30)
        self.assertEqual(free.returncode, 0, err)
        self.assertEqual(len(self._launches()), 1, err)
        os.remove(self.engine_log)

        h = timing_lock.try_acquire("timing leg")
        try:
            proc = self._capture_container("run", "--golden", "--", "true")
            time.sleep(3)
            self.assertIsNone(proc.poll())
            self.assertEqual(self._launches(), [])
            # The golden lock is taken before the gate and kept while it waits.
            self.assertNotEqual(subprocess.run(["flock", "-n", self.golden_lock, "true"]).returncode, 0)
        finally:
            h.release()
        out, err = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, err)
        self.assertEqual(len(self._launches()), 1, err)
        self.assertIn("timing leg", err)

        os.remove(self.engine_log)
        h = timing_lock.try_acquire("timing leg")
        try:
            refused = self._capture_container("run", "--golden", "--", "true",
                                              env_extra={"OPENPETE_TIMING_GATE_TIMEOUT": "1"})
            out, err = refused.communicate(timeout=30)
        finally:
            h.release()
        self.assertEqual(refused.returncode, timing_lock.EXIT_TIMEOUT, err)
        self.assertEqual(self._launches(), [])

    def test_capture_container_mounts_timing_dir(self):
        """The container gets the host's timing-window directory read-write
        at its own path and OPENPETE_BENCH_DIR naming it, so the per-capture
        gate inside probes the host's flock; a holder's token is passed."""
        proc = self._capture_container("run", "--", "true",
                                       env_extra={timing_lock.TOKEN_ENV: "tok.1"})
        out, err = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, err)
        (line,) = self._launches()
        argv = line.split()
        bench = os.path.realpath(self.dir)
        self.assertIn("%s:%s" % (bench, bench), [argv[i + 1] for i, a in enumerate(argv) if a == "-v"])
        envs = [argv[i + 1] for i, a in enumerate(argv) if a == "-e"]
        self.assertIn("OPENPETE_BENCH_DIR=" + bench, envs)
        self.assertIn("%s=tok.1" % timing_lock.TOKEN_ENV, envs)

    def _scratch_tree(self, files):
        """A throwaway git tree holding copies of the repo files in
        files["copy"] and an exit-0 script at each path in files["stub"];
        its path."""
        tree = os.path.join(self.dir, "tree")
        for rel in files["copy"]:
            dst = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(ROOT, rel), dst)
        for rel in files["stub"]:
            dst = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w") as f:
                f.write("#!/bin/bash\nexit 0\n")
            os.chmod(dst, 0o755)
        subprocess.run(["git", "init", "-q", tree], check=True)
        return tree

    def _assert_launch_waits(self, argv, env, want_runs):
        """Run argv free (want_runs engine runs), held (no run until the
        window closes, then want_runs) and held with a 1 s gate timeout
        (exit 75, no run); the free run's engine lines."""
        def launch(extra=None):
            return subprocess.Popen(argv, env=dict(env, **(extra or {})), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        free = launch()
        out, err = free.communicate(timeout=60)
        self.assertEqual(free.returncode, 0, out + err)
        first = self._launches()
        self.assertEqual(len(first), want_runs, out + err)
        os.remove(self.engine_log)

        h = timing_lock.try_acquire("timing leg")
        try:
            proc = launch()
            time.sleep(3)
            self.assertIsNone(proc.poll())
            self.assertEqual(self._launches(), [])
        finally:
            h.release()
        out, err = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, out + err)
        self.assertEqual(len(self._launches()), want_runs, out + err)
        self.assertIn("timing leg", err)

        os.remove(self.engine_log)
        h = timing_lock.try_acquire("timing leg")
        try:
            refused = launch({"OPENPETE_TIMING_GATE_TIMEOUT": "1"})
            out, err = refused.communicate(timeout=60)
        finally:
            h.release()
        self.assertEqual(refused.returncode, timing_lock.EXIT_TIMEOUT, out + err)
        self.assertEqual(self._launches(), [])
        return first

    def test_dev_container_launch_waits_for_window(self):
        tree = self._scratch_tree({
            "copy": ["scripts/container.sh", "scripts/lib/container.sh",
                     "scripts/lib/conmon-no-session-bus", "scripts/lib/timing_gate.sh",
                     "scripts/lib/flock_hold.py",
                     "scripts/bench/timing_lock.py", "packaging/dev-image.Dockerfile",
                     "packaging/dev-image.sh", "packaging/dev-image-packages.txt"],
            "stub": []})
        env = dict(os.environ, OPENPETE_CONTAINER_ENGINE=self._fake_engine())
        (line,) = self._assert_launch_waits(
            ["bash", os.path.join(tree, "scripts", "container.sh"), "shell", "true"], env, 1)
        bench = os.path.realpath(self.dir)
        self.assertIn("-v %s:%s " % (bench, bench), line)
        self.assertIn("-e OPENPETE_BENCH_DIR=%s " % bench, line)

    def test_release_container_launch_waits_for_window(self):
        tree = self._scratch_tree({
            "copy": ["scripts/release-container.sh", "scripts/lib/container.sh",
                     "scripts/lib/conmon-no-session-bus", "scripts/lib/timing_gate.sh",
                     "scripts/lib/flock_hold.py",
                     "scripts/lib/tools_dir.sh", "scripts/lib/release_archive.sh",
                     "scripts/bench/timing_lock.py", "packaging/linux-release.Dockerfile"],
            "stub": ["scripts/setup_linux_clang.sh", "scripts/setup_appimagetool.sh"]})
        env = dict(os.environ, OPENPETE_CONTAINER_ENGINE=self._fake_engine(),
                   OPENPETE_DIST_VERSION="x-test", OPENPETE_TOOLS_DIR=os.path.join(self.dir, "tools"))
        self._assert_launch_waits(["bash", os.path.join(tree, "scripts", "release-container.sh"),
                                   "--test", "--out", "dist"], env, 1)

    def test_display_hold_beats_and_reads_held(self):
        """The holder re-asserts on every beat and rewrites its record; the
        record reads as held only for the window holder's token, a live
        writer and a fresh beat, never from the last input time; release
        removes it. A fake child answers PowerShell's protocol."""
        fake = ("import sys\nprint('display-hold: on prev=2147483648', flush=True)\n"
                "for _ in sys.stdin: print('display-hold: beat ret=2147483651', flush=True)\n")
        h = timing_lock.try_acquire("display test")
        os.environ[timing_lock.TOKEN_ENV] = h.token
        old = timing_lock.DISPLAY_BEAT_S
        timing_lock.DISPLAY_BEAT_S = 0.05
        p = subprocess.Popen([sys.executable, "-c", fake], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        self.assertTrue(p.stdout.readline().startswith("display-hold: on"))
        d = timing_lock.DisplayHold(p, h.token, 2147483648)
        try:
            first = json.load(open(timing_lock.display_record_path()))["beat"]
            time.sleep(0.4)
            rec = json.load(open(timing_lock.display_record_path()))
            self.assertGreater(rec["beat"], first, "no beat rewrote the record")
            self.assertEqual(rec["ret"], 2147483651)
            st = timing_lock.display_hold_state()
            self.assertTrue(st["held"], st["why"])
            stale = timing_lock.display_hold_state(
                now=rec["beat"] + timing_lock.DISPLAY_STALE_S + 1)
            self.assertFalse(stale["held"])
            self.assertIn("last beat", stale["why"])
        finally:
            d.release()
            timing_lock.DISPLAY_BEAT_S = old
        self.assertFalse(os.path.exists(timing_lock.display_record_path()))
        self.assertIn("no display holder", timing_lock.display_hold_state()["why"])
        self.assertIsNotNone(p.poll(), "the child outlived release")
        h.release()

    def test_display_hold_wrong_token_or_dead_writer(self):
        h = timing_lock.try_acquire("display test")
        with open(timing_lock.display_record_path(), "w") as f:
            json.dump({"token": "someone-else", "pid": os.getpid(), "since": time.time(),
                       "beat": time.time(), "ret": 1}, f)
        st = timing_lock.display_hold_state()
        self.assertFalse(st["held"])
        self.assertIn("token", st["why"])
        with open(timing_lock.display_record_path(), "w") as f:
            json.dump({"token": h.token, "pid": 2 ** 22 + 7, "since": time.time(),
                       "beat": time.time(), "ret": 1}, f)
        self.assertIn("gone", timing_lock.display_hold_state()["why"])
        h.release()


GATE_SH = os.path.join(ROOT, "scripts", "lib", "timing_gate.sh")
TL = os.path.join(BENCH, "timing_lock.py")


def _wait_for(path, timeout=20.0):
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > end:
            raise AssertionError("timed out waiting for %s" % path)
        time.sleep(0.02)


class LaunchHoldTest(unittest.TestCase):
    """The gate holds the window shut from its return until the launch
    exits: the check-then-act gap between a launch's gate and its start is
    closed, and a waiting timing job takes the window at a later gate."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="timing-hold-test.")
        self.old = {k: os.environ.get(k) for k in
                    ("OPENPETE_BENCH_DIR", timing_lock.TOKEN_ENV, timing_lock.COVERED_ENV)}
        os.environ["OPENPETE_BENCH_DIR"] = self.dir
        os.environ.pop(timing_lock.TOKEN_ENV, None)
        os.environ.pop(timing_lock.COVERED_ENV, None)
        self.procs = []

    def tearDown(self):
        open(self.f("release"), "w").close()
        for p in self.procs:
            try:
                p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        timing_lock.release_shared()
        for k, v in self.old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.dir, ignore_errors=True)

    def f(self, name):
        return os.path.join(self.dir, name)

    def bash(self, script, **env_extra):
        """Start bash running `script` with GATE (the gate library) and D
        (the scratch directory) set; it is waited for at teardown."""
        env = dict(os.environ, GATE=GATE_SH, D=self.dir, TL=TL, **env_extra)
        p = subprocess.Popen(["bash", "-c", script], env=env, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def writer(self, name):
        """Start a timing job (`run`) that touches <name>.got once it holds the
        window and releases when the scratch `release` file appears."""
        p = subprocess.Popen(
            [sys.executable, TL, "run", "--purpose", name, "--", "bash", "-c",
             'touch "$D/%s.got"; until [ -e "$D/%s.go" ] || [ -e "$D/release" ]; '
             'do sleep 0.05; done' % (name, name)],
            env=dict(os.environ, D=self.dir), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def test_gate_then_launch_excludes_timing_job(self):
        # Scripted order of the race: the launch's gate returns, a timing job
        # tries to take the window, then the launch starts. Before the hold
        # the timing job won and the launch ran beside its legs.
        launch = self.bash('source "$GATE"; op_timing_gate launch || exit 9; '
                           'touch "$D/gated"; until [ -e "$D/go" ]; do sleep 0.05; done; '
                           'touch "$D/launched"; until [ -e "$D/release" ]; do sleep 0.05; done')
        _wait_for(self.f("gated"))
        self.assertIsNone(timing_lock.try_acquire("timing job"),
                          "a timing job took the window between a launch's gate and its start")
        open(self.f("go"), "w").close()
        _wait_for(self.f("launched"))
        self.assertIsNone(timing_lock.try_acquire("timing job"))
        open(self.f("release"), "w").close()
        launch.wait(timeout=20)
        end = time.monotonic() + 10
        h = None
        while h is None and time.monotonic() < end:
            h = timing_lock.try_acquire("timing job")
            time.sleep(0.05)
        self.assertIsNotNone(h, "the hold outlived its shell")
        h.release()

    def test_waiting_timing_job_blocks_new_launches_and_takes_the_next_gate(self):
        harness = self.bash('source "$GATE"; op_timing_gate first || exit 9; touch "$D/held"; '
                            'until [ -e "$D/next" ]; do sleep 0.05; done; '
                            'op_timing_gate second || exit 8; touch "$D/second"; '
                            'until [ -e "$D/release" ]; do sleep 0.05; done')
        _wait_for(self.f("held"))
        w = self.writer("w")
        end = time.monotonic() + 10
        while not timing_lock.state()["wanted"] and time.monotonic() < end:
            time.sleep(0.05)
        self.assertTrue(timing_lock.state()["wanted"], "the timing job does not want the window")
        self.assertFalse(os.path.exists(self.f("w.got")), "the window opened beside a held launch")
        late = subprocess.run(["bash", "-c", 'source "$GATE"; op_timing_gate late; echo rc=$?'],
                              env=dict(os.environ, GATE=GATE_SH, OPENPETE_TIMING_GATE_TIMEOUT="1"),
                              capture_output=True, text=True, timeout=30)
        self.assertIn("rc=75", late.stdout, "a new launch started ahead of a waiting timing job")
        open(self.f("next"), "w").close()          # the harness's next gate yields
        _wait_for(self.f("w.got"))
        self.assertFalse(os.path.exists(self.f("second")), "the yielded gate returned inside the window")
        open(self.f("w.go"), "w").close()
        self.assertEqual(w.wait(timeout=20), 0, w.stderr.read())
        _wait_for(self.f("second"))

    def test_child_of_a_held_launch_does_not_wait(self):
        harness = self.bash('source "$GATE"; op_timing_gate parent || exit 9; touch "$D/held"; '
                            'until [ -e "$D/child" ]; do sleep 0.05; done; '
                            'python3 -c "import sys; sys.path.insert(0, \'%s\'); import timing_lock; '
                            'print(timing_lock.gate(label=\'child\', timeout=2))" > "$D/child.out"; '
                            'touch "$D/child.done"; until [ -e "$D/release" ]; do sleep 0.05; done'
                            % BENCH)
        _wait_for(self.f("held"))
        self.writer("w")
        end = time.monotonic() + 10
        while not timing_lock.state()["wanted"] and time.monotonic() < end:
            time.sleep(0.05)
        open(self.f("child"), "w").close()
        _wait_for(self.f("child.done"))
        with open(self.f("child.out")) as fh:
            self.assertEqual(fh.read().strip(), "True",
                             "a child of a held launch waited on a timing job that waits on its parent")

    def test_timing_job_under_its_own_launch_hold_refuses(self):
        p = self.bash('source "$GATE"; op_timing_gate parent || exit 9; '
                      'python3 "$TL" run --purpose nested -- true; echo rc=$?')
        out, err = p.communicate(timeout=30)
        self.assertIn("rc=69", out, err)
        self.assertIn("ancestor", err)

    def test_in_process_hold_upgrades_to_the_window(self):
        self.assertTrue(timing_lock.gate(label="harness"))
        self.assertIn(os.getpid(), timing_lock.flock_hold.lockers(timing_lock.lock_path(), "READ"))
        probe = timing_lock.flock_hold.open_lock(timing_lock.lock_path())
        self.assertFalse(timing_lock.flock_hold.try_lock(probe, timing_lock.flock_hold.EX),
                         "a hold did not keep LOCK_EX out")
        os.close(probe)
        child = subprocess.run([sys.executable, TL, "run", "--purpose", "child", "--", "true"],
                               capture_output=True, text=True, timeout=30)
        self.assertEqual(child.returncode, timing_lock.EXIT_NESTED, child.stderr)
        h = timing_lock.try_acquire("this harness")
        self.assertIsNotNone(h, "this process's own hold blocked its own timing job")
        h.release()

    def test_covered_container_does_not_wait(self):
        self.writer("w")
        _wait_for(self.f("w.got"))
        os.environ[timing_lock.COVERED_ENV] = "1"
        self.assertTrue(timing_lock.gate(label="in container", timeout=1))
        os.environ.pop(timing_lock.COVERED_ENV)
        self.assertFalse(timing_lock.gate(label="host launch", timeout=1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
