"""mkdocs_sections.py - mkdocs hook that builds a subset of the site's sections.

A section is a top-level directory of the docs_dir holding at least one
page (user/, modding/, developer/, spyro/). OPENPETE_DOCS_SECTIONS names
the sections to build, comma-separated; unset or empty builds all of them,
and a name that is not a section fails the build. A left-out section loses
its files (pages and images), its nav entries and so its search records.
Top-level pages and directories without pages (assets/) always build.

A markdown link on a built page whose target lies in a left-out section is
rewritten: to an absolute URL under extra.published_url when
extra.published_sections lists the section, else to its link text. A
subset build therefore has no link to a missing page and passes --strict.
Links inside fenced code blocks are left alone.
"""

import os
import posixpath
import re

from mkdocs.exceptions import PluginError

ENV = "OPENPETE_DOCS_SECTIONS"
LINK = re.compile(r"(?<!!)\[([^\]\n]*)\]\(([^)\s]+)\)")
FENCE = re.compile(r"^\s*(```|~~~)")

_dropped = set()


def _sections(docs_dir):
    """Top-level directories of docs_dir that hold at least one page."""
    found = set()
    for name in os.listdir(docs_dir):
        top = os.path.join(docs_dir, name)
        if not os.path.isdir(top):
            continue
        for _root, _dirs, files in os.walk(top):
            if any(f.endswith(".md") for f in files):
                found.add(name)
                break
    return found


def _keep_nav(item):
    """The nav entry (path, URL, list or one-key dict) without the dropped
    sections, or None when nothing of it remains."""
    if isinstance(item, str):
        if "://" in item:
            return item
        return None if item.split("/", 1)[0] in _dropped else item
    if isinstance(item, list):
        kept = [k for k in (_keep_nav(i) for i in item) if k is not None]
        return kept or None
    if isinstance(item, dict):
        out = {}
        for title, value in item.items():
            kept = _keep_nav(value)
            if kept is not None:
                out[title] = kept
        return out or None
    return item


def on_config(config):
    """Resolve OPENPETE_DOCS_SECTIONS into the dropped set; prune the nav."""
    _dropped.clear()
    raw = os.environ.get(ENV, "")
    wanted = {s.strip() for s in raw.split(",") if s.strip()}
    if not wanted:
        return config
    known = _sections(config["docs_dir"])
    unknown = wanted - known
    if unknown:
        raise PluginError("%s names %s; the sections are %s" % (
            ENV, ", ".join(sorted(unknown)), ", ".join(sorted(known))))
    _dropped.update(known - wanted)
    if config["nav"] is not None:
        config["nav"] = _keep_nav(config["nav"]) or []
    return config


def on_files(files, config):
    """Remove every file under a dropped section from the build."""
    if not _dropped:
        return files
    for f in list(files):
        if f.src_uri.split("/", 1)[0] in _dropped:
            files.remove(f)
    return files


def _rewrite(match, page_dir, config):
    """Replacement for one LINK match on a page in page_dir: unchanged,
    re-pointed at the published URL, or the link text."""
    text, target = match.group(1), match.group(2)
    if "://" in target or target.startswith(("#", "/", "mailto:")):
        return match.group(0)
    path, _, frag = target.partition("#")
    resolved = posixpath.normpath(posixpath.join(page_dir, path))
    section = resolved.split("/", 1)[0]
    if section not in _dropped:
        return match.group(0)
    extra = config["extra"]
    if section not in extra.get("published_sections", []):
        return text
    if resolved.endswith(".md"):
        resolved = resolved[:-3] + ".html"
    url = extra["published_url"] + resolved + ("#" + frag if frag else "")
    return "[%s](%s)" % (text, url)


def on_page_markdown(markdown, page, config, files):
    """Rewrite this page's links into dropped sections, outside fences."""
    if not _dropped:
        return markdown
    page_dir = posixpath.dirname(page.file.src_uri)
    out, fence = [], None
    for line in markdown.split("\n"):
        m = FENCE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)
            elif m.group(1) == fence:
                fence = None
            out.append(line)
            continue
        if fence is None:
            line = LINK.sub(lambda mt: _rewrite(mt, page_dir, config), line)
        out.append(line)
    return "\n".join(out)
