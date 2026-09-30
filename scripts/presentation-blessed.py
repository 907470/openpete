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

`revision` is the engine revision the tile was blessed from. The recomp
identity pair describes the untracked inputs and outputs of the regen, which
the revision does not pin; both are content digests, so a checkout at another
path, a decomp re-made by another linker and stale files in generated/ leave
them unchanged. `recomp_input` digests PSX.EXE and every overlay by bytes,
psx.map by the symbol table the regen parses from it (object-section and
symbol tuples, sorted, no header, paths or debug sections), and
manifest/globals.toml and manifest/symbols.toml without their comment lines
(recomp_input_digest). `generated_tree` digests the regen outputs in
games/<game>/generated that the build reads: the main EXE's C files, the
overlay table, the PSYQ stubs, channel_sites.txt where the game has a channel
manifest, and each registered overlay's C pair; any other file there is
ignored, and a missing output or an unregistered overlay source the build
would compile is refused (generated_tree_digest). `origin` is `mapped` (the
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
  capture-record <game> [--tier T] <candidate> [--build-dir DIR]
          [--fields FILE]         write capture-record.txt into a candidate
                                  directory: the working copy's tree over the
                                  build's source roots, HEAD, the recomp
                                  identity and the capture environment. With
                                  --fields, head, source-tree, recomp-input and
                                  generated-tree are read from FILE (`key:
                                  value` lines) and no git runs; the
                                  environment is measured in place either way
  capture-fields <game> [--out FILE]
                                  print (or write) those four fields of the
                                  checkout as the file --fields reads
  identity <game>                 print the recomp identity pair of the
                                  checkout (recomp-input=, generated-tree=)
  rebless-identity <game> [--tier T]
                                  rewrite the recomp_input and generated_tree
                                  columns of every row from the checkout's
                                  identity; digests, revisions and the header
                                  are kept. Refused outside the capture image
                                  the header's recipe-hash names and when the
                                  working tree differs from HEAD over the
                                  recomp inputs (recomp_input_paths). Run it
                                  inside the image after the regens:
                                    scripts/golden-capture-container.sh run --
                                      python3 scripts/presentation-blessed.py
                                      rebless-identity <game> --tier <T>
  environment <game> [--tier T] [--candidate DIR] [--strict]
                                  print the header's comparison surface beside
                                  a candidate's (or this process's) environment
  census <game> [--tier T] [--candidate DIR]
                                  print `rows=N matched=M`: manifest rows, and
                                  candidate tiles naming a row
  promote <game> [--tier T] <candidate> [--all | <tile-filter>...]
          [--exclude SUBSTR]... [--create] [--environment-accept REASON]
                                  rewrite the rows of changed candidate tiles
                                  and file them into the store; the blessed
                                  revision is HEAD when HEAD's tree equals the
                                  record's source tree, else a refusal naming
                                  the first differing path; refused outside
                                  the pinned capture environment (Environment)
                                  and when the record's host is not this
                                  machine (a record without a host is local)

Environment: each header records the capture environment. The comparison
surface (SURFACE_KEYS) is the hash of the image recipe the capture ran in, the
Mesa driver version and the device name from the engine's Vulkan device line,
the compiler version the build prints, the preset, and the disc .bin and .cue
hashes; the image digest and the Vulkan loader version are recorded and not
compared; the host name and CPU model are written to the capture record only.
A promote is refused when the candidate's surface differs from the
header's, when a surface field was not measured, or when the promote itself
runs outside the capture image; --environment-accept "<reason>" admits the
departure and writes the reason into the header.

