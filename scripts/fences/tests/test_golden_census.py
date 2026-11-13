#!/usr/bin/env python3
"""Tests for scripts/golden-census.py, the census store of scripts/golden-fleet.py
and scripts/fences/tile_census_accessor.py.

Registry: every counter name the sidecar writer can produce resolves, every
class bit names a `bit` row. Store: a unit's .part is finalized only after
the run's done marker (moved.txt from the accepted attempt's stream lines,
entry.json last), a third entry prunes the oldest, an unfinished entry over
24 h is removed, the start-of-run clean leaves the store, a filtered run, a
stopped unit or a sidecar of another tree makes no entry. Counts source:
a filtered run, an unfinished run and a newer entry whose head is not a
first-parent ancestor of HEAD are passed over for an older ancestor entry;
an empty store prints the no-counts line, `query` exits 1 and `expect
--query` refuses. Predicates: backdrop and own, legs and launch, a counter
newer than a sidecar is unknown. Query files: fixing, the stamp keeping the
hash, `<sha>.tiles` with unknown rows, the `missing:` refusal. Score: both
sides by moved row, `unknown` rows outside both recalls, narrow, launch and
upper-bound pred lines on both sides under --window, a file edited after it
was fixed refused, a hand-only file `engagement: none measured`. The
accessor fence passes the tree and refuses a call outside the two capture
helpers, a read of the variable outside its module and a missing helper.

Run: python3 scripts/fences/tests/test_golden_census.py
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, rel):
    """Import a script of the tree as a module."""
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


gc = load("golden_census", "scripts/golden-census.py")
gf = load("golden_fleet", "scripts/golden-fleet.py")
fence = load("tile_census_accessor", "scripts/fences/tile_census_accessor.py")
FP = gc.first_parent_commits()
HEAD, PARENT = FP[0], FP[1]
OTHER = "0123456789abcdef0123456789abcdef01234567"   # in no history
TREE = "a" * 40
UNIT = "spyro-1-presentation"
CANON = "row1__canon__100-vk.png"
IDENT = "row1__ident__100-vk.png"
INTER = "row1__inter__150-vk.png"
ROWS = {"row1": "OPENPETE_TERRAIN_CACHE_FORCE=1", "row2": "", "row3": "", "newrow": ""}


def write(path, text):
    """Write text to path, creating its directory."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def sidecar(gen, tree, vals):
    """Sidecar text."""
    return "# gen %d tree %s\n" % (gen, tree) + "".join("%s\t%d\n" % (k, vals[k]) for k in sorted(vals))


TILES = {CANON: {"moby.tris": 10, "flag.shell.tris": 4, "moby.type.254": 10},
         IDENT: {"moby.tris": 10, "flag.shell.tris": 4},
         INTER: {"moby.tris": 9, "backdrop.terrain.tris": 4},
         "row2__canon__200-vk.png": {"terrain.tris": 50, "flag.kfold.tris": 3},
         "row3__canon__300-vk.png": {"terrain.tris": 20}}


def make_run(fleet, rid, head, args=None, done=True, tiles=TILES, tree=TREE, gen=2, attempt=1,
             stream=""):
    """A local run directory and its unit's .part as the coordinator leaves them; returns the run dir."""
    run_dir = os.path.join(fleet, "runs", rid)
    os.makedirs(run_dir)
    write(os.path.join(run_dir, "run-args.json"), json.dumps(dict({"head": head}, **(args or {}))))
    write(os.path.join(run_dir, "stream.log"), stream)
    part = os.path.join(fleet, "census", UNIT, rid + ".part")
    for t, vals in tiles.items():
        write(os.path.join(part, t[:-4] + ".census"), sidecar(gen, tree, vals))
    write(os.path.join(part, gc.DIGESTS_NAME),
          "# tile\tdigest16\n" + "".join("%s\t%016x\n" % (t, i) for i, t in enumerate(sorted(tiles))))
    write(os.path.join(part, "accept.json"), json.dumps({"run_id": rid, "unit": UNIT, "attempt": attempt,
                                                         "head": head, "source_tree": tree, "rc": 0,
                                                         "compared": len(tiles), "sidecars": len(tiles)}))
    if done:
        write(os.path.join(run_dir, "done"), "rc=0\n")
    return run_dir


