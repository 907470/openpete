#!/usr/bin/env python3
"""Tests for scripts/bench/preflight.py: unreadable preconditions report
UNCHECKED and refuse, window and post-log readings, the load sample, and
the verdict line. No game, ssh or Windows tool is run: every reader is
replaced by a fake. The game binaries are named as strings only:
rumble-guard: no-launch, input-lock-guard: no-launch, xvfb-guard: no-launch,
sandbox-guard: no-launch, wslenv-guard: no-launch"""
import io
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
import preflight                                            # noqa: E402
import timing_lock                                          # noqa: E402


def _no_tool(*_a, **_k):
    return None, ""


class UncheckedTest(unittest.TestCase):
    """A precondition that cannot be read is UNCHECKED, never PASS."""

    def test_tasklist_missing(self):
        with mock.patch.object(preflight, "_run", _no_tool):
            c = preflight.check_idle_windows()
        self.assertEqual(c.status, "UNCHECKED")
        self.assertTrue(c.blocks())

    def test_pgrep_missing(self):
        with mock.patch.object(preflight, "_run", _no_tool):
            c = preflight.check_idle_linux()
        self.assertEqual(c.status, "UNCHECKED")
        self.assertTrue(c.blocks())

    def test_display_query_failed(self):
        with mock.patch.object(preflight, "_run", _no_tool):
            ds = preflight.windows_display_state()
        self.assertIsNone(ds)
        c = preflight.check_display(ds, required=True)
        self.assertEqual(c.status, "UNCHECKED")
        self.assertTrue(c.blocks())
        self.assertFalse(preflight.check_display(ds, required=False).blocks())
        self.assertEqual(preflight.check_refresh(ds).status, "UNCHECKED")

    def test_display_timeout_unreadable(self):
        c = preflight.check_display({"idle_ms": 5000, "refresh": []}, True)
        self.assertEqual(c.status, "UNCHECKED")

    def test_ssh_unreachable(self):
        with mock.patch.object(preflight, "_run", _no_tool):
            out = preflight.remote_checks("ptah", {"ssh": "nobody@0.0.0.0"}, None)
        self.assertEqual([(c.name, c.status) for c in out], [("remote", "UNCHECKED")])
        self.assertTrue(out[0].blocks())

    def test_proc_stat_unreadable(self):
        c = preflight.load_check("load", "", "", "?", 8)
        self.assertEqual(c.status, "UNCHECKED")

    def test_typeperf_missing(self):
        import bench_guard
        with mock.patch.object(bench_guard, "_windows_cpu_load", lambda: None):
            c = preflight.check_load_windows()
        self.assertEqual(c.status, "UNCHECKED")

    def test_tuned_without_build_tree(self):
        d = tempfile.mkdtemp()
        try:
            exe = os.path.join(d, "openpete.exe")
            open(exe, "wb").write(b"x")
            c = preflight.check_tuned(exe)
            self.assertEqual(c.status, "UNCHECKED")
        finally:
            shutil.rmtree(d)

    def test_accept_names_it(self):
        cs = [preflight.Check("display", "UNCHECKED", "query failed")]
        preflight.apply_accepts(cs, {"display"})
        self.assertEqual(cs[0].status, "ACCEPTED")
        self.assertIn("was UNCHECKED", cs[0].reading)
        self.assertFalse(cs[0].blocks())

    def test_verdict_line(self):
        buf = io.StringIO()
        bad = preflight.report("t", [preflight.Check("a", "PASS", "ok"),
                                     preflight.Check("b", "UNCHECKED", "?"),
                                     preflight.Check("c", "UNCHECKED", "?", required=False)], buf)
        self.assertEqual(bad, ["b"])
        self.assertTrue(buf.getvalue().endswith("preflight[t]: REFUSED (b)\n"))


