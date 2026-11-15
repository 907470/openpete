#!/usr/bin/env python3
"""test_bugreport.py - unit rows for tools/bugreport.py.

Bundle access: a synthetic schema-1 bundle opens, every member verifies, and
`info` prints the manifest's identity, counters, taint, redaction, pace and
log fields and the replay route; a schema-2 bundle's members left out and
members shipped flagged are listed under the redaction line; `extract` writes every member with its
manifest hash; a member altered after the manifest was written fails `info`
and `extract` with exit 1.

Refusals (exit 64, a `refused:` message, no traceback): a truncated zip, a
non-zip, a `../` member, an absolute member, a backslash member, a missing
manifest, a manifest that does not parse, a duplicate member name.

diff-hashes: equal CSVs report IDENTICAL; a CSV with one region of one row
altered reports DIVERGED at that tick naming that region; a bundle argument
reads its hashes.csv; CSVs with no tick in common report VACUOUS.

Pace: present records 16.67 ms apart with one 50 ms gap give p50 16.67 ms
and one hitch.

Performance records: a synthetic pace image holding VBL, PRESENT, SHED,
TICK_SPLIT, EAGER_PLAN and GPU_FRAME records packed as pace_log.c packs them,
and a coarse image of three rows, decode to the values packed (split
microseconds and counts, plan level, standing level, slots, afford, planned
scenes and period, GPU frame span and busy time, coarse fields); the rollup
gives the present rate, the sim tick from the TICK_SPLIT records, sheds and
scenes per tick; TICK_SPLIT records of the exclusive layout decode to guest =
sim - host and host = terrain + moby + snap canonical + host other, and
`perf` prints that line, while a record of the earlier layout decodes with no
guest or host and `perf` names it; `perf` prints every record kind; `corpus` over a directory
holding a new bundle, a bundle with neither new member and a non-zip writes
one TSV row each, the old bundle with its pace rollups and empty record
columns, the non-zip with an error; a coarse image with a bad magic is
refused.

Thumbnails and note: 20 thumbnail PNGs with a 4.75 s span report count,
size, span, rate and "all decode"; a PNG with a broken chunk CRC is counted
as not decoding; no thumbnail member reports the manifest's state; notes.txt
is printed; no note reports the manifest's note state; a fatal-exit
trigger is named.

Sub-tick ring: a synthetic subtick.bin of three presents reports its
present and record counts in `info`; `subtick --class` lists the class and
its record's draws; a record drawn twice at different screen positions in
one present is flagged MULTI, a paired record drawn off its lerp OFFLERP,
and a record on its lerp neither; a ring with no flag exits 0, one with a
flag exits 2; an extracted subtick.bin reads like the bundle.

Audio feed: a synthetic audio-feed.txt of five events (a prime, a slip,
a stretch edge, an underrun and a periodic sample) gives `info` its mute,
device, event counts by kind, underruns and stretch engages; `audio` lists
the events with the ratio in cents, `--kind` and `--no-ticks` select, an
extracted audio-feed.txt reads like the bundle; a feed that played data
with no event held exits 3 (VACUOUS); a bundle without the member says so;
a row with a missing field is refused (exit 64); `corpus` fills the audio
columns.

Knobs: a synthetic knobs.txt with a renderer knob changed live, a sim knob
changed and back, a knob off its default and an unchanged one gives `info`
the live change on the line after the bundle line, with its time, tick, boot
value, default and class, the changed-and-back knob with its change count,
and the off-default knob, and leaves the unchanged one out; a bundle without
the member says so on the same line; `replay` prints the run's knob lines.

Replay: the fallback route runs a stub executable (a script printing the
flag's verdict line, not openpete) and passes its verdict and exit code
through. Archive lookup: EXE-SHA256SUMS lines parse into (hash, artifact,
inner path), malformed lines and unsafe names skipped; a tarball listed
there with the manifest's exe SHA-256 selects the primary route and replay
untars and runs its exe; an AppImage listed there (a stub answering
`--appimage-extract`) is extracted and its AppRun runs; the same artifact
with no EXE-SHA256SUMS line, or a listed hash its exe does not have, is not
used; OPENPETE_RELEASE_ARCHIVE_DIR names the archive when --archive is
absent.

Run: python3 tests/harness/test_bugreport.py (exit 0 = every case passes).
The game binary is named in expected output and never started
(sandbox-guard: no-launch).
"""
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

# The rows run stub executables only; a launch any row grows stays silent
# and input-locked.
os.environ["OPENPETE_VIBRATION"] = "0"
os.environ["OPENPETE_INPUT_LOCK"] = "1"
ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "bugreport.py"
_spec = importlib.util.spec_from_file_location("bugreport", TOOL)
br = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(br)

REGIONS = ["player", "mobys", "camera"]


def hash_csv(rows=range(100, 110), alter=None):
    out = ["frame,vbl,fnv1a64," + ",".join(REGIONS)]
    for f in rows:
        vals = [f"{(f * 7 + i) & 0xFFFFFFFF:016x}" for i in range(len(REGIONS))]
        total = f"{f * 13:016x}"
        if alter and alter[0] == f:
            vals[REGIONS.index(alter[1])] = "deadbeefdeadbeef"
            total = "0000000000000001"
        out.append(f"{f},{f * 2},{total}," + ",".join(vals))
    return ("\n".join(out) + "\n").encode()


def pace_ring(intervals_ms):
    env = b"build=test\n"
    hdr = struct.pack("<IIIIIIqqqqq", br.OPPL_MAGIC, 2, 64 + len(env), 64, len(env), 0,
                      0, 0, 2250000, 37609, 2)
    recs, t = [], 10**9
    for i, iv in enumerate([0] + intervals_ms):
        t += int(iv * 1e6)
        recs.append(struct.pack("<BBHIqqqqqqq", br.PACE_PRESENT, 0, 0, i, t, 0, 0, 0, 0, 0, 0))
    return hdr + env + b"".join(recs)


EXE_BYTES = b"#!/bin/sh\necho archived\n"