@contextlib.contextmanager
def quiet():
    """Capture stdout; yields the buffer."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        yield buf


class Base(unittest.TestCase):
    """A scratch fleet root and the synthetic manifest rows."""

    def setUp(self):
        """Make the scratch root; point the manifest reader at ROWS."""
        self.tmp = tempfile.mkdtemp(prefix="golden-census-test-")
        self.fleet = os.path.join(self.tmp, "fleet")
        self.store = os.path.join(self.fleet, "census")
        self._rows = gc.manifest_rows
        gc.manifest_rows = lambda game, tier="presentation": dict(ROWS) if game == "spyro-1" else {}

    def tearDown(self):
        """Restore the reader; remove the scratch root."""
        gc.manifest_rows = self._rows
        shutil.rmtree(self.tmp, ignore_errors=True)

    def finalized(self, rid, head, **kw):
        """A run whose entry is finalized; returns its run dir."""
        rd = make_run(self.fleet, rid, head, **kw)
        with quiet():
            gf.census_store_finalize(rid, rd)
        return rd


class Registry(unittest.TestCase):
    """Every name the sidecar writer produces resolves; every class bit names a bit row."""

    reg = gc.load_registry()

    def test_writer_names_resolve(self):
        """core/diag/tile_census.c's names, own and under backdrop."""
        names = ["%s.tris" % p for p in self.reg.paths]
        names += ["%s.tris.k%d" % (f, k) for f in self.reg.families for k in range(8)]
        names += ["cat.%d.tris" % c for c in range(16)] + ["blend.%d.tris" % b for b in range(8)]
        names += ["flag.%s.tris" % f for f in ("shell", "key_order", "kfold", "portal_rim",
                                               "host_added", "fog_delta")]
        names += ["flag.%s.prims" % f for f in ("over_fade", "under_player", "carry")]
        names += ["moby.type.0", "moby.type.65535", "tag.unmapped.tris"]
        names += ["terrain.sec.retained" + s for s in ("", "_hp", "_lp", "_near")]
        names += list(self.reg.bits.values())
        for n in names:
            self.assertIsNotNone(self.reg.resolve(n), n)
            self.assertIsNotNone(self.reg.resolve("backdrop." + n), n)
        for n in ("compose.vram", "backdrop.unknown"):
            self.assertIsNotNone(self.reg.resolve(n), n)
        self.assertIsNone(self.reg.resolve("backdrop.compose.vram"))


