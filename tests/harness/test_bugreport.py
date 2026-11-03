#!/usr/bin/env python3
"""test_bugreport.py - unit rows for tools/bugreport.py.

Bundle access: a synthetic schema-1 bundle opens, every member verifies, and
`info` prints the manifest's identity, counters, taint, redaction, pace and
log fields and the replay route; `extract` writes every member with its
manifest hash; a member altered after the manifest was written fails `info`
and `extract` with exit 1.

Refusals (exit 64, a `refused:` message, no traceback): a truncated zip, a
non-zip, a `../` member, an absolute member, a backslash member, a missing
manifest, a manifest that does not parse, a duplicate member name.

diff-hashes: equal CSVs report IDENTICAL; a CSV with one region of one row
altered reports DIVERGED at that tick naming that region; a bundle argument
reads its hashes.csv; CSVs with no tick in common report VACUOUS.

Pace: present records 16.67 ms apart with one 50 ms gap give p50 16.67 ms
and one hitch.

Replay: the fallback route runs a stub executable (a script printing the
flag's verdict line, not openpete) and passes its verdict and exit code
through; an archive directory holding a tarball whose `openpete` member has
the manifest's exe SHA-256 selects the primary route.

Run: python3 tests/harness/test_bugreport.py (exit 0 = every case passes).
"""
import hashlib
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "bugreport.py"
_spec = importlib.util.spec_from_file_location("bugreport", TOOL)
br = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(br)

REGIONS = ["player", "mobys", "camera"]


def hash_csv(rows=range(100, 110), alter=None):
    out = ["frame,vbl,fnv1a64," + ",".join(REGIONS)]
    for f in rows:
        vals = [f"{(f * 7 + i) & 0xFFFFFFFF:016x}" for i in range(len(REGIONS))]
        total = f"{f * 13:016x}"
        if alter and alter[0] == f:
            vals[REGIONS.index(alter[1])] = "deadbeefdeadbeef"
            total = "0000000000000001"
        out.append(f"{f},{f * 2},{total}," + ",".join(vals))
    return ("\n".join(out) + "\n").encode()


def pace_ring(intervals_ms):
    env = b"build=test\n"
    hdr = struct.pack("<IIIIIIqqqqq", br.OPPL_MAGIC, 2, 64 + len(env), 64, len(env), 0,
                      0, 0, 2250000, 37609, 2)
    recs, t = [], 10**9
    for i, iv in enumerate([0] + intervals_ms):
        t += int(iv * 1e6)
        recs.append(struct.pack("<BBHIqqqqqqq", br.PACE_PRESENT, 0, 0, i, t, 0, 0, 0, 0, 0, 0))
    return hdr + env + b"".join(recs)


EXE_BYTES = b"#!/bin/sh\necho archived\n"


def members():
    return {
        "hashes.csv": hash_csv(),
        "summary.txt": b"game=spyro-1\nlevel=10 Artisans\nplayer=(1, 2, 3)\n",
        "module.txt": b"linkage=shared\nabi=7\nmodule_march=(unset)\n",
        "log.txt": b"  0.1 [info ] boot: ok\n  0.2 [warn ] cd: slow read\n  0.3 [error] gpu: lost\n",
        "pace.oppl": pace_ring([16.67] * 50 + [50.0] + [16.67] * 50),
        "device.txt": b"[system]\nplatform=Linux\nlogical_cpus=8\n",
        "card1.mcd": b"\0" * 128,
        "mods/m1/mod.toml": b"id='m1'\n",
    }


def manifest(mem, **over):
    m = {
        "schema": 1, "build_version": "v0.5.0-test", "build_tagged": 0, "build_commit": "abcdef123",
        "exe_sha256": hashlib.sha256(EXE_BYTES).hexdigest(), "codegen_stamp": "cg-1",
        "rom_stamp": "rom-1", "overlays": ["a", "b"], "overlays_source": "recomp/manifest",
        "module_sha256": "11" * 32, "module_march": "", "state_ident": "id-1", "game": "spyro-1",
        "disc": "SCUS-94228.cue", "disc_sha256": "22" * 32, "argv": ["openpete", "--headless"],
        "anchor": {"frame": 2100, "vbl": 4200, "feed_key": 2100, "level": 10, "park_ra": "0x1",
                   "park_sp": "0x2", "capture_us": 900},
        "press": {"frame": 4000, "vbl": 8000, "feed_key": 4000, "level": 10, "park_ra": "0x1",
                  "park_sp": "0x2", "capture_us": 800},
        "window_ticks": 1900, "segment_rows": 1900, "hash_rows": 10,
        "taint": {"freecam": 0, "devui": 3}, "mods": [{"id": "m1", "version": "1.0", "mutative": 1}],
        "answers": {"screen": "done"}, "press_us": 4870, "display_us": 12000,
        "redaction": {"replacements": 5, "findings": 2, "list": [
            {"member": "press.state", "section": "arena", "offset": 10, "what": "home"},
            {"member": "press.state", "section": "arena", "offset": 20, "what": "home"}]},
        "members": [{"name": n, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()}
                    for n, d in mem.items()],
    }
    m.update(over)
    return m


