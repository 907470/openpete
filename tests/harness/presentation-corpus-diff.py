#!/usr/bin/env python3
"""presentation-corpus-diff.py - compare a candidate corpus to the blessed manifest.

A golden tile's verdict is one row of the tier's blessed.tsv (tile, verdict
digest, blessed revision, recomp identity, origin); the reference images are not
in the tree. The verdict digest is the SHA-256 of the decoded RGB payload with
its mode and size, computed by scripts/presentation-blessed.py, which this
script imports so the gate and the promote share one digest. This gate digests
each freshly captured candidate tile and compares it against its row:

  - MISSING / EXTRA are failures (the corpus shape must match): a row with no
    captured tile, a captured tile with no row. Under --expect-subset a row the
    candidate does not hold is counted and not a failure, for a run scoped to
    part of the corpus by a capture filter; an EXTRA tile still fails, since a
    captured tile with no row is a shape change however the run was scoped.
  - A digest that differs from the row's is a divergence; the exit code is
    nonzero if ANY tile diverges. For a presentation-NEUTRAL change that is a
    defect to fix; for an intended visual change it is the adjudication
    worklist (compare against the PSX -og reference, then promote with
    presentation-corpus-promote.sh).

A passing run reads no reference image. For a divergent row only, the reference
image is resolved from the local store (addressed by the row's digest) or the
archive named by golden-ref.state, and the row is reported as today's pixel
diff reported it: the differing pixel count and the worst channel delta, with
a heat-diff PNG (8x-amplified abs difference) written to --diff-out, or, when
the two images differ in size, as a size change. A divergent row whose
reference neither holds is reported with both digests and the golden-at command
that resolves it.

--game selects the manifest (tests/golden/<game>/presentation/blessed.tsv);
--manifest names another (the dragon tier's).

Exit: 0 = every compared tile matches its row, 1 = divergence / missing / extra
      / a manifest absent, unparsable or with zero rows / no candidate tile
      names a row, 64 = SKIP (PIL missing / candidate dir absent).
"""
import argparse
import importlib.util
import os
import sys

try:
    from PIL import Image, ImageChops
except ModuleNotFoundError:
    print("[presentation-diff] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "presentation_blessed",
    os.path.join(HERE, "..", "..", "scripts", "presentation-blessed.py"))
pb = importlib.util.module_from_spec(_spec)
# Registered under its name so the helper's process pool can pickle its
# functions by reference.
sys.modules[_spec.name] = pb
_spec.loader.exec_module(pb)


def diff_images(ref_src, cand_path, heat_out):
    """Pixel diff of a reference against a candidate tile, both decoded to
    RGB. Returns ("size", ref_size, cand_size) or ("pixels", n, max channel
    delta, bbox), bbox the (left, upper, right, lower) box of the differing
    pixels or None when none differ; writes the heat PNG when pixels
    differ."""
    with Image.open(ref_src) as a_img, Image.open(cand_path) as b_img:
        A = a_img.convert("RGB")
        B = b_img.convert("RGB")
    if A.size != B.size:
        return ("size", A.size, B.size)
    d = ImageChops.difference(A, B)
    r, g, b = d.split()
    peak = ImageChops.lighter(ImageChops.lighter(r, g), b)
    hist = peak.histogram()
    n = A.size[0] * A.size[1] - hist[0]
    mx = max((v for v in range(256) if hist[v]), default=0)
    if heat_out and n:
        os.makedirs(os.path.dirname(heat_out), exist_ok=True)
        d.point(lambda v: min(255, v * 8)).save(heat_out)
    return ("pixels", n, mx, d.getbbox())


def divergent_line(name, row, cand_path, cand_dig, tdir, arch, diff_out,
                   game, size_set):
    """Return the DIVERGE line of one divergent row, resolving its
    reference image only here; writes the heat PNG when pixels differ."""
    src, where = pb.resolve_reference(row, tdir, arch)
    cand_size = cand_dig[1]
    if src is None:
        size_note = ""
        if size_set and "%dx%d" % cand_size not in size_set:
            size_note = " size=%dx%d not in the manifest sizes (%s)" % (
                cand_size[0], cand_size[1], ",".join(sorted(size_set)))
        return (f"DIVERGE  {name}  digest={row['digest'][:12]}->"
                f"{cand_dig[0][:12]} reference=unavailable"
                f"{size_note} (resolve: scripts/presentation-blessed.py golden-at "
                f"{row['revision'][:12]} {game} {name})")
    heat = os.path.join(diff_out, name.replace("-vk.png", "-diff.png"))
    res = diff_images(src, cand_path, heat)
    if res[0] == "size":
        (aw, ah), (bw, bh) = res[1], res[2]
        return f"DIVERGE  {name}  size={aw}x{ah}->{bw}x{bh} reference={where}"
    return f"DIVERGE  {name}  pixels={res[1]} maxch={res[2]} reference={where}"