class Store(Base):
    """The fleet's census store: finalize, prune, refusals."""

    def test_finalize(self):
        """After the done marker a .part becomes <run-id>/ with moved.txt from its attempt's stream lines and entry.json."""
        stream = ("%s local 1 1.0 DIVERGE %s x\n%s local 2 2.0 DIVERGE %s x\n"
                  "%s local 2 2.0 SAME %s x\n" % (UNIT, INTER, UNIT, CANON, UNIT, INTER))
        rd = self.finalized("20261101-000000-aaaa", HEAD, attempt=2, stream=stream)
        e = os.path.join(self.store, UNIT, "20261101-000000-aaaa")
        self.assertTrue(os.path.isfile(os.path.join(e, "entry.json")))
        self.assertFalse(os.path.exists(e + ".part"))
        with open(os.path.join(e, "moved.txt")) as f:
            self.assertEqual(f.read(), CANON + "\n")
        for n in ("run-args.json", "done"):
            self.assertTrue(os.path.isfile(os.path.join(e, n)), n)
        self.assertEqual(json.load(open(os.path.join(e, "entry.json")))["head"], HEAD)
        self.assertTrue(os.path.isdir(rd))

    def test_killed_before_done(self):
        """A run with no done marker finalizes nothing; the reader ignores the .part."""
        rd = make_run(self.fleet, "20261101-000000-aaaa", HEAD, done=False)
        with quiet():
            self.assertEqual(gf.census_store_finalize("20261101-000000-aaaa", rd), [])
        self.assertTrue(os.path.isdir(os.path.join(self.store, UNIT, "20261101-000000-aaaa.part")))
        src, line = gc.counts_source(self.store, UNIT)
        self.assertIsNone(src)
        self.assertIn("no completed unfiltered full run", line)

    def test_prune(self):
        """A third entry prunes the oldest; an unfinished entry over 24 h is removed, a younger one kept."""
        for i in range(3):
            self.finalized("2026110%d-000000-aaaa" % (i + 1), HEAD)
        names = sorted(os.listdir(os.path.join(self.store, UNIT)))
        self.assertEqual(names, ["20261102-000000-aaaa", "20261103-000000-aaaa"])
        make_run(self.fleet, "20261104-000000-aaaa", HEAD, done=False)
        make_run(self.fleet, "20261105-000000-aaaa", HEAD, done=False)
        old = os.path.join(self.store, UNIT, "20261104-000000-aaaa.part")
        t = time.time() - 25 * 3600
        os.utime(old, (t, t))
        with quiet():
            gf.census_store_prune(os.path.join(self.store, UNIT), time.time())
        self.assertFalse(os.path.exists(old))
        self.assertTrue(os.path.isdir(os.path.join(self.store, UNIT, "20261105-000000-aaaa.part")))

    def test_clean_leaves_store(self):
        """The start-of-run clean 25 h later removes the run directory and leaves the store's entry."""
        rd = self.finalized("20261101-000000-aaaa", HEAD)
        t = time.time() - 25 * 3600
        os.utime(os.path.join(rd, "done"), (t, t))
        p = subprocess.run(["bash", "-c", gf.CLEAN_SCRIPT, "clean", self.fleet, "no-such-engine",
                            "stale", "1", "none"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(os.path.exists(rd), p.stdout)
        self.assertTrue(os.path.isfile(os.path.join(self.store, UNIT, "20261101-000000-aaaa", "entry.json")))

    def test_tree_refusal(self):
        """A sidecar of `tree unrecorded` or of another tree than the capture record's refuses the entry."""
        local = os.path.join(self.tmp, "pull")
        write(os.path.join(local, "capture-record.txt"), "source-tree=%s\n" % TREE)
        write(os.path.join(local, "a__canon__1-vk.census"), sidecar(2, TREE, {}))
        self.assertIsNone(gf.census_tree_refusal(local))
        for bad in ("unrecorded", "b" * 40):
            write(os.path.join(local, "b__canon__1-vk.census"), sidecar(2, bad, {}))
            self.assertIn("names tree " + bad, gf.census_tree_refusal(local) or "")

    def test_part_refusals(self):
        """A filtered run, a stopped unit and a foreign tree make no .part; an accepted full unit does."""
        local = os.path.join(self.tmp, "pull")
        write(os.path.join(local, "capture-record.txt"), "source-tree=%s\nhead=%s\n" % (TREE, HEAD))
        write(os.path.join(local, "verdict-report.txt"), "compared: 1\n")
        write(os.path.join(local, "a__canon__1-vk.census"), sidecar(2, TREE, {}))
        write(os.path.join(local, gc.DIGESTS_NAME), "a__canon__1-vk.png\t0000000000000000\n")
        run_dir = os.path.join(self.fleet, "runs", "r1")
        os.makedirs(run_dir)

        class Job:
            name, tier, stopped, result = UNIT, "presentation", False, {"attempt": 1, "rc": 0}

        class Co:
            local_run_dir, run_id = run_dir, "r1"
            flt, rep = False, 0

            def filtered(self):
                return self.flt

            def repeat(self):
                return self.rep

        part = os.path.join(self.store, UNIT, "r1.part")
        for setup in ("filter", "args", "stopped", "tree"):
            co, job = Co(), Job()
            write(os.path.join(run_dir, "run-args.json"), json.dumps({"rows": "x"} if setup == "args" else {}))
            co.flt = setup == "filter"
            job.stopped = setup == "stopped"
            if setup == "tree":
                write(os.path.join(local, "a__canon__1-vk.census"), sidecar(2, "unrecorded", {}))
            with quiet():
                gf.Coordinator.census_store_part(co, job, local)
            self.assertFalse(os.path.exists(part), setup)
        write(os.path.join(local, "a__canon__1-vk.census"), sidecar(2, TREE, {}))
        with quiet():
            gf.Coordinator.census_store_part(Co(), Job(), local)
        self.assertTrue(os.path.isfile(os.path.join(part, "a__canon__1-vk.census")))


class CountsSource(Base):
    """The reader's choice among a unit's entries."""

    def test_choice(self):
        """Filtered, unfinished and newer non-ancestor entries are passed over for the older ancestor."""
        self.finalized("20261101-000000-aaaa", PARENT)                       # older ancestor
        self.finalized("20261103-000000-aaaa", OTHER)                        # newer non-ancestor
        make_run(self.fleet, "20261104-000000-aaaa", HEAD, done=False)       # unfinished
        # A filtered run's entry (the fleet makes none; the reader refuses one anyway).
        ud = os.path.join(self.store, UNIT)
        shutil.copytree(os.path.join(ud, "20261103-000000-aaaa"), os.path.join(ud, "20261102-000000-aaaa"))
        write(os.path.join(ud, "20261102-000000-aaaa", "run-args.json"), json.dumps({"filter": "x"}))
        e = json.load(open(os.path.join(ud, "20261102-000000-aaaa", "entry.json")))
        write(os.path.join(ud, "20261102-000000-aaaa", "entry.json"), json.dumps(dict(e, head=HEAD)))
        self.assertEqual(gc.entry_refusal(os.path.join(ud, "20261102-000000-aaaa")), "filtered run (filter)")
        src, line = gc.counts_source(self.store, UNIT)
        self.assertEqual(src.run_id, "20261101-000000-aaaa", line)
        self.assertIn("HEAD is 1 first-parent commits past it", line)
        self.assertEqual(src.unknown, ["newrow"])

    def test_non_ancestor_warns(self):
        """With no ancestor entry the newest is used and the header says so."""
        self.finalized("20261103-000000-aaaa", OTHER)
        src, line = gc.counts_source(self.store, UNIT)
        self.assertEqual(src.run_id, "20261103-000000-aaaa")
        self.assertIn("is not an ancestor of HEAD; newest entry used", line)

    def test_empty_store(self):
        """No entry: the no-counts line, query exits 1, expect --query refuses."""
        with quiet() as out:
            rc = gc.main(["query", "--game", "spyro-1", "moby.tris>0", "--store", self.store])
        self.assertEqual(rc, 1)
        self.assertIn("no completed unfiltered full run", out.getvalue())
        q = os.path.join(self.tmp, "q", "a.q")
        write(q, "reason\tx\npred\tspyro-1\tflag.shell.tris>0\n")
        with quiet() as out:
            rc = gc.main(["expect", "--query", q, "--store", self.store])
        self.assertEqual(rc, 1)
        self.assertIn("has no counts source", out.getvalue())


class Predicates(Base):
    """Predicate terms over one entry."""

    def setUp(self):
        """One finalized entry at HEAD."""
        super().setUp()
        self.finalized("20261101-000000-aaaa", HEAD)
        self.src = gc.counts_source(self.store, UNIT)[0]
        self.reg = gc.load_registry()

    def holds(self, pred, tile=INTER, gen=None):
        """Whether pred holds on a tile of the entry."""
        g, vals = self.src.tiles[tile]
        return gc.eval_tile(gc.parse_pred(pred, self.reg), tile, g if gen is None else gen, vals,
                            ROWS, self.reg)

    def test_backdrop_and_own(self):
        """terrain.tris matches its backdrop. form unless the predicate says own."""
        self.assertTrue(self.holds("terrain.tris>0")[0])
        self.assertFalse(self.holds("terrain.tris>0 and own")[0])

    def test_legs_and_launch(self):
        """Leg terms and launch terms."""
        self.assertTrue(self.holds("moby.tris>0 and inter and orig and launch:CACHE_FORCE")[0])
        self.assertFalse(self.holds("moby.tris>0 and canon")[0])
        self.assertFalse(self.holds("launch:NOSUCH")[0])

    def test_stale_counter_unknown(self):
        """A counter newer than the sidecar's generation is unknown and engaged."""
        ok, _v, unknown = self.holds("flag.shell.tris>100", gen=0)
        self.assertTrue(ok and unknown)

    def test_unregistered_refused(self):
        """A term naming no registered counter is refused."""
        with self.assertRaises(gc.Refused):
            gc.parse_pred("moby.bogus>0", self.reg)


class QueryFiles(Base):
    """fix, expect --query."""

    def setUp(self):
        """One finalized entry at HEAD and a queries directory."""
        super().setUp()
        self.finalized("20261101-000000-aaaa", HEAD)
        self.qd = os.path.join(self.tmp, "queries")

    def test_fix_and_stamp(self):
        """fix records the hash; the stamp keeps it; .tiles holds the selection and the unknown row."""
        q = os.path.join(self.qd, "agent.q")
        write(q, "reason\tenv faces\npred\tspyro-1\tflag.shell.tris>0\n"
                 "hand\tspyro-1\trow3__\tmissing:terrain.face.nearbehind\n")
        with quiet():
            self.assertEqual(gc.main(["fix", q]), 0)
        h = gc.body_hash(q)
        with quiet() as out:
            rc = gc.main(["expect", "--query", q, "--sha", HEAD, "--store", self.store,
                          "--append", os.path.join(self.qd, "expect.txt")])
        self.assertEqual(rc, 0, out.getvalue())
        self.assertIn("fixed ", out.getvalue())
        self.assertEqual(gc.body_hash(q), h)
        with open(q) as f:
            self.assertTrue(f.read().startswith("# commit %s\n# counts spyro-1 20261101-000000-aaaa" % HEAD))
        tl = gc.read_tiles_file(os.path.join(self.qd, HEAD + ".tiles"))
        self.assertEqual(tl["pred"][2], {("spyro-1", CANON), ("spyro-1", IDENT)})
        self.assertEqual(tl["unknown"][2], {("spyro-1", "newrow")})
        self.assertIn(gc.body_hash(os.path.join(self.qd, HEAD + ".tiles")), gc.read_fixed(self.qd))
        with open(os.path.join(self.qd, "expect.txt")) as f:
            lines = f.read().splitlines()
        self.assertIn("%s %s env faces" % (UNIT, IDENT), lines)
        self.assertIn("%s newrow__ env faces" % UNIT, lines)
        self.assertIn("%s row3__ env faces" % UNIT, lines)

    def test_missing_refused(self):
        """A hand line without missing: is refused."""
        q = os.path.join(self.qd, "b.q")
        write(q, "reason\tx\nhand\tspyro-1\trow3__\n")
        with quiet() as out:
            self.assertEqual(gc.main(["expect", "--query", q, "--store", self.store]), 1)
        self.assertIn("missing:", out.getvalue())

    def test_hand_only(self):
        """A file with no pred line: engagement none measured."""
        q = os.path.join(self.qd, "c.q")
        write(q, "reason\tx\nhand\tspyro-1\trow3__\tmissing:none\n")
        with quiet() as out:
            self.assertEqual(gc.main(["expect", "--query", q, "--sha", OTHER, "--store", self.store]), 0)
        self.assertIn("engagement: none measured", out.getvalue())
        self.assertIn("post-feedback", out.getvalue())


class Score(Base):
    """score --run on a synthetic batch with known moved rows."""

    def run_batch(self, files, moved, window=True, edit=None):
        """A run directory with query files fixed and expanded, a stream, and run-args.json; returns score's output."""
        self.finalized("20261101-000000-aaaa", HEAD)
        qd = os.path.join(self.tmp, "batch", "queries")
        for sha, (qtext, hand) in files.items():
            write(os.path.join(qd, sha + ".q"), qtext)
            with quiet():
                gc.main(["fix", os.path.join(qd, sha + ".q")])
                if hand is not None:
                    write(os.path.join(qd, sha + ".hand"), hand)
                    gc.main(["fix", os.path.join(qd, sha + ".hand")])
                self.assertEqual(gc.main(["expect", "--query", os.path.join(qd, sha + ".q"), "--sha", sha,
                                          "--store", self.store]), 0)
        if edit:
            with open(os.path.join(qd, edit), "a") as f:
                f.write("hand\tspyro-1\trow2__\tmissing:none\n")
        run = os.path.join(self.tmp, "fleet", "runs", "scored")
        shutil.copytree(qd, os.path.join(run, "queries"))
        tiles = list(TILES) + ["newrow__canon__5-vk.png"]
        write(os.path.join(run, "stream.log"), "".join(
            "%s local 1 1.0 %s %s 0000\n" % (UNIT, "DIVERGE" if t in moved else "SAME", t) for t in tiles))
        write(os.path.join(run, "run-args.json"), json.dumps(
            {"queries": gf.query_hashes(os.path.join(run, "queries"))}))
        argv = ["score", "--run", run, "--store", self.store] + (["--window"] if window else [])
        with quiet() as out:
            rc = gc.main(argv)
        return rc, out.getvalue()

    def test_both_sides(self):
        """pred holds row1, the hand list row2; row3 is held by both through a hand line; newrow is unknown."""
        files = {"1" * 40: ("reason\tenv\npred\tspyro-1\tmoby.tris>0 and canon\n"
                            "hand\tspyro-1\trow3__\tmissing:none\n",
                            "%s row2__ by hand\n%s row3__ by hand\n" % (UNIT, UNIT))}
        moved = {CANON, INTER, "row2__canon__200-vk.png", "row3__canon__300-vk.png", "newrow__canon__5-vk.png"}
        rc, out = self.run_batch(files, moved, window=False)
        self.assertEqual(rc, 0, out)
        self.assertIn("moved rows 4, moved tiles 5, streamed rows 4", out)
        self.assertIn("query side: held 2 of 3 moved rows (Rq 0.667)", out)
        self.assertIn("hand side: held 2 of 3 moved rows (Rh 0.667)", out)
        self.assertIn("unknown rows (excluded from both recalls): 1, 1 moved", out)
        self.assertIn("miss spyro-1 row2__canon__200-vk.png [uncounted]", out)
        self.assertIn("miss spyro-1 %s [leg]" % INTER, out)

    def test_window_pred_as_hand(self):
        """Under --window a pred line selecting 2 rows or fewer, a launch: term or an upper bound scores on both sides."""
        for pred in ("moby.tris>0", "moby.tris>0 and launch:CACHE", "terrain.tris<100"):
            self.tearDown()
            self.setUp()
            files = {"2" * 40: ("reason\tx\npred\tspyro-1\t%s\n" % pred, "%s nothing__ x\n" % UNIT)}
            rc, out = self.run_batch(files, {CANON, "row2__canon__200-vk.png"})
            self.assertEqual(rc, 0, out)
            self.assertIn("pred lines scored as hand lines (--window): 2", out, pred)
            q = [ln for ln in out.splitlines() if "query side:" in ln][0]
            h = [ln for ln in out.splitlines() if "hand side: held" in ln and "Rh" in ln][0]
            self.assertEqual(q.split("(Rq ")[1][:5], h.split("(Rh ")[1][:5], pred)

    def test_shared_and_equal_sides(self):
        """Two files holding one moved row each record it shared; hand list = hand lines and an empty pred give Rq = Rh."""
        files = {"5" * 40: ("reason\ta\npred\tspyro-1\tmoby.tris>0\n", None),
                 "6" * 40: ("reason\tb\npred\tspyro-1\tmoby.tris>5\n", None)}
        rc, out = self.run_batch(files, {CANON}, window=False)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out.count("sole 0, shared 1"), 2, out)
        self.tearDown()
        self.setUp()
        files = {"7" * 40: ("reason\tc\npred\tspyro-1\tmoby.tris>1000\nhand\tspyro-1\trow2__\tmissing:none\n",
                            "%s row2__ x\n" % UNIT)}
        rc, out = self.run_batch(files, {CANON, "row2__canon__200-vk.png"}, window=False)
        self.assertIn("(Rq 0.500)", out)
        self.assertIn("(Rh 0.500)", out)

    def test_edited_after_fixing(self):
        """A file whose body changed after its fixed.tsv line is refused."""
        files = {"3" * 40: ("reason\tx\npred\tspyro-1\tmoby.tris>0\n", None)}
        rc, out = self.run_batch(files, {CANON}, edit="3" * 40 + ".q")
        self.assertEqual(rc, 1)
        self.assertIn("edited after it was fixed", out)

    def test_hand_only(self):
        """A hand-only file prints engagement: none measured; a file holding nothing scores recall 0."""
        files = {"4" * 40: ("reason\tx\nhand\tspyro-1\tnothing__\tmissing:none\n", None)}
        rc, out = self.run_batch(files, {CANON}, window=False)
        self.assertEqual(rc, 0, out)
        self.assertIn("engagement: none measured", out)
        self.assertIn("(Rq 0.000)", out)


class AccessorFence(unittest.TestCase):
    """scripts/fences/tile_census_accessor.py."""

    def test_tree_passes(self):
        """The tree as it stands."""
        self.assertEqual(fence.main(["x", "tree", ROOT]), 0)

    def test_refusals(self):
        """A call outside the helpers, a read of the variable elsewhere, a missing helper."""
        with open(os.path.join(ROOT, fence.HELPER_FILE)) as f:
            text = f.read()
        seeded = text.replace("    census_submit_note(scene, scene_n);\n",
                              "    if (tile_census_on()) census_submit_note(scene, scene_n);\n", 1)
        self.assertNotEqual(seeded, text)
        self.assertEqual(len(fence.scan(fence.HELPER_FILE, seeded)), 1)
        other = 'int f(void) { return getenv("OPENPETE_TILE_CENSUS") != 0; } /* tile_census_on() */'
        self.assertEqual(len(fence.scan("games/common/renderer/x.c", other)), 1)
        with self.assertRaises(LookupError):
            fence.scan(fence.HELPER_FILE, text.replace("census_shot_write(const char", "w(const char"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
