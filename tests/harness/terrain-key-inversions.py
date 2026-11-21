#!/usr/bin/env python3
"""terrain-key-inversions.py - census of terrain key-contest inversions.

The terrain key contest (core/rhi/shaders/psx_key_contest.glsl op_key_loses)
discards an opaque terrain fragment F when the nearest terrain key K stored
at its pixel sorts nearer than F's own key: a smaller 128-world bucket on a
game whose scene shares one ordering table (Spyro 2, 3), a key smaller by
more than 64 world otherwise (Spyro 1). The key is the face's corner-mean
view-Z plus 128 x its authored slot offset (terrain.c tface_ot_slots), plus
the drain sub-cell term on a face the near drain inserts in sub-cells. The
override distance is unbounded: K's owner may lie any distance behind F.

This harness measures that distance over the presentation corpus. For every
freecam row (games/<g>/corpus/freecam-corpus.manifest) and every pinned VBL
of the bk2 corpus (games/<g>/corpus/presentation-corpus.manifest, 5th field)
it runs the game headless with OPENPETE_TERRAIN_TRIDUMP, which writes the
walk's terrain triangles (screen position, true view-Z, key inputs, face
identity) and the projection (H, OFX, OFY, the Hor+ screen-X range) at each
requested feed key. Each dump is rasterised on a PSX-pixel sample grid:
  - fragment depth is perspective-correct (1/z interpolated in screen
    space); the walk near-clips straddlers in camera space before it
    projects (terrain.c clip_push_tri), so every dumped vertex is a real
    camera-space point and its screen position its true projection;
  - the drain sub-cell coordinate interpolates perspective-correct, as the
    texture coordinate does, and selects the sub-cell's key exactly as
    op_drain_delta (margin 0 on the key side, 1e-4 on the fragment side);
    a skipped sub-cell draws nothing; a skipped sub-cell of a folded
    quadrant (op_drain_fold_delta, which needs the fold corners the dump
    does not carry) takes the face's least drawn entry and is counted;
  - emitters are the opaque (blend 0) non-fill faces with a positive key
    channel; with OPENPETE_TERRAIN_NEARBAND_CONTEST=0 near-band faces
    neither emit nor contest (terrain.c koff).
At each sample the depth winner F (nearest opaque non-fill fragment) is
tested against the stored key K (least emitted key, owner O). An inversion
is a sample where F loses to K and O is not F, so O lies behind F. Recorded
per inversion:
  perp     |z_O - z_F| * |n_F . (x/z, y/z, 1)|, the distance of O's point
           on the pixel's ray from F's plane, n_F F's unit normal in camera
           space (x = (sx - OFX) z / H, y = (sy - OFY) z / H);
  raysep   z_O - z_F along the view axis;
  tier     LP/LP, HP/HP or LP/HP of O and F, an HP stand-in carrying lpkey
           (terrain.standin_lp_key) classed LP;
  sector   same or cross;
  escape   for T in 32..1024 (ESC_T): the sample's verdict under a bounded rule
           (F yields to a key only when that key's owner lies within T of
           F's plane) differs between the single stored key and every key
           layer at the pixel: a layer within T keyed nearer than F exists
           but the stored key's owner is beyond T. One key per pixel cannot
           express that sample (the three-layer escape).

A key of another triangle of F's own face (a drain sub-cell keyed nearer
than F's) is the game's order within one face; it is counted as self-face,
not as an inversion. Every inversion's perp and raysep are kept per face
pair in rows.json, so a census at any bound reads off the raw lists.

Calibration (before any corpus row, unless --no-calibrate): the census
must reproduce the measured cases of the archway class: the S3 archway
(36/5 over 36/46, perp >= 900), Toasty w1a and w2b layering (median and
the largest pair < 64), Toasty w1a 42/20 over 42/1 (700-1250), the Toasty
lowered-camera poses (< 64), the S1 artisans gem close-up (0) and the S1
artisans LP/HP vista (155/56 over 155/6, 200-260). A failed case refuses
the run (calibration.txt names it).

Engagement, per row frame; a frame failing any test is VACUOUS, re-run
once, and never counted: the dump was written at a feed key in
frame-7..frame (the newest tick at or before the capture's key; its header
names it); a freecam row's log shows the pose engaged; the dump holds
terrain triangles and the grid samples > 0; a capture exists and, at an
aspect wider than 4:3, is the widened frame (>= 850 px wide).

Cache-forced legs (--cache [all|freecam]) run with
OPENPETE_TERRAIN_CACHE_FORCE=1 and OPENPETE_TERRAIN_GPU_CENSUS=1, and are
VACUOUS unless the census line at or before the frame shows retained
sectors. The dump describes only the walk: sectors the retained cache
owns skip the walk, so a cache leg's dump holds the walked residue and its
inversions are not measurable; the leg reports the retained sector count,
the residue's triangles against the walk leg's, and the pixel difference
of its capture against the walk leg's. The cache's contest (terrain.vert,
terrain_key_cache.frag, terrain_shade.glsl) mirrors the walk's keys, so
the walk leg's census stands for the cache where the two captures agree.
OPENPETE_TERRAIN_NEARBAND_CONTEST set to any value blocks the cache
(terrain.c tgpu_env_block), so a cache leg under it is the walk.

Not modelled: texture cutout (a transparent texel neither keys nor draws),
blended terrain (it contests but never emits; it is not a depth winner
here), fill twins (stencil-gated, never contest), moby keys.

Usage:
  tests/harness/terrain-key-inversions.py [--game G ...] [--out DIR]
      [--filter S] [--lods "original extended"] [--aspect 16:9]
      [--cache [all|freecam]] [--no-bk2] [--no-freecam] [--step N]
      [--extra-pose "<game>|<level>|<pose>"] [--calibrate-only]
      [--no-calibrate] [--build-dir DIR] [--env KEY=VALUE ...]
  --game        spyro-1, spyro-2, spyro-3 (repeatable; default all three)
  --out         output directory (default build/terrain-key-inversions):
                dumps/, logs/, shots/, calibration.{txt,json},
                report.txt, rows.json
  --filter      substring of the row key (freecam: <level>-<pose md5 6>,
                bk2: the row name)
  --step        sample spacing in PSX pixels (default 1)
  --extra-pose  a freecam row not in the manifest, captured as a row
  --env         an environment entry applied to every leg (repeatable)
Environment: CORPUS_JOBS (game processes at once, default 2),
  KEYINV_ANALYZE_JOBS (analysis processes, default 4),
  KEYINV_LEG_TIMEOUT (seconds per game process, default 900).

Verdict: the archway pose must be among the measured rows and show at
least one inversion with perp >= 256; every frame must be measured. The
census reports and carries no threshold verdict of its own. Outputs are
deleted before the run and asserted after.
Exit 0 PASS, 1 FAIL, VACUOUS or refused, 64 SKIP (no binary, disc, Vulkan
loader or numpy).
"""
import argparse
import collections
import hashlib
import json
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor

