#!/usr/bin/env python3
"""Tests for the recomp identity pair of scripts/presentation-blessed.py.

MapTest drives recomp_input_digest over a psx.map whose build path, address
width, debug-section sizes and section order vary, and over a moved symbol
(the calibration: a change the regen reads must move the digest). TreeTest
drives generated_tree_digest over a stale file, a changed output, a missing
output and an unregistered overlay source. ReblessTest runs
rebless-identity in a scratch repository: refused outside the capture image
(the image recipe file absent), refused on a recomp input that differs from
HEAD, and otherwise rewriting only the two identity columns. PromoteTest
runs promote in a scratch repository with a scratch store over synthetic
capture records: a local record and a record from another host promote
alike on the comparison surface, and the rewritten manifest names no host,
CPU or run id; a record whose source tree is that of an earlier first-parent
commit promotes at that commit, and one matching no ancestor is refused,
naming the nearest commit, with the store and manifest untouched.

Run: python3 scripts/fences/tests/test_presentation_identity.py
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    """Import a script whose file name is not a module name."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pb = load("presentation_blessed",
          os.path.join(ROOT, "scripts", "presentation-blessed.py"))

GAME = "spyro-1"
DECOMP = "games/spyro-1/spyro-1"

# Sections the regen parses (.text/.data per object) between lines it
# skips: the archive list, LOAD lines and a .debug_str record, each
# carrying the absolute directory the decomp was built in.
MAP_BLOCKS = [
    " .text          0x0000000080012204       0xa4 build/src/main.o\n"
    "                0x0000000080012204                main\n"
    "                0x0000000080012250                helper\n",
    " .text          0x00000000800163e4      0x3a0 build/src/cd.o\n"
    "                0x00000000800163e4                CDLoadTime\n"
    "                0x0000000080016490                CDReadDone\n",
    " .data          0x0000000080070000       0x10 build/src/cd.o\n"
    "                0x0000000080070000                g_Foo\n",
]


def map_text(build_dir="<spyro>/spyro-1", blocks=None,
             debug_size=0x120, width=16):
    """A psx.map: header lines naming `build_dir`, the section blocks, and
    a .debug_str record; addresses printed `width` hex digits wide."""
    body = "".join(blocks or MAP_BLOCKS)
    if width != 16:
        body = re.sub(r"0x([0-9a-f]{16})",
                      lambda m: "0x" + m.group(1)[16 - width:], body)
    return ("Archive member included to satisfy reference by file (symbol)\n"
            "\n%s/build/lib/libapi.a(a.o)\n"
            "                              build/src/main.o (foo)\n\n"
            "Linker script and memory map\n\n"
            "LOAD %s/build/src/main.o\n" % (build_dir, build_dir)
            + body +
            " .debug_str     0x%s   0x%x %s/build/src/main.o\n"
            % ("0" * width, debug_size, build_dir))


def write(path, data, mode="w"):
    """Write `data` to `path`, creating its directory."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as fh:
        fh.write(data)


def make_tree(top):
    """A checkout holding one game's regen inputs and outputs."""
    write(os.path.join(top, "games", GAME, "recomp.conf"),
          'DECOMP_DIR="%s"\nPSX_EXE="$DECOMP_DIR/build/PSX.EXE"\n'
          'WAD_DIR="$DECOMP_DIR/build/wad"\n'
          'CHANNEL_MANIFEST="games/spyro-1/manifest/render_channels.toml"\n'
          % DECOMP)
    os.makedirs(os.path.join(top, "tools"), exist_ok=True)
    shutil.copy(os.path.join(ROOT, "tools", "build_manifest.py"),
                os.path.join(top, "tools", "build_manifest.py"))
    d = os.path.join(top, DECOMP, "build")
    write(os.path.join(d, "psx.map"), map_text())
    write(os.path.join(d, "PSX.EXE"), b"PS-X EXE\0\1\2", "wb")
    write(os.path.join(d, "wad", "level_10.ovl"), b"\x05\0\0\0ovl", "wb")
    man = os.path.join(top, "games", GAME, "manifest")
    write(os.path.join(man, "render_channels.toml"), "# channels\n")
    for f in ("globals.toml", "symbols.toml"):
        write(os.path.join(man, f), "# Auto-generated\n# Source: "
              "games/spyro-1/spyro-1/build/psx.map\n\n[[x]]\naddr = 1\n")
    gen = os.path.join(top, "games", GAME, "generated")
    for f in ("PSX.EXE_full.c", "PSX.EXE_dispatch.c", "psyq_stubs.c",
              "channel_sites.txt"):
        write(os.path.join(gen, f), "/* %s */\n" % f)
    write(os.path.join(gen, "spyro_overlay_table.c"),
          '#include "spyro_overlay_table.h"\n'
          'extern int psx_dispatch_level_10(CPUState* cpu, uint32_t addr);\n'
          'const spyro_overlay_entry_t spyro_overlay_table[] = {\n'
          '    {   5, "level_10", psx_dispatch_level_10 },\n};\n')
    for f in ("level_10.bin_full.c", "level_10.bin_dispatch.c"):
        write(os.path.join(gen, "level_10", f), "/* %s */\n" % f)


