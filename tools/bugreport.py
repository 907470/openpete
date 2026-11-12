#!/usr/bin/env python3
"""bugreport.py - reader for the bug-report bundle (the F9 zip).

A bundle is untrusted input. This tool reads zip members by name with size
bounds, refuses absolute names, `..` components, drive letters, NUL bytes
and duplicate names, and never executes or imports anything from a bundle.
It needs the Python 3 standard library only and runs under `python3 -I`.

Subcommands:

  info BUNDLE [--log-lines N]
      The summary a reader starts from: first the knobs changed live after
      boot (knobs.txt: id, value, the session time and tick of the last
      change, boot value, default, replay class) and the knobs whose value
      differs from the declared default, then build, commit, exe SHA-256, game,
      disc, ROM stamp, codegen stamp and march, mods, the press and anchor
      counters and the anchor's age, the state summary, taint counts, the
      member list with sizes and the hash check, redaction findings, a pace
      summary over the last 30 s of present records, the last WARN/ERROR log
      lines, and the replay route with its exact command.
  extract BUNDLE DIR
      Writes every member under DIR and verifies each against the manifest.
  replay BUNDLE [--exe PATH] [--disc CUE] [--archive DIR] [--work DIR]
                [--timeout S] [--dry-run] [-- EXTRA...]
      Runs the route `info` names: the archived artifact whose inner exe
      SHA-256 (the archive's EXE-SHA256SUMS) the manifest names, unpacked
      into the work directory, when the release archive holds it, else
      `--bug-report-replay` on the given or the repository's linux-release
      build. Prints the run's `bug-report-replay: knob` lines (each live
      value applied at the anchor load or at its tick, or not applied and
      why) and its verdict line.
  subtick BUNDLE [--class C] [--key K] [--last N] [--tol-px PX] [--tol-world W]
      The sub-tick present ring (subtick.bin): per object class the records
      drawn over the held presents, then per record the drawn screen and
      world positions present by present against the position its snapshot
      pair lerps to. Flags a record drawn at more than one screen position
      within one present (MULTI) and a paired record drawn off its lerp
      (OFFLERP). BUNDLE may also be an extracted subtick.bin.
  perf BUNDLE
      The performance records: the 30 s pace ring's present and sim
      rollups, the per-tick CPU split (TICK_SPLIT), the eager planner's
      spans (EAGER_PLAN), GPU frame times (GPU_FRAME, under
      OPENPETE_BUGREPORT_GPU) and the coarse session rows
      (pace-coarse.bin). A bundle without a record prints that it holds
      none. BUNDLE may also be an extracted pace.oppl.
  audio BUNDLE [--kind K] [--last N] [--no-ticks]
      The audio device feed at the press (audio-feed.txt): mute and device
      state, the feed's counters, and its event ring (primes, underruns,
      drains, overflows, drops, stretch edges, slips, holds, phase changes,
      a sample every 250 ms of device time) with each event's offset before
      the press, VBL, device frames, ratio in cents, producer speed, drift,
      depth and stretch rate. Exit 3 (VACUOUS) when the feed played data
      and the ring holds no event. BUNDLE may also be an extracted
      audio-feed.txt.
  corpus PATH... [--tsv OUT] [--quiet]
      One row per bundle over directories (every bug-*.zip inside) and
      bundle paths: build, game, level, GPU, refresh, present mode, mods,
      note, and the rollups `perf` computes; prints the TSV (or writes it
      to OUT) and a text summary. A bundle that does not open gets a row
      naming why.
  diff-hashes A B
      First divergent tick and the regions that differ between two
      region-hash CSVs (`frame,vbl,fnv1a64,<region>...`). A or B may be a
      bundle, whose hashes.csv is read.

A bundle written at a fatal exit (manifest `trigger` other than `press`: the
hang watchdog's `hang`, the CdSync spin guard's `cdsync-spin`) holds no
press.state and no screen.png; its hang/ members are that run's dump. Its
replay compares hash rows only, to the last row of hashes.csv.

Exit codes: 0 pass (verified, IDENTICAL), 1 a member fails its manifest
hash or size, 2 DIVERGED, 3 VACUOUS (no rows compared, no verdict line),
64 refused (malformed bundle, bad arguments, replay refused).
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import zipfile
import zlib
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]

SCHEMA = 1
MANIFEST = "manifest.json"
MANIFEST_MAX = 16 << 20       # manifest.json byte bound
MEMBER_MAX = 1 << 30          # one member's uncompressed bound
TOTAL_MAX = 8 << 30           # the bundle's uncompressed bound
TEXT_MAX = 8 << 20            # a text member read for display
MEMBERS_MAX = 100000          # member count bound

# NTSC rates (VBL = 2250000/37609 Hz, tick = half of it).
VBL_HZ = 2250000 / 37609
TICK_HZ = 1125000 / 37609

EXIT_OK, EXIT_MISMATCH, EXIT_DIVERGED, EXIT_VACUOUS, EXIT_REFUSED = 0, 1, 2, 3, 64

# Pace log layout (core/include/openpete_pace_log.h).
OPPL_MAGIC = 0x4C50504F
OPPL_HEADER_FIXED = 64
OPPL_LAYOUT = {48: "<BBHIqqqqq", 64: "<BBHIqqqqqqq"}
PACE_VBL, PACE_PRESENT, PACE_SHED, PACE_SLIP = 1, 2, 3, 4
PACE_TICK_SPLIT, PACE_EAGER_PLAN, PACE_GPU_FRAME = 15, 16, 17
PACE_CANONICAL, PACE_INTERMEDIATE, PACE_REPEAT = 0, 1, 2
PACE_F_NOT_LIVE = 64
PACE_EP_CUT, PACE_EP_LATE, PACE_EP_PINNED, PACE_EP_GREEDY = 1, 2, 4, 8
PACE_EP_RING, PACE_EP_WITHHELD = 16, 32
PACE_WINDOW_S = 30.0
# TICK_SPLIT slots in record order: (slot, high word) per duration (core/include/openpete_pace_log.h).
SPLIT_FIELDS = (("sim", 0, 0), ("extract", 0, 1), ("terrain", 1, 0), ("moby", 1, 1),
                ("snap_rex", 2, 0), ("snap_canon", 2, 1), ("render", 3, 0), ("present", 3, 1))
# Coarse session ring (pace-coarse.bin): header and the 32 u32 fields of a row.
COARSE_MAGIC = b"OPPCRS01"
COARSE_HEADER = struct.Struct("<8sIIIIqq")
COARSE_FIELDS = ("t0_s", "span_s", "vbls", "vbls_not_live", "presents", "repeats", "sheds", "iv_max_us",
                 "iv_long", "ticks", "tick_us_sum", "tick_us_max", "sim_us_sum", "extract_us_sum",
                 "terrain_us_sum", "moby_us_sum", "snap_us_sum", "render_us_sum", "present_us_sum",
                 "extracts", "plans", "plan_slots", "plan_scenes", "plan_cuts", "level_max", "gpu_frames",
                 "gpu_us_sum", "gpu_us_max", "slips", "reserved0", "reserved1", "reserved2")
COARSE_MAX_FIELDS = {"iv_max_us", "tick_us_max", "level_max", "gpu_us_max"}

# The tarfile extraction filter refusing links out of the tree, where the runtime has one.
TAR_FILTER = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}

# Sub-tick present ring layout (core/include/openpete_bugrec_subtick.h).
SUB_MAGIC = b"OPSUBTK1"
SUB_HEADER = struct.Struct("<8sIIIIIIQQ")
SUB_PRESENT = struct.Struct("<QqqqqIIIIfBBBBiIQIIq")
SUB_OBJ = struct.Struct("<IIHBB3i3i3B3BH3f3fiif")
SUB_PATHS = {0: "eager", 1: "inline", 2: "repeat"}
SUB_VERDICTS = {0: "paired", 1: "newborn", 2: "dying", 3: "held"}
SUB_F_SPEC, SUB_F_GHOST, SUB_F_REFPREV, SUB_F_NOOBJ, SUB_F_WORLD, SUB_F_UNSHOWN = 1, 2, 4, 8, 16, 32
SUB_KF = {0: "snap", 1: "lerp", 2: "hold", 3: "?"}

# audio-feed.txt (core/host/audio_feed_report.c), layout version 1.
AUDIO_VERSION = 1
AUDIO_COLS = ("t_s", "vbl", "consumed", "event", "phase", "flags", "ratio", "speed", "drift", "depth_s",
              "st_rate", "fifo", "underruns", "slips", "behind", "pushed", "value")
AUDIO_INT_COLS = {"vbl", "consumed", "fifo", "underruns", "slips", "pushed"}
AUDIO_TEXT_COLS = {"event", "phase", "flags"}
AUDIO_KV_RE = re.compile(r"([A-Za-z_0-9]+)=(-?[0-9][0-9.e+-]*|[A-Za-z_]+)")

VERDICT_RE = re.compile(r"bug-report-replay:\s*(IDENTICAL|DIVERGED|REFUSED)\b.*")


class BundleError(Exception):
    """A bundle or input this tool refuses; the message names why."""


# --------------------------------------------------------------------------
# bundle access

def member_name_ok(name):
    """Return None when a member name is safe to read and extract, else the reason."""
    if not name or "\0" in name:
        return "empty name or NUL byte"
    if "\\" in name:
        return "backslash in name"
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        return "absolute name"
    parts = PurePosixPath(name).parts
    if any(p == ".." for p in parts):
        return "'..' component"
    if name.endswith("/"):
        return None
    if any(p in ("", ".") for p in name.split("/")):
        return "empty or '.' component"
    return None


class Bundle:
    """An opened bundle: its zip, validated member table and parsed manifest."""

    def __init__(self, path):
        self.path = Path(path)
        if not self.path.is_file():
            raise BundleError(f"{path}: not a file")
        try:
            self.zip = zipfile.ZipFile(self.path)
        except (zipfile.BadZipFile, OSError, EOFError, ValueError) as e:
            raise BundleError(f"{path}: not a readable zip ({e})")
        try:
            infos = self.zip.infolist()
        except (zipfile.BadZipFile, OSError, ValueError) as e:
            raise BundleError(f"{path}: zip directory unreadable ({e})")
        if len(infos) > MEMBERS_MAX:
            raise BundleError(f"{path}: {len(infos)} members, over the bound {MEMBERS_MAX}")
        self.infos = {}
        total = 0
        for zi in infos:
            why = member_name_ok(zi.filename)
            if why:
                raise BundleError(f"{path}: member {zi.filename!r} refused: {why}")
            if zi.filename.endswith("/"):
                continue
            mode = (zi.external_attr >> 16) & 0xFFFF
            if stat.S_ISLNK(mode):
                raise BundleError(f"{path}: member {zi.filename!r} refused: symbolic link")
            if zi.filename in self.infos:
                raise BundleError(f"{path}: member {zi.filename!r} refused: duplicate name")
            if zi.file_size > MEMBER_MAX:
                raise BundleError(f"{path}: member {zi.filename!r} is {zi.file_size} B, over the bound {MEMBER_MAX}")
            total += zi.file_size
            self.infos[zi.filename] = zi
        if total > TOTAL_MAX:
            raise BundleError(f"{path}: {total} B uncompressed, over the bound {TOTAL_MAX}")
        if MANIFEST not in self.infos:
            raise BundleError(f"{path}: no {MANIFEST}; not a bug-report bundle")
        raw = self.read(MANIFEST, MANIFEST_MAX)
        try:
            self.manifest = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as e:
            raise BundleError(f"{path}: {MANIFEST} does not parse ({e})")
        if not isinstance(self.manifest, dict):
            raise BundleError(f"{path}: {MANIFEST} is not an object")
        members = self.manifest.get("members")
        if not isinstance(members, list):
            raise BundleError(f"{path}: {MANIFEST} has no member list")
        self.listed = {}
        for m in members:
            if not isinstance(m, dict) or not isinstance(m.get("name"), str):
                raise BundleError(f"{path}: {MANIFEST} member entry malformed: {m!r}")
            why = member_name_ok(m["name"])
            if why:
                raise BundleError(f"{path}: {MANIFEST} lists {m['name']!r}: {why}")
            self.listed[m["name"]] = m
        self.schema = self.manifest.get("schema")

    def has(self, name):
        return name in self.infos

    def stream(self, name, bound=MEMBER_MAX):
        """Yield a member's bytes in chunks, refusing more than its declared size or the bound."""
        zi = self.infos.get(name)
        if zi is None:
            raise BundleError(f"{self.path}: no member {name!r}")
        if zi.file_size > bound:
            raise BundleError(f"{self.path}: member {name!r} is {zi.file_size} B, over the bound {bound}")
        got = 0
        try:
            with self.zip.open(zi) as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    got += len(b)
                    if got > zi.file_size:
                        raise BundleError(f"{self.path}: member {name!r} inflates past its declared size")
                    yield b
        except (zipfile.BadZipFile, OSError, EOFError, ValueError, RuntimeError) as e:
            raise BundleError(f"{self.path}: member {name!r} unreadable ({e})")

    def read(self, name, bound=MEMBER_MAX):
        return b"".join(self.stream(name, bound))

    def text(self, name, bound=TEXT_MAX):
        """A text member decoded as UTF-8 (replacing), its last `bound` bytes when longer; None when absent."""
        if not self.has(name):
            return None
        zi = self.infos[name]
        if zi.file_size <= bound:
            return self.read(name).decode("utf-8", "replace")
        tail = bytearray()
        for b in self.stream(name):
            tail += b
            if len(tail) > 2 * bound:
                del tail[: len(tail) - bound]
        return bytes(tail[-bound:]).decode("utf-8", "replace")

    def member_hash(self, name):
        h = hashlib.sha256()
        n = 0
        for b in self.stream(name):
            h.update(b)
            n += len(b)
        return n, h.hexdigest()

    def verify(self):
        """Check every listed member's size and SHA-256 and the zip's unlisted members.

        @return a list of (name, status, detail): status is ok, size, hash,
        missing or unlisted."""
        out = []
        for name, m in self.listed.items():
            if not self.has(name):
                out.append((name, "missing", "listed, not in the zip"))
                continue
            n, h = self.member_hash(name)
            if m.get("size") is not None and n != m.get("size"):
                out.append((name, "size", f"{n} B, manifest {m.get('size')}"))
            elif h != m.get("sha256"):
                out.append((name, "hash", f"{h[:16]}, manifest {str(m.get('sha256'))[:16]}"))
            else:
                out.append((name, "ok", ""))
        for name in self.infos:
            if name != MANIFEST and name not in self.listed:
                out.append((name, "unlisted", "in the zip, not in the manifest"))
        return out


