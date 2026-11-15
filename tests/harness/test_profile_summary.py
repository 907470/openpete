#!/usr/bin/env python3
"""test_profile_summary.py - unit rows for scripts/profile-summary.py's sim-tick split.

Region trace: a synthetic format 3 trace whose sim worker writes, per
canonical inline present, a sim_tick region (guest) holding a terrain hook
(with a guest renderer call and a face walk that waits on the worker pool), a
moby hook running the snap resolve, and a PsyQ thunk, plus a face walk rooted
outside the tick. The reader returns the region fields as packed; the split
gives guest = game logic + renderers, host by subsystem with the pool wait
marked and the snap resolve in its own branch naming the hook it ran in,
psyq on its own line, and leaves summing to sim_tick; the region rooted
outside the tick is not counted; the text names the three sections.

Scenes: an intermediate present per tick carrying an in-between scene's
production root and render root (and their regions, flagged intermediate)
gives one tree each per scene, summing to the root, with no guest, the pool
wait marked and the sub-tick snap resolve in its own branch; the tick split
does not count them; a trace with no scene root has no scene split.

Earlier trace: the same spans with no region fields (as written before the
region tree) read, and the split is the inclusive hook table with its
"unattributed" remainder.

Usage: tests/harness/test_profile_summary.py
"""
import importlib.util
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "..", "scripts")
sys.path.insert(0, SCRIPTS)
import _opp_reader as rd  # noqa: E402

_spec = importlib.util.spec_from_file_location("profile_summary", os.path.join(SCRIPTS, "profile-summary.py"))
ps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ps)

GUEST, WAIT, INTER = rd.REGION_GUEST, rd.REGION_WAIT, rd.REGION_INTERMEDIATE

# (name, parent, root, dur, self, flags) per canonical tick; the self times of
# the regions rooted at sim_tick sum to its 1000 us.
TICK = (
    ("sim_tick", None, "sim_tick", 1000, 200, GUEST),
    ("hook.terrain_native", "sim_tick", "sim_tick", 300, 50, 0),
    ("terrain.srend", "hook.terrain_native", "sim_tick", 100, 100, GUEST),
    ("ter.walk", "hook.terrain_native", "sim_tick", 150, 140, 0),
    ("pool.wait", "ter.walk", "sim_tick", 10, 10, WAIT),
    ("hook.moby_native", "sim_tick", "sim_tick", 400, 100, 0),
    ("mcan.resolve", "hook.moby_native", "sim_tick", 300, 300, 0),
    ("psyq.drawotag", "sim_tick", "sim_tick", 100, 100, 0),
    ("ter.walk", None, "ter.walk", 500, 500, 0),
)

# One in-between present: its scene production (600 us) and its render
# (400 us), every region flagged as under an in-between root.
INTER_SCENE = (
    ("scene", None, "scene", 600, 10, INTER),
    ("scene.rex", "scene", "scene", 500, 100, INTER),
    ("rex.terrain", "scene.rex", "scene", 250, 200, INTER),
    ("pool.wait", "rex.terrain", "scene", 50, 50, WAIT | INTER),
    ("mrex.resolve", "scene.rex", "scene", 150, 150, INTER),
    ("scene.publish", "scene", "scene", 90, 90, INTER),
    ("render", None, "render", 400, 50, INTER),
    ("cmp.key", "render", "render", 350, 350, INTER),
)


def trace(ticks=4, regions=True, inter=False):
    """A format 3 trace of `ticks` canonical inline presents on thread 0, each preceded by TICK's spans,
    and with `inter` an intermediate inline present after each, preceded by INTER_SCENE's spans."""
    names = sorted({n for row in TICK + INTER_SCENE for n in (row[0], row[1], row[2]) if n})
    nid = {n: i for i, n in enumerate(names)}
    out = [b"OPPR" + struct.pack("<IQQ", 3, 1000000, 0) + bytes(8)]
    for n in names:
        b = n.encode()
        out.append(struct.pack("<BHH", 1, nid[n], len(b)) + b)
    t, idx = 1000, 0
    for k in range(ticks):
        for kind, rows in ((rd.KIND_CANONICAL, TICK), (rd.KIND_INTERMEDIATE, INTER_SCENE if inter else ())):
            if kind == rd.KIND_INTERMEDIATE and not inter:
                continue
            for (name, parent, root, dur, self_us, flags) in rows:
                r = bytearray(32)
                r[0], r[1] = 3, 0
                struct.pack_into("<H", r, 2, nid[name])
                struct.pack_into("<QII", r, 8, t, dur, 1)
                if regions:
                    struct.pack_into("<HH", r, 4, 0x8000 | (nid[parent] + 1 if parent else 0), nid[root])
                    struct.pack_into("<I", r, 24, self_us)
                    r[29] = flags
                out.append(bytes(r))
            r = bytearray(32)
            r[0], r[1] = 2, kind
            struct.pack_into("<IIIQ", r, 4, idx, k + 1, len(rows), t + 2000)
            out.append(bytes(r))
            idx += 1
            t += 16500
    return b"".join(out)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, data):
        p = os.path.join(self.tmp.name, "t.opp")
        with open(p, "wb") as f:
            f.write(data)
        return p


