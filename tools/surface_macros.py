"""surface_macros.py - expand the surface macros of the User docs.

A User page writes a value the program declares (a default, a key binding,
a config key, a flag spelling) as a macro, and the value comes from the
committed surface baseline `packaging/surface/<game>.json` (the
OPENPETE_SURFACE_DUMP output of that game plus `source_sha`) overlaid by
the game's scope of the shipped `packaging/openpete-dist.toml`. Every macro
is followed on its line by the id marker it renders
(`<!-- id: <id> -->`), which tools/gen_docs_map.py reads.

Macros
------
  {{ default("<id>"[, "<game>"]) }}       the bare `def` of a knob: or
                                          flag: entry; with a game, the
                                          shipped toml's value for that
                                          game in CLI vocabulary
                                          (true -> 1) when it sets one
  {{ key("<id>"[, "<game>"][, n]) }}      the n-th keyboard element (from
                                          1) of a key action's binding as
                                          ++name++; a KM_DEV row with no
                                          binding renders
                                          `none (++X++ when bound)`
  {{ key_pad("<id>"[, "<game>"][, n]) }}  the n-th pad element as
                                          `pad:<button>`
  {{ config_key("<id>") }}                `<config key>` of a knob: or
                                          flag: entry
  {{ flag("<id>") }}                      `--x` of a flag: entry, or of a
                                          knob: entry's flag

A macro with no game argument reads every shipped game's baseline and
fails when they render differently. A macro naming an id absent from the
baseline, a game with no baseline, or a key name the translation table
`tools/mkdocs_surface_keys.toml` lacks fails. Keyboard names are SDL names
(comma lists in the binding); the table maps them case-insensitively to
the pymdownx.keys spelling the pages use.

Stdlib only (tomllib, else tomli), so the converter, the identity check and
the snippet check run without mkdocs; tools/mkdocs_surface.py is the mkdocs
hook over this module.
"""

import ast
import copy
import json
import re
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

ROOT = Path(__file__).resolve().parent.parent
SURFACE_DIR = ROOT / "packaging" / "surface"
DIST_TOML = ROOT / "packaging" / "openpete-dist.toml"
SHIPPED_TOML = ROOT / "games" / "shipped.toml"
KEYS_TOML = Path(__file__).resolve().parent / "mkdocs_surface_keys.toml"
USER_DIR = ROOT / "docs" / "src" / "user"

MACRO_RE = re.compile(r"\{\{\s*([a-z_]+)\((.*?)\)\s*\}\}")
MARKER_RE = re.compile(r"<!--\s*id:\s*(.*?)\s*-->")
KINDS = ("default", "key", "key_pad", "config_key", "flag")


class MacroError(Exception):
    pass


