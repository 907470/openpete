#!/usr/bin/env python3
"""test_bugreport_replay_unpack.py - --bug-report-replay's bundle unpack and scratch reset on the engine.

Each case writes a synthetic bundle (python zipfile, no recording), runs
`openpete --bug-report-replay <bundle>` with OPENPETE_BUGREPORT_REPLAY_DIR
naming a scratch directory and no --disc, and reads the refusal line
(`bug-report-replay: REFUSED <reason>`, exit 64). The replay refuses before
any game starts, so no disc and no game process are needed.

Control: a well-formed bundle unpacks (every member lands in scratch) and is
refused only for the missing --disc.

Bounds (the reader's in tools/bugreport.py): a member whose zip entry
declares more than 1 GiB, listed members declaring more than 8 GiB in all,
and an archive of more than 100000 entries are refused before anything is
extracted; the forged sizes are written into the local and central headers
of a small entry, so nothing large is allocated by the test.

Duplicates: two zip entries of one name (compared ignoring case, as miniz's
lookup does) and one name listed twice in manifest.json are refused.

Directory bundles: a member that is a symlink, and a member under a
symlinked directory, are refused.

Owned names: a member under sandbox/ (the replay's own sandbox root inside
the scratch directory) is refused and nothing is written there.

Scratch reset: a symlink to a sentinel directory planted in a marked scratch
directory is removed and the sentinel's file survives.

Every case runs in its own sandbox root (scripts/lib/hostenv.py
sandbox_run_root), which also holds the case's scratch directory and
bundle.

Run: python3 -I tests/harness/test_bugreport_replay_unpack.py --build-dir build/linux-release
(exit 0 = every case passes, 64 = no binary).
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "scripts" / "lib"))
import hostenv  # noqa: E402

os.environ["OPENPETE_VIBRATION"] = "0"
BIN = None
MARKER = ".openpete-bugreport-replay"


def members():
    return {
        "anchor.state": b"A" * 64,
        "press.state": b"P" * 64,
        "segment.bk2": b"S" * 32,
        "hashes.csv": b"frame,vbl,total\n1,1,1\n",
    }


def manifest(mem, names=None):
    names = list(mem) if names is None else names
    return {
        "schema": 1, "game": "spyro-1", "disc": "SCUS-94228.cue", "disc_sha256": "22" * 32,
        "argv": ["openpete", "--headless"],
        "anchor": {"frame": 10, "vbl": 20, "feed_key": 10, "consume": 0},
        "press": {"frame": 20, "vbl": 40, "feed_key": 20},
        "members": [{"name": n, "size": len(mem[n]), "sha256": hashlib.sha256(mem[n]).hexdigest()}
                    for n in names],
    }


def write_zip(path, mem, man=None, extra=()):
    man = manifest(mem) if man is None else man
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(man))
        for n, d in mem.items():
            z.writestr(n, d)
        for n, d in extra:
            z.writestr(n, d)
    return path


def forge_size(path, name, size):
    """Rewrite one entry's uncompressed size in its local and central headers."""
    with zipfile.ZipFile(path) as z:
        off = z.getinfo(name).header_offset
    data = bytearray(Path(path).read_bytes())
    assert data[off:off + 4] == b"PK\x03\x04"
    struct.pack_into("<I", data, off + 22, size)
    enc = name.encode()
    i = 0
    hits = 0
    while True:
        i = data.find(b"PK\x01\x02", i)
        if i < 0:
            break
        nlen = struct.unpack_from("<H", data, i + 28)[0]
        if bytes(data[i + 46:i + 46 + nlen]) == enc:
            struct.pack_into("<I", data, i + 24, size)
            hits += 1
        i += 4
    assert hits == 1
    Path(path).write_bytes(bytes(data))


