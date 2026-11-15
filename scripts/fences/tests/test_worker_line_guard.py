#!/usr/bin/env python3
"""Tests for the per-worker cache-line fence (worker_line_guard.py).

Covers what counts as a per-worker array (a dimension naming a WORKERS or
THREADS macro, or `[worker]` indexing in the file), the element rule (a type
checked by OPENPETE_WORKER_ELEM passes, an unchecked type, a pointer and a
builtin scalar are refused), the marker escape, and that comments neither
declare an array nor check a type.

Run: python3 scripts/fences/tests/test_worker_line_guard.py
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import os
import sys
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import worker_line_guard as g  # noqa: E402


def refusals(text):
    """Refusal lines of one fixture source, with its own checked types."""
    return g.scan("x.c", text, g.elem_names(text))


class WorkerLineTest(unittest.TestCase):
    def test_checked_type_passes(self):
        src = ("typedef struct { OPENPETE_WORKER_ALIGN int n; } ws;\n"
               "OPENPETE_WORKER_ELEM(ws);\n"
               "static ws s_ws[MN_MAX_WORKERS];\n")
        self.assertEqual(refusals(src), [])

    def test_unchecked_type_refused(self):
        src = "typedef struct { int n; } ws;\nstatic ws s_ws[TN_MAX_WORKERS + 1];\n"
        r = refusals(src)
        self.assertEqual(len(r), 1)
        self.assertIn("x.c:2: s_ws[]", r[0])

    def test_worker_index_makes_per_worker(self):
        src = ("typedef struct { int n; } ws;\nstatic ws s_slot[8];\n"
               "void f(int item, int worker) { s_slot[worker].n++; }\n")
        self.assertEqual(len(refusals(src)), 1)

    def test_pointer_and_scalar_refused(self):
        src = "static SDL_Thread* s_t[WP_MAX_THREADS];\nstatic int s_n[POOL_MAX_WORKERS];\n"
        self.assertEqual(len(refusals(src)), 2)

    def test_marker_exempts(self):
        src = "static SDL_Thread* s_t[WP_MAX_THREADS];   /* worker-line: written at spawn only */\n"
        self.assertEqual(refusals(src), [])

    def test_comments_ignored(self):
        src = ("/* static ws s_ws[MN_MAX_WORKERS]; */\n"
               "typedef struct { int n; } ws;\n// OPENPETE_WORKER_ELEM(ws);\n"
               "static ws s_ws[MN_MAX_WORKERS];\n")
        r = refusals(src)
        self.assertEqual(len(r), 1)
        self.assertIn("x.c:4:", r[0])

    def test_other_arrays_ignored(self):
        src = "static int s_vrun0[1024];\nstatic float s_ub[6];\nint x = s_vrun0[w];\n"
        self.assertEqual(refusals(src), [])


if __name__ == "__main__":
    unittest.main()
