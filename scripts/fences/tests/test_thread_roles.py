#!/usr/bin/env python3
"""Tests for the thread-role fence (thread_roles.py).

TextTest covers the text-level pieces: comment stripping, the roles
header's tables, the sealed-declaration scan, the writer-body scan and the
const rewrite. AnalyseTest runs the per-TU analysis (libclang 14 walk and
clang-19 over the rewritten copy) on fixture TUs in a scratch tree that
holds a copy of the roles header: each negative control yields its one
refusal, each positive control none. Skipped when clang-19 or libclang is
missing.

Run: python3 scripts/fences/tests/test_thread_roles.py
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import thread_roles as tr  # noqa: E402


class TextTest(unittest.TestCase):
    def test_strip_comments_keeps_lines_and_strings(self):
        s = 'a /* x(y)\n z */ b // c(d)\n"//s" e('
        out = tr.strip_comments(s)
        self.assertEqual(out.count("\n"), s.count("\n"))
        self.assertNotIn("x(y)", out)
        self.assertNotIn("c(d)", out)
        self.assertIn('"//s" e(', out)

    def test_tables_of_the_tree(self):
        t = tr.Tables(ROOT, [tr.ROLES_H])
        self.assertIn("sim", t.roles)
        self.assertIn("c:run.c@F@openpete_sim_thread", t.rows)
        form, roles = t.rows["c:run.c@F@openpete_sim_thread"]
        self.assertTrue(form.startswith("prop:"))
        self.assertEqual(roles, {"sim"})
        self.assertIn("c:@F@openpete_game_runtime_register", t.seal_writers["game_layout"])

    def test_writer_spans_skip_calls_and_conditions(self):
        text = ("static void w(void) {\n  x = 1;\n}\n"
                "void f(void) { if (w()) { y(); } w(); }\n"
                "int g = w();\n")
        self.assertEqual(tr.writer_spans(text, ["w"]), [(1, 3)])

    def test_rewrite_sealed(self):
        text = ("static int s_a OP_SEALED(t) = 0;\n"
                "extern T g_b OP_SEALED(t);\n"
                "void w(void) {\n  s_a = 1; op_seal_check(s_a); o.s_a = 2;\n}\n")
        out = tr.rewrite_sealed(text, ["s_a", "g_b"], [(3, 5)], "c")
        lines = out.split("\n")
        self.assertEqual(lines[0], "static int const s_a OP_SEALED(t) = 0;")
        self.assertEqual(lines[1], "extern T const g_b OP_SEALED(t);")
        self.assertIn("(*(__typeof_unqual__(s_a)*)(__UINTPTR_TYPE__)&(s_a)) = 1;", lines[3])
        self.assertIn("op_seal_check(s_a)", lines[3])
        self.assertIn("o.s_a = 2", lines[3])
        self.assertEqual(len(lines), len(text.split("\n")))


HEADER = r'''
#include "openpete_thread_roles.h"
#include <string.h>
typedef struct lay { int a; int* p; } lay_t;
OP_SEAL_DEFINE(lay);
extern lay_t g_lay OP_SEALED(lay);
lay_t g_lay OP_SEALED(lay);
static int s_g OP_GUARDED_BY(sim);
static void need(void) OP_REQUIRES(sim) { s_g = 1; }
static void call_it(void (*fn)(void) OP_SYNC_FN) { fn(); }
static void (*s_cb)(void) OP_SLOT(sim);
void lay_register(void) { op_seal_check(lay); g_lay.a = 1; memset(&g_lay, 0, sizeof g_lay); }
void lay_read(void) { const int* q = &g_lay.a; g_lay.p[0] = *q; }
void ok_sync(void) OP_REQUIRES(sim);
void ok_sync(void) OP_REQUIRES(sim) { call_it(need); s_cb(); }
void ok_store(void) { s_cb = need; }
void ok_test(void) { if (need) { } }
'''

CONTROLS = {
    "guarded_write": ("void bad(void) { s_g = 2; }", "writing variable 's_g'"),
    "sync_without_role": ("void bad(void) { call_it(need); }", "calling function 'need'"),
    "slot_call_without_role": ("void bad(void) { s_cb(); }", "calling function 's_cb'"),
    "other_reference": ("void bad(void) { void (*p)(void) = need; (void)p; }",
                        "is neither synchronous"),
    "sealed_write": ("void bad(void) { g_lay.a = 3; }", "write site of a sealed global"),
    "sealed_address": ("void bad(void) { memset(&g_lay, 0, 1); }", "write site of a sealed global"),
    "bind_outside_row": ("void bad(void) { OP_ROLE_BIND(sim); }", "does not list"),
    "boot_outside": ("void bad(void) { op_role_boot_reacquire(); }", "boot function"),
    "co_switch_outside": ("void co_switch(void*); void bad(void) { co_switch(0); }",
                          "co_switch outside"),
    "direct_assert_call": ("void bad(void) { op_role__assert_sim(0); }", "direct call"),
    "reference_before_assert": ("void bad(void) { need(); OP_ROLE_ASSERT(sim); need(); }",
                                "calling function 'need'"),
    "slot_read_escapes": ("void bad(void) { void (*p)(void) = s_cb; (void)p; }",
                          "read of the OP_SLOT(sim) slot"),
}


class DatabaseTest(unittest.TestCase):
    def test_entry_with_an_absent_source_is_skipped(self):
        tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        try:
            os.makedirs(os.path.join(tmp, "core"))
            with open(os.path.join(tmp, "core", "here.c"), "w") as fh:
                fh.write("int here;\n")
            db = os.path.join(tmp, "compile_commands.json")
            with open(db, "w") as fh:
                json.dump([{"directory": tmp, "file": f, "arguments": ["cc", "-c", f]}
                           for f in ("core/here.c", "core/gone.c")], fh)
            out = io.StringIO()
            with contextlib.redirect_stderr(out):
                ent = tr.load_entries(tmp, db, True)
            self.assertEqual(sorted(ent), ["core/here.c"])
            self.assertIn("STALE-DB (skipped): 1 windows-mingw entry names a source "
                          "absent from the tree: core/gone.c. Re-run: cmake --preset "
                          "windows-mingw", out.getvalue())
            # The selection a whole-tree hook analyses holds no absent source,
            # so no worker opens one.
            sel = tr.select_all({}, ent)
            self.assertNotIn("core/gone.c", sel["mingw"])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipUnless(shutil.which("clang-19") and os.path.isfile(tr.lib.LIBCLANG),
                     "clang-19 or libclang 14 missing")
class AnalyseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        inc = os.path.join(self.tmp, "core", "include")
        os.makedirs(inc)
        hdr = tr.read_text(os.path.join(ROOT, tr.ROLES_H))
        hdr = hdr.replace('SEAL(game_layout, "c:@F@openpete_game_runtime_register")',
                          'SEAL(lay, "c:@F@lay_register")')
        with open(os.path.join(inc, "openpete_thread_roles.h"), "w") as fh:
            fh.write(hdr)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def analyse(self, body):
        rel = "core/fixture.c"
        with open(os.path.join(self.tmp, rel), "w") as fh:
            fh.write(HEADER + body + "\n")
        files = [tr.ROLES_H, rel]
        tables = tr.Tables(self.tmp, files)
        ctx = tr.build_ctx(self.tmp, tables, self.tmp)
        ctx["writer_files"] = {rel}
        tr.compile_patterns(ctx)
        flags = ["-I" + os.path.join(self.tmp, "core", "include"), "-std=gnu11"]
        return tr.analyse((ctx, rel, "linux", self.tmp, flags))

    def test_positive_controls_pass(self):
        r = self.analyse("")
        self.assertEqual(r["parse"], [])
        self.assertEqual(r["refusals"], [])
        self.assertTrue(r["walked"])
        self.assertGreaterEqual(r["kinds"].get("sync", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("slot", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("slot-call", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("test", 0), 1)

    def test_negative_controls_refuse_once(self):
        for name, (body, want) in sorted(CONTROLS.items()):
            with self.subTest(control=name):
                r = self.analyse(body)
                self.assertEqual(r["parse"], [], name)
                hits = [x for x in r["refusals"] if want in x]
                self.assertEqual(len(hits), 1, "%s: %s" % (name, r["refusals"]))

    def test_unannotated_redeclaration_is_refused(self):
        r = self.analyse("void ok_sync(void);")
        self.assertEqual(r["refusals"], [])
        tables = tr.Tables(self.tmp, [tr.ROLES_H, "core/fixture.c"])
        rc, _ = tr.report(self.tmp, tables, {}, [r], 0.0, set(), "test")
        self.assertEqual(rc, 1)

    def analyse_cxx(self, body):
        """A C++ TU that includes an annotated header inside `extern "C" {`
        and names a slot, so the walk runs over it."""
        with open(os.path.join(self.tmp, "core", "include", "fixture_api.h"), "w") as fh:
            fh.write('#include "openpete_thread_roles.h"\n'
                     "void api_fn(void) OP_REQUIRES(sim);\n"
                     "static inline void api_inline(void) { void (*p)(void) = api_fn; (void)p; }\n")
        rel = "core/fixture.cpp"
        with open(os.path.join(self.tmp, rel), "w") as fh:
            fh.write('#include "openpete_thread_roles.h"\n'
                     'extern "C" {\n#include "fixture_api.h"\n}\n'
                     "static void (*s_cb)(void) OP_SLOT(sim);\n" + body + "\n")
        files = [tr.ROLES_H, "core/include/fixture_api.h", rel]
        ctx = tr.build_ctx(self.tmp, tr.Tables(self.tmp, files), self.tmp)
        tr.compile_patterns(ctx)
        flags = ["-I" + os.path.join(self.tmp, "core", "include"), "-std=gnu++17"]
        return tr.analyse((ctx, rel, "linux", self.tmp, flags))

    def test_included_declarations_are_not_local(self):
        # The header's prototype and inline body arrive through the
        # #include inside extern "C"; they are the header's, not this TU's.
        r = self.analyse_cxx("")
        self.assertEqual(r["parse"], [])
        self.assertTrue(r["walked"])
        self.assertEqual(r["refusals"], [])

    def test_local_prototype_in_cxx_is_refused(self):
        for name, body in (("extern_c_block", 'extern "C" { void api_fn(void) OP_REQUIRES(sim); }'),
                           ("block_scope", 'extern "C" void bad(void) '
                                           "{ void api_fn(void) OP_REQUIRES(sim); }")):
            with self.subTest(control=name):
                r = self.analyse_cxx(body)
                self.assertEqual(r["parse"], [], name)
                hits = [x for x in r["refusals"] if "local prototype" in x]
                self.assertEqual(len(hits), 1, "%s: %s" % (name, r["refusals"]))
                self.assertIn("core/fixture.cpp:", hits[0])

    def test_slot_store_recorded(self):
        r = self.analyse("")
        self.assertIn(("c:fixture.c@F@need", "need", "sim"), r["slot_stores"])


class AnnotatorTextTest(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, os.path.join(ROOT, "scripts"))
        import thread_roles_annotate as ta
        self.ta = ta

    def test_place_adds_and_upgrades(self):
        b = b"static int f(int a,\n              int b) {\n  return a;\n}\n"
        out, changed = self.ta.place(b, 1, 12, "f", "sim", False)
        self.assertTrue(changed)
        self.assertIn(b"int b) OP_REQUIRES_SHARED(sim) {", out)
        out2, changed = self.ta.place(out, 1, 12, "f", "sim", True)
        self.assertTrue(changed)
        self.assertIn(b"int b) OP_REQUIRES(sim) {", out2)
        out3, changed = self.ta.place(out2, 1, 12, "f", "sim", False)
        self.assertFalse(changed)

    def test_place_refuses_a_name_not_spelled_there(self):
        with self.assertRaises(ValueError):
            self.ta.place(b"#define G f\nint G(void);\n", 2, 5, "f", "sim", True)

    def test_ensure_include_after_the_last_top_include(self):
        b = b'#include <a.h>\n#include "b.h"\nstruct s { int x; };\n#include "c.h"\n'
        out = self.ta.ensure_include(b)
        self.assertTrue(out.startswith(b'#include <a.h>\n#include "b.h"\n#include "openpete_thread_roles.h"'))
        self.assertEqual(self.ta.ensure_include(out), out)


if __name__ == "__main__":
    unittest.main()
