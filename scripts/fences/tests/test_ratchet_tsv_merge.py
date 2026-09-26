#!/usr/bin/env python3
"""Tests for the ratchet baseline merge driver (ratchet_tsv_merge.py).

MergeTest drives the pure three-way merge by key. PickTest builds a scratch
repository whose baseline carries a hook-style `# head:` line, lands a
commit on the default branch that retires one row, and cherry-picks a
branch commit (cut before that landing) that retires the adjacent row: with
the driver armed the pick ends with no conflict and neither row present;
with the driver unarmed the same pick conflicts (the control that the
driver is what engaged).

Run: python3 scripts/fences/tests/test_ratchet_tsv_merge.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)
import ratchet_tsv_merge as rm                               # noqa: E402

TSV = "baselines/complexity/cognitive.tsv"


def tsv(head, rows, by="hook"):
    out = ["# cognitive-complexity ratchet baseline",
           "# tool: clang-tidy 14",
           "# written-by: " + by,
           "# head: " + head,
           "file\tfunction\tscore\tmarked\taccepted"]
    out += ["\t".join(r) for r in rows]
    return "\n".join(out) + "\n"


def row(f, fn, score, reason=""):
    return (f, fn, str(score), "0", reason)


BASE_ROWS = [row("core/a.c", "f1", 30), row("core/a.c", "f2", 31),
             row("core/a.c", "f3", 32), row("core/b.c", "g", 40)]


class MergeTest(unittest.TestCase):
    def rows(self, text):
        return rm.split(text)[1]

    def test_deletion_on_either_side_is_kept(self):
        base = tsv("aaa", BASE_ROWS)
        ours = tsv("bbb", [r for r in BASE_ROWS if r[1] != "f1"])
        theirs = tsv("ccc", [r for r in BASE_ROWS if r[1] != "f2"])
        text, conflicts = rm.merge(base, ours, theirs)
        self.assertEqual(conflicts, 0)
        self.assertEqual(sorted(self.rows(text)),
                         ["core/a.c\tf3", "core/b.c\tg"])
        self.assertIn("# head: bbb", text)
        self.assertNotIn("ccc", text)

    def test_one_sided_change_and_addition(self):
        base = tsv("a", BASE_ROWS)
        ours = tsv("b", BASE_ROWS + [row("core/c.c", "h", 26)])
        theirs = tsv("c", [row("core/a.c", "f1", 28)] + BASE_ROWS[1:])
        text, conflicts = rm.merge(base, ours, theirs)
        self.assertEqual(conflicts, 0)
        rows = self.rows(text)
        self.assertEqual(rows["core/a.c\tf1"].split("\t")[2], "28")
        self.assertIn("core/c.c\th", rows)

    def test_both_sides_change_one_row_differently(self):
        base = tsv("a", BASE_ROWS)
        ours = tsv("b", [row("core/a.c", "f1", 29)] + BASE_ROWS[1:])
        theirs = tsv("c", [row("core/a.c", "f1", 28)] + BASE_ROWS[1:])
        text, conflicts = rm.merge(base, ours, theirs)
        self.assertEqual(conflicts, 1)
        self.assertIn("<<<<<<< ours\ncore/a.c\tf1\t29", text)

    def test_delete_against_change_conflicts(self):
        base = tsv("a", BASE_ROWS)
        ours = tsv("b", BASE_ROWS[1:])
        theirs = tsv("c", [row("core/a.c", "f1", 30, "why")] + BASE_ROWS[1:])
        self.assertEqual(rm.merge(base, ours, theirs)[1], 1)

    def test_same_change_on_both_sides(self):
        base = tsv("a", BASE_ROWS)
        side = tsv("b", BASE_ROWS[1:])
        text, conflicts = rm.merge(base, side, tsv("c", BASE_ROWS[1:]))
        self.assertEqual((conflicts, text), (0, side))

    def test_header_other_than_head_and_writer_conflicts(self):
        base = tsv("a", BASE_ROWS)
        other = base.replace("clang-tidy 14", "clang-tidy 15")
        self.assertEqual(rm.merge(base, base, other)[1], 1)

    def test_rows_render_in_fence_order(self):
        # `file:function` order: "a.c:x" sorts after "a.c_y:" is false, the
        # tab-joined key would sort the other way.
        rows = [row("core/a.c", "x", 30), row("core/a.c_y.c", "z", 30)]
        base = tsv("a", [])
        text, _ = rm.merge(base, tsv("b", rows), base)
        lines = [l for l in text.splitlines() if l.startswith("core/")]
        self.assertEqual(lines, ["\t".join(r) for r in
                                 sorted(rows, key=lambda r: r[0] + ":" + r[1])])


class PickTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ratchet-merge-")
        self.repo = self.tmp
        self.env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        self.env.pop("GIT_INDEX_FILE", None)
        # Configuration from the caller's environment would override the
        # scratch repository's own (the merge driver under test).
        for k in [k for k in self.env if k.startswith("GIT_CONFIG_")]:
            del self.env[k]
        self.git("init", "-q", "-b", "main")
        shutil.copy(os.path.join(FENCES, "..", "..", ".gitattributes"),
                    os.path.join(self.repo, ".gitattributes"))
        # The driver runs from the worktree top under the registered command.
        os.makedirs(os.path.join(self.repo, "scripts", "fences"))
        shutil.copy(os.path.join(FENCES, "ratchet_tsv_merge.py"),
                    os.path.join(self.repo, "scripts", "fences"))
        self.git("add", "scripts/fences/ratchet_tsv_merge.py")
        self.write(TSV, tsv("0" * 40, BASE_ROWS))
        self.write("core/a.c", "int f1;\n")
        self.git("add", ".gitattributes", TSV, "core/a.c")
        self.git("commit", "-q", "-m", "base")
        base = self.sha()
        # The branch is cut here; the default branch then lands a commit
        # retiring f1 with a new head line.
        self.git("switch", "-q", "-c", "topic")
        self.write(TSV, tsv(base, [r for r in BASE_ROWS if r[1] != "f2"]))
        self.write("core/a.c", "int f2;\n")
        self.git("add", TSV, "core/a.c")
        self.git("commit", "-q", "-m", "topic retires f2")
        self.git("switch", "-q", "main")
        self.write("core/b.c", "int g;\n")
        self.git("add", "core/b.c")
        self.git("commit", "-q", "-m", "main edits b.c")
        self.main_head = self.sha()
        self.write(TSV, tsv(self.main_head,
                            [r for r in BASE_ROWS if r[1] != "f1"]))
        self.write("core/b.c", "int g2;\n")
        self.git("add", TSV, "core/b.c")
        self.git("commit", "-q", "-m", "main retires f1")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args, ok=True):
        p = subprocess.run(["git", "-C", self.repo] + list(args),
                           capture_output=True, text=True, env=self.env)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def sha(self):
        return self.git("rev-parse", "HEAD").stdout.strip()

    def write(self, rel, text):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)

    def test_pick_across_a_landing_merges_by_key(self):
        self.git("config", "merge.%s.driver" % rm.DRIVER_NAME,
                 rm.DRIVER_CONFIG)
        p = self.git("cherry-pick", "topic", ok=False)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        text = self.git("show", "HEAD:" + TSV).stdout
        keys = sorted(rm.split(text)[1])
        self.assertEqual(keys, ["core/a.c\tf3", "core/b.c\tg"])
        self.assertIn("# head: %s\n" % self.main_head, text)
        self.assertEqual(self.git("status", "--porcelain").stdout.strip(), "")

    def test_unarmed_pick_conflicts(self):
        # The control: the same pick under git's text merge conflicts on the
        # rows, so the armed pass above is the driver's.
        p = self.git("cherry-pick", "topic", ok=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(TSV, self.git("diff", "--name-only",
                                    "--diff-filter=U").stdout)


if __name__ == "__main__":
    unittest.main()
