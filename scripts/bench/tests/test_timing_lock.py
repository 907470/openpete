#!/usr/bin/env python3
"""Tests for scripts/bench/timing_lock.py, scripts/lib/timing_gate.sh and the
gate's use in scripts/golden-capture-container.sh.

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
        engine = os.path.join(d, "fake-engine")
        self.engine_log = os.path.join(d, "engine.log")
        with open(engine, "w") as f:
            f.write('#!/bin/bash\n'
                    'if [[ "$1 $2" == "image inspect" ]]; then\n'
                    '  [[ "$*" == *Labels* ]] && echo %s || echo sha256:0\n'
                    'else echo "$*" >>%s; fi\n' % (recipe_hash, self.engine_log))
        os.chmod(engine, 0o755)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