class ReadingsTest(unittest.TestCase):
    def test_load_sample(self):
        a = "cpu  100 0 100 800 0 0 0 0 0 0\n"
        b = "cpu  150 0 150 1700 0 0 0 0 0 0\n"     # 100 busy of 1000
        c = preflight.load_check("load", a, b, "0.5", 8)
        self.assertEqual(c.status, "PASS")
        self.assertIn("10.0% busy", c.reading)
        b2 = "cpu  300 0 100 1600 0 0 0 0 0 0\n"    # 200 busy of 1000
        self.assertEqual(preflight.load_check("load", a, b2, "0.5", 8).status, "FAIL")

    def test_display_states(self):
        awake = preflight.check_display({"idle_ms": 10000, "videoidle_ac": 900, "refresh": []}, True)
        asleep = preflight.check_display({"idle_ms": 7200000, "videoidle_ac": 900, "refresh": []},
                                         True)
        self.assertEqual((awake.status, asleep.status), ("PASS", "FAIL"))

    def test_window_args(self):
        exe = "/x/build/linux-release/openpete"
        cw = preflight.check_window
        self.assertEqual(cw(exe, ["--headless"], {}).status, "PASS")
        self.assertEqual(cw(exe, ["--fullscreen", "1", "--background", "1"], {}).status, "FAIL")
        self.assertEqual(cw(exe, ["--fullscreen", "1"], {"OPENPETE_BACKGROUND": "1"}).status,
                         "FAIL")
        self.assertEqual(cw(exe, ["--fullscreen", "1", "--window", "640x448"], {}).status, "FAIL")
        self.assertEqual(cw(exe, ["--fullscreen", "0"], {}).status, "UNCHECKED")
        self.assertEqual(cw(exe, ["--fullscreen", "0", "--window", "1280x896"], {}).status, "PASS")

    def test_split_leg(self):
        exe, args, env = preflight.split_leg(
            ["env", "OPENPETE_BACKGROUND=1", "/mnt/c/op/openpete.exe", "--vsync", "0"])
        self.assertEqual(exe, "/mnt/c/op/openpete.exe")
        self.assertEqual(args, ["--vsync", "0"])
        self.assertEqual(env, {"OPENPETE_BACKGROUND": "1"})

    def test_windows_local(self):
        d = tempfile.mkdtemp()
        try:
            disc = os.path.join(d, "disc.cue")
            open(disc, "w").close()
            c = preflight.check_windows_local("/mnt/c/op/openpete.exe", ["--disc", disc])
            self.assertEqual(c.status, "FAIL")
            self.assertIn(disc, c.reading)
            self.assertEqual(preflight.check_windows_local(
                "/mnt/c/op/openpete.exe", ["--frame-limit", "100"]).status, "PASS")
            self.assertEqual(preflight.check_windows_local(
                "/home/u/build/windows-mingw/openpete.exe", []).status, "FAIL")
        finally:
            shutil.rmtree(d)

    def test_post_log(self):
        d = tempfile.mkdtemp()
        try:
            log = os.path.join(d, "leg.log")
            with open(log, "w") as f:
                f.write("present mode: immediate (--vsync 0)\n"
                        "swapchain: 3 image(s), acquire budget 3.1 ms (refresh 480.000 Hz)\n"
                        "offscreen extent follows the swapchain at 640x432 (swapchain 640x432)\n")
            cs = {c.name: c for c in preflight.post_checks(log, "1280x896")}
            self.assertEqual(cs["window-actual"].status, "FAIL")
            self.assertEqual(cs["present-mode-actual"].reading, "immediate (--vsync 0)")
            self.assertEqual(cs["refresh-actual"].reading, "480.000 Hz")
            cs = {c.name: c for c in preflight.post_checks(log, "640x432")}
            self.assertEqual(cs["window-actual"].status, "PASS")
            missing = preflight.post_checks(os.path.join(d, "none.log"), "640x432")
            self.assertEqual(missing[0].status, "UNCHECKED")
        finally:
            shutil.rmtree(d)

    def test_lock_check(self):
        d = tempfile.mkdtemp()
        old = os.environ.get("OPENPETE_BENCH_DIR"), os.environ.pop(timing_lock.TOKEN_ENV, None)
        os.environ["OPENPETE_BENCH_DIR"] = d
        try:
            self.assertEqual(preflight.check_lock(None).status, "FAIL")
            h = timing_lock.try_acquire("other")
            self.assertEqual(preflight.check_lock(None).status, "FAIL")
            os.environ[timing_lock.TOKEN_ENV] = h.token
            self.assertEqual(preflight.check_lock(None).status, "PASS")
            h.release()
        finally:
            os.environ.pop(timing_lock.TOKEN_ENV, None)
            if old[0] is None:
                os.environ.pop("OPENPETE_BENCH_DIR", None)
            else:
                os.environ["OPENPETE_BENCH_DIR"] = old[0]
            if old[1]:
                os.environ[timing_lock.TOKEN_ENV] = old[1]
            shutil.rmtree(d)

    def test_wait_never_launches_on_a_config_failure(self):
        calls = []

        def fake(*_a, **_k):
            calls.append(1)
            return [preflight.Check("window", "FAIL", "conflict")]
        with mock.patch.object(preflight, "run_checks", fake), \
                mock.patch.object(sys, "stdout", io.StringIO()):
            bad = preflight.preflight("local", wait=True)
        self.assertEqual(bad, ["window"])
        self.assertEqual(len(calls), 1, "a non-waitable failure refuses at once")

    def test_lock_waitable_only_when_held_by_another(self):
        free = preflight.Check("lock", "FAIL", "free: this job holds no window")
        held = preflight.Check("lock", "FAIL", "held by item (pid 1)")
        self.assertFalse(free.waitable())
        self.assertTrue(held.waitable())
        self.assertTrue(preflight.Check("idle-linux", "FAIL", "1 busy").waitable())


if __name__ == "__main__":
    unittest.main(verbosity=2)