def members():
    return {
        "hashes.csv": hash_csv(),
        "summary.txt": b"game=spyro-1\nlevel=10 Artisans\nplayer=(1, 2, 3)\n",
        "module.txt": b"linkage=shared\nabi=7\nmodule_march=(unset)\n",
        "log.txt": b"  0.1 [info ] boot: ok\n  0.2 [warn ] cd: slow read\n  0.3 [error] gpu: lost\n",
        "pace.oppl": pace_ring([16.67] * 50 + [50.0] + [16.67] * 50),
        "device.txt": b"[system]\nplatform=Linux\nlogical_cpus=8\n",
        "card1.mcd": b"\0" * 128,
        "mods/m1/mod.toml": b"id='m1'\n",
    }


def manifest(mem, **over):
    m = {
        "schema": 1, "build_version": "v0.5.0-test", "build_tagged": 0, "build_commit": "abcdef123",
        "exe_sha256": hashlib.sha256(EXE_BYTES).hexdigest(), "codegen_stamp": "cg-1",
        "rom_stamp": "rom-1", "overlays": ["a", "b"], "overlays_source": "recomp/manifest",
        "module_sha256": "11" * 32, "module_march": "", "state_ident": "id-1", "game": "spyro-1",
        "disc": "SCUS-94228.cue", "disc_sha256": "22" * 32, "argv": ["openpete", "--headless"],
        "anchor": {"frame": 2100, "vbl": 4200, "feed_key": 2100, "level": 10, "park_ra": "0x1",
                   "park_sp": "0x2", "capture_us": 900},
        "press": {"frame": 4000, "vbl": 8000, "feed_key": 4000, "level": 10, "park_ra": "0x1",
                  "park_sp": "0x2", "capture_us": 800},
        "window_ticks": 1900, "segment_rows": 1900, "hash_rows": 10,
        "taint": {"freecam": 0, "devui": 3}, "mods": [{"id": "m1", "version": "1.0", "mutative": 1}],
        "answers": {"screen": "done"}, "press_us": 4870, "display_us": 12000,
        "redaction": {"replacements": 5, "findings": 2, "list": [
            {"member": "press.state", "section": "arena", "offset": 10, "what": "home"},
            {"member": "press.state", "section": "arena", "offset": 20, "what": "home"}]},
        "members": [{"name": n, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()}
                    for n, d in mem.items()],
    }
    m.update(over)
    return m


def png(w=32, h=18, shade=0):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + bytes([shade, 0, 0, 255]) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def write_bundle(path, mem=None, man=None, extra=(), tamper=None, skip_manifest=False):
    mem = dict(members() if mem is None else mem)
    man = manifest(mem) if man is None else man
    if tamper:
        mem[tamper] = mem[tamper] + b"x"
    with zipfile.ZipFile(path, "w") as z:
        if not skip_manifest:
            z.writestr("manifest.json", json.dumps(man) if isinstance(man, dict) else man)
        for n, d in mem.items():
            z.writestr(n, d)
        for n, d in extra:
            z.writestr(n, d)
    return path


def run(*args):
    r = subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)],
                       capture_output=True, text=True, timeout=60,
                       env=dict(os.environ, OPENPETE_VIBRATION="0"))
    return r.returncode, r.stdout, r.stderr


class Base(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.d = Path(self.td.name)
        self.arch = self.d / "no-archive"

    def tearDown(self):
        self.td.cleanup()


class InfoExtract(Base):
    def test_info_fields(self):
        z = write_bundle(self.d / "b.zip")
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        for s in ["schema 1", "commit abcdef123", "exe_sha256 " + hashlib.sha256(EXE_BYTES).hexdigest(),
                  "codegen_stamp cg-1", "rom_stamp rom-1", "march (unset)", "game spyro-1",
                  "disc SCUS-94228.cue", "m1 1.0 mutative", "press : frame 4000 vbl 8000",
                  "anchor: frame 2100", "anchor age: frame +1900 (31.8 s at the VBL rate), vbl +3800", "taint: 3 total (devui=3)",
                  "state: level=10 Artisans", "redaction: 5 replacement(s), 2 finding(s)",
                  "2 home in press.state [arena]", "hash check all match",
                  "p50 16.67", "max 50.00 ms; hitches 1", "[warn ] cd: slow read",
                  "[error] gpu: lost", "replay route: fallback", "--bug-report-replay"]:
            self.assertIn(s, out)
        self.assertNotIn("[info ] boot", out)
        self.assertNotIn("this reader knows", out)
        self.assertNotIn("left out:", out)

    def test_schema2_redaction_lists(self):
        mem = dict(members())
        red = {"replacements": 5, "findings": 1, "list": [
                   {"member": "host-trace.bin", "section": "", "offset": 4, "what": "home"}],
               "dropped_total": 1, "dropped": [
                   {"member": "log-prev.txt", "reason": "the home directory survives redaction"}],
               "flagged_total": 1, "flagged": [
                   {"member": "host-trace.bin", "home": 1, "user": 0, "host": 0}]}
        z = write_bundle(self.d / "b.zip", mem, manifest(mem, schema=2, redaction=red))
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        for s in ["schema 2", "left out: 1 member(s) that could not be redacted",
                  "log-prev.txt: the home directory survives redaction",
                  "flagged: 1 member(s) shipped unchanged with identity matches",
                  "host-trace.bin: home 1 user 0 host 0"]:
            self.assertIn(s, out)
        self.assertNotIn("this reader knows", out)

    def test_extract_round_trip(self):
        z = write_bundle(self.d / "b.zip")
        rc, out, _ = run("extract", z, self.d / "x")
        self.assertEqual(rc, 0, out)
        man = json.loads((self.d / "x" / "manifest.json").read_text())
        for m in man["members"]:
            got = (self.d / "x" / m["name"]).read_bytes()
            self.assertEqual(hashlib.sha256(got).hexdigest(), m["sha256"])

    def test_tampered_member(self):
        z = write_bundle(self.d / "b.zip", tamper="summary.txt")
        rc, out, _ = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 1)
        self.assertIn("SIZE summary.txt", out)
        rc, out, _ = run("extract", z, self.d / "x")
        self.assertEqual(rc, 1)
        self.assertIn("FAIL summary.txt", out)


def sub_present(seq, present_ns, kind, alpha, first, n, path=0):
    return br.SUB_PRESENT.pack(seq, present_ns, present_ns + 1000, present_ns - 40_000_000, present_ns - 6_600_000,
                               99, 100, 200 + seq, 400 + seq, alpha, kind, 1, path, 0, seq % 4, seq, first, n,
                               7, 33_430_000)