# --------------------------------------------------------------------------
# parsers

def parse_kv(text):
    """key=value lines into a dict (first occurrence wins); other lines ignored."""
    d = {}
    for line in (text or "").splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            d.setdefault(k.strip(), v.strip())
    return d


def pace_summary(data, window_s=PACE_WINDOW_S):
    """Present-interval statistics over the last window_s seconds of an OPPL ring.

    @return a dict, or a dict with key `error`."""
    if len(data) < OPPL_HEADER_FIXED:
        return {"error": f"{len(data)} B, shorter than the header"}
    (magic, version, header_bytes, rec_bytes, env_bytes, _r, _c, _e,
     vbl_num, vbl_den, _tv) = struct.unpack_from("<IIIIIIqqqqq", data, 0)
    if magic != OPPL_MAGIC:
        return {"error": f"bad magic 0x{magic:08x}"}
    fmt = OPPL_LAYOUT.get(rec_bytes)
    if fmt is None:
        return {"error": f"record size {rec_bytes} unknown"}
    if header_bytes < OPPL_HEADER_FIXED or header_bytes > len(data):
        return {"error": f"header size {header_bytes} out of range"}
    body = data[header_bytes:]
    body = body[: len(body) // rec_bytes * rec_bytes]
    pres, vbls, slips, sheds = [], [], 0, 0
    for r in struct.iter_unpack(fmt, body):
        typ, flags, t = r[0], r[1], r[4]
        if typ == PACE_PRESENT:
            pres.append((t, flags))
        elif typ == PACE_VBL:
            vbls.append(t)
        elif typ == PACE_SLIP:
            slips += 1
        elif typ == PACE_SHED:
            sheds += 1
    times = [t for t, _ in pres] + vbls
    out = {"records": len(body) // rec_bytes, "version": version, "window_s": window_s}
    if not times:
        out["presents"] = 0
        return out
    end = max(times)
    start = end - int(window_s * 1e9)
    win = sorted(t for t, _ in pres if t >= start)
    out["presents"] = len(win)
    out["repeats"] = sum(1 for t, f in pres if t >= start and f == PACE_REPEAT)
    out["vbls"] = sum(1 for t in vbls if t >= start)
    out["span_s"] = (end - min(t for t in times if t >= start)) / 1e9
    out["slips"], out["sheds"] = slips, sheds
    iv = [(b - a) / 1e6 for a, b in zip(win, win[1:])]
    if iv:
        s = sorted(iv)
        p50 = s[len(s) // 2]
        out["p50_ms"] = p50
        out["p99_ms"] = s[min(len(s) - 1, int(len(s) * 0.99))]
        out["max_ms"] = s[-1]
        out["hitch_ms"] = 2 * p50
        out["hitches"] = sum(1 for x in iv if x > 2 * p50)
    return out


def pace_records(data):
    """Parse an OPPL image into (header dict, list of record tuples); raises BundleError naming the fault.

    A record tuple is (type, flags, aux, index, t_ns, a, b, c, d, e, f); a v1
    (48 B) record has e and f as None."""
    if len(data) < OPPL_HEADER_FIXED:
        raise BundleError(f"pace log: {len(data)} B, shorter than the header")
    (magic, version, header_bytes, rec_bytes, env_bytes, _r, created, epoch,
     vbl_num, vbl_den, tick_vbls) = struct.unpack_from("<IIIIIIqqqqq", data, 0)
    if magic != OPPL_MAGIC:
        raise BundleError(f"pace log: bad magic 0x{magic:08x}")
    fmt = OPPL_LAYOUT.get(rec_bytes)
    if fmt is None:
        raise BundleError(f"pace log: record size {rec_bytes} unknown")
    if header_bytes < OPPL_HEADER_FIXED or header_bytes > len(data) or env_bytes > header_bytes - OPPL_HEADER_FIXED:
        raise BundleError(f"pace log: header size {header_bytes} out of range")
    env = parse_kv(data[OPPL_HEADER_FIXED:OPPL_HEADER_FIXED + env_bytes].decode("utf-8", "replace"))
    body = data[header_bytes:]
    body = body[: len(body) // rec_bytes * rec_bytes]
    recs = list(struct.iter_unpack(fmt, body))
    if rec_bytes == 48:
        recs = [r + (None, None) for r in recs]
    hdr = {"version": version, "record_bytes": rec_bytes, "created_unix_ns": created, "epoch_ns": epoch,
           "vbl_hz": vbl_num / vbl_den if vbl_den else VBL_HZ, "tick_vbls": tick_vbls, "env": env}
    return hdr, recs


def pct(xs, p):
    """The p-th percentile of xs by nearest rank (index int(p/100 * n), clamped); None for an empty list."""
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(p / 100.0 * len(s)))]


def u32w(v, hi):
    """The low (hi=0) or high (hi=1) unsigned 32-bit word of a signed 64-bit slot."""
    return ((v or 0) >> (32 * hi)) & 0xFFFFFFFF


def split_decode(r):
    """A TICK_SPLIT record as a dict: index, t_ns, durations in ms, counts, the drawing VBL's own run in ms."""
    slots = (r[5], r[6], r[7], r[8])
    d = {"index": r[3], "t_ns": r[4]}
    for name, slot, hi in SPLIT_FIELDS:
        d[name] = u32w(slots[slot], hi) / 1000.0
    e = r[9] or 0
    d["n_extract"], d["n_render"] = e & 0xFFFF, (e >> 16) & 0xFFFF
    d["n_present"], d["n_snap"] = (e >> 32) & 0xFFFF, (e >> 48) & 0xFFFF
    d["tick"] = u32w(r[10], 0) / 1000.0
    return d


def plan_decode(r):
    """An EAGER_PLAN record as a dict (the fields of the OPENPETE_EAGER_DBG plan line; times in ms)."""
    f = r[10] or 0
    return {"index": r[3], "t_ns": r[4], "flags": r[1], "cut": bool(r[1] & PACE_EP_CUT),
            "ring": bool(r[1] & PACE_EP_RING), "withheld": bool(r[1] & PACE_EP_WITHHELD),
            "late": bool(r[1] & PACE_EP_LATE), "level": r[2] & 0xFF, "std": (r[2] >> 8) & 0xFF,
            "budget": r[5] / 1e6, "tail": r[6] / 1e6, "lag": r[7] / 1e6, "vbl": r[8] / 1e6,
            "late_ms": (r[9] or 0) / 1e6, "slots": f & 0xFFFF, "afford": (f >> 16) & 0xFFFF,
            "planned": (f >> 32) & 0xFFFF, "period_ms": ((f >> 48) & 0xFFFF) / 1000.0}


def gpu_decode(r):
    """A GPU_FRAME record as a dict: token, zones, frame span and depth-0 busy time in ms."""
    return {"token": r[7], "zones": r[2], "frame_ms": r[5] / 1e6, "busy_ms": r[6] / 1e6, "t_ns": r[4]}


def coarse_parse(data):
    """Parse a pace-coarse.bin image into (header dict, rows as dicts); raises BundleError naming the fault."""
    if len(data) < COARSE_HEADER.size:
        raise BundleError("pace-coarse.bin: shorter than its header")
    magic, ver, rb, n, span, created, at = COARSE_HEADER.unpack_from(data, 0)
    if magic != COARSE_MAGIC:
        raise BundleError("pace-coarse.bin: bad magic")
    if ver != 1 or rb != 4 * len(COARSE_FIELDS):
        raise BundleError(f"pace-coarse.bin: version {ver} row bytes {rb}, this reader knows 1 / {4 * len(COARSE_FIELDS)}")
    if COARSE_HEADER.size + n * rb > len(data):
        raise BundleError(f"pace-coarse.bin: {n} rows do not fit {len(data)} B")
    fmt = struct.Struct("<%dI" % len(COARSE_FIELDS))
    rows = []
    for i in range(n):
        v = dict(zip(COARSE_FIELDS, fmt.unpack_from(data, COARSE_HEADER.size + i * rb)))
        v["t0_s"], v["span_s"] = i * span, span
        rows.append(v)
    return {"version": ver, "rows": n, "span_s": span, "created_unix_ns": created, "at_ns": at}, rows


def tick_runs(vbls):
    """The drawing VBL's fiber run in ms per tick: the longer run of each VBL pair (index // 2)."""
    by = {}
    for r in vbls:
        run = ((r[8] or 0) - (r[7] or 0)) / 1e6
        k = r[3] // 2
        by[k] = max(by.get(k, 0.0), run)
    return list(by.values())


def perf_rollup(data, tail_s=10.0):
    """Rollups of an OPPL image: presents and intervals over the window and the last tail_s seconds,
    the sim tick, sheds and scenes per tick, and the TICK_SPLIT, EAGER_PLAN and GPU_FRAME records.

    @return a dict; keys of a record kind the image does not hold are absent, and
    `types` lists the record types present."""
    hdr, recs = pace_records(data)
    out = {"pace_records": len(recs), "env": hdr["env"], "types": sorted({r[0] for r in recs})}
    pres = [r for r in recs if r[0] == PACE_PRESENT]
    vbls = [r for r in recs if r[0] == PACE_VBL]
    times = [r[4] for r in pres] + [r[4] for r in vbls]
    if not times:
        return out
    end, start = max(times), min(times)
    out["window_s"] = (end - start) / 1e9
    out["vbls"] = len(vbls)
    out["not_live"] = sum(1 for r in vbls if r[1] & PACE_F_NOT_LIVE)
    shown = sorted(r[4] for r in pres)
    out["presents"] = len(shown)
    out["repeats"] = sum(1 for r in pres if r[1] == PACE_REPEAT)
    canon = sum(1 for r in pres if r[1] == PACE_CANONICAL)
    inter = sum(1 for r in pres if r[1] == PACE_INTERMEDIATE)
    sheds = sum(1 for r in recs if r[0] == PACE_SHED)
    out["sheds"] = sheds
    if out["window_s"] > 0:
        out["pres_s"] = len(shown) / out["window_s"]
    tail0 = end - int(tail_s * 1e9)
    tail = [t for t in shown if t >= tail0]
    if tail and out["window_s"] >= tail_s:
        out["pres_s_tail"] = len(tail) / tail_s
    iv = [(b - a) / 1e6 for a, b in zip(shown, shown[1:])]
    if iv:
        out["iv_p50"], out["iv_p99"], out["iv_max"] = pct(iv, 50), pct(iv, 99), max(iv)
    if canon:
        out["scenes_tick"] = (canon + inter) / canon
        out["sheds_tick"] = sheds / canon
    splits = [split_decode(r) for r in recs if r[0] == PACE_TICK_SPLIT]
    runs = [d["tick"] for d in splits] if splits else tick_runs(vbls)
    if runs:
        out["sim_p50"], out["sim_p90"] = pct(runs, 50), pct(runs, 90)
    if splits:
        out["split_ticks"] = len(splits)
        for name, _s, _h in SPLIT_FIELDS:
            out["split_" + name] = pct([d[name] for d in splits], 50)
    plans = [plan_decode(r) for r in recs if r[0] == PACE_EAGER_PLAN]
    if plans:
        n = len(plans)
        out["plans"] = n
        out["plan_slots"] = sum(p["slots"] for p in plans) / n
        out["plan_afford"] = sum(p["afford"] for p in plans) / n
        out["plan_scenes"] = sum(p["planned"] for p in plans) / n
        out["plan_level_p50"], out["plan_level_max"] = pct([p["level"] for p in plans], 50), max(p["level"] for p in plans)
        out["plan_cut_frac"] = sum(1 for p in plans if p["cut"]) / n
        out["plan_cut_ring_frac"] = sum(1 for p in plans if p["cut"] and p["ring"]) / n
        out["plan_budget_p50"] = pct([p["budget"] for p in plans], 50)
    gpus = [gpu_decode(r) for r in recs if r[0] == PACE_GPU_FRAME]
    if gpus:
        out["gpu_frames"] = len(gpus)
        out["gpu_p50"], out["gpu_p99"] = pct([g["frame_ms"] for g in gpus], 50), pct([g["frame_ms"] for g in gpus], 99)
    return out


def coarse_rollup(rows):
    """Session rollups of coarse rows: covered seconds, and the per-row present rate's p10/p50/p90 over
    rows with VBLs mostly live, with the first and last such rows' rates."""
    live = [r for r in rows if r["vbls"] and r["vbls_not_live"] * 2 < r["vbls"]]
    out = {"coarse_rows": len(rows), "coarse_span_s": rows[0]["span_s"] if rows else 0,
           "coarse_s": sum(r["span_s"] for r in rows if r["vbls"])}
    rates = [(r["presents"] + r["repeats"]) / r["span_s"] for r in live if r["span_s"]]
    if rates:
        out["coarse_pres_p10"], out["coarse_pres_p50"], out["coarse_pres_p90"] = pct(rates, 10), pct(rates, 50), pct(rates, 90)
        out["coarse_pres_first"], out["coarse_pres_last"] = rates[0], rates[-1]
    return out


PNG_SIG = b"\x89PNG\r\n\x1a\n"
PNG_PIXELS_MAX = 1 << 26       # a thumbnail's decoded bound (w * h)


def png_check(data):
    """Decode a PNG's structure: chunk CRCs, IHDR, and the IDAT stream inflated to h rows of 1 + w * channels bytes.

    @return (width, height) when it decodes; raises ValueError naming the fault."""
    if not data.startswith(PNG_SIG):
        raise ValueError("no PNG signature")
    pos, ihdr, idat = len(PNG_SIG), None, []
    while pos + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n or pos + 12 + n > len(data):
            raise ValueError("truncated chunk")
        (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise ValueError(f"chunk {kind!r} CRC mismatch")
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + n
    if not ihdr:
        raise ValueError("no IHDR")
    w, h, depth, ctype = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
    chans = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or not chans or not w or not h or w * h > PNG_PIXELS_MAX:
        raise ValueError(f"unsupported IHDR {w}x{h} depth {depth} type {ctype}")
    d = zlib.decompressobj()
    need = h * (1 + w * chans)
    raw = d.decompress(b"".join(idat), need + 1)
    if len(raw) != need:
        raise ValueError(f"IDAT inflates to {len(raw)} B, rows need {need}")
    if any(raw[r * (1 + w * chans)] > 4 for r in range(h)):
        raise ValueError("bad row filter byte")
    return w, h


def thumbs_line(b, m):
    """The thumbnail ring's summary: count, size, span, rate and decode check, from the manifest and the members."""
    names = sorted(n for n in b.listed if n.startswith("thumbs/") and n.endswith(".png"))
    th = m.get("thumbs") if isinstance(m.get("thumbs"), dict) else {}
    if not names:
        return f"thumbnails: none ({th.get('state') or 'not in this bundle'})"
    bad, size = 0, None
    for n in names:
        try:
            size = png_check(b.read(n))
        except (ValueError, BundleError, zlib.error):
            bad += 1
    span = th.get("span_ms")
    line = f"thumbnails: {len(names)}"
    if size:
        line += f" ({size[0]}x{size[1]})"
    if isinstance(span, (int, float)):
        line += f", span {span / 1000:.2f} s"
        if len(names) > 1 and span > 0:
            line += f", {(len(names) - 1) * 1000 / span:.2f} Hz"
    if isinstance(th.get("newest_age_ms"), (int, float)):
        line += f", newest {th['newest_age_ms'] / 1000:.2f} s before the press"
    return line + ("; all decode" if not bad else f"; {bad} do not decode")


def subtick_parse(data):
    """Parse a subtick.bin image into (header dict, presents, entries); each present gains its entry list."""
    if len(data) < SUB_HEADER.size:
        raise BundleError("subtick.bin: shorter than its header")
    magic, ver, psz, osz, npres, nobj, pdrop, base, outside = SUB_HEADER.unpack_from(data, 0)
    if magic != SUB_MAGIC:
        raise BundleError("subtick.bin: bad magic")
    if psz != SUB_PRESENT.size or osz != SUB_OBJ.size:
        raise BundleError(f"subtick.bin: version {ver} record sizes {psz}/{osz}, this reader knows "
                          f"{SUB_PRESENT.size}/{SUB_OBJ.size}")
    if len(data) < SUB_HEADER.size + npres * psz + nobj * osz:
        raise BundleError("subtick.bin: shorter than its counts")
    hdr = {"version": ver, "presents": npres, "entries": nobj, "presents_dropped": pdrop,
           "first_entry": base, "outside": outside}
    names = ("seq", "present_ns", "target_ns", "tick_prev_ns", "tick_curr_ns", "tick_prev", "tick_curr",
             "feed_key", "vbl", "alpha", "kind", "paired", "path", "late", "slot", "scene_id",
             "obj_first", "obj_n", "stamp", "period_ns")
    off = SUB_HEADER.size
    pres = []
    for i in range(npres):
        pres.append(dict(zip(names, SUB_PRESENT.unpack_from(data, off + i * psz))))
    off += npres * psz
    objs = []
    for i in range(nobj):
        v = SUB_OBJ.unpack_from(data, off + i * osz)
        objs.append({"key": v[0], "moby": v[1], "cls": v[2], "verdict": v[3], "flags": v[4],
                     "prev_pos": v[5:8], "curr_pos": v[8:11], "prev_rot": v[11:14], "curr_rot": v[14:17],
                     "nverts": v[17], "world": v[18:21], "sx": v[21], "sy": v[22], "vz": v[23],
                     "prev_dt_us": v[24], "curr_dt_us": v[25], "kf_alpha": v[26]})
    for p in pres:
        lo = p["obj_first"] - base
        p["truncated"] = lo < 0 or lo + p["obj_n"] > nobj
        p["objs"] = [] if p["truncated"] else objs[lo:lo + p["obj_n"]]
        for e in p["objs"]:
            e["present"] = p
    return hdr, pres, objs


def subtick_expected(e, period_ns):
    """The world position a record's snapshot pair lerps to at its present, and the pair's phase t; (None, None) without a pair."""
    if not any(e["prev_pos"]) and not any(e["curr_pos"]):
        return None, None
    pv, cv = e["prev_pos"], e["curr_pos"]
    if e["verdict"] == 3:
        return tuple(float(x) for x in pv), 0.0
    if e["verdict"] != 0:
        return tuple(float(x) for x in cv), 1.0
    span = e["curr_dt_us"] - e["prev_dt_us"]
    cap = period_ns / 1000.0 if period_ns > 0 else span
    span = min(span, cap) if span > 0 else 0
    t = 1.0 if span <= 0 else max(0.0, min(1.0, -e["curr_dt_us"] / span))
    return tuple(c + (p - c) * (1.0 - t) for p, c in zip(pv, cv)), t


def subtick_findings(pres, tol_px, tol_world):
    """Per present, the keys drawn at more than one screen position (MULTI) and the paired entries drawn off their lerp (OFFLERP).

    @return (multi, offlerp): multi a list of (present, key, entries), offlerp a list of (entry, error, t)."""
    multi, off = [], []
    for p in pres:
        by = {}
        for e in p["objs"]:
            by.setdefault(e["key"], []).append(e)
        for k, es in by.items():
            if len(es) > 1 and any(abs(a["sx"] - es[0]["sx"]) > tol_px or abs(a["sy"] - es[0]["sy"]) > tol_px
                                   for a in es[1:]):
                multi.append((p, k, es))
        for e in p["objs"]:
            if not (e["flags"] & SUB_F_WORLD) or e["verdict"] != 0:
                continue
            x, t = subtick_expected(e, p["period_ns"])
            if x is None:
                continue
            err = max(abs(a - b) for a, b in zip(e["world"], x))
            snap = max(abs(a - b) for a, b in zip(e["world"], e["curr_pos"]))
            if err > tol_world and snap > tol_world:
                off.append((e, err, t))
    return multi, off


def subtick_load(arg):
    """The subtick.bin bytes of a bundle or of an extracted member; None when a bundle holds none."""
    pth = Path(arg)
    if pth.is_file() and not zipfile.is_zipfile(pth):
        return pth.read_bytes()
    b = Bundle(arg)
    return b.read("subtick.bin") if b.has("subtick.bin") else None


def subtick_line(b):
    """The info line for the sub-tick present ring: presents by kind and path, span, entries, and the finding counts."""
    if not b.has("subtick.bin"):
        return "subtick: no subtick.bin"
    try:
        hdr, pres, objs = subtick_parse(b.read("subtick.bin"))
    except BundleError as e:
        return f"subtick: unreadable ({e})"
    if not pres:
        return "subtick: no present held"
    kinds = {}
    for p in pres:
        k = ("canonical" if p["kind"] == 0 else "intermediate") + "/" + SUB_PATHS.get(p["path"], "?")
        kinds[k] = kinds.get(k, 0) + 1
    span = (pres[-1]["present_ns"] - pres[0]["present_ns"]) / 1e9
    multi, off = subtick_findings(pres, 2.0, 2.0)
    trunc = sum(1 for p in pres if p["truncated"])
    return (f"subtick: {len(pres)} presents over {span:.2f} s ("
            + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())) + f"), {len(objs)} drawn records"
            + (f", {trunc} presents truncated" if trunc else "")
            + f"; MULTI {len(multi)} OFFLERP {len(off)} (python3 -I tools/bugreport.py subtick)")


def audio_parse(text):
    """Parse audio-feed.txt into (header, counters, events): header key=value lines (`note` a list), the
    `spu:`/`feed:` counter fields keyed `<line>.<key>` (`feed.pushed`, `feed.accounting.data_out`,
    `feed.now.device`, ...), and one dict per event row with AUDIO_COLS typed."""
    hdr, cnt, evs = {"note": []}, {}, []
    for ln in text.splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        if ln.startswith(("spu:", "feed:")):
            head, _sep, rest = ln.partition(": ")
            sub = rest.split(":", 1)[0] if re.match(r"^[a-z]+:", rest) else ""
            pre = head + ("." + sub if sub and not sub.startswith("phase") else "")
            if sub.startswith("phase"):
                continue
            for k, v in AUDIO_KV_RE.findall(rest):
                cnt.setdefault(f"{pre}.{k}", v)
            continue
        if "=" in ln.split(" ", 1)[0]:
            k, v = ln.split("=", 1)
            if k == "note":
                hdr["note"].append(v)
            else:
                hdr[k] = v
            continue
        f = ln.split()
        if len(f) != len(AUDIO_COLS):
            raise BundleError(f"audio-feed.txt: event row with {len(f)} fields, want {len(AUDIO_COLS)}: {ln[:80]}")
        try:
            evs.append({c: (v if c in AUDIO_TEXT_COLS else int(v) if c in AUDIO_INT_COLS else float(v))
                        for c, v in zip(AUDIO_COLS, f)})
        except ValueError:
            raise BundleError(f"audio-feed.txt: unparseable event row: {ln[:80]}")
    if "version" not in hdr:
        raise BundleError("audio-feed.txt: no version line")
    if int(hdr["version"]) != AUDIO_VERSION:
        raise BundleError(f"audio-feed.txt: version {hdr['version']}, this reader knows {AUDIO_VERSION}")
    if len(evs) != int(hdr.get("events_held", -1)):
        raise BundleError(f"audio-feed.txt: {len(evs)} event rows, header says {hdr.get('events_held')}")
    return hdr, cnt, evs


def audio_played(cnt):
    """Output frames of data the device played (0 when the counters were not read)."""
    try:
        return int(cnt.get("feed.accounting.data_out", 0))
    except ValueError:
        return 0


def audio_load(arg):
    """The audio-feed.txt text of a bundle or of an extracted member; None when a bundle holds none."""
    p = Path(arg)
    if p.suffix == ".txt" and p.is_file():
        return p.read_text(errors="replace")
    b = Bundle(arg)
    return b.text("audio-feed.txt")


def audio_line(b):
    """The info line for the audio feed: mute, device, events by kind and span, underruns, stretch, pitch."""
    txt = b.text("audio-feed.txt")
    if txt is None:
        return "audio: no audio-feed.txt (written by builds that record the feed's event ring)"
    try:
        hdr, cnt, evs = audio_parse(txt)
    except BundleError as e:
        return f"audio: unreadable ({e})"
    kinds = {}
    for e in evs:
        kinds[e["event"]] = kinds.get(e["event"], 0) + 1
    span = (evs[-1]["t_s"] - evs[0]["t_s"]) if evs else 0.0
    line = (f"audio: mute={hdr.get('mute')} device={hdr.get('device')}; {len(evs)} events over {span:.1f} s ("
            + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())) + ")")
    if "feed.pushed" not in cnt:
        line += "; counters: " + ("no frame pushed or consumed" if cnt else "not read")
    else:
        line += (f"; underruns {cnt.get('feed.underruns', '?')} primes {cnt.get('feed.primes', '?')} "
                 f"stretch engages {cnt.get('feed.stretch.engages', '?')} on={cnt.get('feed.stretch.on', '?')} "
                 f"speed {cnt.get('feed.drift.speed', '?')} pitch p99 {cnt.get('feed.pitch.p99', '?')}")
    if audio_played(cnt) and not evs:
        line += "; VACUOUS: the feed played data and holds no event"
    return line + " (python3 -I tools/bugreport.py audio)"


