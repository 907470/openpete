#!/usr/bin/env python3
"""Tests for scripts/bench/timing_lock.py and scripts/lib/timing_gate.sh.

Every test runs against a private OPENPETE_BENCH_DIR, so no real timing
window is read, created or removed."""
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
