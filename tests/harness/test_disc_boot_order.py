#!/usr/bin/env python3
"""test_disc_boot_order.py - unit rows for tools/disc_boot_order.py.

Parser: a well-formed profile parses with its range order intact and its
generated C table lists the ranges in that order; each malformed profile
(not TOML, wrong format, wrong sector size, missing game or window, empty
ranges, a range of the wrong shape, a negative or boolean field, a zero
count, overlapping ranges, VBLs out of order, a VBL past the window) is
refused with ProfileError, and `header` exits 1 on it.

Generator: the same trace gives the same profile text twice; a trace whose
host-clock column is rewritten gives the same text (only VBL stamps
enter); host-tagged, cache-load, VBL 0 and post-window rows contribute no
range; an already-listed sector is not listed again and a range that
continues the previous one merges into it.

Simulation: on a synthetic trace the profile order serves a read the file
order misses, and a profile range loads before file order reaches it.

Run: python3 tests/harness/test_disc_boot_order.py (exit 0 = every case
passes).
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "disc_boot_order.py"
_spec = importlib.util.spec_from_file_location("disc_boot_order", TOOL)
dbo = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dbo)

GOOD = """format = 1
game = "t"
sector_bytes = 2352
window_end_vbl = 100
ranges = [
  [500, 4, 3],
  [20, 2, 3],
  [9000, 8, 50],
]
"""

HDR = "#ns\tvbl\ttag\tlba\tcount\tresult\toffset\tlength\n"


def row(ns, vbl, tag, lba, count, result="hit"):
    """One trace line."""
    return f"{ns}\t{vbl}\t{tag}\t{lba}\t{count}\t{result}\t{lba * 2352}\t{count * 2352}\n"


def write(path, text):
    """Write text to path and return the path."""
    Path(path).write_text(text, encoding="utf-8")
    return path


class Parser(unittest.TestCase):
    """parse_profile and header."""

    def test_order_preserved(self):
        game, end, ranges = dbo.parse_profile(GOOD)
        self.assertEqual((game, end), ("t", 100))
        self.assertEqual(ranges, [(500, 4, 3), (20, 2, 3), (9000, 8, 50)])
        h = dbo.format_header(game, ranges)
        self.assertLess(h.index("{ 500u, 4u }"), h.index("{ 20u, 2u }"))
        self.assertLess(h.index("{ 20u, 2u }"), h.index("{ 9000u, 8u }"))
        self.assertIn("OPENPETE_DISC_BOOT_ORDER_COUNT 3u", h)

    def test_malformed_refused(self):
        bad = {
            "not toml": "ranges = [[1, 2, 3]",
            "format": GOOD.replace("format = 1", "format = 2"),
            "sector": GOOD.replace("sector_bytes = 2352", "sector_bytes = 2048"),
            "no game": GOOD.replace('game = "t"\n', ""),
            "no window": GOOD.replace("window_end_vbl = 100\n", ""),
            "empty": GOOD.split("ranges")[0] + "ranges = []\n",
            "shape": GOOD.replace("[20, 2, 3]", "[20, 2]"),
            "negative": GOOD.replace("[20, 2, 3]", "[-20, 2, 3]"),
            "bool": GOOD.replace("[20, 2, 3]", "[20, true, 3]"),
            "string": GOOD.replace("[20, 2, 3]", '[20, "2", 3]'),
            "zero count": GOOD.replace("[20, 2, 3]", "[20, 0, 3]"),
            "overlap": GOOD.replace("[20, 2, 3]", "[502, 2, 3]"),
            "vbl order": GOOD.replace("[20, 2, 3]", "[20, 2, 2]"),
            "past window": GOOD.replace("[9000, 8, 50]", "[9000, 8, 101]"),
        }
        for name, text in bad.items():
            with self.subTest(name):
                with self.assertRaises(dbo.ProfileError):
                    dbo.parse_profile(text)

    def test_header_cli_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            m = write(os.path.join(d, "p.toml"), GOOD.replace("[20, 2, 3]", "[20, 0, 3]"))
            out = os.path.join(d, "p.h")
            r = subprocess.run([sys.executable, str(TOOL), "header", "--manifest", m, "--out", out],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertFalse(os.path.exists(out))
            write(m, GOOD)
            r = subprocess.run([sys.executable, str(TOOL), "header", "--manifest", m, "--out", out],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(os.path.exists(out))


TRACE = (HDR
         + row(5, 0, "exe", 24, 10, "miss")          # VBL 0: left out
         + row(9, 3, "libcd", 500, 1)
         + row(10, 3, "libcd", 570, 37)
         + row(11, 3, "libcd", 501, 2)               # continues nothing listed before it
         + row(12, 15, "libcd", 570, 40)             # 570..606 listed; 607..609 new
         + row(13, 20, "woah", 260000, 100)          # host: left out
         + row(14, 21, "xa", 90000, 8)
         + row(15, 22, "xa", 90008, 8)               # continues the previous range
         + row(16, 30, "cache-load", 0, 3566, "direct")
         + row(17, 200, "libcd", 70000, 5))          # past the window


class Generator(unittest.TestCase):
    """generate and format_profile."""

    def ranges(self, text, end=100):
        with tempfile.TemporaryDirectory() as d:
            rows = dbo.read_trace(write(os.path.join(d, "t.tsv"), text))
        return dbo.format_profile("t", end, dbo.generate(rows, end))

    def test_deterministic(self):
        self.assertEqual(self.ranges(TRACE), self.ranges(TRACE))

    def test_host_clock_ignored(self):
        shifted = "".join(
            line if line.startswith("#") else str(int(line.split("\t", 1)[0]) * 7 + 3) + "\t" +
            line.split("\t", 1)[1] for line in TRACE.splitlines(keepends=True))
        self.assertEqual(self.ranges(TRACE), self.ranges(shifted))

    def test_rows(self):
        _, _, r = dbo.parse_profile(self.ranges(TRACE))
        self.assertEqual(r, [(500, 1, 3), (570, 37, 3), (501, 2, 3), (607, 3, 15),
                             (90000, 16, 21)])

    def test_cli_twice_identical(self):
        with tempfile.TemporaryDirectory() as d:
            t = write(os.path.join(d, "t.tsv"), TRACE)
            outs = []
            for k in range(2):
                o = os.path.join(d, f"p{k}.toml")
                subprocess.run([sys.executable, str(TOOL), "generate", "--trace", t, "--game", "t",
                                "--end-vbl", "100", "--out", o], check=True)
                outs.append(Path(o).read_bytes())
            self.assertEqual(outs[0], outs[1])


class Simulation(unittest.TestCase):
    """simulate on a synthetic image of 20 chunks."""

    def test_profile_serves_what_file_order_misses(self):
        nsec = 20 * dbo.CHUNK_SECTORS
        far = 15 * dbo.CHUNK_SECTORS + 10
        text = (HDR + row(1, 6, "libcd", 5, 4) + row(2, 30, "libcd", far, 8)
                + row(3, 40, "cache-load", nsec - 1, 1, "direct"))
        with tempfile.TemporaryDirectory() as d:
            rows = dbo.read_trace(write(os.path.join(d, "t.tsv"), text))
        prof = dbo.generate(rows, 60)
        ranges = [(a, b, c) for a, b, c, _ in prof]
        self.assertEqual(ranges, [(5, 4, 6), (far, 8, 30)])
        f = dbo.simulate(rows, nsec, ranges, 60, 25, "file")
        p = dbo.simulate(rows, nsec, ranges, 60, 25, "profile")
        self.assertEqual(f["game_miss_rows"], 2)        # chunk 0 at 0.1 s, chunk 15 at 0.5 s
        self.assertEqual(p["game_miss_rows"], 0)
        self.assertGreater(f["game_direct_bytes"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
