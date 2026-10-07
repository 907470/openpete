#!/usr/bin/env python3
"""gen_docs_map.py - generate the User docs map docs/src/user/pages.toml.

The map sends each namespaced id to the guide anchor that documents it,
`page#anchor` with the page relative to docs/src/user/. It is built from
the id markers on the User pages and nothing else; no page path lives in
engine code.

Markers
-------
A marker follows a literal value on a page (a default, a key, a flag
spelling) that the surface dump will generate:

    <!-- id: <id>[ @<game>][ home] -->

  <id>      one of
              flag:--<name>           a command-line flag
              knob:<registry id>      a knob registry row
              key:<domain>.<action>   a key action, domain host or pad
              menu:<row id>           a settings-menu row
              directive:<key>         a config-file word that is not a
                                      knob (config.c is_directive_key)
  @<game>   the value is the one that game's package ships in its
            openpete.toml (packaging/openpete-dist.toml), not the built-in
            one. A marker on games/<game>.md is in that game's scope
            without the suffix.
  home      this marker's anchor is the id's entry in the map. Needed only
            when the id's markers in one scope sit under more than one
            anchor.

Namespaces decided here:

  - A per-game shipped binding keeps the key action's own id,
    `key:host.<action>`, with the `@<game>` suffix. The action is the same
    one the built-in binding names; only the value comes from the shipped
    file. A game-named key domain would be an id the surface dump never
    emits.
  - `portable` is `directive:portable`. It is a top-level word of
    openpete.toml that selects the file layout, is not a knob registry
    row, and has no flag; config.c names it the one directive among the
    knob keys. The surface dump emits it as a `directive:` entry.

Anchors
-------
A marker belongs to the nearest heading above it (the page title when none
is nearer). Anchor ids come from Python-Markdown's toc extension run with
the markdown_extensions of docs/mkdocs.yml, the same slugs mkdocs builds.

Output
------
  top-level keys     global scope: id = "page#anchor"
  [game.<game>]      that game's scope; an id found here wins over the
                     global entry for that game
  [section]          the default page for an id with no entry: knob rows by
                     their registry section, key actions by domain, menu
                     rows, directives, and flags with no knob row

Refusals (exit 2, one line each)
--------------------------------
  - an id's markers in one scope sit under two or more anchors (on one page
    or on several) and none is marked `home`;
  - two `home` markers of one id in one scope sit under different anchors;
  - a malformed marker, an unknown namespace, or an `@<game>` suffix on a
    page of another game;
  - a page whose source headings and the toc extension's headings differ
    in count.

Usage
-----
  tools/gen_docs_map.py            write docs/src/user/pages.toml
  tools/gen_docs_map.py --check    exit 1 when the file differs from what
                                   the pages generate (a stale map)

Stdlib plus Python-Markdown, pymdown-extensions and PyYAML (the mkdocs
toolchain); deterministic output.
"""

import argparse
import re
import sys
from pathlib import Path

import markdown
import yaml

ROOT = Path(__file__).resolve().parent.parent
MKDOCS_YML = ROOT / "docs" / "mkdocs.yml"
USER_DIR = ROOT / "docs" / "src" / "user"
OUT = USER_DIR / "pages.toml"

MARKER_RE = re.compile(r"<!--\s*id:\s*(.*?)\s*-->")
ID_RE = re.compile(
    r"^(flag:--[a-z0-9][a-z0-9-]*"
    r"|knob:[a-z0-9_]+"
    r"|key:(?:host|pad)\.[a-z0-9_]+"
    r"|menu:[a-z0-9_.]+"
    r"|directive:[a-z0-9_]+)$"
)
GAME_RE = re.compile(r"^@([a-z0-9-]+)$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
HEADING_RE = re.compile(r"^\s{0,12}(#{1,6})\s+\S")

# Default page per section, for an id with no entry. Knob rows key on the
# registry section string (knob_desc_t.section); the Debug rows a release
# build reaches are dev-overlay rows. Recording and "Debug diagnostics"
# rows are absent from the shipped packages and have no default page.
SECTION_PAGES = {
    "knob:Display": "graphics/display.md",
    "knob:Pacing": "graphics/smoothness.md",
    "knob:Audio": "customizing/audio.md",
    "knob:Input": "customizing/controllers.md",
    "knob:Paths": "help/file-locations.md",
    "knob:Gameplay": "playing/practice-tools.md",
    "knob:Cheats": "playing/practice-tools.md",
    "knob:Debug": "playing/dev-overlay.md",
    "key:host": "customizing/key-bindings.md",
    "key:pad": "customizing/key-bindings.md",
    "menu": "playing/settings-menu.md",
    "directive": "customizing/config-file.md",
    "flag": "customizing/config-file.md",
}


class Refusal(Exception):
    pass


def md_extensions():
    cfg = yaml.safe_load(MKDOCS_YML.read_text())
    names, configs = [], {}
    for ext in cfg.get("markdown_extensions", []):
        if isinstance(ext, dict):
            (name, conf), = ext.items()
            names.append(name)
            if conf:
                configs[name] = conf
        else:
            names.append(ext)
    # The permalink glyph is presentation; slugs do not depend on it.
    configs.setdefault("toc", {})
    configs["toc"] = {k: v for k, v in configs["toc"].items() if k != "permalink"}
    return names, configs


def toc_ids(text, exts):
    names, configs = exts
    md = markdown.Markdown(extensions=names, extension_configs=configs)
    md.convert(text)
    out = []

    def walk(tokens):
        for t in tokens:
            out.append(t["id"])
            walk(t["children"])

    walk(md.toc_tokens)
    return out


def source_headings(lines):
    """Line numbers (0-based) of the ATX headings outside code fences."""
    heads, fence = [], None
    for i, line in enumerate(lines):
        m = FENCE_RE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)
            elif m.group(1) == fence:
                fence = None
            continue
        if fence is None and HEADING_RE.match(line):
            heads.append(i)
    return heads