def sub_obj(key, cls, world, sx, sy, prev=(0, 0, 0), curr=(100, 0, 0), verdict=0, prev_dt=-40_000,
            curr_dt=-6_600):
    return br.SUB_OBJ.pack(key, key, cls, verdict, br.SUB_F_WORLD | (1 << 6), *prev, *curr, 1, 2, 3, 4, 5, 6,
                           50, *world, sx, sy, 900.0, prev_dt, curr_dt, 0.5)


def subtick_bin(flags=True):
    """Three presents of a Sparx-class record 0x80010000 and one other record."""
    # t = 6.6 ms / 33.4 ms (the pair span, under the tick period) = 0.198: on the lerp x = 100 - 100*(1-t) = 19.8.
    objs = [sub_obj(0x80010000, 0x78, (19.8, 0, 0), 10, 10), sub_obj(0x80020000, 0x10, (100, 0, 0), 50, 50,
                                                                      prev=(100, 0, 0))]
    objs += [sub_obj(0x80010000, 0x78, (19.8, 0, 0), 11, 10),
             sub_obj(0x80010000, 0x78, (19.8, 0, 0), 90 if flags else 11.5, 10)]
    objs += [sub_obj(0x80010000, 0x78, (60.0 if flags else 19.8, 0, 0), 12, 10)]
    pres = [sub_present(1, 1_000_000_000, 0, 1.0, 0, 2), sub_present(2, 1_002_083_333, 1, 0.06, 2, 2),
            sub_present(3, 1_004_166_666, 1, 0.12, 4, 1)]
    hdr = br.SUB_HEADER.pack(br.SUB_MAGIC, 1, br.SUB_PRESENT.size, br.SUB_OBJ.size, len(pres), len(objs), 0, 0, 0)
    return hdr + b"".join(pres) + b"".join(objs)


class Subtick(Base):
    def bundle(self, flags=True):
        mem = members()
        mem["subtick.bin"] = subtick_bin(flags)
        return write_bundle(self.d / "s.zip", mem, manifest(mem))

    def test_info_line(self):
        rc, out, err = run("info", self.bundle(), "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("subtick: 3 presents over 0.00 s (canonical/eager 1, intermediate/eager 2), 5 drawn records; "
                      "MULTI 1 OFFLERP 1", out)

    def test_flags(self):
        rc, out, err = run("subtick", self.bundle(), "--class", "0x78")
        self.assertEqual(rc, 2, out + err)
        self.assertIn("0x078        4     1         3                3", out)
        self.assertIn("MULTI (one record at more than one screen position within a present, > 2.0 px): 1", out)
        self.assertIn("seq 2 key 202 a=0.060 rec 80010000 cls 0x078 x2: (11.0,10.0) (90.0,10.0)", out)
        self.assertIn("OFFLERP (paired record drawn more than 2.0 world units off its lerp and off curr): 1", out)
        self.assertIn("seq 3 key 203 a=0.120 rec 80010000 cls 0x078 t=0.198 err 40.2", out)
        self.assertIn("record 80010000 (moby 80010000 cls 0x078), last 4 draws:", out)
        self.assertNotIn("80020000", out)

    def test_clean(self):
        rc, out, err = run("subtick", self.bundle(flags=False))
        self.assertEqual(rc, 0, out + err)
        self.assertIn("MULTI (one record at more than one screen position within a present, > 2.0 px): 0", out)
        self.assertIn("): 0", out.split("OFFLERP")[1])

    def test_extracted_member(self):
        f = self.d / "subtick.bin"
        f.write_bytes(subtick_bin())
        rc, out, err = run("subtick", f, "--key", "0x80010000")
        self.assertEqual(rc, 2, out + err)
        self.assertIn("subtick.bin v1: 3 presents over 0.004 s, 5 drawn records", out)


AUDIO_EVENTS = [
    "-2.000 600 6144 prime headless -p- 1.000000 1.0000 1.000000 5200 1.0000 5897 0 0 0.020 5897 5200.0",
    "-1.500 630 28160 tick headless -p- 1.000000 1.0000 1.000000 5100 1.0000 900 0 0 0.030 27000 0.0",
    "-0.500 690 70000 slip headless -p- 0.999000 0.9600 1.000000 2663 0.7500 737 0 4 5.487 69000 4.0",
    "-0.500 690 70000 stretch headless -ps 0.999000 0.9600 1.000000 2663 0.7500 737 0 4 5.487 69000 0.0",
    "-0.400 696 74000 UNDERRUN headless --s 0.999000 0.9600 1.000000 1000 0.9600 1474 1 4 5.503 73000 0.0",
]


def audio_txt(events=AUDIO_EVENTS, data_out=70000, held=None, mute=1):
    hdr = ["# audio-feed.txt: test", "version=1", f"mute={mute}", "device=synthetic", "snapshot_wall_ns=5000000000",
           "snapshot_present_ns=1", "snapshot_vbl=700", "counters=read", "ring=2048",
           f"events_total={len(events)}", f"events_held={len(events) if held is None else held}", "events_skipped=0"]
    if mute:
        hdr.append("note=muted: the SPU output was silenced")
    cnt = ["spu: reverb_cycles=0 reverb_in=0 noise=0 pmon=0 sweep=0 host_plays=0 host_clip_frames=0 host_stream_frames=0",
           "feed: pushed=80000 held=1000 overflow=0 consumed=75000 periods=73 underruns=1 primes=1 target=5200 "
           "depth_min=900 p01=1000 p50=4000 p99=4600 ratio=0.99900 rate=1.00000",
           f"feed: accounting: pushed=80000 = taken=79000 + overflow=0 + discarded=0 + fifo=1000 (residual 0); "
           f"consumed=75000 = data_out={data_out} + silence_out={75000 - data_out}",
           "feed: pitch: pumps=300 median=+0.00c p99=1.50c max=-1.73c",
           "feed: drift: drift=1.000000 (+0.0 ppm) speed=0.9600 fits=0 refits=2 behind_used=5.5 behind_dropped=0.0",
           "feed: stretch: hops=40 engages=1 on=1 corr_p01=0.80 corr_p50=0.97",
           "feed: phase headless: pushed=80000 held=0 overflow=0 underruns=1 primes=1",
           "feed: now: device=synthetic hold=0 playing=0 phase=headless tag=700 queued=0 fifo=1474 depth_s=1000 "
           "stretch_on=1 st_rate=0.9600 nominal=1.0000 slips=4 behind=5.503 events=5",
           "# events, oldest first: t_s vbl consumed event phase flags ratio speed drift depth_s st_rate fifo "
           "underruns slips behind pushed value"]
    return ("\n".join(hdr + cnt + list(events)) + "\n").encode()