Exit: 0 success; 1 mismatch, refusal or missing input; 2 usage.
"""
import argparse
import hashlib
import importlib.util
import io
import os
import pathlib
import re
import shutil
import socket
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
TIERS = {"presentation": GAMES, "dragons": ("spyro-1",),
         "native-vk": ("spyro-1",)}
# The native-tick tier's manifest sits beside its corpus, outside tests/golden.
NATIVE_TICK_MANIFEST = "games/spyro-1/native-tick-corpus.manifest"
COLUMNS = ("tile", "digest", "revision", "recomp_input", "generated_tree",
           "origin")
ORIGINS = ("mapped", "archive-only")
DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
BLOB_RE = re.compile(r"^([0-9a-f]{64})\.png$")
HEADER_TITLE = "presentation golden reference manifest"
DIGEST_RULE = ('sha256(b"RGB\\n" + width + b"\\n" + height + b"\\n" + '
               'RGB bytes in scanline order), PIL convert("RGB")')
# Environment fields of a manifest header; a value not measured is written
# `unrecorded`. SURFACE_KEYS are the comparison surface: a bless or promote
# whose capture differs from the header in one of them is refused unless
# --environment-accept records the reason. image-digest and libvulkan are
# recorded and never compared: two builds of one recipe produce different
# image digests, and the loader version is fixed by the recipe hash. host
# (the machine's hostname, host_here) and cpu (the /proc/cpuinfo model name)
# are written to the capture record only, never to a manifest header: they
# name the machine that captured, a promote refuses a capture record whose
# host is not its own (cmd_promote), and a committed header names no machine.
ENV_KEYS = ("recipe-hash", "mesa", "device", "compiler", "preset",
            "image-digest", "libvulkan")
RECORDED_KEYS = ("image-digest", "libvulkan", "host", "cpu")
SURFACE_KEYS = ("recipe-hash", "mesa", "device", "compiler", "preset",
                "disc-bin", "disc-cue")
# The capture image carries the recipe it was built from at IMAGE_RECIPE
# (docs/linux-capture.Dockerfile copies itself there), so a process can tell
# whether it runs inside the image and which recipe built it.
RECIPE = "docs/linux-capture.Dockerfile"
IMAGE_RECIPE = "/etc/openpete/capture.Dockerfile"
OUTSIDE_IMAGE = "none (outside the capture image)"
# The capture legs append the engine's Vulkan device line
# (`[rhi/vk] device='<name>' ... driver-info='<info>'`) to this file in the
# candidate; for lavapipe the driver info is the Mesa version string.
DEVICE_NAME = "capture-device.txt"
DEVICE_RE = re.compile(r"device='([^']*)'.*?\bdriver-info='([^']*)'")
# Materialize price components (the materialize rebuilds the blessed revision
# and re-captures each corpus row a divergent tile belongs to): worktree,
# decomp re-make, PSXRecomp and regens ~1 min; the in-image configure and
# game build 3 to 10 min (spyro-1 to spyro-3); a capture per corpus row.
PRICE_PREP_MIN = 1
PRICE_BUILD_MIN = (3, 10)
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
    """Repo-relative blessed.tsv of a game's tier; the native-tick tier's
    manifest is NATIVE_TICK_MANIFEST."""
    if tier == "native-vk":
        return NATIVE_TICK_MANIFEST
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
        out.append((NATIVE_TICK_MANIFEST, "native-vk",
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


class IdentityError(RuntimeError):
    """A recomp identity that cannot be computed: an expected regen output
    is missing, or the build would compile a file the regen did not write."""


def load_map_parser():
    """The psx.map parser the regen runs (tools/build_manifest.py of the
    checkout repo_root() names), imported fresh so a materialize worktree
    digests with its own revision's parser."""
    path = os.path.join(repo_root(), "tools", "build_manifest.py")
    name = "_openpete_build_manifest"
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    # dataclasses resolves the module's annotations through sys.modules.
    sys.modules[name] = mod
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.modules.pop(name, None)
    return mod


def map_symbol_table(path, parser=None):
    """Canonical text of the symbol table a psx.map feeds the regen: one
    line per object-section record (`S <section> <addr> <size> <object>`)
    and per symbol (`Y <addr> <section> <name> <object>`), addresses and
    sizes as 8-digit lowercase hex, lines sorted. The records are the ones
    tools/build_manifest.py's parse_map returns, which is every tuple
    regen-manifest.sh derives symbols.toml and globals.toml from."""
    parser = parser or load_map_parser()
    lines = []
    for sec in parser.parse_map(pathlib.Path(path)):
        lines.append("S\t%s\t%08x\t%08x\t%s" % (sec.name, sec.addr, sec.size,
                                               sec.source_object))
        for sym in sec.symbols:
            lines.append("Y\t%08x\t%s\t%s\t%s" % (sym.addr, sym.section,
                                                 sym.name, sec.source_object))
    return "".join(ln + "\n" for ln in sorted(lines))


def toml_content_digest(path):
    """SHA-256 of a generated TOML file with its comment lines removed:
    build_manifest.py's `# Source: <map path>` header carries the path
    spelling the writer was invoked with, and no consumer reads a
    comment."""
    hsh = hashlib.sha256()
    with open(path, "rb") as fh:
        for line in fh:
            if not line.lstrip().startswith(b"#"):
                hsh.update(line)
    return hsh.hexdigest()


def recomp_input_digest(game):
    """Digest of the untracked inputs of a game's regen: PSX.EXE and every
    overlay by bytes, psx.map by its symbol table (map_symbol_table), and
    the map-derived manifest/globals.toml and manifest/symbols.toml by
    their non-comment lines. The map's bytes are not hashed: its header,
    .debug sections and address width carry the linker version and the
    absolute directory the decomp was built in, none of which reaches a
    symbol tuple. Each entry is `<repo-relative path>\\t<digest>`, sorted;
    an absent input enters as `absent`."""
    root = repo_root()
    conf = source_conf("games/%s/recomp.conf" % game,
                       ("PSX_EXE", "WAD_DIR", "DECOMP_DIR"))
    map_rel = conf["DECOMP_DIR"] + "/build/psx.map"
    tomls = ["games/%s/manifest/globals.toml" % game,
             "games/%s/manifest/symbols.toml" % game]
    paths = [conf["PSX_EXE"], map_rel] + tomls
    wad = os.path.join(root, conf["WAD_DIR"])
    if os.path.isdir(wad):
        paths += [conf["WAD_DIR"] + "/" + f for f in sorted(os.listdir(wad))
                  if f.endswith(".ovl")]
    hsh = hashlib.sha256()
    for rel in sorted(paths):
        ap = os.path.join(root, rel)
        if not os.path.isfile(ap):
            val = "absent"
        elif rel == map_rel:
            val = "symbols:" + hashlib.sha256(
                map_symbol_table(ap).encode()).hexdigest()
        elif rel in tomls:
            val = "toml:" + toml_content_digest(ap)
        else:
            val = sha256_file(ap)
        hsh.update(("%s\t%s\n" % (rel, val)).encode())
    return hsh.hexdigest()


