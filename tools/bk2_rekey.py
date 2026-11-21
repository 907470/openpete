#!/usr/bin/env python3
"""Re-key bk2 movies onto the consume key (E, j).

A bk2 row was indexed by `openpete_frame` (the game fiber's yields; Spyro 1,
and Spyro 2 and 3 movies without a key line) or by `openpete_vbl` (Header.txt
`OpenpeteFeedKey vbl`; Spyro 2 and 3). The consume key groups rows by E,
`openpete_consume`, the entries of the game's pad-ring consumer (Spyro 1
GamestateUpdate 0x8003385C, Spyro 2 func_8001271C, Spyro 3 func_8003A584),
with one row per host pad write (one per yield of a VBL body) inside a
group, j being
`openpete_consume_write`. Before each yield the host stamps row j of group E,
holding the group's last row past its count. Header.txt carries
`OpenpeteFeedKey consume`; `Input Groups.txt` holds each group's row count,
one per line. The conversion of a movie is read off a run, never computed.

  map   Replays one source (a bk2 under its own key, or the plain boot) with
        OPENPETE_FEED_MAP and writes the run's map: a `W row e j` line per
        pad write of a yield, with the old-key row the host stamped before it
        (negative before the movie, the row count or more past its end), and
        a `P frame vbl e j` line per canonical present.
  bk2   Rewrites a frame- or vbl-keyed movie to the consume key: the write
        at (E, j) takes the old row stamped before it. Old rows no write took
        are not read by the old replay either and are dropped. Groups start
        at E 0 when the run's first write reads the movie; otherwise at the E
        of the first write inside it, which becomes the movie's offset.
        Reported per movie: lossy spans (writes inside the movie that read no
        old row: a repeat of an (E, j), a gap in j, a write before the movie
        inside its first group, or a write outside the movie between two in
        it; each makes the converted movie differ from the old replay), the
        old rows no write read, and the groups.
  pins  Prints, for each run number under the old key, the E of the
        present a --screenshot-on-frame pin at that number takes, and marks
        a pin whose present is not the first present of its E.

The binary replays an old-keyed movie under its own key (convert on load),
so `map` runs on the same binary that replays the converted movie, and
`--bk2-record` during such a replay writes the same rows `bk2` does.
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
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
from hostenv import sandbox  # noqa: E402
KEY_PREFIX = "OpenpeteFeedKey "
NEW_KEY = "consume"
COLS = {"frame": 0, "vbl": 1, "consume": 2}
GROUPS = "Input Groups.txt"


def die(msg):
    print(f"bk2_rekey: {msg}", file=sys.stderr)
    sys.exit(1)


def read_map(path):
    """Return the (writes, presents) of a map file: (row, e, j) and (frame, vbl, e, j) tuples."""
    writes, presents = [], []
    with open(path) as f:
        for line in f:
            c = line.split()
            if not c or c[0].startswith("#"):
                continue
            if c[0] == "W":
                writes.append((int(c[1]), int(c[2]), int(c[3])))
            elif c[0] == "P":
                presents.append((int(c[1]), int(c[2]), int(c[3]), int(c[4])))
    if not writes:
        die(f"{path}: no W lines")
    for a, b in zip(writes, writes[1:]):
        if b[1] < a[1]:
            die(f"{path}: E falls from {a[1]} to {b[1]}")
    return writes, presents


def default_bin():
    return os.path.join(ROOT, "build", "linux-release", "openpete")


def cmd_map(a):
    disc = a.disc
    if not disc:
        conf = os.path.join(ROOT, "games", a.game, "oracle.conf")
        disc = subprocess.run(["bash", "-c", f'source "{conf}" >/dev/null 2>&1; echo "$DISC"'],
                              capture_output=True, text=True).stdout.strip()
        disc = os.path.join(ROOT, disc)
    tmp = tempfile.mkdtemp(prefix="bk2_rekey_")
    try:
        mlog = os.path.join(tmp, "map.txt")
        cmd = [a.bin or default_bin(), "--game", a.game, "--headless", "--no-config", "--disc", disc,
               "--renderer", "vk", "--aspect", "4:3", "--lod", "original", "--mute", "1",
               "--frame-limit", str(a.frame_limit)]
        if a.bk2:
            cmd += ["--bk2", a.bk2, "--bk2-stop-on-end"]
        cmd += a.args
        sandbox()
        env = dict(os.environ, OPENPETE_FEED_MAP=mlog)
        r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not os.path.exists(mlog):
            die(f"no map from {' '.join(cmd)} (rc {r.returncode})")
        shutil.copyfile(mlog, a.out + ".tmp")
        os.replace(a.out + ".tmp", a.out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    read_map(a.out)


def old_key_of(hdr):
    for line in hdr.splitlines():
        if line.startswith(KEY_PREFIX):
            return line[len(KEY_PREFIX):].strip()
    return "frame"


def convert(old_rows, writes):
    """Return (rows, counts, e0, report) converting old_rows by the map's writes."""
    n = len(old_rows)
    i0 = next((i for i, p in enumerate(writes) if 0 <= p[0] < n), None)
    if i0 is None:
        die("no write of the map reads a row of the movie")
    i_end = next((i for i in range(i0, len(writes)) if writes[i][0] >= n), len(writes))
    e0, j0 = writes[i0][1], writes[i0][2]
    lossy = []
    if j0:
        lossy.append(("pre", e0, j0))
    rows = [old_rows[writes[i0][0]]] * j0
    counts = {e0: j0}
    seen = set()
    read = set()
    prev = None
    for i in range(i0, i_end):
        row, e, j = writes[i]
        if not 0 <= row < n:
            lossy.append(("outside", e, j))
            row = max(0, min(row, n - 1))
        if (e, j) in seen:
            lossy.append(("repeat", e, j))
            continue
        seen.add((e, j))
        want = prev[2] + 1 if prev and prev[1] == e else (j0 if e == e0 else 0)
        if j != want:
            lossy.append(("gap", e, j))
        read.add(row)
        rows.append(old_rows[row])
        counts[e] = counts.get(e, 0) + 1
        prev = (row, e, j)
    e_last = writes[i_end - 1][1]
    if i0 == 0:
        e0 = 0   # the movie opens at the run's first write: groups from E 0, offset 0
    cl = [counts.get(e, 0) for e in range(e0, e_last + 1)]
    rep = {"writes": i_end - i0, "lossy": lossy, "unread": n - len(read), "groups": len(cl),
           "empty": sum(1 for c in cl if c == 0)}
    return rows, cl, e0, rep