def records_line(b):
    """The `info` line naming the performance records a bundle holds, with the `perf` command."""
    have = []
    if b.has("pace.oppl"):
        try:
            _h, recs = pace_records(b.read("pace.oppl"))
        except BundleError as e:
            return f"perf records: pace.oppl unreadable ({e})"
        n = {}
        for r in recs:
            if r[0] in (PACE_TICK_SPLIT, PACE_EAGER_PLAN, PACE_GPU_FRAME):
                n[r[0]] = n.get(r[0], 0) + 1
        have += [f"{n.get(PACE_TICK_SPLIT, 0)} TICK_SPLIT", f"{n.get(PACE_EAGER_PLAN, 0)} EAGER_PLAN",
                 f"{n.get(PACE_GPU_FRAME, 0)} GPU_FRAME"]
    have.append("pace-coarse.bin" if b.has("pace-coarse.bin") else "no pace-coarse.bin")
    if not b.manifest.get("records"):
        have.append("manifest names no records (older writer)")
    return "perf records: " + ", ".join(have) + " (python3 -I tools/bugreport.py perf)"


def knobs_parse(text):
    """knobs.txt: the session fields, the `knob` rows and the `change` rows (tab-separated); @return a dict."""
    session, rows, changes = {}, [], []
    for line in text.splitlines():
        f = line.split("\t")
        if f[0] == "session":
            session = dict(x.split("=", 1) for x in f[1:] if "=" in x)
        elif f[0] == "knob" and len(f) >= 11:
            frame, _, t = f[10].partition("@")
            rows.append({"id": f[1], "cls": f[2], "value": f[3], "boot": f[4], "default": f[5],
                         "source": f[6], "flags": f[7], "raw": f[8], "changes": int(f[9]) if f[9].isdigit() else 0,
                         "last_frame": int(frame) if frame.isdigit() else None,
                         "last_t": float(t) if t else None})
        elif f[0] == "change" and len(f) >= 10:
            changes.append({"frame": int(f[1]) if f[1].isdigit() else 0, "t": f[4], "id": f[5],
                            "old": f[6], "value": f[7]})
    return {"session": session, "rows": rows, "changes": changes}


