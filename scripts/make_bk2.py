#!/usr/bin/env python3
"""Synthesize a BizHawk .bk2 input movie for openpete's --bk2 replay.

The movie is a zip of Header.txt and Input Log.txt in the Octoshock 2.9.1
shape core/record/bk2_movie.c parses: one line per VBL, segment 1 carrying
four analog axes (128 = centre) then seventeen digital columns in the order
Up Down Left Right Select Start Square Triangle Circle Cross L1 R1 L2 R2 L3
R3 MODE, '.' released and any other character pressed.

  make_bk2.py OUT.bk2 --frames 2400 \
      --hold 1260:2400:Up --hold 1400:1440:Cross

A --hold span is [start, end) in movie frames, its button named as in the
column list above (case-insensitive). --axes sets the left stick over a span.
--feed-key vbl marks the movie as one row per VBL (Header.txt
"OpenpeteFeedKey vbl"); without it a row is one yield (openpete_frame). Both
are the keys before the consume key a recording writes; each game converts
either on load, and tools/bk2_rekey.py rewrites them to the consume key.
"""
import argparse, sys, zipfile

BUTTONS = ["Up","Down","Left","Right","Select","Start","Square","Triangle",
           "Circle","Cross","L1","R1","L2","R2","L3","R3","MODE"]
IDX = {b.lower(): i for i, b in enumerate(BUTTONS)}

HEADER = ("MovieVersion BizHawk v2.0\n"
          "Author openpete\n"
          "emuVersion openpete-bk2-record\n"
          "Platform PSX\n"
          "Core Octoshock\n"
          "rerecordCount 0\n"
          "LoadDelay {delay}\n")

LOGKEY = ("LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|"
          "P1 RStick X|P1 RStick Y|"
          + "|".join("P1 " + b for b in BUTTONS) + "|")


def span(text):
    a, b, rest = text.split(":", 2)
    return int(a), int(b), rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--load-delay", type=int, default=66)
    ap.add_argument("--feed-key", choices=["frame", "vbl"], default="frame",
                    help="the counter rows are indexed by (vbl for Spyro 2 and 3)")
    ap.add_argument("--hold", action="append", default=[],
                    help="START:END:Button[,Button...]")
    ap.add_argument("--axes", action="append", default=[],
                    help="START:END:LX,LY")
    args = ap.parse_args()

    n = args.frames
    digital = [[False] * len(BUTTONS) for _ in range(n)]
    axes = [[128, 128, 128, 128] for _ in range(n)]

    for h in args.hold:
        s, e, names = span(h)
        for name in names.split(","):
            i = IDX.get(name.strip().lower())
            if i is None:
                sys.exit("unknown button: " + name)
            for f in range(max(0, s), min(n, e)):
                digital[f][i] = True
    for a in args.axes:
        s, e, vals = span(a)
        lx, ly = (int(v) for v in vals.split(","))
        for f in range(max(0, s), min(n, e)):
            axes[f][0], axes[f][1] = lx, ly

    lines = ["[Input]", LOGKEY]
    for f in range(n):
        tail = "".join("X" if d else "." for d in digital[f])
        a = axes[f]
        lines.append("|    1,...|%5d,%5d,%5d,%5d,%s|" % (a[0], a[1], a[2], a[3], tail))
    lines.append("[/Input]")

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        header = HEADER.format(delay=args.load_delay)
        if args.feed_key == "vbl":
            header += "OpenpeteFeedKey vbl\n"
        z.writestr("Header.txt", header)
        z.writestr("Input Log.txt", "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
