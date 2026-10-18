#!/usr/bin/env python3
"""docs-shot-pairs.py - check the presentation rows that supply User docs images.

A presentation corpus row (games/<game>/corpus/presentation-corpus.manifest)
is a docs row when its launch field carries one or more tokens

    docs:<page>#<anchor>             an image for that page section
    docs:<page>#<anchor>/before      the setting's default side of a pair
    docs:<page>#<anchor>/after       the setting's changed side of a pair

<page> is a path under docs/src/user/ without `.md`; <anchor> is a heading
id of that page. The capture harness drops these tokens
(scripts/presentation-game.sh pg_split_launch), so a tag never changes a
capture. A docs row's images are its canonical original-LOD tiles at its
pinned VBLs, <name>__canon__<pin>-vk.png; a docs row pins at least one VBL.

A pair is the two rows sharing <page>#<anchor>, one /before and one /after,
with the same pins. The check fails when the two images of a pair at any pin
have the same verdict digest (scripts/presentation-blessed.py), so a setting
that stops changing the picture fails instead of publishing two identical
images.

Usage:
  docs-shot-pairs.py check [--game G] [--manifest M] [--blessed PATH | --dir CANDIDATE] [--expect-subset]
      Digests from the tier's blessed.tsv (default), or decoded from the
      PNGs in a capture directory. --manifest replaces the game's corpus
      manifest (the self-test's fixtures). --expect-subset skips a pair whose tiles
      are all absent from the capture directory (a filtered capture).
  docs-shot-pairs.py list [--game G]
      Print each docs row's image tiles with its page anchor and side.

Exit: 0 every pair differs; 1 an identical pair, a missing tile, or a
malformed tag; 65 the manifest has no docs rows; 64 usage.
"""

import argparse
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
_spec = importlib.util.spec_from_file_location(
    "presentation_blessed", os.path.join(ROOT, "scripts", "presentation-blessed.py"))
pb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pb)

TOKEN_RE = re.compile(r"^docs:([a-z0-9_/-]+)#([a-z0-9_-]+)(?:/(before|after))?$")
SIDES = ("before", "after")


def slugify(heading):
    """Heading id as the site's toc extension writes it: lower case, word
    characters, spaces and hyphens kept, runs of spaces and hyphens joined
    to one hyphen."""
    s = re.sub(r"[^\w\s-]", "", heading).strip().lower()
    return re.sub(r"[-\s]+", "-", s)


def page_anchors(path):
    """Heading ids of a Markdown page, outside fenced code."""
    out, fenced = set(), False
    with open(path) as fh:
        for line in fh:
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            m = None if fenced else re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
            if m:
                out.add(slugify(m.group(1)))
    return out


def manifest_rows(path):
    """Yield (line number, name, pins, launch tokens) per manifest row."""
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            f = [x.strip() for x in line.rstrip("\n").split("|")]
            f += [""] * (6 - len(f))
            pins = [int(p) for p in f[4].split(",") if p.strip()]
            yield n, f[0], pins, f[5].split()


def docs_rows(manifest, errors):
    """Collect docs tags. Returns ({key: {side: (name, pins)}}, singles),
    singles a list of (key, name, pins); defects are appended to errors."""
    pairs, singles = {}, []
    user = os.path.join(ROOT, "docs", "src", "user")
    anchors = {}
    for n, name, pins, toks in manifest_rows(manifest):
        for tok in toks:
            if not tok.startswith("docs:"):
                continue
            m = TOKEN_RE.match(tok)
            where = "%s:%d" % (manifest, n)
            if not m:
                errors.append("%s: malformed tag %r" % (where, tok))
                continue
            page, anchor, side = m.groups()
            md = os.path.join(user, page + ".md")
            if not os.path.isfile(md):
                errors.append("%s: %r names no page docs/src/user/%s.md" % (where, tok, page))
                continue
            if md not in anchors:
                anchors[md] = page_anchors(md)
            if anchor not in anchors[md]:
                errors.append("%s: %r: no heading #%s on %s.md" % (where, tok, anchor, page))
            if not pins:
                errors.append("%s: docs row %s pins no VBL" % (where, name))
                continue
            key = "%s#%s" % (page, anchor)
            if side is None:
                singles.append((key, name, pins))
                continue
            slot = pairs.setdefault(key, {})
            if side in slot:
                errors.append("%s: second /%s row for %s (%s and %s)"
                              % (where, side, key, slot[side][0], name))
                continue
            slot[side] = (name, pins)
    for key, slot in sorted(pairs.items()):
        for side in SIDES:
            if side not in slot:
                errors.append("pair %s has no /%s row" % (key, side))
        if all(s in slot for s in SIDES) and slot["before"][1] != slot["after"][1]:
            errors.append("pair %s: /before pins %s, /after pins %s"
                          % (key, slot["before"][1], slot["after"][1]))
    return pairs, singles


