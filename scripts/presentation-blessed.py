#!/usr/bin/env python3
"""presentation-blessed.py -- digest manifests and the reference store of the
presentation golden.

A golden tile's verdict is a digest of its decoded pixels, not the PNG file:
the tiles are written as RGBA while the comparison normalises to RGB, and a
file-byte digest would gate on the alpha channel and on the PNG encoder's
output. The verdict digest is SHA-256 over this preimage:

    b"RGB\\n" + b"<width>\\n" + b"<height>\\n" + <RGB bytes, scanline order>

taken after PIL's convert("RGB"), so two shapes cannot alias and a size change
reads as a size change.

Manifests (reference manifests): tests/golden/<game>/presentation/blessed.tsv
per game, the dragon-rescue tier's tests/golden/spyro-1/dragons/blessed.tsv,
and, where present, tests/golden/<game>/presentation/pre-migration.tsv and
games/spyro-1/native-tick-corpus.manifest. Each opens with `# key: value`
header lines (digest rule, mode, sizes, capture environment, disc hashes),
then the column line

    tile  digest  revision  recomp_input  generated_tree  origin

`revision` is the engine revision the tile was blessed from; `recomp_input`
digests the untracked build inputs (PSX.EXE, every overlay, psx.map,
manifest/globals.toml, manifest/symbols.toml); `generated_tree` digests the
recompiled source tree games/<game>/generated; `origin` is `mapped` (the
revision resolves in this repository) or `archive-only` (it resolves in the
archive alone).

Store: one file per verdict digest, <store>/<digest>.png, where <store> is
$OPENPETE_GOLDEN_REF_DIR or ${XDG_DATA_HOME:-~/.local/share}/openpete/golden-ref.
A blob is verified on read by decoding and re-digesting; a blob whose pixels
do not hash to its name is deleted. The state file golden-ref.state sits in
the parent directory of the resolved store (never inside it, since gc frees
every store entry no manifest names) and holds `key=value` lines:
`archive=<mirror clone path>` and
`decomp-reproducibility=pass|fail <details>`.

Subcommands:
  digest <png>...                 print each file's verdict digest
  read <game> [--tier T]          print a manifest's header and row count
  write <game> [--tier T]         digest the golden tiles, write blessed.tsv,
                                  file every tile into the store
  verify <game> [--tier T]        each row's tile and store blob reproduce its
                                  digest, and the tile set equals the row set
  store-file <png>...             file tiles into the store
  store-read <digest> [--out P]   read a blob, verified
  gc [--keep-days N] [--to-head] [--dry-run]
                                  free store blobs outside the keep-set; runs
                                  only when invoked and only after a passing
                                  decomp-reproducibility result
  golden-at <rev> [<game>] [<tile-filter>] [--out DIR] [--materialize]
                                  resolve a revision's rows: store, then the
                                  archive, then the materialize price
  capture-record <game> [--tier T] <candidate>
                                  write capture-record.txt into a candidate
                                  directory: the working copy's tree over the
                                  build's source roots and the recomp identity
  census <game> [--tier T] [--candidate DIR]
                                  print `rows=N matched=M`: manifest rows, and
                                  candidate tiles naming a row
  promote <game> [--tier T] <candidate> [--all | <tile-filter>...]
                                  rewrite the rows of changed candidate tiles
                                  and file them into the store; the blessed
                                  revision is HEAD when HEAD's tree equals the
                                  record's source tree, else a refusal naming
                                  the first differing path

Exit: 0 success; 1 mismatch, refusal or missing input; 2 usage.
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

try:
    from PIL import Image
except ModuleNotFoundError:
    print("[presentation-blessed] python3-PIL missing", file=sys.stderr)
    sys.exit(1)

GAMES = ("spyro-1", "spyro-2", "spyro-3")
TIERS = {"presentation": GAMES, "dragons": ("spyro-1",)}
COLUMNS = ("tile", "digest", "revision", "recomp_input", "generated_tree",
           "origin")
ORIGINS = ("mapped", "archive-only")
DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
BLOB_RE = re.compile(r"^([0-9a-f]{64})\.png$")
HEADER_TITLE = "presentation golden reference manifest"
DIGEST_RULE = ('sha256(b"RGB\\n" + width + b"\\n" + height + b"\\n" + '
               'RGB bytes in scanline order), PIL convert("RGB")')
# Environment fields of the capture surface; values not yet measurable in this
# environment are written `unrecorded` and set with --env key=value.
ENV_KEYS = ("recipe-hash", "mesa", "device", "compiler", "preset",
            "image-digest")
# Materialize price components (the materialize rebuilds the blessed revision
# and re-captures each corpus row a divergent tile belongs to).
PRICE_BUILD_MIN = 10
PRICE_ROW_S = 46


# ---- decode and digest ----------------------------------------------------

def digest_image(img):
    """Verdict digest of a decoded image (the preimage in the module
    docstring). Returns (hex digest, (width, height))."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    hsh = hashlib.sha256(b"RGB\n%d\n%d\n" % (w, h))
    hsh.update(rgb.tobytes())
    return hsh.hexdigest(), (w, h)


def digest_bytes(data):
    """Verdict digest of PNG bytes; None when they do not decode."""
    try:
        with Image.open(io.BytesIO(data)) as img:
            return digest_image(img)
    except Exception:                                   # noqa: BLE001
        return None


def digest_file(path):
    """Verdict digest of a PNG file; None when it does not decode."""
    try:
        with Image.open(path) as img:
            return digest_image(img)
    except Exception:                                   # noqa: BLE001
        return None


def digest_files(paths):
    """Digest many files in parallel. Returns {path: result|None}."""
    paths = list(paths)
    if len(paths) < 32:
        return {p: digest_file(p) for p in paths}
    with ProcessPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        return dict(zip(paths, ex.map(digest_file, paths, chunksize=16)))