def knobs_lines(b):
    """The `info` lines of knobs.txt: each knob changed live after boot, with its time, then the knobs whose value differs from the declared default."""
    text = b.text("knobs.txt")
    if text is None:
        return ["knobs: no knobs.txt (written before the member): the values of knobs changed live are unknown; "
                "config.txt holds the boot decisions"]
    k = knobs_parse(text)
    live = [r for r in k["rows"] if "C" in r["flags"] or "L" in r["flags"]]
    out = []
    for r in live:
        when = (f"changed live at {r['last_t']:.1f} s, frame {r['last_frame']}" if r["last_t"] is not None
                else "differs from its boot value")
        back = "" if "L" in r["flags"] else f", back to its boot value after {r['changes']} change(s)"
        out.append(f"knob changed live: {r['id']} = {r['value']} ({when}{back}; boot {r['boot']}, "
                   f"default {r['default']}; {r['cls']})")
    if not live:
        out.append("knobs changed live: none")
    dflt = [r for r in k["rows"] if "D" in r["flags"] and r not in live]
    if dflt:
        out.append("knobs off default: " + ", ".join(f"{r['id']}={r['value']} (default {r['default']}, {r['source']})"
                                                       for r in dflt))
    s = k["session"]
    if s.get("dropped", "0") != "0":
        out.append(f"knobs: {s.get('dropped')} older change(s) dropped from the ring of {s.get('kept')}")
    return out


def note_line(b, m):
    """The tester's note, or why there is none."""
    txt = b.text("notes.txt")
    if txt is not None:
        return "note: " + txt.strip().replace("\n", " ")[:500]
    st = (m.get("note") or {}).get("state") if isinstance(m.get("note"), dict) else None
    return f"note: none ({st or 'not recorded'})"


def read_hash_csv(source):
    """Parse a region-hash CSV (bytes or text) into (header, {frame: row}, order)."""
    if isinstance(source, bytes):
        source = source.decode("utf-8", "replace")
    rd = csv.reader(io.StringIO(source))
    try:
        header = next(rd)
    except StopIteration:
        raise BundleError("hash CSV is empty")
    if header[:3] != ["frame", "vbl", "fnv1a64"]:
        raise BundleError(f"hash CSV header starts {header[:3]}, expected frame,vbl,fnv1a64")
    rows, order = {}, []
    for ln, r in enumerate(rd, 2):
        if not r:
            continue
        if len(r) != len(header):
            raise BundleError(f"hash CSV line {ln}: {len(r)} fields, header has {len(header)}")
        try:
            fr = int(r[0])
        except ValueError:
            raise BundleError(f"hash CSV line {ln}: tick {r[0]!r} is not an integer")
        if fr not in rows:
            order.append(fr)
        rows[fr] = r
    return header, rows, order


def load_hashes(arg):
    """A hash CSV from a path: a CSV file, or a bundle's hashes.csv."""
    p = Path(arg)
    if not p.is_file():
        raise BundleError(f"{arg}: not a file")
    with open(p, "rb") as f:
        head = f.read(4)
    if head == b"PK\x03\x04":
        b = Bundle(p)
        if not b.has("hashes.csv"):
            raise BundleError(f"{arg}: bundle has no hashes.csv")
        return read_hash_csv(b.read("hashes.csv"))
    if p.stat().st_size > MEMBER_MAX:
        raise BundleError(f"{arg}: over the bound {MEMBER_MAX} B")
    return read_hash_csv(p.read_bytes())