def classify_tiles(rows, cand_dir, names):
    """Classify candidate tiles against manifest rows.

    Returns one (name, verb, row digest, candidate digest) per name, in
    order: SAME when the tile's decoded verdict digest equals its row's,
    DIVERGE when it differs or the tile does not decode (candidate digest
    None), EXTRA when no row names the tile (row digest None). Candidate
    digests are pb.digest_files results, (hex, (width, height)) or None.
    """
    names = list(names)
    digs = pb.digest_files(os.path.join(cand_dir, n) for n in names)
    out = []
    for n in names:
        got = digs[os.path.join(cand_dir, n)]
        row = rows.get(n)
        if row is None:
            out.append((n, "EXTRA", None, got))
        elif got is not None and got[0] == row["digest"]:
            out.append((n, "SAME", row["digest"], got))
        else:
            out.append((n, "DIVERGE", row["digest"], got))
    return out


def manifest_tile_dir(game, manifest):
    """The directory a manifest's tiles were tracked in before the store
    held them; the native-tick manifest sits apart from its tiles."""
    rel = os.path.relpath(os.path.abspath(manifest), pb.repo_root())
    if rel == pb.NATIVE_TICK_MANIFEST:
        return pb.tier_dir(game, "native-vk")
    return os.path.dirname(rel)


def load_rows(path):
    """Rows and header of a manifest, or (None, message)."""
    try:
        with open(path) as fh:
            m = pb.parse_manifest(fh.read(), path)
    except OSError as e:
        return None, f"cannot read the manifest {path}: {e.strerror}"
    except ValueError as e:
        return None, f"unparsable manifest: {e}"
    if not m.rows:
        return None, f"VACUOUS - {path} holds zero rows"
    return m, None


def main():
    """Compare a candidate directory's tiles against the manifest rows."""
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", help="directory of freshly captured -vk PNGs")
    ap.add_argument("--game", default="spyro-1", choices=pb.GAMES,
                    help="game slug; selects the manifest")
    ap.add_argument("--manifest", default=None,
                    help="blessed.tsv (default: tests/golden/<game>/presentation/blessed.tsv)")
    ap.add_argument("--diff-out", default=None, help="dir for heat-diff PNGs (default: <candidate>/diff)")
    ap.add_argument("--expect-subset", action="store_true",
                    help="the candidate covers part of the corpus: rows it "
                         "does not hold are counted, not failed")
    args = ap.parse_args()
    if args.manifest is None:
        args.manifest = pb.manifest_path(args.game, "presentation")

    if not os.path.isdir(args.candidate):
        print(f"[presentation-diff] SKIP: no candidate dir {args.candidate}")
        return 64
    m, err = load_rows(args.manifest)
    if m is None:
        print(f"[presentation-diff] FAIL: {err}")
        return 1
    rows = {r["tile"]: r for r in m.rows}
    tdir = manifest_tile_dir(args.game, args.manifest)
    size_set = {s for s in m.header.get("size", "").split(",") if "x" in s}

    cand = pb.list_tiles(args.candidate)
    gset, cset = set(rows), set(cand)
    diff_out = args.diff_out or os.path.join(args.candidate, "diff")
    fails = 0

    missing = sorted(gset - cset)
    extra = sorted(cset - gset)
    if args.expect_subset:
        print(f"SUBSET   {len(missing)} golden tile(s) outside the captured scope, not compared")
    else:
        for name in missing:
            print(f"MISSING  {name}  (in golden, not captured)"); fails += 1
    for e in extra:
        print(f"EXTRA    {e}  (captured, not in golden)"); fails += 1

    common = sorted(gset & cset)
    if not common:
        print("[presentation-diff] FAIL: VACUOUS - no candidate tile has a golden twin")
        return 1

    divergent = []
    arch = None
    for name, verb, _want, got in classify_tiles(rows, args.candidate, common):
        if verb == "SAME":
            print(f"ok       {name}")
            continue
        divergent.append(name); fails += 1
        if got is None:
            print(f"DIVERGE  {name}  undecodable candidate tile")
            continue
        if arch is None:
            arch = pb.Archive()
        print(divergent_line(name, rows[name], os.path.join(args.candidate, name), got,
                             tdir, arch, diff_out, args.game, size_set))
    if arch is not None:
        arch.close()

    miss_label = "outside scope" if args.expect_subset else "missing"
    print(f"[presentation-diff] {args.game}: {len(common)} compared, {len(divergent)} divergent, "
          f"{len(missing)} {miss_label}, {len(extra)} extra")
    if divergent:
        print(f"[presentation-diff] heat diffs in {diff_out}/ - adjudicate reference-first, "
              f"then promote with tests/harness/presentation-corpus-promote.sh")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
