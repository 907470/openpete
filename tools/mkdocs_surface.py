"""mkdocs_surface.py - mkdocs hook that expands the surface macros of the
User pages and generates the User reference pages.

Macros (tools/surface_macros.py) are expanded on every page under user/;
a macro that names an id absent from the baseline, a game with no
baseline, or a value the shipped games' baselines disagree on fails the
build. The baselines are packaging/surface/<game>.json for each game in
games/shipped.toml, overlaid by the game's scope of
packaging/openpete-dist.toml.

Generated pages, added in on_files as File.generated (no file is written):

  user/reference/command-line.md   every flag a shipped build accepts
  user/reference/config-keys.md    every openpete.toml key
  user/reference/key-actions.md    every key action of the host and pad
                                   domains
  user/reference/changelog.md      CHANGELOG.md without its Unreleased
                                   section, site-absolute /docs/ links made
                                   relative

An entry is listed when the baseline places it in the shipped build: a
flag whose help-only row is in the release or preamble block, or whose
registry row is in the release block, with no build condition; a config
key whose registry row is in the release block or is reached by a
displayed settings-menu row; a key action that is not a development row
or that the shipped toml binds. Each entry links the page the docs map
(docs/src/user/pages.toml) resolves it to.

This hook is listed before tools/mkdocs_sections.py, whose on_files drops
the generated pages from a build without the user section.
"""

import posixpath
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import surface_macros as sm  # noqa: E402

from mkdocs.exceptions import PluginError  # noqa: E402

CHANGELOG = sm.ROOT / "CHANGELOG.md"
PAGES_TOML = sm.USER_DIR / "pages.toml"
REF_DIR = "user/reference"

_state = {}


def on_config(config):
    """Load the baselines, the shipped toml, the key table and the docs map."""
    try:
        _state["surface"] = sm.Surface()
        _state["map"] = sm.tomllib.loads(PAGES_TOML.read_text())
    except (OSError, KeyError, ValueError) as exc:
        raise PluginError(f"surface: cannot load the surface baseline: "
                          f"{exc}") from None
    if not _state["surface"].index:
        raise PluginError("surface: no baseline in packaging/surface/ for "
                          "any game in games/shipped.toml")
    return config


def on_files(files, config):
    """Add the generated reference pages under user/reference/."""
    from mkdocs.structure.files import File

    try:
        pages = reference_pages(_state["surface"], _state["map"])
    except sm.MacroError as exc:
        raise PluginError(f"surface: {exc}") from None
    for name, text in pages.items():
        files.append(File.generated(config, f"{REF_DIR}/{name}",
                                    content=text))
    return files


def on_page_markdown(markdown, page, config, files):
    """Expand the macros of a User page."""
    uri = page.file.src_uri
    if not uri.startswith("user/"):
        return markdown
    try:
        return sm.expand(markdown, _state["surface"], uri[len("user/"):])
    except sm.MacroError as exc:
        raise PluginError(f"surface: {exc}") from None


# -- reference pages ---------------------------------------------------


def doc_link(dmap, ident, alt=None):
    """Markdown link to the docs-map entry of an id (or of alt, the flag or
    knob it resolves through), relative to user/reference/; '' when none."""
    for i in (ident, alt):
        if not i:
            continue
        for scope in [dmap.get("game", {}).get(g, {})
                      for g in _state["surface"].shipped] + [dmap]:
            target = scope.get(i)
            if isinstance(target, str):
                page, _, anchor = target.partition("#")
                title = posixpath.splitext(posixpath.basename(page))[0]
                href = "../" + page + ("#" + anchor if anchor else "")
                return f"[{title.replace('-', ' ')}]({href})"
    return ""


def per_game(surface, fn):
    """fn(game) for each shipped game with a baseline, folded to one text
    when they agree, else `game: text` joined."""
    vals = {g: fn(g) for g in surface.shipped if g in surface.index}
    if len(set(vals.values())) <= 1:
        return next(iter(vals.values()), "")
    return "; ".join(f"{g}: {v}" for g, v in vals.items())