# The regen's outputs the game build reads, besides the per-overlay pair:
# cmake/openpete_game.cmake compiles the four C files, and spyro-1's
# CMakeLists copies channel_sites.txt into the recomp module kit.
GENERATED_FIXED = ("PSX.EXE_full.c", "PSX.EXE_dispatch.c",
                   "spyro_overlay_table.c", "psyq_stubs.c")
GENERATED_CHANNEL = ("channel_sites.txt",)
# spyro_overlay_table.c row: `{ <id>, "<name>", psx_dispatch_<name> },`.
OVERLAY_ROW_RE = re.compile(
    r'^\s*\{\s*\d+,\s*"([A-Za-z0-9_]+)",\s*psx_dispatch_\1\s*\},\s*$',
    re.MULTILINE)
# The build compiles every file these globs match (file(GLOB) in
# cmake/openpete_game.cmake), whoever wrote it.
OVERLAY_SOURCE_RE = re.compile(r"^([^/]+)/\1\.bin_(full|dispatch)\.c$")


def generated_outputs(game):
    """Paths, relative to games/<game>/generated, of the regen outputs the
    game build reads: GENERATED_FIXED, channel_sites.txt when recomp.conf
    names a CHANNEL_MANIFEST, and <name>/<name>.bin_full.c and
    .bin_dispatch.c for every overlay spyro_overlay_table.c registers (the
    regen writes the table from the overlays it translated). Raises
    IdentityError when the table is missing."""
    top = os.path.join(repo_root(), "games", game, "generated")
    table = os.path.join(top, "spyro_overlay_table.c")
    if not os.path.isfile(table):
        raise IdentityError(
            "%s: games/%s/generated/spyro_overlay_table.c is missing; run "
            "`GAME=%s bash scripts/regen-recomp.sh`" % (game, game, game))
    with open(table, errors="replace") as fh:
        names = OVERLAY_ROW_RE.findall(fh.read())
    conf = source_conf("games/%s/recomp.conf" % game, ("CHANNEL_MANIFEST",))
    out = list(GENERATED_FIXED)
    if conf["CHANNEL_MANIFEST"]:
        out += GENERATED_CHANNEL
    for n in names:
        out += ["%s/%s.bin_full.c" % (n, n), "%s/%s.bin_dispatch.c" % (n, n)]
    return sorted(set(out))


def generated_tree_digest(game):
    """Digest of the regen's outputs in games/<game>/generated that the
    build reads (generated_outputs): each file's relative path and content
    hash, sorted. Any other file in the directory (recompiler scratch,
    per-pass forced-entry lists, logs, outputs of a retired regen step) is
    not read, so a stale file cannot change the digest. Raises
    IdentityError naming the file when an expected output is missing, and
    when an overlay source the build's glob compiles is not one the
    overlay table registers, since that stale file would enter the
    binary."""
    top = os.path.join(repo_root(), "games", game, "generated")
    if not os.path.isdir(top):
        raise IdentityError("%s: games/%s/generated does not exist; run "
                            "`GAME=%s bash scripts/regen-recomp.sh`"
                            % (game, game, game))
    want = generated_outputs(game)
    missing = [r for r in want if not os.path.isfile(os.path.join(top, r))]
    if missing:
        raise IdentityError(
            "%s: regen output games/%s/generated/%s is missing (%d missing); "
            "run `GAME=%s bash scripts/regen-recomp.sh`"
            % (game, game, missing[0], len(missing), game))
    wanted = set(want)
    for d in sorted(os.listdir(top)):
        if not os.path.isdir(os.path.join(top, d)):
            continue
        for f in sorted(os.listdir(os.path.join(top, d))):
            rel = d + "/" + f
            if OVERLAY_SOURCE_RE.match(rel) and rel not in wanted:
                raise IdentityError(
                    "%s: games/%s/generated/%s is an overlay source the build "
                    "compiles but spyro_overlay_table.c does not register; "
                    "delete it or rerun the regen" % (game, game, rel))
    hsh = hashlib.sha256()
    for rel in want:
        hsh.update(("%s\t%s\n" % (rel, sha256_file(os.path.join(top, rel))))
                   .encode())
    return hsh.hexdigest()


