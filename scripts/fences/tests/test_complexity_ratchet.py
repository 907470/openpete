#!/usr/bin/env python3
"""Tests for the cognitive-complexity ratchet (complexity_ratchet.py).

DecideTest drives the pure decision function per rule. EndToEndTest builds a throwaway git repo with the
hook installed and its own compile database (OPENPETE_COMPLEXITY_DB) and
commits each index shape: M, A, D, identical rename, changed rename, the
temporary-index form, and a stale hook-written baseline on a doc-only commit.

Run: python3 scripts/fences/tests/test_complexity_ratchet.py
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
sys.path.insert(0, FENCES)
import complexity_ratchet as cr                              # noqa: E402

SCRIPT = os.path.join(FENCES, "complexity_ratchet.py")


def nest(name, depth, flat=0, marker=None):
    """A function scoring depth*(depth+1)/2 + flat."""
    out = []
    if marker:
        out.append("// complexity-ratchet: " + marker)
    out += ["int %s(int a) {" % name, "    int r = 0;"]
    for d in range(depth):
        out.append("    " * (d + 1) + "if (a > %d) {" % d)
    out.append("    " * (depth + 1) + "r = a;")
    for d in reversed(range(depth)):
        out.append("    " * (d + 1) + "}")
    out += ["    if (a == %d) r += %d;" % (100 + f, f) for f in range(flat)]
    out += ["    return r;", "}", ""]
    return "\n".join(out)


def decide(before, after, acted, **kw):
    files = {cr.key_file(k) for k in list(before) + list(after)}
    args = dict(renames={}, extents={}, changed_ranges={}, markers={},
                wired_files=files, present_files=files, accept_reason=None)
    args.update(kw)
    return cr.decide({k: (v, "") if isinstance(v, int) else v
                      for k, v in before.items()}, after, acted,
                     args["renames"], args["extents"], args["changed_ranges"],
                     args["markers"], args["wired_files"],
                     args["present_files"], args["accept_reason"],
                     unwired_all=kw.get("unwired_all", False))


class DecideTest(unittest.TestCase):
    def test_rise_refused(self):
        v, _, bins = decide({"a.c:f": 30}, {"a.c:f": 40}, {"a.c"})
        self.assertEqual(v, "REFUSED")
        self.assertEqual(bins["RISE"], [("a.c:f", 30, 40)])

    def test_body_edit_under_cap_passes_with_no_row(self):
        v, rows, _ = decide({}, {"a.c:f": 25}, {"a.c"})
        self.assertEqual((v, rows), ("PASS", {}))

    def test_unfunded_new_refused(self):
        v, _, _ = decide({}, {"a.c:f": 26}, {"a.c"})
        self.assertEqual(v, "REFUSED")

    def test_split_passes_and_writes_rows(self):
        v, rows, _ = decide({"a.c:big": 600},
                            {"a.c:big": 10, "a.c:p1": 250, "b.c:p2": 200,
                             "b.c:p3": 150}, {"a.c", "b.c"})
        self.assertEqual(v, "PASS")
        self.assertEqual(rows, {"a.c:p1": (250, ""), "b.c:p2": (200, ""),
                                "b.c:p3": (150, "")})

    def test_score_accounting_is_not_the_rule(self):
        # Retiring a 30 to 25 frees 5 of excess, not 30 of score.
        v, _, _ = decide({"a.c:f": 30}, {"a.c:f": 25, "a.c:g": 30}, {"a.c"})
        self.assertEqual(v, "PASS")
        v, _, _ = decide({"a.c:f": 30}, {"a.c:f": 25, "a.c:g": 31}, {"a.c"})
        self.assertEqual(v, "REFUSED")

    def test_delete_funds_new_in_one_commit_only(self):
        v, rows, _ = decide({"a.c:f": 600}, {"b.c:g": 600}, {"a.c", "b.c"},
                            present_files={"b.c"}, wired_files={"b.c"})
        self.assertEqual((v, rows), ("PASS", {"b.c:g": (600, "")}))
        v, _, _ = decide({}, {"b.c:g": 600}, {"b.c"})
        self.assertEqual(v, "REFUSED")

    def test_keys_outside_the_acted_on_set_never_refuse(self):
        v, rows, _ = decide({"o.c:f": 30}, {"o.c:f": 90, "a.c:g": 3}, {"a.c"})
        self.assertEqual((v, rows), ("PASS", {"o.c:f": (30, "")}))

    def test_marker_leaves_the_balance_and_records_the_score(self):
        v, rows, bins = decide({}, {"a.c:f": 40}, {"a.c"},
                               markers={"a.c:f": "table"})
        self.assertEqual((v, rows), ("PASS", {"a.c:f": (40, "")}))
        self.assertTrue(bins["MARKED"])
        # A marked reduction funds nothing.
        v, _, _ = decide({"a.c:f": 600}, {"a.c:f": 30, "a.c:g": 40}, {"a.c"},
                         markers={"a.c:f": "table"})
        self.assertEqual(v, "REFUSED")

    def test_inherited_rise_is_not_charged(self):
        kw = dict(extents={"a.c:f": (10, 50)})
        v, rows, bins = decide({"a.c:f": 30}, {"a.c:f": 40}, {"a.c"},
                               changed_ranges={"a.c": [(60, 61)]}, **kw)
        self.assertEqual((v, rows), ("PASS", {"a.c:f": (30, "")}))
        self.assertEqual(bins["INHERITED"], [("a.c:f", 30, 40)])
        v, _, _ = decide({"a.c:f": 30}, {"a.c:f": 40}, {"a.c"},
                         changed_ranges={"a.c": [(50, 50)]}, **kw)
        self.assertEqual(v, "REFUSED")

    def test_identical_rename_carries_rows_unscored(self):
        v, rows, _ = decide({"old/a.c:f": 40}, {}, {"new/a.c"},
                            renames={"old/a.c": ("new/a.c", True)},
                            present_files={"new/a.c"}, wired_files=set())
        self.assertEqual((v, rows), ("PASS", {"new/a.c:f": (40, "")}))

    def test_changed_rename_is_scored_against_the_carried_row(self):
        kw = dict(renames={"old/a.c": ("new/a.c", False)},
                  present_files={"new/a.c"}, wired_files={"new/a.c"})
        v, _, _ = decide({"old/a.c:f": 40}, {"new/a.c:f": 41}, {"new/a.c"}, **kw)
        self.assertEqual(v, "REFUSED")
        v, rows, _ = decide({"old/a.c:f": 40}, {"new/a.c:f": 39}, {"new/a.c"}, **kw)
        self.assertEqual((v, rows), ("PASS", {"new/a.c:f": (39, "")}))

    def test_unwired_row(self):
        kw = dict(present_files={"u.c", "a.c"}, wired_files={"a.c"})
        v, rows, bins = decide({"u.c:f": 40}, {"a.c:g": 1}, {"u.c"}, **kw)
        self.assertEqual(v, "REFUSED")
        self.assertEqual(rows, {"u.c:f": (40, "")})
        v, _, _ = decide({"u.c:f": 40}, {"a.c:g": 1}, {"a.c"}, **kw)
        self.assertEqual(v, "PASS")
        v, _, _ = decide({"u.c:f": 40}, {"a.c:g": 1}, {"a.c"},
                         unwired_all=True, **kw)
        self.assertEqual(v, "REFUSED")

    def test_accept_raises_only_refused_rows(self):
        v, rows, _ = decide({"a.c:f": 30, "a.c:g": 50},
                            {"a.c:f": 40, "a.c:g": 45}, {"a.c"},
                            accept_reason="profiling knob")
        self.assertEqual(v, "PASS")
        self.assertEqual(rows, {"a.c:f": (40, "profiling knob"),
                                "a.c:g": (45, "")})
        # A funded rise passes on the balance and takes no reason.
        v, rows, _ = decide({"a.c:f": 30, "a.c:g": 50},
                            {"a.c:f": 32, "a.c:g": 45}, {"a.c"},
                            accept_reason="unused")
        self.assertEqual(rows["a.c:f"], (32, ""))

    def test_named_accept_leaves_the_unnamed_rise_refused(self):
        v, rows, bins = decide({"a.c:f": 30, "b.c:g": 30},
                               {"a.c:f": 40, "b.c:g": 40}, None,
                               accept_reason={"a.c:f": "why"})
        self.assertEqual(v, "REFUSED")
        self.assertEqual(bins["RISE"], [("b.c:g", 30, 40)])
        self.assertEqual(rows, {"a.c:f": (40, "why"), "b.c:g": (30, "")})

    def test_accepted_reason_survives_a_tightening(self):
        v, rows, _ = decide({"a.c:f": (40, "why")}, {"a.c:f": 38}, {"a.c"})
        self.assertEqual(rows, {"a.c:f": (38, "why")})

    def test_tsv_round_trip(self):
        rows = {"a.c:f": (40, "why"), "b.c:g#2": (26, "")}
        text = cr.render_tsv(rows, {"a.c:f": "m"}, "tighten", "abc")
        head, back = cr.parse_tsv(text)
        self.assertEqual(back, rows)
        self.assertEqual((head["written-by"], head["head"]), ("tighten", "abc"))


@unittest.skipUnless(shutil.which("clang-tidy-%d" % cr.TOOL_MAJOR),
                     "clang-tidy-%d not installed" % cr.TOOL_MAJOR)
class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="complexity-e2e-")
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "core"))
        self.db = os.path.join(self.tmp, "compile_commands.json")
        self.env = dict(os.environ, OPENPETE_COMPLEXITY_DB=self.db,
                        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        self.env.pop("OPENPETE_COMPLEXITY_ACCEPT", None)
        self.env.pop("GIT_INDEX_FILE", None)
        self.git("init", "-q", "-b", "main")
        for name, fn in (("tail", "tail"), ("body", None), ("del", "gone"),
                         ("mv", "moved"), ("mvc", "edited")):
            self.write("core/%s.c" % name,
                       nest(fn, 8, 4) if fn else nest("small", 2))
        self.write("README", "x\n")
        self.wire("tail", "body", "del", "mv", "mvc")
        self.git("add", "core", "README")
        self.git("commit", "-q", "-m", "init")
        # The baseline lands before the hook exists, as on the real tree.
        self.run_fence("baseline")
        hook = os.path.join(self.repo, ".git", "hooks", "pre-commit")
        with open(hook, "w") as fh:
            fh.write("#!/bin/sh\nexec python3 \"%s\" hook\n" % SCRIPT)
        os.chmod(hook, 0o755)
        self.git("add", cr.TSV)
        self.commit("baseline")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args, ok=True, env=None):
        p = subprocess.run(["git", "-C", self.repo] + list(args),
                           capture_output=True, text=True, env=env or self.env)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def commit(self, msg, ok=True, env=None, extra=()):
        p = self.git("commit", "-q", "-m", msg, *extra, ok=False, env=env)
        self.assertEqual(p.returncode == 0, ok, p.stderr)
        return p

    def run_fence(self, *args):
        p = subprocess.run([sys.executable, SCRIPT] + list(args), cwd=self.repo,
                           capture_output=True, text=True, env=self.env)
        self.assertEqual(p.returncode, 0, p.stderr)

    def write(self, rel, text):
        with open(os.path.join(self.repo, rel), "w") as fh:
            fh.write(text)

    def wire(self, *names):
        with open(self.db, "w") as fh:
            json.dump([{"directory": self.repo, "file": "core/%s.c" % n,
                        "command": "cc -std=c11 -c core/%s.c -o %s.o" % (n, n)}
                       for n in names], fh)

    def rows(self):
        return cr.parse_tsv(self.git("show", "HEAD:" + cr.TSV).stdout)[1]

    def test_baseline_rows(self):
        self.assertEqual(self.rows(), {
            "core/tail.c:tail": (40, ""), "core/del.c:gone": (40, ""),
            "core/mv.c:moved": (40, ""), "core/mvc.c:edited": (40, "")})

    def test_modify_rise_refused_then_accepted(self):
        self.write("core/tail.c", nest("tail", 8, 5))
        self.git("add", "core/tail.c")
        p = self.commit("rise", ok=False)
        self.assertIn("REFUSED  core/tail.c:tail  40 -> 41", p.stderr)
        self.assertIn("increments on changed lines (1 of", p.stderr)
        self.commit("rise", env=dict(self.env, OPENPETE_COMPLEXITY_ACCEPT="1"),
                    ok=False)
        self.commit("rise", env=dict(self.env,
                                     OPENPETE_COMPLEXITY_ACCEPT="fixture"))
        self.assertEqual(self.rows()["core/tail.c:tail"], (41, "fixture"))

    def test_modify_tightens(self):
        self.write("core/tail.c", nest("tail", 8, 1))
        self.git("add", "core/tail.c")
        self.commit("lower")
        self.assertEqual(self.rows()["core/tail.c:tail"], (37, ""))

    def test_body_edit_leaves_the_baseline_untouched(self):
        self.write("core/body.c", nest("small", 3))
        self.git("add", "core/body.c")
        self.commit("body")
        self.assertNotIn(cr.TSV, self.git(
            "show", "--name-only", "--format=", "HEAD").stdout)

    def test_marker_passes_a_rise_and_sets_the_marked_column(self):
        self.write("core/tail.c", nest("tail", 8, 4, marker="fixture table"))
        self.git("add", "core/tail.c")
        self.commit("marker only")
        text = self.git("show", "HEAD:" + cr.TSV).stdout
        self.assertEqual(cr.marked_keys(text), {"core/tail.c:tail"})
        self.write("core/tail.c", nest("tail", 8, 9, marker="fixture table"))
        self.git("add", "core/tail.c")
        self.commit("marked rise")
        self.assertEqual(self.rows()["core/tail.c:tail"], (45, ""))

    def test_unstaged_hunk_refused(self):
        self.write("core/body.c", nest("small", 3))
        self.git("add", "core/body.c")
        self.write("core/body.c", nest("small", 4))
        p = self.commit("partial", ok=False)
        self.assertIn("unstaged hunks", p.stderr)

    def test_add_needs_an_entry_and_funding(self):
        self.write("core/new.c", nest("fresh", 8, 4))
        self.git("add", "core/new.c")
        p = self.commit("add", ok=False)
        self.assertIn("no exact compile-database entry", p.stderr)
        self.wire("tail", "body", "del", "mv", "mvc", "new")
        self.commit("add", ok=False)
        self.git("rm", "-q", "core/del.c")
        self.commit("add, funded by the delete")
        rows = self.rows()
        self.assertNotIn("core/del.c:gone", rows)
        self.assertEqual(rows["core/new.c:fresh"], (40, ""))

    def test_identical_rename_carries_with_no_entry(self):
        self.git("mv", "core/mv.c", "core/renamed.c")
        self.commit("move")
        rows = self.rows()
        self.assertEqual(rows["core/renamed.c:moved"], (40, ""))
        self.assertNotIn("core/mv.c:moved", rows)

    def test_changed_rename_needs_an_entry_and_is_scored(self):
        self.git("mv", "core/mvc.c", "core/mvc2.c")
        self.write("core/mvc2.c", nest("edited", 8, 2))
        self.git("add", "core/mvc2.c")
        p = self.commit("move+edit", ok=False)
        self.assertIn("no exact compile-database entry", p.stderr)
        self.wire("tail", "body", "del", "mv", "mvc2")
        self.commit("move+edit")
        self.assertEqual(self.rows()["core/mvc2.c:edited"], (38, ""))

    def test_temporary_index_form_refused(self):
        self.write("core/body.c", nest("small", 3))
        p = self.commit("pathspec", ok=False, extra=("core/body.c",))
        self.assertIn("temporary index", p.stderr)

    def test_stale_hook_written_baseline_is_unstaged(self):
        stale = cr.render_tsv({"core/tail.c:tail": (999, "")}, {}, "hook", "x")
        self.write(cr.TSV, stale)
        self.write("README", "y\n")
        self.git("add", cr.TSV, "README")
        self.commit("doc only")
        self.assertEqual(self.rows()["core/tail.c:tail"], (40, ""))
        self.assertEqual(self.git("status", "--porcelain").stdout.strip(), "")

    def test_hand_edited_baseline_refused(self):
        head = self.git("rev-parse", "HEAD").stdout.strip()
        rows = dict(self.rows(), **{"core/tail.c:tail": (999, "")})
        self.write(cr.TSV, cr.render_tsv(rows, {}, "tighten", head))
        self.git("add", cr.TSV)
        p = self.commit("prefund", ok=False)
        self.assertIn("hand edit", p.stderr)


if __name__ == "__main__":
    unittest.main()