def scan_page(path, exts, games):
    rel = path.relative_to(USER_DIR).as_posix()
    text = path.read_text()
    lines = text.split("\n")
    heads = source_headings(lines)
    ids = toc_ids(text, exts)
    if len(heads) != len(ids):
        raise Refusal(f"{rel}: {len(heads)} source headings but the toc "
                      f"extension built {len(ids)} anchors")
    page_game = None
    if rel.startswith("games/"):
        page_game = Path(rel).stem
    out = []
    for i, line in enumerate(lines):
        for m in MARKER_RE.finditer(line):
            words = m.group(1).split()
            where = f"{rel}:{i + 1}"
            if not words or not ID_RE.match(words[0]):
                raise Refusal(f"{where}: malformed marker '{m.group(0)}'")
            ident, game, home = words[0], page_game, False
            for w in words[1:]:
                g = GAME_RE.match(w)
                if g:
                    if page_game and g.group(1) != page_game:
                        raise Refusal(f"{where}: @{g.group(1)} on a page of "
                                      f"{page_game}")
                    if g.group(1) not in games:
                        raise Refusal(f"{where}: unknown game @{g.group(1)}")
                    game = g.group(1)
                elif w == "home":
                    home = True
                else:
                    raise Refusal(f"{where}: unknown marker word '{w}'")
            k = -1
            for n, h in enumerate(heads):
                if h < i:
                    k = n
            if k < 0:
                raise Refusal(f"{where}: marker above the page's first heading")
            out.append((game, ident, f"{rel}#{ids[k]}", home, where))
    return out


def build():
    exts = md_extensions()
    pages = sorted(USER_DIR.rglob("*.md"))
    games = {p.stem for p in (USER_DIR / "games").glob("*.md")}
    markers = []
    for p in pages:
        markers.extend(scan_page(p, exts, games))

    by_scope = {}
    for game, ident, anchor, home, where in markers:
        by_scope.setdefault((game, ident), []).append((anchor, home, where))

    refusals, resolved = [], {}
    for (game, ident), rows in sorted(by_scope.items(),
                                      key=lambda kv: (kv[0][0] or "", kv[0][1])):
        scope = f"[game.{game}] " if game else ""
        homes = {a for a, h, _ in rows if h}
        anchors = {a for a, _, _ in rows}
        if len(homes) > 1:
            refusals.append(f"{scope}{ident}: home markers under "
                            f"{len(homes)} anchors: "
                            + ", ".join(w for a, h, w in rows if h))
            continue
        if homes:
            resolved[(game, ident)] = homes.pop()
            continue
        if len(anchors) > 1:
            pages_seen = {a.split("#")[0] for a in anchors}
            kind = ("on several pages" if len(pages_seen) > 1
                    else "on one page")
            refusals.append(f"{scope}{ident}: markers under {len(anchors)} "
                            f"anchors {kind} and none marked home: "
                            + ", ".join(sorted(anchors)))
            continue
        resolved[(game, ident)] = anchors.pop()
    if refusals:
        raise Refusal("\n".join(refusals))
    return resolved, len(markers)


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(resolved):
    out = [
        "# The User docs map: namespaced id -> page#anchor (page relative to",
        "# docs/src/user/). Generated by tools/gen_docs_map.py from the id",
        "# markers on the User pages; edit the markers, not this file.",
        "# Lookup for game G: [game.G], then the top-level keys, then",
        "# [section] by the id's section.",
        "",
    ]
    glob_ids = sorted(k[1] for k in resolved if k[0] is None)
    for ident in glob_ids:
        out.append(f"{q(ident)} = {q(resolved[(None, ident)])}")
    for game in sorted({k[0] for k in resolved if k[0]}):
        out += ["", f"[game.{game}]"]
        for ident in sorted(k[1] for k in resolved if k[0] == game):
            out.append(f"{q(ident)} = {q(resolved[(game, ident)])}")
    out += ["", "[section]"]
    for sec, page in SECTION_PAGES.items():
        out.append(f"{q(sec)} = {q(page)}")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when pages.toml is stale")
    args = ap.parse_args()
    try:
        resolved, n_markers = build()
    except Refusal as e:
        print(f"gen_docs_map: refused:\n{e}", file=sys.stderr)
        return 2
    for page in SECTION_PAGES.values():
        if not (USER_DIR / page).is_file():
            print(f"gen_docs_map: refused: section page {page} missing",
                  file=sys.stderr)
            return 2
    text = render(resolved)
    n_game = sum(1 for k in resolved if k[0])
    summary = (f"{n_markers} markers, {len(resolved) - n_game} global ids, "
               f"{n_game} game-scoped ids")
    if args.check:
        cur = OUT.read_text() if OUT.is_file() else ""
        if cur != text:
            print(f"gen_docs_map: {OUT.relative_to(ROOT)} is stale; run "
                  f"tools/gen_docs_map.py", file=sys.stderr)
            return 1
        print(f"gen_docs_map: current ({summary})")
        return 0
    OUT.write_text(text)
    print(f"gen_docs_map: wrote {OUT.relative_to(ROOT)} ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