def digest_blobs(blobs):
    """Digest many PNG byte strings in parallel, preserving order."""
    blobs = list(blobs)
    if len(blobs) < 32:
        return [digest_bytes(b) for b in blobs]
    with ProcessPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        return list(ex.map(digest_bytes, blobs, chunksize=16))


def sha256_file(path):
    """SHA-256 hex of a file's bytes."""
    hsh = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            hsh.update(chunk)
    return hsh.hexdigest()


# ---- repository layout ----------------------------------------------------

def repo_root():
    """Top level of the checkout the script runs in."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, ".."))


def git(*args, cwd=None, check=True, binary=False):
    """Run git in the checkout (or cwd); return stdout."""
    p = subprocess.run(["git", "-C", cwd or repo_root()] + list(args),
                       capture_output=True, text=not binary)
    if check and p.returncode != 0:
        err = p.stderr if not binary else p.stderr.decode(errors="replace")
        raise RuntimeError("git %s: %s" % (" ".join(args), err.strip()))
    return p.stdout if p.returncode == 0 else None


def tier_dir(game, tier):
    """Repo-relative golden directory of a game's tier."""
    return "tests/golden/%s/%s" % (game, tier)


def manifest_path(game, tier):
    """Repo-relative blessed.tsv of a game's tier."""
    return tier_dir(game, tier) + "/blessed.tsv"


def reference_manifests(game):
    """Every reference manifest path of a game, as (path, tier, tile
    directory). The tile directory is where the tiles were tracked before
    the manifests replaced them."""
    out = [(manifest_path(game, "presentation"), "presentation",
            tier_dir(game, "presentation")),
           (tier_dir(game, "presentation") + "/pre-migration.tsv",
            "presentation", tier_dir(game, "presentation"))]
    if game in TIERS["dragons"]:
        out.append((manifest_path(game, "dragons"), "dragons",
                    tier_dir(game, "dragons")))
    if game == "spyro-1":
        out.append(("games/spyro-1/native-tick-corpus.manifest", "native-vk",
                    tier_dir(game, "native-vk")))
    return out


def all_reference_manifest_paths():
    """Every reference manifest path across the games."""
    return [p for g in GAMES for p, _, _ in reference_manifests(g)]


def list_tiles(d):
    """Golden tiles of a directory: every `-vk.png` except the
    identity leg's `__ident__` tiles, which are never golden."""
    if not os.path.isdir(d):
        return []
    return sorted(f for f in os.listdir(d)
                  if f.endswith("-vk.png") and "__ident__" not in f)


# ---- manifest read/write ---------------------------------------------------

class Manifest:
    """A parsed reference manifest: header dict and rows."""

    def __init__(self, header=None, rows=None):
        self.header = header or {}
        self.rows = rows or []          # list of dicts keyed by COLUMNS


def parse_manifest(text, where="<manifest>"):
    """Parse manifest text. Raises ValueError naming the defect."""
    header, rows, cols = {}, [], None
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("#"):
            body = line[1:].strip()
            key, sep, val = body.partition(":")
            if sep and cols is None and re.match(r"^[a-z][a-z0-9-]*$", key):
                header[key] = val.strip()
            continue
        fields = line.split("\t")
        if cols is None:
            if tuple(fields) != COLUMNS:
                raise ValueError("%s:%d: column line is %r, expected %r"
                                 % (where, n, fields, "\t".join(COLUMNS)))
            cols = fields
            continue
        if len(fields) != len(COLUMNS):
            raise ValueError("%s:%d: %d fields, expected %d"
                             % (where, n, len(fields), len(COLUMNS)))
        row = dict(zip(COLUMNS, fields))
        if not DIGEST_RE.match(row["digest"]):
            raise ValueError("%s:%d: digest %r is not sha256 hex"
                             % (where, n, row["digest"]))
        if row["origin"] not in ORIGINS:
            raise ValueError("%s:%d: origin %r not in %s"
                             % (where, n, row["origin"], ORIGINS))
        rows.append(row)
    if cols is None:
        raise ValueError("%s: no column line" % where)
    return Manifest(header, rows)


def format_manifest(m):
    """Serialise a manifest: header lines, column line, rows sorted by
    tile."""
    out = ["# " + HEADER_TITLE]
    for k, v in m.header.items():
        out.append("# %s: %s" % (k, v))
    out.append("\t".join(COLUMNS))
    for r in sorted(m.rows, key=lambda r: r["tile"]):
        out.append("\t".join(r[c] for c in COLUMNS))
    return "\n".join(out) + "\n"


def load_manifest(game, tier):
    """Read the working tree's manifest of a game's tier."""
    path = os.path.join(repo_root(), manifest_path(game, tier))
    with open(path) as fh:
        return parse_manifest(fh.read(), manifest_path(game, tier))


# ---- identity columns and header ------------------------------------------

def source_conf(conf, names):
    """Values of shell variables from a sourced config file."""
    script = 'source "$1" >/dev/null 2>&1; shift; for v in "$@"; do ' \
             'printf "%s\\n" "${!v}"; done'
    p = subprocess.run(["bash", "-c", script, "_", conf] + list(names),
                       cwd=repo_root(), capture_output=True, text=True)
    vals = p.stdout.split("\n")
    return {n: (vals[i] if i < len(vals) else "") for i, n in enumerate(names)}


def recomp_input_digest(game):
    """Digest of the untracked build inputs of a game: PSX.EXE, every
    overlay, psx.map and the psx.map-derived globals.toml and symbols.toml.
    An absent input enters the preimage as `absent`."""
    root = repo_root()
    conf = source_conf("games/%s/recomp.conf" % game,
                       ("PSX_EXE", "WAD_DIR", "DECOMP_DIR"))
    paths = [conf["PSX_EXE"], conf["DECOMP_DIR"] + "/build/psx.map",
             "games/%s/manifest/globals.toml" % game,
             "games/%s/manifest/symbols.toml" % game]
    wad = os.path.join(root, conf["WAD_DIR"])
    if os.path.isdir(wad):
        paths += [conf["WAD_DIR"] + "/" + f for f in sorted(os.listdir(wad))
                  if f.endswith(".ovl")]
    hsh = hashlib.sha256()
    for rel in sorted(paths):
        ap = os.path.join(root, rel)
        val = sha256_file(ap) if os.path.isfile(ap) else "absent"
        hsh.update(("%s\t%s\n" % (rel, val)).encode())
    return hsh.hexdigest()


