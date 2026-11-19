#!/usr/bin/env python3
"""Licence checks run by scripts/package-dist.sh on the staged release tree.

  dist_compliance.py scan PATH...
      Refuses when a shipped ELF or PE binary under PATH carries a marker of
      a copyleft component that must never ship: pcsx-redux's GTE
      (GPL-2.0-or-later) or a statically linked FFmpeg/x264 (GPL). Exit 1 on
      a hit, and on zero binaries scanned.

  dist_compliance.py licences --stage DIR --scope S [--scope S...]
                              [--toolchain-dir DIR]
      Checks NOTICE.md against packaging/licences/manifest.tsv and copies the
      text of every component the archive carries into DIR/licences/.
      Exit 1 when a NOTICE.md row has no manifest row, a manifest row has no
      NOTICE.md row, a licence file (LICENSE*, LICENCE*, COPYING*) at any
      depth under third_party/ is no row's text, a third_party/ directory is
      covered by no row's source, a row's source path does not exist, a
      packaging/licences/ text is named by no row, or an active row's text
      is missing or empty.

  dist_compliance.py docs --stage DIR
      Checks the documentation site staged at DIR/docs against NOTICE.md.
      Exit 1 when DIR/docs/index.html is missing, the site's search index
      names a language other than English, DIR/docs holds
      assets/javascripts/lunr/ (the search language files, which no package
      ships), or a path in a NOTICE.md row of the documentation-site section
      (a backquoted token starting "assets/", a glob relative to DIR/docs)
      matches no staged file.

Runs from the repository root.
"""

import argparse
import glob
import json
import mmap
import os
import re
import shutil
import subprocess
import sys

# Byte strings a clean openpete binary never contains. The bare words
# "x264" and "libavcodec" are excluded: the exe names them in --record's
# option and help text.
COPYLEFT_MARKERS = (
    b"_ZN4PCSX3GTE",        # pcsx-redux GTE (C++ namespace PCSX::GTE)
    b"x264 - core",         # x264's SEI user-data version banner
    b"Lavc",                # libavcodec ident (LIBAVCODEC_IDENT)
    b"Lavf",                # libavformat ident (LIBAVFORMAT_IDENT)
    b"libavcodec version",  # libavcodec's av_log version line
)

MANIFEST = "packaging/licences/manifest.tsv"
NOTICE = "NOTICE.md"
THIRD_PARTY = "third_party"
PACKAGING_TEXTS = "packaging/licences"
# A licence file's base name: LICENSE, LICENCE or COPYING, with an optional
# suffix after "." or "-" (LICENSE.txt, LICENSE-bsd.txt, COPYING.txt).
LICENCE_NAME = re.compile(r"^(LICEN[CS]E|COPYING)([.-][^/]*)?$")
# The NOTICE.md section listing the documentation site's components, the
# staged directory its search worker imports language files from, and the
# path form a row of that section names its files by.
DOCS_SECTION = "## Documentation site"
DOCS_LUNR = "assets/javascripts/lunr"
DOCS_PATH = re.compile(r"`(assets/[^`]+)`")


def fail(msg):
    """Print a refusal to stderr and exit 1."""
    print(f"[dist-compliance] REFUSED: {msg}", file=sys.stderr)
    sys.exit(1)


def is_binary(path):
    """True when the file starts with an ELF or a PE (MZ) magic."""
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return head == b"\x7fELF" or head[:2] == b"MZ"


def iter_files(paths):
    """Yield every regular non-symlink file under the given files and directories."""
    for p in paths:
        if os.path.isfile(p) and not os.path.islink(p):
            yield p
            continue
        for root, _dirs, files in os.walk(p):
            for name in files:
                fp = os.path.join(root, name)
                if os.path.isfile(fp) and not os.path.islink(fp):
                    yield fp


def cmd_scan(args):
    """Scan every ELF/PE under args.paths for COPYLEFT_MARKERS."""
    n_bin = 0
    n_bytes = 0
    hits = []
    for fp in iter_files(args.paths):
        if not is_binary(fp):
            continue
        size = os.path.getsize(fp)
        n_bin += 1
        n_bytes += size
        if size == 0:
            continue
        with open(fp, "rb") as f, \
                mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            for marker in COPYLEFT_MARKERS:
                if m.find(marker) >= 0:
                    hits.append((fp, marker.decode()))
    print(f"[dist-compliance] scanned {n_bin} ELF/PE binaries, {n_bytes} bytes")
    if n_bin == 0:
        fail("no ELF/PE binary under " + " ".join(args.paths))
    if hits:
        for fp, marker in hits:
            print(f"[dist-compliance] {fp}: contains '{marker}'", file=sys.stderr)
        fail(f"{len(hits)} copyleft marker hit(s); the binary links a"
             " component that must not ship")
    print("[dist-compliance] scan PASS: no copyleft marker")