class Audio(Base):
    def bundle(self, txt=None, name="a.zip"):
        mem = members()
        mem["audio-feed.txt"] = audio_txt() if txt is None else txt
        return write_bundle(self.d / name, mem, manifest(mem))

    def test_info_line(self):
        rc, out, err = run("info", self.bundle(), "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("audio: mute=1 device=synthetic; 5 events over 1.6 s (UNDERRUN 1, prime 1, slip 1, stretch 1, "
                      "tick 1); underruns 1 primes 1 stretch engages 1 on=1 speed 0.9600 pitch p99 1.50", out)

    def test_list(self):
        rc, out, err = run("audio", self.bundle(), "--no-ticks")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("audio-feed.txt v1: mute=1 device=synthetic counters=read; events 5 held of 5 recorded", out)
        self.assertIn("note: muted: the SPU output was silenced", out)
        self.assertIn("feed: stretch: hops=40 engages=1", out)
        self.assertIn("events (4 of 5 listed)", out)
        self.assertIn("   -0.500     690      70000 slip         headless  -p-   -1.732  0.9600", out)
        self.assertNotIn(" tick ", out)
        rc, out, err = run("audio", self.bundle(), "--kind", "UNDERRUN,prime", "--last", "1")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("events (1 of 5 listed)", out)
        self.assertIn(" UNDERRUN ", out)
        self.assertNotIn(" prime ", out.split("events (")[1])

    def test_extracted_member(self):
        f = self.d / "audio-feed.txt"
        f.write_bytes(audio_txt())
        rc, out, err = run("audio", f)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("events (5 of 5 listed)", out)

    def test_vacuous(self):
        rc, out, err = run("audio", self.bundle(audio_txt(events=[])))
        self.assertEqual(rc, 3, out + err)
        self.assertIn("VACUOUS: the feed played data and the ring holds no event", out)
        rc, out, err = run("audio", self.bundle(audio_txt(events=[], data_out=0), "silent.zip"))
        self.assertEqual(rc, 0, out + err)

    def test_absent_and_malformed(self):
        z = write_bundle(self.d / "old.zip")
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertIn("audio: no audio-feed.txt", out)
        rc, out, err = run("audio", z)
        self.assertEqual(rc, 3, out + err)
        bad = audio_txt(events=AUDIO_EVENTS[:-1] + [AUDIO_EVENTS[-1].rsplit(" ", 1)[0]])
        rc, out, err = run("audio", self.bundle(bad, "bad.zip"))
        self.assertEqual(rc, 64, out + err)
        self.assertIn("refused: audio-feed.txt: event row with 16 fields", err)
        rc, out, err = run("info", self.bundle(bad, "bad2.zip"), "--archive", self.arch)
        self.assertIn("audio: unreadable", out)

    def test_corpus_columns(self):
        corp = self.d / "corpus"
        corp.mkdir()
        self.bundle(name="corpus/bug-spyro-1-a.zip")
        tsv = self.d / "c.tsv"
        rc, out, err = run("corpus", corp, "--tsv", tsv)
        self.assertEqual(rc, 0, out + err)
        lines = tsv.read_text().splitlines()
        row = dict(zip(lines[0].split("\t"), lines[1].split("\t")))
        self.assertEqual((row["audio_device"], row["audio_mute"], row["audio_events"], row["audio_underruns"],
                          row["audio_stretch_engages"], row["audio_pitch_p99_c"]),
                         ("synthetic", "1", "5", "1", "1", "1.50"))


class ThumbsNote(Base):
    def bundle(self, n_thumbs=20, note_text=None, broken=None, **over):
        mem = members()
        for i in range(n_thumbs):
            d = png(shade=i)
            if i == broken:
                d = d[:40] + bytes([d[40] ^ 1]) + d[41:]
            mem[f"thumbs/{i:03d}.png"] = d
        if note_text is not None:
            mem["notes.txt"] = note_text.encode()
        return write_bundle(self.d / "t.zip", mem, manifest(mem, **over))

    def test_thumbs_and_note(self):
        z = self.bundle(note_text="spyro fell through the floor\n",
                        thumbs={"state": "held", "span_ms": 4750, "newest_age_ms": 120},
                        note={"state": "saved"})
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        for s in ["thumbnails: 20 (32x18), span 4.75 s, 4.00 Hz, newest 0.12 s before the press; all decode",
                  "note: spyro fell through the floor"]:
            self.assertIn(s, out)

    def test_broken_thumb(self):
        z = self.bundle(broken=3, thumbs={"state": "held", "span_ms": 4750})
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertIn("1 do not decode", out)

    def test_none(self):
        z = self.bundle(n_thumbs=0, **{"thumbs": {"state": "off"}, "note": {"state": "skipped"}})
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("thumbnails: none (off)", out)
        self.assertIn("note: none (skipped)", out)

    def test_fatal_trigger(self):
        z = self.bundle(n_thumbs=0, trigger="hang", exit_thread="game fiber", press=None)
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("trigger hang: written at a fatal exit on the game fiber thread", out)
        self.assertIn("press : none", out)

    def test_png_check(self):
        self.assertEqual(br.png_check(png(320, 180)), (320, 180))
        with self.assertRaises(ValueError):
            br.png_check(png()[:-20])


class Refusals(Base):
    def refused(self, z, needle):
        rc, out, err = run("info", z)
        self.assertEqual(rc, 64, out + err)
        self.assertIn("refused:", err)
        self.assertIn(needle, err)
        self.assertNotIn("Traceback", err)

    def test_truncated(self):
        z = write_bundle(self.d / "b.zip")
        data = z.read_bytes()
        z.write_bytes(data[: len(data) // 2])
        self.refused(z, "not a readable zip")

    def test_not_zip(self):
        p = self.d / "b.zip"
        p.write_bytes(b"hello")
        self.refused(p, "not a readable zip")

    def test_dotdot(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("../evil.txt", b"x")]), "'..' component")

    def test_absolute(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("/etc/evil", b"x")]), "absolute name")

    def test_backslash(self):
        self.refused(write_bundle(self.d / "b.zip", extra=[("..\\evil", b"x")]), "backslash")

    def test_missing_manifest(self):
        self.refused(write_bundle(self.d / "b.zip", skip_manifest=True), "no manifest.json")

    def test_bad_manifest(self):
        self.refused(write_bundle(self.d / "b.zip", man="{not json"), "does not parse")

    def test_duplicate(self):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            self.refused(write_bundle(self.d / "b.zip", extra=[("summary.txt", b"y")]), "duplicate")

    def test_extract_refuses_dotdot(self):
        z = write_bundle(self.d / "b.zip", extra=[("a/../../evil", b"x")])
        rc, _, err = run("extract", z, self.d / "x")
        self.assertEqual(rc, 64)
        self.assertFalse((self.d / "evil").exists())