def generated_tree_digest(game):
    """Digest of games/<game>/generated: every file's relative path and
    content hash, sorted. `absent` when the tree does not exist."""
    top = os.path.join(repo_root(), "games", game, "generated")
    if not os.path.isdir(top):
        return "absent"
    entries = []
    for dp, dns, fns in os.walk(top, followlinks=True):
        dns.sort()
        for f in fns:
            ap = os.path.join(dp, f)
            entries.append((os.path.relpath(ap, top), ap))
    hsh = hashlib.sha256()
    for rel, ap in sorted(entries):
        hsh.update(("%s\t%s\n" % (rel, sha256_file(ap))).encode())
    return hsh.hexdigest()


def disc_hashes(game, preset):
    """Content hashes of the disc .cue and the .bin it names, memoized
    in build/<preset>/golden-disc-hash.tsv by path, size and mtime (the
    memo decides only whether to re-hash). Returns {"disc-bin": ...,
    "disc-cue": ...}, each `<sha256> <basename>` or `absent <path>`."""
    root = repo_root()
    cue = source_conf("games/%s/oracle.conf" % game, ("DISC",))["DISC"]
    out = {"disc-bin": "absent", "disc-cue": "absent"}
    if not cue:
        return out
    cue_ap = os.path.join(root, cue)
    if not os.path.isfile(cue_ap):
        out["disc-cue"] = "absent " + cue
        return out
    bin_ap = None
    with open(cue_ap, errors="replace") as fh:
        m = re.search(r'FILE\s+"([^"]+)"', fh.read())
    if m:
        bin_ap = os.path.join(os.path.dirname(cue_ap), m.group(1))
    memo_path = os.path.join(root, "build", preset, "golden-disc-hash.tsv")
    memo = {}
    if os.path.isfile(memo_path):
        with open(memo_path) as fh:
            for line in fh:
                f = line.rstrip("\n").split("\t")
                if len(f) == 4:
                    memo[(f[0], f[1], f[2])] = f[3]
    dirty = False

    def hashed(ap):
        nonlocal dirty
        st = os.stat(ap)
        key = (os.path.realpath(ap), str(st.st_size), str(st.st_mtime_ns))
        if key not in memo:
            memo[key] = sha256_file(ap)
            dirty = True
        return memo[key]

    out["disc-cue"] = "%s %s" % (hashed(cue_ap), os.path.basename(cue_ap))
    if bin_ap and os.path.isfile(bin_ap):
        out["disc-bin"] = "%s %s" % (hashed(bin_ap), os.path.basename(bin_ap))
    elif bin_ap:
        out["disc-bin"] = "absent " + os.path.basename(bin_ap)
    if dirty and os.path.isdir(os.path.dirname(memo_path)):
        with open(memo_path, "w") as fh:
            for k, v in sorted(memo.items()):
                fh.write("\t".join(k + (v,)) + "\n")
    return out


def compiler_version(preset):
    """First line of the version string of the C compiler the preset's
    build directory is configured with; `unrecorded` when unknown."""
    cache = os.path.join(repo_root(), "build", preset, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return "unrecorded"
    with open(cache) as fh:
        m = re.search(r"^CMAKE_C_COMPILER:\w+=(.+)$", fh.read(), re.M)
    if not m:
        return "unrecorded"
    try:
        p = subprocess.run([m.group(1), "--version"], capture_output=True,
                           text=True, timeout=30)
        return p.stdout.splitlines()[0].strip() or "unrecorded"
    except (OSError, IndexError, subprocess.SubprocessError):
        return "unrecorded"


def build_header(game, tier, sizes, preset, env_over, accept):
    """The manifest header: digest rule, mode, channel order, tile
    sizes, the capture environment and the disc hashes."""
    env = {k: "unrecorded" for k in ENV_KEYS}
    env["preset"] = preset
    env["compiler"] = compiler_version(preset)
    env.update(env_over)
    hdr = {"game": game, "tier": tier, "digest": DIGEST_RULE, "mode": "RGB",
           "channel-order": "R,G,B",
           "size": ",".join("%dx%d" % s for s in sorted(sizes)) or "none"}
    hdr.update(env)
    hdr.update(disc_hashes(game, preset))
    hdr["environment-accept"] = accept or "none"
    return hdr


# ---- store -----------------------------------------------------------------

def store_dir():
    """The resolved store directory."""
    d = os.environ.get("OPENPETE_GOLDEN_REF_DIR")
    if not d:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser(
            "~/.local/share")
        d = os.path.join(base, "openpete", "golden-ref")
    return os.path.realpath(d)


def state_path():
    """golden-ref.state in the parent directory of the resolved store."""
    return os.path.join(os.path.dirname(store_dir()), "golden-ref.state")


def read_state():
    """The state file's key=value pairs; {} when absent."""
    out = {}
    try:
        with open(state_path()) as fh:
            for line in fh:
                k, sep, v = line.strip().partition("=")
                if sep and not k.startswith("#"):
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def blob_path(digest):
    """Store path of a verdict digest."""
    return os.path.join(store_dir(), digest + ".png")


def store_put(src, digest):
    """File a tile whose verdict digest is already known into the
    store. A present blob that verifies is kept; one that does not is
    replaced. Returns True when a file was written."""
    dst = blob_path(digest)
    if os.path.isfile(dst):
        got = digest_file(dst)
        if got and got[0] == digest:
            return False
    os.makedirs(store_dir(), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".incoming-", dir=store_dir())
    os.close(fd)
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)
    return True