def resolve_disc_bin(cue_rel):
    """The .bin the engine loads for a cue, repo-relative, or None: the
    first FILE "..." line's path (relative to the cue's directory), then the
    cue's own basename with .bin, then the lone .bin beside the cue (none
    when there are several), the order openpete_psx_resolve_disc_bin in
    core/psx/exe_loader.cpp tries."""
    root = repo_root()
    cue_dir = os.path.dirname(cue_rel)
    order = []
    with open(os.path.join(root, cue_rel), errors="replace") as fh:
        for line in fh:
            m = re.search(r'FILE[^"]*"([^"]+)"', line)
            if m:
                order.append(m.group(1) if os.path.isabs(m.group(1))
                             else os.path.join(cue_dir, m.group(1)))
                break
    order.append(os.path.splitext(cue_rel)[0] + ".bin")
    try:
        bins = [f for f in os.listdir(os.path.join(root, cue_dir or "."))
                if f.lower().endswith(".bin")]
    except OSError:
        bins = []
    if len(bins) == 1:
        order.append(os.path.join(cue_dir, bins[0]))
    for c in order:
        if os.path.isfile(os.path.join(root, c)):
            return os.path.normpath(c)
    return None


def disc_hashes(game, preset):
    """Content hashes of the disc .cue and the .bin the engine loads for
    it (resolve_disc_bin), memoized in build/<preset>/golden-disc-hash.tsv
    by path, size and mtime (the memo decides only whether to re-hash).
    Returns {"disc-bin": ..., "disc-cue": ...}, each `<sha256> <repo-relative
    path>` or `absent <path>`."""
    root = repo_root()
    cue = source_conf("games/%s/oracle.conf" % game, ("DISC",))["DISC"]
    out = {"disc-bin": "absent", "disc-cue": "absent"}
    if not cue:
        return out
    cue_ap = os.path.join(root, cue)
    if not os.path.isfile(cue_ap):
        out["disc-cue"] = "absent " + cue
        return out
    bin_rel = resolve_disc_bin(cue)
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

    out["disc-cue"] = "%s %s" % (hashed(cue_ap), os.path.normpath(cue))
    if bin_rel:
        out["disc-bin"] = "%s %s" % (hashed(os.path.join(root, bin_rel)),
                                     bin_rel)
    else:
        out["disc-bin"] = "absent (no .bin resolves for %s)" % cue
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


# ---- capture environment -----------------------------------------------------

def recipe_hash_here():
    """SHA-256 of the recipe the running capture image was built from, or
    OUTSIDE_IMAGE when this process does not run inside the image."""
    if not os.path.isfile(IMAGE_RECIPE):
        return OUTSIDE_IMAGE
    return sha256_file(IMAGE_RECIPE)


