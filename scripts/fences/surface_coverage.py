#!/usr/bin/env python3
"""Fence: every release id of a shipped game has a User docs entry.

Also: the error ids agree with the troubleshooting page, and every mod.toml
key the loader reads has a Modding docs entry.

Reads, per built game, the OPENPETE_SURFACE_DUMP output and the `--help`
output of its linux-release binary, the User docs map
(docs/src/user/pages.toml), the shipped games (games/shipped.toml), the
shipped settings (packaging/openpete-dist.toml), the shipped presets
(cmake/presets/*-launcher.cmake), the parser literals of
core/config/options.c and the schema rows of core/config/config_schema.c.

Audience (derived per shipped game, never typed):
  flag:       release when the shipped `--help` defines it: a `--x` token
              that begins a line after exactly two spaces, or on a preamble
              line (above the `--disc` line) follows two or more spaces;
              the debug block counts while a shipped preset compiles it
              (OPENPETE_HIDE_DEBUG_FLAGS not ON), the libav block while one
              sets OPENPETE_ENABLE_RECORD ON.
  knob:       release when compiled in a shipped build and set by a
              release flag, named by a displayed settings-menu row, or
              outside the debug block while the game's shipped binding of
              `key:host.devui` is not `none`; a pass-through env row (a
              string row with no flag whose id is its OPENPETE_ env name,
              the ENVROW_* shape) takes no release from the overlay.
  menu:       release when displayed.
  key:        release when not KM_DEV, or the shipped toml binds it.
  directive:  always release.
  vocab:      release when the flag or knob that takes it is release.

Resolution: an id with no docs-map entry resolves through a flag and the
registry row its `flag` field names (or a schema alias naming the row),
a menu row through its knob, a vocabulary entry through its flag, else its
knob, and a key:pad2..pad4 action through its key:pad action.

Legs (each failure is one REFUSED line):
  parser    every `strcmp(a, "--x")` literal of options.c is a dump flag
  help      the `--help` definitions and the dump's flags match both ways
  map       docs/src/user/pages.toml equals what the markers generate
  entry     every release id of each shipped game has an entry, or
            resolves through one, whose page and anchor exist
  stray     every docs-map entry names an id of some dump
  schema    every schema key is a dump row
  default   every release flag taking a value and every release non-action
            row declares `def` or `default_note`
  hint      every menu hint is inside the menu glyph set
  errid     the OP-E ids of core/include/openpete_error_ids.h and the rows
            of the By error id table of docs/src/user/help/
            troubleshooting.md agree both ways, each row anchored at the
            id in lower case, and OPENPETE_TROUBLESHOOTING_URL names that
            page
  modkey    every mod.toml key core/mods/mod_loader.c reads (the literal
            key of a toml_*_in call or of a helper taking
            (toml_table_t*, const char* key)) is named on a
            docs/src/modding/ page, in backticks or as a TOML key or table
            line
  A shipped game with zero release rows or zero key actions is VACUOUS.
`--help` and `-h` are exempt from the flag legs.

The map and anchor legs need Python-Markdown and PyYAML (the mkdocs
toolchain, as tools/gen_docs_map.py); without them they are reported as
not run, never as passed.

Usage:
  surface_coverage.py check --build-dir DIR [--warn]
  surface_coverage.py check --input-dir DIR [--warn]
      --build-dir runs each built game's binary for its dump and --help;
      --input-dir reads <game>.json and <game>.help from DIR.
  surface_coverage.py capture --build-dir DIR --out DIR
      Write <game>.json and <game>.help of each built game into DIR.
  surface_coverage.py compare --game G --dump FILE --help-file FILE
                             --baseline FILE
      The release subset of a shipped package's dump (FILE and its
      --help output) against the committed baseline
      packaging/surface/<G>.json, with the release audience of both read
      off the package's --help: the same release ids, each entry equal
      (source_sha and the selfcheck counts ignored). The release scripts
      run it on the package's own binary.
  surface_coverage.py shipped
      Print the games of games/shipped.toml, one per line.

--warn prints each finding as a warning and exits 0 (the per-build step).
Exit: 0 pass, 1 refusal or VACUOUS, 2 usage, 64 skip (no binary, or the
map leg could not run and nothing else failed).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(ROOT / "scripts" / "lib"))
from surface_macros import tomllib  # noqa: E402
import game_conf  # noqa: E402

USER_DIR = ROOT / "docs" / "src" / "user"
MAP_TOML = USER_DIR / "pages.toml"
SHIPPED_TOML = ROOT / "games" / "shipped.toml"
DIST_TOML = ROOT / "packaging" / "openpete-dist.toml"
PRESETS = (ROOT / "cmake" / "presets" / "linux-release-launcher.cmake",
           ROOT / "cmake" / "presets" / "windows-mingw-launcher.cmake")
OPTIONS_C = ROOT / "core" / "config" / "options.c"
SCHEMA_C = ROOT / "core" / "config" / "config_schema.c"
ERROR_IDS_H = ROOT / "core" / "include" / "openpete_error_ids.h"
TROUBLESHOOTING_MD = USER_DIR / "help" / "troubleshooting.md"
MOD_LOADER_C = ROOT / "core" / "mods" / "mod_loader.c"
MODDING_DIR = ROOT / "docs" / "src" / "modding"
ALL_GAMES = ("spyro-1", "spyro-2", "spyro-3")
EXEMPT_FLAGS = {"--help", "-h"}

# The --help block headers. A header is an unindented line ending in ':'
# after the preamble; an unknown one is refused, since its audience is
# undecided.
HELP_HEADERS = {"Video recording (needs libav):": "libav",
                "Debug / diagnostic flags:": "debug"}
# Build conditions a dump row may carry, and the CMake option of a shipped
# preset that compiles it.
BUILD_CONDS = {"OPENPETE_HAS_LIBAV": "OPENPETE_ENABLE_RECORD"}
# The settings menu draws its hints with the game's text mobys: 0-9 A-Z
# % / ? + ^ . ' and a faked colon (core/ui/openpete_menu.c notify_row).
HINT_RE = re.compile(r"^[0-9A-Z%/?+^.': ]*$")
FLAG_TOKEN = r"--[a-z0-9][a-z0-9-]*"
BODY_DEF_RE = re.compile(r"^  (" + FLAG_TOKEN + r")(?![a-z0-9-])")
PRE_DEF_RE = re.compile(r"(?<=  )(" + FLAG_TOKEN + r")(?![a-z0-9-])")
PARSER_RE = re.compile(r'strcmp\(a, "(--[^"]+)"\)')
SCHEMA_ROW_RE = re.compile(r'^\s*\{\s*"([a-z0-9_]+)",\s*(?:"|NULL)')
# One OPENPETE_ERROR_IDS row: X(NAME, "OP-E-...", "anchor").
ERROR_ROW_RE = re.compile(r'X\(\s*(\w+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')
ERROR_URL_RE = re.compile(r'#define\s+OPENPETE_TROUBLESHOOTING_URL\s*\\?\s*'
                          r'"https://openpete\.com/docs/([^"#]+)\.html"')
# One row of the troubleshooting page's By error id table.
ERROR_DOC_RE = re.compile(r'^\|\s*<a id="([^"]+)"></a>`([^`]+)`\s*\|', re.M)
# A mod.toml key the loader reads: the literal key of a toml_*_in call, or
# of a call to a helper that takes (toml_table_t*, const char* key).
TOML_READ_RE = re.compile(r'toml_\w+_in\(\s*[\w>.-]+\s*,\s*"([^"]+)"')
KEY_HELPER_RE = re.compile(r'^\w[\w\s\*]*?\b(\w+)\(\s*toml_table_t\s*\*\s*\w+\s*,'
                           r'\s*const char\s*\*\s*key\b', re.M)
MARKER_IDS = re.compile(r"^(flag:|knob:|key:(?:host|pad)\.|menu:|directive:)")


class Finding:
    def __init__(self, leg, text):
        self.leg, self.text = leg, text


# -- inputs ------------------------------------------------------------


def exe_argv(build_dir, game):
    """The argv prefix running `game` from the build tree with no disc."""
    return game_conf.tree_argv(str(build_dir), game)


def scrubbed_env():
    return {k: v for k, v in os.environ.items()
            if not k.startswith("OPENPETE_")}


def capture(build_dir, out_dir):
    """Write <game>.json and <game>.help of each built game; returns the
    games captured.

    Runs the Linux binaries of a linux-release build only; OPENPETE_HAS_LIBAV
    above is a build-condition name, not an environment variable
    (wslenv-guard: no-launch)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    games = []
    for g in ALL_GAMES:
        argv = exe_argv(build_dir, g)
        exe = argv[0]
        dump, helptxt = out_dir / f"{g}.json", out_dir / f"{g}.help"
        for p in (dump, helptxt):
            if p.exists():
                p.unlink()
        if not os.path.isfile(exe):
            continue
        env = scrubbed_env()
        env["OPENPETE_SURFACE_DUMP"] = str(dump)
        r = subprocess.run(argv, env=env, capture_output=True,
                           text=True, timeout=120)
        if r.returncode != 0 or not dump.is_file():
            raise RuntimeError(f"{exe}: the surface dump exited "
                               f"{r.returncode}: {r.stderr.strip()[-400:]}")
        r = subprocess.run(argv + ["--help"], env=scrubbed_env(),
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(f"{exe} --help exited {r.returncode}")
        helptxt.write_text(r.stdout + r.stderr)
        games.append(g)
    return games


def load_inputs(in_dir):
    """{game: (dump, help text)} of every game with both files in in_dir."""
    out = {}
    for g in ALL_GAMES:
        d, h = Path(in_dir) / f"{g}.json", Path(in_dir) / f"{g}.help"
        if d.is_file() and h.is_file():
            out[g] = (json.loads(d.read_text()), h.read_text())
    return out


def preset_options():
    """{option: [value per shipped preset]} of the preset defaults."""
    opts = {}
    for p in PRESETS:
        text = p.read_text()
        for m in re.finditer(r"openpete_preset_default\((\w+)\s+(\w+)\)",
                             text):
            opts.setdefault(m.group(1), []).append(m.group(2))
    return opts


class Shipped:
    """The shipped presets' conditions, games and settings."""

    def __init__(self):
        opts = preset_options()
        on = ("ON", "1", "TRUE", "YES")
        hide = opts.get("OPENPETE_HIDE_DEBUG_FLAGS", [])
        self.debug = len(hide) < len(PRESETS) or any(
            v.upper() not in on for v in hide)
        self.conds = {}
        for cond, opt in BUILD_CONDS.items():
            self.conds[cond] = any(v.upper() in on
                                   for v in opts.get(opt, ["ON"]))
        self.games = list(tomllib.loads(SHIPPED_TOML.read_text())["games"])
        self.dist = tomllib.loads(DIST_TOML.read_text())

    def compiled(self, cond):
        if not cond:
            return True
        if cond not in self.conds:
            raise KeyError(cond)
        return self.conds[cond]

    def binding(self, key_id, game, built_in):
        """The game's shipped binding of a key action: built-in, then the
        global [keys.<domain>], then [game.<game>.keys.<domain>]."""
        domain, action = key_id[len("key:"):].split(".", 1)
        val = built_in
        for scope in (self.dist, self.dist.get("game", {}).get(game, {})):
            v = scope.get("keys", {}).get(domain, {}).get(action)
            if v is not None:
                val = ",".join(v) if isinstance(v, list) else str(v)
        return val


# -- --help ------------------------------------------------------------


def help_defs(text):
    """{flag: block} of the --help definitions. Blocks: preamble, release,
    libav, debug. Raises ValueError on an unknown header."""
    defs, block, pre = {}, "preamble", True
    for line in text.splitlines():
        if pre and BODY_DEF_RE.match(line) and \
                BODY_DEF_RE.match(line).group(1) == "--disc":
            pre, block = False, "release"
        if pre:
            for m in PRE_DEF_RE.finditer(line):
                defs.setdefault(m.group(1), "preamble")
            continue
        if line and not line[0].isspace() and line.rstrip().endswith(":"):
            name = line.strip()
            if name not in HELP_HEADERS:
                raise ValueError(f"unknown --help block header '{name}'")
            block = HELP_HEADERS[name]
            continue
        m = BODY_DEF_RE.match(line)
        if m:
            defs.setdefault(m.group(1), block)
    if pre:
        raise ValueError("--help has no --disc line")
    return defs


def help_release(defs, shipped):
    """The flags a shipped package's --help defines."""
    out = set()
    for f, b in defs.items():
        if b in ("preamble", "release") \
                or (b == "debug" and shipped.debug) \
                or (b == "libav" and shipped.compiled("OPENPETE_HAS_LIBAV")):
            out.add(f)
    return out


# -- audience and resolution -------------------------------------------


class Game:
    """One game's dump with its derived audience."""

    def __init__(self, name, dump, helptext, shipped):
        self.name = name
        self.entries = dump["entries"]
        self.idx = {e["id"]: e for e in self.entries}
        self.defs = help_defs(helptext)
        self.shipped = shipped
        self.rel_flags = help_release(self.defs, shipped)
        self.menu_knobs = {e["knob"] for e in self.entries
                           if e["id"].startswith("menu:")
                           and e.get("displayed") and e.get("knob")}
        devui = self.idx.get("key:host.devui")
        self.devui = shipped.binding("key:host.devui", name,
                                     devui["def"] if devui else "none")
        self.alias_of = {}
        for e in self.entries:
            if e["id"].startswith("flag:") and "alias_value" in e:
                self.alias_of.setdefault(e["knob"], []).append(e["id"])

    def ns(self, ident):
        return ident.split(":", 1)[0]

    def compiled(self, e):
        if e.get("block") == "debug" and not self.shipped.debug:
            return False
        return self.shipped.compiled(e.get("build_cond"))

    def passthrough(self, e):
        return (e.get("type") == "string" and not e.get("flag")
                and e.get("env") == "OPENPETE_" + e["id"][len("knob:"):])

    def release_reason(self, e):
        """Why an entry is release in a shipped build, or None."""
        i, ns = e["id"], self.ns(e["id"])
        if ns == "flag":
            if i in EXEMPT_FLAGS:
                return None
            return "the shipped --help defines it" \
                if e["flag"] in self.rel_flags else None
        if ns == "knob":
            if not self.compiled(e):
                return None
            name = i[len("knob:"):]
            if e.get("flag") and e["flag"] in self.rel_flags:
                return f"set by the release flag {e['flag']}"
            if any(self.idx[a]["flag"] in self.rel_flags
                   for a in self.alias_of.get(name, [])):
                return "set by a release schema alias"
            if name in self.menu_knobs:
                return "a displayed settings-menu row sets it"
            if e.get("block") != "debug" and self.devui != "none" \
                    and not self.passthrough(e):
                return "the dev overlay shows it (devui bound to " \
                       f"{self.devui})"
            return None
        if ns == "menu":
            return "displayed in the settings menu" \
                if e.get("displayed") else None
        if ns == "key":
            if not e.get("dev"):
                return "a key action not marked KM_DEV"
            b = self.shipped.binding(i, self.name, e["def"])
            return f"the shipped toml binds it ({b})" if b != "none" else None
        if ns == "directive":
            return "a config-file directive"
        if ns == "vocab":
            f = self.idx.get("flag:" + e["flag"]) if e.get("flag") else None
            if f is not None and self.release_reason(f):
                return f"taken by the release flag {e['flag']}"
            return None
        return None

    def via(self, ident):
        """The ids an id resolves through, in order, itself first."""
        e = self.idx[ident]
        ns = self.ns(ident)
        out = [ident]
        if ns == "flag" and e.get("knob"):
            out += self.knob_group(e["knob"])
        elif ns == "knob":
            out += self.knob_group(ident[len("knob:"):])
        elif ns == "menu" and e.get("knob"):
            out += self.knob_group(e["knob"])
        elif ns == "vocab" and e.get("flag"):
            f = self.idx.get("flag:" + e["flag"])
            if f is not None:
                out += self.via(f["id"])
        elif ns == "key":
            domain, action = ident[len("key:"):].split(".", 1)
            if domain in ("pad2", "pad3", "pad4"):
                out.append(f"key:pad.{action}")
        seen, uniq = set(), []
        for i in out:
            if i not in seen:
                seen.add(i)
                uniq.append(i)
        return uniq

    def knob_group(self, name):
        out = ["knob:" + name]
        k = self.idx.get("knob:" + name)
        if k is not None and k.get("flag"):
            out.append("flag:" + k["flag"])
        out += self.alias_of.get(name, [])
        return out

    def marker_for(self, ident):
        """The id a marker for this id names (the marker grammar admits no
        vocab: and no pad2..pad4 id)."""
        for i in self.via(ident):
            if MARKER_IDS.match(i) and i in self.idx or MARKER_IDS.match(i) \
                    and i.startswith("key:pad."):
                return i
        return ident

    def section_page(self, ident, sections):
        ns = self.ns(ident)
        if ns == "knob":
            k = self.idx.get(ident) or {}
            return sections.get("knob:" + str(k.get("section")),
                                "(no section page)")
        if ns == "flag":
            e = self.idx.get(ident) or {}
            if e.get("knob"):
                return self.section_page("knob:" + e["knob"], sections)
            return sections.get("flag")
        if ns == "key":
            return sections.get("key:host" if ident.startswith("key:host.")
                                else "key:pad")
        return sections.get(ns, "(no section page)")


# -- docs map ----------------------------------------------------------


class DocsMap:
    def __init__(self, text):
        data = tomllib.loads(text)
        self.sections = data.get("section", {})
        self.games = data.get("game", {})
        self.glob = {k: v for k, v in data.items()
                     if k not in ("section", "game")}

    def lookup(self, ident, game):
        return self.games.get(game, {}).get(ident) or self.glob.get(ident)

    def all_ids(self):
        out = set(self.glob)
        for scope in self.games.values():
            out |= set(scope)
        return out


class Anchors:
    """Anchor ids per User page from Python-Markdown's toc extension, the
    slugs mkdocs builds (tools/gen_docs_map.py)."""

    def __init__(self):
        import gen_docs_map as gdm  # needs markdown and yaml
        self.gdm = gdm
        self.exts = gdm.md_extensions()
        self.cache = {}

    def has(self, page, frag):
        if page not in self.cache:
            path = USER_DIR / page
            self.cache[page] = set(self.gdm.toc_ids(path.read_text(),
                                                    self.exts)) \
                if path.is_file() else None
        ids = self.cache[page]
        return ids is not None and frag in ids


def map_current():
    """(ok, message) of gen_docs_map.py --check, or (None, why) when the
    toolchain is missing."""
    try:
        import gen_docs_map as gdm
    except ImportError as exc:
        return None, f"not run ({exc.name} is not importable)"
    try:
        resolved, _n = gdm.build()
    except gdm.Refusal as exc:
        return False, f"gen_docs_map refuses the markers: {exc}"
    text = gdm.render(resolved)
    cur = MAP_TOML.read_text() if MAP_TOML.is_file() else ""
    if cur != text:
        return False, ("docs/src/user/pages.toml is stale: run "
                       "tools/gen_docs_map.py")
    return True, "current"


# -- legs --------------------------------------------------------------


def flag_ids(game):
    return {e["flag"] for e in game.entries if e["id"].startswith("flag:")}


def leg_parser(games):
    lits = set(PARSER_RE.findall(OPTIONS_C.read_text())) - EXEMPT_FLAGS
    out = []
    for g in games.values():
        have = flag_ids(g)
        for f in sorted(lits - have):
            out.append(Finding("parser", f"{g.name}: the parser literal {f} "
                               f"(core/config/options.c) is no flag entry of "
                               f"the dump: add a registry row (§27) or a "
                               f"help-only row"))
    return out, len(lits)


def leg_help(games):
    out = []
    for g in games.values():
        defs = set(g.defs) - EXEMPT_FLAGS
        has_libav = "libav" in g.defs.values()
        dump = set()
        for e in g.entries:
            if not e["id"].startswith("flag:") or e["flag"] in EXEMPT_FLAGS:
                continue
            if "alias_value" in e:
                continue  # an alias is named in its row's help text
            if e.get("build_cond") == "OPENPETE_HAS_LIBAV" and not has_libav:
                continue
            dump.add(e["flag"])
        for f in sorted(defs - dump):
            out.append(Finding("help", f"{g.name}: --help defines {f} and "
                               f"the dump has no flag entry for it"))
        for f in sorted(dump - defs):
            out.append(Finding("help", f"{g.name}: the dump has flag:{f} "
                               f"and --help defines no {f}"))
    return out


def leg_entries(games, dmap, anchors, shipped):
    out, counts = [], {}
    for name in shipped.games:
        g = games.get(name)
        if g is None:
            out.append(Finding("entry", f"{name}: shipped (games/shipped."
                               f"toml) but no dump or --help was read"))
            continue
        c = counts.setdefault(name, {})
        for e in g.entries:
            reason = g.release_reason(e)
            if reason is None:
                continue
            ns = g.ns(e["id"])
            c[ns] = c.get(ns, 0) + 1
            hit = None
            for i in g.via(e["id"]):
                a = dmap.lookup(i, name)
                if a:
                    hit = (i, a)
                    break
            if hit is None:
                m = g.marker_for(e["id"])
                out.append(Finding("entry", (
                    f"{name}: {e['id']} is release ({reason}) and has no "
                    f"docs-map entry and resolves through none: add "
                    f"`<!-- id: {m} -->` after its literal on "
                    f"{g.section_page(m, dmap.sections)} (the section's "
                    f"default page) or the page that documents it")))
                continue
            c["resolved_via"] = c.get("resolved_via", 0) + (hit[0] != e["id"])
            page, _, frag = hit[1].partition("#")
            if not (USER_DIR / page).is_file():
                out.append(Finding("entry", f"{name}: {e['id']} resolves "
                                   f"through {hit[0]} to {hit[1]}, whose "
                                   f"page does not exist"))
            elif anchors is not None and frag and not anchors.has(page, frag):
                out.append(Finding("entry", f"{name}: {e['id']} resolves "
                                   f"through {hit[0]} to {hit[1]}, whose "
                                   f"anchor does not exist"))
    return out, counts


def leg_stray(games, dmap):
    ids = set()
    for g in games.values():
        ids |= set(g.idx)
    return [Finding("stray", f"the docs map names {i}, which no game's "
                    f"dump holds: a typo or a removed row; fix or delete "
                    f"its marker")
            for i in sorted(dmap.all_ids() - ids)]


def leg_schema(games):
    keys = set()
    for line in SCHEMA_C.read_text().splitlines():
        m = SCHEMA_ROW_RE.match(line)
        if m:
            keys.add(m.group(1))
    out = []
    for g in games.values():
        have = {e["config_key"] for e in g.entries
                if e["id"].startswith("knob:") and e.get("config_key")}
        for k in sorted(keys - have):
            out.append(Finding("schema", f"{g.name}: the schema key {k} "
                               f"(core/config/config_schema.c) is no row of "
                               f"the dump"))
    return out, len(keys)


def leg_defaults(games, shipped):
    out = []
    for name in shipped.games:
        g = games.get(name)
        if g is None:
            continue
        for e in g.entries:
            ns = g.ns(e["id"])
            if ns == "flag":
                takes = e.get("takes_arg", "none") != "none"
            elif ns == "knob":
                takes = e.get("type") != "action"
            else:
                continue
            if not takes or not g.release_reason(e):
                continue
            if e.get("def") is None and not e.get("default_note"):
                out.append(Finding("default", (
                    f"{name}: {e['id']} is a release "
                    f"{'flag taking a value' if ns == 'flag' else 'row'} "
                    f"and declares neither def nor default_note")))
    return out


def leg_hints(games):
    out = []
    for g in games.values():
        for e in g.entries:
            if e["id"].startswith("menu:") and e.get("hint") is not None \
                    and not HINT_RE.match(e["hint"]):
                bad = sorted({ch for ch in e["hint"]
                              if not HINT_RE.match(ch)})
                out.append(Finding("hint", f"{g.name}: {e['id']} hint "
                                   f"'{e['hint']}' holds {bad}, outside the "
                                   f"menu glyph set (0-9 A-Z % / ? + ^ . ' : "
                                   f"space)"))
    return out


def leg_error_ids(header_text, page_text, page_rel):
    """The OPENPETE_ERROR_IDS table and the troubleshooting page's By error
    id table agree: the same ids, each page row anchored at its code row's
    anchor, every anchor the id in lower case, and the URL constant naming
    the page. Returns (findings, row count)."""
    out = []
    code = ERROR_ROW_RE.findall(header_text)
    if not code:
        return [Finding("errid", "no OPENPETE_ERROR_IDS row parsed from "
                        "core/include/openpete_error_ids.h")], 0
    m = ERROR_URL_RE.search(header_text)
    want = page_rel[:-len(".md")]
    if not m or m.group(1) != want:
        out.append(Finding("errid", (
            f"OPENPETE_TROUBLESHOOTING_URL names "
            f"{m.group(1) + '.html' if m else 'no openpete.com page'}, not "
            f"the page that holds the By error id table ({want}.html)")))
    section = page_text.split("\n## By error id", 1)
    if len(section) < 2:
        return out + [Finding("errid", f"{page_rel} has no '## By error id' "
                              f"section")], len(code)
    body = section[1].split("\n## ", 1)[0]
    docs = {}
    for anchor, ident in ERROR_DOC_RE.findall(body):
        if ident in docs:
            out.append(Finding("errid", f"{page_rel}: {ident} has two rows"))
        docs[ident] = anchor
    seen = set()
    for name, ident, anchor in code:
        if ident in seen:
            out.append(Finding("errid", f"openpete_error_ids.h: {ident} is "
                               f"on two rows"))
        seen.add(ident)
        if anchor != ident.lower():
            out.append(Finding("errid", (
                f"openpete_error_ids.h: {name} anchor '{anchor}' is not its "
                f"id in lower case ('{ident.lower()}')")))
        if ident not in docs:
            out.append(Finding("errid", (
                f"{ident} (openpete_error_ids.h {name}) has no row in "
                f"{page_rel} '## By error id'; add "
                f"| <a id=\"{anchor}\"></a>`{ident}` | ... |")))
        elif docs[ident] != anchor:
            out.append(Finding("errid", (
                f"{page_rel}: {ident} is anchored '{docs[ident]}', the code "
                f"links '#{anchor}'")))
    for ident in sorted(set(docs) - seen):
        out.append(Finding("errid", (
            f"{page_rel}: {ident} has a row but no OPENPETE_ERROR_IDS row; "
            f"a retired error keeps its row in both tables")))
    return out, len(code)


def mod_toml_keys(loader_text):
    """The mod.toml keys mod_loader.c reads, in source order."""
    keys = TOML_READ_RE.findall(loader_text)
    for helper in set(KEY_HELPER_RE.findall(loader_text)):
        keys += re.findall(r'\b' + re.escape(helper) +
                           r'\(\s*\w+\s*,\s*"([^"]+)"', loader_text)
    return list(dict.fromkeys(keys))


def key_documented(key, pages):
    """Whether a Modding page names the key: in backticks, or as a TOML key
    or table header at the start of a line."""
    pats = (re.compile(r"`" + re.escape(key) + r"`"),
            re.compile(r"^\s*" + re.escape(key) + r"\s*=", re.M),
            re.compile(r"^\s*\[\[?" + re.escape(key) + r"\]\]?", re.M))
    return any(p.search(t) for t in pages.values() for p in pats)


def leg_mod_keys(loader_text, pages):
    """Every mod.toml key the loader reads is named on a Modding page.
    Returns (findings, key count)."""
    keys = mod_toml_keys(loader_text)
    if not keys:
        return [Finding("modkey", "no mod.toml key parsed from "
                        "core/mods/mod_loader.c")], 0
    out = [Finding("modkey", (
        f"mod.toml key '{k}' (core/mods/mod_loader.c) is named on no page "
        f"of docs/src/modding/; document it in manifest.md or the page of "
        f"its table"))
        for k in keys if not key_documented(k, pages)]
    return out, len(keys)


def source_legs():
    """The legs over source tables with no dump input: error ids and
    mod.toml keys. Returns (findings, error id rows, mod.toml keys)."""
    page_rel = TROUBLESHOOTING_MD.relative_to(ROOT / "docs" / "src").as_posix()
    f1, n_err = leg_error_ids(ERROR_IDS_H.read_text(),
                              TROUBLESHOOTING_MD.read_text(), page_rel)
    pages = {p.name: p.read_text() for p in sorted(MODDING_DIR.glob("*.md"))}
    f2, n_keys = leg_mod_keys(MOD_LOADER_C.read_text(), pages)
    return f1 + f2, n_err, n_keys


def check(inputs, warn=False):
    """Run every leg over {game: (dump, help)}; returns the exit code."""
    shipped = Shipped()
    games = {}
    try:
        for g, (dump, helptext) in inputs.items():
            games[g] = Game(g, dump, helptext, shipped)
    except (ValueError, KeyError) as exc:
        print(f"surface_coverage: REFUSED: {exc}")
        return 0 if warn else 1
    hidden = [g for g, x in games.items() if "debug" not in x.defs.values()]
    if hidden:
        print(f"surface_coverage: SKIP: {', '.join(hidden)} --help prints no "
              f"debug block (OPENPETE_HIDE_DEBUG_FLAGS); the check reads a "
              f"build that compiles every row (linux-release)")
        return 0 if warn else 64
    findings = []
    f, n_lits = leg_parser(games)
    findings += f
    findings += leg_help(games)
    map_ok, map_msg = map_current()
    if map_ok is False:
        findings.append(Finding("map", map_msg))
    try:
        anchors = Anchors()
    except ImportError:
        anchors = None
    dmap = DocsMap(MAP_TOML.read_text())
    f, counts = leg_entries(games, dmap, anchors, shipped)
    findings += f
    findings += leg_stray(games, dmap)
    f, n_schema = leg_schema(games)
    findings += f
    findings += leg_defaults(games, shipped)
    findings += leg_hints(games)
    f, n_err, n_keys = source_legs()
    findings += f

    vacuous = []
    for name in shipped.games:
        c = counts.get(name, {})
        if not c.get("knob") or not c.get("key"):
            vacuous.append(f"{name}: {c.get('knob', 0)} release rows, "
                           f"{c.get('key', 0)} release key actions")
    tag = "warning" if warn else "REFUSED"
    for x in findings:
        print(f"surface_coverage: {tag} [{x.leg}] {x.text}")
    for v in vacuous:
        print(f"surface_coverage: {'warning' if warn else 'VACUOUS'}: {v}")
    for name, c in counts.items():
        parts = ", ".join(f"{ns} {c.get(ns, 0)}" for ns in
                          ("flag", "knob", "menu", "key", "directive",
                           "vocab"))
        print(f"surface_coverage: {name}: release ids {parts}; "
              f"{c.get('resolved_via', 0)} resolved through another id")
    print(f"surface_coverage: {len(games)} games read, {n_lits} parser "
          f"literals, {n_schema} schema keys; map leg "
          f"{'current' if map_ok else map_msg if map_ok is None else 'STALE'}"
          f"; anchors {'checked' if anchors else 'not run'}; {n_err} error "
          f"ids, {n_keys} mod.toml keys; {len(findings)} findings")
    if warn:
        return 0
    if findings or vacuous:
        return 1
    if map_ok is None or anchors is None:
        print("surface_coverage: SKIP: the map leg did not run (install "
              "the mkdocs toolchain)")
        return 64
    print("surface_coverage: PASS")
    return 0


def release_subset(game, dump, helptext, shipped):
    """{id: entry} of the release entries of a dump."""
    g = Game(game, dump, helptext, shipped)
    return {e["id"]: e for e in g.entries if g.release_reason(e)}


def compare(game, dump_path, help_path, baseline_path):
    """Exit code of the shipped dump's release subset against the
    baseline's."""
    helptext = Path(help_path).read_text()
    shipped = Shipped()
    try:
        a = release_subset(game, json.loads(Path(dump_path).read_text()),
                           helptext, shipped)
        b = release_subset(game, json.loads(Path(baseline_path).read_text()),
                           helptext, shipped)
    except (ValueError, KeyError) as exc:
        print(f"surface_coverage: REFUSED: {exc}")
        return 1
    diffs = []
    for i in sorted(set(a) | set(b)):
        if i not in b:
            diffs.append(f"{i} is release in the package and absent from "
                         f"the baseline's release subset")
        elif i not in a:
            diffs.append(f"{i} is release in the baseline and absent from "
                         f"the package's release subset")
        elif a[i] != b[i]:
            keys = sorted(k for k in set(a[i]) | set(b[i])
                          if a[i].get(k) != b[i].get(k))
            diffs.append(f"{i} differs in {', '.join(keys)}: package "
                         f"{ {k: a[i].get(k) for k in keys} }, baseline "
                         f"{ {k: b[i].get(k) for k in keys} }")
    for d in diffs:
        print(f"surface_coverage: REFUSED [compare] {game}: {d}")
    print(f"surface_coverage: compare {game}: {len(a)} release ids in the "
          f"package, {len(b)} in the baseline, {len(diffs)} differences")
    if not a:
        print("surface_coverage: VACUOUS: the package has no release id")
        return 1
    return 1 if diffs else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    src = c.add_mutually_exclusive_group(required=True)
    src.add_argument("--build-dir")
    src.add_argument("--input-dir")
    c.add_argument("--warn", action="store_true")
    p = sub.add_parser("capture")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--out", required=True)
    m = sub.add_parser("compare")
    m.add_argument("--game", required=True)
    m.add_argument("--dump", required=True)
    m.add_argument("--help-file", dest="help_file", required=True)
    m.add_argument("--baseline", required=True)
    sub.add_parser("shipped")
    args = ap.parse_args()
    if args.cmd == "shipped":
        print("\n".join(Shipped().games))
        return 0
    if args.cmd == "compare":
        return compare(args.game, args.dump, args.help_file, args.baseline)
    if args.cmd == "capture":
        got = capture(args.build_dir, args.out)
        print(f"surface_coverage: captured {', '.join(got) or 'nothing'}")
        return 0 if got else 64
    if args.input_dir:
        inputs = load_inputs(args.input_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="surface-coverage-") as tmp:
            try:
                capture(args.build_dir, tmp)
            except RuntimeError as exc:
                print(f"surface_coverage: REFUSED: {exc}")
                return 0 if args.warn else 1
            inputs = load_inputs(tmp)
    if not inputs:
        print("surface_coverage: SKIP: no game binary or input to read")
        return 0 if args.warn else 64
    return check(inputs, args.warn)


if __name__ == "__main__":
    sys.exit(main())