def store_get(digest):
    """Path of a verified store blob, or None. A blob whose decoded
    pixels do not hash to its name is deleted."""
    p = blob_path(digest)
    if not os.path.isfile(p):
        return None
    got = digest_file(p)
    if got and got[0] == digest:
        return p
    try:
        os.unlink(p)
    except OSError:
        pass
    return None


def store_verified(digests):
    """Subset of digests whose store blob verifies, decoded in parallel. A
    blob whose decoded pixels do not hash to its name is deleted."""
    want = {d for d in digests if os.path.isfile(blob_path(d))}
    got = digest_files(blob_path(d) for d in sorted(want))
    ok = set()
    for d in want:
        r = got[blob_path(d)]
        if r and r[0] == d:
            ok.add(d)
        else:
            try:
                os.unlink(blob_path(d))
            except OSError:
                pass
    return ok


# ---- subcommands: digest, read, write, verify, store ------------------------

def cmd_digest(a):
    """Print each file's verdict digest and size."""
    rc = 0
    for p, got in digest_files(a.png).items():
        if got is None:
            print("UNDECODABLE\t%s" % p)
            rc = 1
        else:
            print("%s\t%dx%d\t%s" % (got[0], got[1][0], got[1][1], p))
    return rc


def cmd_read(a):
    """Print a manifest's header and its row count."""
    m = load_manifest(a.game, a.tier)
    for k, v in m.header.items():
        print("%s: %s" % (k, v))
    print("rows: %d" % len(m.rows))
    print("distinct digests: %d" % len({r["digest"] for r in m.rows}))
    return 0


def cmd_write(a):
    """Digest a game tier's golden tiles, write blessed.tsv and file
    every tile into the store."""
    root = repo_root()
    src = a.src or os.path.join(root, tier_dir(a.game, a.tier))
    tiles = list_tiles(src)
    if not tiles:
        print("[presentation-blessed] write: no tiles in %s" % src,
              file=sys.stderr)
        return 1
    env_over = {}
    for kv in a.env or []:
        k, sep, v = kv.partition("=")
        if not sep or k not in ENV_KEYS:
            print("[presentation-blessed] --env %r: key must be one of %s"
                  % (kv, ", ".join(ENV_KEYS)), file=sys.stderr)
            return 2
        env_over[k] = v
    rev = git("rev-parse", "--verify", (a.revision or "HEAD") + "^{commit}"
              ).strip()
    rin = recomp_input_digest(a.game)
    gen = generated_tree_digest(a.game)
    paths = [os.path.join(src, t) for t in tiles]
    digs = digest_files(paths)
    bad = [p for p in paths if digs[p] is None]
    if bad:
        print("[presentation-blessed] write: undecodable: %s" % bad[0],
              file=sys.stderr)
        return 1
    rows = [dict(tile=t, digest=digs[p][0], revision=rev, recomp_input=rin,
                 generated_tree=gen, origin="mapped")
            for t, p in zip(tiles, paths)]
    sizes = {digs[p][1] for p in paths}
    m = Manifest(build_header(a.game, a.tier, sizes, a.preset, env_over,
                              a.environment_accept), rows)
    out = os.path.join(root, manifest_path(a.game, a.tier))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out + ".tmp", "w") as fh:
        fh.write(format_manifest(m))
    os.replace(out + ".tmp", out)
    filed = 0
    if not a.no_store:
        for r, p in zip(rows, paths):
            filed += store_put(p, r["digest"])
    print("[presentation-blessed] wrote %s: %d rows, %d distinct digests; "
          "store %s: %d filed" % (manifest_path(a.game, a.tier), len(rows),
                                  len({r["digest"] for r in rows}),
                                  store_dir(), filed))
    return 0


def cmd_verify(a):
    """Check that every row's tile and store blob reproduce its digest
    and that the tile set equals the row set."""
    root = repo_root()
    src = a.src or os.path.join(root, tier_dir(a.game, a.tier))
    m = load_manifest(a.game, a.tier)
    rows = {r["tile"]: r for r in m.rows}
    tiles = set(list_tiles(src)) if not a.store_only else set()
    missing = sorted(set(rows) - tiles) if not a.store_only else []
    extra = sorted(tiles - set(rows))
    digs = digest_files(os.path.join(src, t) for t in sorted(tiles & set(rows)))
    tile_mis = []
    for t in sorted(tiles & set(rows)):
        got = digs[os.path.join(src, t)]
        if got is None or got[0] != rows[t]["digest"]:
            tile_mis.append(t)
    in_store = store_verified({r["digest"] for r in rows.values()})
    store_miss = [t for t, r in sorted(rows.items())
                  if r["digest"] not in in_store]
    for t in missing:
        print("MISSING-TILE\t%s" % t)
    for t in extra:
        print("EXTRA-TILE\t%s" % t)
    for t in tile_mis:
        print("TILE-MISMATCH\t%s" % t)
    for t in store_miss:
        print("STORE-MISS\t%s" % t)
    bad = len(missing) + len(extra) + len(tile_mis) + len(store_miss)
    print("[presentation-blessed] verify %s: rows=%d tiles=%d compared=%d "
          "mismatches=%d missing=%d extra=%d store-miss=%d"
          % (manifest_path(a.game, a.tier), len(rows), len(tiles),
             len(tiles & set(rows)), len(tile_mis), len(missing), len(extra),
             len(store_miss)))
    return 1 if bad else 0


def cmd_store_file(a):
    """File tiles into the store under their verdict digests."""
    rc = 0
    for p, got in digest_files(a.png).items():
        if got is None:
            print("UNDECODABLE\t%s" % p)
            rc = 1
            continue
        wrote = store_put(p, got[0])
        print("%s\t%s\t%s" % (got[0], "filed" if wrote else "present", p))
    return rc


