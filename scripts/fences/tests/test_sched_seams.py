#!/usr/bin/env python3
"""Tests for the present-thread harness's upkeep fence (sched_seams.py).

Fixtures that must refuse: an unwrapped SDL_TryLockMutex and a wrapped call
with no __wrap_ definition (check 1); an atomic the harness does not know, a
watched atomic with no registration, an excluded atomic the harness watches
and a stale row (check 2, parsed by libclang); a reordered and a re-guarded
compose bracket (check 4); a source seam token (check 5); an unlisted
handoff client and a stale client row (check 6). The clean form of each
passes.

Run: python3 scripts/fences/tests/test_sched_seams.py
Exit 64 when the libclang bindings are missing.
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import os
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import sched_seams as f  # noqa: E402
import ratchet_lib as lib  # noqa: E402


def parse(text, name="fx.c"):
    """Parse a C fixture with libclang; -> TranslationUnit (the file is kept until exit)."""
    d = tempfile.mkdtemp(prefix="sched-seams-test-")
    path = os.path.join(d, name)
    with open(path, "w") as fh:
        fh.write(text)
    return lib.parse_tu(path, ["-std=c11"])


class Wraps(unittest.TestCase):
    def test_unwrapped_trylock_refused(self):
        r = f.check_wraps({"pt.o": {"SDL_LockMutex", "SDL_TryLockMutex", "memcpy"}},
                          {"__wrap_SDL_LockMutex"}, {"SDL_LockMutex"})
        self.assertEqual(len(r), 1)
        self.assertIn("SDL_TryLockMutex", r[0])

    def test_wrap_without_definition_refused(self):
        r = f.check_wraps({"pt.o": {"SDL_LockMutex"}}, set(), {"SDL_LockMutex"})
        self.assertEqual(len(r), 1)
        self.assertIn("no __wrap_SDL_LockMutex", r[0])

    def test_stub_and_wrap_pass(self):
        self.assertEqual(f.check_wraps({"pt.o": {"SDL_LockMutex", "op_now", "pthread_self"}},
                                       {"__wrap_SDL_LockMutex", "op_now", "__wrap_pthread_self"},
                                       {"SDL_LockMutex", "pthread_self"}), [])


SRC = ("struct { int plain; _Atomic int known; _Atomic int watched; } s_x;\n"
       "_Atomic int s_new;\n"
       "int f(void) { return s_x.known + s_x.watched + s_new + s_x.plain; }\n")


class Census(unittest.TestCase):
    def setUp(self):
        self.found = f.atomic_census(parse(SRC))

    def test_census_reads_atomics_only(self):
        self.assertEqual(sorted(self.found), ["s_new", "s_x.known", "s_x.watched"])

    def test_unseamed_atomic_refused(self):
        rows = {"s_x.known": ("excluded", "statistic"), "s_x.watched": ("watched", "sched_watch(&s_x,")}
        harness = "sched_watch(&s_x, 1); sched_watch_exclude(&s_x.known, 4);"
        r = f.check_census("fx.c", self.found, rows, harness)
        self.assertEqual(len(r), 1)
        self.assertIn("s_new is unknown", r[0])

    def test_registration_rules(self):
        rows = {"s_x.known": ("excluded", "statistic"), "s_x.watched": ("watched", "sched_watch(&s_x,"),
                "s_new": ("once", "cache"), "s_gone": ("once", "cache")}
        r = f.check_census("fx.c", self.found, rows, "sched_watch_exclude(&s_x.watched, 4);")
        text = "\n".join(r)
        self.assertIn("s_x.watched is classed watched but the harness excludes it", text)
        self.assertIn("s_x.known is classed excluded", text)
        self.assertIn("s_gone matches no access", text)
        self.assertEqual(len(r), 3)


PIN = ("int rhi_begin_frame(int); void rhi_blit_to_swapchain(int); void rhi_end_frame(int);\n"
       "void openpete_native_scene_retire(int); int s_last_frame_presented, s_stage_target, live;\n"
       "static void %s(void) {\n"
       "    s_last_frame_presented = 0;\n"
       "    if (!rhi_begin_frame(0)) return;\n"
       "    openpete_native_scene_retire(1);\n"
       "%s"
       "    s_last_frame_presented = 1;\n}\n")
BRACKET = "    if (s_stage_target < 0) rhi_blit_to_swapchain(0);\n    rhi_end_frame(0);\n"


def events(name, body):
    tu = parse(PIN % (name, body))
    return f.pin_events(f.find_definition(tu, name, "fx.c"))


class Pin(unittest.TestCase):
    def test_same_passes(self):
        self.assertEqual(f.check_pin(events("a", BRACKET), events("b", BRACKET), {}), [])

    def test_reordered_bracket_refused(self):
        swapped = "    rhi_end_frame(0);\n    if (s_stage_target < 0) rhi_blit_to_swapchain(0);\n"
        r = f.check_pin(events("a", BRACKET), events("b", swapped), {})
        self.assertEqual(len(r), 1)
        self.assertIn("rhi_end_frame", r[0])

    def test_changed_guard_refused_and_substitution(self):
        guarded = "    if (live) rhi_blit_to_swapchain(0);\n    rhi_end_frame(0);\n"
        self.assertEqual(len(f.check_pin(events("a", BRACKET), events("b", guarded), {})), 1)
        self.assertEqual(f.check_pin(events("a", BRACKET), events("b", guarded), {"s_stage_target < 0": "live"}), [])

    def test_early_return_pinned(self):
        evs = events("a", BRACKET)
        self.assertIn(("return", (("if", "! rhi_begin_frame ( 0 )"),)), evs)


class Tokens(unittest.TestCase):
    def test_source_seam_refused(self):
        r = f.check_tokens({"core/runtime/present_thread.c": "int a = OP_SCHED_ATOMIC(s_pt.active);\n"})
        self.assertEqual(len(r), 1)
        self.assertEqual(f.check_tokens({"x.c": "int a = s_pt.active; /* sched: owned */\n"}), [])


class Clients(unittest.TestCase):
    def tree(self, files):
        root = tempfile.mkdtemp(prefix="sched-seams-tree-")
        for rel, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            with open(os.path.join(root, rel), "w") as fh:
                fh.write(text)
        return root

    def test_unlisted_client_refused(self):
        files = {rel: "openpete_present_thread_borrow();\n" for rel in f.CLIENTS if rel.endswith(".c")}
        files.update({"core/rhi/smoketest.c": "", "core/runtime/eager.h": "openpete_present_thread_set_active(0);\n",
                      "core/runtime/new_client.c": "int w = openpete_present_thread_borrow();\n"})
        r = f.check_clients(self.tree(files), {"core/rhi/smoketest.c"})
        self.assertEqual(len(r), 1)
        self.assertIn("core/runtime/new_client.c", r[0])

    def test_stale_client_refused(self):
        r = f.check_clients(self.tree({"core/runtime/eager.c": ""}), set())
        self.assertTrue(any("stale row" in x for x in r))


if __name__ == "__main__":
    try:
        lib.libclang()
    except lib.Refusal as e:
        print("test_sched_seams: %s" % e)
        sys.exit(64)
    unittest.main()