def diff_hashes(a, b):
    """Compare two parsed hash CSVs on the ticks both hold.

    @return (exit code, lines)."""
    ha, ra, oa = a
    hb, rb, ob = b
    lines = []
    cols_a, cols_b = ha[3:], hb[3:]
    common = [c for c in cols_a if c in cols_b]
    if cols_a != cols_b:
        lines.append(f"columns differ: {len(cols_a)} vs {len(cols_b)} regions, "
                     f"{len(common)} common; only A: {[c for c in cols_a if c not in cols_b][:8]} "
                     f"only B: {[c for c in cols_b if c not in cols_a][:8]}")
    ia = {c: i for i, c in enumerate(ha)}
    ib = {c: i for i, c in enumerate(hb)}
    both = [f for f in oa if f in rb]
    only_a = len(oa) - len(both)
    only_b = sum(1 for f in ob if f not in ra)
    lines.append(f"ticks: A {len(oa)} ({oa[0] if oa else '-'}..{oa[-1] if oa else '-'}), "
                 f"B {len(ob)} ({ob[0] if ob else '-'}..{ob[-1] if ob else '-'}), "
                 f"common {len(both)}, only A {only_a}, only B {only_b}")
    if not both:
        lines.append("VACUOUS rows=0 (no tick in both files)")
        return EXIT_VACUOUS, lines
    for f in both:
        x, y = ra[f], rb[f]
        regions = [c for c in common if x[ia[c]] != y[ib[c]]]
        total = x[2] != y[2] if cols_a == cols_b else False
        if regions or total:
            n = both.index(f)
            vbl = x[1] if x[1] == y[1] else f"{x[1]}/{y[1]}"
            shown = ",".join(regions[:16]) + (f",+{len(regions) - 16}" if len(regions) > 16 else "")
            lines.append(f"DIVERGED tick={f} vbl={vbl} row={n} regions={shown or '(total only)'}"
                         f" total={'differs' if x[2] != y[2] else 'equal'}")
            for c in regions[:16]:
                lines.append(f"  {c}: A {x[ia[c]]} B {y[ib[c]]}")
            later = sum(1 for g in both[n + 1:] if ra[g][1:] != rb[g][1:])
            lines.append(f"  rows differing after it: {later} of {len(both) - n - 1}")
            return EXIT_DIVERGED, lines
    lines.append(f"IDENTICAL rows={len(both)}")
    return EXIT_OK, lines


# --------------------------------------------------------------------------
# replay route

def archive_dir(arg=None):
    """The release archive root: --archive, else OPENPETE_RELEASE_ARCHIVE_DIR, else the XDG data default."""
    if arg:
        return Path(arg)
    env = os.environ.get("OPENPETE_RELEASE_ARCHIVE_DIR")
    if env:
        return Path(env)
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return Path(base) / "openpete" / "release-archive"


def file_sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def parse_exe_sums(text):
    """Parse EXE-SHA256SUMS lines, `<hex>  <artifact>:<path inside>`.

    An artifact name holds no `:` or `/` and the inner path is relative
    with no `..`; a line breaking either, or with a hex field that is not 64
    hex digits, is skipped.
    @return a list of (hex, artifact, inner path)."""
    out = []
    for line in text.splitlines():
        m = re.match(r"^([0-9A-Fa-f]{64}) [ *]([^:/\\]+):(.+)$", line.rstrip("\r"))
        if not m or member_name_ok(m.group(3)):
            continue
        out.append((m.group(1).lower(), m.group(2), m.group(3)))
    return out


def artifact_kind(name):
    if name.endswith(".AppImage"):
        return "appimage"
    if re.search(r"\.tar(\.(xz|gz|bz2))?$", name):
        return "tarball"
    if name.endswith(".zip"):
        return "windows-zip"
    return None


def find_archived(man, arch):
    """Find the archived artifact holding the exe the manifest names.

    The manifest's exe_sha256 is the hash of the running binary, the
    executable inside the package (inside an AppImage, the inner binary), so
    the lookup reads each archive directory's EXE-SHA256SUMS, which the
    archive writes from the packager's sidecar. Looks in
    <arch>/<build_version>-<build_commit>/ first, then every directory of
    the archive.
    @return (artifact path, path inside it, kind) or None."""
    want = str(man.get("exe_sha256") or "").lower()
    if not want or not arch.is_dir():
        return None
    first = arch / f"{man.get('build_version', '')}-{man.get('build_commit', '')}"
    dirs = [first] if first.is_dir() else []
    dirs += sorted(d for d in arch.iterdir() if d.is_dir() and d != first)
    for d in dirs:
        s = d / "EXE-SHA256SUMS"
        if not s.is_file():
            continue
        for h, art, inner in parse_exe_sums(s.read_text("utf-8", "replace")):
            kind = artifact_kind(art)
            if h == want and kind and (d / art).is_file():
                return d / art, inner, kind
    return None


def default_exe():
    return ROOT / "build" / "linux-release" / "openpete"


def default_disc(man):
    game = str(man.get("game") or "")
    disc = PurePosixPath(str(man.get("disc") or "")).name
    if not game or not disc:
        return None
    return ROOT / "games" / game / game / "build" / disc


def replay_plan(b, exe=None, disc=None, arch=None, work=None):
    """Choose the route and build the command; nothing runs.

    @return a dict: route, label, exe, cmd (list or None), env, note."""
    man = b.manifest
    arch = archive_dir(arch)
    disc = Path(disc) if disc else default_disc(man)
    work = (Path(work) if work else Path("bugreport-replay") / b.path.stem).resolve()
    hit = None if exe else find_archived(man, arch)
    env = {}
    if man.get("module_march"):
        env["OPENPETE_MODULE_MARCH"] = str(man["module_march"])
    plan = {"env": env, "disc": disc, "work": work, "archive": arch,
            "want_sha": str(man.get("exe_sha256") or "").lower()}
    if hit:
        art, member, kind = hit
        plan.update(route="primary", artifact=art, member=member, kind=kind)
        if kind == "tarball":
            plan["exe"] = work / "artifact" / member
        elif kind == "appimage":
            # --appimage-extract writes squashfs-root/ under the working
            # directory; its AppRun sets the bundled library path and execs
            # the inner binary, with no FUSE mount.
            plan["exe"] = work / "artifact" / "squashfs-root" / "AppRun"
        else:
            plan["exe"] = None
            plan["note"] = (f"Windows artifact {art.name} ({member}), extracted to {work / 'artifact'}: "
                            f"run {member} on the Windows host with --bug-report-replay")
    else:
        plan.update(route="fallback", exe=Path(exe).resolve() if exe else default_exe())
        here = None
        if plan["exe"] and Path(plan["exe"]).is_file():
            try:
                here = file_sha(plan["exe"])
            except OSError:
                here = None
        plan["same_exe"] = here == man.get("exe_sha256")
        why = "no archived artifact holds exe " + str(man.get("exe_sha256", ""))[:16]
        if exe:
            why = "--exe given"
        plan["note"] = why + (" (same exe SHA-256 as the tester's)" if plan["same_exe"] else
                              " (cross-build unless the codegen stamp matches; labels come from the run)")
    cmd = None
    if plan.get("exe"):
        # Headless with input locked and rumble off: an unattended run.
        cmd = [str(plan["exe"]), "--game", str(man.get("game", "")), "--headless", "--input-lock",
               "--vibration", "0", "--mute", "1", "--disc", str(disc), "--bug-report-replay", str(b.path.resolve())]
        cards = work / "cards"
        for p in (1, 2):
            if b.has(f"card{p}.mcd"):
                cmd += [f"--card{p}", str(cards / f"card{p}.mcd")]
            else:
                cmd += [f"--no-card{p}" if p == 2 else "--no-card"]
                if p == 1:
                    # --no-card already covers slot 2; drop the slot-2 pass.
                    break
    plan["cmd"] = cmd
    return plan


def plan_command_text(plan):
    if not plan.get("cmd"):
        return None
    env = " ".join(f"{k}={shlex.quote(v)}" for k, v in plan["env"].items())
    return (env + " " if env else "") + "OPENPETE_VIBRATION=0 " + " ".join(shlex.quote(c) for c in plan["cmd"])


# --------------------------------------------------------------------------
# info