class Surface:
    """The baselines of the shipped games, the shipped toml and the key
    translation table: everything a macro reads."""

    def __init__(self, surface_dir=SURFACE_DIR, dist_toml=DIST_TOML,
                 shipped_toml=SHIPPED_TOML, keys_toml=KEYS_TOML):
        self.shipped = list(tomllib.loads(
            Path(shipped_toml).read_text())["games"])
        self.baselines = {}
        for game in self.shipped:
            path = Path(surface_dir) / f"{game}.json"
            if path.is_file():
                self.baselines[game] = json.loads(path.read_text())
        self.dist = tomllib.loads(Path(dist_toml).read_text())
        table = tomllib.loads(Path(keys_toml).read_text())["keys"]
        self.keys = {k.lower(): v for k, v in table.items()}
        self.index = {g: {e["id"]: e for e in b["entries"]}
                      for g, b in self.baselines.items()}

    def clone(self):
        """A deep copy, for a negative control that mutates one field."""
        twin = copy.copy(self)
        twin.baselines = copy.deepcopy(self.baselines)
        twin.index = {g: {e["id"]: e for e in b["entries"]}
                      for g, b in twin.baselines.items()}
        return twin

    # -- lookups ---------------------------------------------------------

    def games_for(self, game):
        """The baselines a macro reads: its game's, or every shipped one."""
        if game is not None:
            if game not in self.index:
                raise MacroError(f"no surface baseline for game '{game}' "
                                 f"(packaging/surface/{game}.json)")
            return [game]
        if not self.index:
            raise MacroError("no surface baseline for any shipped game")
        return [g for g in self.shipped if g in self.index]

    def entry(self, ident, game):
        e = self.index[game].get(ident)
        if e is None:
            raise MacroError(f"'{ident}' is not in the {game} surface "
                             f"baseline")
        return e

    def knob_of(self, ident, game):
        """The knob: entry an id names: itself, or a flag's registry row."""
        e = self.entry(ident, game)
        if ident.startswith("knob:"):
            return e
        if ident.startswith("flag:") and e.get("knob"):
            return self.index[game].get("knob:" + e["knob"])
        return None

    def scoped_toml(self, game):
        """The shipped toml's bare keys for a game: global, then
        [game.<game>] over them."""
        out = {k: v for k, v in self.dist.items()
               if not isinstance(v, dict)}
        gt = self.dist.get("game", {}).get(game, {})
        out.update({k: v for k, v in gt.items() if not isinstance(v, dict)})
        return out

    def scoped_binding(self, ident, game):
        """The shipped toml's binding of a key action for a game, as an SDL
        comma list, or None when it sets none."""
        domain, action = ident[len("key:"):].split(".", 1)
        val = None
        for scope in (self.dist, self.dist.get("game", {}).get(game, {})):
            v = scope.get("keys", {}).get(domain, {}).get(action)
            if v is not None:
                val = v
        if val is None:
            return None
        return ",".join(val) if isinstance(val, list) else str(val)

    # -- renderings ------------------------------------------------------

    def render_default(self, ident, game, scoped):
        if not ident.startswith(("knob:", "flag:")):
            raise MacroError(f"default() takes a knob: or flag: id, not "
                             f"'{ident}'")
        e = self.entry(ident, game)
        knob = self.knob_of(ident, game)
        if knob is not None and knob.get("config_key"):
            val = self.scoped_toml(game).get(knob["config_key"])
            if val is not None and scoped:
                return cli_word(val)
        if e.get("def") is None:
            raise MacroError(f"'{ident}' declares no def"
                             + (f" (default_note: {e['default_note']})"
                                if e.get("default_note") else ""))
        return e["def"]

    def binding(self, ident, game, scoped):
        if not ident.startswith("key:"):
            raise MacroError(f"key()/key_pad() take a key: id, not "
                             f"'{ident}'")
        e = self.entry(ident, game)
        b = self.scoped_binding(ident, game) if scoped else None
        return e, (b if b is not None else e["def"])

    def keycap(self, name):
        cap = self.keys.get(name.strip().lower())
        if cap is None:
            raise MacroError(f"SDL key name '{name}' is not in "
                             f"{KEYS_TOML.name}")
        return f"++{cap}++"

    def render_key(self, ident, game, n, scoped):
        e, b = self.binding(ident, game, scoped)
        if b == "none":
            if e.get("dev"):
                sug = keyboard_elems(e.get("suggested") or "")
                if not sug:
                    raise MacroError(f"'{ident}' is unbound and suggests no "
                                     f"keyboard key")
                return f"none ({self.keycap(sug[0])} when bound)"
            return "none"
        elems = keyboard_elems(b)
        if len(elems) < n:
            raise MacroError(f"'{ident}' binding '{b}' has no keyboard "
                             f"element {n}")
        return self.keycap(elems[n - 1])

    def render_key_pad(self, ident, game, n, scoped):
        _e, b = self.binding(ident, game, scoped)
        elems = [t.strip() for t in b.split(",") if
                 t.strip().startswith("pad:")]
        if len(elems) < n:
            raise MacroError(f"'{ident}' binding '{b}' has no pad element "
                             f"{n}")
        return f"`{elems[n - 1]}`"

    def render_config_key(self, ident, game):
        knob = self.knob_of(ident, game)
        if knob is None or not knob.get("config_key"):
            raise MacroError(f"'{ident}' has no config key")
        return f"`{knob['config_key']}`"

    def render_flag(self, ident, game):
        e = self.entry(ident, game)
        if not (ident.startswith(("flag:", "knob:")) and e.get("flag")):
            raise MacroError(f"'{ident}' has no flag")
        return f"`{e['flag']}`"

    def render_one(self, kind, ident, game, n, scoped):
        if kind == "default":
            return self.render_default(ident, game, scoped)
        if kind == "key":
            return self.render_key(ident, game, n, scoped)
        if kind == "key_pad":
            return self.render_key_pad(ident, game, n, scoped)
        if kind == "config_key":
            return self.render_config_key(ident, game)
        if kind == "flag":
            return self.render_flag(ident, game)
        raise MacroError(f"unknown macro '{kind}'")

    def render(self, kind, ident, game=None, n=1):
        """One macro's text. With no game, every shipped game's baseline
        must render the same text."""
        scoped = game is not None
        outs = {}
        for g in self.games_for(game):
            outs[g] = self.render_one(kind, ident, g, n, scoped)
        if len(set(outs.values())) > 1:
            raise MacroError(f"{kind}('{ident}') differs between shipped "
                             f"games: " + ", ".join(f"{g}={v!r}" for g, v in
                                                    outs.items())
                             + "; give the game argument")
        return next(iter(outs.values()))