def tile(name, pin):
    """The docs image tile of a row at one pinned VBL."""
    return "%s__canon__%d-vk.png" % (name, pin)


def manifest_path(a):
    """The corpus manifest to read: --manifest, else the game's."""
    if getattr(a, "manifest", None):
        return a.manifest
    return os.path.join(ROOT, "games", a.game, "corpus", "presentation-corpus.manifest")


def cmd_list(a):
    """Print every docs tile with its anchor and side."""
    errors = []
    pairs, singles = docs_rows(manifest_path(a), errors)
    for e in errors:
        print("ERROR " + e)
    for key, slot in sorted(pairs.items()):
        for side in SIDES:
            if side in slot:
                name, pins = slot[side]
                for p in pins:
                    print("%s\t%s\t%s" % (key, side, tile(name, p)))
    for key, name, pins in singles:
        for p in pins:
            print("%s\t-\t%s" % (key, tile(name, p)))
    if errors:
        return 1
    return 0 if (pairs or singles) else 65


def cmd_check(a):
    """Compare each pair's images; see the module docstring for exits."""
    errors = []
    pairs, singles = docs_rows(manifest_path(a), errors)
    if not pairs and not singles and not errors:
        print("docs-shot-pairs: %s: no docs rows" % a.game)
        return 65
    if a.dir:
        def digest(t):
            r = pb.digest_file(os.path.join(a.dir, t))
            return r[0] if r else None
        source = a.dir
    else:
        path = a.blessed or os.path.join(ROOT, pb.manifest_path(a.game, "presentation"))
        with open(path) as fh:
            rows = pb.parse_manifest(fh.read(), path).rows
        table = {r["tile"]: r["digest"] for r in rows}
        digest = table.get
        source = os.path.relpath(path, ROOT)
    compared = differ = 0
    for key, slot in sorted(pairs.items()):
        if not all(s in slot for s in SIDES) or slot["before"][1] != slot["after"][1]:
            continue
        (bname, pins), (aname, _) = slot["before"], slot["after"]
        tiles = [(tile(bname, p), tile(aname, p)) for p in pins]
        if a.dir and a.expect_subset and not any(
                os.path.exists(os.path.join(a.dir, t)) for pr in tiles for t in pr):
            print("SKIP %s (not captured)" % key)
            continue
        for bt, at in tiles:
            bd, ad = digest(bt), digest(at)
            if bd is None or ad is None:
                errors.append("%s: MISSING %s" % (key, bt if bd is None else at))
                continue
            compared += 1
            if bd == ad:
                errors.append("%s: IDENTICAL %s %s (%s)" % (key, bt, at, bd[:12]))
            else:
                differ += 1
                print("DIFFER %s %s %s" % (key, bt, at))
    for key, name, pins in singles:
        for p in pins:
            if digest(tile(name, p)) is None and not (a.dir and a.expect_subset):
                errors.append("%s: MISSING %s" % (key, tile(name, p)))
    for e in errors:
        print("FAIL " + e)
    print("docs-shot-pairs: %s: %d pair image(s) compared, %d differ, %d single row(s), "
          "%d defect(s) [%s]" % (a.game, compared, differ, len(singles), len(errors), source))
    if errors:
        return 1
    if compared == 0 and pairs and not (a.dir and a.expect_subset):
        print("docs-shot-pairs: VACUOUS: no pair image compared")
        return 1
    return 0


def main():
    """Parse the command line and run the subcommand."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("check")
    c.add_argument("--game", default="spyro-1")
    c.add_argument("--manifest")
    g = c.add_mutually_exclusive_group()
    g.add_argument("--blessed")
    g.add_argument("--dir")
    c.add_argument("--expect-subset", action="store_true")
    li = sub.add_parser("list")
    li.add_argument("--game", default="spyro-1")
    li.add_argument("--manifest")
    a = ap.parse_args()
    if a.cmd == "check":
        return cmd_check(a)
    if a.cmd == "list":
        return cmd_list(a)
    ap.print_usage(sys.stderr)
    return 64


if __name__ == "__main__":
    sys.exit(main())
