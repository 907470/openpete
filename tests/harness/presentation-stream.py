#!/usr/bin/env python3
"""presentation-stream.py - append the verdict lines of finished tiles to a stream file.

The golden fleet's supervisor sets OPENPETE_STREAM_FILE for a unit's capture;
scripts/corpus-resume.sh calls this script when the last leg of a row
finishes (and once over the rows an attempt imports), so the coordinator reads
each row's verdict while the capture is still running. The classification is
the end verdict's own code: classify_tiles of presentation-corpus-diff.py for
golden tiles against blessed.tsv, classify_pair of presentation-ident-check.py
for `__ident__` tiles against ident-baseline.tsv.

One line per tile, appended with one write(2) on an O_APPEND descriptor so
rows finishing in parallel never interleave within a line:

    SAME    <tile> <blessed digest>                     <host epoch>
    DIVERGE <tile> <blessed digest>-><candidate digest> <host epoch>
    EXTRA   <tile> -><candidate digest>                 <host epoch>
    IDENT   <tile> pixels=<n>/<baseline>                <host epoch>
    EXCEED  <tile> pixels=<n>/<baseline>                <host epoch>

Digests are the first 16 hex digits of the verdict digest; an undecodable
candidate is `undecodable`. EXCEED also covers an identity tile with no canon
twin (`pixels=nocanon/<baseline>`), which the end verdict reports as
identity-nocanon. --tag appends one word (the import passes `imported`).

Exit: 0 = every line written, 1 = the manifest is unreadable or a tile
could not be read; no line is written then, which the coordinator's unit-end
check reports as a stream mismatch.
"""
import argparse
import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DIGEST_HEX = 16


def load(name, path):
    """Import a sibling script as a module under name."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def short(dig):
    """The stream spelling of a pb.digest_files result."""
    return dig[0][:DIGEST_HEX] if dig else "undecodable"


def golden_lines(diff, rows, cand, names):
    """(verb, tile, detail) of each golden tile."""
    out = []
    for name, verb, want, got in diff.classify_tiles(rows, cand, names):
        if verb == "SAME":
            out.append((verb, name, want[:DIGEST_HEX]))
        elif verb == "DIVERGE":
            out.append((verb, name, f"{want[:DIGEST_HEX]}->{short(got)}"))
        else:
            out.append((verb, name, f"->{short(got)}"))
    return out


def ident_lines(ident, cand, names, base):
    """(verb, tile, detail) of each identity tile."""
    out = []
    for name in names:
        verb, n, allowed = ident.classify_pair(cand, name, base)
        stream_verb = "EXCEED" if verb in ("EXCEED", "NOCANON") else "IDENT"
        out.append((stream_verb, name, f"pixels={'nocanon' if n is None else n}/{allowed}"))
    return out


def main():
    """Classify the named tiles and append one line each to the stream file."""
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", help="candidate directory holding the tiles")
    ap.add_argument("tiles", nargs="+", help="tile file names in the candidate directory")
    ap.add_argument("--game", default="spyro-1")
    ap.add_argument("--manifest", required=True, help="blessed.tsv")
    ap.add_argument("--baseline", required=True, help="ident-baseline.tsv")
    ap.add_argument("--stream", required=True, help="stream file (appended)")
    ap.add_argument("--tag", default="", help="one word appended to every line")
    args = ap.parse_args()
    diff = load("presentation_corpus_diff", os.path.join(HERE, "presentation-corpus-diff.py"))
    ident = load("presentation_ident_check", os.path.join(HERE, "presentation-ident-check.py"))
    # Each script imports presentation-blessed.py under one module name, the
    # later import replacing the earlier in sys.modules. pb.digest_files
    # pickles its worker by that name for a process pool (32 tiles or more),
    # so both scripts use the module sys.modules holds.
    ident.pb = diff.pb
    sys.modules["presentation_blessed"] = diff.pb
    m, err = diff.load_rows(args.manifest)
    if m is None:
        print(f"[stream] {err}", file=sys.stderr)
        return 1
    rows = {r["tile"]: r for r in m.rows}
    idents = [t for t in args.tiles if "__ident__" in t]
    golden = [t for t in args.tiles if "__ident__" not in t]
    try:
        lines = golden_lines(diff, rows, args.candidate, golden)
        lines += ident_lines(ident, args.candidate, idents, ident.read_baseline(args.baseline))
    except (OSError, ValueError) as e:
        print(f"[stream] {args.candidate}: {e}", file=sys.stderr)
        return 1
    tag = f" {args.tag}" if args.tag else ""
    fd = os.open(args.stream, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        for verb, name, detail in lines:
            os.write(fd, f"{verb} {name} {detail} {time.time():.3f}{tag}\n".encode())
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
