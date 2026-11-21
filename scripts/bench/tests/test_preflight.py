#!/usr/bin/env python3
"""Tests for scripts/bench/preflight.py: unreadable preconditions report
UNCHECKED and refuse, window and post-log readings, the load sample, and
the verdict line. No game, ssh or Windows tool is run: every reader is
replaced by a fake. The game binaries are named as strings only:
rumble-guard: no-launch, input-lock-guard: no-launch, xvfb-guard: no-launch,
sandbox-guard: no-launch, wslenv-guard: no-launch"""
import fcntl
import io
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
import preflight                                            # noqa: E402
import timing_lock                                          # noqa: E402


def _no_tool(*_a, **_k):
    return None, ""


def _put(path, text):
    with open(path, "w") as f:
        f.write(text)


NOT_HELD = {"held": False, "why": "no display holder", "record": None}


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
        c = preflight.check_display(ds, "awake", NOT_HELD)
        self.assertEqual(c.status, "UNCHECKED")
        self.assertTrue(c.blocks())
        self.assertFalse(preflight.check_display(ds, None, NOT_HELD).blocks())
        m = preflight.check_monitors(ds, None, required=True)
        self.assertEqual(m.status, "UNCHECKED")
        self.assertTrue(m.blocks())

    def test_display_timeout_unreadable(self):
        c = preflight.check_display({"idle_ms": 5000, "monitors": []}, "awake", NOT_HELD)
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

    def test_hot_process_fails_a_quiet_machine(self):
        a = "cpu  100 0 100 800 0 0 0 0 0 0\n"
        b = "cpu  130 0 130 1740 0 0 0 0 0 0\n"     # 6% busy machine-wide
        st0 = "41 (python3) R 1 1 1 0 -1 0 0 0 0 0 100 50 0 0\n7 (my job) S 1 1 1 0 -1 0 0 0 0 0 1 1\n"
        st1 = "41 (python3) R 1 1 1 0 -1 0 0 0 0 0 290 54 0 0\n7 (my job) S 1 1 1 0 -1 0 0 0 0 0 400 1\n"
        hot = preflight.hot_processes(preflight.proc_ticks(st0), preflight.proc_ticks(st1),
                                      2.0, exclude={"7"}, hz=100)
        self.assertEqual([(round(v), p, c) for v, p, c in hot], [(97, "41", "python3")])
        c = preflight.load_check("load-linux", a, b, "0.9", 16, hot)
        self.assertEqual(c.status, "FAIL")
        self.assertIn("python3[41] 97%", c.reading)
        self.assertEqual(preflight.load_check("load-linux", a, b, "0.9", 16, []).status, "PASS")

    def test_display_states(self):
        awake = preflight.check_display({"idle_ms": 10000, "videoidle_ac": 900}, "awake", NOT_HELD)
        asleep = preflight.check_display({"idle_ms": 7200000, "videoidle_ac": 900}, "awake",
                                         NOT_HELD)
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


