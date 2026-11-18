#!/usr/bin/env python3
"""Tests for the knob read-path fence (knob_read_path.py).

SplitTest: a registered variable read by getenv in one file and by
openpete_config_env in another is split, including through a function or a
macro that wraps either reader, and through a wrapper of a wrapper.
CleanTest: one family, a literal in a comment, a literal outside a reader
call and an unregistered name are not split. TreeTest: the checkout passes
and engages (registered names and read sites both nonzero).

Run: python3 scripts/fences/tests/test_knob_read_path.py
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import knob_read_path  # noqa: E402

REGISTRY = ('static const knob_desc_t k[] = {\n'
            '    { .id = "a", .env = "OPENPETE_A" },\n'
            '    ENVROW_LIVE("B", "Debug", "", "b"),\n'
            '};\n')


def tree(files):
    """Return a temporary root holding the fixture registry and the given {path: text} sources."""
    d = tempfile.mkdtemp(prefix="knob_read_path_")
    files = dict(files)
    files.setdefault("core/config/knob_registry.c", REGISTRY)
    for rel, text in files.items():
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)
    return d


def split(files):
    return knob_read_path.scan(tree(files))[2]


CONFIG_READ = 'void c(void) { const char* v = openpete_config_env("OPENPETE_A"); (void)v; }\n'


class SplitTest(unittest.TestCase):
    def test_getenv_and_config_env(self):
        self.assertEqual(split({"core/x.c": 'int r(void) { return getenv("OPENPETE_A") != 0; }\n',
                                "games/common/y.cpp": CONFIG_READ}), ["OPENPETE_A"])

    def test_envrow_name(self):
        self.assertEqual(split({"core/x.c": 'int r(void) { return getenv("OPENPETE_B") != 0; }\n'
                                            'int s(void) { return rhi_knob("OPENPETE_B") != 0; }\n'}),
                         ["OPENPETE_B"])

    def test_raw_macro_wrapper(self):
        macro = ('#define ENV_FLAG_LATCH(slot, var) \\\n'
                 '    do { const char* e_ = getenv(var); (slot) = e_ != 0; } while (0)\n'
                 'int r(void) { static int s = -1; ENV_FLAG_LATCH(s, "OPENPETE_A"); return s; }\n')
        self.assertEqual(split({"core/x.c": macro, "core/y.c": CONFIG_READ}), ["OPENPETE_A"])

    def test_wrapper_of_wrapper(self):
        src = ('static const char* raw1(const char* name) { return getenv(name); }\n'
               'static int raw2(const char* var, int d) { const char* e = raw1(var); return e ? 1 : d; }\n'
               'int r(void) { return raw2("OPENPETE_A", 0); }\n')
        self.assertEqual(split({"core/x.c": src, "core/y.c": CONFIG_READ}), ["OPENPETE_A"])

    def test_config_wrapper_against_raw(self):
        src = ('static int env_value(const char* name, const char** out) {\n'
               '    const char* e = openpete_config_env(name); *out = e; return e != 0; }\n'
               'int c(const char** o) { return env_value("OPENPETE_A", o); }\n')
        self.assertEqual(split({"core/x.c": src,
                                "core/y.c": 'int r(void) { return getenv("OPENPETE_A") != 0; }\n'}),
                         ["OPENPETE_A"])


class CleanTest(unittest.TestCase):
    def test_one_family(self):
        self.assertEqual(split({"core/x.c": CONFIG_READ, "core/y.c": CONFIG_READ.replace("c(", "d(")}), [])

    def test_comment_is_not_a_read(self):
        self.assertEqual(split({"core/x.c": CONFIG_READ + '/* getenv("OPENPETE_A") */\n'
                                                          '// getenv("OPENPETE_A")\n'}), [])

    def test_non_reader_call(self):
        self.assertEqual(split({"core/x.c": CONFIG_READ + 'void l(void) { log_line("OPENPETE_A"); }\n'}), [])

    def test_unregistered_name(self):
        self.assertEqual(split({"core/x.c": 'int r(void) { return getenv("OPENPETE_Z") != 0; }\n'
                                            'int c(void) { return openpete_config_env("OPENPETE_Z") != 0; }\n'}),
                         [])

    def test_decomp_checkout_skipped(self):
        self.assertEqual(split({"core/x.c": CONFIG_READ,
                                "games/spyro-1/spyro-1/z.c": 'int r(void) { return getenv("OPENPETE_A") != 0; }\n'}),
                         [])


class TreeTest(unittest.TestCase):
    def test_checkout_passes_and_engages(self):
        root = os.path.dirname(os.path.dirname(FENCES))
        names, sites, _ = knob_read_path.scan(root)
        self.assertGreater(len(names), 100)
        self.assertGreater(sum(len(s) for s in sites.values()), 100)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            rc = knob_read_path.main(["--root", root])
        self.assertEqual(rc, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