def write_bundle(path, mem=None, man=None, extra=(), tamper=None, skip_manifest=False):
    mem = dict(members() if mem is None else mem)
    man = manifest(mem) if man is None else man
    if tamper:
        mem[tamper] = mem[tamper] + b"x"
    with zipfile.ZipFile(path, "w") as z:
        if not skip_manifest:
            z.writestr("manifest.json", json.dumps(man) if isinstance(man, dict) else man)
        for n, d in mem.items():
            z.writestr(n, d)
        for n, d in extra:
            z.writestr(n, d)
    return path


def run(*args):
    r = subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)],
                       capture_output=True, text=True, timeout=60,
                       env=dict(os.environ, OPENPETE_VIBRATION="0"))
    return r.returncode, r.stdout, r.stderr


class Base(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.d = Path(self.td.name)
        self.arch = self.d / "no-archive"

    def tearDown(self):
        self.td.cleanup()


class InfoExtract(Base):
    def test_info_fields(self):
        z = write_bundle(self.d / "b.zip")
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        for s in ["schema 1", "commit abcdef123", "exe_sha256 " + hashlib.sha256(EXE_BYTES).hexdigest(),
                  "codegen_stamp cg-1", "rom_stamp rom-1", "march (unset)", "game spyro-1",
                  "disc SCUS-94228.cue", "m1 1.0 mutative", "press : frame 4000 vbl 8000",
                  "anchor: frame 2100", "anchor age: frame +1900 (31.8 s at the VBL rate), vbl +3800", "taint: 3 total (devui=3)",
                  "state: level=10 Artisans", "redaction: 5 replacement(s), 2 finding(s)",
                  "2 home in press.state [arena]", "hash check all match",
                  "p50 16.67", "max 50.00 ms; hitches 1", "[warn ] cd: slow read",
                  "[error] gpu: lost", "replay route: fallback", "--bug-report-replay"]:
            self.assertIn(s, out)
        self.assertNotIn("[info ] boot", out)

    def test_extract_round_trip(self):
        z = write_bundle(self.d / "b.zip")
        rc, out, _ = run("extract", z, self.d / "x")
        self.assertEqual(rc, 0, out)
        man = json.loads((self.d / "x" / "manifest.json").read_text())
        for m in man["members"]:
            got = (self.d / "x" / m["name"]).read_bytes()
            self.assertEqual(hashlib.sha256(got).hexdigest(), m["sha256"])

    def test_tampered_member(self):
        z = write_bundle(self.d / "b.zip", tamper="summary.txt")
        rc, out, _ = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 1)
        self.assertIn("SIZE summary.txt", out)
        rc, out, _ = run("extract", z, self.d / "x")
        self.assertEqual(rc, 1)
        self.assertIn("FAIL summary.txt", out)


class Refusals(Base):
    def refused(self, z, needle):
        rc, out, err = run("info", z)
        self.assertEqual(rc, 64, out + err)
        self.assertIn("refused:", err)
        self.assertIn(needle, err)
        self.assertNotIn("Traceback", err)

    def test_truncated(self):
        z = write_bundle(self.d / "b.zip")
        data = z.read_bytes()
        z.write_bytes(data[: len(data) // 2])
        self.refused(z, "not a readable zip")

    def test_not_zip(self):
        p = self.d / "b.zip"
        p.write_bytes(b"hello")
        self.refused(p, "not a readable zip")

    def test_dotdot(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("../evil.txt", b"x")]), "'..' component")

    def test_absolute(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("/etc/evil", b"x")]), "absolute name")

    def test_backslash(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("..\\evil", b"x")]), "backslash")

    def test_missing_manifest(self):
        self.refused(write_bundle(self.d / "b.zip", skip_manifest=True), "no manifest.json")

    def test_bad_manifest(self):
        self.refused(write_bundle(self.d / "b.zip", man="{not json"), "does not parse")

    def test_duplicate(self):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            self.refused(write_bundle(self.d / "b.zip", extra=[("summary.txt", b"y")]), "duplicate")

    def test_extract_refuses_dotdot(self):
        z = write_bundle(self.d / "b.zip", extra=[("a/../../evil", b"x")])
        rc, _, err = run("extract", z, self.d / "x")
        self.assertEqual(rc, 64)
        self.assertFalse((self.d / "evil").exists())


