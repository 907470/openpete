#!/usr/bin/env python3
"""Fence: the documentation a package ships holds only allowlisted images.

A package ships no data derived from a game: before a game's disc is added,
the launcher draws only shipped drawing, and every picture of a game comes
from the player's disc at run time. The User docs therefore carry only
screenshots of the launcher before a disc is added, and every image file
under a package's docs/ is one of the paths packaging/docs-images.txt names.
A game capture added to the docs, or copied in by a theme or a plugin, fails
the package build instead of shipping.

An image is a file whose extension is an image type (IMAGE_SUFFIXES), or
whose leading bytes are an image signature (SIGNATURES), so a renamed image
is found too. The allowlist holds one path per line relative to the docs
root, '#' starting a comment. An allowlisted path that the tree does not hold
also fails, so a removed screenshot takes its line with it and a later file
at that path is not admitted unread.

Wiring: scripts/package-dist.sh runs `check` on the staged docs/ of every
package; the ctest docs_image_allowlist runs it on the docs target's site.

Run: python3 scripts/fences/docs_image_allowlist.py check <docs dir> [--allowlist FILE]
Exit: 0 clean, 1 refused, 64 usage or a missing docs dir.
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALLOWLIST = os.path.join(ROOT, "packaging", "docs-images.txt")

IMAGE_SUFFIXES = (".png", ".apng", ".jpg", ".jpeg", ".jpe", ".jfif", ".gif", ".webp", ".avif",
                  ".heic", ".heif", ".jxl", ".bmp", ".ico", ".cur", ".tif", ".tiff", ".tga",
                  ".svg", ".svgz", ".psd", ".exr", ".hdr", ".dds", ".ktx", ".ktx2", ".qoi")

# (offset, bytes) pairs that start an image file; RIFF....WEBP is matched in is_image.
SIGNATURES = (
    (0, b"\x89PNG\r\n\x1a\n"),
    (0, b"\xff\xd8\xff"),          # JPEG
    (0, b"GIF87a"),
    (0, b"GIF89a"),
    (0, b"\x00\x00\x01\x00"),      # ICO
    (0, b"II*\x00"),               # TIFF, little-endian
    (0, b"MM\x00*"),               # TIFF, big-endian
    (0, b"\xff\x0a"),              # JPEG XL codestream
    (0, b"\x00\x00\x00\x0cJXL "),  # JPEG XL container
    (0, b"qoif"),
    (0, b"8BPS"),                  # Photoshop
    (4, b"ftypavif"),
    (4, b"ftypheic"),
    (4, b"ftypmif1"),
)


def is_image(path):
    """True when the file is an image by its extension or its leading bytes.

    @param path The file.
    @return True for an image."""
    if path.lower().endswith(IMAGE_SUFFIXES):
        return True
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError:
        return False
    if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        return True
    return any(head[off:off + len(sig)] == sig for off, sig in SIGNATURES)


def read_allowlist(path):
    """The allowlisted paths, '/'-separated and relative to the docs root.

    @param path The allowlist file.
    @return The set of paths."""
    out = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split("#", 1)[0].strip()
            p = p.replace("\\", "/")
            while p.startswith("./"):
                p = p[2:]
            if p:
                out.add(p)
    return out


def images_under(root):
    """Every image file under a directory, as '/'-separated paths relative to it.

    @param root The docs root.
    @return The sorted paths."""
    found = []
    for d, _, files in os.walk(root):
        for name in files:
            full = os.path.join(d, name)
            if is_image(full):
                found.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(found)


def refusals(root, allowed):
    """The refusal lines for a docs tree: each image not allowlisted, each allowlisted path absent.

    @param root The docs root.
    @param allowed The allowlisted paths.
    @return The lines; empty when the tree is clean."""
    found = images_under(root)
    out = [f"image not on the allowlist: {p}" for p in found if p not in allowed]
    present = set(found)
    out += [f"allowlisted image missing from the docs: {p}" for p in sorted(allowed - present)]
    return out


def main(argv):
    """Run the check named on the command line.

    @param argv The arguments after the program name.
    @return The exit code."""
    ap = argparse.ArgumentParser(description="Refuse an image under a docs tree not on the allowlist.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("docs")
    c.add_argument("--allowlist", default=ALLOWLIST)
    a = ap.parse_args(argv)
    if not os.path.isdir(a.docs):
        print(f"docs-image-allowlist: {a.docs} is not a directory", file=sys.stderr)
        return 64
    bad = refusals(a.docs, read_allowlist(a.allowlist))
    for line in bad:
        print(f"docs-image-allowlist: REFUSED: {line}", file=sys.stderr)
    if bad:
        print(f"docs-image-allowlist: the package docs ship only images listed in {a.allowlist}; "
              "a User docs screenshot shows the launcher before a disc is added", file=sys.stderr)
        return 1
    print(f"docs-image-allowlist: {len(images_under(a.docs))} images, all allowlisted")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