class DiffHashes(Base):
    def test_identical(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv())
        b.write_bytes(hash_csv())
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 0)
        self.assertIn("IDENTICAL rows=10", out)

    def test_planted(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv())
        b.write_bytes(hash_csv(alter=(105, "mobys")))
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=105 vbl=210 row=5 regions=mobys total=differs", out)

    def test_bundle_argument_and_subset(self):
        z = write_bundle(self.d / "b.zip")
        b = self.d / "b.csv"
        b.write_bytes(hash_csv(rows=range(103, 120), alter=(108, "camera")))
        rc, out, _ = run("diff-hashes", z, b)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=108", out)
        self.assertIn("regions=camera", out)
        self.assertIn("common 7, only A 3, only B 10", out)

    def test_vacuous(self):
        a, b = self.d / "a.csv", self.d / "b.csv"
        a.write_bytes(hash_csv(rows=range(0, 5)))
        b.write_bytes(hash_csv(rows=range(5, 10)))
        rc, out, _ = run("diff-hashes", a, b)
        self.assertEqual(rc, 3)
        self.assertIn("VACUOUS", out)

    def test_bad_header(self):
        a = self.d / "a.csv"
        a.write_bytes(b"x,y\n1,2\n")
        rc, _, err = run("diff-hashes", a, a)
        self.assertEqual(rc, 64)
        self.assertNotIn("Traceback", err)


def rec(typ, t, index=0, flags=0, aux=0, a=0, b=0, c=0, d=0, e=0, f=0):
    return struct.pack("<BBHIqqqqqqq", typ, flags, aux, index, t, a, b, c, d, e, f)


def u32pair(lo, hi):
    return lo | (hi << 32)


def perf_ring(ticks=60, exclusive=False):
    """A pace image of `ticks` ticks at 30 Hz: two VBLs (0.2 and 4.0 ms runs), four presents (one canonical),
    one shed, one TICK_SPLIT, one EAGER_PLAN and one GPU_FRAME per tick. With `exclusive` the TICK_SPLIT
    records carry the exclusive layout's flag and 0.25 ms of host other in f's high word."""
    env = b"build=test\ngpu=Test GPU\nrefresh_mhz=120000\npresent_mode=fifo\nrender_fps=120\neager=1\nplatform=Linux\n"
    hdr = struct.pack("<IIIIIIqqqqq", br.OPPL_MAGIC, 2, 64 + len(env), 64, len(env), 0, 0, 0, 2250000, 37609, 2)
    out, t = [], 10**9
    for k in range(ticks):
        v0 = 2 * k
        out.append(rec(br.PACE_VBL, t, v0, c=t, d=t + 200000))
        out.append(rec(br.PACE_VBL, t + 16_700_000, v0 + 1, c=t + 16_700_000, d=t + 20_700_000))
        out.append(rec(br.PACE_TICK_SPLIT, t + 20_800_000, v0 + 1, a=u32pair(4200, 1500), b=u32pair(900, 400),
                       c=u32pair(120, 60), d=u32pair(2000, 300), e=3 | 4 << 16 | 4 << 32 | 5 << 48,
                       flags=br.PACE_TS_EXCLUSIVE if exclusive else 0,
                       f=u32pair(4000, 250) if exclusive else 4000))
        out.append(rec(br.PACE_EAGER_PLAN, t + 20_900_000, v0 + 1,
                       flags=(br.PACE_EP_CUT if k % 10 == 0 else 0) | (br.PACE_EP_FILL if k % 2 else 0),
                       aux=2 | 1 << 8, a=20_000_000 | 2 << 32, b=1_500_000, c=5_000_000, d=200_000, e=0,
                       f=4 | 3 << 16 | 2 << 32 | 8333 << 48))
        out.append(rec(br.PACE_GPU_FRAME, t + 21_000_000, k, aux=12, a=3_000_000, b=2_500_000, c=k))
        for q in range(4):
            out.append(rec(br.PACE_PRESENT, t + q * 8_333_333, flags=0 if q == 0 else 1))
        out.append(rec(br.PACE_SHED, t + 30_000_000, flags=1, aux=2))
        t += 33_433_000
    return hdr + env + b"".join(out)


def coarse_image(rows=3):
    fields = len(br.COARSE_FIELDS)
    body = b""
    for i in range(rows):
        v = [0] * fields
        v[br.COARSE_FIELDS.index("vbls")] = 60
        v[br.COARSE_FIELDS.index("presents")] = 100 + 10 * i
        v[br.COARSE_FIELDS.index("level_max")] = i + 1
        v[0], v[1] = i, 1
        body += struct.pack("<%dI" % fields, *v)
    return br.COARSE_HEADER.pack(br.COARSE_MAGIC, 1, 4 * fields, rows, 1, 0, rows * 10**9) + body