class Scratch(unittest.TestCase):
    """A scratch checkout with repo_root() pointing at it."""

    def setUp(self):
        self.top = tempfile.mkdtemp(prefix="pb-identity-")
        make_tree(self.top)
        self.saved = pb.repo_root
        pb.repo_root = lambda: self.top

    def tearDown(self):
        pb.repo_root = self.saved
        shutil.rmtree(self.top)

    def path(self, rel):
        """Absolute path of a checkout-relative path."""
        return os.path.join(self.top, rel)


class MapTest(Scratch):
    def digest_with(self, **kw):
        """recomp_input_digest with psx.map rewritten by map_text(**kw)."""
        write(self.path(DECOMP + "/build/psx.map"), map_text(**kw))
        return pb.recomp_input_digest(GAME)

    def test_invariant_under_build_path(self):
        base = self.digest_with()
        self.assertEqual(base, self.digest_with(build_dir="/s1"))

    def test_invariant_under_address_width_and_debug_sizes(self):
        base = self.digest_with()
        self.assertEqual(base, self.digest_with(width=8, debug_size=0x168))

    def test_invariant_under_reordered_map(self):
        base = self.digest_with()
        self.assertEqual(base, self.digest_with(blocks=MAP_BLOCKS[::-1]))

    def test_invariant_under_toml_source_comment(self):
        base = pb.recomp_input_digest(GAME)
        p = self.path("games/%s/manifest/symbols.toml" % GAME)
        with open(p) as fh:
            text = fh.read()
        write(p, text.replace("games/spyro-1/spyro-1/build/psx.map",
                              "spyro-1/build/psx.map"))
        self.assertEqual(base, pb.recomp_input_digest(GAME))

    def test_moved_symbol_changes_digest(self):
        base = self.digest_with()
        moved = [MAP_BLOCKS[0].replace("0x0000000080012250",
                                       "0x0000000080012254")] + MAP_BLOCKS[1:]
        self.assertNotEqual(base, self.digest_with(blocks=moved))

    def test_changed_overlay_changes_digest(self):
        base = pb.recomp_input_digest(GAME)
        write(self.path(DECOMP + "/build/wad/level_10.ovl"),
              b"\x05\0\0\0ovm", "wb")
        self.assertNotEqual(base, pb.recomp_input_digest(GAME))

    def test_symbol_table_rows(self):
        text = pb.map_symbol_table(self.path(DECOMP + "/build/psx.map"))
        self.assertNotIn("/home/", text)
        self.assertNotIn("debug", text)
        self.assertIn("Y\t80012250\ttext\thelper\tbuild/src/main.o\n", text)
        self.assertIn("S\tdata\t80070000\t00000010\tbuild/src/cd.o\n", text)
        self.assertEqual(len(text.splitlines()), 3 + 5)