def fmt_size(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


def counter_line(label, a):
    if not isinstance(a, dict):
        return f"{label}: none"
    return (f"{label}: frame {a.get('frame')} vbl {a.get('vbl')} feed_key {a.get('feed_key')} "
            f"level {a.get('level')} park ra={a.get('park_ra')} sp={a.get('park_sp')} "
            f"capture {a.get('capture_us')} us")


def log_problems(text, n):
    lines = [l for l in (text or "").splitlines() if "[warn" in l or "[error" in l or "[fatal" in l]
    return lines[-n:] if n > 0 else []


def cmd_info(args):
    b = Bundle(args.bundle)
    m = b.manifest
    o = []
    w = o.append
    w(f"bundle {b.path.name}: schema {b.schema}"
      + ("" if b.schema == SCHEMA else f" (this reader knows schema {SCHEMA}; fields may be missing)"))
    trig = m.get("trigger") or "press"
    if trig != "press":
        w(f"trigger {trig}: written at a fatal exit on the {m.get('exit_thread') or '?'} thread; "
          "no press.state or screen.png; hang/ holds that run's dump")
    o.extend(knobs_lines(b))
    w(note_line(b, m))
    w(f"build {m.get('build_version')} tagged={m.get('build_tagged')} commit {m.get('build_commit')}")
    w(f"exe_sha256 {m.get('exe_sha256')}")
    mod = parse_kv(b.text("module.txt"))
    w(f"module {mod.get('linkage', '?').split(' ')[0]} sha256 {m.get('module_sha256') or '-'}"
      f" abi {mod.get('abi', '?')} march {m.get('module_march') or '(unset)'}")
    w(f"codegen_stamp {m.get('codegen_stamp')}")
    w(f"rom_stamp {m.get('rom_stamp') or '(empty: static build)'}")
    w(f"state_ident {m.get('state_ident')}  overlays {len(m.get('overlays') or [])} ({m.get('overlays_source')})")
    w(f"game {m.get('game')}  disc {m.get('disc')} sha256 {str(m.get('disc_sha256'))[:16]}")
    mods = m.get("mods") or []
    w("mods: " + (", ".join(f"{x.get('id')} {x.get('version')}{' mutative' if x.get('mutative') else ''}"
                            for x in mods) if mods else "none enabled"))
    argv = m.get("argv") or []
    w("argv: " + " ".join(shlex.quote(str(a)) for a in argv[1:]))
    pr, an = m.get("press"), m.get("anchor")
    w(counter_line("press ", pr))
    w(counter_line("anchor", an))
    if isinstance(an, dict) and isinstance(pr, dict):
        df = (pr.get("frame") or 0) - (an.get("frame") or 0)
        dv = (pr.get("vbl") or 0) - (an.get("vbl") or 0)
        dk = (pr.get("feed_key") or 0) - (an.get("feed_key") or 0)
        # openpete_frame steps once per VSync(0) yield; the anchor cadence
        # (OPENPETE_BUGREC_CADENCE_VBLS) counts it at the VBL rate.
        w(f"anchor age: frame +{df} ({df / VBL_HZ:.1f} s at the VBL rate), vbl +{dv}, feed_key +{dk}; "
          f"window_ticks {m.get('window_ticks')} segment_rows {m.get('segment_rows')} hash_rows {m.get('hash_rows')}")
    elif isinstance(an, dict):
        w(f"anchor age: no press snapshot (fatal exit); segment_rows {m.get('segment_rows')} "
          f"hash_rows {m.get('hash_rows')} from the anchor to the exit")
    else:
        w("anchor age: no anchor held (no replay window)")
    taint = m.get("taint") or {}
    nz = {k: v for k, v in taint.items() if v}
    w(f"taint: {sum(taint.values()) if taint else 0} total"
      + (" (" + ", ".join(f"{k}={v}" for k, v in nz.items()) + ")" if nz else "")
      + f"  answers {m.get('answers')}  press {m.get('press_us')} us display {m.get('display_us')} us")
    summ = parse_kv(b.text("summary.txt"))
    shown = {"game", "game_name", "frame", "vbl", "feed_key", "park_ra", "park_sp"}
    rest = [f"{k}={v}" for k, v in summ.items() if k not in shown]
    if b.text("summary.txt") is None:
        w("summary.txt: absent")
    for i in range(0, len(rest), 6):
        w(("state: " if i == 0 else "       ") + " ".join(rest[i:i + 6]))
    envt = [l for l in (b.text("env.txt") or "").splitlines() if l.strip()]
    w("env: " + (" ".join(envt) if envt else "(no OPENPETE_* set)"))
    red = m.get("redaction") or {}
    lst = red.get("list") or []
    by = {}
    for f in lst:
        k = (f.get("member"), f.get("section") or "-", f.get("what"))
        by[k] = by.get(k, 0) + 1
    w(f"redaction: {red.get('replacements')} replacement(s), {red.get('findings')} finding(s)"
      + (f" ({len(lst)} listed)" if red.get("findings") != len(lst) else ""))
    for (mem, sec, what), n in sorted(by.items(), key=lambda kv: -kv[1])[:6]:
        w(f"  {n:4d} {what} in {mem} [{sec}]")
    ver = b.verify()
    bad = [v for v in ver if v[1] != "ok"]
    total = sum(x.get("size") or 0 for x in b.listed.values())
    w(f"members: {len(b.listed)} listed, {fmt_size(total)}; hash check "
      + ("all match" if not bad else f"{len(bad)} FAIL"))
    if args.members:
        for name, x in b.listed.items():
            w(f"  {x.get('size', 0):>11} {name}")
    else:
        big = sorted(b.listed.values(), key=lambda x: -(x.get("size") or 0))[:6]
        w("  largest: " + ", ".join(f"{x['name']} {fmt_size(x.get('size') or 0)}" for x in big))
        w("  " + " ".join(sorted(n for n in b.listed if "/" not in n)))
        dirs = {}
        for n in b.listed:
            if "/" in n:
                d = n.split("/")[0] + ("/" + n.split("/")[1] if n.startswith("mods/") else "")
                dirs[d] = dirs.get(d, 0) + 1
        if dirs:
            w("  " + " ".join(f"{d}/({c})" for d, c in sorted(dirs.items())))
    for name, st, det in bad:
        w(f"  {st.upper()} {name}: {det}")
    w(thumbs_line(b, m))
    w(subtick_line(b))
    w(audio_line(b))
    if b.has("pace.oppl"):
        ps = pace_summary(b.read("pace.oppl"))
        if "error" in ps:
            w(f"pace: pace.oppl unreadable: {ps['error']}")
        elif not ps.get("presents"):
            w(f"pace: {ps['records']} record(s), no present in the last 30 s (headless or no display)")
        else:
            line = (f"pace (last {ps['span_s']:.1f} s): {ps['presents']} presents ({ps['repeats']} repeat), "
                    f"{ps['vbls']} VBLs")
            if "p50_ms" in ps:
                line += (f"; present interval p50 {ps['p50_ms']:.2f} p99 {ps['p99_ms']:.2f} "
                         f"max {ps['max_ms']:.2f} ms; hitches {ps['hitches']} (> {ps['hitch_ms']:.2f} ms)")
            w(line + f"; slips {ps['slips']} sheds {ps['sheds']} (whole ring)")
    else:
        w("pace: no pace.oppl")
    w(records_line(b))
    dev = parse_kv(b.text("device.txt"))
    w("device: " + " ".join(f"{k}={dev[k]}" for k in ("platform", "os_release_PRETTY_NAME", "kernel", "cpu",
                                                     "logical_cpus", "ram_mib", "wsl") if k in dev))
    logt = b.text("log.txt")
    probs = log_problems(logt, args.log_lines)
    w(f"log.txt: " + ("absent" if logt is None else
                       f"{len(logt.splitlines())} lines; last {len(probs)} WARN/ERROR:"))
    for line in probs:
        w("  " + line.strip()[:200])
    plan = replay_plan(b, arch=args.archive)
    if plan["route"] == "primary":
        w(f"replay route: primary, archived artifact {plan['artifact']} ({plan['kind']}), "
          f"exe {plan['member']} (EXE-SHA256SUMS)")
    else:
        w(f"replay route: fallback (--bug-report-replay on the current build): {plan['note']}")
    if not isinstance(an, dict):
        w("  no anchor: the bundle carries no replay window; --bug-report-replay refuses it")
    if plan.get("note") and plan["route"] == "primary":
        w("  " + plan["note"])
    w(f"  python3 -I {shlex.quote(str(Path(__file__).resolve()))} replay {shlex.quote(str(b.path))}")
    txt = plan_command_text(plan)
    if txt:
        w(f"  runs: {txt}")
    print("\n".join(o))
    return EXIT_MISMATCH if bad else EXIT_OK


# --------------------------------------------------------------------------
# extract, replay, diff-hashes

def safe_target(root, name):
    t = (root / name).resolve()
    if t != root and root not in t.parents:
        raise BundleError(f"member {name!r} resolves outside {root}")
    return t


def extract_to(b, dest, names=None):
    """Write members under dest, verifying each listed member; @return the failure list."""
    root = Path(dest).resolve()
    root.mkdir(parents=True, exist_ok=True)
    fails = []
    for name in (names if names is not None else list(b.infos)):
        t = safe_target(root, name)
        t.parent.mkdir(parents=True, exist_ok=True)
        h = hashlib.sha256()
        n = 0
        tmp = t.with_name(t.name + ".part")
        with open(tmp, "wb") as f:
            for chunk in b.stream(name):
                f.write(chunk)
                h.update(chunk)
                n += len(chunk)
        os.replace(tmp, t)
        if name == MANIFEST:
            continue
        m = b.listed.get(name)
        if m is None:
            fails.append((name, "unlisted"))
        elif n != m.get("size") or h.hexdigest() != m.get("sha256"):
            fails.append((name, f"size {n}/{m.get('size')} sha256 {h.hexdigest()[:16]}/{str(m.get('sha256'))[:16]}"))
    if names is None:
        for name in b.listed:
            if not b.has(name):
                fails.append((name, "missing"))
    return fails


def cmd_extract(args):
    b = Bundle(args.bundle)
    fails = extract_to(b, args.dir)
    print(f"extracted {len(b.infos)} member(s) to {args.dir}; "
          + ("every listed member matches its manifest size and SHA-256" if not fails
             else f"{len(fails)} FAIL"))
    for name, why in fails:
        print(f"  FAIL {name}: {why}")
    return EXIT_MISMATCH if fails else EXIT_OK


def unpack_artifact(plan):
    """Unpack the primary route's artifact under <work>/artifact/ and check its exe's SHA-256.

    A tarball is untarred (unsafe names, links and devices skipped), a
    Windows zip unzipped (unsafe names skipped), an AppImage extracted
    with its own `--appimage-extract`, which needs no FUSE.
    @return None, or the reason the artifact cannot be used."""
    art, kind, member = Path(plan["artifact"]), plan["kind"], plan["member"]
    dest = Path(plan["work"]) / "artifact"
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    try:
        if kind == "tarball":
            with tarfile.open(art) as t:
                for ti in t:
                    if member_name_ok(ti.name) or not (ti.isfile() or ti.isdir()):
                        continue
                    t.extract(ti, dest, **TAR_FILTER)
        elif kind == "windows-zip":
            with zipfile.ZipFile(art) as z:
                for zi in z.infolist():
                    if not member_name_ok(zi.filename):
                        z.extract(zi, dest)
        elif kind == "appimage":
            img = dest / art.name
            shutil.copyfile(art, img)
            img.chmod(0o755)
            r = subprocess.run([str(img), "--appimage-extract"], cwd=dest, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=600)
            img.unlink()
            if r.returncode != 0:
                return f"{art.name} --appimage-extract exit {r.returncode}: {r.stderr[-200:]!r}"
    except (tarfile.TarError, zipfile.BadZipFile, OSError, subprocess.TimeoutExpired) as e:
        return f"cannot unpack {art.name}: {e}"
    exe = dest / ("squashfs-root" if kind == "appimage" else "") / member
    if not exe.is_file():
        return f"{art.name} holds no {member}"
    got = file_sha(exe)
    if got != str(plan.get("want_sha") or got):
        return f"{art.name}:{member} SHA-256 {got[:16]} is not the manifest's {str(plan['want_sha'])[:16]}"
    return None


def cmd_replay(args):
    # wslenv-guard: no-launch (runs Linux executables only; a Windows artifact's command is printed, not run)
    b = Bundle(args.bundle)
    bad = [v for v in b.verify() if v[1] != "ok"]
    if bad:
        for name, st, det in bad:
            print(f"refused: {st} {name}: {det}")
        return EXIT_REFUSED
    if not isinstance(b.manifest.get("anchor"), dict):
        print("refused: the bundle holds no anchor, so there is no window to replay")
        return EXIT_REFUSED
    plan = replay_plan(b, exe=args.exe, disc=args.disc, arch=args.archive, work=args.work)
    work = Path(plan["work"])
    print(f"route: {plan['route']}" + (f" ({plan['note']})" if plan.get("note") else ""))
    if plan["route"] == "primary":
        print(f"artifact: {plan['artifact']} ({plan['kind']}), exe {plan['member']}")
    if not plan.get("cmd") and not (plan["route"] == "primary" and not args.dry_run):
        print("no command runs on this host")
        return EXIT_REFUSED
    if not args.dry_run:
        if not Path(plan["disc"]).is_file():
            print(f"refused: disc {plan['disc']} not found (pass --disc)")
            return EXIT_REFUSED
        cards = [f"card{p}.mcd" for p in (1, 2) if b.has(f"card{p}.mcd")]
        if cards:
            fails = extract_to(b, work / "cards", cards)
            if fails:
                print(f"refused: card member fails its hash: {fails}")
                return EXIT_REFUSED
        if plan["route"] == "primary":
            why = unpack_artifact(plan)
            if why:
                print(f"refused: {why}")
                return EXIT_REFUSED
    if not plan.get("cmd"):
        print("no command runs on this host")
        return EXIT_REFUSED
    cmd = plan["cmd"] + list(args.extra or [])
    env = dict(os.environ)
    env.update(plan["env"])
    env["OPENPETE_VIBRATION"] = "0"
    print("command: " + plan_command_text(dict(plan, cmd=cmd)))
    if args.dry_run:
        return EXIT_OK
    work.mkdir(parents=True, exist_ok=True)
    log = work / "replay.log"
    log.unlink(missing_ok=True)
    try:
        with open(log, "wb") as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env, timeout=args.timeout)
        rc = r.returncode
    except subprocess.TimeoutExpired:
        print(f"timeout after {args.timeout} s; log {log}")
        return EXIT_VACUOUS
    except OSError as e:
        print(f"refused: cannot run {cmd[0]}: {e}")
        return EXIT_REFUSED
    text = log.read_text("utf-8", "replace")
    found = [mt.group(0) for mt in VERDICT_RE.finditer(text)]
    print(f"exit {rc}; log {log}")
    for line in text.splitlines():
        if "bug-report-replay: knob" in line:
            print(line[line.index("bug-report-replay: knob"):])
    if "unknown arg: --bug-report-replay" in text:
        print(f"refused: {cmd[0]} predates --bug-report-replay; pass --exe a build that has it")
        return EXIT_REFUSED
    if not found:
        print("no 'bug-report-replay:' verdict line in the run's output")
        tail = [l for l in text.splitlines() if l.strip()][-8:]
        for l in tail:
            print("  " + l[:200])
        return EXIT_VACUOUS if rc in (0, 2) else (rc or EXIT_VACUOUS)
    print(found[-1])
    return rc