def package_version(pkg):
    """The installed version of a Debian package; `unrecorded` when
    dpkg-query cannot name it."""
    try:
        p = subprocess.run(["dpkg-query", "-W", "-f", "${Version}", pkg],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "unrecorded"
    v = p.stdout.strip()
    return v if p.returncode == 0 and v else "unrecorded"


def preset_of(build_dir):
    """The preset a build directory belongs to: its basename
    (build/<preset>)."""
    return os.path.basename(os.path.normpath(build_dir))


def host_here():
    """The name of the machine this process runs on: $OPENPETE_CAPTURE_HOST
    when set, else `unrecorded` inside the capture image (a container's
    hostname is its container id, which names no machine), else the
    hostname. The capture image's callers pass the machine's hostname in
    OPENPETE_CAPTURE_HOST (pg_enter_capture_image)."""
    h = os.environ.get("OPENPETE_CAPTURE_HOST", "").strip()
    if h:
        return h
    if recipe_hash_here() != OUTSIDE_IMAGE:
        return "unrecorded"
    return socket.gethostname() or "unrecorded"


def cpu_here():
    """The first `model name` of /proc/cpuinfo, or `unrecorded`."""
    try:
        with open("/proc/cpuinfo", errors="replace") as fh:
            for line in fh:
                k, sep, v = line.partition(":")
                if sep and k.strip() == "model name" and v.strip():
                    return " ".join(v.split())
    except OSError:
        pass
    return "unrecorded"


def environment_here(game, preset):
    """The capture environment measurable without running the engine: the
    image's recipe hash and digest, the Vulkan loader version, the compiler
    of the preset's build directory, the preset, the host and CPU model and
    the disc hashes. The Mesa and device strings come from the engine's log
    (device_record)."""
    env = {"recipe-hash": recipe_hash_here(),
           "image-digest": os.environ.get("OPENPETE_CAPTURE_IMAGE_DIGEST")
           or "unrecorded",
           "libvulkan": package_version("libvulkan1"),
           "compiler": compiler_version(preset), "preset": preset,
           "host": host_here(), "cpu": cpu_here()}
    env.update(disc_hashes(game, preset))
    return env


def device_record(cand):
    """(mesa, device) from a candidate's DEVICE_NAME file: the driver
    info and the device name of the engine's Vulkan device line, each
    `unrecorded` when no leg reported one and every distinct value joined
    with ` | ` when the legs disagree."""
    names, drivers = set(), set()
    try:
        with open(os.path.join(cand, DEVICE_NAME), errors="replace") as fh:
            for line in fh:
                m = DEVICE_RE.search(line)
                if m:
                    names.add(m.group(1))
                    drivers.add(m.group(2))
    except OSError:
        pass
    return (" | ".join(sorted(drivers)) or "unrecorded",
            " | ".join(sorted(names)) or "unrecorded")


def capture_environment(cand):
    """The environment a candidate was captured in: the record's fields
    (written by capture-record inside the capture run) and the device
    line the capture legs logged."""
    rec = read_record(cand)
    env = {k: rec.get(k, "unrecorded")
           for k in ENV_KEYS + ("disc-bin", "disc-cue")}
    env["mesa"], env["device"] = device_record(cand)
    return env


def environment_problems(header, env):
    """Why a bless of a capture taken in `env` against a manifest carrying
    `header` departs from the pinned environment, as (field, header value,
    capture value) triples: a surface field that differs, a surface field
    the capture did not measure, and a blessing process running outside
    the capture image. Empty when the bless may proceed."""
    out = []
    for k in SURFACE_KEYS:
        h, c = header.get(k, "unrecorded"), env.get(k, "unrecorded")
        if c == "unrecorded" or h != c:
            out.append((k, h, c))
    here = recipe_hash_here()
    if here == OUTSIDE_IMAGE:
        out.append(("blessing-process", "inside the capture image", here))
    return out


ENV_REFUSAL_HINT = (
    "  Capture and bless inside the capture image "
    "(scripts/golden-capture-container.sh); a deliberate migration passes "
    "--environment-accept \"<reason>\", which writes the reason into the "
    "header.")


def environment_refused(who, problems, accept, hint=ENV_REFUSAL_HINT):
    """Print the environment refusal for `problems` and return True, or
    return False when there are none or --environment-accept admits
    them (the admission is printed). `hint` closes the refusal."""
    if not problems:
        return False
    if accept:
        print("[%s] environment departs from the manifest header; admitted by "
              "--environment-accept %r:" % (who, accept))
        for k, h, c in problems:
            print("    %s: header=%s capture=%s" % (k, h, c))
        return False
    print("[%s] REFUSED: the capture environment is not the pinned one "
          "recorded in the manifest header:" % who, file=sys.stderr)
    for k, h, c in problems:
        print("    %s: header=%s capture=%s" % (k, h, c), file=sys.stderr)
    print(hint, file=sys.stderr)
    return True


def apply_environment(header, env, accept):
    """A copy of `header` carrying `env`'s environment fields and disc
    hashes; environment-accept becomes `accept` when given and keeps its
    value otherwise."""
    tail = ENV_KEYS + ("disc-bin", "disc-cue", "environment-accept")
    out = {k: v for k, v in header.items() if k not in tail}
    for k in ENV_KEYS + ("disc-bin", "disc-cue"):
        out[k] = env.get(k, "unrecorded")
    out["environment-accept"] = (" ".join(accept.split()) if accept
                                 else header.get("environment-accept", "none"))
    return out


def env_header_changed(header, env):
    """True when `header`'s environment fields differ from `env`'s."""
    return any(header.get(k, "unrecorded") != env.get(k, "unrecorded")
               for k in ENV_KEYS + ("disc-bin", "disc-cue"))


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
    return ("prep~%dmin+build~%d-%dmin+capture~%ds*%d corpus row(s)"
            % ((PRICE_PREP_MIN,) + PRICE_BUILD_MIN + (PRICE_ROW_S, nrows)))


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
        # Only a tier's blessed.tsv row is regenerable by a materialize.
        tiers = {manifest_path(g, t): (g, t) for t in TIERS for g in TIERS[t]}
        groups, rc = {}, 0
        for mpath, r in misses:
            if mpath not in tiers:
                print("[presentation-blessed] --materialize: %s row %s is "
                      "not a blessed.tsv row; not materialized"
                      % (mpath, r["tile"]), file=sys.stderr)
                rc = 1
                continue
            groups.setdefault(tiers[mpath], []).append(r["tile"])
        sys.stdout.flush()
        for (game, tier), tiles in sorted(groups.items()):
            extra = [] if src_repo == root else ["--from-archive"]
            rc |= subprocess.call(
                ["tests/harness/check_presentation_golden.sh",
                 "--materialize-ref", "--game", game, "--tier", tier,
                 "--at", src_sha] + extra + sorted(set(tiles)), cwd=root)
        return rc
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


# The capture record's fields derived from the checkout: HEAD and the source
# tree need git and the repository's objects, the recomp identity the
# untracked regen inputs and outputs.
TREE_FIELDS = ("head", "source-tree", "recomp-input", "generated-tree")


def tree_fields(game):
    """The TREE_FIELDS of the checkout, measured here."""
    return {"head": git("rev-parse", "HEAD").strip(),
            "source-tree": source_tree(),
            "recomp-input": recomp_input_digest(game),
            "generated-tree": generated_tree_digest(game)}


def read_tree_fields(path):
    """TREE_FIELDS from a file of `key: value` lines (blank lines and `#`
    lines skipped). Raises ValueError naming an unknown, repeated, empty or
    missing key."""
    out = {}
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            k, sep, v = s.partition(":")
            k, v = k.strip(), v.strip()
            if not sep or k not in TREE_FIELDS or k in out or not v:
                raise ValueError("%s:%d: expected `<key>: <value>` with a "
                                 "key of %s, once each" % (
                                     path, n, ", ".join(TREE_FIELDS)))
            out[k] = v
    missing = [k for k in TREE_FIELDS if k not in out]
    if missing:
        raise ValueError("%s: missing %s" % (path, ", ".join(missing)))
    return out


def cmd_capture_fields(a):
    """Print (or write to --out) the checkout's TREE_FIELDS as the
    `key: value` file capture-record --fields reads."""
    text = "".join("%s: %s\n" % kv for kv in tree_fields(a.game).items())
    if a.out:
        with open(a.out + ".tmp", "w") as fh:
            fh.write(text)
        os.replace(a.out + ".tmp", a.out)
    else:
        sys.stdout.write(text)
    return 0


def cmd_capture_record(a):
    """Write the capture record into a candidate directory: the source
    tree over SOURCE_ROOTS, HEAD, the recomp identity of the build and the
    capture environment. Under --fields the four tree fields come from the
    file and no git runs; the environment is measured here either way."""
    rec = {"game": a.game, "tier": a.tier}
    if a.fields:
        try:
            rec.update(read_tree_fields(a.fields))
        except (OSError, ValueError) as e:
            print("[presentation-blessed] capture-record --fields: %s" % e,
                  file=sys.stderr)
            return 1
    else:
        rec.update(tree_fields(a.game))
    rec.update(environment_here(a.game, preset_of(a.build_dir)))
    os.makedirs(a.candidate, exist_ok=True)
    path = os.path.join(a.candidate, RECORD_NAME)
    with open(path + ".tmp", "w") as fh:
        for k, v in rec.items():
            fh.write("%s=%s\n" % (k, v))
    os.replace(path + ".tmp", path)
    print("[presentation-blessed] capture record %s: source-tree %s"
          % (path, rec["source-tree"][:12]))
    return 0


def cmd_environment(a):
    """Print the manifest header's comparison surface beside the capture
    environment: a candidate's record when one is named, else what this
    process measures (no Mesa or device string without a capture). Exit 1
    on a difference under --strict, else 0."""
    try:
        header = load_manifest(a.game, a.tier).header
    except (OSError, ValueError) as e:
        print("[presentation-env] %s" % e, file=sys.stderr)
        header = {}
    if a.candidate:
        env, what = capture_environment(a.candidate), "capture"
    else:
        env = environment_here(a.game, preset_of(a.build_dir))
        env["mesa"] = env["device"] = "unmeasured (no capture)"
        what = "here"
    diff = [k for k in SURFACE_KEYS
            if header.get(k, "unrecorded") != env.get(k, "unrecorded")]
    for k in SURFACE_KEYS:
        print("  %s %s: header=%s %s=%s" % ("ENV-MISMATCH" if k in diff
                                            else "env-match   ", k,
                                            header.get(k, "unrecorded"), what,
                                            env.get(k, "unrecorded")))
    for k in RECORDED_KEYS:
        print("  env-recorded %s: header=%s %s=%s" % (
            k, header.get(k, "unrecorded"), what, env.get(k, "unrecorded")))
    print("[presentation-env] %s %s: %s" % (
        a.game, a.tier, "environment equals the manifest header" if not diff
        else "%d surface field(s) differ from the manifest header" % len(diff)))
    return 1 if diff and a.strict else 0


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


def promote_select(cand, all_tiles, filters, excludes=()):
    """Candidate tiles a promote considers: every golden tile under
    --all, else those whose basename contains a filter; a tile whose
    basename contains an --exclude substring is left out either way."""
    tiles = list_tiles(cand)
    if not all_tiles:
        tiles = [t for t in tiles if any(f and f in t for f in filters)]
    return [t for t in tiles if not any(x and x in t for x in excludes)]


def promote_manifest(a, rec):
    """The manifest a promote rewrites: the tier's, or under --create an
    empty one with a fresh header when the file is absent. None on a
    refusal (printed)."""
    if a.game != rec.get("game", a.game) or a.tier != rec.get("tier", a.tier):
        print("[presentation-promote] the candidate was captured for %s/%s, "
              "not %s/%s" % (rec.get("game"), rec.get("tier"), a.game, a.tier),
              file=sys.stderr)
        return None
    path = os.path.join(repo_root(), manifest_path(a.game, a.tier))
    if a.create and not os.path.isfile(path):
        return Manifest(build_header(a.game, a.tier, set(),
                                     rec.get("preset", "unrecorded"), {},
                                     None), [])
    try:
        return load_manifest(a.game, a.tier)
    except (OSError, ValueError) as e:
        print("[presentation-promote] %s" % e, file=sys.stderr)
        return None


def foreign_host(rec):
    """Why a capture record was not captured on this machine, or None. A
    record without a host (written before records named one, or captured
    where the host was not passed in) counts as local; a record naming a
    host is refused when that host differs from host_here(), or when this
    process cannot name its own host."""
    theirs = rec.get("host", "unrecorded")
    if theirs == "unrecorded":
        return None
    ours = host_here()
    if ours == theirs:
        return None
    if ours == "unrecorded":
        return ("the candidate was captured on host %s and this process "
                "cannot name its own host (inside the capture image, pass "
                "OPENPETE_CAPTURE_HOST); promote from a capture taken on "
                "this machine" % theirs)
    return ("the candidate was captured on host %s, not on this host %s; "
            "re-capture the rows here (--filter / --freecam-filter) and "
            "promote from that capture" % (theirs, ours))


def cmd_promote(a):
    """Rewrite the manifest rows of promoted candidate tiles (digest,
    blessed revision, recomp identity, origin) and file the tiles into the
    store. A tile whose digest equals its row's is unchanged and keeps its
    row. The header keeps its fields, its size list extended by any new
    size and its environment fields set from the capture's. A capture
    whose comparison surface departs from the header's, or a promote run
    outside the capture image, is refused unless --environment-accept
    names the reason, which the header records."""
    root = repo_root()
    rec = read_record(a.candidate)
    foreign = foreign_host(rec)
    if foreign:
        print("[presentation-promote] REFUSED: %s" % foreign, file=sys.stderr)
        return 1
    m = promote_manifest(a, rec)
    if m is None:
        return 1
    env = capture_environment(a.candidate)
    if environment_refused("presentation-promote",
                           environment_problems(m.header, env),
                           a.environment_accept):
        return 1
    rows = {r["tile"]: r for r in m.rows}
    sel = promote_select(a.candidate, a.all, a.filters, a.exclude or ())
    paths = [os.path.join(a.candidate, t) for t in sel]
    digs = digest_files(paths)
    bad = [p for p in paths if digs[p] is None]
    if bad:
        print("[presentation-promote] undecodable: %s" % bad[0],
              file=sys.stderr)
        return 1
    changed = [(t, p) for t, p in zip(sel, paths)
               if t not in rows or rows[t]["digest"] != digs[p][0]]
    header_moves = (env_header_changed(m.header, env) or
                    (a.environment_accept and
                     m.header.get("environment-accept") != " ".join(
                         a.environment_accept.split())))
    if not changed and not header_moves:
        print("[presentation-promote] nothing to promote (no matching / "
              "changed tiles)")
        return 0
    rev = None
    if changed:
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
    m.header = apply_environment(m.header, env, a.environment_accept)
    m.rows = list(rows.values())
    out = os.path.join(root, manifest_path(a.game, a.tier))
    with open(out + ".tmp", "w") as fh:
        fh.write(format_manifest(m))
    os.replace(out + ".tmp", out)
    print("[presentation-promote] %s: %d row(s) rewritten%s, header "
          "environment %s; tiles filed into %s"
          % (manifest_path(a.game, a.tier), len(changed),
             ", blessed revision %s" % rev[:12] if rev else "",
             "rewritten" if header_moves else "unchanged", store_dir()))
    return 0


def recomp_input_paths(game):
    """Tracked paths the regen of a game reads: its recomp.conf and
    manifest directory, the decomp submodule (asm, overlays.ld, sources),
    the regen scripts, the tools they run and the recompiler's sources."""
    return ["games/%s/recomp.conf" % game, "games/%s/manifest" % game,
            "games/%s/%s" % (game, game), "tools", "third_party/psxrecomp",
            "scripts/regen-recomp.sh", "scripts/regen-manifest.sh",
            "scripts/emit-recomp-manifest.sh"]


def cmd_identity(a):
    """Print a game's recomp identity pair as `recomp-input=` and
    `generated-tree=` lines."""
    print("recomp-input=%s" % recomp_input_digest(a.game))
    print("generated-tree=%s" % generated_tree_digest(a.game))
    return 0


def cmd_rebless_identity(a):
    """Rewrite the recomp_input and generated_tree columns of every row of
    a game tier's manifest from this checkout's recomp identity; digests,
    revisions, origins and the header are kept. Refused outside the capture
    image whose recipe hash the header records (environment_refused), and
    when `git status` reports a change under recomp_input_paths, since the
    identity would then describe sources no revision holds."""
    who = "presentation-rebless"
    try:
        m = load_manifest(a.game, a.tier)
    except (OSError, ValueError) as e:
        print("[%s] %s" % (who, e), file=sys.stderr)
        return 1
    here = recipe_hash_here()
    problems = []
    if here == OUTSIDE_IMAGE:
        problems.append(("blessing-process", "inside the capture image",
                         here))
    elif here != m.header.get("recipe-hash", "unrecorded"):
        problems.append(("recipe-hash", m.header.get("recipe-hash",
                                                     "unrecorded"), here))
    if environment_refused(who, problems, None, hint=(
            "  Run it inside the image: scripts/golden-capture-container.sh "
            "run -- python3 scripts/presentation-blessed.py rebless-identity "
            "%s --tier %s" % (a.game, a.tier))):
        return 1
    dirty = git("status", "--porcelain", "--", *recomp_input_paths(a.game))
    if dirty.strip():
        print("[%s] REFUSED: the working tree differs from HEAD over the "
              "recomp inputs; commit or revert, rerun the regen, then "
              "rebless:" % who, file=sys.stderr)
        for ln in dirty.splitlines()[:20]:
            print("    " + ln, file=sys.stderr)
        return 1
    rin = recomp_input_digest(a.game)
    gen = generated_tree_digest(a.game)
    moved = 0
    for r in m.rows:
        if (r["recomp_input"], r["generated_tree"]) != (rin, gen):
            moved += 1
        r["recomp_input"], r["generated_tree"] = rin, gen
    head = git("rev-parse", "HEAD").strip()
    for rev in sorted({r["revision"] for r in m.rows}):
        diff = git("diff", "--name-only", rev, head, "--",
                   *recomp_input_paths(a.game), check=False)
        if diff is None:
            print("[%s] note: revision %s does not resolve here; its rows "
                  "take HEAD's identity" % (who, rev[:12]))
        elif diff.strip():
            print("[%s] note: revision %s differs from HEAD over the recomp "
                  "inputs (%s); its rows take HEAD's identity"
                  % (who, rev[:12], diff.split()[0]))
    out = os.path.join(repo_root(), manifest_path(a.game, a.tier))
    with open(out + ".tmp", "w") as fh:
        fh.write(format_manifest(m))
    os.replace(out + ".tmp", out)
    print("[%s] %s: %d row(s), %d rewritten; recomp-input %s generated-tree "
          "%s" % (who, manifest_path(a.game, a.tier), len(m.rows), moved,
                  rin[:12], gen[:12]))
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
    p.add_argument("--fields", metavar="FILE",
                   help="take head, source-tree, recomp-input and "
                   "generated-tree from FILE (`key: value` lines, as "
                   "capture-fields writes) and run no git")
    p.add_argument("--build-dir", default="build/linux-release",
                   help="the capture binary's build directory; its basename "
                   "is the preset recorded")
    p = sub.add_parser("capture-fields")
    p.add_argument("game", choices=GAMES)
    p.add_argument("--out", metavar="FILE", help="write FILE instead of "
                   "printing")
    p = sub.add_parser("environment")
    game_tier(p)
    p.add_argument("--candidate", help="a captured candidate directory")
    p.add_argument("--build-dir", default="build/linux-release")
    p.add_argument("--strict", action="store_true",
                   help="exit 1 when a surface field differs")
    p = sub.add_parser("census")
    game_tier(p)
    p.add_argument("--candidate")
    p = sub.add_parser("promote")
    game_tier(p)
    p.add_argument("candidate")
    p.add_argument("filters", nargs="*", help="tile basename substrings")
    p.add_argument("--all", action="store_true")
    p.add_argument("--exclude", action="append", metavar="SUBSTR",
                   help="leave out tiles whose basename contains SUBSTR")
    p.add_argument("--create", action="store_true",
                   help="write the manifest when it does not exist")
    p.add_argument("--environment-accept", metavar="REASON")
    p = sub.add_parser("golden-at")
    p.add_argument("rev")
    p.add_argument("game", nargs="?", choices=GAMES)
    p.add_argument("filter", nargs="?", help="tile basename substring")
    p.add_argument("--out")
    p.add_argument("--materialize", action="store_true")
    p = sub.add_parser("identity")
    p.add_argument("game", choices=GAMES)
    p = sub.add_parser("rebless-identity")
    game_tier(p)
    a = ap.parse_args(argv)
    if a.cmd in ("read", "write", "verify", "capture-record", "census",
                 "promote", "environment", "rebless-identity") \
            and a.game not in TIERS[a.tier]:
        ap.error("tier %s has no game %s" % (a.tier, a.game))
    try:
        return {"digest": cmd_digest, "read": cmd_read, "write": cmd_write,
                "verify": cmd_verify, "store-file": cmd_store_file,
                "store-read": cmd_store_read, "gc": cmd_gc,
                "golden-at": cmd_golden_at,
                "capture-record": cmd_capture_record,
                "capture-fields": cmd_capture_fields,
                "census": cmd_census, "promote": cmd_promote,
                "environment": cmd_environment, "identity": cmd_identity,
                "rebless-identity": cmd_rebless_identity}[a.cmd](a)
    except IdentityError as e:
        print("[presentation-blessed] recomp identity: %s" % e,
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