def read_manifest():
    """Return manifest rows as (component, scope, text, shipped, source) tuples."""
    rows = []
    with open(MANIFEST, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) != 5 or not all(cols):
                fail(f"{MANIFEST}:{n}: expected 5 non-empty tab-separated columns")
            rows.append(tuple(cols))
    return rows


def gitignored_dirs():
    """Return the third_party/ directories .gitignore names, without the slash."""
    out = set()
    try:
        with open(".gitignore", encoding="utf-8") as f:
            for line in f:
                line = line.strip().rstrip("/")
                if line.startswith(THIRD_PARTY + "/") and "*" not in line:
                    out.add(line)
    except OSError:
        pass
    return out


def third_party_files():
    """Return every file path under third_party/, submodules included.

    git lists tracked files, through checked-out submodules; where git
    cannot answer (a tree mounted without its git directory) the walk skips
    the directories .gitignore names.
    """
    try:
        r = subprocess.run(["git", "ls-files", "--recurse-submodules", "-z",
                            THIRD_PARTY], capture_output=True, check=True)
        files = [p for p in r.stdout.decode("utf-8").split("\0") if p]
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass
    skip = gitignored_dirs()
    files = []
    for root, dirs, names in os.walk(THIRD_PARTY):
        dirs[:] = [d for d in dirs if os.path.join(root, d) not in skip
                   and d != ".git"]
        files += [os.path.join(root, n) for n in names]
    return files


def third_party_dirs(files):
    """Return the top-level third_party/ entries holding a file, plus submodule directories."""
    out = {"/".join(p.split("/")[:2]) for p in files if p.count("/") >= 2}
    for name in os.listdir(THIRD_PARTY) if os.path.isdir(THIRD_PARTY) else []:
        path = f"{THIRD_PARTY}/{name}"
        if os.path.exists(os.path.join(path, ".git")):
            out.add(path)
    return out - gitignored_dirs()


def unshipped_sources(rows):
    """Return the source directories that only scope-none rows name.

    A component no package contains needs no text for the licence files of
    what it vendors; its row accounts for every file beneath its source.
    @param rows The manifest rows.
    @return The set of such directories, without a trailing slash.
    """
    none, shipped = set(), set()
    for _comp, scope, _text, _shipped, source in rows:
        if source == "-":
            continue
        for s in source.split(","):
            (none if scope == "none" else shipped).add(s.rstrip("/"))
    return {n for n in none
            if not any(s == n or s.startswith(n + "/") for s in shipped)}


def check_coverage(rows):
    """Refuse a licence file, third_party/ directory or packaging text no row accounts for."""
    texts = {r[2] for r in rows}
    sources = set()
    for comp, _scope, _text, _shipped, source in rows:
        if source == "-":
            continue
        for s in source.split(","):
            if not os.path.exists(s):
                fail(f"'{comp}' names source {s}, which does not exist")
            sources.add(s.rstrip("/"))
    files = third_party_files()
    unshipped = unshipped_sources(rows)
    orphan = sorted(p for p in files
                    if LICENCE_NAME.match(os.path.basename(p)) and p not in texts
                    and not any(p.startswith(u + "/") for u in unshipped))
    if orphan:
        fail("third_party licence files in no " + MANIFEST + " row: "
             + ", ".join(orphan))
    uncovered = sorted(d for d in third_party_dirs(files)
                       if not any(s == d or s.startswith(d + "/") for s in sources))
    if uncovered:
        fail("third_party directories no " + MANIFEST + " row's source covers: "
             + ", ".join(uncovered))
    stale = sorted(os.path.join(PACKAGING_TEXTS, n)
                   for n in os.listdir(PACKAGING_TEXTS)
                   if n.endswith(".txt") and os.path.join(PACKAGING_TEXTS, n) not in texts)
    if stale:
        fail(PACKAGING_TEXTS + " texts no " + MANIFEST + " row names: "
             + ", ".join(stale))


