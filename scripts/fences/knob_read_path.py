#!/usr/bin/env python3
"""Fence: every registered OPENPETE_* variable is read through one path.

A variable reaches the program from the process environment or from the
config file's [env] tables. The game-neutral stage keeps its [env] winners
in memory (core/config/config.c), so before the full stage writes them with
setenv only the config path sees them:

  config  openpete_config_env, openpete_config_device_value, rhi_knob (the
          RHI's device-creation source, set to openpete_config_device_value),
          and every function or macro that passes its name parameter to one
          of them
  raw     getenv, secure_getenv, SDL_getenv, SDL_getenv_unsafe, and every
          function or macro that passes its name parameter to one of them

A variable read by a raw reader and a config reader has two sources of
truth: the two agree only while every raw read happens after the full
stage, and a raw read moved earlier (a launcher path, a first-use latch hit
during identification) loses the config file's value without an error. The
fence refuses such a variable unless ALLOW names it with the reason.

Registered names: the `.env` members and ENVROW_* rows of
core/config/knob_registry.c and the OPENPETE_* literals of
core/config/config_schema.c. Read sites: each string literal equal to a
registered name in the C and C++ sources under core/ and games/ (comments
stripped; generated/ trees and the decomp checkouts games/<g>/<g>/ skipped),
classified by the innermost call whose argument list holds it. A call of
neither family (a table, a log string, setenv) is not a read.

Exit 0 clean, 1 refused (one REFUSED line per variable, naming each read
site and its family) or VACUOUS (no registered name, or no read site of
either family), 2 usage.

Usage: knob_read_path.py [--root DIR]
"""
import argparse
import os
import re
import sys

CONFIG_SEEDS = {"openpete_config_env", "openpete_config_device_value", "rhi_knob"}
RAW_SEEDS = {"getenv", "secure_getenv", "SDL_getenv", "SDL_getenv_unsafe"}

# Variables read through both families on purpose, each with the reason the
# raw read cannot take the config path. An entry whose variable is no longer
# split is refused, so the list holds only live exceptions.
ALLOW = {
    "OPENPETE_LOG": "core/diag/log.c compiles into unit tests without the "
                    "config layer; every boot call of openpete_log_init passes "
                    "the openpete_config_env value as level_spec, which "
                    "overrides its getenv",
}

SRC_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "defined"}


def strip_comments(text):
    """Return text with C and C++ comments blanked, string and char literals kept, offsets preserved."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_text(path):
    """Return the text of the file at path, undecodable bytes replaced."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def registered(root):
    """Return the registered OPENPETE_* names: knob_registry.c rows and config_schema.c literals."""
    names = set()
    reg = os.path.join(root, "core/config/knob_registry.c")
    sch = os.path.join(root, "core/config/config_schema.c")
    if os.path.exists(reg):
        t = strip_comments(read_text(reg))
        names |= set(re.findall(r'\.env\s*=\s*"(OPENPETE_\w+)"', t))
        names |= {"OPENPETE_" + m for m in re.findall(r'\bENVROW_\w+\(\s*"(\w+)"', t)}
    if os.path.exists(sch):
        t = strip_comments(read_text(sch))
        names |= set(re.findall(r'"(OPENPETE_\w+)"', t))
    return names


def sources(root):
    """Yield (relative path, comment-stripped text) of each C/C++ source under core/ and games/."""
    for top in ("core", "games"):
        base = os.path.join(root, top)
        for dp, dn, fn in os.walk(base):
            rel = os.path.relpath(dp, root).replace(os.sep, "/")
            parts = rel.split("/")
            if "generated" in parts or (len(parts) == 3 and parts[0] == "games" and parts[1] == parts[2]):
                dn[:] = []
                continue
            dn.sort()
            for f in sorted(fn):
                if f.endswith(SRC_EXT):
                    p = os.path.join(dp, f)
                    yield (os.path.relpath(p, root).replace(os.sep, "/"),
                           strip_comments(read_text(p)))


