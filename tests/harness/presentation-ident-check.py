#!/usr/bin/env python3
"""presentation-ident-check.py - the sub-tick spine's identity check.

The corpus captures an `ident` leg: the intermediate re-extract phased onto the
newest tick's stamp (OPENPETE_INTERMEDIATE_ALPHA=1) on every canonical frame, the
tick the canonical dump composes. A complete
spine reproduces the canonical dump of that frame pixel for pixel, so each
`<row>__<lod>ident__<f>-vk.png` is diffed against `<row>__<lod>canon__<f>-vk.png`
from the SAME candidate directory. No golden image is involved.

The target is zero. Until every producer reaches it, a per-tile baseline
(tests/golden/<game>/presentation/ident-baseline.tsv: tile, differing pixels)
holds the current residual; a tile fails when it exceeds its baseline, and a
tile without a baseline row fails on any difference. --bless rewrites the
baseline from the candidate (review the diff before committing it: a raised
count is a spine regression, not a re-bless). The rewrite merges: the
candidate's pairs replace their rows, rows the candidate does not hold stay.

The baseline's counts come from the same capture as the verdict digests, so
it carries the same `# key: value` environment header as blessed.tsv and a
bless is refused under the same rule (scripts/presentation-blessed.py,
Environment): a capture whose comparison surface differs from the header's,
or a bless run outside the capture image, is refused unless
--environment-accept "<reason>" admits it and the header records the reason.

Exit: 0 = every pair within baseline, 1 = a pair exceeds it / an ident tile has
no canon twin / no pair at all (a capture that produced no identity pair is
vacuous, not clean), 64 = SKIP (PIL missing / candidate dir absent).
"""
import argparse, importlib.util, os, sys
try:
    from PIL import Image, ImageChops
except ImportError:
    print("[presentation-ident] SKIP: python3-PIL missing", file=sys.stderr)
    sys.exit(64)

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "presentation_blessed",
    os.path.join(HERE, "..", "..", "scripts", "presentation-blessed.py"))
pb = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = pb
_spec.loader.exec_module(pb)

COLUMN_LINE = "# tile\tdiffering pixels vs the canon tile of the same frame (target 0)"

GAMES = ("spyro-1", "spyro-2", "spyro-3")


def baseline_path(game):
    return f"tests/golden/{game}/presentation/ident-baseline.tsv"


def diff_count(a, b):
    A = Image.open(a).convert("RGB")
    B = Image.open(b).convert("RGB")
    if A.size != B.size:
        return A.size[0] * A.size[1] + B.size[0] * B.size[1]
    d = ImageChops.difference(A, B).convert("L").point(lambda v: 255 if v else 0)
    return d.histogram()[255]


def classify_pair(cand_dir, name, base):
    """Classify one identity tile against the canon tile of its frame.

    Returns (verb, differing pixels, allowed): NOCANON (pixels None) when the
    candidate holds no canon twin, EXCEED when the count is over the tile's
    baseline row (0 without one), `within` when it is under it and nonzero,
    `ident` at zero.
    """
    canon = name.replace("__ident__", "__canon__")
    cpath = os.path.join(cand_dir, canon)
    allowed = base.get(name, 0)
    if not os.path.isfile(cpath):
        return "NOCANON", None, allowed
    n = diff_count(cpath, os.path.join(cand_dir, name))
    if n > allowed:
        return "EXCEED", n, allowed
    return ("within" if n else "ident"), n, allowed


def read_baseline(path):
    base = {}
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                name, n = line.split("\t")
                base[name] = int(n)
    return base


def read_header(path):
    """The baseline's `# key: value` header lines as a dict, in order;
    {} when the file is absent or carries none."""
    header = {}
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                if not line.startswith("#"):
                    break
                key, sep, val = line[1:].strip().partition(":")
                if sep and "\t" not in key and key.replace("-", "").isalnum():
                    header[key] = val.strip()
    return header


def write_baseline(path, header, rows):
    """Write the header lines, the column line and one row per tile."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w") as f:
        f.write("# presentation identity-leg baseline\n")
        for k, v in header.items():
            f.write(f"# {k}: {v}\n")
        f.write(COLUMN_LINE + "\n")
        for name in sorted(rows):
            f.write(f"{name}\t{rows[name]}\n")
    os.replace(path + ".tmp", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", help="directory holding both the canon and ident -vk PNGs")
    ap.add_argument("--game", default="spyro-1", choices=GAMES,
                    help="game slug; selects the baseline file")
    ap.add_argument("--baseline", default=None,
                    help="baseline file (default: tests/golden/<game>/presentation/ident-baseline.tsv)")
    ap.add_argument("--bless", action="store_true", help="rewrite the baseline from this candidate")
    ap.add_argument("--environment-accept", metavar="REASON",
                    help="admit a bless outside the pinned capture environment; recorded in the header")
    args = ap.parse_args()
    if args.baseline is None:
        args.baseline = baseline_path(args.game)
    if not os.path.isdir(args.candidate):
        print(f"[presentation-ident] SKIP: no candidate dir {args.candidate}")
        return 64
    idents = sorted(f for f in os.listdir(args.candidate) if "__ident__" in f and f.endswith("-vk.png"))
    if not idents:
        print("[presentation-ident] FAIL: no __ident__ tiles captured (vacuous)")
        return 1
    base = read_baseline(args.baseline)
    fails = 0
    counts = {}
    total_px = 0
    for name in idents:
        verb, n, allowed = classify_pair(args.candidate, name, base)
        if verb == "NOCANON":
            canon = name.replace("__ident__", "__canon__")
            print(f"NOCANON  {name}  (no {canon} in the candidate)"); fails += 1
            continue
        counts[name] = n
        total_px += n
        if verb == "EXCEED":
            print(f"EXCEED   {name}  pixels={n} baseline={allowed}"); fails += 1
        elif verb == "within":
            print(f"within   {name}  pixels={n} baseline={allowed}")
        else:
            print(f"ident    {name}")
    exact = sum(1 for n in counts.values() if n == 0)
    print(f"[presentation-ident] {args.game}: {len(counts)} pairs, {exact} exact, "
          f"{len(counts) - exact} within baseline or exceeding, {fails} failing, "
          f"{total_px} differing pixels in total")
    if args.bless:
        header = read_header(args.baseline)
        env = pb.capture_environment(args.candidate)
        if pb.environment_refused("presentation-ident",
                                  pb.environment_problems(header, env),
                                  args.environment_accept):
            return 1
        rec = pb.read_record(args.candidate)
        header = pb.apply_environment(
            {"game": args.game, "tier": rec.get("tier", "presentation")},
            env, args.environment_accept or header.get("environment-accept"))
        # Merge: the candidate's pairs replace their rows and every other row
        # stays. A candidate captured with a row filter holds a few pairs, and
        # a whole-file rewrite from it would drop the rest of the corpus's
        # baseline.
        merged = dict(base)
        merged.update(counts)
        write_baseline(args.baseline, header, merged)
        print(f"[presentation-ident] baseline written: {args.baseline} "
              f"({len(counts)} rows from this candidate, {len(merged) - len(counts)} kept)")
        return 0
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