def cmd_bk2(a):
    writes, _ = read_map(a.map)
    with zipfile.ZipFile(a.input) as z:
        names = z.namelist()
        files = {n: z.read(n) for n in names}
    hdr = files.get("Header.txt", b"").decode("latin-1")
    key = old_key_of(hdr)
    if key not in ("frame", "vbl"):
        die(f"{a.input} is keyed on '{key}', not frame or vbl")
    log = files["Input Log.txt"].decode("latin-1").splitlines(keepends=True)
    rows = [l for l in log if l.startswith("|")]
    head = [l for l in log if not l.startswith("|") and not l.startswith("[/Input]")]
    tail = [l for l in log if l.startswith("[/Input]")]
    first = log.index(rows[0]) if rows else len(log)
    if any(not l.startswith("|") and not l.startswith("[/Input]") for l in log[first:]):
        die(f"{a.input}: non-row line inside the Input Log")
    new, counts, off_e, rep = convert(rows, writes)
    files["Input Log.txt"] = "".join(head + new + tail).encode("latin-1")
    files[GROUPS] = "".join(f"{c}\n" for c in counts).encode("latin-1")
    eol = "\r\n" if "\r\n" in hdr else "\n"
    kept = [l for l in hdr.splitlines() if l.strip() and not l.startswith(KEY_PREFIX)]
    files["Header.txt"] = (eol.join(kept + [KEY_PREFIX + NEW_KEY]) + eol).encode("latin-1")
    if "Header.txt" not in names:
        names.insert(0, "Header.txt")
    if GROUPS not in names:
        names.insert(names.index("Input Log.txt") + 1, GROUPS)
    tmp = a.output + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.writestr(n, files[n])
    os.replace(tmp, a.output)
    lossy = rep["lossy"]
    print(f"{os.path.basename(a.input)}: {len(rows)} {key} rows -> {len(new)} consume rows in "
          f"{rep['groups']} groups ({rep['empty']} empty), offset E {off_e}; lossy spans {len(lossy)}"
          f"{' (first ' + ','.join(f'{k}@{e}.{j}' for k, e, j in lossy[:5]) + ')' if lossy else ''}; "
          f"old rows unread {rep['unread']}")
    if off_e != 0:
        print(f"  note: offset {off_e} must be passed as --bk2-frame-offset", file=sys.stderr)


def cmd_pins(a):
    _, presents = read_map(a.map)
    col = COLS[a.key]
    out = []
    for p in a.numbers:
        idx = next((i for i, pr in enumerate(presents) if pr[col] >= p), None)
        if idx is None:
            die(f"{a.key} {p} past the map's last present")
        e = presents[idx][2]
        first = next(i for i, pr in enumerate(presents) if pr[2] >= e)
        out.append(f"{e}" if first == idx else f"{e}!shared")
    print(" ".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("map")
    m.add_argument("--game", required=True, choices=["spyro-1", "spyro-2", "spyro-3"])
    m.add_argument("--bin", default="")
    m.add_argument("--disc", default="")
    m.add_argument("--bk2", default="")
    m.add_argument("--frame-limit", type=int, required=True)
    m.add_argument("--out", required=True)
    m.add_argument("args", nargs="*", help="further launch arguments (after --)")
    b = sub.add_parser("bk2")
    b.add_argument("--map", required=True)
    b.add_argument("input")
    b.add_argument("output")
    p = sub.add_parser("pins")
    p.add_argument("--map", required=True)
    p.add_argument("--key", choices=["frame", "vbl"], required=True)
    p.add_argument("numbers", type=int, nargs="+")
    a = ap.parse_args()
    {"map": cmd_map, "bk2": cmd_bk2, "pins": cmd_pins}[a.cmd](a)


if __name__ == "__main__":
    main()
