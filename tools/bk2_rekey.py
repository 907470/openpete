#!/usr/bin/env python3
"""Re-key Spyro 2 and 3 test data from the yield counter to the VBL counter.

A bk2 row, a --frame-limit, a --screenshot-on-frame pin and every other
frame number a Spyro 2 or 3 harness passes were indexed by `openpete_frame`,
the count of the game fiber's yields. The feed key of those games is now
`openpete_vbl`, the count of VBLs psyq_VSync has run. The two differ by the
fast-boot VBLs (which run without a yield) and by every VSync(n > 1) excess
(one yield for n VBLs), so the conversion of a number is read off a run,
never computed.

  map   Replays one source (a bk2, or the plain boot) on a binary whose bk2
        feed is keyed on `openpete_frame` and writes the (frame, vbl) pair of
        every host iteration that ran the game: the --gameplay-hash-log's
        first two columns, plus (0, 0), the iteration before the first
        yield. Refuses a log whose frames are not consecutive or whose vbl
        does not rise strictly with the frame: a VBL two yields share cannot
        name one of them.
  bk2   Rewrites a movie to one row per VBL. Row v holds the row the
        frame-keyed feed latched at the yield that reached the greatest
        reached VBL not above v; a VBL no yield reached holds the row before
        it. The movie's length becomes the VBL of the yield at which the
        frame-keyed feed ran past its end, so a replay leaves the movie at
        the same yield. Header.txt gains `OpenpeteFeedKey vbl`.
  pins  Prints the VBL of the yield each frame number names.

The binary's own convert-on-load (core/record/bk2_movie.c) applies the
same rule online to a frame-keyed movie it is handed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_LINE = "OpenpeteFeedKey vbl"


def die(msg):
    print(f"bk2_rekey: {msg}", file=sys.stderr)
    sys.exit(1)


def read_map(path):
    """Return the frame -> vbl list of a map file; index = frame."""
    vbls = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            fr, vb = (int(x) for x in line.split()[:2])
            if fr != len(vbls):
                die(f"{path}: frame {fr} out of sequence (expected {len(vbls)})")
            if vbls and vb <= vbls[-1]:
                die(f"{path}: vbl {vb} at frame {fr} does not rise past {vbls[-1]}")
            vbls.append(vb)
    if not vbls or vbls[0] != 0:
        die(f"{path}: the map must open with frame 0 at vbl 0")
    return vbls


def cmd_map(a):
    disc = a.disc
    if not disc:
        conf = os.path.join(ROOT, "games", a.game, "oracle.conf")
        disc = subprocess.run(["bash", "-c", f'source "{conf}" >/dev/null 2>&1; echo "$DISC"'],
                              capture_output=True, text=True).stdout.strip()
        disc = os.path.join(ROOT, disc)
    tmp = tempfile.mkdtemp(prefix="bk2_rekey_")
    try:
        hlog = os.path.join(tmp, "hash.csv")
        cmd = [a.bin, "--headless", "--no-config", "--disc", disc, "--renderer", "vk",
               "--aspect", "4:3", "--lod", "original", "--mute", "1",
               "--frame-limit", str(a.frame_limit), "--gameplay-hash-log", hlog]
        if a.bk2:
            cmd += ["--bk2", a.bk2]
        cmd += a.args
        r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not os.path.exists(hlog):
            die(f"no hash log from {' '.join(cmd)} (rc {r.returncode})")
        pairs = [(0, 0)]
        with open(hlog) as f:
            next(f, None)
            for line in f:
                c = line.split(",", 2)
                pairs.append((int(c[0]), int(c[1])))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    last = pairs[-1][0]
    if last < a.frame_limit:
        die(f"run stopped at frame {last}, short of --frame-limit {a.frame_limit}")
    with open(a.out, "w") as f:
        f.write(f"# frame vbl: {os.path.basename(a.bk2) if a.bk2 else 'plain boot'} {' '.join(a.args)}\n")
        for fr, vb in pairs:
            f.write(f"{fr}\t{vb}\n")
    read_map(a.out)


def held_rows(old_rows, vbls, offset_frames):
    """Return the VBL-keyed rows and the VBL offset for a frame-keyed movie."""
    n = len(old_rows)
    end = n + offset_frames
    if offset_frames < 0:
        die("a negative --bk2-frame-offset has no VBL of its own")
    if end >= len(vbls):
        die(f"map ends at frame {len(vbls) - 1}, short of the movie end {end}")
    off_v = vbls[offset_frames]
    new = []
    f = offset_frames
    for v in range(off_v, vbls[end]):
        while f + 1 <= end and vbls[f + 1] <= v:
            f += 1
        new.append(old_rows[f - offset_frames])
    return new, off_v


def cmd_bk2(a):
    vbls = read_map(a.map)
    with zipfile.ZipFile(a.input) as z:
        names = z.namelist()
        files = {n: z.read(n) for n in names}
    hdr = files.get("Header.txt", b"").decode("latin-1")
    if KEY_LINE in hdr:
        die(f"{a.input} is already keyed on the VBL counter")
    log = files["Input Log.txt"].decode("latin-1").splitlines(keepends=True)
    rows = [l for l in log if l.startswith("|")]
    head = [l for l in log if not l.startswith("|") and not l.startswith("[/Input]")]
    tail = [l for l in log if l.startswith("[/Input]")]
    first = log.index(rows[0]) if rows else len(log)
    if any(not l.startswith("|") and not l.startswith("[/Input]") for l in log[first:]):
        die(f"{a.input}: non-row line inside the Input Log")
    new, off_v = held_rows(rows, vbls, a.offset)
    files["Input Log.txt"] = "".join(head + new + tail).encode("latin-1")
    if hdr and not hdr.endswith("\n"):
        hdr += "\n"
    eol = "\r\n" if "\r\n" in hdr else "\n"
    files["Header.txt"] = (hdr + KEY_LINE + eol).encode("latin-1")
    if "Header.txt" not in names:
        names.insert(0, "Header.txt")
    tmp = a.output + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.writestr(n, files[n])
    os.replace(tmp, a.output)
    print(f"{os.path.basename(a.input)}: {len(rows)} frame rows -> {len(new)} VBL rows, offset {a.offset} -> {off_v}")


def cmd_pins(a):
    vbls = read_map(a.map)
    out = []
    for p in a.frames:
        if p < 0 or p >= len(vbls):
            die(f"frame {p} outside the map (0..{len(vbls) - 1})")
        out.append(str(vbls[p]))
    print(" ".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("map")
    m.add_argument("--game", required=True, choices=["spyro-2", "spyro-3"])
    m.add_argument("--bin", default=os.path.join(ROOT, "build", "linux-release", "openpete"))
    m.add_argument("--disc", default="")
    m.add_argument("--bk2", default="")
    m.add_argument("--frame-limit", type=int, required=True)
    m.add_argument("--out", required=True)
    m.add_argument("args", nargs="*", help="further launch arguments (after --)")
    b = sub.add_parser("bk2")
    b.add_argument("--map", required=True)
    b.add_argument("--offset", type=int, default=0, help="the movie's --bk2-frame-offset")
    b.add_argument("input")
    b.add_argument("output")
    p = sub.add_parser("pins")
    p.add_argument("--map", required=True)
    p.add_argument("frames", type=int, nargs="+")
    a = ap.parse_args()
    {"map": cmd_map, "bk2": cmd_bk2, "pins": cmd_pins}[a.cmd](a)


if __name__ == "__main__":
    main()
