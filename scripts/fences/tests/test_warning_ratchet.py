#!/usr/bin/env python3
"""Tests for the warning ratchet (warning_ratchet.py).

TextTest covers the command rewrite, the GCC text parser on captured
GCC 11 output (function context, an inlining chain into a libc header, a
macro expansion note), row attribution (outermost in-scope expansion, the
first in-scope inlined-from frame, the TU fallback), message
normalisation and the pragma escape. DecideTest drives the pure decision:
a rise an altered entry produces refuses, one only unaltered entries
produce is INHERITED, only rows the mode may lower shrink, an accepted rise
carries its reason. CompileTest compiles a scratch TU with the host GCC and
reads one front-end and one middle-end row from it; skipped without gcc.

Run: python3 scripts/fences/tests/test_warning_ratchet.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import warning_ratchet as wr  # noqa: E402

ERRSET = {"-Werror=format", "-Wno-error=format-truncation",
          "-Werror=implicit-function-declaration"}

# GCC 11.4, -O2 -Wall -Wextra and the four diagnostic-format options, over
# h.h (a comparison macro and a static inline strcpy wrapper) and a.c.
GCC_TEXT = """In file included from core/a.c:1:
core/a.c: In function 'f':
core/h.h:2:23: warning: comparison of integer expressions of different signedness: 'int' and 'unsigned int' [-Wsign-compare]
core/a.c:2:47: note: in expansion of macro 'CMP'
core/a.c:2:32: warning: unused variable 'unused' [-Wunused-variable]
In file included from /usr/include/string.h:535,
                 from core/h.h:1,
                 from core/a.c:1:
In function 'strcpy',
    inlined from 'hcopy' at core/h.h:3:52,
    inlined from 'g' at core/a.c:3:29:
