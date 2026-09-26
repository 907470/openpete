#!/usr/bin/env python3
"""Tests for the function-documentation ratchet (doc_ratchet.py).

DecideTest drives the pure decision function per rule. TouchedTest drives
the touched rule. MergeTest drives the per-entry merge (the worst form
across a TU's entries, ordinals on a repeated name). ControlsTest runs the
fixture controls and a tampered copy of them (the controls must fail when
an expectation is wrong). EndToEndTest builds a throwaway git repository
with the hook installed and its own compile database (OPENPETE_DOCS_DB)
and commits each shape: A, M touching and not touching an undocumented
function, a comment-only commit, header against definition placement, a
duplicate, a wrong @param, a comment above a #define, a macro-generated
definition, an unstaged canonical header, an extern "C" definition in a
C++ TU, a @brief comment, a header whose parameter names differ from the
definition's, the accept escape, and a cherry-pick of a branch that retired
rows across a landing that retired adjacent rows.

Run: python3 scripts/fences/tests/test_doc_ratchet.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)
import doc_ratchet as dr                                     # noqa: E402
import ratchet_tsv_merge as tm                               # noqa: E402

SCRIPT = os.path.join(FENCES, "doc_ratchet.py")


def fx(form="none", file="a.c", start=10, end=20, macro=False,
       two_headers=(), canonical=None, header=None):
    ext = [(file, start, end)]
    if header:
        ext.append(header)
    return dict(form=form, macro=macro, two_headers=list(two_headers),
                extents=ext, canonical=canonical or (file, start))


def decide(before, after, acted, **kw):
    files = {k.rsplit(":", 1)[0] for k in list(before) + list(after)}
    args = dict(renames={}, changed={}, wired=files, present=files,
                accept=None, staged=(), wdoc=(), unwired_all=False)
    args.update(kw)
    return dr.decide({k: v if isinstance(v, tuple) else (v, "")
                      for k, v in before.items()}, after, acted,
                     args["renames"], args["changed"], args["wired"],
                     args["present"], args["accept"],
                     staged_files=args["staged"], wdoc=args["wdoc"],
                     unwired_all=args["unwired_all"])


def codes(bins):
    return {k: c for k, c, _ in bins["REFUSED"]}


class DecideTest(unittest.TestCase):
    def test_clause1_new_undocumented_refused(self):
        v, _, bins = decide({}, {"a.c:f": fx()}, {"a.c:f"})
        self.assertEqual((v, codes(bins)), ("REFUSED", {"a.c:f": "clause1"}))

    def test_clause1_meets_needs_no_row(self):
        v, rows, _ = decide({}, {"a.c:f": fx("meets")}, {"a.c:f"})
        self.assertEqual((v, rows), ("PASS", {}))

    def test_clause2_touched_row_refused_unless_it_meets(self):
        ch = {"a.c": [(15, 1)]}
        v, _, bins = decide({"a.c:f": "plain"}, {"a.c:f": fx("plain")},
                            {"a.c:f"}, changed=ch)
        self.assertEqual(codes(bins), {"a.c:f": "clause2"})
        v, rows, bins = decide({"a.c:f": "plain"}, {"a.c:f": fx("meets")},
                               {"a.c:f"}, changed=ch)
        self.assertEqual((v, rows), ("PASS", {}))
        self.assertEqual(bins["RETIRED"], [("a.c:f", "plain", "meets")])

    def test_clause3_untouched_row_kept_with_form_reread(self):
        v, rows, bins = decide({"a.c:f": "plain"}, {"a.c:f": fx("none")},
                               {"a.c:f"}, changed={"a.c": [(5, 1)]})
        self.assertEqual((v, rows), ("PASS", {"a.c:f": ("none", "")}))
        self.assertEqual(bins["FORM"], [("a.c:f", "plain", "none")])

    def test_clause3_documented_row_retires(self):
        v, rows, _ = decide({"a.c:f": "none"}, {"a.c:f": fx("meets")},
                            {"a.c:f"}, changed={"a.c": [(9, 1)]})
        self.assertEqual((v, rows), ("PASS", {}))

    def test_keys_outside_the_acted_on_set_never_refuse(self):
        v, rows, _ = decide({"o.c:g": "none"},
                            {"o.c:g": fx("none", "o.c"), "o.c:h": fx("none", "o.c")},
                            {"a.c:f"})
        self.assertEqual((v, rows), ("PASS", {"o.c:g": ("none", "")}))

    def test_misplaced_duplicate_two_headers_refused(self):
        after = {"a.c:m": fx("misplaced"), "a.c:d": fx("duplicate"),
                 "a.c:t": fx("meets", two_headers=("x.h", "y.h"))}
        v, _, bins = decide({"a.c:m": "none"}, after, set(after))
        self.assertEqual(codes(bins), {"a.c:m": "misplaced", "a.c:d": "duplicate",
                                       "a.c:t": "two-headers"})

    def test_accept_covers_clauses_and_misplaced_only(self):
        after = {"a.c:n": fx("none"), "a.c:m": fx("misplaced"),
                 "a.c:d": fx("duplicate")}
        v, rows, bins = decide({}, after, set(after), accept="mechanical move")
        self.assertEqual(codes(bins), {"a.c:d": "duplicate"})
        self.assertEqual(rows, {"a.c:n": ("none", "mechanical move"),
                                "a.c:m": ("misplaced", "mechanical move")})
        # An accepted misplaced row is carried by later commits.
        v, _, _ = decide({"a.c:m": ("misplaced", "why")},
                         {"a.c:m": fx("misplaced")}, {"a.c:m"})
        self.assertEqual(v, "PASS")

    def test_accepted_row_touched_again_is_refused(self):
        v, _, bins = decide({"a.c:f": ("none", "why")}, {"a.c:f": fx("none")},
                            {"a.c:f"}, changed={"a.c": [(12, 1)]})
        self.assertEqual(codes(bins), {"a.c:f": "clause2"})

    def test_macro_exempt(self):
        v, rows, bins = decide({}, {"a.c:g": fx("MACRO", macro=True)}, {"a.c:g"})
        self.assertEqual((v, rows, bins["MACRO"]), ("PASS", {}, ["a.c:g"]))

    def test_absent_function_retires(self):
        v, rows, bins = decide({"a.c:gone": "none"}, {}, {"a.c:gone"})
        self.assertEqual((v, rows), ("PASS", {}))

    def test_wdoc_refused_in_staged_or_canonical_file_else_binned(self):
        w = [("x.h", 3, "parameter 'q' not found")]
        v, _, bins = decide({}, {"a.c:f": fx("meets")}, {"a.c:f"}, wdoc=w,
                            staged=("x.h",))
        self.assertEqual(codes(bins), {"x.h:3": "wdoc"})
        v, _, bins = decide({}, {"a.c:f": fx("meets", canonical=("x.h", 2))},
                            {"a.c:f"}, wdoc=w)
        self.assertEqual(codes(bins), {"x.h:3": "wdoc"})
        v, _, bins = decide({}, {"a.c:f": fx("meets")}, {"a.c:f"}, wdoc=w)
        self.assertEqual((v, len(bins["WDOC"])), ("PASS", 1))

    def test_unwired_row(self):
        kw = dict(present={"u.c", "a.c"}, wired={"a.c"})
        v, _, bins = decide({"u.c:f": "none"}, {}, {"u.c:f"}, **kw)
        self.assertEqual(codes(bins), {"u.c:f": "unwired"})
        v, _, _ = decide({"u.c:f": "none"}, {}, {"a.c:f"}, **kw)
        self.assertEqual(v, "PASS")
        v, _, _ = decide({"u.c:f": "none"}, {}, {"a.c:f"}, unwired_all=True, **kw)
        self.assertEqual(v, "REFUSED")

    def test_rename_carry(self):
        # Identical blob: rows carry to the new key without a parse.
        v, rows, _ = decide({"old/a.c:f": "none"}, {}, {"new/a.c:f"},
                            renames={"old/a.c": ("new/a.c", True)},
                            present={"new/a.c"}, wired=set())
        self.assertEqual((v, rows), ("PASS", {"new/a.c:f": ("none", "")}))
        # Changed blob: the carried row meets clause 2 at the new key.
        kw = dict(renames={"old/a.c": ("new/a.c", False)}, present={"new/a.c"},
                  wired={"new/a.c"}, changed={"new/a.c": [(12, 1)]})
        v, _, bins = decide({"old/a.c:f": "none"},
                            {"new/a.c:f": fx("none", "new/a.c")}, {"new/a.c:f"}, **kw)
        self.assertEqual(codes(bins), {"new/a.c:f": "clause2"})

    def test_tsv_round_trip(self):
        rows = {"a.c:f": ("none", ""), "b.c:g#2": ("plain", "why")}
        text = dr.render_tsv(rows, "tighten", "abc", "libclang 14")
        head, back = dr.parse_tsv(text)
        self.assertEqual(back, rows)
        self.assertEqual((head["written-by"], head["head"], head["library"]),
                         ("tighten", "abc", "libclang 14"))


class TouchedTest(unittest.TestCase):
    def test_added_line_inside(self):
        self.assertTrue(dr.touched([("a.c", 10, 20)], {"a.c": [(20, 3)]}))
        self.assertFalse(dr.touched([("a.c", 10, 20)], {"a.c": [(7, 3)]}))

    def test_pure_deletion_strict_upper_bound(self):
        # b is the after-side line before the deletion point.
        self.assertTrue(dr.touched([("a.c", 10, 20)], {"a.c": [(10, 0)]}))
        self.assertTrue(dr.touched([("a.c", 10, 20)], {"a.c": [(19, 0)]}))
        self.assertFalse(dr.touched([("a.c", 10, 20)], {"a.c": [(20, 0)]}))
        self.assertFalse(dr.touched([("a.c", 10, 20)], {"a.c": [(9, 0)]}))

    def test_header_declaration_hunk_is_a_touch(self):
        ext = [("a.c", 10, 20), ("a.h", 5, 6)]
        self.assertTrue(dr.touched(ext, {"a.h": [(6, 1)]}))
        self.assertFalse(dr.touched(ext, {"a.h": [(4, 1)]}))


class MergeTest(unittest.TestCase):
    def res(self, tu, defs):
        return dict(tu=tu, errors=[], wdoc=[], defs=defs, hdecls={}, seconds=0)

    def d(self, name, line, form, file="a.c"):
        return dict(file=file, name=name, start=line, form=form, two_headers=[])

    def test_worst_form_across_entries(self):
        c = dr.merge_results([self.res("a.c", [self.d("f", 3, "meets")]),
                              self.res("a.c", [self.d("f", 3, "untagged")])])
        self.assertEqual(c.facts["a.c:f"]["form"], "untagged")
        self.assertEqual(c.definitions, 1)

    def test_repeated_name_takes_an_ordinal_by_line(self):
        c = dr.merge_results([self.res("a.cpp", [
            self.d("f", 9, "none", "a.cpp"), self.d("f", 3, "meets", "a.cpp")])])
        self.assertEqual((c.facts["a.cpp:f"]["start"], c.facts["a.cpp:f#2"]["start"]),
                         (3, 9))

    def test_header_inline_keyed_once_across_includers(self):
        c = dr.merge_results([
            self.res("a.c", [self.d("h", 4, "meets", "x.h")]),
            self.res("b.c", [self.d("h", 4, "none", "x.h")])])
        self.assertEqual(list(c.facts), ["x.h:h"])
        self.assertEqual(c.tus_of["x.h:h"], {"a.c", "b.c"})
        self.assertEqual(c.facts["x.h:h"]["form"], "none")


class ControlsTest(unittest.TestCase):
    def test_controls_pass(self):
        sc = dr.Scorer({"fixture": dr.fixture_ctx()})
        try:
            self.assertEqual(dr.run_controls(sc), 20)
        finally:
            sc.close()

    def test_a_wrong_expectation_fails_the_controls(self):
        with open(os.path.join(dr.FIXTURE_DIR, "controls.json")) as fh:
            controls = json.load(fh)
        controls[0]["expect"]["new.c:undocumented_new"] = "pass"
        tmp = tempfile.mkdtemp(prefix="doc-controls-")
        try:
            for name in os.listdir(dr.FIXTURE_DIR):
                src = os.path.join(dr.FIXTURE_DIR, name)
                (shutil.copytree if os.path.isdir(src) else shutil.copy)(
                    src, os.path.join(tmp, name))
            with open(os.path.join(tmp, "controls.json"), "w") as fh:
                json.dump(controls, fh)
            saved = dr.FIXTURE_DIR
            dr.FIXTURE_DIR = tmp
            sc = dr.Scorer({"fixture": dr.fixture_ctx()})
            try:
                with self.assertRaises(dr.Refusal) as cm:
                    dr.run_controls(sc)
                self.assertIn("1 new undocumented", str(cm.exception))
            finally:
                sc.close()
                dr.FIXTURE_DIR = saved
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


HEADER = """#ifndef API_H
#define API_H
/** Adds one.
 * @param a The value.
 * @return a plus one. */
