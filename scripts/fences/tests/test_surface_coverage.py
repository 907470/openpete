#!/usr/bin/env python3
"""Tests for the docs coverage fence (surface_coverage.py).

Captures each built game's surface dump and --help once, then runs the
check over mutated copies: one negative control per refusal kind (a release
flag with no docs entry, a value-taking release row with no default, a
displayed menu row, a key action, a config directive), plus a menu hint
outside the glyph set, a --help definition the dump lacks, and a dump with
no key action (VACUOUS). Each control must exit 1 with its refusal line,
which the unmutated run must not print. AudienceTest pins the derived
audience of the flags --help prints in its debug block while their
registry rows sit in the release block.

Run: python3 scripts/fences/tests/test_surface_coverage.py [--build-dir DIR]
Exit 64 (skip) when no game binary is built.
"""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

FENCES = Path(__file__).resolve().parent.parent
ROOT = FENCES.parent.parent
sys.path.insert(0, str(FENCES))
import surface_coverage as sc                                # noqa: E402

SCRIPT = FENCES / "surface_coverage.py"
BUILD = os.environ.get("SURFACE_COVERAGE_BUILD_DIR",
                       str(ROOT / "build" / "linux-release"))
GAME = "spyro-1"
STATE = {}


def run_check(in_dir):
    r = subprocess.run([sys.executable, str(SCRIPT), "check", "--input-dir",
                        str(in_dir)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def setUpModule():
    tmp = Path(tempfile.mkdtemp(prefix="surface-coverage-test-"))
    STATE["tmp"] = tmp
    games = sc.capture(BUILD, tmp / "base")
    if GAME not in games:
        raise unittest.SkipTest(f"no {GAME} binary under {BUILD}")
    STATE["base_rc"], STATE["base_out"] = run_check(tmp / "base")


def tearDownModule():
    if "tmp" in STATE:
        shutil.rmtree(STATE["tmp"], ignore_errors=True)


class ControlTest(unittest.TestCase):
    """Each mutation must be refused with its own line."""

    def mutate(self, name, dump_fn=None, help_fn=None):
        d = STATE["tmp"] / name
        shutil.copytree(STATE["tmp"] / "base", d)
        jp, hp = d / f"{GAME}.json", d / f"{GAME}.help"
        if dump_fn:
            dump = json.loads(jp.read_text())
            dump_fn(dump)
            jp.write_text(json.dumps(dump))
        if help_fn:
            hp.write_text(help_fn(hp.read_text()))
        return run_check(d)

    def assert_refused(self, rc, out, needle):
        self.assertEqual(rc, 1, out)
        self.assertIn(needle, out)
        self.assertNotIn(needle, STATE["base_out"])
        line = next(x for x in out.splitlines() if needle in x)
        print(f"\n  control: {line}", file=sys.stderr)

    def test_release_flag_without_entry(self):
        def dump_fn(d):
            d["entries"].append({"id": "flag:--zz-selftest",
                                 "flag": "--zz-selftest", "takes_arg": "none",
                                 "block": "release", "build_cond": None,
                                 "def": None, "default_note": None})

        def help_fn(h):
            return h.replace("  --no-log-file ",
                             "  --zz-selftest              selftest flag\n"
                             "  --no-log-file ", 1)
        rc, out = self.mutate("flag", dump_fn, help_fn)
        self.assert_refused(rc, out, "[entry] spyro-1: flag:--zz-selftest "
                            "is release")

    def test_release_row_without_default(self):
        def dump_fn(d):
            for e in d["entries"]:
                if e["id"] == "knob:fullscreen":
                    e["def"] = None
                    e["default_note"] = None
        rc, out = self.mutate("default", dump_fn)
        self.assert_refused(rc, out, "[default] spyro-1: knob:fullscreen is a "
                            "release row and declares neither")

    def test_menu_row_without_entry(self):
        def dump_fn(d):
            d["entries"].append({"id": "menu:menu.zz_selftest", "knob": None,
                                 "hint": "SELFTEST", "displayed": True})
        rc, out = self.mutate("menu", dump_fn)
        self.assert_refused(rc, out, "[entry] spyro-1: menu:menu.zz_selftest "
                            "is release")

    def test_key_action_without_entry(self):
        def dump_fn(d):
            d["entries"].append({"id": "key:host.zz_selftest",
                                 "domain": "host", "kind": "digital",
                                 "dev": False, "devbuild": False,
                                 "def": "F13", "suggested": "F13"})
        rc, out = self.mutate("key", dump_fn)
        self.assert_refused(rc, out, "[entry] spyro-1: key:host.zz_selftest "
                            "is release")

    def test_directive_without_entry(self):
        def dump_fn(d):
            d["entries"].append({"id": "directive:zz_selftest"})
        rc, out = self.mutate("directive", dump_fn)
        self.assert_refused(rc, out, "[entry] spyro-1: directive:zz_selftest "
                            "is release")

    def test_hint_outside_glyph_set(self):
        def dump_fn(d):
            for e in d["entries"]:
                if e["id"] == "menu:menu.display":
                    e["hint"] = "WINDOW, OR FULLSCREEN"
        rc, out = self.mutate("hint", dump_fn)
        self.assert_refused(rc, out, "[hint] spyro-1: menu:menu.display")

    def test_help_definition_without_dump_flag(self):
        def help_fn(h):
            return h.replace("  --headless ", "  --zz-help-only             "
                             "selftest\n  --headless ", 1)
        rc, out = self.mutate("help", help_fn=help_fn)
        self.assert_refused(rc, out, "[help] spyro-1: --help defines "
                            "--zz-help-only")

    def test_no_key_action_is_vacuous(self):
        def dump_fn(d):
            d["entries"] = [e for e in d["entries"]
                            if not e["id"].startswith("key:")]
        rc, out = self.mutate("vacuous", dump_fn)
        self.assert_refused(rc, out, "VACUOUS: spyro-1:")


class AudienceTest(unittest.TestCase):
    """The derived audience of the rows outside the debug block that only
    the dev overlay shows besides their flag, of every registry flag's
    --help block, and of the pass-through env row."""

    def setUp(self):
        base = STATE["tmp"] / "base"
        dump = json.loads((base / f"{GAME}.json").read_text())
        self.g = sc.Game(GAME, dump, (base / f"{GAME}.help").read_text(),
                         sc.Shipped())

    def reason(self, ident):
        return self.g.release_reason(self.g.idx[ident])

    def test_overlay_rows_flags_print_release(self):
        for flag, knob in (("--wireframe", "wireframe"),
                           ("--freecam-pose", "freecam_pose"),
                           ("--player-pose", "player_pose"),
                           ("--notex", "notex")):
            self.assertEqual(self.g.defs.get(flag), "release")
            self.assertIn("--help", self.reason("flag:" + flag), flag)
            self.assertIn("release flag", self.reason("knob:" + knob), knob)

    def test_row_flag_help_block_is_row_block(self):
        n = 0
        for e in self.g.entries:
            if not e["id"].startswith("flag:") or "alias_value" in e \
                    or not e.get("knob"):
                continue
            k = self.g.idx["knob:" + e["knob"]]
            want = "debug" if k["block"] == "debug" else \
                "libav" if k.get("build_cond") == "OPENPETE_HAS_LIBAV" \
                else "release"
            self.assertEqual(self.g.defs.get(e["flag"]), want, e["flag"])
            n += 1
        self.assertGreater(n, 0)

    def test_passthrough_env_row_not_release(self):
        self.assertIsNone(self.reason("knob:TERRAIN_HDFOLD"))

    def test_km_dev_key_not_release(self):
        self.assertIsNone(self.reason("key:host.camlock"))
        self.assertIn("release flag", self.reason("knob:mute"))


if __name__ == "__main__":
    if "--build-dir" in sys.argv:
        i = sys.argv.index("--build-dir")
        BUILD = sys.argv[i + 1]
        del sys.argv[i:i + 2]
    prog = unittest.main(exit=False, verbosity=2)
    res = prog.result
    if res.testsRun and len(res.skipped) == res.testsRun:
        sys.exit(64)
    sys.exit(0 if res.wasSuccessful() else 1)
