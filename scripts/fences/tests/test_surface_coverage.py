#!/usr/bin/env python3
"""Tests for the docs coverage fence (surface_coverage.py).

Captures each built game's surface dump and --help once, then runs the
check over mutated copies: one negative control per refusal kind (a release
flag with no docs entry, a value-taking release row with no default, a
displayed menu row, a key action, a config directive), plus a menu hint
outside the glyph set, a --help definition the dump lacks, and a dump with
no key action (VACUOUS). Each control must exit 1 with its refusal line,
which the unmutated run must not print. SourceLegTest runs the error id
and mod.toml key legs on the tree and on planted mismatches of the source
text; it needs no binary. AudienceTest pins the derived
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
        STATE["skip"] = f"no {GAME} binary under {BUILD}"
        return
    STATE["base_rc"], STATE["base_out"] = run_check(tmp / "base")


class NeedsBinary(unittest.TestCase):
    """Skipped when no game binary was captured."""

    @classmethod
    def setUpClass(cls):
        if "skip" in STATE:
            raise unittest.SkipTest(STATE["skip"])


def tearDownModule():
    if "tmp" in STATE:
        shutil.rmtree(STATE["tmp"], ignore_errors=True)


class ControlTest(NeedsBinary):
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


class AudienceTest(NeedsBinary):
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


class SourceLegTest(unittest.TestCase):
    """The error id and mod.toml key legs over the tree, then one planted
    mismatch per refusal on a copy of the source text."""

    PAGE = "user/help/troubleshooting.md"

    def setUp(self):
        self.h = sc.ERROR_IDS_H.read_text()
        self.p = sc.TROUBLESHOOTING_MD.read_text()
        self.loader = sc.MOD_LOADER_C.read_text()
        self.pages = {f.name: f.read_text()
                      for f in sorted(sc.MODDING_DIR.glob("*.md"))}

    def errid(self, h=None, p=None):
        f, n = sc.leg_error_ids(h or self.h, p or self.p, self.PAGE)
        return [x.text for x in f], n

    def refused(self, texts, needle):
        self.assertTrue(any(needle in t for t in texts), texts)
        line = next(t for t in texts if needle in t)
        print(f"\n  control: {line}", file=sys.stderr)

    def test_tree_agrees(self):
        f, n = self.errid()
        self.assertEqual(f, [])
        self.assertGreater(n, 0)
        f, n = sc.leg_mod_keys(self.loader, self.pages)
        self.assertEqual([x.text for x in f], [])
        self.assertGreater(n, 0)

    def test_code_id_without_page_row(self):
        h = self.h.replace('X(CRASH,', 'X(ZZ_SELFTEST, "OP-E-ZZ-SELFTEST", '
                           '"op-e-zz-selftest") \\\n    X(CRASH,', 1)
        self.refused(self.errid(h=h)[0], "OP-E-ZZ-SELFTEST (openpete_error_ids"
                     ".h ZZ_SELFTEST) has no row")

    def test_page_row_without_code_id(self):
        h = "\n".join(x for x in self.h.splitlines()
                      if '"OP-E-HANG"' not in x)
        self.refused(self.errid(h=h)[0], "OP-E-HANG has a row but no "
                     "OPENPETE_ERROR_IDS row")

    def test_page_anchor_differs(self):
        p = self.p.replace('<a id="op-e-hang"></a>', '<a id="op-e-hung"></a>', 1)
        self.refused(self.errid(p=p)[0], "OP-E-HANG is anchored 'op-e-hung'")

    def test_code_anchor_not_lower_id(self):
        h = self.h.replace('"OP-E-HANG",          "op-e-hang"',
                           '"OP-E-HANG",          "op-e-stall"', 1)
        self.refused(self.errid(h=h)[0], "HANG anchor 'op-e-stall'")

    def test_url_names_another_page(self):
        h = self.h.replace("user/help/troubleshooting.html",
                           "user/help/faq.html", 1)
        self.refused(self.errid(h=h)[0], "OPENPETE_TROUBLESHOOTING_URL names "
                     "user/help/faq.html")

    def test_undocumented_mod_key(self):
        loader = self.loader + ('\nstatic void zz(toml_table_t* t) '
                                '{ toml_bool_in(t, "zz_selftest_key"); }\n')
        f, _ = sc.leg_mod_keys(loader, self.pages)
        self.refused([x.text for x in f], "mod.toml key 'zz_selftest_key'")

    def test_undocumented_helper_key(self):
        loader = self.loader.replace('level_count_key(lt, "eggs"',
                                     'level_count_key(lt, "zz_eggs"', 1)
        f, _ = sc.leg_mod_keys(loader, self.pages)
        self.refused([x.text for x in f], "mod.toml key 'zz_eggs'")


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