class TreeTest(Scratch):
    gen = "games/%s/generated/" % GAME

    def test_invariant_under_stale_file(self):
        base = pb.generated_tree_digest(GAME)
        write(self.path(self.gen + "overlay_fn_pointers.txt"), "stale\n")
        write(self.path(self.gen + "level_10/extra_funcs.combined.txt"),
              "0x8007AA38\n")
        write(self.path(self.gen + "_main_exe_pass1/PSX.EXE_full.c"), "x\n")
        self.assertEqual(base, pb.generated_tree_digest(GAME))

    def test_output_content_changes_digest(self):
        base = pb.generated_tree_digest(GAME)
        write(self.path(self.gen + "level_10/level_10.bin_full.c"), "/**/\n")
        self.assertNotEqual(base, pb.generated_tree_digest(GAME))

    def test_channel_sites_counted(self):
        base = pb.generated_tree_digest(GAME)
        write(self.path(self.gen + "channel_sites.txt"), "changed\n")
        self.assertNotEqual(base, pb.generated_tree_digest(GAME))

    def test_missing_output_refused(self):
        os.unlink(self.path(self.gen + "psyq_stubs.c"))
        with self.assertRaisesRegex(pb.IdentityError, "psyq_stubs.c"):
            pb.generated_tree_digest(GAME)

    def test_unregistered_overlay_source_refused(self):
        write(self.path(self.gen + "level_99/level_99.bin_full.c"), "x\n")
        with self.assertRaisesRegex(pb.IdentityError, "level_99"):
            pb.generated_tree_digest(GAME)


def run_git(top, *args):
    """git in the scratch checkout, quiet, failing loudly."""
    subprocess.run(["git", "-C", top] + list(args), check=True,
                   capture_output=True)


def commit_scratch(t):
    """Write the image recipe and .gitignore into Scratch `t`'s checkout
    and commit it; return (HEAD, recipe hash)."""
    t.recipe = os.path.join(t.top, "image-recipe")
    write(t.recipe, "FROM scratch\n")
    write(t.path(".gitignore"),
          "/image-recipe\n/games/*/generated/\n/games/*/*/build/\n"
          "/games/*/manifest/globals.toml\n"
          "/games/*/manifest/symbols.toml\n")
    run_git(t.top, "init", "-q")
    run_git(t.top, "add", ".")
    run_git(t.top, "-c", "user.name=t", "-c", "user.email=t@t",
            "commit", "-qm", "base")
    rev = subprocess.run(["git", "-C", t.top, "rev-parse", "HEAD"],
                         capture_output=True, text=True,
                         check=True).stdout.strip()
    return rev, hashlib.sha256(b"FROM scratch\n").hexdigest()


class ReblessTest(Scratch):
    OLD_IN, OLD_GEN = "a" * 64, "b" * 64

    def setUp(self):
        super().setUp()
        rev, recipe_hash = commit_scratch(self)
        m = pb.Manifest({"game": GAME, "tier": "presentation",
                         "recipe-hash": recipe_hash}, [
            dict(tile="t%d-vk.png" % i, digest=("%d" % i) * 64, revision=rev,
                 recomp_input=self.OLD_IN, generated_tree=self.OLD_GEN,
                 origin="mapped") for i in (1, 2)])
        self.man = self.path(pb.manifest_path(GAME, "presentation"))
        write(self.man, pb.format_manifest(m))
        self.saved_recipe = pb.IMAGE_RECIPE

    def tearDown(self):
        pb.IMAGE_RECIPE = self.saved_recipe
        super().tearDown()

    def rebless(self):
        """Run rebless-identity; (rc, manifest text after)."""
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            rc = pb.main(["rebless-identity", GAME])
        with open(self.man) as fh:
            return rc, fh.read()

    def test_refused_outside_image(self):
        with open(self.man) as fh:
            before = fh.read()
        pb.IMAGE_RECIPE = os.path.join(self.top, "no-such-recipe")
        self.assertEqual(self.rebless(), (1, before))

    def test_refused_on_other_recipe(self):
        with open(self.man) as fh:
            before = fh.read()
        other = os.path.join(self.top, "other-recipe")
        write(other, "FROM other\n")
        pb.IMAGE_RECIPE = other
        self.assertEqual(self.rebless(), (1, before))

    def test_refused_on_dirty_recomp_input(self):
        with open(self.man) as fh:
            before = fh.read()
        pb.IMAGE_RECIPE = self.recipe
        write(self.path("games/%s/manifest/render_channels.toml" % GAME),
              "# edited\n")
        self.assertEqual(self.rebless(), (1, before))

    def test_rewrites_only_identity_columns(self):
        with open(self.man) as fh:
            before = pb.parse_manifest(fh.read())
        pb.IMAGE_RECIPE = self.recipe
        rc, text = self.rebless()
        self.assertEqual(rc, 0)
        after = pb.parse_manifest(text)
        self.assertEqual(before.header, after.header)
        rin = pb.recomp_input_digest(GAME)
        gen = pb.generated_tree_digest(GAME)
        self.assertEqual(len(after.rows), 2)
        for b, r in zip(before.rows, after.rows):
            for c in ("tile", "digest", "revision", "origin"):
                self.assertEqual(b[c], r[c])
            self.assertEqual((r["recomp_input"], r["generated_tree"]),
                             (rin, gen))
            self.assertNotEqual(r["recomp_input"], self.OLD_IN)


