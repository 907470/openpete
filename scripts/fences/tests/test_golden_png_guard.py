#!/usr/bin/env python3
"""Tests for the golden-image fence (golden-png-guard.py).

RuleTest drives the path predicate. HookTest builds a scratch repository armed with the tracked dispatcher
(githooks/pre-commit, core.hooksPath=githooks) and the real fence scripts,
and commits each staged shape: a PNG, a BMP, a nested PNG, a tile under a
renamed golden directory (`git mv`, staged as R), the allowed .tsv/.md and an
exception-set PNG, a commit staging no image while tracked tiles sit in the
index, the declared bypass admitting exactly one commit, and an unfunded
complexity rise refused through the dispatcher.

Run: python3 scripts/fences/tests/test_golden_png_guard.py
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)


def load(name, fname):
    """Import a fence script whose file name is not a module name."""
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(FENCES, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gpg = load("golden_png_guard", "golden-png-guard.py")

TINY_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d000000"
    "0049454e44ae426082")


class RuleTest(unittest.TestCase):
    def test_refused(self):
        for p in ("tests/golden/spyro-1/presentation/a-vk.png",
                  "tests/golden/spyro-1/presentation/a.bmp",
                  "tests/golden/spyro-1/presentation/sub/x.png",
                  "tests/golden/spyro-1/presentation-old/x.png",
                  "tests/golden/spyro-1/native-vk-old/x.png",
                  "tests/golden/spyro-1/psycross-ref/a.png",
                  "tests/golden/spyro-1/native-vk/x.bmp",
                  "tests/golden/spyro-1/native-vk/demo-100-vk.png",
                  "tests/golden/spyro-1/psxtrace-demo-20k.bin",
                  "./tests/golden/spyro-2/presentation/x.png",
                  "/abs/openpete/tests/golden/spyro-3/presentation/x.png"):
            self.assertTrue(gpg.refused(p), p)

    def test_allowed(self):
        for p in ("tests/golden/spyro-1/presentation/blessed.tsv",
                  "tests/golden/spyro-1/presentation/ident-baseline.tsv",
                  "tests/golden/spyro-1/native-vk/README.md",
                  "tests/golden/spyro-1/dragons/blessed.tsv",
                  "docs/phase1/x.png", "games/spyro-1/a.png"):
            self.assertFalse(gpg.refused(p), p)



def nest(name, depth, flat=0):
    """A documented C function scoring depth*(depth+1)/2 + flat."""
    out = ["/** @brief Fixture. @param a input. @return a or zero. */",
           "int %s(int a) {" % name, "    int r = 0;"]
    for d in range(depth):
        out.append("    " * (d + 1) + "if (a > %d) {" % d)
    out.append("    " * (depth + 1) + "r = a;")
    for d in reversed(range(depth)):
        out.append("    " * (d + 1) + "}")
    out += ["    if (a == %d) r += %d;" % (100 + f, f) for f in range(flat)]
    out += ["    return r;", "}", ""]
    return "\n".join(out)


class HookTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="golden-png-guard-")
        self.repo = os.path.join(self.tmp, "repo")
        # The real fences, linked and left untracked: the dispatcher resolves
        # them under the scratch top level, and the ratchets' own-tree checks
        # (keyed on the script's real location) stay out of the scratch repo.
        os.makedirs(os.path.join(self.repo, "scripts"))
        os.symlink(FENCES, os.path.join(self.repo, "scripts", "fences"))
        os.makedirs(os.path.join(self.repo, "githooks"))
        shutil.copy2(os.path.join(ROOT, "githooks", "pre-commit"),
                     os.path.join(self.repo, "githooks", "pre-commit"))
        os.makedirs(os.path.join(self.repo, "core"))
        self.cdb = os.path.join(self.tmp, "compile_commands.json")
        with open(self.cdb, "w") as fh:
            json.dump([{"directory": self.repo, "file": "core/tail.c",
                        "command": "cc -std=c11 -c core/tail.c -o tail.o"}], fh)
        self.env = dict(os.environ, OPENPETE_COMPLEXITY_DB=self.cdb,
                        OPENPETE_DOCS_DB=self.cdb,
                        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        for k in ("OPENPETE_COMPLEXITY_ACCEPT", "OPENPETE_DOCS_ACCEPT",
                  gpg.ENV, "GIT_INDEX_FILE"):
            self.env.pop(k, None)
        self.git("init", "-q", "-b", "main")
        self.write("core/tail.c", nest("tail", 8, 4))
        # Tracked tiles committed before the hook is armed, as on the real
        # tree through the interim: they stay in the index.
        for i in range(3):
            self.write("tests/golden/spyro-1/presentation/t%d-vk.png" % i,
                       TINY_PNG)
        self.write("tests/golden/spyro-1/native-vk/demo-1-vk.png", TINY_PNG)
        self.git("add", "githooks", "core", "tests")
        self.git("commit", "-q", "-m", "init")
        for fence in ("complexity_ratchet.py", "doc_ratchet.py"):
            p = subprocess.run([sys.executable, os.path.join(FENCES, fence),
                                "baseline"],
                cwd=self.repo, capture_output=True, text=True, env=self.env)
            self.assertEqual(p.returncode, 0, fence + p.stdout + p.stderr)
        self.git("add", "baselines")
        self.git("commit", "-q", "-m", "baselines")
        self.git("config", "core.hooksPath", "githooks")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args, ok=True, env=None):
        p = subprocess.run(["git", "-C", self.repo] + list(args),
                           capture_output=True, text=True, env=env or self.env)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def write(self, rel, data):
        ap = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(ap), exist_ok=True)
        with open(ap, "wb" if isinstance(data, bytes) else "w") as fh:
            fh.write(data)

    def commit(self, msg, ok, env=None):
        p = self.git("commit", "-q", "-m", msg, ok=False, env=env)
        self.assertEqual(p.returncode == 0, ok, p.stdout + p.stderr)
        return p

    def stage_refused(self, rel, data=TINY_PNG):
        self.write(rel, data)
        self.git("add", "-f", rel)
        p = self.commit("x", ok=False)
        self.assertIn("golden-png-guard: REFUSED", p.stderr)
        self.assertIn(rel, p.stderr)
        self.git("reset", "-q")

    def test_png_bmp_nested_refused(self):
        self.stage_refused("tests/golden/spyro-1/presentation/new-vk.png")
        self.stage_refused("tests/golden/spyro-1/presentation/new.bmp")
        self.stage_refused("tests/golden/spyro-2/presentation/sub/x.png")

    def test_modified_tracked_tile_refused(self):
        self.stage_refused("tests/golden/spyro-1/presentation/t0-vk.png",
                           TINY_PNG + b"\0")

    def test_renamed_golden_directory_refused(self):
        self.git("mv", "tests/golden/spyro-1/presentation",
                 "tests/golden/spyro-1/presentation-old")
        st = self.git("diff", "--cached", "--name-status").stdout
        self.assertIn("R100", st)
        p = self.commit("mv", ok=False)
        self.assertIn("presentation-old/t0-vk.png", p.stderr)

    def test_manifests_and_readme_pass(self):
        self.write("tests/golden/spyro-1/presentation/blessed.tsv", "x\n")
        self.write("tests/golden/spyro-1/presentation/ident-baseline.tsv", "y\n")
        self.write("tests/golden/spyro-1/native-vk/README.md", "z\n")
        self.git("add", "tests")
        self.commit("allowed", ok=True)

    def test_no_exception_set(self):
        rel = "tests/golden/spyro-1/native-vk/demo-2-vk.png"
        self.write(rel, TINY_PNG)
        self.git("add", "-f", rel)
        p = self.commit("native-vk tile", ok=False)
        self.assertIn(rel, p.stderr)

    def test_commit_without_images_passes_with_tiles_in_index(self):
        n = len(self.git("ls-files", "tests/golden/spyro-1/presentation")
                .stdout.split())
        self.assertEqual(n, 3)
        self.write("README", "r\n")
        self.git("add", "README")
        self.commit("doc", ok=True)

    def test_bypass_admits_exactly_one_commit(self):
        rel = "tests/golden/spyro-1/presentation/new-vk.png"
        self.write(rel, TINY_PNG)
        self.git("add", "-f", rel)
        short = dict(self.env, **{gpg.ENV: "1"})
        self.commit("x", ok=False, env=short)
        reason = "fixture: deliberate tile for the fence test"
        env = dict(self.env, **{gpg.ENV: reason})
        self.commit("bypass", ok=True, env=env)
        log = os.path.join(self.repo, ".git", gpg.LOG)
        with open(log) as fh:
            lines = fh.read().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn(reason, lines[0])
        self.assertIn(rel, lines[0])
        rel2 = "tests/golden/spyro-1/presentation/new2-vk.png"
        self.write(rel2, TINY_PNG + b"\0")
        self.git("add", "-f", rel2)
        p = self.commit("again", ok=False, env=env)
        self.assertIn("admits one commit", p.stderr)

    def test_complexity_rise_refused_through_dispatcher(self):
        self.write("core/tail.c", nest("tail", 8, 5))
        self.git("add", "core/tail.c")
        p = self.commit("rise", ok=False)
        self.assertIn("REFUSED  core/tail.c:tail  40 -> 41", p.stderr)
        self.assertNotIn("golden-png-guard: REFUSED", p.stderr)
        self.commit("rise", ok=True, env=dict(
            self.env, OPENPETE_COMPLEXITY_ACCEPT="fixture"))


if __name__ == "__main__":
    unittest.main()