def notice_components():
    """Return the first cell of every NOTICE.md table body row."""
    out = []
    with open(NOTICE, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("|"):
                continue
            cell = line.split("|")[1].strip()
            if cell == "Component" or set(cell) <= {"-", ":"}:
                continue
            out.append(cell)
    return out


def cmd_licences(args):
    """Cross-check NOTICE.md with the manifest and stage the active texts."""
    rows = read_manifest()
    shipped_rows = [r for r in rows if r[1] != "none"]
    manifest_names = {r[0] for r in shipped_rows}
    notice = notice_components()
    missing = [c for c in notice if c not in manifest_names]
    if missing:
        fail("NOTICE.md rows with no licence text in " + MANIFEST + ": "
             + "; ".join(missing))
    stale = sorted(manifest_names - set(notice))
    if stale:
        fail(MANIFEST + " rows with no NOTICE.md row: " + "; ".join(stale))
    check_coverage(rows)

    dest = os.path.join(args.stage, "licences")
    os.makedirs(dest, exist_ok=True)
    scopes = set(args.scope)
    copied = 0
    for comp, scope, text, shipped, _source in shipped_rows:
        if scope not in scopes or text == "-" or text.startswith("lib:"):
            continue
        if scope.startswith("toolchain-"):
            if not args.toolchain_dir:
                fail(f"scope {scope} active with no --toolchain-dir")
            src = os.path.join(args.toolchain_dir, text)
        else:
            src = text
        if not os.path.isfile(src) or os.path.getsize(src) == 0:
            fail(f"'{comp}' ships with no licence text: {src} missing or empty")
        shutil.copyfile(src, os.path.join(dest, shipped))
        copied += 1
    if copied == 0:
        fail("no licence text staged")
    print(f"[dist-compliance] licences PASS: {copied} texts in {dest}"
          f" (scopes: {' '.join(sorted(scopes))})")


def docs_section_rows():
    """Return the table body rows of NOTICE.md's documentation-site section."""
    rows, inside = [], False
    with open(NOTICE, encoding="utf-8") as f:
        for line in f:
            if line.startswith("## "):
                inside = line.startswith(DOCS_SECTION)
                continue
            if not inside or not line.startswith("|"):
                continue
            cell = line.split("|")[1].strip()
            if cell != "Component" and not set(cell) <= {"-", ":"}:
                rows.append(line)
    return rows


def search_languages(docs):
    """Return the language list of the staged site's search index config."""
    path = os.path.join(docs, "search", "search_index.json")
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)["config"]["lang"]
    except (OSError, ValueError, KeyError, TypeError):
        fail(f"{path} missing or has no config.lang")
    return None


def cmd_docs(args):
    """Check the staged documentation site against NOTICE.md's docs rows."""
    docs = os.path.join(args.stage, "docs")
    if not os.path.isfile(os.path.join(docs, "index.html")):
        fail(f"{docs}/index.html missing: no documentation site staged")
    langs = search_languages(docs)
    if langs != ["en"]:
        fail(f"search languages {langs}: the worker would import files from"
             f" {DOCS_LUNR}/, which no package ships; set search lang: en")
    if os.path.lexists(os.path.join(docs, DOCS_LUNR)):
        fail(f"{docs}/{DOCS_LUNR} staged: its search language files ship in"
             " no package")
    rows = docs_section_rows()
    if not rows:
        fail(f"NOTICE.md has no '{DOCS_SECTION}' rows")
    absent = []
    n_paths = 0
    for row in rows:
        for rel in DOCS_PATH.findall(row):
            n_paths += 1
            hits = [p for p in glob.glob(os.path.join(docs, rel))
                    if os.path.isfile(p) or os.listdir(p)]
            if not hits:
                absent.append(f"{rel} ({row.split('|')[1].strip().split(' (')[0]})")
    if absent:
        fail("NOTICE.md lists a licence for docs files that do not ship: "
             + "; ".join(absent))
    print(f"[dist-compliance] docs PASS: {len(rows)} rows, {n_paths} paths"
          f" present, search {langs[0]}, no {DOCS_LUNR}/")


def main():
    """Parse the subcommand and run it."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("paths", nargs="+")
    s.set_defaults(fn=cmd_scan)
    lic = sub.add_parser("licences")
    lic.add_argument("--stage", required=True)
    lic.add_argument("--scope", action="append", required=True,
                     choices=["all", "windows", "sdk", "toolchain-windows",
                              "toolchain-linux", "appimage"])
    lic.add_argument("--toolchain-dir")
    lic.set_defaults(fn=cmd_licences)
    d = sub.add_parser("docs")
    d.add_argument("--stage", required=True)
    d.set_defaults(fn=cmd_docs)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