def enclosing_call(text, pos):
    """Return the name of the innermost call whose argument list holds offset pos, or None."""
    depth = 0
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                m = re.search(r"(\w+)\s*$", text[max(0, i - 80):i])
                if m and m.group(1) not in KEYWORDS:
                    return m.group(1)
                return None
            depth -= 1
        elif c in ";{}" and depth == 0:
            return None
        i -= 1
    return None


DEF_RE = re.compile(r"(?:^|[\s*&])(\w+)\s*\(([^()]*)\)\s*(?:const\s*)?\{", re.M)
MACRO_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^()]*)\)((?:[^\n]*\\\n)*[^\n]*)", re.M)


def body_at(text, open_brace):
    """Return the text of the brace block opening at offset open_brace."""
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i + 1]
    return text[open_brace:]


def wrappers(texts):
    """Return [(name, params, body)] for every function definition and function-like macro."""
    out = []
    for t in texts:
        for m in DEF_RE.finditer(t):
            if m.group(1) in KEYWORDS:
                continue
            params = [p.strip().split()[-1].lstrip("*&") for p in m.group(2).split(",")
                      if "char" in p and p.strip().split()]
            if params:
                out.append((m.group(1), params, body_at(t, m.end() - 1)))
        for m in MACRO_RE.finditer(t):
            params = [p.strip() for p in m.group(2).split(",") if p.strip()]
            out.append((m.group(1), params, m.group(3)))
    return out


def families(texts):
    """Return {callee: 'config'|'raw'}: the seeds, then every wrapper passing a name parameter to a classified callee, to a fixed point."""
    fam = {n: "config" for n in CONFIG_SEEDS}
    fam.update({n: "raw" for n in RAW_SEEDS})
    defs = wrappers(texts)
    changed = True
    while changed:
        changed = False
        for name, params, body in defs:
            if name in CONFIG_SEEDS or name in RAW_SEEDS or fam.get(name) == "config":
                continue
            got = set()
            for p in params:
                pat = r"(\w+)\s*\(\s*(?:\w+\s*,\s*)?" + re.escape(p) + r"\s*[,)]"
                got |= {fam[m.group(1)] for m in re.finditer(pat, body) if m.group(1) in fam}
            f = "config" if "config" in got else "raw" if "raw" in got else None
            if f and fam.get(name) != f:
                fam[name] = f
                changed = True
    return fam


def scan(root):
    """Return (names, sites, split): sites maps each name to [(path:line, callee, family)]; split lists the names read by both families."""
    names = registered(root)
    files = list(sources(root))
    fam = families([t for _, t in files])
    lit = re.compile(r'"(OPENPETE_\w+)"')
    sites = {}
    for path, t in files:
        for m in lit.finditer(t):
            name = m.group(1)
            if name not in names:
                continue
            callee = enclosing_call(t, m.start())
            f = fam.get(callee)
            if f:
                line = t.count("\n", 0, m.start()) + 1
                sites.setdefault(name, []).append(("%s:%d" % (path, line), callee, f))
    split = sorted(n for n, s in sites.items() if {f for _, _, f in s} == {"config", "raw"})
    return names, sites, split


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    a = ap.parse_args(argv)
    names, sites, split = scan(a.root)
    nsites = sum(len(s) for s in sites.values())
    if not names or not nsites:
        print("knob_read_path: VACUOUS: %d registered names, %d read sites" % (len(names), nsites))
        return 1
    bad = 0
    for n in split:
        if n in ALLOW:
            continue
        bad += 1
        print("knob_read_path: REFUSED %s is read through both families: %s; read it through "
              "openpete_config_env everywhere (rhi_knob inside the RHI)" % (
                  n, ", ".join("%s %s (%s)" % s for s in sites[n])))
    for n in sorted(ALLOW):
        if n not in split:
            bad += 1
            print("knob_read_path: REFUSED stale ALLOW entry %s: no longer read through both families" % n)
    print("knob_read_path: %d registered names, %d read sites, %d split (%d allowed): %s" % (
        len(names), nsites, len(split), len([n for n in split if n in ALLOW]),
        "REFUSED" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