def cell(text):
    """Free text as one table cell: pipes escaped, < and > as entities."""
    return ((text or "").replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace("|", "\\|").replace("\n", " "))


def code(text):
    return f"`{text}`" if text not in (None, "") else ""


def default_cell(surface, knob_entry, e, game):
    """The default of an entry: def or default_note, and the shipped toml's
    value where it differs."""
    built = code(e.get("def")) if e.get("def") is not None else \
        cell(e.get("default_note") or "")
    if knob_entry is not None and knob_entry.get("config_key"):
        val = surface.scoped_toml(game).get(knob_entry["config_key"])
        if val is not None and sm.cli_word(val) != e.get("def"):
            return f"{code(sm.cli_word(val))} (shipped); built-in {built}"
    return built


def values_cell(k):
    if k is None:
        return ""
    if k.get("vocab"):
        return ", ".join(code(v) for v in k["vocab"])
    if k.get("range"):
        lo, hi = k["range"]
        return f"{lo} to {hi}"
    return k.get("schema_type") or k.get("type") or ""


def flag_shipped(e, idx):
    if e.get("build_cond"):
        return False
    if "block" in e:
        return e["block"] in ("release", "preamble", "none")
    k = idx.get("knob:" + e["knob"]) if e.get("knob") else None
    return k is not None and k["block"] == "release" and not k["build_cond"]


def command_line_page(surface, dmap):
    g0 = next(g for g in surface.shipped if g in surface.index)
    idx = surface.index[g0]
    rows = []
    for e in surface.baselines[g0]["entries"]:
        if not e["id"].startswith("flag:") or not flag_shipped(e, idx):
            continue
        k = idx.get("knob:" + e["knob"]) if e.get("knob") else None
        if "alias_value" in e:
            arg = f"sets {code(k['flag'] if k else '')} " \
                  f"{code(e['alias_value'])}"
        else:
            arg = {"required": "value", "optional": "optional value",
                   "none": ""}.get(e.get("takes_arg"), "")
            vals = values_cell(k) if arg else ""
            arg = f"{arg}: {vals}" if vals and k and (k.get("vocab") or
                                                       k.get("range")) \
                else arg
        dflt = per_game(surface, lambda g, e=e: default_cell(
            surface, surface.knob_of(e["id"], g),
            surface.index[g][e["id"]], g)) if "alias_value" not in e else ""
        rows.append((e["flag"], arg, code(k.get("config_key")) if k else "",
                     dflt, doc_link(dmap, e["id"],
                                    "knob:" + e["knob"] if e.get("knob")
                                    else None)))
    rows.sort(key=lambda r: r[0].lstrip("-"))
    out = ["# Command line", "",
           "Every flag a shipped build accepts, typed after the "
           "executable's name on the command line. A flag with a "
           "config key can also be set in `openpete.toml`; see "
           "[Config file](../customizing/config-file.md).", "",
           "| Flag | Argument | Config key | Default | Page |",
           "| --- | --- | --- | --- | --- |"]
    for f, arg, ck, d, link in rows:
        out.append(f"| `{f}` | {cell(arg)} | {ck} | {d} | {link} |")
    return "\n".join(out) + "\n"


def knob_shipped(e, menu_knobs):
    if e.get("build_cond") or not e.get("config_key"):
        return False
    return e["block"] == "release" or e["id"][len("knob:"):] in menu_knobs