class Regions(Base):
    def test_reader(self):
        tr = rd.read_opp(self.write(trace(1)))
        regs = {(r.name, r.root): r for r in tr.markers[0].regions}
        self.assertEqual(len(tr.markers[0].spans), len(TICK))
        r = regs[("pool.wait", "sim_tick")]
        self.assertEqual((r.parent, r.self_us, r.dur_us, r.flags, r.calls), ("ter.walk", 10, 10, WAIT, 1))
        self.assertIsNone(regs[("sim_tick", "sim_tick")].parent)
        self.assertIsNone(regs[("ter.walk", "ter.walk")].parent)

    def test_split(self):
        ss = ps.summarize(self.write(trace(4)))["sim_split"]
        self.assertEqual(ss["format"], "regions")
        self.assertEqual(ss["canonical_ticks"], 4)
        self.assertEqual(ss["sim_tick_us_per_tick"], 1000.0)
        self.assertEqual(ss["leaves_sum_us"], 1000.0)
        self.assertEqual((ss["guest_us"], ss["guest_logic_us"], ss["guest_renderers_us"]), (300.0, 200.0, 100.0))
        self.assertEqual((ss["host_us"], ss["host_wait_us"], ss["psyq_us"]), (600.0, 10.0, 100.0))
        host = {h["subsystem"]: h for h in ss["host"]}
        self.assertEqual({k: h["us"] for k, h in host.items()}, {"terrain": 200.0, "moby": 100.0, "snap": 300.0})
        leaves = {r["scope"]: r for r in host["terrain"]["leaves"]}
        self.assertEqual(leaves["ter.walk"]["us"], 140.0)   # the walk rooted outside the tick is not counted
        self.assertEqual((leaves["pool.wait<ter.walk"]["kind"], leaves["pool.wait<ter.walk"]["us"]), ("wait", 10.0))
        snap = host["snap"]["leaves"][0]
        self.assertEqual((snap["scope"], snap["in"], snap["elsewhere"]), ("mcan.resolve", ["hook.moby_native"], True))
        self.assertEqual([r["scope"] for r in ss["guest"]], ["sim_tick", "terrain.srend"])

    def test_text(self):
        text = ps.render_text(ps.summarize(self.write(trace(4))))
        for want in ("[sim.split]", "guest           300.0   30.0%   game logic 200.0 + original renderers 100.0",
                     "host            600.0   60.0%   on the thread 590.0 + waiting on the worker pool 10.0",
                     "leaves sum 1000.0 (+0.00% of sim_tick)", "[sim.split.guest]", "[sim.split.host]",
                     "(runs in hook.moby_native)", "wait  pool.wait<ter.walk"):
            self.assertIn(want, text)


class Scenes(Base):
    def test_scene_split(self):
        s = ps.summarize(self.write(trace(4, inter=True)))
        self.assertEqual(s["sim_split"]["sim_tick_us_per_tick"], 1000.0)   # the in-between scene is not in it
        sc = s["scene_split"]
        self.assertEqual(sorted(sc), ["render.intermediate", "scene.intermediate"])
        t = sc["scene.intermediate"]
        self.assertEqual((t["instances"], t["root_us"], t["leaves_sum_us"]), (4, 600.0, 600.0))
        self.assertEqual((t["guest_us"], t["host_us"], t["host_wait_us"]), (0.0, 600.0, 50.0))
        host = {h["subsystem"]: h["us"] for h in t["host"]}
        self.assertEqual(host, {"scene": 200.0, "terrain": 250.0, "snap": 150.0})
        r = sc["render.intermediate"]
        self.assertEqual((r["root_us"], {h["subsystem"]: h["us"] for h in r["host"]}),
                         (400.0, {"render": 50.0, "compose": 350.0}))
        text = ps.render_text(s)
        for want in ("[scene.inter]", "per intermediate scene production (4)", "no guest region ran",
                     "[render.inter]", "wait  pool.wait<rex.terrain"):
            self.assertIn(want, text)

    def test_no_scene_roots(self):
        self.assertIsNone(ps.summarize(self.write(trace(4)))["scene_split"])


class Earlier(Base):
    def test_legacy(self):
        p = self.write(trace(4, regions=False))
        self.assertEqual(rd.read_opp(p).markers[0].regions, [])
        ss = ps.summarize(p)["sim_split"]
        self.assertEqual(ss["format"], "legacy")
        rows = {r["hook"]: r for r in ss["rows"]}
        self.assertEqual(rows["terrain_native"]["srend_us"], 100.0)
        self.assertEqual(ss["unattributed_us"], 1000.0 - 300 - 400 - 100)
        self.assertIn("trace without region records", ps.render_text(ps.summarize(p)))


if __name__ == "__main__":
    unittest.main()
