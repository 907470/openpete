#!/usr/bin/env python3
"""bugreport.py - reader for the bug-report bundle (the F9 zip).

A bundle is untrusted input. This tool reads zip members by name with size
bounds, refuses absolute names, `..` components, drive letters, NUL bytes
and duplicate names, and never executes or imports anything from a bundle.
It needs the Python 3 standard library only and runs under `python3 -I`.

Subcommands:

  info BUNDLE [--log-lines N]
      The summary a reader starts from: build, commit, exe SHA-256, game,
      disc, ROM stamp, codegen stamp and march, mods, the press and anchor
      counters and the anchor's age, the state summary, taint counts, the
      member list with sizes and the hash check, redaction findings, a pace
      summary over the last 30 s of present records, the last WARN/ERROR log
      lines, and the replay route with its exact command.
  extract BUNDLE DIR
      Writes every member under DIR and verifies each against the manifest.
  replay BUNDLE [--exe PATH] [--disc CUE] [--archive DIR] [--work DIR]
                [--timeout S] [--dry-run] [-- EXTRA...]
      Runs the route `info` names: the archived artifact whose exe SHA-256
      the manifest names, when the release archive holds it, else
      `--bug-report-replay` on the given or the repository's linux-release
      build. Prints the run's `bug-report-replay:` verdict line.
  diff-hashes A B
      First divergent tick and the regions that differ between two
      region-hash CSVs (`frame,vbl,fnv1a64,<region>...`). A or B may be a
      bundle, whose hashes.csv is read.

Exit codes: 0 pass (verified, IDENTICAL), 1 a member fails its manifest
hash or size, 2 DIVERGED, 3 VACUOUS (no rows compared, no verdict line),
64 refused (malformed bundle, bad arguments, replay refused).
"""
import argparse
import csv
import hashlib
import io
import json
import os
import re
import shlex
import stat
import struct
import subprocess
import sys
import tarfile
import zipfile
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
PACE_REPEAT = 2
PACE_WINDOW_S = 30.0

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
    if arg:
        return Path(arg)
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return Path(base) / "openpete" / "release-archive"


def file_sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def find_archived(man, arch):
    """Find the archived artifact holding the exe the manifest names.

    Looks in <arch>/<build_version>-<build_commit>/ first, then every
    directory of the archive. An artifact matches when its own SHA-256
    (SHA256SUMS line or computed) equals the manifest's exe_sha256, or when
    it is a tarball or zip holding an `openpete` / `openpete.exe` member of
    that SHA-256.
    @return (artifact path, member name or None, kind) or None."""
    want = str(man.get("exe_sha256") or "")
    if not want or not arch.is_dir():
        return None
    first = arch / f"{man.get('build_version', '')}-{man.get('build_commit', '')}"
    dirs = [first] if first.is_dir() else []
    dirs += sorted(d for d in arch.iterdir() if d.is_dir() and d != first)
    for d in dirs:
        sums = {}
        s = d / "SHA256SUMS"
        if s.is_file():
            for line in s.read_text("utf-8", "replace").splitlines():
                parts = line.split()
                if len(parts) >= 2:
                    sums[parts[-1].lstrip("*")] = parts[0].lower()
        for f in sorted(d.iterdir()):
            if not f.is_file() or f.name == "SHA256SUMS":
                continue
            h = sums.get(f.name) or file_sha(f)
            if h == want:
                return f, None, "appimage" if f.name.endswith(".AppImage") else "exe"
            if re.search(r"\.tar(\.(xz|gz|bz2))?$", f.name):
                try:
                    with tarfile.open(f) as t:
                        for ti in t:
                            if ti.isfile() and PurePosixPath(ti.name).name == "openpete":
                                fo = t.extractfile(ti)
                                if hashlib.sha256(fo.read()).hexdigest() == want:
                                    return f, ti.name, "tarball"
                except (tarfile.TarError, OSError):
                    continue
            elif f.name.endswith(".zip"):
                try:
                    with zipfile.ZipFile(f) as z:
                        for zi in z.infolist():
                            if PurePosixPath(zi.filename).name == "openpete.exe":
                                if hashlib.sha256(z.read(zi)).hexdigest() == want:
                                    return f, zi.filename, "windows-zip"
                except (zipfile.BadZipFile, OSError):
                    continue
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
    plan = {"env": env, "disc": disc, "work": work, "archive": arch}
    if hit:
        art, member, kind = hit
        plan.update(route="primary", artifact=art, kind=kind)
        if kind == "tarball":
            plan["exe"] = work / "artifact" / member
        elif kind == "windows-zip":
            plan["exe"] = None
            plan["note"] = (f"Windows artifact {art.name} ({member}): run it on the Windows host "
                            f"with --bug-report-replay (LAW §V: the orchestrator drives it)")
        else:
            plan["exe"] = art
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
    pr, an = m.get("press") or {}, m.get("anchor")
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
        w(f"replay route: primary, archived artifact {plan['artifact']} ({plan['kind']})")
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
    if not plan.get("cmd"):
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
        if plan.get("kind") == "tarball":
            dest = work / "artifact"
            dest.mkdir(parents=True, exist_ok=True)
            with tarfile.open(plan["artifact"]) as t:
                for ti in t:
                    if member_name_ok(ti.name) or ti.issym() or ti.islnk() or ti.isdev():
                        continue
                    t.extract(ti, dest)
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
    p = sub.add_parser("diff-hashes", help="first divergent tick and region of two hash CSVs")
    p.add_argument("a")
    p.add_argument("b")
    args = ap.parse_args(argv)
    fn = {"info": cmd_info, "extract": cmd_extract, "replay": cmd_replay, "diff-hashes": cmd_diff}[args.cmd]
    try:
        return fn(args)
    except BundleError as e:
        print(f"refused: {e}", file=sys.stderr)
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main())