class WaitBeforeWindowTest(unittest.TestCase):
    """`leg --wait` waits holding no window, takes it once its waitable
    checks clear, and never holds it while waiting on a golden lock."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="preflight-wait.")
        self.old = {k: os.environ.get(k) for k in ("OPENPETE_BENCH_DIR", timing_lock.TOKEN_ENV)}
        os.environ["OPENPETE_BENCH_DIR"] = self.dir
        os.environ.pop(timing_lock.TOKEN_ENV, None)
        self.golden = os.path.join(self.dir, "golden.lock")
        open(self.golden, "w").close()
        self.calls = []
        patches = [mock.patch.object(preflight, "run_checks", self._checks),
                   mock.patch.object(preflight, "WAIT_POLL_S", 0.05),
                   mock.patch.object(sys, "stdout", io.StringIO())]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        preflight._release_window()
        for k, v in self.old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.dir, ignore_errors=True)

    def _golden_held(self):
        fd = os.open(self.golden, os.O_RDONLY)
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            return False
        except OSError:
            return True
        finally:
            os.close(fd)

    def _checks(self, *_a, **_k):
        """The lock check and a golden-lock check read from a real flock;
        records whether the window was held at each read."""
        self.calls.append(timing_lock.state()["held"])
        held = self._golden_held() or getattr(self, "fail_when_held", False) and \
            timing_lock.state()["held"]
        return [preflight.check_lock(None),
                preflight.Check("golden-lock", "FAIL" if held else "PASS", "probe")]

    def test_waits_unheld_then_takes_the_window(self):
        fd = os.open(self.golden, os.O_RDWR)
        fcntl.flock(fd, fcntl.LOCK_EX)
        out = {}
        t = threading.Thread(target=lambda: out.setdefault("bad", preflight.preflight(
            "local", wait=True, take_window="test leg")))
        t.start()
        time.sleep(0.5)
        self.assertTrue(t.is_alive())
        self.assertFalse(timing_lock.state()["held"], "the window is held while waiting")
        self.assertGreater(len(self.calls), 2)
        self.assertNotIn(True, self.calls)
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)
        t.join(10)
        self.assertFalse(t.is_alive())
        self.assertEqual(out["bad"], [])
        st = timing_lock.state()
        self.assertTrue(st["held"] and st["mine"])
        self.assertEqual(st["lock"]["purpose"], "test leg")

    def test_recheck_failure_releases_and_is_bounded(self):
        self.fail_when_held = True
        bad = preflight.preflight("local", wait=True, take_window="test leg")
        self.assertEqual(bad, ["golden-lock"])
        self.assertEqual(self.calls.count(True), preflight.WINDOW_TRIES)
        self.assertFalse(timing_lock.state()["held"])
        self.assertIsNone(os.environ.get(timing_lock.TOKEN_ENV))

    def test_caller_held_window_refuses_golden_wait_at_once(self):
        h = timing_lock.try_acquire("runner")
        os.environ[timing_lock.TOKEN_ENV] = h.token
        fd = os.open(self.golden, os.O_RDWR)
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            bad = preflight.preflight("local", wait=True, take_window="test leg")
        finally:
            os.close(fd)
            h.release()
        self.assertEqual(bad, ["golden-lock"])
        self.assertEqual(len(self.calls), 1)


MON_OUT = ("idle_ms=1200000\r\nvideoidle_ac=900\r\n"
           "monitor=\\\\.\\DISPLAY1|0|2560x1440|59|32|2560,0\r\n"
           "monitor=\\\\.\\DISPLAY2|1|2560x1440|480|32|0,0\r\n")
HELD = {"held": True, "why": "held since t by pid 1; last beat 3 s ago", "record": {}}


class DisplayAndMonitorTest(unittest.TestCase):
    def test_held_display_passes_past_the_timeout(self):
        """Last input 1200 s ago against a 900 s timeout: the inference says
        asleep, the hold record says held, and the check reads the hold."""
        ds = preflight.parse_display_state(MON_OUT)
        self.assertEqual(preflight.check_display(ds, "held", HELD).status, "PASS")
        self.assertEqual(preflight.check_display(ds, "awake", HELD).status, "PASS")
        self.assertEqual(preflight.check_display(ds, "awake", NOT_HELD).status, "FAIL")
        c = preflight.check_display(ds, "held", NOT_HELD)
        self.assertEqual(c.status, "FAIL")
        self.assertIn("not held", c.reading)
        self.assertFalse(preflight.check_display(ds, None, NOT_HELD).blocks())

    def test_monitor_parse(self):
        ds = preflight.parse_display_state(MON_OUT)
        self.assertEqual(preflight.primary_hz(ds), 480)
        self.assertEqual([m["hz"] for m in ds["monitors"]], ["59", "480"])

    def test_monitor_baseline(self):
        d = tempfile.mkdtemp()
        try:
            base = os.path.join(d, "base.json")
            ds = preflight.parse_display_state(MON_OUT)
            c = preflight.check_monitors(ds, base, True)
            self.assertEqual(c.status, "PASS")
            self.assertIn("recorded as the baseline", c.reading)
            self.assertEqual(preflight.check_monitors(ds, base, True).status, "PASS")
            changed = preflight.parse_display_state(MON_OUT.replace("|59|", "|144|"))
            c = preflight.check_monitors(changed, base, True)
            self.assertEqual(c.status, "FAIL")
            self.assertIn("differs", c.reading)
            fewer = preflight.parse_display_state(MON_OUT.split("monitor=\\\\.\\DISPLAY1")[0] +
                                                  "monitor=\\\\.\\DISPLAY2|1|2560x1440|480|32|0,0")
            self.assertEqual(preflight.check_monitors(fewer, base, True).status, "FAIL")
            self.assertEqual(preflight.check_monitors(ds, None, True).status, "FAIL")
            self.assertEqual(preflight.check_monitors(ds, None, False).status, "INFO")
        finally:
            shutil.rmtree(d)


class ClassTest(unittest.TestCase):
    def test_no_class_refuses(self):
        self.assertTrue(preflight.check_class(None, False, [], {}).blocks())

    def test_cost(self):
        cc = preflight.check_class
        self.assertEqual(cc("cost", False, ["--vsync", "0", "--render-fps", "9999"], {}).status,
                         "PASS")
        self.assertEqual(cc("cost", False, ["--vsync", "1", "--render-fps", "9999"], {}).status,
                         "FAIL")
        self.assertEqual(cc("cost", False, ["--vsync", "0"], {}).status, "FAIL")
        self.assertEqual(cc("cost", False, ["--vsync", "0", "--render-fps", "600"], {}).status,
                         "FAIL")
        self.assertEqual(cc("cost", False, ["--render-fps", "9999"],
                            {"OPENPETE_PRESENT_MODE": "immediate"}).status, "PASS")
        self.assertEqual(cc("cost", True, ["--headless"], {}).status, "PASS")
        self.assertEqual(cc("cost", False, ["--headless", "--vsync", "0",
                                            "--render-fps", "9999"], {}).status, "FAIL")

    def test_cadence(self):
        cc = preflight.check_class
        self.assertEqual(cc("cadence", False, ["--vsync", "1"], {}, 480).status, "PASS")
        self.assertEqual(cc("cadence", False, [], {}, 480).status, "PASS")
        self.assertEqual(cc("cadence", False, ["--vsync", "0"], {}, 480).status, "FAIL")
        self.assertEqual(cc("cadence", False, ["--render-fps", "240"], {}, 480).status, "FAIL")
        self.assertEqual(cc("cadence", False, ["--render-fps", "480"], {}, 480).status, "PASS")
        self.assertEqual(cc("cadence", False, [], {"OPENPETE_PRESENT_MODE": "mailbox"},
                            480).status, "FAIL")

    def test_boot_and_display_need(self):
        self.assertEqual(preflight.check_class("boot", False, ["--headless"], {}).status, "PASS")
        self.assertEqual(preflight.display_need("cadence", False), "held")
        self.assertEqual(preflight.display_need("cost", False), "awake")
        self.assertIsNone(preflight.display_need("cost", True))
        self.assertIsNone(preflight.display_need("boot", False))

    def test_pixels(self):
        cp = preflight.check_pixels
        self.assertEqual(cp("cost", False, ["--window", "1280x720"], None).status, "PASS")
        self.assertEqual(cp("cost", False, ["--ssaa", "2"], None).status, "FAIL")
        self.assertEqual(cp("cost", False, ["--resolution", "1920x1080"], None).status, "FAIL")
        self.assertEqual(cp("cost", False, ["--ssaa", "2"], "2560x1440").status, "PASS")
        self.assertEqual(cp("cost", False, [], "big").status, "FAIL")
        self.assertEqual(cp("cadence", False, ["--ssaa", "4"], None).status, "INFO")


class NeighbourTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.exe = os.path.join(self.d, "openpete.exe")
        open(self.exe, "wb").close()
        os.makedirs(os.path.join(self.d, "assets", "music"))

    def tearDown(self):
        shutil.rmtree(self.d)

    def _st(self, args=(), staged=(), env=None):
        return preflight.check_neighbours(self.exe, list(args), env or {}, set(staged))

    def test_clean_folder(self):
        c = self._st()
        self.assertEqual(c.status, "PASS")
        self.assertIn("assets/: music", c.reading)

    def test_texture_pack(self):
        os.makedirs(os.path.join(self.d, "assets", "textures"))
        self.assertEqual(self._st().status, "PASS", "an empty textures dir is no pack")
        open(os.path.join(self.d, "assets", "textures", "a.png"), "wb").close()
        self.assertEqual(self._st().status, "FAIL")
        self.assertEqual(self._st(staged=["texpack"]).status, "PASS")
        self.assertEqual(self._st(["--texpack-dir", "/x"]).status, "FAIL")

    def test_config_and_mods(self):
        _put(os.path.join(self.d, "openpete.toml"), "fullscreen = true\n")
        self.assertEqual(self._st().status, "FAIL")
        self.assertEqual(self._st(["--no-config"]).status, "PASS")
        self.assertEqual(self._st(staged=["config"]).status, "PASS")
        os.makedirs(os.path.join(self.d, "mods"))
        _put(os.path.join(self.d, "mods", "enabled.txt"), "# none\n")
        self.assertEqual(self._st(["--no-config"]).status, "PASS")
        _put(os.path.join(self.d, "mods", "enabled.txt"), "crt-beans\n")
        self.assertEqual(self._st(["--no-config"]).status, "FAIL")
        self.assertEqual(self._st(["--no-config", "--mods", ""]).status, "PASS")
        self.assertEqual(self._st(["--no-config"], staged=["mods"]).status, "PASS")

    def test_declared_but_absent_and_retired(self):
        self.assertEqual(self._st(staged=["texpack"]).status, "FAIL")
        open(os.path.join(self.d, "openpete.opts"), "w").close()
        c = self._st()
        self.assertEqual(c.status, "PASS")
        self.assertIn("openpete.opts present, not read", c.reading)


RR = ("[render-res] why=live value=window B=2560x1440 T={t} V=0,0,2560x1440 C={c} wf=1.2444 "
      "wf'=1.2444 crop=0.000 filter=x:copy,y:copy scene_rect=0,0,1,1 scene_grid=512,240,224,1 "
      "composes=0 base_composes=0 rq_passes=0\n")


def _pace(path, env, rate, seconds=12.0, not_live_s=1.0):
    """A pace log presenting `rate` per second for `seconds`, the first
    `not_live_s` flagged not-live."""
    import pacelog
    n = int(rate * seconds)
    recs = [(pacelog.PRESENT, 64 if i / rate < not_live_s else 0, 0, i, int(i * 1e9 / rate))
            for i in range(n)]
    pacelog.write(path, env, recs)


class PostTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.log = os.path.join(self.d, "leg.log")
        self.pace = os.path.join(self.d, "leg.oppl")
        sys.path.insert(0, os.path.join(preflight.ROOT, "scripts", "pacing"))

    def tearDown(self):
        shutil.rmtree(self.d)

    def _post(self, decl, rate, env=None, t="2560x1440", c="2560x1440", expect=None,
              static=False):
        with open(self.log, "w") as f:
            f.write("[rhi/vk] swapchain: 3 image(s), acquire budget 4.0 ms (refresh 479.990 Hz)\n")
            f.write(RR.format(t=t, c=c))
        e = {"present_mode": "fifo", "refresh_mhz": "479990", "fullscreen": "0",
             "render_fps": "480"}
        e.update(env or {})
        _pace(self.pace, e, rate)
        return {k.name: k for k in preflight.post_checks(self.log, decl, expect, self.pace, static)}

    def test_main_exe_log_window_and_pixels(self):
        """The main exe's own lines: [render-res] C is the window, T the
        render target; no `present mode:` or `(swapchain WxH)` line needed."""
        cs = self._post(preflight.Decl("cadence"), 480, expect="2560x1440")
        self.assertEqual(cs["window-actual"].status, "PASS")
        self.assertEqual(cs["present-mode-actual"].status, "PASS")
        cs = self._post(preflight.Decl("cadence"), 480, c="1280x720", expect="2560x1440")
        self.assertEqual(cs["window-actual"].status, "FAIL")
        cs = self._post(preflight.Decl("cadence", pixels="5120x2880"), 480, t="5120x2880")
        self.assertEqual(cs["pixels-actual"].status, "PASS")
        cs = self._post(preflight.Decl("cadence", pixels="2560x1440"), 480, t="5120x2880")
        self.assertEqual(cs["pixels-actual"].status, "FAIL")

    def test_cadence_static_rate(self):
        ok = self._post(preflight.Decl("cadence"), 479.99, static=True)["steady-rate"]
        self.assertEqual(ok.status, "PASS", ok.reading)
        low = self._post(preflight.Decl("cadence"), 458, static=True)["steady-rate"]
        self.assertEqual(low.status, "FAIL")
        self.assertIn("458.0 presents/s", low.reading)
        self.assertNotIn("steady-rate", self._post(preflight.Decl("cadence"), 458))
        mode = self._post(preflight.Decl("cadence"), 480, env={"present_mode": "immediate"})
        self.assertEqual(mode["present-mode-actual"].status, "FAIL")

    def test_cost_post(self):
        imm = {"present_mode": "immediate", "render_fps": "10000"}
        cs = self._post(preflight.Decl("cost"), 1500, env=imm)
        self.assertEqual(cs["present-mode-actual"].status, "PASS")
        self.assertEqual(cs["cap-headroom"].status, "PASS")
        self.assertEqual(cs["pixels-actual"].status, "PASS")
        capped = self._post(preflight.Decl("cost"), 1500, env={"present_mode": "immediate",
                                                               "render_fps": "1600"})
        self.assertEqual(capped["cap-headroom"].status, "FAIL")
        ss = self._post(preflight.Decl("cost"), 1500, env=imm, t="5120x2880")
        self.assertEqual(ss["pixels-actual"].status, "FAIL")
        fifo = self._post(preflight.Decl("cost"), 480)
        self.assertEqual(fifo["present-mode-actual"].status, "FAIL")

    def test_missing_pace_log(self):
        with open(self.log, "w") as f:
            f.write(RR.format(t="640x480", c="640x480"))
        cs = {c.name: c for c in preflight.post_checks(self.log, preflight.Decl("cadence"))}
        self.assertTrue(cs["pace-log"].blocks())
        cs = {c.name: c for c in preflight.post_checks(self.log, preflight.Decl("boot"))}
        self.assertFalse(cs["pace-log"].blocks())

    def test_windows_pace_path(self):
        self.assertEqual(preflight.linux_path("C:\\Users\\a\\x.oppl"), "/mnt/c/Users/a/x.oppl")
        self.assertEqual(preflight.linux_path("/tmp/x"), "/tmp/x")


if __name__ == "__main__":
    unittest.main(verbosity=2)