def config_keys_page(surface, dmap):
    g0 = next(g for g in surface.shipped if g in surface.index)
    entries = surface.baselines[g0]["entries"]
    menu_knobs = {e["knob"] for e in entries
                  if e["id"].startswith("menu:") and e.get("displayed")
                  and e.get("knob")}
    rows = []
    for e in entries:
        if not e["id"].startswith("knob:") or not knob_shipped(e, menu_knobs):
            continue
        dflt = per_game(surface, lambda g, e=e: default_cell(
            surface, surface.index[g][e["id"]], surface.index[g][e["id"]], g))
        aliases = ", ".join(code(a) for a in e.get("aliases") or [])
        rows.append((e["config_key"], values_cell(e), dflt,
                     code(e.get("flag")), aliases,
                     doc_link(dmap, e["id"],
                              "flag:" + e["flag"] if e.get("flag")
                              else None)))
    rows.sort()
    out = ["# Config keys", "",
           "Every key of `openpete.toml`. A key is written bare at the top "
           "of the file or under a `[game.ID]` or `[disc.SERIAL]` table; "
           "see [Config file](../customizing/config-file.md).", "",
           "| Key | Values | Default | Flag | Aliases | Page |",
           "| --- | --- | --- | --- | --- | --- |"]
    for ck, vals, d, f, al, link in rows:
        out.append(f"| `{ck}` | {cell(vals)} | {d} | {f} | {al} | {link} |")
    return "\n".join(out) + "\n"


def binding_cell(surface, b):
    if b in (None, "", "none"):
        return "none"
    parts = []
    for t in (x.strip() for x in b.split(",")):
        if t.startswith(("pad:", "mouse:", "hold:")):
            parts.append(code(t))
        else:
            parts.append(surface.keycap(t))
    return ", ".join(parts)


def key_actions_page(surface, dmap):
    g0 = next(g for g in surface.shipped if g in surface.index)
    out = ["# Key actions", "",
           "Every action a key, pad button or mouse input can be bound to, "
           "with its default binding. Bindings are changed in the "
           "`[keys.host]` and `[keys.pad]` tables of `openpete.toml`; see "
           "[Key bindings](../customizing/key-bindings.md). `pad2` to "
           "`pad4` take the `pad` actions."]
    extra = {e["def"] for e in surface.baselines[g0]["entries"]
             if re.match(r"key:pad[2-4]\.", e["id"])}
    if extra == {"none"}:
        out[-1] = out[-1][:-1] + "; every one is unbound by default."
    for domain in ("host", "pad"):
        out += ["", f"## {domain}", "",
                "| Action | Default | Page |", "| --- | --- | --- |"]
        for e in surface.baselines[g0]["entries"]:
            if not e["id"].startswith(f"key:{domain}."):
                continue
            shipped_b = {g: surface.scoped_binding(e["id"], g)
                         for g in surface.shipped if g in surface.index}
            if e["dev"] and not any(b not in (None, "none")
                                    for b in shipped_b.values()):
                continue

            def text(g, e=e):
                b = shipped_b[g]
                if b is not None:
                    return (binding_cell(surface, b) + " (shipped); built-in "
                            + binding_cell(surface,
                                           surface.index[g][e["id"]]["def"]))
                return binding_cell(surface, surface.index[g][e["id"]]["def"])
            action = e["id"][len(f"key:{domain}."):]
            out.append(f"| `{action}` | {per_game(surface, text)} | "
                       f"{doc_link(dmap, e['id'])} |")
    return "\n".join(out) + "\n"


ABS_LINK = re.compile(r"\]\(/docs/([^)#\s]*?)(?:\.html)?(#[^)\s]*)?\)")


def changelog_page():
    """CHANGELOG.md without `## Unreleased` and without its HTML comments
    (the format note for the website's generator), its /docs/ links
    relative."""
    out, skip = [], False
    source = re.sub(r"<!--.*?-->\n*", "", CHANGELOG.read_text(), flags=re.S)
    for line in source.split("\n"):
        if line.startswith("## "):
            skip = line.strip() == "## Unreleased"
        if not skip:
            out.append(line)
    text = "\n".join(out)
    return ABS_LINK.sub(lambda m: "](../../%s.md%s)" % (m.group(1),
                                                        m.group(2) or ""),
                        text)


def reference_pages(surface, dmap):
    return {
        "command-line.md": command_line_page(surface, dmap),
        "config-keys.md": config_keys_page(surface, dmap),
        "key-actions.md": key_actions_page(surface, dmap),
        "changelog.md": changelog_page(),
    }
