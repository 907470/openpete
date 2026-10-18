#!/usr/bin/env python3
"""Tests for tests/harness/docs-shot-pairs.py.

The pair check runs over the Spyro 1 corpus manifest's docs rows and a
scratch capture directory of synthetic tiles: every tile distinct passes; an
/after tile identical to its /before tile fails naming the pair; an absent
tile fails; a manifest with no docs rows exits 65; a malformed tag, a page
or heading the site does not have, an unpaired side and unequal pins each
fail. The capture harness drops docs: tokens from a row's launch field.

Run: python3 scripts/fences/tests/test_docs_shot_pairs.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SCRIPT = os.path.join(ROOT, "tests", "harness", "docs-shot-pairs.py")


def run(*args):
    """Run the checker; returns (exit code, stdout)."""
    cp = subprocess.run([sys.executable, SCRIPT, *args], capture_output=True,
                        text=True, cwd=ROOT)
    return cp.returncode, cp.stdout + cp.stderr


def docs_tiles(manifest=None):
    """Every docs tile the manifest names, from the checker's list."""
    extra = ["--manifest", manifest] if manifest else []
    rc, out = run("list", *extra)
    assert rc == 0, out
    return sorted({line.split("\t")[2] for line in out.splitlines() if "\t" in line})


class PairTest(unittest.TestCase):
    """The pair check over synthetic capture directories."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="docs-shot-pairs-")
        self.tiles = docs_tiles()
        for i, t in enumerate(self.tiles):
            Image.new("RGB", (8, 8), (i, 0, 0)).save(os.path.join(self.dir, t))

    def tearDown(self):
        shutil.rmtree(self.dir)

    def manifest(self, text):
        """Write a fixture manifest in the scratch directory."""
        p = os.path.join(self.dir, "fixture.manifest")
        with open(p, "w") as fh:
            fh.write(text)
        return p

    def test_distinct_passes(self):
        rc, out = run("check", "--dir", self.dir)
        self.assertEqual(rc, 0, out)
        self.assertIn("DIFFER", out)
        self.assertNotIn("FAIL", out)

    def test_identical_pair_fails(self):
        shutil.copy(os.path.join(self.dir, "docs-look__canon__1900-vk.png"),
                    os.path.join(self.dir, "docs-look@dither__canon__1900-vk.png"))
        rc, out = run("check", "--dir", self.dir)
        self.assertEqual(rc, 1, out)
        self.assertIn("graphics/retro-look#dither: IDENTICAL", out)

    def test_missing_tile_fails(self):
        os.remove(os.path.join(self.dir, "docs-look@affine__canon__1900-vk.png"))
        rc, out = run("check", "--dir", self.dir)
        self.assertEqual(rc, 1, out)
        self.assertIn("MISSING docs-look@affine__canon__1900-vk.png", out)

    def test_subset_skips_uncaptured_pair(self):
        empty = tempfile.mkdtemp(prefix="docs-shot-pairs-empty-")
        try:
            rc, out = run("check", "--dir", empty, "--expect-subset")
        finally:
            shutil.rmtree(empty)
        self.assertEqual(rc, 0, out)
        self.assertIn("SKIP", out)

    def test_no_docs_rows(self):
        m = self.manifest("plain|1700|100||1760|--skip-to-level artisans\n")
        rc, out = run("check", "--manifest", m, "--dir", self.dir)
        self.assertEqual(rc, 65, out)

    def test_bad_tags_fail(self):
        cases = {
            "malformed": "a|1700|100||1760|docs:Graphics/Retro\n",
            "no page": "a|1700|100||1760|docs:graphics/nope#wobble\n",
            "no heading": "a|1700|100||1760|docs:graphics/retro-look#nope\n",
            "unpaired": "a|1700|100||1760|docs:graphics/retro-look#wobble/before\n",
            "no pin": "a|1700|100|||docs:graphics/retro-look#wobble\n",
            "pins differ": "a|1700|100||1760|docs:graphics/retro-look#wobble/before\n"
                           "b|1700|100||1800|docs:graphics/retro-look#wobble/after\n",
        }
        for what, text in cases.items():
            rc, out = run("check", "--manifest", self.manifest(text), "--dir", self.dir)
            self.assertEqual(rc, 1, "%s: %s" % (what, out))


class LaunchTest(unittest.TestCase):
    """pg_split_launch passes no docs: token to a run."""

    def test_docs_token_dropped(self):
        script = ('source scripts/presentation-game.sh; '
                  'pg_split_launch "--dither 1 docs:graphics/retro-look#dither/after K=V"; '
                  'echo "args=${ROW_ARGS[*]} env=${ROW_ENV[*]}"')
        cp = subprocess.run(["bash", "-c", script], capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(cp.stdout.strip(), "args=--dither 1 env=K=V", cp.stderr)


if __name__ == "__main__":
    unittest.main()
