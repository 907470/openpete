#!/usr/bin/env python3
"""Tests for the provenance fence (provenance_census.py).

Covers the token rule (a whole-word internal name is refused, a longer
identifier containing one is not), the form rule (a program's name with an
upstream source path, a line-numbered file or a qualified name is refused;
the name alone, as a tool, is not), an allow-list row, the marker escape and
its required reason, and both modes end to end in a scratch repository: a
planted token in a committed file fails `tree` and a staged one fails
`hook` (the positive controls), and the same repository without it passes
(the negative control).

Run: python3 scripts/fences/tests/test_provenance_census.py
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import os
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import provenance_census as pc  # noqa: E402

PLANT = "/* steps like UVStepper */\n"


def kind(line, rel="core/x.c"):
    """The scan verdict of one line."""
    return pc.scan_line(rel, line)[0]


class LineTest(unittest.TestCase):
    def test_token_refused(self):
        self.assertEqual(kind("/* as in AnalogController */"), "refused")
        self.assertEqual(kind("see r3000a.h"), "refused")

    def test_token_whole_word_only(self):
        self.assertIsNone(kind("my_UVStepper_like = 1;"))
        self.assertIsNone(kind("static int ds_line_count;"))

    def test_form_refused(self):
        self.assertEqual(kind("# DuckStation src/core/gpu.h has it"), "refused")
        self.assertEqual(kind("pcsx-redux foo.cpp:120 does this"), "refused")
        self.assertEqual(kind("DuckStation's Foo::Bar computes it"), "refused")

    def test_tool_use_passes(self):
        self.assertIsNone(kind("# Runs duckstation-regtest with -psxtrace PATH"))
        self.assertIsNone(kind("scripts/pcsx-redux-ram-dump.lua writes the dump"))
        self.assertIsNone(kind("third_party/psxrecomp/src/code_generator.cpp:1973"))

    def test_allow_row(self):
        line = "PCSX.Events.createEventListener('GPU::Vsync', function()"
        self.assertEqual(kind(line, "scripts/x.lua"), "allowed")
        self.assertEqual(kind(line, "core/x.c"), "refused")

    def test_marker(self):
        self.assertEqual(kind("UVStepper  /* provenance-census: quoting the audit's term */"), "marked")
        self.assertEqual(kind("UVStepper  /* provenance-census: */"), "refused")

    def test_scope(self):
        self.assertFalse(pc.in_scope("third_party/duckstation-patches/0001-x.patch"))
        self.assertFalse(pc.in_scope("games/spyro-1/spyro-1/src/main.c"))
        self.assertTrue(pc.in_scope("third_party/psxrecomp/runner/src/runtime.c"))
        self.assertTrue(pc.in_scope("docs/src/developer/x.md"))


def git(root, *args):
    """Run git in a scratch repository."""
    subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t",
                    "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false"] + list(args),
                   check=True, capture_output=True)


def fence(root, mode):
    """Exit code of the fence script over root."""
    return subprocess.run([sys.executable, os.path.join(FENCES, "provenance_census.py"), mode, root],
                          capture_output=True).returncode


class RepoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        git(self.root, "init", "-q")
        with open(os.path.join(self.root, "clean.c"), "w") as fh:
            fh.write("/* captured with duckstation-regtest -psxtrace */\nint x;\n")
        git(self.root, "add", "clean.c")
        git(self.root, "commit", "-q", "-m", "init")

    def tearDown(self):
        self.tmp.cleanup()

    def test_tree_negative_then_positive(self):
        self.assertEqual(fence(self.root, "tree"), 0)
        with open(os.path.join(self.root, "planted.c"), "w") as fh:
            fh.write(PLANT)
        git(self.root, "add", "planted.c")
        self.assertEqual(fence(self.root, "tree"), 66)

    def test_hook_negative_then_positive(self):
        with open(os.path.join(self.root, "clean.c"), "a") as fh:
            fh.write("int y;\n")
        git(self.root, "add", "clean.c")
        self.assertEqual(fence(self.root, "hook"), 0)
        with open(os.path.join(self.root, "clean.c"), "a") as fh:
            fh.write(PLANT)
        git(self.root, "add", "clean.c")
        self.assertEqual(fence(self.root, "hook"), 66)

    def test_hook_reads_only_added_lines(self):
        with open(os.path.join(self.root, "old.c"), "w") as fh:
            fh.write(PLANT)
        git(self.root, "add", "old.c")
        git(self.root, "commit", "-q", "-m", "old", "--no-verify")
        with open(os.path.join(self.root, "old.c"), "a") as fh:
            fh.write("int z;\n")
        git(self.root, "add", "old.c")
        self.assertEqual(fence(self.root, "hook"), 0)


if __name__ == "__main__":
    unittest.main()