def cmd_store_read(a):
    """Read one verified blob; copy it to --out when given."""
    p = store_get(a.digest)
    if p is None:
        print("[presentation-blessed] %s: not in the store" % a.digest,
              file=sys.stderr)
        return 1
    if a.out:
        shutil.copyfile(p, a.out)
    print(p)
    return 0


# ---- git object reads -------------------------------------------------------

class CatFile:
    """A persistent `git cat-file --batch` reader over one repository."""

    def __init__(self, repo):
        self.proc = subprocess.Popen(
            ["git", "-C", repo, "cat-file", "--batch"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, spec):
        """Object bytes for a spec (`<rev>:<path>`, a sha), or None."""
        self.proc.stdin.write(spec.encode() + b"\n")
        self.proc.stdin.flush()
        head = self.proc.stdout.readline().decode().split()
        if len(head) < 3 or head[-1] == "missing" or head[1] == "missing":
            return None
        size = int(head[2])
        data = self.proc.stdout.read(size)
        self.proc.stdout.read(1)
        return data

    def close(self):
        """End the reader process."""
        self.proc.stdin.close()
        self.proc.wait()


# ---- gc ---------------------------------------------------------------------

def manifest_digests(text, where):
    """Digest set of a manifest text; empty when it does not parse."""
    try:
        return {r["digest"] for r in parse_manifest(text, where).rows}
    except ValueError:
        return set()


def keep_commits(keep_days, to_head, now):
    """The commits whose reference manifests the keep-set reads."""
    root = repo_root()
    if to_head:
        return [git("rev-parse", "HEAD").strip()]
    paths = all_reference_manifest_paths()
    ref = "main" if git("rev-parse", "--verify", "-q", "main",
                        check=False) else "HEAD"
    log = git("log", ref, "--format=%H %ct", "--", *paths, cwd=root) or ""
    boundary = now - keep_days * 86400
    inside, before = [], None
    for line in log.splitlines():
        sha, ct = line.split()
        if int(ct) >= boundary:
            inside.append(sha)
        elif before is None:
            before = sha
    commits = inside + ([before] if before else [])
    tags = git("for-each-ref", "--format=%(objectname)", "refs/tags") or ""
    for t in tags.split():
        c = git("rev-parse", "-q", "--verify", t + "^{commit}", check=False)
        if c:
            commits.append(c.strip())
    return commits


def tracked_png_digests():
    """Verdict digests of every PNG tracked under tests/golden, read
    from the index."""
    out = git("ls-files", "-s", "--", "tests/golden") or ""
    shas = [ln.split()[1] for ln in out.splitlines()
            if ln.split("\t")[-1].endswith(".png")]
    cf = CatFile(repo_root())
    blobs = [cf.read(s) for s in shas]
    cf.close()
    return {r[0] for r in digest_blobs(b for b in blobs if b) if r}


def compute_keep_set(keep_days, to_head, now):
    """The digest keep-set: every digest a reference manifest names at
    the kept commits and in the working tree, plus every tracked PNG's."""
    root = repo_root()
    keep = set()
    commits = keep_commits(keep_days, to_head, now)
    cf = CatFile(root)
    for c in dict.fromkeys(commits):
        for p in all_reference_manifest_paths():
            data = cf.read("%s:%s" % (c, p))
            if data:
                keep |= manifest_digests(data.decode(errors="replace"), p)
    cf.close()
    for p in all_reference_manifest_paths():
        ap = os.path.join(root, p)
        if os.path.isfile(ap):
            with open(ap) as fh:
                keep |= manifest_digests(fh.read(), p)
    tracked = tracked_png_digests()
    return keep | tracked, len(set(commits)), len(tracked)


def cmd_gc(a):
    """Free store blobs outside the keep-set, after a passing
    decomp-reproducibility result in the state file."""
    st = read_state()
    result = st.get("decomp-reproducibility", "")
    if result.split()[:1] != ["pass"]:
        print("[presentation-blessed] gc: no passing decomp-reproducibility "
              "result in %s (read %r); frees nothing"
              % (state_path(), result or "absent"), file=sys.stderr)
        return 1
    keep, ncommits, ntracked = compute_keep_set(a.keep_days, a.to_head,
                                                time.time())
    sd = store_dir()
    names = sorted(os.listdir(sd)) if os.path.isdir(sd) else []
    free, kept, foreign = [], [], []
    for n in names:
        m = BLOB_RE.match(n)
        if not m:
            foreign.append(n)
        elif m.group(1) in keep:
            kept.append(n)
        else:
            free.append(n)

    def size(ns):
        return sum(os.path.getsize(os.path.join(sd, n)) for n in ns)

    print("[presentation-blessed] gc %s: keep-set %d digests (%d commits, "
          "%d tracked PNG digests); store %d blobs: keep %d (%d bytes), "
          "free %d (%d bytes); %d non-blob entries untouched%s"
          % ("--to-head" if a.to_head else "--keep-days %d" % a.keep_days,
             len(keep), ncommits, ntracked, len(kept) + len(free), len(kept),
             size(kept), len(free), size(free), len(foreign),
             " (dry run)" if a.dry_run else ""))
    if not a.dry_run:
        for n in free:
            os.unlink(os.path.join(sd, n))
    return 0


# ---- golden-at ----------------------------------------------------------------

def load_commit_map(archive):
    """{rewritten sha: pre-rewrite sha} from <archive>/commit-map.txt."""
    out = {}
    try:
        with open(os.path.join(archive, "commit-map.txt")) as fh:
            for line in fh:
                f = line.split()
                if len(f) == 2 and all(re.match(r"^[0-9a-f]{40,64}$", x)
                                       for x in f):
                    out[f[1]] = f[0]
    except OSError:
        pass
    return out


def resolve(repo, rev):
    """Full commit sha of rev in repo, or None."""
    if not repo or not os.path.isdir(repo):
        return None
    out = git("rev-parse", "-q", "--verify", rev + "^{commit}", cwd=repo,
              check=False)
    return out.strip() if out else None


def corpus_row(tile):
    """The corpus row a tile belongs to: its basename before `__`."""
    return tile.split("__", 1)[0]


def price(nrows):
    """Materialize price for re-capturing nrows corpus rows."""
    return ("build~%dmin+capture~%ds*%d corpus row(s)+unmeasured "
            "(decomp re-make, decomp image, PSXRecomp rebuild, regens)"
            % (PRICE_BUILD_MIN, PRICE_ROW_S, nrows))


class Archive:
    """The archive named by golden-ref.state and its commit map."""

    def __init__(self):
        self.path = read_state().get("archive") or None
        if self.path and not os.path.isdir(self.path):
            self.path = None
        self.map = load_commit_map(self.path) if self.path else {}
        self.cf = CatFile(self.path) if self.path else None

    def rev(self, sha):
        """The archive commit for a sha: mapped when rewritten."""
        if not self.path:
            return None
        return resolve(self.path, self.map.get(sha, sha))

    def close(self):
        """End the archive reader."""
        if self.cf:
            self.cf.close()


def golden_at_rows(a, arch, src_repo, src_sha, local_cf):
    """Resolve a revision's manifest rows. Returns (lines, hits,
    misses, pre_cutover_games)."""
    lines, hits, misses, pre = [], [], [], []
    games = [a.game] if a.game else list(GAMES)
    for game in games:
        found = False
        for mpath, tier, tdir in reference_manifests(game):
            cf = local_cf if src_repo == repo_root() else arch.cf
            data = cf.read("%s:%s" % (src_sha, mpath))
            if data is None:
                continue
            found = True
            m = parse_manifest(data.decode(errors="replace"), mpath)
            rows = [r for r in m.rows
                    if not a.filter or a.filter in r["tile"]]
            in_store = store_verified({r["digest"] for r in rows})
            for r in rows:
                key = (game, tier, r["tile"])
                if r["digest"] in in_store:
                    lines.append((mpath, r["tile"], "store"))
                    hits.append((key, "store", r["digest"]))
                    continue
                got = archive_tile(arch, r, tdir)
                if got:
                    lines.append((mpath, r["tile"], "archive"))
                    hits.append((key, "archive", got))
                    continue
                misses.append((mpath, r))
        if not found:
            pre.append(game)
    return lines, hits, misses, pre


def archive_tile(arch, row, tdir):
    """Archive object spec of a row's tile whose decoded digest equals
    the row's, or None. Read at the row's revision (through the commit map)
    and the tile's tracked path."""
    if not arch.path:
        return None
    sha = arch.rev(row["revision"])
    if not sha:
        return None
    spec = "%s:%s/%s" % (sha, tdir, row["tile"])
    data = arch.cf.read(spec)
    if data is None:
        return None
    got = digest_bytes(data)
    return spec if got and got[0] == row["digest"] else None


def materialize_supported():
    """True when the gate script carries --materialize-ref."""
    p = os.path.join(repo_root(), "tests/harness/check_presentation_golden.sh")
    try:
        with open(p) as fh:
            return "--materialize-ref" in fh.read()
    except OSError:
        return False


def cmd_golden_at(a):
    """Print, per manifest row at a revision, where its reference image
    is (`store`, `archive`) or what it costs (`materialize <price>`); list a
    pre-cutover revision's tracked PNGs as `unverified`."""
    root = repo_root()
    arch = Archive()
    local_sha = resolve(root, a.rev)
    if local_sha:
        src_repo, src_sha = root, local_sha
    else:
        src_repo, src_sha = arch.path, arch.rev(a.rev) if arch.path else None
    if not src_sha:
        print("[presentation-blessed] golden-at: %s resolves in neither this "
              "repository nor the archive (%s)" % (a.rev, arch.path or
                                                   "none named"),
              file=sys.stderr)
        arch.close()
        return 1
    local_cf = CatFile(root)
    lines, hits, misses, pre = golden_at_rows(a, arch, src_repo, src_sha,
                                              local_cf)
    rows_by_corpus = {}
    for mpath, r in misses:
        rows_by_corpus.setdefault(corpus_row(r["tile"]), []).append(r)
    for mpath, r in misses:
        lines.append((mpath, r["tile"], "materialize " + price(1)))
    unverified = []
    for game in pre:
        # A revision before the cutover carries no manifest: its tracked PNGs
        # are the reference, read from the archive where it holds the
        # revision, else from this repository; nothing verifies them.
        repo, sha = (arch.path, arch.rev(src_sha)) if arch.path else (None,
                                                                      None)
        if not sha:
            repo, sha = src_repo, src_sha
        ls = git("ls-tree", "-r", "--name-only", sha, "--",
                 "tests/golden/%s/" % game, cwd=repo) or ""
        for p in ls.splitlines():
            if p.endswith(".png") and (not a.filter or a.filter in p):
                lines.append((p, os.path.basename(p), "unverified"))
                unverified.append((repo, "%s:%s" % (sha, p), p))
    for mpath, tile, status in lines:
        print("%s\t%s\t%s" % (mpath, tile, status))
    print("[presentation-blessed] golden-at %s (%s): store=%d archive=%d "
          "materialize=%d unverified=%d%s"
          % (a.rev, src_sha[:12], sum(1 for h in hits if h[1] == "store"),
             sum(1 for h in hits if h[1] == "archive"), len(misses),
             len(unverified),
             "; materialize total " + price(len(rows_by_corpus))
             if misses else ""))
    if a.out:
        copied = golden_at_copy(a.out, hits, unverified, arch, local_cf)
        print("[presentation-blessed] copied %d file(s) to %s"
              % (copied, a.out))
    local_cf.close()
    arch.close()
    if misses and a.materialize:
        if not materialize_supported():
            print("[presentation-blessed] --materialize: the gate script "
                  "carries no --materialize-ref; %d row(s) not materialized"
                  % len(misses), file=sys.stderr)
            return 1
        tiles = sorted({r["tile"] for _, r in misses})
        return subprocess.call(
            ["tests/harness/check_presentation_golden.sh",
             "--materialize-ref"] + tiles, cwd=root)
    return 0


def golden_at_copy(out, hits, unverified, arch, local_cf):
    """Copy the resolved reference images into out/<game>/<tier>/."""
    n = 0
    for (game, tier, tile), kind, ref in hits:
        d = os.path.join(out, game, tier)
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, tile)
        if kind == "store":
            shutil.copyfile(blob_path(ref), dst)
        else:
            with open(dst, "wb") as fh:
                fh.write(arch.cf.read(ref))
        n += 1
    for repo, spec, path in unverified:
        cf = arch.cf if repo == arch.path else local_cf
        data = cf.read(spec)
        if data is None:
            continue
        dst = os.path.join(out, "unverified", path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(data)
        n += 1
    return n


# ---- capture record, census, promote ----------------------------------------

# Source roots of the build: the blessed revision is the commit whose tree over
# these roots equals the capture's working copy. The recompiled source trees
# (games/<game>/generated, ignored in a checkout, a symlink in a worktree) are
# excluded; the recomp identity columns carry them.
SOURCE_ROOTS = ("core", "games", "scripts", "CMakeLists.txt", "cmake")
SOURCE_EXCLUDES = (":(exclude,glob)games/*/generated",
                   ":(exclude,glob)games/*/generated/**",
                   ":(exclude,glob)games/*/*/build/**")
RECORD_NAME = "capture-record.txt"


def source_tree():
    """Tree sha of the working copy over SOURCE_ROOTS: a temporary index
    seeded from HEAD, the roots added to it (tracked edits, deletions and
    untracked files git does not ignore), then `git write-tree`."""
    root = repo_root()
    fd, idx = tempfile.mkstemp(prefix="golden-src-index-")
    os.close(fd)
    os.unlink(idx)
    env = dict(os.environ, GIT_INDEX_FILE=idx)
    try:
        for args in (["read-tree", "HEAD"],
                     ["add", "-A", "--"] + list(SOURCE_ROOTS)
                     + list(SOURCE_EXCLUDES)):
            p = subprocess.run(["git", "-C", root] + args, env=env,
                               capture_output=True, text=True)
            if p.returncode != 0:
                raise RuntimeError("git %s: %s" % (args[0], p.stderr.strip()))
        p = subprocess.run(["git", "-C", root, "write-tree"], env=env,
                           capture_output=True, text=True, check=True)
        return p.stdout.strip()
    finally:
        if os.path.exists(idx):
            os.unlink(idx)


def source_tree_diff(tree_a, tree_b):
    """Paths under SOURCE_ROOTS at which two trees differ, sorted."""
    out = git("diff-tree", "-r", "--name-only", tree_a, tree_b, "--",
              *SOURCE_ROOTS) or ""
    return sorted(ln for ln in out.splitlines() if ln)


def read_record(cand):
    """The capture record of a candidate directory as a dict; {} when
    absent."""
    out = {}
    try:
        with open(os.path.join(cand, RECORD_NAME)) as fh:
            for line in fh:
                k, sep, v = line.rstrip("\n").partition("=")
                if sep:
                    out[k] = v
    except OSError:
        pass
    return out


def cmd_capture_record(a):
    """Write the capture record into a candidate directory: the source
    tree over SOURCE_ROOTS, HEAD, and the recomp identity of the build."""
    rec = {"game": a.game, "tier": a.tier,
           "head": git("rev-parse", "HEAD").strip(),
           "source-tree": source_tree(),
           "recomp-input": recomp_input_digest(a.game),
           "generated-tree": generated_tree_digest(a.game)}
    os.makedirs(a.candidate, exist_ok=True)
    path = os.path.join(a.candidate, RECORD_NAME)
    with open(path + ".tmp", "w") as fh:
        for k, v in rec.items():
            fh.write("%s=%s\n" % (k, v))
    os.replace(path + ".tmp", path)
    print("[presentation-blessed] capture record %s: source-tree %s"
          % (path, rec["source-tree"][:12]))
    return 0


def cmd_census(a):
    """Print a manifest's row count and how many candidate tiles name a
    row: `rows=N matched=M`."""
    try:
        m = load_manifest(a.game, a.tier)
    except (OSError, ValueError) as e:
        print("[presentation-blessed] census: %s" % e, file=sys.stderr)
        return 1
    names = {r["tile"] for r in m.rows}
    tiles = set(list_tiles(a.candidate)) if a.candidate else set()
    print("rows=%d matched=%d" % (len(names), len(tiles & names)))
    return 0


def resolve_reference(row, tdir, arch=None):
    """A reference image for a manifest row, from the store, else from
    the archive (golden-at's first two tiers). Returns (source, where):
    source is a path or a BytesIO PIL opens, where is `store` or
    `archive`; (None, None) when neither holds it."""
    p = store_get(row["digest"])
    if p:
        return p, "store"
    if arch is not None:
        spec = archive_tile(arch, row, tdir)
        if spec:
            return io.BytesIO(arch.cf.read(spec)), "archive"
    return None, None


def blessed_revision(rec):
    """HEAD when HEAD's tree equals the capture record's source tree over
    SOURCE_ROOTS. Returns (sha, None) or (None, reason)."""
    tree = rec.get("source-tree")
    if not tree:
        return None, "the candidate carries no %s" % RECORD_NAME
    if git("cat-file", "-e", tree + "^{tree}", check=False) is None:
        return None, "source tree %s is not in this repository" % tree
    diff = source_tree_diff("HEAD^{tree}", tree)
    if diff:
        return None, ("HEAD's tree differs from the capture's source tree "
                      "at %s (%d path(s))" % (diff[0], len(diff)))
    return git("rev-parse", "HEAD").strip(), None


def promote_select(cand, all_tiles, filters):
    """Candidate tiles a promote considers: every golden tile under
    --all, else those whose basename contains a filter."""
    tiles = list_tiles(cand)
    if all_tiles:
        return tiles
    return [t for t in tiles if any(f and f in t for f in filters)]


def cmd_promote(a):
    """Rewrite the manifest rows of promoted candidate tiles (digest,
    blessed revision, recomp identity, origin) and file the tiles into the
    store. A tile whose digest equals its row's is unchanged and keeps its
    row. The header is kept, its size list extended by any new size."""
    root = repo_root()
    rec = read_record(a.candidate)
    if rec.get("game", a.game) != a.game or rec.get("tier", a.tier) != a.tier:
        print("[presentation-promote] the candidate was captured for %s/%s, "
              "not %s/%s" % (rec.get("game"), rec.get("tier"), a.game, a.tier),
              file=sys.stderr)
        return 1
    try:
        m = load_manifest(a.game, a.tier)
    except (OSError, ValueError) as e:
        print("[presentation-promote] %s" % e, file=sys.stderr)
        return 1
    rows = {r["tile"]: r for r in m.rows}
    sel = promote_select(a.candidate, a.all, a.filters)
    paths = [os.path.join(a.candidate, t) for t in sel]
    digs = digest_files(paths)
    bad = [p for p in paths if digs[p] is None]
    if bad:
        print("[presentation-promote] undecodable: %s" % bad[0],
              file=sys.stderr)
        return 1
    changed = [(t, p) for t, p in zip(sel, paths)
               if t not in rows or rows[t]["digest"] != digs[p][0]]
    if not changed:
        print("[presentation-promote] nothing to promote (no matching / "
              "changed tiles)")
        return 0
    rev, why = blessed_revision(rec)
    if rev is None:
        print("[presentation-promote] REFUSED: %s; commit the capture's "
              "sources, rebuild and recapture, then promote" % why,
              file=sys.stderr)
        return 1
    sizes = {s for s in m.header.get("size", "").split(",")
             if re.match(r"^\d+x\d+$", s)}
    for t, p in changed:
        print("  %s  %s" % ("replace" if t in rows else "add", t))
        rows[t] = dict(tile=t, digest=digs[p][0], revision=rev,
                       recomp_input=rec.get("recomp-input", "unrecorded"),
                       generated_tree=rec.get("generated-tree", "unrecorded"),
                       origin="mapped")
        sizes.add("%dx%d" % digs[p][1])
        store_put(p, digs[p][0])
    m.header["size"] = ",".join(sorted(sizes, key=lambda s: tuple(
        int(x) for x in s.split("x")))) or "none"
    m.rows = list(rows.values())
    out = os.path.join(root, manifest_path(a.game, a.tier))
    with open(out + ".tmp", "w") as fh:
        fh.write(format_manifest(m))
    os.replace(out + ".tmp", out)
    print("[presentation-promote] %s: %d row(s) rewritten, blessed revision "
          "%s, tiles filed into %s" % (manifest_path(a.game, a.tier),
                                       len(changed), rev[:12], store_dir()))
    return 0


# ---- CLI --------------------------------------------------------------------

def main(argv=None):
    """Parse the command line and dispatch."""
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def game_tier(p):
        p.add_argument("game", choices=GAMES)
        p.add_argument("--tier", choices=sorted(TIERS),
                       default="presentation")

    p = sub.add_parser("digest")
    p.add_argument("png", nargs="+")
    p = sub.add_parser("read")
    game_tier(p)
    p = sub.add_parser("write")
    game_tier(p)
    p.add_argument("--from", dest="src", help="tile directory "
                   "(default: the tier's golden directory)")
    p.add_argument("--revision", help="blessed revision (default HEAD)")
    p.add_argument("--preset", default="linux-release")
    p.add_argument("--env", action="append", metavar="KEY=VALUE")
    p.add_argument("--environment-accept", metavar="REASON")
    p.add_argument("--no-store", action="store_true")
    p = sub.add_parser("verify")
    game_tier(p)
    p.add_argument("--from", dest="src")
    p.add_argument("--store-only", action="store_true",
                   help="check the store alone (no tile directory)")
    p = sub.add_parser("store-file")
    p.add_argument("png", nargs="+")
    p = sub.add_parser("store-read")
    p.add_argument("digest")
    p.add_argument("--out")
    p = sub.add_parser("gc")
    p.add_argument("--keep-days", type=int, default=90)
    p.add_argument("--to-head", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("capture-record")
    game_tier(p)
    p.add_argument("candidate")
    p = sub.add_parser("census")
    game_tier(p)
    p.add_argument("--candidate")
    p = sub.add_parser("promote")
    game_tier(p)
    p.add_argument("candidate")
    p.add_argument("filters", nargs="*", help="tile basename substrings")
    p.add_argument("--all", action="store_true")
    p = sub.add_parser("golden-at")
    p.add_argument("rev")
    p.add_argument("game", nargs="?", choices=GAMES)
    p.add_argument("filter", nargs="?", help="tile basename substring")
    p.add_argument("--out")
    p.add_argument("--materialize", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd in ("read", "write", "verify", "capture-record", "census",
                 "promote") and a.game not in TIERS[a.tier]:
        ap.error("tier %s has no game %s" % (a.tier, a.game))
    return {"digest": cmd_digest, "read": cmd_read, "write": cmd_write,
            "verify": cmd_verify, "store-file": cmd_store_file,
            "store-read": cmd_store_read, "gc": cmd_gc,
            "golden-at": cmd_golden_at, "capture-record": cmd_capture_record,
            "census": cmd_census, "promote": cmd_promote}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
