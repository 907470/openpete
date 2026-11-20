#!/usr/bin/env python3
"""test_entry.py - the level-entry anchor of scripts/pacing/entry.py.

fixtures/s1portal-entry.oppl.gz is a recorded Windows pace log of a Spyro 1
portal entry (town square, vsync on at 480 Hz, a build that drops steady-play
presents), trimmed to the boot's not-live run boundaries, the first present
and every PRESENT and VBL record from 6 s before the level-load tick to 11 s
after it, with the payload slots entry.py does not read zeroed. The portal
keeps presenting through the transition, so the pace log flags no not-live
run there; the anchor is the load tick. The synthetic case checks the flagged
entry (a not-live run after boot).

Run: python3 scripts/pacing/test_entry.py
"""
import gzip
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import entry                                                    # noqa: E402
import pacelog                                                  # noqa: E402

FIXTURE = HERE / "fixtures" / "s1portal-entry.oppl.gz"


class RecordedPortal(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.path = os.path.join(cls.dir, "s1portal-entry.oppl")
        with gzip.open(FIXTURE, "rb") as f, open(cls.path, "wb") as o:
            shutil.copyfileobj(f, o)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def test_anchor_is_the_load_tick(self):
        d = entry.leg(self.path)
        self.assertEqual(d["t0_how"], "load tick")
        # The load tick starts 27.8-27.9 s after the first present; the portal
        # stall's two long intervals (30.6 ms, 26.7 ms) end within 0.1 s of it.
        self.assertGreater(d["t0_s"], 27.8)
        self.assertLess(d["t0_s"], 27.95)
        self.assertAlmostEqual(d["max5"][1], 30.6, delta=0.1)
        self.assertLess(d["max5"][0], 0.1)
        self.assertAlmostEqual(d["rate"], 452.4, delta=0.2)

    def test_present_interval_anchor_misfires_here(self):
        """The log holds a steady-play interval over 12 ms between boot + 3 s
        and the load, so the first-interval-over-12-ms anchor lands before
        the entry; the load tick does not."""
        log = pacelog.read(self.path)
        pres = entry.new_images(log)
        t0, _ = entry.entry_t0(log, pres)
        boot = entry.last_not_live_end(log)
        early = [x for x, y in zip(pres, pres[1:])
                 if x >= boot + 3e9 and y - x > 12e6 and x < t0 - 0.5e9]
        self.assertTrue(early, "fixture lost its pre-entry steady-play miss")

    def test_load_tick_dominates(self):
        """The load tick's exec time is at least twice every other live VBL's
        after boot in the fixture (8.4 ms against at most 4.1 ms)."""
        log = pacelog.read(self.path)
        boot = entry.last_not_live_end(log)
        ex = sorted((r.exec_end_ns - r.exec_start_ns for r in log.of(pacelog.VBL)
                     if not (r.flags & pacelog.F_NOT_LIVE) and r.exec_start_ns >= boot + 3e9),
                    reverse=True)
        self.assertGreaterEqual(ex[0], 2 * ex[1])


class SyntheticFlaggedEntry(unittest.TestCase):
    def test_not_live_run_anchor(self):
        """A not-live run 10 s after the first present anchors t0 at the first
        live VBL after it."""
        vbl_ns = int(1e9 * 37609 / 2250000)
        recs, t = [], 0
        for i in range(60 * 30):
            t = i * vbl_ns
            not_live = i < 30 or 600 <= i < 660
            recs.append((pacelog.VBL, 64 if not_live else 0, 0, i, t, 0, 0, t, t + 100000))
        for j in range(0, 30 * 480):
            ts = j * int(1e9 / 480)
            recs.append((pacelog.PRESENT, 0, 0, j, ts))
        d = tempfile.mkdtemp()
        try:
            p = os.path.join(d, "flag.oppl")
            pacelog.write(p, {"present_mode": "fifo"}, recs)
            r = entry.leg(p)
            self.assertEqual(r["t0_how"], "not-live run end")
            self.assertAlmostEqual(r["t0_s"], 660 * vbl_ns / 1e9, places=3)
            self.assertAlmostEqual(r["rate"], 480.0, delta=0.1)
        finally:
            shutil.rmtree(d)


if __name__ == "__main__":
    unittest.main(verbosity=2)