class PerfRecords(Base):
    def test_decode(self):
        _h, recs = br.pace_records(perf_ring(2))
        sp = [br.split_decode(r) for r in recs if r[0] == br.PACE_TICK_SPLIT][0]
        self.assertEqual((sp["sim"], sp["extract"], sp["terrain"], sp["moby"]), (4.2, 1.5, 0.9, 0.4))
        self.assertEqual((sp["snap_rex"], sp["snap_canon"], sp["render"], sp["present"]), (0.12, 0.06, 2.0, 0.3))
        self.assertEqual((sp["n_extract"], sp["n_render"], sp["n_present"], sp["n_snap"], sp["tick"]), (3, 4, 4, 5, 4.0))
        self.assertEqual((sp["exclusive"], sp["host_other"]), (False, 0.0))
        self.assertNotIn("guest", sp)
        pl = [br.plan_decode(r) for r in recs if r[0] == br.PACE_EAGER_PLAN][0]
        self.assertEqual((pl["level"], pl["std"], pl["slots"], pl["afford"], pl["planned"]), (2, 1, 4, 3, 2))
        self.assertTrue(pl["cut"])
        self.assertAlmostEqual(pl["period_ms"], 8.333)
        self.assertAlmostEqual(pl["budget"], 20.0)
        self.assertEqual((pl["made"], pl["fill"]), (2, False))
        g = [br.gpu_decode(r) for r in recs if r[0] == br.PACE_GPU_FRAME][1]
        self.assertEqual((g["token"], g["zones"], g["frame_ms"], g["busy_ms"]), (1, 12, 3.0, 2.5))

    def test_exclusive_split(self):
        _h, recs = br.pace_records(perf_ring(2, exclusive=True))
        sp = [br.split_decode(r) for r in recs if r[0] == br.PACE_TICK_SPLIT][0]
        self.assertTrue(sp["exclusive"])
        self.assertEqual((sp["tick"], sp["host_other"]), (4.0, 0.25))
        self.assertAlmostEqual(sp["host"], 0.9 + 0.4 + 0.06 + 0.25)
        self.assertAlmostEqual(sp["guest"], 4.2 - sp["host"])
        r = br.perf_rollup(perf_ring(60, exclusive=True))
        self.assertEqual(r["split_exclusive_ticks"], 60)
        self.assertAlmostEqual(r["split_mean_guest"] + r["split_mean_host"], r["split_mean_sim"])
        self.assertNotIn("split_exclusive_ticks", br.perf_rollup(perf_ring(60)))

    def test_rollup(self):
        r = br.perf_rollup(perf_ring(60))
        self.assertAlmostEqual(r["pres_s"], 240 / r["window_s"], places=6)
        self.assertAlmostEqual(r["sim_p50"], 4.0)
        self.assertAlmostEqual(r["scenes_tick"], 4.0)
        self.assertAlmostEqual(r["sheds_tick"], 1.0)
        self.assertEqual(r["split_ticks"], 60)
        self.assertAlmostEqual(r["split_render"], 2.0)
        self.assertEqual((r["plans"], r["plan_level_max"]), (60, 2))
        self.assertAlmostEqual(r["plan_cut_frac"], 0.1)
        self.assertAlmostEqual(r["plan_made"], 2.0)
        self.assertAlmostEqual(r["plan_fill_frac"], 0.5)
        self.assertAlmostEqual(r["gpu_p50"], 3.0)

    def test_coarse(self):
        h, rows = br.coarse_parse(coarse_image())
        self.assertEqual((h["rows"], h["span_s"]), (3, 1))
        self.assertEqual([x["presents"] for x in rows], [100, 110, 120])
        c = br.coarse_rollup(rows)
        self.assertEqual((c["coarse_pres_first"], c["coarse_pres_last"], c["coarse_s"]), (100, 120, 3))
        with self.assertRaises(br.BundleError):
            br.coarse_parse(b"BADMAGIC" + coarse_image()[8:])

    def test_perf_and_corpus(self):
        new = members()
        new["pace.oppl"] = perf_ring(60)
        new["pace-coarse.bin"] = coarse_image()
        new["notes.txt"] = b"stutter\tin the hub\n"
        man = manifest(new, records={"pace": {"member": "pace.oppl", "types": list(range(1, 18))},
                                     "pace_coarse": {"member": "pace-coarse.bin", "rows": 3}})
        corp = self.d / "corpus"
        corp.mkdir()
        write_bundle(corp / "bug-spyro-1-new.zip", new, man)
        write_bundle(corp / "bug-spyro-1-old.zip")
        (corp / "bug-spyro-1-junk.zip").write_bytes(b"not a zip")
        rc, out, _err = run("perf", corp / "bug-spyro-1-new.zip")
        self.assertEqual(rc, 0)
        for want in ("split (60 ticks)", "planner (60 spans, fill cadence 50.0%)", "gpu (60 frames)", "coarse: 3 rows",
                     "sim tick: a log written before the exclusive split"):
            self.assertIn(want, out)
        ex = members()
        ex["pace.oppl"] = perf_ring(60, exclusive=True)
        write_bundle(self.d / "bug-spyro-1-ex.zip", ex,
                     manifest(ex, records={"pace": {"member": "pace.oppl", "types": list(range(1, 18))}}))
        rc, out, _err = run("perf", self.d / "bug-spyro-1-ex.zip")
        self.assertEqual(rc, 0)
        self.assertIn("sim tick (60 ticks), mean ms per tick: sim 4.20 = guest 2.59 + host 1.61 (terrain 0.90, "
                      "moby 0.40, snap 0.06, other 0.25)", out)
        tsv = self.d / "c.tsv"
        rc, out, _err = run("corpus", corp, "--tsv", tsv)
        self.assertEqual(rc, 0)
        self.assertIn("corpus: 3 bundle(s)", out)
        lines = tsv.read_text().splitlines()
        cols = lines[0].split("\t")
        rows = {r.split("\t")[0]: dict(zip(cols, r.split("\t"))) for r in lines[1:]}
        self.assertEqual(len(rows), 3)
        nw, old, junk = rows["bug-spyro-1-new.zip"], rows["bug-spyro-1-old.zip"], rows["bug-spyro-1-junk.zip"]
        self.assertEqual((nw["split_ticks"], nw["plans"], nw["gpu_frames"], nw["note"]), ("60", "60", "60", "stutter in the hub"))
        self.assertEqual(nw["gpu"], "Test GPU")
        self.assertEqual((old["split_ticks"], old["plans"], old["coarse_s"]), ("", "", ""))
        self.assertNotEqual(old["pres_s"], "")
        self.assertIn("not a readable zip", junk["error"])


class Pace(unittest.TestCase):
    def test_hitch(self):
        ps = br.pace_summary(pace_ring([16.67] * 50 + [50.0] + [16.67] * 50))
        self.assertEqual(ps["presents"], 102)
        self.assertAlmostEqual(ps["p50_ms"], 16.67, places=2)
        self.assertEqual(ps["hitches"], 1)
        self.assertAlmostEqual(ps["max_ms"], 50.0, places=2)

    def test_window(self):
        ps = br.pace_summary(pace_ring([1000.0] * 40))
        self.assertEqual(ps["presents"], 31)

    def test_bad_magic(self):
        self.assertIn("error", br.pace_summary(b"\0" * 80))