/usr/include/x86_64-linux-gnu/bits/string_fortified.h:79:10: warning: '__builtin___strcpy_chk' writing 12 bytes into a region of size 2 [-Wstringop-overflow=]
core/a.c: In function 'g':
core/a.c:3:21: note: destination object 'buf' of size 2
At top level:
core/a.c:9:13: warning: 'unused_fn.constprop.0' defined but not used [-Wunused-function]
core/a.c:12:5: error: implicit declaration of function 'nope' [-Werror=implicit-function-declaration]
"""


class TextTest(unittest.TestCase):
    def test_rewrite_strips_warning_options_but_keeps_the_error_set(self):
        argv = ["/usr/bin/ccache", "/usr/bin/cc", "-DX", "-w", "-Wno-format",
                "-Wformat", "-Werror=format", "-Wno-error=format-truncation",
                "-Wl,--as-needed", "-Wp,-D_FORTIFY_SOURCE=2", "-MD", "-MF", "x.d",
                "-O1", "-o", "x.o", "-c"]
        cc, flags = wr.rewrite(argv, ERRSET)
        self.assertEqual(cc, "/usr/bin/cc")
        self.assertEqual(flags[:6], ["-DX", "-Werror=format", "-Wno-error=format-truncation",
                                     "-Wl,--as-needed", "-Wp,-D_FORTIFY_SOURCE=2", "-O1"])
        self.assertEqual(flags[6:8], ["-Wall", "-Wextra"])
        self.assertIn("-fdiagnostics-color=never", flags)
        self.assertNotIn("-fdiagnostics-plain-output", flags)

    def test_cache_key_command_keeps_the_build_id_length_only(self):
        a = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-2-gabcdef"'])
        b = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-3-g123456"'])
        c = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-13-g123456"'])
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)

    def test_parse_reads_context_inlining_and_macro_notes(self):
        ds = wr.parse(GCC_TEXT, "/r")
        self.assertEqual([d["opt"] for d in ds],
                         ["-Wsign-compare", "-Wunused-variable", "-Wstringop-overflow=",
                          "-Wunused-function", "-Wimplicit-function-declaration"])
        self.assertEqual(ds[0]["macros"], [("/r/core/a.c", 2, 47)])
        self.assertEqual(ds[0]["func"], "f")
        self.assertEqual(ds[2]["func"], "strcpy")
        self.assertEqual([f[0] for f in ds[2]["inl"]], ["hcopy", "g"])
        self.assertIsNone(ds[3]["func"])
        self.assertEqual(ds[4]["sev"], "error")

    def test_attribution(self):
        ds = wr.parse(GCC_TEXT, "/r")
        a = lambda d, k: wr.attribute(d, "core/a.c", "/r", k)
        # The macro use site in a.c, not the definition in h.h.
        self.assertEqual(a(ds[0], "fe"), (("core/a.c", "-Wsign-compare", "fe",
                                           "comparison of integer expressions of different "
                                           "signedness: 'int' and 'unsigned int'"),
                                          ("core/a.c", 2, 47)))
        # Outside the scope: the first in-scope inlined-from frame (h.h's hcopy).
        self.assertEqual(a(ds[2], "me"), (("core/h.h", "-Wstringop-overflow=", "me", "hcopy"),
                                          ("core/h.h", 3, 52)))
        # Clone suffixes leave the key; digits normalise in front-end details.
        key, _ = a(ds[3], "fe")
        self.assertEqual(key[3], "'unused_fn.constprop.N' defined but not used")
        self.assertEqual(a(ds[3], "me")[0][3], "-")
        self.assertEqual(wr.function_name("emit.part.0"), "emit")

    def test_out_of_scope_without_frame_goes_to_the_tu(self):
        d = wr.parse("/usr/include/x.h:4:1: warning: thing 7 [-Wfoo]\n", "/r")[0]
        key, loc = wr.attribute(d, "core/t.c", "/r", "fe")
        self.assertEqual(key, ("core/t.c", "-Wfoo", "fe", "thing N"))
        self.assertEqual(loc[0], "core/t.c")

    def test_pragmas(self):
        names = wr.error_set_names(ERRSET)
        lines = ['#pragma GCC diagnostic ignored "-Wformat"',
                 '#pragma GCC diagnostic ignored "-Wunused-variable"',
                 '// warning-ratchet: the table is read by the generator',
                 '#pragma GCC diagnostic ignored "-Wunused-variable"',
                 '#pragma GCC diagnostic push']
        out = wr.pragma_refusals(lines, "core/a.c", range(1, 6), names)
        self.assertEqual(len(out), 2)
        self.assertIn("error-set member", out[0])
        self.assertIn("core/a.c:2:", out[1])


class DecideTest(unittest.TestCase):
    K1 = ("core/a.h", "-Wx", "fe", "m")
    K2 = ("core/b.c", "-Wx", "fe", "m")

    def run_decide(self, before, after, contrib, altered, shrink=lambda k: False,
                   accept=None):
        return wr.decide(before, after, contrib, altered, lambda k: True, shrink,
                         accept or {}, lambda k: False)

    def test_rise_from_an_altered_entry_refuses_even_in_an_unstaged_header(self):
        v, _, bins = self.run_decide({self.K1: (1, "")}, {self.K1: 2},
                                     {self.K1: {"e1", "e2"}}, {"e2"})
        self.assertEqual(v, "REFUSED")
        self.assertEqual(bins["RISE"][0][1:], (1, 2))

    def test_rise_only_unaltered_entries_produce_is_inherited(self):
        v, rows, bins = self.run_decide({self.K1: (1, "")}, {self.K1: 2},
                                        {self.K1: {"e1"}}, {"e2"})
        self.assertEqual(v, "PASS")
        self.assertEqual(len(bins["INHERITED"]), 1)
        self.assertEqual(rows[self.K1], (1, ""))

    def test_only_permitted_rows_shrink(self):
        v, rows, bins = self.run_decide({self.K1: (3, ""), self.K2: (2, "")},
                                        {self.K1: 1}, {}, set(),
                                        shrink=lambda k: k[0] == "core/b.c")
        self.assertEqual(rows, {self.K1: (3, "")})
        self.assertEqual([b[0] for b in bins["RETIRED"]], [wr.key_text(self.K2)])
        self.assertEqual(len(bins["SHRINKABLE"]), 1)

    def test_accepted_rise_carries_its_reason(self):
        v, rows, bins = self.run_decide({}, {self.K2: 1}, {self.K2: {"e"}}, None,
                                        accept={wr.key_text(self.K2): "why"})
        self.assertEqual((v, rows[self.K2]), ("PASS", (1, "why")))


class CompileTest(unittest.TestCase):
    def setUp(self):
        if not shutil.which("gcc"):
            self.skipTest("no gcc")
        self.tmp = tempfile.mkdtemp(prefix="warning-ratchet-test-")
        os.makedirs(os.path.join(self.tmp, "core"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_front_and_middle_end_rows(self):
        src = os.path.join(self.tmp, "core", "t.c")
        with open(src, "w") as fh:
            fh.write("int f(int c) { int unused; return c; }\n"
                     "static int g(void) { return 0; }\n")
        _, flags = wr.rewrite(["gcc", "-O2", "-c", src, "-o", "t.o"], set())
        flags = [t for t in flags if t != src]
        e = wr.Entry("t", "core/t.c", self.tmp, "gcc", flags, src)
        res = {"t": wr.measure(e)}
        after, contrib, _, errors, stale, n = wr.rows_of([e], res, self.tmp)
        # The unused local is a front-end row; the unused static, reported
        # "At top level" after code generation, a middle-end row with no
        # enclosing function.
        self.assertEqual(set(after), {
            ("core/t.c", "-Wunused-variable", "fe", "unused variable 'unused'"),
            ("core/t.c", "-Wunused-function", "me", "-")})
        self.assertFalse(errors or stale)


if __name__ == "__main__":
    unittest.main()
