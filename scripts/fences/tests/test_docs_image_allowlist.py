#!/usr/bin/env python3
"""Tests for the package docs image fence (docs_image_allowlist.py).

A scratch docs tree holding exactly the allowlisted images passes; an image
not on the list fails naming it, by extension and, renamed to a text
extension, by its signature; an allowlisted path the tree lacks fails; a
non-image file passes. The checkout's allowlist names only files under
docs/src or the theme's favicon, so every listed User docs image has its
source.

Run: python3 scripts/fences/tests/test_docs_image_allowlist.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
SCRIPT = os.path.join(FENCES, "docs_image_allowlist.py")
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24


def write(path, data):
    """Write bytes to a path, making its folders."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def check(docs, allowlist):
    """Run the fence's check; returns (exit code, output)."""
    cp = subprocess.run([sys.executable, SCRIPT, "check", docs, "--allowlist", allowlist],
                        capture_output=True, text=True)
    return cp.returncode, cp.stdout + cp.stderr


class FenceTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="docs-image-allowlist-")
        self.docs = os.path.join(self.d, "docs")
        self.allow = os.path.join(self.d, "allow.txt")
        write(os.path.join(self.docs, "assets", "images", "favicon.png"), PNG)
        write(os.path.join(self.docs, "user", "a", "shot.png"), PNG)
        write(os.path.join(self.docs, "user", "a", "page.html"), b"<html></html>")
        with open(self.allow, "w") as f:
            f.write("# comment\nassets/images/favicon.png\n./user/a/shot.png  # the launcher\n")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_clean_tree_passes(self):
        rc, out = check(self.docs, self.allow)
        self.assertEqual(rc, 0, out)
        self.assertIn("2 images", out)

    def test_unlisted_image_refused(self):
        write(os.path.join(self.docs, "user", "b", "game.jpg"), b"\xff\xd8\xff\xe0")
        rc, out = check(self.docs, self.allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("image not on the allowlist: user/b/game.jpg", out)

    def test_renamed_image_refused_by_signature(self):
        write(os.path.join(self.docs, "user", "b", "notes.txt"), PNG)
        rc, out = check(self.docs, self.allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("user/b/notes.txt", out)

    def test_webp_signature_refused(self):
        write(os.path.join(self.docs, "user", "b", "x.bin"), b"RIFF\x10\x00\x00\x00WEBPVP8 ")
        rc, out = check(self.docs, self.allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("user/b/x.bin", out)

    def test_missing_listed_image_refused(self):
        os.remove(os.path.join(self.docs, "user", "a", "shot.png"))
        rc, out = check(self.docs, self.allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("allowlisted image missing from the docs: user/a/shot.png", out)

    def test_missing_docs_dir_is_usage(self):
        rc, _ = check(os.path.join(self.d, "absent"), self.allow)
        self.assertEqual(rc, 64)


class RepoTest(unittest.TestCase):
    def test_allowlist_sources_exist(self):
        sys.path.insert(0, FENCES)
        import docs_image_allowlist as fence
        for p in sorted(fence.read_allowlist(fence.ALLOWLIST)):
            if p == "assets/images/favicon.png":
                continue
            src = os.path.join(ROOT, "docs", "src", p)
            self.assertTrue(os.path.isfile(src), f"{p}: no source at {src}")
            self.assertTrue(p.startswith(("user/", "modding/")), f"{p}: the package ships the User and Modding sections")


if __name__ == "__main__":
    unittest.main()