def cmd_subtick(args):
    data = subtick_load(args.bundle)
    if data is None:
        print("no subtick.bin in this bundle")
        return EXIT_VACUOUS
    hdr, pres, objs = subtick_parse(data)
    o = []
    w = o.append
    if not pres:
        print("subtick.bin holds no present")
        return EXIT_VACUOUS
    span = (pres[-1]["present_ns"] - pres[0]["present_ns"]) / 1e9
    w(f"subtick.bin v{hdr['version']}: {len(pres)} presents over {span:.3f} s, {len(objs)} drawn records; "
      f"ring dropped {hdr['presents_dropped']} presents and {hdr['first_entry']} records; "
      f"{hdr['outside']} records offered outside a present")
    cls_f = int(args.cls, 0) if args.cls else None
    key_f = int(args.key, 0) if args.key else None
    sel = [e for e in objs if "present" in e and (cls_f is None or e["cls"] == cls_f)
           and (key_f is None or e["key"] == key_f)]
    by_cls = {}
    for e in sel:
        c = by_cls.setdefault(e["cls"], {"n": 0, "keys": set(), "pres": set(), "inter": 0})
        c["n"] += 1
        c["keys"].add(e["key"])
        c["pres"].add(e["present"]["seq"])
        c["inter"] += e["present"]["kind"] != 0
    w("class  records  keys  presents  on-intermediate")
    for c, v in sorted(by_cls.items(), key=lambda kv: -kv[1]["n"]):
        w(f"0x{c:03x} {v['n']:8d} {len(v['keys']):5d} {len(v['pres']):9d} {v['inter']:16d}")
    multi, off = subtick_findings(pres, args.tol_px, args.tol_world)
    multi = [x for x in multi if (key_f is None or x[1] == key_f) and (cls_f is None or x[2][0]["cls"] == cls_f)]
    off = [x for x in off if x[0] in sel] if (cls_f is not None or key_f is not None) else off
    w(f"MULTI (one record at more than one screen position within a present, > {args.tol_px} px): {len(multi)}")
    for p, k, es in multi[:args.flag_lines]:
        w(f"  seq {p['seq']} key {p['feed_key']} a={p['alpha']:.3f} rec {k:08x} cls 0x{es[0]['cls']:03x} x{len(es)}: "
          + " ".join(f"({e['sx']:.1f},{e['sy']:.1f}){'S' if e['flags'] & SUB_F_SPEC else ''}"
                     f"{'G' if e['flags'] & SUB_F_GHOST else ''}" for e in es))
    w(f"OFFLERP (paired record drawn more than {args.tol_world} world units off its lerp and off curr): {len(off)}")
    for e, err, t in off[:args.flag_lines]:
        p = e["present"]
        w(f"  seq {p['seq']} key {p['feed_key']} a={p['alpha']:.3f} rec {e['key']:08x} cls 0x{e['cls']:03x} "
          f"t={t:.3f} err {err:.1f} drawn ({e['world'][0]:.0f},{e['world'][1]:.0f},{e['world'][2]:.0f}) "
          f"prev {e['prev_pos']} curr {e['curr_pos']}")
    keys = sorted({e["key"] for e in sel}) if (cls_f is not None or key_f is not None) else []
    for k in keys[:args.max_keys]:
        es = [e for e in sel if e["key"] == k][-args.last:]
        w(f"record {k:08x} (moby {es[-1]['moby']:08x} cls 0x{es[-1]['cls']:03x}), last {len(es)} draws:")
        w("     seq  feed  kind path    alpha verdict  kf    t      sx      sy       vz   world x,y,z"
          "                lerp x,y,z")
        for e in es:
            p = e["present"]
            x, t = subtick_expected(e, p["period_ns"])
            wx = "(" + ",".join(f"{v:.0f}" for v in e["world"]) + ")" if e["flags"] & SUB_F_WORLD else "-"
            lx = "(" + ",".join(f"{v:.0f}" for v in x) + ")" if x else "-"
            tag = ("S" if e["flags"] & SUB_F_SPEC else "") + ("G" if e["flags"] & SUB_F_GHOST else "") + \
                  ("P" if e["flags"] & SUB_F_REFPREV else "")
            w(f"  {p['seq']:6d} {p['feed_key']:5d} {'C' if p['kind'] == 0 else 'I'}    {SUB_PATHS.get(p['path'], '?'):6s}"
              f" {p['alpha']:6.3f} {SUB_VERDICTS.get(e['verdict'], '?'):7s} {SUB_KF[(e['flags'] >> 6) & 3]:4s}"
              f" {t if t is not None else float('nan'):5.3f} {e['sx']:7.1f} {e['sy']:7.1f} {e['vz']:8.1f}"
              f"   {wx:24s} {lx} {tag}")
    print("\n".join(o))
    return EXIT_DIVERGED if (multi or off) else EXIT_OK


def cmd_audio(args):
    txt = audio_load(args.bundle)
    if txt is None:
        print("no audio-feed.txt in this bundle")
        return EXIT_VACUOUS
    hdr, cnt, evs = audio_parse(txt)
    o = []
    w = o.append
    w(f"audio-feed.txt v{hdr['version']}: mute={hdr.get('mute')} device={hdr.get('device')} "
      f"counters={hdr.get('counters')}; events {hdr.get('events_held')} held of {hdr.get('events_total')} "
      f"recorded (ring {hdr.get('ring')}, {hdr.get('events_skipped')} skipped at the copy); "
      f"snapshot at VBL {hdr.get('snapshot_vbl')}")
    for n in hdr["note"]:
        w("note: " + n)
    for ln in txt.splitlines():
        if ln.startswith(("spu:", "feed:")):
            w(ln)
    sel = [e for e in evs if (not args.kind or e["event"] in args.kind.split(","))
           and not (args.no_ticks and e["event"] == "tick")]
    if args.last:
        sel = sel[-args.last:]
    w(f"events ({len(sel)} of {len(evs)} listed): t_s before the snapshot, VBL, device frames, ratio in cents")
    w(f"{'t_s':>9} {'vbl':>7} {'consumed':>10} {'event':<12} {'phase':<9} {'fl':<3} {'cents':>8} {'speed':>7} "
      f"{'drift':>9} {'depth':>6} {'st_rate':>7} {'fifo':>6} {'undr':>4} {'slips':>5} {'behind':>7} {'value':>9}")
    for e in sel:
        cents = 1200.0 * math.log2(e["ratio"]) if e["ratio"] > 0 else float("nan")
        w(f"{e['t_s']:9.3f} {e['vbl']:7d} {e['consumed']:10d} {e['event']:<12} {e['phase']:<9} {e['flags']:<3} "
          f"{cents:+8.3f} {e['speed']:7.4f} {e['drift']:9.6f} {e['depth_s']:6.0f} {e['st_rate']:7.4f} "
          f"{e['fifo']:6d} {e['underruns']:4d} {e['slips']:5d} {e['behind']:7.3f} {e['value']:9.1f}")
    rc = EXIT_OK
    if audio_played(cnt) and not evs:
        w("VACUOUS: the feed played data and the ring holds no event")
        rc = EXIT_VACUOUS
    print("\n".join(o))
    return rc


def perf_source(arg):
    """(pace bytes or None, coarse bytes or None, manifest or None) of a bundle or an extracted pace log."""
    pth = Path(arg)
    if pth.is_file() and not zipfile.is_zipfile(pth):
        if pth.stat().st_size > MEMBER_MAX:
            raise BundleError(f"{arg}: over the bound {MEMBER_MAX}")
        return pth.read_bytes(), None, None
    b = Bundle(arg)
    return (b.read("pace.oppl") if b.has("pace.oppl") else None,
            b.read("pace-coarse.bin") if b.has("pace-coarse.bin") else None, b.manifest)


def fmt_ms(v, nd=2):
    """A float to nd decimals, or '-' for None."""
    return "-" if v is None else f"{v:.{nd}f}"


def perf_lines(pace, coarse, manifest):
    """The `perf` report lines for a pace image, a coarse image and a manifest (each may be None)."""
    o = []
    w = o.append
    rec = (manifest or {}).get("records")
    if manifest is not None:
        w("records: " + (json.dumps(rec, sort_keys=True) if rec else
                         "the manifest names none (a bundle written before the performance records)"))
    if pace is None:
        w("pace: no pace.oppl")
    else:
        r = perf_rollup(pace)
        env = r.get("env", {})
        w(f"pace: {r['pace_records']} records, types {r['types']}; gpu {env.get('gpu', '?')} refresh "
          f"{int(env.get('refresh_mhz', 0) or 0) / 1000:.3f} Hz {env.get('present_mode', '?')} eager={env.get('eager', '?')}")
        if "window_s" in r:
            w(f"  window {r['window_s']:.1f} s: {r['presents']} presents ({fmt_ms(r.get('pres_s'), 1)}/s, last 10 s "
              f"{fmt_ms(r.get('pres_s_tail'), 1)}/s), interval p50 {fmt_ms(r.get('iv_p50'))} p99 {fmt_ms(r.get('iv_p99'))} "
              f"max {fmt_ms(r.get('iv_max'))} ms; sim tick p50 {fmt_ms(r.get('sim_p50'))} p90 {fmt_ms(r.get('sim_p90'))} ms; "
              f"scenes/tick {fmt_ms(r.get('scenes_tick'))} sheds/tick {fmt_ms(r.get('sheds_tick'))}; "
              f"{r['not_live']} of {r['vbls']} VBLs not live")
        if "split_ticks" in r:
            w(f"  split ({r['split_ticks']} ticks), p50 ms per tick: " +
              " ".join(f"{n} {fmt_ms(r['split_' + n])}" for n, _s, _h in SPLIT_FIELDS))
        elif PACE_TICK_SPLIT not in r["types"]:
            w("  split: no TICK_SPLIT record")
        if "plans" in r:
            w(f"  planner ({r['plans']} spans): slots {r['plan_slots']:.2f} afford {r['plan_afford']:.2f} "
              f"planned {r['plan_scenes']:.2f} per span; level p50 {r['plan_level_p50']} max {r['plan_level_max']}; "
              f"cut {100 * r['plan_cut_frac']:.1f}% (ring {100 * r['plan_cut_ring_frac']:.1f}%); "
              f"budget p50 {r['plan_budget_p50']:.2f} ms")
        else:
            w("  planner: no EAGER_PLAN record (eager production did not run: headless, or not windowed)")
        if "gpu_frames" in r:
            w(f"  gpu ({r['gpu_frames']} frames): frame p50 {r['gpu_p50']:.3f} p99 {r['gpu_p99']:.3f} ms")
        else:
            w("  gpu: no GPU_FRAME record (OPENPETE_BUGREPORT_GPU off)")
    if coarse is None:
        w("coarse: no pace-coarse.bin")
    else:
        ch, rows = coarse_parse(coarse)
        c = coarse_rollup(rows)
        w(f"coarse: {ch['rows']} rows of {ch['span_s']} s, {c['coarse_s']} s with VBLs; present rate per row "
          f"p10 {fmt_ms(c.get('coarse_pres_p10'), 1)} p50 {fmt_ms(c.get('coarse_pres_p50'), 1)} "
          f"p90 {fmt_ms(c.get('coarse_pres_p90'), 1)}/s, first {fmt_ms(c.get('coarse_pres_first'), 1)} "
          f"last {fmt_ms(c.get('coarse_pres_last'), 1)}/s")
    return o


def cmd_perf(args):
    pace, coarse, man = perf_source(args.bundle)
    print("\n".join(perf_lines(pace, coarse, man)))
    if args.rows and coarse is not None:
        _h, rows = coarse_parse(coarse)
        cols = ("t0_s", "span_s", "vbls", "vbls_not_live", "presents", "repeats", "sheds", "iv_max_us", "ticks",
                "tick_us_max", "sim_us_sum", "extract_us_sum", "render_us_sum", "plans", "plan_scenes", "level_max",
                "gpu_frames", "gpu_us_max")
        print("\t".join(cols))
        for r in rows:
            print("\t".join(str(r[k]) for k in cols))
    return EXIT_OK