try:
    import numpy as np
except ImportError:
    print("[keyinv] SKIP: numpy missing")
    sys.exit(64)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
from hostenv import sandbox_run_root  # noqa: E402

GAMES = ["spyro-1", "spyro-2", "spyro-3"]
FREECAM_START = {"spyro-1": 1900, "spyro-2": 2473, "spyro-3": 2473}
SLOT_CONTEST = {"spyro-1": False, "spyro-2": True, "spyro-3": True}
ARCHWAY = ("spyro-3", "haunted_tomb", "pos=(93215, 91480, 22144) yaw=1493 pitch=49")
PERP_BINS = [32, 64, 128, 256, 512, 1024]
PERP_LABELS = ["<32", "<64", "<128", "<256", "<512", "<1024", ">=1024"]
TIERS = ["LP/LP", "HP/HP", "LP/HP"]
ESC_T = [32, 64, 96, 128, 192, 256, 384, 512, 768, 1024]
DRAIN_SKIP = 16383
DRAIN_MARGIN = 1e-4


def pose_slug(level, pose):
    """The freecam tile key of scripts/presentation-game.sh pg_pose_slug."""
    return level.replace(":", "-") + "-" + hashlib.md5(pose.encode()).hexdigest()[:6]


def game_conf(game):
    """DISC and RUN_ARGS of games/<game>/oracle.conf, sourced by bash."""
    conf = os.path.join(ROOT, "games", game, "oracle.conf")
    out = subprocess.run(["bash", "-c", 'source "$1" >/dev/null 2>&1; printf "%s\\n" "$DISC"; '
                          'printf "%s\\0" "${RUN_ARGS[@]}"', "_", conf],
                         capture_output=True, text=True, cwd=ROOT).stdout
    disc, _, rest = out.partition("\n")
    return disc, [a for a in rest.split("\0") if a]


def sw_env():
    """The software-Vulkan environment of scripts/lib/swrender.sh."""
    out = subprocess.run(["bash", "-c", "source scripts/lib/swrender.sh; sw_render_env; env -0"],
                         capture_output=True, text=True, cwd=ROOT).stdout
    env = {}
    for kv in out.split("\0"):
        k, _, v = kv.partition("=")
        if k:
            env[k] = v
    return env


def split_launch(launch):
    """Row launch tokens: KEY=VALUE (upper-case key) is env, the rest argv."""
    env, args = {}, []
    for tok in shlex.split(launch or ""):
        if tok.startswith("docs:"):
            continue
        if re.match(r"^[A-Z_][A-Z0-9_]*=", tok):
            k, _, v = tok.partition("=")
            env[k] = v
        else:
            args.append(tok)
    return env, args


def read_rows(game, args):
    """Freecam and pinned bk2 rows of one game as job descriptions."""
    rows = []
    cdir = os.path.join(ROOT, "games", game, "corpus")
    fman = os.path.join(cdir, "freecam-corpus.manifest")
    poses = []
    if not args.no_freecam:
        for line in open(fman):
            f = line.rstrip("\n").split("|")
            name = f[0].strip()
            if not name or name.startswith("#") or len(f) < 2 or not f[1].strip():
                continue
            poses.append((name, f[1].strip(), (f[2].strip() if len(f) > 2 else ""),
                          (f[3].strip() if len(f) > 3 else "")))
        for ep in args.extra_pose:
            g, level, pose = [s.strip() for s in ep.split("|", 2)]
            if g == game and not any(p[0] == level and p[1] == pose for p in poses):
                poses.append((level, pose, "", ""))
    for name, pose, start, launch in poses:
        level = name.split("@", 1)[0]
        key = pose_slug(name, pose)
        if args.filter and args.filter not in key and args.filter not in name:
            continue
        start = int(start) if start else FREECAM_START[game]
        m = re.match(r"^(.+):(\d+)$", level)
        skip = ["--skip-to-area", level] if m else ["--skip-to-level", level]
        env, extra = split_launch(launch)
        rows.append(dict(kind="freecam", key=key, name=name, frames=[start], env=env,
                         argv=skip + ["--freecam-pose", pose] + extra,
                         archway=(game, level, pose) == ARCHWAY))
    if not args.no_bk2:
        bman = os.path.join(cdir, "presentation-corpus.manifest")
        for line in open(bman):
            f = line.rstrip("\n").split("|")
            name = f[0].strip()
            if not name or name.startswith("#") or len(f) < 5 or not f[4].strip():
                continue
            if args.filter and args.filter not in name:
                continue
            pins = sorted({int(x) for x in f[4].split(",") if x.strip()})
            env, extra = split_launch(f[5] if len(f) > 5 else "")
            env.pop("PRESENTATION_LEG_TIMEOUT", None)
            bk2 = os.path.join(ROOT, "games", game, "bk2", name.split("@", 1)[0] + ".bk2")
            argv = (["--bk2", bk2] if os.path.isfile(bk2) else []) + extra
            rows.append(dict(kind="bk2", key=name, name=name, frames=pins, env=env, argv=argv,
                             archway=False))
    return rows


def leg_paths(out, game, row, lod, leg):
    base = f"{row['key']}__{lod}__{leg}"
    return (os.path.join(out, "dumps", game), base,
            os.path.join(out, "logs", game, base + ".log"),
            os.path.join(out, "shots", game))