KNOBS_TXT = ("# knobs.txt v1\n"
             "session\tfirst_frame=1\tchanges=2\tkept=2\tdropped=0\n"
             "knob\taspect\trender\t16:9\t16:9\tauto\tcli\tD\t3\t0\t-\n"
             "knob\tterrain_visibility\trender\tgame\tfrustum\tfrustum\tdefault\tLDC\t0\t1\t2557@85.512\n"
             "knob\tfreecam\tsim\toff\toff\t-\tdefault\tC\t0\t2\t2600@87.000\n"
             "knob\tssaa\trender\t1\t1\t1\tdefault\t-\t0\t0\t-\n"
             "change\t2557\t5110\t2557\t85.512\tterrain_visibility\tfrustum\tgame\t1\t0\n"
             "change\t2600\t5196\t2600\t87.000\tfreecam\toff\toff\t1\t0\n").encode()


class Knobs(Base):
    def test_info_names_live_change_first(self):
        mem = members()
        mem["knobs.txt"] = KNOBS_TXT
        rc, out, err = run("info", write_bundle(self.d / "k.zip", mem, manifest(mem)), "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        lines = out.splitlines()
        self.assertEqual(lines[1], "knob changed live: terrain_visibility = game (changed live at 85.5 s, "
                                   "frame 2557; boot frustum, default frustum; render)")
        self.assertIn("knob changed live: freecam = off (changed live at 87.0 s, frame 2600, back to its boot "
                      "value after 2 change(s); boot off, default -; sim)", out)
        self.assertIn("knobs off default: aspect=16:9 (default auto, cli)", out)
        self.assertNotIn("ssaa", out)
        k = br.knobs_parse(KNOBS_TXT.decode())
        self.assertEqual(k["session"]["dropped"], "0")
        self.assertEqual([c["frame"] for c in k["changes"]], [2557, 2600])

    def test_info_without_member(self):
        rc, out, _ = run("info", write_bundle(self.d / "b.zip"), "--archive", self.arch)
        self.assertEqual(rc, 0)
        self.assertEqual(out.splitlines()[1].split(":")[0], "knobs")
        self.assertIn("knobs: no knobs.txt (written before the member)", out)

    def test_replay_prints_knob_lines(self):
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "d.cue"
        disc.write_text("x")
        stub = self.d / "stub-openpete"
        stub.write_text("#!/bin/sh\n"
                        "# Stub: stands in for openpete --bug-report-replay; prints knob lines and a verdict.\n"
                        "echo 'bug-report-replay: knob terrain_visibility = game applied at the anchor load "
                        "(frame 2100; set live before the anchor; boot value frustum)'\n"
                        "echo 'bug-report-replay: knobs: 1 changed after boot, 1 applied (1 at the anchor load, "
                        "0 at their tick), 0 not applied'\n"
                        "echo 'bug-report-replay: IDENTICAL rows=1900'\nexit 0\n")
        stub.chmod(0o755)
        rc, out, err = run("replay", z, "--exe", stub, "--disc", disc, "--work", self.d / "w", "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("bug-report-replay: knob terrain_visibility = game applied at the anchor load", out)
        self.assertIn("bug-report-replay: knobs: 1 changed after boot, 1 applied", out)


class Replay(Base):
    def stub(self, verdict, rc):
        p = self.d / "stub-openpete"
        p.write_text("#!/bin/sh\n"
                     "# Stub: stands in for openpete --bug-report-replay; prints its argv and a verdict.\n"
                     f"echo \"args: $*\"\necho '  1.0 [info ] bug-report-replay: {verdict}'\nexit {rc}\n")
        p.chmod(0o755)
        return p

    def test_fallback_identical(self):
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "SCUS-94228.cue"
        disc.write_text("x")
        rc, out, err = run("replay", z, "--exe", self.stub("IDENTICAL rows=1900", 0), "--disc", disc,
                           "--work", self.d / "w", "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("route: fallback", out)
        self.assertIn("bug-report-replay: IDENTICAL rows=1900", out)
        log = (self.d / "w" / "replay.log").read_text()
        self.assertIn("--bug-report-replay", log)
        self.assertIn("--card1 " + str(self.d / "w" / "cards" / "card1.mcd"), log)
        self.assertIn("--no-card2", log)
        self.assertTrue((self.d / "w" / "cards" / "card1.mcd").is_file())

    def test_fallback_diverged(self):
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "d.cue"
        disc.write_text("x")
        rc, out, _ = run("replay", z, "--exe", self.stub("DIVERGED tick=2500 region=mobys section=ram+0x10 taint=0", 2),
                         "--disc", disc, "--work", self.d / "w", "--archive", self.arch)
        self.assertEqual(rc, 2)
        self.assertIn("DIVERGED tick=2500 region=mobys", out)

    def test_no_anchor_refused(self):
        mem = members()
        z = write_bundle(self.d / "b.zip", mem=mem, man=manifest(mem, anchor=None))
        rc, out, _ = run("replay", z, "--exe", self.stub("IDENTICAL rows=1", 0), "--archive", self.arch)
        self.assertEqual(rc, 64)
        self.assertIn("no anchor", out)

    def archive(self, kind="tarball", sums=True, listed=None):
        """Write an archive directory as scripts/lib/release_archive.sh lays it out; @return its root."""
        root = self.d / "archive"
        d = root / "v0.5.0-test-abcdef123"
        d.mkdir(parents=True)
        exe_sha = listed or hashlib.sha256(EXE_BYTES).hexdigest()
        if kind == "tarball":
            name, inner = "openpete-v0.5.0-linux-x86_64.tar.xz", "openpete-v0.5.0/openpete"
            with tarfile.open(d / name, "w:xz") as t:
                ti = tarfile.TarInfo(inner)
                ti.size = len(EXE_BYTES)
                ti.mode = 0o755
                t.addfile(ti, io.BytesIO(EXE_BYTES))
        else:
            # A stand-in AppImage: answers --appimage-extract by writing
            # squashfs-root/ with the inner exe and an AppRun that execs it.
            name, inner = "OpenPete-v0.5.0-x86_64.AppImage", "usr/openpete"
            exe = EXE_BYTES.decode().replace("\n", "\\n")
            (d / name).write_text(
                "#!/bin/sh\n"
                "# Stub AppImage for the reader's unit rows.\n"
                "[ \"$1\" = --appimage-extract ] || exit 9\n"
                "mkdir -p squashfs-root/usr\n"
                f"printf '{exe}' > squashfs-root/usr/openpete\n"
                "printf '#!/bin/sh\\necho \"apprun: $*\"\\necho \"  bug-report-replay: IDENTICAL rows=7 sections=skipped fatal=hang\"\\n' > squashfs-root/AppRun\n"
                "chmod 755 squashfs-root/usr/openpete squashfs-root/AppRun\n")
            (d / name).chmod(0o755)
        (d / "SHA256SUMS").write_text(f"{file_sha(d / name)}  {name}\n")
        if sums:
            (d / "EXE-SHA256SUMS").write_text(f"{exe_sha}  {name}:{inner}\n")
        return root

    def test_exe_sums_parse(self):
        good = "ab" * 32
        text = (f"{good}  a.tar.xz:a/openpete\n"
                f"{good.upper()}  OpenPete.AppImage:usr/openpete\r\n"
                f"{good} *w.zip:openpete.exe\n"
                "not-a-hash  a.tar.xz:a/openpete\n"
                f"{good}  a.tar.xz\n"
                f"{good}  a.tar.xz:../openpete\n"
                f"{good}  a.tar.xz:/abs/openpete\n"
                f"{good}  dir/a.tar.xz:openpete\n")
        self.assertEqual(br.parse_exe_sums(text), [
            (good, "a.tar.xz", "a/openpete"), (good, "OpenPete.AppImage", "usr/openpete"),
            (good, "w.zip", "openpete.exe")])

    def test_primary_route_from_archive(self):
        arch = self.archive()
        z = write_bundle(self.d / "b.zip")
        rc, out, err = run("info", z, "--archive", arch)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("replay route: primary", out)
        self.assertIn("openpete-v0.5.0-linux-x86_64.tar.xz (tarball), exe openpete-v0.5.0/openpete", out)
        rc, out, _ = run("replay", z, "--archive", arch, "--work", self.d / "w", "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn(str(self.d / "w" / "artifact" / "openpete-v0.5.0" / "openpete"), out)

    def test_primary_tarball_runs(self):
        arch = self.archive()
        z = write_bundle(self.d / "b.zip")
        disc = self.d / "SCUS-94228.cue"
        disc.write_text("x")
        rc, out, err = run("replay", z, "--archive", arch, "--work", self.d / "w", "--disc", disc)
        # The archived exe prints "archived" and no verdict line: VACUOUS, but it ran.
        self.assertEqual(rc, 3, out + err)
        self.assertIn("archived", (self.d / "w" / "replay.log").read_text())

    def test_primary_appimage_extracts(self):
        arch = self.archive(kind="appimage")
        z = write_bundle(self.d / "b.zip")
        rc, out, err = run("info", z, "--archive", arch)
        self.assertIn("OpenPete-v0.5.0-x86_64.AppImage (appimage), exe usr/openpete", out)
        disc = self.d / "SCUS-94228.cue"
        disc.write_text("x")
        rc, out, err = run("replay", z, "--archive", arch, "--work", self.d / "w", "--disc", disc)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("IDENTICAL rows=7 sections=skipped fatal=hang", out)
        self.assertIn("apprun: --game spyro-1", (self.d / "w" / "replay.log").read_text())

    def test_unlisted_artifact_not_used(self):
        arch = self.archive(sums=False)
        z = write_bundle(self.d / "b.zip")
        rc, out, _ = run("info", z, "--archive", arch)
        self.assertIn("replay route: fallback", out)

    def test_listed_hash_mismatch_refused(self):
        # The listed hash is the manifest's, but the exe inside is not.
        mem = members()
        want = "cd" * 32
        arch = self.archive(listed=want)
        z = write_bundle(self.d / "b.zip", mem=mem, man=manifest(mem, exe_sha256=want))
        disc = self.d / "SCUS-94228.cue"
        disc.write_text("x")
        rc, out, _ = run("replay", z, "--archive", arch, "--work", self.d / "w", "--disc", disc)
        self.assertEqual(rc, 64, out)
        self.assertIn("is not the manifest's", out)

    def test_archive_env(self):
        arch = self.archive()
        z = write_bundle(self.d / "b.zip")
        r = subprocess.run([sys.executable, "-I", str(TOOL), "info", str(z)], capture_output=True, text=True,
                           timeout=60, env=dict(os.environ, OPENPETE_RELEASE_ARCHIVE_DIR=str(arch)))
        self.assertIn("replay route: primary", r.stdout)

    def test_archive_version_escape(self):
        """A build_version holding `../` names no directory outside the archive."""
        self.archive()
        out = self.d / "outside-x"
        shutil.copytree(self.d / "archive" / "v0.5.0-test-abcdef123", out)
        shutil.rmtree(self.d / "archive" / "v0.5.0-test-abcdef123")
        (self.d / "archive" / "empty").mkdir()
        mem = members()
        man = manifest(mem, build_version="../outside", build_commit="x")
        self.assertIsNone(br.find_archived(man, self.d / "archive"))
        man = manifest(mem, build_version="v0.5.0-test", build_commit="../../x")
        self.assertIsNone(br.find_archived(man, self.d / "archive"))


class TermSafe(Base):
    def test_info_escapes_control_characters(self):
        mem = members()
        mem["log.txt"] = "  0.1 [error] gpu: \x1b]0;title\x07lost\x08\x9b2J\n".encode()
        mem["notes.txt"] = b"note \x1b[31mred\x1b[0m\n"
        z = write_bundle(self.d / "b.zip", mem=mem,
                         man=manifest(mem, build_version="v\x1b[2J", build_commit="abc"))
        rc, out, err = run("info", z, "--archive", self.arch)
        self.assertEqual(rc, 0, out + err)
        for c in ("\x1b", "\x07", "\r", "\x9b"):
            self.assertNotIn(c, out)
        self.assertIn("gpu: \\x1b]0;title\\x07lost\\x08\\x9b2J", out)
        self.assertIn("v\\x1b[2J", out)

    def test_term_safe_keeps_text(self):
        self.assertEqual(br.term_safe("a\tb\nc é"), "a\tb\nc é")
        self.assertEqual(br.term_safe("x‮y"), "x\\u202ey")


def file_sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


if __name__ == "__main__":
    unittest.main()