# Corpus TSV columns, in order: identity, device, then rollups.
CORPUS_COLS = ("bundle", "error", "build", "commit", "game", "level", "trigger", "platform", "gpu", "driver",
               "refresh_hz", "present_mode", "fullscreen", "render_fps", "eager", "extent", "mods", "note",
               "records", "window_s", "pres_s", "pres_s_tail", "iv_p50", "iv_p99", "iv_max", "sim_p50", "sim_p90",
               "sheds_tick", "scenes_tick", "not_live", "split_ticks") + tuple(
                   "split_" + n for n, _s, _h in SPLIT_FIELDS) + (
               "plans", "plan_slots", "plan_afford", "plan_scenes", "plan_level_p50", "plan_level_max",
               "plan_cut_frac", "gpu_frames", "gpu_p50", "gpu_p99", "coarse_s", "coarse_span_s",
               "coarse_pres_p10", "coarse_pres_p50", "coarse_pres_p90", "coarse_pres_first", "coarse_pres_last",
               "audio_device", "audio_mute", "audio_events", "audio_underruns", "audio_stretch_engages",
               "audio_speed", "audio_pitch_p99_c")


def tsv_cell(v):
    """A TSV cell: floats to 3 decimals, None as empty, tabs and newlines as spaces, at most 120 characters."""
    if v is None:
        return ""
    if isinstance(v, float):
        return f"{v:.3f}"
    return re.sub(r"[\t\r\n]+", " ", str(v))[:120]


def corpus_row(path):
    """The corpus row (a dict over CORPUS_COLS) of one bundle; a bundle that does not open gives a row with `error`."""
    row = {"bundle": Path(path).name}
    try:
        b = Bundle(path)
    except BundleError as e:
        row["error"] = str(e)
        return row
    m = b.manifest
    summ = parse_kv(b.text("summary.txt"))
    pr = m.get("press") if isinstance(m.get("press"), dict) else {}
    row.update(build=m.get("build_version"), commit=m.get("build_commit"), game=m.get("game"),
               level=summ.get("level", pr.get("level")), trigger=m.get("trigger") or "press",
               mods=",".join(str(x.get("id")) for x in (m.get("mods") or []) if isinstance(x, dict)) or "none")
    note = b.text("notes.txt", 4096)
    row["note"] = (note or "").strip()[:80] if note else ""
    dev = b.text("device.txt") or ""
    mm = re.search(r"swapchain=(\d+x\d+)", dev)
    row["extent"] = mm.group(1) if mm else None
    rec = m.get("records")
    row["records"] = "pace%s%s" % ("+types" + str(max((rec.get("pace") or {}).get("types") or [0])) if rec else "",
                                   "+coarse" if b.has("pace-coarse.bin") else "")
    errs = []
    if b.has("pace.oppl"):
        try:
            r = perf_rollup(b.read("pace.oppl"))
            env = r.pop("env", {})
            row.update({k: v for k, v in r.items() if k in CORPUS_COLS})
            row.update(platform=env.get("platform"), gpu=env.get("gpu"), driver=env.get("driver"),
                       present_mode=env.get("present_mode"), fullscreen=env.get("fullscreen"),
                       render_fps=env.get("render_fps"), eager=env.get("eager"))
            try:
                row["refresh_hz"] = int(env.get("refresh_mhz") or 0) / 1000.0
            except ValueError:
                row["refresh_hz"] = None
        except BundleError as e:
            errs.append(str(e))
    else:
        errs.append("no pace.oppl")
    if b.has("pace-coarse.bin"):
        try:
            _h, rows = coarse_parse(b.read("pace-coarse.bin"))
            row.update(coarse_rollup(rows))
        except BundleError as e:
            errs.append(str(e))
    row.update(audio_row(b, errs))
    if errs:
        row["error"] = "; ".join(errs)
    return row


def audio_row(b, errs):
    """The corpus row's audio columns from audio-feed.txt; empty for a bundle without one, an error in `errs`."""
    txt = b.text("audio-feed.txt")
    if txt is None:
        return {}
    try:
        hdr, cnt, evs = audio_parse(txt)
    except BundleError as e:
        errs.append(str(e))
        return {}
    return {"audio_device": hdr.get("device"), "audio_mute": hdr.get("mute"), "audio_events": len(evs),
            "audio_underruns": cnt.get("feed.underruns"), "audio_stretch_engages": cnt.get("feed.stretch.engages"),
            "audio_speed": cnt.get("feed.drift.speed"), "audio_pitch_p99_c": cnt.get("feed.pitch.p99")}


def corpus_paths(args):
    """Bundle paths from directories (every bug-*.zip inside, sorted) and file arguments, de-duplicated."""
    out = []
    for a in args:
        p = Path(a)
        if p.is_dir():
            out += sorted(x for x in p.glob("bug-*.zip") if x.is_file())
        else:
            out.append(p)
    seen, uniq = set(), []
    for p in out:
        k = str(p.resolve())
        if k not in seen:
            seen.add(k)
            uniq.append(p)
    return uniq


def present_target(r):
    """The present rate a row aims at: --render-fps when it is below the refresh, else the refresh; None when unknown."""
    hz = r.get("refresh_hz")
    try:
        fps = float(r.get("render_fps") or 0)
    except ValueError:
        fps = 0.0
    if hz and 0 < fps < hz:
        return fps
    return hz or (fps if fps > 0 else None)


def corpus_summary(rows):
    """The corpus text summary: counts by game and GPU, median rollups per game, and the rows furthest below their refresh."""
    o = []
    w = o.append
    ok = [r for r in rows if r.get("pres_s") is not None]
    w(f"corpus: {len(rows)} bundle(s), {len(ok)} with presents, {sum(1 for r in rows if r.get('error'))} with an error; "
      f"{sum(1 for r in rows if r.get('split_ticks'))} with TICK_SPLIT, {sum(1 for r in rows if r.get('plans'))} "
      f"with EAGER_PLAN, {sum(1 for r in rows if r.get('gpu_frames'))} with GPU_FRAME, "
      f"{sum(1 for r in rows if r.get('coarse_s') is not None)} with coarse rows")
    by = {}
    for r in ok:
        by.setdefault((r.get("game"), r.get("gpu")), []).append(r)
    for (g, gpu), rs in sorted(by.items(), key=lambda kv: (str(kv[0][0]), str(kv[0][1]))):
        w(f"  {g} on {gpu}: {len(rs)} bundle(s); median present rate {fmt_ms(pct([x['pres_s'] for x in rs], 50), 1)}/s, "
          f"interval p99 {fmt_ms(pct([x['iv_p99'] for x in rs if x.get('iv_p99') is not None], 50))} ms, "
          f"sim tick p50 {fmt_ms(pct([x['sim_p50'] for x in rs if x.get('sim_p50') is not None], 50))} ms, "
          f"scenes/tick {fmt_ms(pct([x['scenes_tick'] for x in rs if x.get('scenes_tick') is not None], 50))}")
    frac = []
    for r in ok:
        tgt = present_target(r)
        if tgt and r.get("pres_s_tail") is not None:
            frac.append((r["pres_s_tail"] / tgt, tgt, r))
    for f, tgt, r in sorted(frac, key=lambda x: x[0])[:5]:
        w(f"  low: {r['bundle']} {r['game']} level {r['level']}: last 10 s {fmt_ms(r['pres_s_tail'], 1)}/s of a "
          f"{tgt:.0f}/s target ({100 * f:.0f}%), sim p50 {fmt_ms(r.get('sim_p50'))} ms, "
          f"scenes/tick {fmt_ms(r.get('scenes_tick'))}")
    return o


def cmd_corpus(args):
    paths = corpus_paths(args.paths)
    if not paths:
        print("corpus: no bundle found", file=sys.stderr)
        return EXIT_REFUSED
    rows = [corpus_row(p) for p in paths]
    tsv = "\t".join(CORPUS_COLS) + "\n" + "".join(
        "\t".join(tsv_cell(r.get(c)) for c in CORPUS_COLS) + "\n" for r in rows)
    if args.tsv:
        Path(args.tsv).write_text(tsv)
    elif not args.quiet:
        sys.stdout.write(tsv)
    print("\n".join(corpus_summary(rows)))
    return EXIT_OK


def cmd_diff(args):
    a = load_hashes(args.a)
    b = load_hashes(args.b)
    rc, lines = diff_hashes(a, b)
    print("\n".join(lines))
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(prog="bugreport.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info", help="summary a reader starts from")
    p.add_argument("bundle")
    p.add_argument("--log-lines", type=int, default=20, help="WARN/ERROR log lines shown (default 20)")
    p.add_argument("--members", action="store_true", help="list every member with its size")
    p.add_argument("--archive", help="release archive (default ~/.local/share/openpete/release-archive)")
    p = sub.add_parser("extract", help="members to a directory, hashes verified")
    p.add_argument("bundle")
    p.add_argument("dir")
    p = sub.add_parser("replay", help="run the replay route and print the verdict")
    p.add_argument("bundle")
    p.add_argument("--exe", help="build to replay on (skips the archive); default build/linux-release/openpete")
    p.add_argument("--disc", help="disc cue (default games/<game>/<game>/build/<manifest disc>)")
    p.add_argument("--archive", help="release archive directory")
    p.add_argument("--work", help="scratch directory for cards, the extracted artifact and replay.log")
    p.add_argument("--timeout", type=float, default=1800)
    p.add_argument("--dry-run", action="store_true", help="print the route and command only")
    p.add_argument("extra", nargs="*", help="arguments after -- go to the exe")
    p = sub.add_parser("subtick", help="per-record sub-tick draws of the last presents, with MULTI and OFFLERP flags")
    p.add_argument("bundle", help="a bundle, or an extracted subtick.bin")
    p.add_argument("--class", dest="cls", help="object class to list (0x78 = Spyro 2 Sparx)")
    p.add_argument("--key", help="pairing key to list")
    p.add_argument("--last", type=int, default=40, help="draws listed per record (default 40)")
    p.add_argument("--max-keys", type=int, default=8, help="records listed (default 8)")
    p.add_argument("--flag-lines", type=int, default=20, help="flag lines listed per kind (default 20)")
    p.add_argument("--tol-px", type=float, default=2.0, help="MULTI screen tolerance (default 2 px)")
    p.add_argument("--tol-world", type=float, default=2.0, help="OFFLERP world tolerance (default 2 units)")
    p = sub.add_parser("perf", help="performance records: pace rollups, CPU split, planner, GPU, coarse rows")
    p.add_argument("bundle", help="a bundle, or an extracted pace.oppl")
    p.add_argument("--rows", action="store_true", help="also print the coarse rows as TSV")
    p = sub.add_parser("audio", help="the audio feed at the press: mute and device state, counters, event ring")
    p.add_argument("bundle", help="a bundle, or an extracted audio-feed.txt")
    p.add_argument("--kind", help="comma-separated event names to list (prime,UNDERRUN,stretch,slip,...)")
    p.add_argument("--last", type=int, default=0, help="list only the last N selected events")
    p.add_argument("--no-ticks", action="store_true", help="leave out the 250 ms periodic samples")
    p = sub.add_parser("corpus", help="one TSV row per bundle over directories and paths, and a text summary")
    p.add_argument("paths", nargs="+", help="directories (every bug-*.zip inside) and bundle paths")
    p.add_argument("--tsv", help="write the TSV here instead of standard output")
    p.add_argument("--quiet", action="store_true", help="print the summary only")
    p = sub.add_parser("diff-hashes", help="first divergent tick and region of two hash CSVs")
    p.add_argument("a")
    p.add_argument("b")
    args = ap.parse_args(argv)
    fn = {"info": cmd_info, "extract": cmd_extract, "replay": cmd_replay, "subtick": cmd_subtick,
          "audio": cmd_audio, "perf": cmd_perf, "corpus": cmd_corpus, "diff-hashes": cmd_diff}[args.cmd]
    try:
        return fn(args)
    except BundleError as e:
        print(f"refused: {e}", file=sys.stderr)
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main())