def run_leg(job):
    """One game process: every frame of the row dumped and captured."""
    (game, row, lod, leg, args, binp, disc, runargs, swenv) = job
    ddir, base, log, sdir = leg_paths(args.out, game, row, lod, leg)
    for d in (ddir, os.path.dirname(log), sdir):
        os.makedirs(d, exist_ok=True)
    dumps = [os.path.join(ddir, f"{base}__{f}.txt") for f in row["frames"]]
    shots = [os.path.join(sdir, f"{base}__{f}-vk.png") for f in row["frames"]]
    for p in dumps + shots + [log]:
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ)
    env.update(swenv)
    env["OPENPETE_VIBRATION"] = "0"
    sbroot = sandbox_run_root()
    env["OPENPETE_SANDBOX"] = sbroot
    env["OPENPETE_CARD_DIR"] = os.path.join(sbroot, "cards")
    env["OPENPETE_TERRAIN_TRIDUMP"] = "+".join(str(f) for f in row["frames"]) + "," + \
        os.path.join(ddir, base + "__{n}.txt")
    if leg == "cache":
        env["OPENPETE_TERRAIN_CACHE_FORCE"] = "1"
        env["OPENPETE_TERRAIN_GPU_CENSUS"] = "1"
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    env.update(row["env"])
    with tempfile.TemporaryDirectory(prefix="keyinv-") as legdir:
        rargs = [a.replace("{legdir}", legdir) for a in row["argv"]]
        boot = list(runargs)
        if "--card1" in rargs:
            boot = [a for a in boot if a != "--no-card"]
        elif "--no-card" not in boot:
            boot.append("--no-card")
        tmo = int(os.environ.get("KEYINV_LEG_TIMEOUT", "900"))
        cmd = ["timeout", "-k", "10", str(tmo), binp, "--headless", "--no-config",
               "--disc", disc, "--disc-cache", "0", "--renderer", "vk", "--mute", "1",
               "--aspect", args.aspect, "--lod", lod] + boot + rargs + \
              ["--screenshot-native-only", "--screenshot-on-frame",
               ",".join(str(f) for f in row["frames"]),
               "--screenshot-output", os.path.join(sdir, base + "__{n}.png"),
               "--frame-limit", str(max(row["frames"]) + 4),
               "--max-runtime-seconds", str(tmo - 20)]
        with open(log, "w") as lf:
            lf.write("# " + " ".join(shlex.quote(c) for c in cmd) + "\n")
            lf.flush()
            rc = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env,
                                cwd=ROOT).returncode
    return rc


def dump_key(path):
    """The feed key a dump was written at (its first header line)."""
    try:
        with open(path) as fp:
            m = re.match(r"# frame=\d+ key=(\d+) n=(\d+)", fp.readline())
        return (int(m.group(1)), int(m.group(2))) if m else (None, None)
    except OSError:
        return (None, None)


def png_size(path):
    try:
        from PIL import Image
        with Image.open(path) as im:
            return im.size
    except Exception:
        return None


def leg_engagement(args, game, row, lod, leg, rc):
    """Per frame of a leg: (dump path, VACUOUS reasons, facts). An empty
    reason list is an engaged frame."""
    ddir, base, log, sdir = leg_paths(args.out, game, row, lod, leg)
    text = open(log, errors="replace").read() if os.path.isfile(log) else ""
    freecam = row["kind"] == "freecam" or "OPENPETE_FREECAM_POSE" in row["env"]
    fc_ok = "freecam: pose set to" in text
    cen = [(int(f), int(s), int(c)) for f, s, c in
           re.findall(r"\[tgpu-cen f=(\d+)\] sub=\d+ secs=(\d+) claims=(\d+)", text)]
    wide = args.aspect != "4:3"
    out = []
    for f in row["frames"]:
        p = os.path.join(ddir, f"{base}__{f}.txt")
        shot = os.path.join(sdir, f"{base}__{f}-vk.png")
        why, facts = [], dict(rc=rc)
        key, ntri = dump_key(p)
        facts.update(dump_key=key, dump_tris=ntri)
        if key is None and rc == 0 and "exit: reason=frame-limit" in text and os.path.isfile(shot):
            # The run reached the frame and captured it, and no terrain feed
            # ran in the window: the frame draws no terrain (the world draw
            # was not called). Nothing to measure; listed as skipped.
            facts["skipped"] = "no terrain feed in the frame's window (no terrain drawn)"
            out.append((p, [], facts))
            continue
        if key is not None and ntri == 0 and leg == "walk" and rc == 0 and f - 8 < key <= f:
            facts["skipped"] = "the frame's walk emitted no terrain triangle (no terrain drawn)"
            out.append((p, [], facts))
            continue
        if key is None:
            why.append("no dump")
        elif not (f - 8 < key <= f):
            why.append(f"dump at key {key}, not in {f - 7}..{f}")
        size = png_size(shot)
        facts["capture"] = size
        if size is None:
            why.append("no capture")
        elif wide and size[0] < 850:
            why.append(f"capture {size[0]}x{size[1]} is not the widened frame")
        if freecam and not fc_ok:
            why.append("freecam did not engage")
        if leg == "cache":
            # The retained sectors of the last census line at or before the
            # frame (the census labels its lines with the feed key).
            before = [(s, c) for cf, s, c in cen if cf <= f]
            secs, claims = before[-1] if before else (0, 0)
            facts["cache_secs"] = secs
            facts["cache_claims"] = claims
            if secs <= 0 and claims > 0 and not why:
                # The cache ran and claimed every sector in view back to the
                # walk (its per-sector fallback): the leg is the walk.
                facts["skipped"] = f"cache claimed all {claims} sector(s) back to the walk"
            elif secs <= 0:
                why.append("retained cache drew no sector")
        elif ntri is not None and ntri <= 0:
            why.append("dump holds no terrain triangle")
        out.append((p, why, facts))
    return out


# ---------------------------------------------------------------- analysis

def load_dump(path):
    hdr = {}
    rows = []
    with open(path) as fp:
        for line in fp:
            if line.startswith("# view"):
                for kv in line[6:].split():
                    k, _, v = kv.partition("=")
                    hdr[k] = float(v)
            elif line.startswith("#"):
                continue
            else:
                rows.append(line)
    if not rows:
        return hdr, None
    a = np.array([[float(v) for v in r.split(",")] for r in rows], dtype=np.float64)
    return hdr, a


def drain_slot(px, py, grid, q):
    """op_drain_slot over arrays (grid and q scalar per triangle)."""
    if grid == 1:
        r = (q >> 1) * 2 + (py > 0.0).astype(np.int64)
        c = (q & 1) * 2 + (px > 0.0).astype(np.int64)
        return r * 4 + c
    h = 1.0 if grid == 2 else 2.0
    n = 4 if grid == 2 else 2
    gx, gy = px / h, py / h
    i = np.clip(np.floor(gx).astype(np.int64), 0, n - 1)
    j = np.clip(np.floor(gy).astype(np.int64), 0, n - 1)
    up = (gx - i) + (gy - j) < 1.0
    over = i + j > n - 1
    ii = np.where(over & (i >= j), n - 1 - j, i)
    jj = np.where(over & (i < j), n - 1 - i, j)
    up = np.where(over, True, np.where(~up & (i + j > n - 2), True, up))
    return np.where(up, jj * 4 + ii, (3 - jj) * 4 + (3 - ii))