int api_add(int a);

int api_plain(int a);

/** Returns the anchor.
 * @param anchor The anchor.
 * @return The anchor. */
int api_anchor(int anchor);
#endif
"""

SOURCE = """#include "api.h"
#define GEN(n) static int n(void) { return 1; }
GEN(generated)

int api_add(int a) {
    return a + 1;
}

/* Plain comment, the row's form. */
int api_plain(int a) {
    return a;
}

int api_anchor(int a) { return a + generated(); }

static int helper(int a) {
    return a * 2;
}

static int other(int a) {
    return helper(a);
}

int use_other(int a);
int use_other(int a) { return other(a); }
"""

CPP = """extern "C" {
/** Returns a.
 * @param a The value.
 * @return a. */
int externc_fn(int a) { return a; }
}
"""


@unittest.skipUnless(os.path.isfile(dr.lib.LIBCLANG) or os.environ.get(
    "OPENPETE_LIBCLANG"), "libclang 14 not installed")
class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="doc-e2e-")
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "core"))
        self.db = os.path.join(self.tmp, "compile_commands.json")
        self.env = dict(os.environ, OPENPETE_DOCS_DB=self.db,
                        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        self.env.pop(dr.ACCEPT_VAR, None)
        self.env.pop("GIT_INDEX_FILE", None)
        # Configuration from the caller's environment would override the
        # scratch repository's own (the merge driver under test).
        for k in [k for k in self.env if k.startswith("GIT_CONFIG_")]:
            del self.env[k]
        self.git("init", "-q", "-b", "main")
        self.write("core/api.h", HEADER)
        self.write("core/api.c", SOURCE)
        self.write("core/ec.cpp", CPP)
        self.write("README", "x\n")
        self.wire("api.c", "ec.cpp")
        self.git("add", "core", "README")
        self.git("commit", "-q", "-m", "init")
        self.run_fence("baseline")
        hook = os.path.join(self.repo, ".git", "hooks", "pre-commit")
        with open(hook, "w") as fh:
            fh.write("#!/bin/sh\nexec python3 \"%s\" hook\n" % SCRIPT)
        os.chmod(hook, 0o755)
        self.git("add", dr.TSV)
        self.commit("baseline")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, *args, ok=True, env=None):
        p = subprocess.run(["git", "-C", self.repo] + list(args),
                           capture_output=True, text=True, env=env or self.env)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def commit(self, msg, ok=True, env=None):
        p = self.git("commit", "-q", "-m", msg, ok=False, env=env)
        self.assertEqual(p.returncode == 0, ok, p.stderr)
        return p

    def run_fence(self, *args):
        p = subprocess.run([sys.executable, SCRIPT] + list(args), cwd=self.repo,
                           capture_output=True, text=True, env=self.env)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def write(self, rel, text):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)

    def read(self, rel):
        with open(os.path.join(self.repo, rel)) as fh:
            return fh.read()

    def sub(self, rel, old, new):
        text = self.read(rel)
        self.assertIn(old, text)
        self.write(rel, text.replace(old, new, 1))

    def wire(self, *names):
        with open(self.db, "w") as fh:
            json.dump([{"directory": self.repo, "file": "core/%s" % n,
                        "command": "%s -std=%s -Icore -c core/%s -o %s.o" % (
                            "c++" if n.endswith(".cpp") else "cc",
                            "c++17" if n.endswith(".cpp") else "c11", n, n)}
                       for n in names], fh)

    def rows(self, ref="HEAD"):
        return dr.parse_tsv(self.git("show", ref + ":" + dr.TSV).stdout)[1]

    def test_baseline_rows(self):
        self.assertEqual(self.rows(), {
            # The canonical site is the header declaration, which carries no
            # comment; the plain block above the definition is a body note.
            "core/api.c:api_plain": ("none", ""),
            "core/api.c:helper": ("none", ""),
            "core/api.c:other": ("none", ""),
            "core/api.c:use_other": ("none", "")})

    def test_add_undocumented_refused_then_documented(self):
        self.write("core/new.c", "int fresh(int a) { return a; }\n")
        self.wire("api.c", "ec.cpp", "new.c")
        self.git("add", "core/new.c")
        p = self.commit("add", ok=False)
        self.assertIn("REFUSED  core/new.c:fresh  (new)", p.stderr)
        self.assertIn("External linkage: brief, @param a, @return.", p.stderr)
        self.write("core/new.c", "/** Returns a.\n * @param a The value.\n"
                   " * @return a. */\nint fresh(int a) { return a; }\n")
        self.git("add", "core/new.c")
        self.commit("add, documented")
        self.assertNotIn("core/new.c:fresh", self.rows())

    def test_modify_touching_undocumented_refused(self):
        self.sub("core/api.c", "return a * 2;", "return a * 3;")
        self.git("add", "core/api.c")
        p = self.commit("touch", ok=False)
        self.assertIn("REFUSED  core/api.c:helper  (touched, lines 16-18; "
                      "changed 17)", p.stderr)
        self.assertIn("Internal linkage: a brief is enough.", p.stderr)
        self.assertIn("Ways to land this commit", p.stderr)

    def test_modify_not_touching_keeps_rows(self):
        self.sub("core/api.c", "return a + 1;", "return 1 + a;")
        self.git("add", "core/api.c")
        self.commit("touch documented only")
        self.assertIn("core/api.c:helper", self.rows())

    def test_comment_only_commit_retires_the_row(self):
        self.sub("core/api.c", "static int helper(", "/** Doubles a. */\nstatic int helper(")
        self.git("add", "core/api.c")
        self.commit("document helper")
        rows = self.rows()
        self.assertNotIn("core/api.c:helper", rows)
        self.assertIn("core/api.c:other", rows)

    def test_accept_escape_writes_the_reason(self):
        self.sub("core/api.c", "return a * 2;", "return a * 3;")
        self.git("add", "core/api.c")
        self.commit("touch", env=dict(self.env, **{dr.ACCEPT_VAR: "1"}), ok=False)
        self.commit("touch", env=dict(self.env, **{dr.ACCEPT_VAR: "fixture pass"}))
        self.assertEqual(self.rows()["core/api.c:helper"], ("none", "fixture pass"))

    def test_definition_comment_when_header_declares_is_misplaced(self):
        self.sub("core/api.c", "/* Plain comment, the row's form. */",
                 "/** Returns a.\n * @param a The value.\n * @return a. */")
        self.git("add", "core/api.c")
        p = self.commit("misplaced", ok=False)
        self.assertIn("REFUSED  core/api.c:api_plain  (misplaced)", p.stderr)
        self.assertIn("canonical site is core/api.h:8", p.stderr)

    def test_duplicate_refused(self):
        self.sub("core/api.c", "int api_add(int a) {",
                 "/** Adds one.\n * @param a The value.\n * @return a plus one. */\n"
                 "int api_add(int a) {")
        self.git("add", "core/api.c")
        p = self.commit("dup", ok=False)
        self.assertIn("REFUSED  core/api.c:api_add  (duplicate)", p.stderr)

    def test_wrong_param_refused(self):
        self.sub("core/api.h", " * @param a The value.", " * @param b The value.")
        self.git("add", "core/api.h")
        p = self.commit("wrong param", ok=False)
        self.assertIn("-Wdocumentation: parameter 'b' not found", p.stderr)

    def test_comment_above_define_refused_naming_the_token(self):
        self.sub("core/api.c", "static int other(",
                 "/** Calls the helper. */\n#define OTHER_SCALE 1\nstatic int other(")
        self.sub("core/api.c", "return helper(a);", "return helper(a) * OTHER_SCALE;")
        self.git("add", "core/api.c")
        p = self.commit("blocked", ok=False)
        self.assertIn("`#define OTHER_SCALE 1` at", p.stderr)
        self.assertIn("move the comment below it", p.stderr)

    def test_macro_generated_definition_passes(self):
        self.sub("core/api.c", "GEN(generated)", "GEN(generated)\nGEN(generated2)")
        self.git("add", "core/api.c")
        p = self.commit("macro")
        self.assertIn("MACRO", self.run_fence("check").stderr)

    def test_unstaged_canonical_header_comment_refused(self):
        self.sub("core/api.c", "return a;\n}", "return a + 0;\n}")
        self.sub("core/api.h", "int api_plain(int a);",
                 "/** Returns a.\n * @param a The value.\n * @return a. */\n"
                 "int api_plain(int a);")
        self.git("add", "core/api.c")
        p = self.commit("header not staged", ok=False)
        self.assertIn("git add -p core/api.h", p.stderr)
        self.git("add", "core/api.h")
        self.commit("header staged")
        self.assertNotIn("core/api.c:api_plain", self.rows())

    def test_header_only_commit_acts_on_its_keys(self):
        self.sub("core/api.h", "int api_plain(int a);",
                 "/** Returns a.\n * @param a The value.\n * @return a. */\n"
                 "int api_plain(int a);")
        self.git("add", "core/api.h")
        self.commit("document at the header")
        self.assertNotIn("core/api.c:api_plain", self.rows())

    def test_extern_c_definition_is_scored(self):
        self.sub("core/ec.cpp", " * @param a The value.\n", "")
        self.git("add", "core/ec.cpp")
        p = self.commit("untag", ok=False)
        self.assertIn("REFUSED  core/ec.cpp:externc_fn", p.stderr)
        self.assertIn("lacks @param a", p.stderr)

    def test_brief_command_passes(self):
        self.sub("core/api.c", "static int helper(",
                 "/** @brief Doubles a. */\nstatic int helper(")
        self.git("add", "core/api.c")
        self.commit("brief")
        self.assertNotIn("core/api.c:helper", self.rows())

    def test_header_parameter_names_differ_from_the_definition(self):
        # api_anchor: the header names `anchor`, the definition `a`; the
        # comment at the header validates against the header's names.
        self.sub("core/api.c", "int api_anchor(int a) { return a + generated(); }",
                 "int api_anchor(int a) {\n    return a + generated();\n}")
        self.git("add", "core/api.c")
        self.commit("reshape api_anchor")

    def test_pick_across_a_landing_merges_rows_by_key(self):
        self.git("config", "merge.%s.driver" % tm.DRIVER_NAME,
                 "python3 %s %%O %%A %%B %%P"
                 % os.path.join(FENCES, "ratchet_tsv_merge.py"))
        self.write(".gitattributes", "baselines/docs/*.tsv merge=ratchet-tsv\n")
        self.git("add", ".gitattributes")
        self.commit("attributes")
        self.git("switch", "-q", "-c", "topic")
        self.sub("core/api.c", "static int other(", "/** Calls the helper. */\nstatic int other(")
        self.git("add", "core/api.c")
        self.commit("topic documents other")
        self.git("switch", "-q", "main")
        self.sub("core/api.c", "static int helper(", "/** Doubles a. */\nstatic int helper(")
        self.git("add", "core/api.c")
        self.commit("main documents helper")
        p = self.git("cherry-pick", "topic", ok=False)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        rows = self.rows()
        self.assertNotIn("core/api.c:helper", rows)
        self.assertNotIn("core/api.c:other", rows)
        self.assertIn("core/api.c:use_other", rows)
        self.assertEqual(self.run_fence("check").returncode, 0)


if __name__ == "__main__":
    unittest.main()