class PromoteTest(Scratch):
    """promote of a local and of a fleet-pulled capture record."""
    HERE_HOST, REMOTE, CPU = "here-box", "remote-box", "Fake CPU 9000"
    FLEET_HOST = "fleethost-b"
    RUN = "20261010-120000-abcd"
    ENV = {"mesa": "Mesa 24.0.0", "device": "llvmpipe",
           "compiler": "cc 13", "preset": "linux-release-container",
           "disc-bin": "d" * 64, "disc-cue": "e" * 64,
           "image-digest": "sha256:" + "f" * 64, "libvulkan": "1.3.275"}

    def setUp(self):
        super().setUp()
        # source_tree adds every SOURCE_ROOTS path, so each must exist.
        for rel in ("core/a.c", "scripts/a.sh", "CMakeLists.txt",
                    "cmake/a.cmake"):
            write(self.path(rel), "# %s\n" % rel)
        self.rev, recipe_hash = commit_scratch(self)
        self.env = dict(self.ENV, **{"recipe-hash": recipe_hash})
        header = {"game": GAME, "tier": "presentation", "size": "4x4"}
        header.update(self.env)
        m = pb.Manifest(header, [dict(
            tile="t1-vk.png", digest="1" * 64, revision=self.rev,
            recomp_input="a" * 64, generated_tree="b" * 64,
            origin="mapped")])
        self.man = self.path(pb.manifest_path(GAME, "presentation"))
        write(self.man, pb.format_manifest(m))
        self.work = tempfile.mkdtemp(prefix="pb-promote-")
        self.cand = os.path.join(self.work, "cand")
        self.run_dir = os.path.join(self.work, "runs", self.RUN)
        os.makedirs(self.run_dir)
        Image.new("RGB", (4, 4), (9, 8, 7)).save(
            os.path.join(self.cand_dir(), "t1-vk.png"))
        write(os.path.join(self.cand, pb.DEVICE_NAME),
              "[rhi/vk] device='llvmpipe' api=1.3 driver-info='Mesa 24.0.0'\n")
        self.saved_recipe = pb.IMAGE_RECIPE
        pb.IMAGE_RECIPE = self.recipe
        self.saved_env = {k: os.environ.get(k) for k in
                          ("OPENPETE_CAPTURE_HOST", "OPENPETE_GOLDEN_REF_DIR")}
        os.environ["OPENPETE_CAPTURE_HOST"] = self.HERE_HOST
        os.environ["OPENPETE_GOLDEN_REF_DIR"] = os.path.join(self.work,
                                                             "store")

    def tearDown(self):
        pb.IMAGE_RECIPE = self.saved_recipe
        for k, v in self.saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.work)
        super().tearDown()

    def cand_dir(self):
        """The candidate directory, created."""
        os.makedirs(self.cand, exist_ok=True)
        return self.cand

    def record(self, host, **extra):
        """Write the candidate's capture record: this checkout's tree
        fields, the pinned environment, `host`, then `extra` (a None value
        drops the key)."""
        rec = {"game": GAME, "tier": "presentation"}
        rec.update(pb.tree_fields(GAME))
        rec.update(self.env)
        rec.update(host=host, cpu=self.CPU)
        rec.update(extra)
        write(os.path.join(self.cand, pb.RECORD_NAME), "".join(
            "%s=%s\n" % (k, v) for k, v in rec.items() if v is not None))

    def fleet(self, **extra):
        """A remote record stamped as a golden-fleet pull of FLEET_HOST."""
        self.record(self.REMOTE, **dict({"fleet-run": self.RUN,
                                          "fleet-host": self.FLEET_HOST,
                                          "fleet-run-dir": self.run_dir},
                                         **extra))

    def promote(self):
        """Run promote --all; (rc, manifest text, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = pb.main(["promote", GAME, self.cand, "--all"])
        with open(self.man) as fh:
            return rc, fh.read(), out.getvalue(), err.getvalue()

    def assert_promoted(self, res):
        """The row of t1 carries the candidate's digest, HEAD, origin
        `mapped`, and the tile is in the store."""
        rc, text, _out, err = res
        self.assertEqual(rc, 0, err)
        row = pb.parse_manifest(text).rows[0]
        dig = pb.digest_file(os.path.join(self.cand, "t1-vk.png"))[0]
        self.assertEqual((row["digest"], row["revision"], row["origin"]),
                         (dig, self.rev, "mapped"))
        self.assertTrue(os.path.isfile(pb.blob_path(dig)))

    def assert_refused(self, res, why):
        """Exit 1, the manifest unchanged, `why` in the refusal."""
        with open(self.man) as fh:
            before = fh.read()
        rc, text, _out, err = res
        self.assertEqual((rc, text), (1, before))
        self.assertIn(why, err)

    def test_local_record(self):
        self.record(self.HERE_HOST)
        self.assert_promoted(self.promote())

    def test_local_record_needs_no_fleet_fields(self):
        self.record("unrecorded")
        self.assert_promoted(self.promote())

    def test_remote_record_admitted(self):
        self.fleet()
        res = self.promote()
        self.assert_promoted(res)
        self.assertIn("fleet run %s, fleet host %s (host %s)"
                      % (self.RUN, self.FLEET_HOST, self.REMOTE), res[2])

    def test_remote_writes_no_machine_names(self):
        self.fleet()
        rc, text, _out, err = self.promote()
        self.assertEqual(rc, 0, err)
        for word in (self.REMOTE, self.CPU, self.RUN, "fleet",
                     self.FLEET_HOST, self.HERE_HOST, self.run_dir):
            self.assertNotIn(word, text)

    def commit(self, rel, text):
        """Write `rel` and commit it; return the new HEAD."""
        write(self.path(rel), text)
        run_git(self.top, "add", rel)
        run_git(self.top, "-c", "user.name=t", "-c", "user.email=t@t",
                "commit", "-qm", rel)
        return subprocess.run(["git", "-C", self.top, "rev-parse", "HEAD"],
                              capture_output=True, text=True,
                              check=True).stdout.strip()

    def test_ancestor_revision(self):
        self.fleet()
        self.commit("core/b.c", "/* after the capture */\n")
        self.assert_promoted(self.promote())

    def test_ancestor_newest_match_without_head(self):
        self.record(self.HERE_HOST, head="0" * 40)
        self.commit("tests/x.txt", "outside the source roots\n")
        top = self.commit("core/b.c", "/* after the capture */\n")
        rc, text, _out, err = self.promote()
        self.assertEqual(rc, 0, err)
        row = pb.parse_manifest(text).rows[0]
        self.assertNotIn(row["revision"], (self.rev, top))
        self.assertEqual(pb.git("rev-parse", "HEAD~1").strip(),
                         row["revision"])

    def test_no_ancestor_refused(self):
        write(self.path("core/x.c"), "/* never committed */\n")
        tree = pb.source_tree()
        os.unlink(self.path("core/x.c"))
        self.record(self.HERE_HOST, **{"source-tree": tree})
        top = self.commit("core/b.c", "/* after the capture */\n")
        self.assert_refused(self.promote(), "the nearest, %s, differs at "
                            "core/x.c (1 path(s))" % self.rev[:12])
        self.assertNotEqual(top, self.rev)
        self.assertEqual([], [f for _d, _s, fs in
                              os.walk(os.path.join(self.work, "store"))
                              for f in fs])

    def test_no_record_refused(self):
        self.assert_refused(self.promote(), "holds no %s" % pb.RECORD_NAME)

    def test_remote_device_mismatch(self):
        self.fleet()
        write(os.path.join(self.cand, pb.DEVICE_NAME),
              "[rhi/vk] device='other' api=1.3 driver-info='Mesa 99.0.0'\n")
        self.assert_refused(self.promote(), "capture environment is not the "
                            "pinned one")

    def test_remote_other_source_tree(self):
        self.fleet(**{"source-tree": "4b825dc642cb6eb9a060e54bf8d69288fbee4904"})
        self.assert_refused(self.promote(), "source tree")


if __name__ == "__main__":
    unittest.main(verbosity=2)