def drain_delta(px, py, ents, grid, q, margin):
    """op_drain_delta over arrays; returns (delta, skipped)."""
    e = ents[drain_slot(px, py, grid, q)]
    if margin > 0.0:
        d = np.full(px.shape, DRAIN_SKIP, dtype=np.int64)
        for k in range(4):
            ox = margin if (k & 1) else -margin
            oy = margin if k >= 2 else -margin
            n = ents[drain_slot(px + ox, py + oy, grid, q)]
            d = np.where(n < DRAIN_SKIP, np.minimum(d, n), d)
        e = np.where((e < DRAIN_SKIP) & (d < DRAIN_SKIP), d, e)
    return e


def analyze(path, game, nbc_off=False, step=1.0):
    """Census of one dump. Returns a dict of counts and pair lists."""
    hdr, a = load_dump(path)
    res = dict(path=path, tris=0, samples=0, covered=0, multi=0, contest=0, emit=0,
               inv=0, self_face=0, fold_approx=0, bins={t: [0] * len(PERP_LABELS) for t in TIERS},
               sector={"same": 0, "cross": 0}, esc={str(t): 0 for t in ESC_T},
               esc_base={str(t): 0 for t in ESC_T}, pairs=[], hdr=hdr)
    if a is None or "H" not in hdr:
        res["error"] = "empty dump or no view header"
        return res
    H, ofx, ofy = hdr["H"], hdr["ofx"], hdr["ofy"]
    xlo, xhi = hdr.get("xlo", 0.0), hdr.get("xhi", 2 * ofx)
    slot_contest = SLOT_CONTEST[game]
    sec, face, tier, fill, blend = (a[:, i].astype(np.int64) for i in range(5))
    slots = a[:, 6]
    X = a[:, [7, 10, 13]]
    Y = a[:, [8, 11, 14]]
    Z = a[:, [9, 12, 15]]
    otkey = a[:, 16]
    nearband = a[:, 17].astype(np.int64)
    FX = a[:, [19, 21, 23]]
    FY = a[:, [20, 22, 24]]
    KC = a[:, 25:33].astype(np.int64)
    lpkey = a[:, 33].astype(np.int64) if a.shape[1] > 33 else np.zeros(len(a), np.int64)
    n = len(a)
    res["tris"] = int(n)
    mean = np.where(otkey > 0.0, otkey, Z.sum(axis=1) / 3.0)
    key = mean + 128.0 * slots
    opaque = (blend == 0) & (fill == 0)
    keyed = opaque & ~((nearband == 1) & nbc_off)
    tclass = np.where((tier == 1) | (lpkey == 1), 0, 1)        # 0 LP, 1 HP
    # Camera-space corners and unit normals.
    CX = (X - ofx) * Z / H
    CY = (Y - ofy) * Z / H
    P0 = np.stack([CX[:, 0], CY[:, 0], Z[:, 0]], 1)
    P1 = np.stack([CX[:, 1], CY[:, 1], Z[:, 1]], 1)
    P2 = np.stack([CX[:, 2], CY[:, 2], Z[:, 2]], 1)
    N = np.cross(P1 - P0, P2 - P0)
    NL = np.linalg.norm(N, axis=1)
    N = N / np.where(NL > 0, NL, 1.0)[:, None]
    # Sample grid (pixel centres, PSX px).
    gx = np.arange(xlo + step * 0.5, xhi, step)
    gy = np.arange(step * 0.5, 2.0 * ofy, step)
    W_, H_ = len(gx), len(gy)
    res["samples"] = W_ * H_
    zmin = np.full((H_, W_), np.inf)
    zid = np.full((H_, W_), -1, np.int64)
    zkey = np.zeros((H_, W_))
    cnt = np.zeros((H_, W_), np.int32)
    kmin = np.full((H_, W_), np.inf)
    kid = np.full((H_, W_), -1, np.int64)
    kz = np.zeros((H_, W_))
    ents_all = []
    for t in range(n):
        ents_all.append(np.array([((KC[t, i >> 1] >> ((i & 1) * 16)) & 0x3FFF) for i in range(16)],
                                 np.int64))
    frag_cache = {}

    def raster(t):
        x, y, z = X[t], Y[t], Z[t]
        det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if abs(det) < 1e-9 or min(z) <= 0:
            return None
        i0 = max(int(math.floor((min(x) - xlo) / step - 0.5)), 0)
        i1 = min(int(math.ceil((max(x) - xlo) / step - 0.5)), W_ - 1)
        j0 = max(int(math.floor(min(y) / step - 0.5)), 0)
        j1 = min(int(math.ceil(max(y) / step - 0.5)), H_ - 1)
        if i0 > i1 or j0 > j1:
            return None
        sx = gx[i0:i1 + 1][None, :]
        sy = gy[j0:j1 + 1][:, None]
        b1 = ((sx - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (sy - y[0])) / det
        b2 = ((x[1] - x[0]) * (sy - y[0]) - (sx - x[0]) * (y[1] - y[0])) / det
        b0 = 1.0 - b1 - b2
        inside = (b0 >= -1e-6) & (b1 >= -1e-6) & (b2 >= -1e-6)
        if not inside.any():
            return None
        jj, ii = np.nonzero(inside)
        b0, b1, b2 = b0[inside], b1[inside], b2[inside]
        w0, w1, w2 = b0 / z[0], b1 / z[1], b2 / z[2]
        s = w0 + w1 + w2
        zz = 1.0 / s
        w0, w1, w2 = w0 * zz, w1 * zz, w2 * zz
        return jj + j0, ii + i0, zz, w0, w1, w2

    def frag_keys(t, w0, w1, w2, margin):
        grid = int((KC[t, 0] >> 14) & 3)
        base = np.full(w0.shape, key[t])
        if grid == 0:
            return base, np.zeros(w0.shape, bool), 0
        px = w0 * FX[t, 0] + w1 * FX[t, 1] + w2 * FX[t, 2]
        py = w0 * FY[t, 0] + w1 * FY[t, 1] + w2 * FY[t, 2]
        q = int((KC[t, 0] >> 30) & 3)
        ents = ents_all[t]
        d = drain_delta(px, py, ents, grid, q, margin)
        skip = d >= DRAIN_SKIP
        folds = 0
        if grid == 1 and skip.any():
            w = KC[t, 0] if q < 2 else KC[t, 1]
            if (w >> ((q & 1) * 16 + 14)) & 1:
                drawn = ents[ents < DRAIN_SKIP]
                if len(drawn):
                    folds = int(skip.sum())
                    d = np.where(skip, drawn.min(), d)
                    skip = np.zeros(skip.shape, bool)
        return base + d, skip, folds

    for t in range(n):
        if not opaque[t]:
            continue
        r = raster(t)
        if r is None:
            continue
        jj, ii, zz, w0, w1, w2 = r
        fk, fskip, fold = frag_keys(t, w0, w1, w2, DRAIN_MARGIN)
        ek, eskip, _ = frag_keys(t, w0, w1, w2, 0.0)
        res["fold_approx"] += fold
        keep = ~fskip
        frag_cache[t] = (jj[keep], ii[keep], zz[keep], fk[keep])
        jk, ik, zk, fkk = frag_cache[t]
        np.add.at(cnt, (jk, ik), 1)
        # depth winner (sequential per triangle: ties keep the earlier)
        cur = zmin[jk, ik]
        better = zk < cur
        # duplicate (j,i) within one triangle cannot occur (one sample, one bary)
        zmin[jk[better], ik[better]] = zk[better]
        zid[jk[better], ik[better]] = t
        zkey[jk[better], ik[better]] = fkk[better]
        if keyed[t]:
            res["emit"] += 1
            em = ~eskip
            je, ie, ze, ke = jj[em], ii[em], zz[em], ek[em]
            curk = kmin[je, ie]
            bk = ke < curk
            kmin[je[bk], ie[bk]] = ke[bk]
            kid[je[bk], ie[bk]] = t
            kz[je[bk], ie[bk]] = ze[bk]
    res["covered"] = int((cnt > 0).sum())
    res["multi"] = int((cnt > 1).sum())
    res["contest"] = int(keyed.sum())
    has = (zid >= 0) & (kid >= 0)
    F = np.where(has, zid, 0)
    contests = has & keyed[F]
    if slot_contest:
        loses = contests & (np.floor(kmin / 128.0) < np.floor(zkey / 128.0))
    else:
        loses = contests & (kmin < zkey - 64.0)
    # A key of another triangle of F's own face (a drain sub-cell keyed
    # nearer than F's) is the game's order within one face: counted apart.
    K_ = np.where(has, kid, 0)
    selfface = (sec[K_] == sec[F]) & (face[K_] == face[F]) & (tier[K_] == tier[F])
    res["self_face"] = int((loses & (kid != zid) & selfface).sum())
    inv = loses & (kid != zid) & ~selfface
    jv, iv = np.nonzero(inv)
    Fv = zid[jv, iv]
    Ov = kid[jv, iv]
    zF = zmin[jv, iv]
    zO = kz[jv, iv]
    dx = (gx[iv] - ofx) / H
    dy = (gy[jv] - ofy) / H
    dotn = np.abs(N[Fv, 0] * dx + N[Fv, 1] * dy + N[Fv, 2])
    perp = np.abs(zO - zF) * dotn
    ray = zO - zF
    res["inv"] = int(len(Fv))
    rel = np.where(tclass[Ov] == tclass[Fv], np.where(tclass[Fv] == 0, 0, 1), 2)
    pb = np.searchsorted(PERP_BINS, perp, side="right")
    for r_, b_ in zip(rel, pb):
        res["bins"][TIERS[r_]][b_] += 1
    same = sec[Ov] == sec[Fv]
    res["sector"]["same"] = int(same.sum())
    res["sector"]["cross"] = int((~same).sum())
    # Per face pair (a face is several triangles), every sample's perp and
    # raysep kept, so any bound is read off the raw lists later.
    merged = {}
    for o, f, p_, r_ in zip(Ov, Fv, perp, ray):
        k = (int(sec[o]), int(face[o]), int(tclass[o]), int(sec[f]), int(face[f]), int(tclass[f]))
        m = merged.get(k)
        if m is None:
            m = merged[k] = dict(
                K=dict(sec=k[0], face=k[1], tier="LP" if k[2] == 0 else "HP",
                       lpkey=int(lpkey[o]), slots=int(slots[o]), nb=int(nearband[o])),
                F=dict(sec=k[3], face=k[4], tier="LP" if k[5] == 0 else "HP",
                       lpkey=int(lpkey[f]), slots=int(slots[f]), nb=int(nearband[f])),
                rel=TIERS[0 if k[2] == k[5] == 0 else 1 if k[2] == k[5] else 2],
                same_sector=k[0] == k[3], perps=[], rays=[])
        m["perps"].append(round(float(p_), 1))
        m["rays"].append(round(float(r_), 1))
    out = []
    for m in merged.values():
        m["perps"].sort()
        m["rays"].sort()
        ps, rs = m["perps"], m["rays"]
        m.update(n=len(ps), perp_med=ps[len(ps) // 2], perp_max=ps[-1],
                 ray_med=rs[len(rs) // 2], ray_max=rs[-1])
        out.append(m)
    out.sort(key=lambda m: (-m["perp_max"], -m["n"]))
    res["pairs"] = out
    # Three-layer escape: samples whose stored key beats F and whose owner
    # lies beyond T, where another emitted layer within T beats F too.
    if len(Fv):
        perp_img = np.full((H_, W_), -1.0)
        perp_img[jv, iv] = perp
        cand = np.zeros((H_, W_), bool)
        cand[jv, iv] = perp >= min(ESC_T)
        for T in ESC_T:
            res["esc_base"][str(T)] = int((perp >= T).sum())
        if cand.any():
            ideal = {T: np.zeros((H_, W_), bool) for T in ESC_T}
            csum = np.pad(cand.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
            for t in range(n):
                if not keyed[t] or t not in frag_cache:
                    continue
                jj, ii, zz, _ = frag_cache[t]
                if not len(jj):
                    continue
                j0, j1, i0, i1 = jj.min(), jj.max() + 1, ii.min(), ii.max() + 1
                if csum[j1, i1] - csum[j0, i1] - csum[j1, i0] + csum[j0, i0] == 0:
                    continue
                r = raster(t)
                jr, ir, zr, w0, w1, w2 = r
                ek, eskip, _ = frag_keys(t, w0, w1, w2, 0.0)
                m = cand[jr, ir] & ~eskip & (zid[jr, ir] != t)
                if not m.any():
                    continue
                jr, ir, zr, ek = jr[m], ir[m], zr[m], ek[m]
                fk = zkey[jr, ir]
                beats = (np.floor(ek / 128.0) < np.floor(fk / 128.0)) if slot_contest else (ek < fk - 64.0)
                Fm = zid[jr, ir]
                dxx = (gx[ir] - ofx) / H
                dyy = (gy[jr] - ofy) / H
                pg = np.abs(zr - zmin[jr, ir]) * np.abs(N[Fm, 0] * dxx + N[Fm, 1] * dyy + N[Fm, 2])
                for T in ESC_T:
                    ok = beats & (pg < T)
                    ideal[T][jr[ok], ir[ok]] = True
            for T in ESC_T:
                res["esc"][str(T)] = int((ideal[T] & (perp_img >= T)).sum())
    return res


def analyze_job(j):
    path, game, nbc_off, step = j
    try:
        return analyze(path, game, nbc_off, step)
    except Exception as e:  # report, never hide
        return dict(path=path, error=repr(e))


def png_diff(a, b):
    """Pixels differing in any channel by more than 8, and the image size."""
    try:
        from PIL import Image
    except ImportError:
        return None
    if not (os.path.isfile(a) and os.path.isfile(b)):
        return None
    A = np.asarray(Image.open(a).convert("RGB"), dtype=np.int16)
    B = np.asarray(Image.open(b).convert("RGB"), dtype=np.int16)
    if A.shape != B.shape:
        return dict(size_a=list(A.shape[:2]), size_b=list(B.shape[:2]), diff=None)
    return dict(size=list(A.shape[:2]), diff=int((np.abs(A - B).max(axis=2) > 8).sum()))


def fmt_bins(b):
    return " ".join(f"{lab}:{v}" for lab, v in zip(PERP_LABELS, b))


def pair_line(p):
    K, F = p["K"], p["F"]
    return (f"K {K['sec']}/{K['face']} {K['tier']}{'*' if K['lpkey'] else ''} s{K['slots']}"
            f"{' nb' if K['nb'] else ''} over F {F['sec']}/{F['face']} {F['tier']}"
            f"{'*' if F['lpkey'] else ''} s{F['slots']}{' nb' if F['nb'] else ''}"
            f"  n={p['n']} perp med/max={p['perp_med']:.0f}/{p['perp_max']:.0f}"
            f" ray med/max={p['ray_med']:.0f}/{p['ray_max']:.0f}")


# ------------------------------------------------------------ calibration
#
# Known truth the census must reproduce before a corpus run counts (the
# measurements of the two reviews of the archway class). Each case names
# its rows and a check over the walk legs' analyses at the given LODs.

def _toasty(tag, pose):
    return dict(kind="bk2", key=f"cal-toasty-{tag}", name="toasty-defeat", frames=[3665],
                env={"OPENPETE_FREECAM_AT_VBL": "3650", "OPENPETE_FREECAM_POSE": pose},
                argv=["--bk2", os.path.join(ROOT, "games", "spyro-1", "bk2", "toasty-defeat.bk2")],
                archway=False)


def _fc(game, key, level, pose, archway=False):
    return dict(kind="freecam", key=key, name=level, frames=[FREECAM_START[game]], env={},
                argv=["--skip-to-level", level, "--freecam-pose", pose], archway=archway)


TOASTY_42 = [("p120", "111554,126370,21037,-924,120"), ("p250", "111554,126370,21037,-924,250"),
             ("p501", "111554,126370,21037,-924,501"), ("z20050", "111554,126370,20050,-924,25"),
             ("z20150", "111554,126370,20150,-924,25"), ("z20300", "111554,126370,20300,-924,40"),
             ("z20600", "111554,126370,20600,-924,40")]


def calibration_cases():
    arch = _fc("spyro-3", "cal-archway", "haunted_tomb", ARCHWAY[2], archway=True)

    def all_pairs(res):
        return [p for r in res for p in r.get("pairs", [])]

    def c_arch(res):
        ps = all_pairs(res)
        top = max((p["perp_max"] for p in ps), default=0)
        hit = [p for p in ps if (p["K"]["sec"], p["K"]["face"], p["F"]["sec"], p["F"]["face"]) ==
               (36, 5, 36, 46)]
        ok = top >= 256 and hit and hit[0]["perp_med"] >= 900
        return ok, f"max perp {top:.0f}; 36/5 over 36/46: " + (pair_line(hit[0]) if hit else "absent")

    def c_layer(res):
        ps = all_pairs(res)
        n = sum(p["n"] for p in ps)
        allp = sorted(x for p in ps for x in p["perps"])
        med = allp[len(allp) // 2] if allp else None
        big = max(ps, key=lambda p: p["n"]) if ps else None
        ok = n > 0 and med is not None and med < 64 and big["perp_max"] < 64
        return ok, (f"inversions {n}, median perp {med}, largest pair " + (pair_line(big) if big else "-"))

    def c_42(res):
        hit = [p for p in all_pairs(res)
               if {(p["K"]["sec"], p["K"]["face"]), (p["F"]["sec"], p["F"]["face"])} == {(42, 20), (42, 1)}]
        if not hit:
            return False, "pair 42/20 over 42/1 absent"
        top = max(p["perp_max"] for p in hit)
        return 700 <= top <= 1250, "; ".join(pair_line(p) for p in hit[:3])

    def c_zero(res):
        n = sum(p["n"] for p in all_pairs(res))
        return n == 0, f"inversions {n}"

    def c_twin(res):
        lh = [p for p in all_pairs(res) if p["rel"] == "LP/HP"]
        top = max((p["perp_max"] for p in lh), default=None)
        return (top is not None and 200 <= top <= 260), (f"LP/HP pairs {len(lh)}, max perp {top}; " +
                                                  "; ".join(pair_line(p) for p in lh[:2]))
    return [
        ("archway >= 1 inversion >= 256 (36/5 over 36/46 about 1000)", "spyro-3", [arch],
         ["extended", "original"], c_arch, "any"),
        ("Toasty w1a layering < 64", "spyro-1",
         [_toasty("w1a", "100084,130751,23620,8,758")], ["original", "extended"], c_layer, "any"),
        ("Toasty w2b layering < 64", "spyro-1",
         [_toasty("w2b", "111554,126370,21037,-924,501")], ["original", "extended"], c_layer, "any"),
        ("Toasty w1a 42/20 over 42/1 at 768-1121 (1188 on the review's own dump)", "spyro-1",
         [_toasty("w1a", "100084,130751,23620,8,758")], ["original", "extended"], c_42, "any"),
        ("Toasty lowered-camera poses layering < 64", "spyro-1",
         [_toasty(t, p) for t, p in TOASTY_42], ["original"], c_layer, "any"),
        ("artisans gem close-up: 0 inversions", "spyro-1",
         [_fc("spyro-1", "cal-artisans-gem", "artisans", "pos=(105243, 34497, 6393) yaw=3281 pitch=160")],
         ["original", "extended"], c_zero, "any"),
        ("artisans LP/HP twin vista: 155/56 over 155/6 at 228 (the review's own dump, K-owner metric)", "spyro-1",
         [_fc("spyro-1", "cal-artisans-twin", "artisans", "pos=(100000, 50000, 12000) yaw=512 pitch=-100")],
         ["original"], c_twin, "any"),
    ]


class Runner:
    """Runs legs (CORPUS_JOBS at once), checks engagement, re-runs a VACUOUS
    leg once, and analyses each engaged dump as its leg finishes."""

    def __init__(self, args, binp):
        self.args = args
        self.binp = binp
        self.swenv = sw_env()
        self.conf = {}
        self.apool = ProcessPoolExecutor(max_workers=max(1, int(os.environ.get("KEYINV_ANALYZE_JOBS", "4"))))
        self.nbc_off = any(kv == "OPENPETE_TERRAIN_NEARBAND_CONTEST=0" for kv in args.env)

    def game(self, g):
        if g not in self.conf:
            disc, runargs = game_conf(g)
            self.conf[g] = (os.path.join(ROOT, disc), runargs)
        return self.conf[g]

    def one(self, item):
        g, row, lod, leg = item
        disc, runargs = self.game(g)
        job = (g, row, lod, leg, self.args, self.binp, disc, runargs, self.swenv)
        eng = None
        if self.args.reuse:
            log = leg_paths(self.args.out, g, row, lod, leg)[2]
            m = re.findall(r"openpete-exit: .* code=(-?\d+)",
                           open(log, errors="replace").read()) if os.path.isfile(log) else []
            if m:
                eng = leg_engagement(self.args, g, row, lod, leg, int(m[-1]))
                if not any(w for _, w, _ in eng):
                    return eng
        for attempt in (1, 2):
            rc = run_leg(job)
            eng = leg_engagement(self.args, g, row, lod, leg, rc)
            bad = [w for _, w, _ in eng if w]
            tag = f"{g} {row['key']} {lod} {leg}"
            if not bad:
                print(f"[keyinv] engaged {tag} rc={rc}", flush=True)
                break
            print(f"[keyinv] VACUOUS {tag} rc={rc} attempt {attempt}: {bad[0]}", flush=True)
        return eng

    def run(self, items):
        """items: (game, row, lod, leg). Returns records, one per frame."""
        nj = max(1, int(os.environ.get("CORPUS_JOBS", "2")))
        recs, futs = [], []
        with ThreadPoolExecutor(max_workers=nj) as ex:
            for item, eng in zip(items, ex.map(self.one, items)):
                g, row, lod, leg = item
                for f, (p, why, facts) in zip(row["frames"], eng):
                    rec = dict(game=g, kind=row["kind"], key=row["key"], name=row["name"], lod=lod,
                               leg=leg, frame=f, archway=row["archway"], vacuous=why, **facts)
                    recs.append(rec)
                    # A cache leg's dump holds only the walked residue: its
                    # inversions are not the cache's and are not counted.
                    if not why and leg == "walk" and not facts.get("skipped"):
                        futs.append((rec, self.apool.submit(analyze_job, (p, g, self.nbc_off,
                                                                          self.args.step))))
        for rec, fu in futs:
            r = fu.result()
            if "error" in r:
                rec["vacuous"] = [f"analysis: {r['error']}"]
                continue
            if r["samples"] <= 0 or r["tris"] <= 0:
                rec["vacuous"] = ["no samples or no triangles examined"]
            rec.update({k: v for k, v in r.items() if k not in ("path", "hdr")})
        return recs


def calibrate(runner, cases, out):
    items, seen = [], set()
    for (_, g, rows, lods, _, _) in cases:
        for row in rows:
            for lod in lods:
                if (g, row["key"], lod) not in seen:     # a row two cases share runs once
                    seen.add((g, row["key"], lod))
                    items.append((g, row, lod, "walk"))
    recs = runner.run(items)
    by = collections.defaultdict(list)
    for ci, (_, g, rows, lods, _, _) in enumerate(cases):
        keys = {(g, row["key"], lod) for row in rows for lod in lods}
        by[ci] = [r for r in recs if (r["game"], r["key"], r["lod"]) in keys]
    ok_all, lines = True, ["== calibration (known truth) =="]
    for ci, (name, g, rows, lods, check, _) in enumerate(cases):
        rs = by[ci]
        vac = [f"{r['key']} {r['lod']}: {r['vacuous']}" for r in rs if r["vacuous"]]
        good = [r for r in rs if not r["vacuous"]]
        if vac or not good:
            ok_all = False
            lines.append(f"  FAIL {name}: VACUOUS {vac}")
            continue
        # The check holds on the LOD legs together (a case passes on any of
        # its LODs: a pair lives on the tier the LOD draws).
        per_lod = []
        passed = False
        for lod in lods:
            ok, msg = check([r for r in good if r["lod"] == lod])
            per_lod.append(f"{lod}: {'ok' if ok else 'no'} ({msg})")
            passed |= ok
        ok_all &= passed
        lines.append(f"  {'PASS' if passed else 'FAIL'} {name}")
        lines += ["      " + x for x in per_lod]
    with open(os.path.join(out, "calibration.txt"), "w") as fp:
        fp.write("\n".join(lines) + "\n")
    with open(os.path.join(out, "calibration.json"), "w") as fp:
        json.dump(recs, fp)
    print("\n".join(lines), flush=True)
    return ok_all


def report(args, recs, games, lods, legs):
    lines = []
    walk_tris = {(r["game"], r["key"], r["lod"], r["frame"]): r.get("tris")
                 for r in recs if r["leg"] == "walk"}
    for r in recs:
        hd = f"[{r['game']}] {r['kind']} {r['key']} f={r['frame']} lod={r['lod']} leg={r['leg']}"
        if r["vacuous"]:
            lines.append(f"{hd}\n  VACUOUS: {'; '.join(r['vacuous'])}")
            continue
        if r.get("skipped"):
            lines.append(f"{hd}\n  skipped: {r['skipped']}")
            continue
        if r["leg"] == "cache":
            shot = os.path.join(args.out, "shots", r["game"], f"{r['key']}__{r['lod']}__%s__{r['frame']}-vk.png")
            r["walk_vs_cache"] = png_diff(shot % "walk", shot % "cache")
            lines.append(f"{hd}\n  cache: retained sectors={r.get('cache_secs')} walked residue "
                         f"tris={r.get('dump_tris')} (walk leg {walk_tris.get((r['game'], r['key'], r['lod'], r['frame']))})"
                         f" walk-vs-cache px>8={r['walk_vs_cache']}; inversions: not measurable "
                         f"(the dump describes the walk only)")
            continue
        lines.append(hd)
        lines.append(f"  engagement: key={r['dump_key']} capture={r['capture']} tris={r['tris']} "
                     f"keyed={r['contest']} samples={r['samples']} covered={r['covered']} "
                     f"multi={r['multi']} inversions={r['inv']} same-sector={r['sector']['same']} "
                     f"cross-sector={r['sector']['cross']} self-face={r['self_face']} fold-approx={r['fold_approx']}")
        for t in TIERS:
            if sum(r["bins"][t]):
                lines.append(f"  perp {t:6s} {fmt_bins(r['bins'][t])}")
        if r["inv"]:
            lines.append("  escape (bounded verdict, single key != all layers) T:n/base " +
                         " ".join(f"{t}:{r['esc'][str(t)]}/{r['esc_base'][str(t)]}" for t in ESC_T))
        for p in r["pairs"][:5]:
            lines.append("  " + pair_line(p))
    # Per-game summary and population.
    lines.append("")
    lines.append("== per game ==")
    for g in games:
        rg = [r for r in recs if r["game"] == g]
        for leg in legs:
            rl = [r for r in rg if r["leg"] == leg]
            vac = [r for r in rl if r["vacuous"]]
            why = collections.Counter(w.split(":")[0] if "dump at key" not in w else "dump key"
                                      for r in vac for w in r["vacuous"])
            sk = collections.Counter(r["skipped"] for r in rl if r.get("skipped"))
            lines.append(f"  {g} {leg}: frames={len(rl)} measured={len(rl) - len(vac) - sum(sk.values())} "
                         f"VACUOUS={len(vac)} {dict(why) if why else ''} skipped={sum(sk.values())} "
                         f"{dict(sk) if sk else ''}")
    lines.append("")
    lines.append("== population, walk legs: inversion samples by perp bin ==")
    for g in games + ["all"]:
        for lod in lods:
            for t in TIERS:
                b = [0] * len(PERP_LABELS)
                for r in recs:
                    if (g == "all" or r["game"] == g) and r["lod"] == lod and r["leg"] == "walk" \
                            and not r["vacuous"] and not r.get("skipped"):
                        b = [x + y for x, y in zip(b, r["bins"][t])]
                lines.append(f"  {g:8s} {lod:9s} {t:6s} {fmt_bins(b)}")
    return lines


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--game", action="append", default=[])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "terrain-key-inversions"))
    ap.add_argument("--filter", default="")
    ap.add_argument("--lods", default="original extended")
    ap.add_argument("--aspect", default="16:9")
    ap.add_argument("--cache", nargs="?", const="all", default="", choices=["", "all", "freecam"])
    ap.add_argument("--no-bk2", action="store_true")
    ap.add_argument("--no-freecam", action="store_true")
    ap.add_argument("--step", type=float, default=1.0)
    ap.add_argument("--extra-pose", action="append", default=[])
    ap.add_argument("--calibrate-only", action="store_true")
    ap.add_argument("--no-calibrate", action="store_true")
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--env", action="append", default=[])
    args = ap.parse_args()
    args.out = os.path.abspath(args.out)
    games = args.game or GAMES
    lods = args.lods.split()
    binp = os.path.abspath(os.path.join(args.build_dir, "openpete"))
    outs = [os.path.join(args.out, f) for f in ("report.txt", "rows.json", "calibration.txt",
                                                  "calibration.json")]
    for p in outs:
        if os.path.exists(p):
            os.remove(p)
    os.makedirs(args.out, exist_ok=True)
    if not os.access(binp, os.X_OK):
        print(f"[keyinv] SKIP: no binary at {binp}")
        return 64
    if subprocess.run("ldconfig -p | grep -q 'libvulkan\\.so\\.1'", shell=True).returncode:
        print("[keyinv] SKIP: no Vulkan loader (libvulkan.so.1)")
        return 64
    for g in GAMES if not args.calibrate_only else ["spyro-1", "spyro-3"]:
        disc = os.path.join(ROOT, game_conf(g)[0])
        if (g in games or args.calibrate_only or not args.no_calibrate) and not os.path.isfile(disc):
            print(f"[keyinv] SKIP: no disc at {disc}")
            return 64
    runner = Runner(args, binp)
    if not args.no_calibrate:
        if not calibrate(runner, calibration_cases(), args.out):
            print(f"[keyinv] REFUSED: calibration failed; {os.path.join(args.out, 'calibration.txt')}")
            return 1
        if args.calibrate_only:
            print("[keyinv] PASS: calibration")
            return 0
    legs = ["walk"] + (["cache"] if args.cache else [])
    items = []
    for g in games:
        for row in read_rows(g, args):
            for lod in lods:
                for leg in legs:
                    if leg == "cache" and args.cache == "freecam" and row["kind"] != "freecam":
                        continue
                    items.append((g, row, lod, leg))
    print(f"[keyinv] games={games} legs={len(items)} lods={lods} cache={args.cache} "
          f"jobs={os.environ.get('CORPUS_JOBS', '2')} out={args.out}", flush=True)
    recs = runner.run(items)
    lines = report(args, recs, games, lods, legs)
    with open(outs[0], "w") as fp:
        fp.write("\n".join(lines) + "\n")
    with open(outs[1], "w") as fp:
        json.dump(recs, fp)
    tail = lines[lines.index("== per game =="):]
    print("\n".join(tail))
    for p in outs[:2]:
        if not os.path.isfile(p):
            print(f"[keyinv] FAIL: output {p} not written")
            return 1
    vac = [r for r in recs if r["vacuous"]]
    arch = [r for r in recs if r["archway"] and r["leg"] == "walk" and not r["vacuous"]]
    if not arch:
        print("[keyinv] REFUSED: the archway pose is not among the measured rows (filter, or pass "
              "--extra-pose 'spyro-3|haunted_tomb|pos=(93215, 91480, 22144) yaw=1493 pitch=49')")
        return 1
    if not any(sum(sum(r["bins"][t][4:]) for t in TIERS) >= 1 for r in arch):
        print("[keyinv] REFUSED: the archway pose shows no inversion with perp >= 256")
        return 1
    if vac:
        print(f"[keyinv] FAIL: {len(vac)} frame(s) VACUOUS after a re-run; see {outs[0]}")
        return 1
    print(f"[keyinv] PASS: {len(recs)} frame(s) measured; report {outs[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