def keyboard_elems(binding):
    """The keyboard sources of a binding; a `hold:` source is not one."""
    return [t.strip() for t in binding.split(",")
            if t.strip() and not t.strip().startswith(("pad:", "mouse:", "hold:"))
            and t.strip() != "none"]


def cli_word(val):
    """A shipped-toml value in the CLI vocabulary a user types."""
    if isinstance(val, bool):
        return "1" if val else "0"
    return str(val)


def macro_text(kind, ident, game=None, n=None):
    """The source spelling of one macro, as the converter writes it."""
    args = [json.dumps(ident)]
    if game is not None:
        args.append(json.dumps(game))
    if n is not None and n != 1:
        args.append(str(n))
    return "{{ %s(%s) }}" % (kind, ", ".join(args))


def parse_macro(m):
    """(kind, id, game, n) of a MACRO_RE match."""
    kind = m.group(1)
    if kind not in KINDS:
        raise MacroError(f"unknown macro '{kind}' in '{m.group(0)}'")
    try:
        args = ast.literal_eval("(" + m.group(2) + ",)")
    except (SyntaxError, ValueError) as exc:
        raise MacroError(f"malformed macro '{m.group(0)}': {exc}") from None
    ident, game, n = args[0], None, 1
    for a in args[1:]:
        if isinstance(a, str) and game is None:
            game = a
        elif isinstance(a, int) and not isinstance(a, bool):
            n = a
        else:
            raise MacroError(f"malformed macro '{m.group(0)}'")
    if not isinstance(ident, str) or n < 1:
        raise MacroError(f"malformed macro '{m.group(0)}'")
    if kind in ("config_key", "flag") and (game is not None or n != 1):
        raise MacroError(f"{kind}() takes the id only: '{m.group(0)}'")
    if kind == "default" and n != 1:
        raise MacroError(f"default() takes no element index: "
                         f"'{m.group(0)}'")
    return kind, ident, game, n


def expand(text, surface, where="page"):
    """The page text with every macro replaced by its rendering. Raises
    MacroError naming the page line on a bad macro."""
    out = []
    for i, line in enumerate(text.split("\n")):
        def sub(m, i=i):
            try:
                kind, ident, game, n = parse_macro(m)
                return surface.render(kind, ident, game, n)
            except MacroError as exc:
                raise MacroError(f"{where}:{i + 1}: {exc}") from None
        out.append(MACRO_RE.sub(sub, line) if "{{" in line else line)
    return "\n".join(out)


def count_macros(text):
    return len(MACRO_RE.findall(text))


def page_game(rel):
    """The game scope of a page path relative to docs/src/user/."""
    parts = Path(rel).parts
    if len(parts) == 2 and parts[0] == "games":
        return Path(rel).stem
    return None