class DiffHashes(Base):
    def test_identical(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv())
        b.write_bytes(hash_csv())
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 0)
        self.assertIn("IDENTICAL rows=10", out)

    def test_planted(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv())
        b.write_bytes(hash_csv(alter=(105, "mobys")))
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=105 vbl=210 row=5 regions=mobys total=differs", out)

    def test_bundle_argument_and_subset(self):
        z = write_bundle(self.d / "b.zip")
        b = self.d / "b.csv"
        b.write_bytes(hash_csv(rows=range(103, 120), alter=(108, "camera")))
        rc, out, _ = run("diff-hashes", z, b)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=108", out)
        self.assertIn("regions=camera", out)
        self.assertIn("common 7, only A 3, only B 10", out)

    def test_vacuous(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv(rows=range(0, 5)))
        b.write_bytes(hash_csv(rows=range(5, 10)))
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 3)
        self.assertIn("VACUOUS", out)

    def test_bad_header(self):
        a = self.d / "a.csv"
        a.write_bytes(b"x,y\n1,2\n")
        rc, _, err = run("diff-hashes", a, a)
        self.assertEqual(rc, 64)
        self.assertNotIn("Traceback", err)


class Pace(unittest.TestCase):
    def test_hitch(self):
        ps = br.pace_summary(pace_ring([16.67] * 50 + [50.0] + [16.67] * 50))
        self.assertEqual(ps["presents"], 102)
        self.assertAlmostEqual(ps["p50_ms"], 16.67, places=2)
        self.assertEqual(ps["hitches"], 1)
        self.assertAlmostEqual(ps["max_ms"], 50.0, places=2)

    def test_window(self):
        ps = br.pace_summary(pace_ring([1000.0] * 40))
        self.assertEqual(ps["presents"], 31)

    def test_bad_magic(self):
        self.assertIn("error", br.pace_summary(b"\0" * 80))


class Replay(Base):
    def stub(self, verdict, rc):
        p = self.d / "stub-openpete"
        p.write_text("#!/bin/sh\n"
                     "# Stub: stands in for openpete --bug-report-replay; prints its argv and a verdict.\n"
                     f"echo \"args: $*\"\necho '  1.0 [info ] bug-report-replay: {verdict}'\nexit {rc}\n")
        p.chmod(0o755)
        return p

    def test_fallback_identical(self):
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "SCUS-94228.cue"
        disc.write_text("x")
        rc, out, err = run("replay", z, "--exe", self.stub("IDENTICAL rows=1900", 0), "--disc", disc,
                           "--work", self.d / "w", "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("route: fallback", out)
        self.assertIn("bug-report-replay: IDENTICAL rows=1900", out)
        log = (self.d / "w" / "replay.log").read_text()
        self.assertIn("--bug-report-replay", log)
        self.assertIn("--card1 " + str(self.d / "w" / "cards" / "card1.mcd"), log)
        self.assertIn("--no-card2", log)
        self.assertTrue((self.d / "w" / "cards" / "card1.mcd").is_file())

    def test_fallback_diverged(self):
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "d.cue"
        disc.write_text("x")
        rc, out, _ = run("replay", z, "--exe", self.stub("DIVERGED tick=2500 region=mobys section=ram+0x10 taint=0", 2),
                         "--disc", disc, "--work", self.d / "w", "--archive", self.arch)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=2500 region=mobys", out)

    def test_no_anchor_refused(self):
        mem = members()
        z = write_bundle(self.d / "b.zip", mem=mem, man=manifest(mem, anchor=None))
        rc, out, _ = run("replay", z, "--exe", self.stub("IDENTICAL rows=1", 0), "--archive", self.arch)
        self.assertEqual(rc, 64)
        self.assertIn("no anchor", out)

    def test_primary_route_from_archive(self):
        arch = self.d / "archive" / "v0.5.0-test-abcdef123"
        arch.mkdir(parents=True)
        with tarfile.open(arch / "openpete-v0.5.0-linux-x86_64.tar.xz", "w:xz") as t:
            ti = tarfile.TarInfo("openpete-v0.5.0/openpete")
            ti.size = len(EXE_BYTES)
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(EXE_BYTES))
        z = write_bundle(self.d / "b.zip")
        rc, out, err = run("info", z, "--archive", self.d / "archive")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("replay route: primary", out)
        self.assertIn("(tarball)", out)
        rc, out, _ = run("replay", z, "--archive", self.d / "archive", "--work", self.d / "w", "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn(str(self.d / "w" / "artifact" / "openpete-v0.5.0" / "openpete"), out)


if __name__ == "__main__":
    unittest.main()