class Unpack(unittest.TestCase):
    def setUp(self):
        self.root = hostenv.sandbox_run_root()
        self.d = Path(tempfile.mkdtemp(prefix="case.", dir=self.root))
        self.scratch = self.d / "scratch"

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def replay(self, bundle):
        env = hostenv.child_env({"OPENPETE_VIBRATION": "0", "OPENPETE_SANDBOX": self.root,
                                 "OPENPETE_CARD_DIR": os.path.join(self.root, "cards"),
                                 "OPENPETE_BUGREPORT_REPLAY_DIR": str(self.scratch)})
        r = subprocess.run([BIN, "--bug-report-replay", str(bundle)], capture_output=True, text=True,
                           timeout=60, env=env)
        refused = [l for l in r.stderr.splitlines() if "bug-report-replay: REFUSED" in l]
        return r.returncode, refused[0] if refused else "", r.stderr

    def assert_refused(self, bundle, words):
        rc, line, err = self.replay(bundle)
        self.assertEqual(rc, 64, err)
        self.assertIn(words, line, err)
        return line

    def test_control_unpacks(self):
        mem = members()
        b = write_zip(self.d / "ok.zip", mem)
        self.assert_refused(b, "pass --disc")
        for n, d in mem.items():
            self.assertEqual((self.scratch / n).read_bytes(), d)

    def test_member_over_bound(self):
        b = write_zip(self.d / "big.zip", members())
        forge_size(b, "anchor.state", 0xFFFFFFF0)
        self.assert_refused(b, "over the member bound")
        self.assertFalse((self.scratch / "anchor.state").exists())

    def test_total_over_bound(self):
        mem = members()
        mem.update({f"pad{i}": bytes([i]) * 16 for i in range(9)})
        b = write_zip(self.d / "total.zip", mem)
        for i in range(9):
            forge_size(b, f"pad{i}", (1 << 30) - 1)
        self.assert_refused(b, "over the total bound")
        self.assertFalse((self.scratch / "anchor.state").exists())

    def test_count_over_bound(self):
        mem = members()
        b = write_zip(self.d / "count.zip", mem, extra=((f"x/{i}", b"") for i in range(100001)))
        self.assert_refused(b, "entries, over the bound")

    def test_duplicate_zip_entry(self):
        mem = members()
        b = write_zip(self.d / "dup.zip", mem, extra=(("ANCHOR.STATE", b"other"),))
        self.assert_refused(b, "duplicate")

    def test_duplicate_manifest_name(self):
        mem = members()
        b = write_zip(self.d / "dupm.zip", mem, man=manifest(mem, list(mem) + ["anchor.state"]))
        self.assert_refused(b, "listed twice")

    def test_member_under_owned_sandbox(self):
        mem = members()
        mem["Sandbox/config/openpete/openpete.toml"] = b"[video]\n"
        b = write_zip(self.d / "owned.zip", mem)
        self.assert_refused(b, "sandbox/")
        self.assertFalse((self.scratch / "Sandbox").exists())
        self.assertFalse((self.scratch / "sandbox" / "config").exists())

    def write_dir(self, mem):
        bd = self.d / "bundle"
        bd.mkdir()
        (bd / "manifest.json").write_text(json.dumps(manifest(mem)))
        for n, d in mem.items():
            p = bd / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(d)
        return bd

    def test_directory_bundle_unpacks(self):
        mem = members()
        self.assert_refused(self.write_dir(mem), "pass --disc")
        self.assertEqual((self.scratch / "anchor.state").read_bytes(), mem["anchor.state"])

    def test_directory_member_symlink(self):
        mem = members()
        outside = self.d / "outside.bin"
        outside.write_bytes(mem["segment.bk2"])
        bd = self.write_dir(mem)
        (bd / "segment.bk2").unlink()
        (bd / "segment.bk2").symlink_to(outside)
        self.assert_refused(bd, "symbolic link")
        self.assertFalse((self.scratch / "segment.bk2").exists())

    def test_directory_component_symlink(self):
        mem = members()
        mem["mods/m1/mod.toml"] = b"id='m1'\n"
        outside = self.d / "outside"
        (outside / "m1").mkdir(parents=True)
        (outside / "m1" / "mod.toml").write_bytes(mem["mods/m1/mod.toml"])
        bd = self.write_dir(mem)
        (bd / "mods" / "m1" / "mod.toml").unlink()
        (bd / "mods" / "m1").rmdir()
        (bd / "mods").rmdir()
        (bd / "mods").symlink_to(outside)
        self.assert_refused(bd, "symbolic link")

    def test_scratch_reset_keeps_link_target(self):
        sentinel = self.d / "sentinel"
        sentinel.mkdir()
        (sentinel / "keep.txt").write_text("keep")
        self.scratch.mkdir()
        (self.scratch / MARKER).write_text("")
        (self.scratch / "old.txt").write_text("old")
        (self.scratch / "link").symlink_to(sentinel)
        (self.scratch / "sub").mkdir()
        (self.scratch / "sub" / "link2").symlink_to(sentinel)
        b = write_zip(self.d / "ok.zip", members())
        self.assert_refused(b, "pass --disc")
        self.assertEqual((sentinel / "keep.txt").read_text(), "keep")
        self.assertFalse(os.path.lexists(self.scratch / "link"))
        self.assertFalse(os.path.lexists(self.scratch / "sub"))
        self.assertFalse((self.scratch / "old.txt").exists())


def main():
    global BIN
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build/linux-release")
    a, rest = ap.parse_known_args()
    BIN = str(Path(a.build_dir).resolve() / "openpete")
    if not os.access(BIN, os.X_OK):
        print(f"SKIP: no binary at {BIN}")
        return 64
    r = unittest.main(argv=[sys.argv[0], *rest], exit=False, verbosity=2)
    return 0 if r.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
