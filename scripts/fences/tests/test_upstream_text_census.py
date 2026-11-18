#!/usr/bin/env python3
"""Tests for the upstream-text fence (upstream_text_census.py).

A scratch upstream repository holds one file; a scratch openpete repository
holds a series file and hooks.txt. A line copied from the upstream file into
the series file fails `tree` and, staged, fails `hook` (the positive
controls); the same series without it passes (the negative control), as do
an `#include` line, an allow-listed line and a short matching line.

Run: python3 scripts/fences/tests/test_upstream_text_census.py
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import os
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import upstream_text_census as ut  # noqa: E402

UPSTREAM_LINE = "const unsigned planted_upstream_value = compute_planted_value(alpha, beta);"
UPSTREAM_FILE = ("#include \"common/types.h\"\n"
                 "{\n"
                 "  " + UPSTREAM_LINE + "\n"
                 "  SPU::GeneratePendingSamples();\n"
                 "  return 0;\n"
                 "}\n")
SERIES = "third_party/duckstation-patches/"


def sh(cwd, *args):
    subprocess.run(["git", "-C", cwd, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgSign=false"]
                   + list(args), check=True, capture_output=True)


class Fence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.up = os.path.join(self.tmp.name, "up")
        self.root = os.path.join(self.tmp.name, "op")
        os.makedirs(self.up)
        os.makedirs(os.path.join(self.root, SERIES, "files/src"))
        sh(self.up, "init", "-q")
        with open(os.path.join(self.up, "a.cpp"), "w") as f:
            f.write(UPSTREAM_FILE)
        sh(self.up, "add", "a.cpp")
        sh(self.up, "commit", "-q", "-m", "up")
        self.pin = subprocess.run(["git", "-C", self.up, "rev-parse", "HEAD"], capture_output=True,
                                  text=True).stdout.strip()
        sh(self.root, "init", "-q")
        self.write("files/src/ours.cpp", "#include \"common/types.h\"\n{\n  SPU::GeneratePendingSamples();\n"
                   "  return 0;\n}\n")
        self.write("hooks.txt", "file src/a.cpp\n@ a.cpp:1 3 3 0000000000000000\n+  ours();\n")
        sh(self.root, "add", SERIES)
        sh(self.root, "commit", "-q", "-m", "series")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, text):
        with open(os.path.join(self.root, SERIES, rel), "w") as f:
            f.write(text)

    def run_fence(self, mode):
        return ut.main(["x", mode, self.root, "--ds", self.up, "--pin", self.pin])

    def test_clean_series_passes(self):
        self.assertEqual(self.run_fence("tree"), 0)

    def test_planted_line_in_file_fails_tree(self):
        self.write("files/src/ours.cpp", "{\n  " + UPSTREAM_LINE + "\n}\n")
        self.assertEqual(self.run_fence("tree"), 66)

    def test_planted_inserted_line_fails_tree(self):
        self.write("hooks.txt", "file src/a.cpp\n@ a.cpp:1 3 3 0000000000000000\n+  " + UPSTREAM_LINE + "\n")
        self.assertEqual(self.run_fence("tree"), 66)

    def test_planted_line_staged_fails_hook(self):
        self.assertEqual(self.run_fence("hook"), 0)
        self.write("files/src/ours.cpp", "{\n  " + UPSTREAM_LINE + "\n}\n")
        sh(self.root, "add", SERIES)
        self.assertEqual(self.run_fence("hook"), 66)

    def test_anchor_lines_of_hooks_txt_are_not_series_text(self):
        self.write("hooks.txt", "# " + UPSTREAM_LINE + "\nfile src/a.cpp\n")
        self.assertEqual(self.run_fence("tree"), 0)

    def test_missing_pin_skips(self):
        self.assertEqual(ut.main(["x", "tree", self.root, "--ds", self.up, "--pin", "0" * 40]), 0)


if __name__ == "__main__":
    unittest.main()
