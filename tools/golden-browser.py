#!/usr/bin/env python3
"""golden-browser.py -- a local web page over the presentation golden.

The golden's verdicts are digest rows in blessed.tsv manifests and its images
live in the reference store, so no directory of PNGs exists to open in an image
viewer. This page reads the manifests, the store, the archive and candidate
directories, and shows them as images. Three views:

  current    every row of a game tier's blessed.tsv at the working tree as a
             thumbnail grid: tile name, corpus row, VBL, leg, LOD; filtered by
             substring (q) and by leg.
  candidate  a candidate directory against its tier's manifest: one row per
             divergent tile with the differing pixel count, the maximum
             channel delta and the bounding box from diff_images in
             tests/harness/presentation-corpus-diff.py, sorted by pixel count
             descending; filtered by a pixel floor, by leg and by corpus row;
             each row opens a reference | candidate | heat panel and carries a
             checkbox that adds its tile name to the promote list.
  history    two revisions (a, default HEAD~1; b, default HEAD) resolved with
             golden-at's tiers (store, archive, materialize price): the rows
             whose digest differs with the same panel, and the rows one side
             lacks as added or removed. A row neither the store nor the archive
             holds shows its materialize price and the golden-at command; the
             page runs nothing.

The page writes one output, the promote list: one tile name per line in
<promote-dir>/promote-<game>-<tier>.txt, which
tests/harness/presentation-corpus-promote.sh takes as its tile arguments. It
also fills two caches in golden-ref-thumbs/, the sibling of the store in the
directory holding golden-ref.state: thumbnails keyed by verdict digest and heat
images keyed by the digest pair. A store blob whose pixels do not hash to its
name is discarded on read, which is the store's own contract
(presentation-blessed.py store_get).

The server binds 127.0.0.1 and serves only files a rendered view registered.
Its only child processes are the read-only `git cat-file --batch` and
`git rev-parse` calls presentation-blessed.py makes to read a revision.

Usage:
  tools/golden-browser.py [--port N] [--promote-dir DIR]
  tools/golden-browser.py --once VIEW --out FILE [--game G] [--tier T]
        [--cand DIR] [--a REV] [--b REV] [--q S] [--leg L] [--floor N]
        [--row S]
--once renders one view to a static HTML file with file:// images (thumbnails
generated eagerly) and writes FILE.rows.tsv, the listed rows, for a headless
check.
"""
import argparse
import html
import importlib.util
import io
import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

try:
    from PIL import Image
except ModuleNotFoundError:
    print("[golden-browser] python3-PIL missing", file=sys.stderr)
    sys.exit(1)

# The page writes nothing into the checkout, bytecode caches included.
sys.dont_write_bytecode = True

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HOST = "127.0.0.1"
THUMB_W = 192


def _load(name, rel):
    """Import a hyphen-named script as a module."""
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# The diff script loads presentation-blessed.py itself; its module instance is
# reused so the digest helper and the store are one implementation.
cdiff = _load("presentation_corpus_diff",
              "tests/harness/presentation-corpus-diff.py")
pb = cdiff.pb


# ---- tile metadata ----------------------------------------------------------

def tile_meta(tile):
    """Corpus row, leg, LOD and VBL of a tile basename of the form
    <row>[__lodext]__<leg>__<vbl>-vk.png."""
    stem = tile[:-len("-vk.png")] if tile.endswith("-vk.png") else tile
    parts = stem.split("__")
    row = parts[0]
    vbl = parts[-1] if len(parts) >= 3 else ""
    leg = parts[-2] if len(parts) >= 3 else ""
    lod = "extended" if "lodext" in parts[1:-2] else "original"
    return {"row": row, "leg": leg, "lod": lod, "vbl": vbl}


def tiers():
    """Every (game, tier) pair that holds a manifest."""
    return [(g, t) for t, gs in sorted(pb.TIERS.items()) for g in gs]


def default_candidate(game, tier):
    """The tier's kept-candidate directory, as scripts/presentation-game.sh
    names it."""
    base = "build/presentation-candidate" if tier == "presentation" \
        else "build/dragon-candidate"
    return base if game == "spyro-1" else "%s-%s" % (base, game)


def thumbs_dir():
    """golden-ref-thumbs/, the sibling of the store."""
    return os.path.join(os.path.dirname(pb.store_dir()), "golden-ref-thumbs")


# ---- rendering context --------------------------------------------------------

class Ctx:
    """Maps images to URLs. Served: an opaque token per registered file, so
    the server serves nothing a view did not register. Static: file:// paths,
    in-memory images written beside the HTML."""

    def __init__(self, static_dir=None):
        self.static_dir = static_dir
        self.files = {}
        self.by_path = {}

    def file(self, path):
        """URL of a file on disk."""
        path = os.path.abspath(path)
        if self.static_dir is not None:
            return "file://" + urllib.parse.quote(path)
        tok = self.by_path.get(path)
        if tok is None:
            tok = "%x" % len(self.files)
            self.files[tok] = path
            self.by_path[path] = tok
        return "/f/" + tok

    def blob(self, data, name):
        """URL of in-memory image bytes (an archive read)."""
        d = self.static_dir or os.path.join(thumbs_dir(), "archive")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, name)
        with open(p, "wb") as fh:
            fh.write(data)
        return self.file(p)

    def src(self, source, name):
        """URL of a reference source: a path or a BytesIO."""
        if isinstance(source, io.BytesIO):
            return self.blob(source.getvalue(), name)
        return self.file(source)

    def thumb(self, digest):
        """URL of a store blob's thumbnail. Served, it is built on request;
        static, it is built now, or None when the store lacks the blob."""
        if self.static_dir is None:
            return "/thumb/" + digest
        p = make_thumb(digest)
        return self.file(p) if p else None


def make_thumb(digest):
    """Path of the cached thumbnail of a store blob, built from the verified
    blob on a miss; None when the store lacks it."""
    out = os.path.join(thumbs_dir(), digest + ".png")
    if os.path.isfile(out):
        return out
    src = pb.store_get(digest)
    if not src:
        return None
    with Image.open(src) as im:
        im = im.convert("RGB")
        h = max(1, round(im.height * THUMB_W / im.width))
        im = im.resize((THUMB_W, h), Image.BILINEAR)
    os.makedirs(thumbs_dir(), exist_ok=True)
    tmp = out + ".tmp%d" % os.getpid()
    im.save(tmp, format="PNG")
    os.replace(tmp, out)
    return out


# ---- data: current set ----------------------------------------------------------

def load_manifest(game, tier):
    """The working tree's manifest of a tier, or (None, error)."""
    try:
        return pb.load_manifest(game, tier), None
    except (OSError, ValueError) as e:
        return None, str(e)


def current_rows(p):
    """Rows of the current view after its filters."""
    m, err = load_manifest(p["game"], p["tier"])
    if m is None:
        return [], err
    out = []
    for r in m.rows:
        meta = tile_meta(r["tile"])
        if p["q"] and p["q"] not in r["tile"]:
            continue
        if p["leg"] and meta["leg"] != p["leg"]:
            continue
        out.append(dict(r, **meta))
    return out, None


# ---- data: candidate review -------------------------------------------------------

_DIGEST_CACHE = {}


def candidate_digests(paths):
    """Verdict digests of candidate tiles, cached by path, mtime and size."""
    def key(pth):
        st = os.stat(pth)
        return (pth, st.st_mtime_ns, st.st_size)
    todo = [pth for pth in paths if key(pth) not in _DIGEST_CACHE]
    if todo:
        got = pb.digest_files(todo)
        for pth in todo:
            _DIGEST_CACHE[key(pth)] = got[pth]
    return {pth: _DIGEST_CACHE[key(pth)] for pth in paths}


def heat_path(a, b):
    """Cached heat image path of a digest pair."""
    return os.path.join(thumbs_dir(), "heat", "%s_%s.png" % (a[:16], b[:16]))


def pixel_diff(ref_src, cand_src, a_dig, b_dig):
    """diff_images over two sources, the heat written into the cache. Returns
    dict(pixels, maxch, bbox, size, heat)."""
    heat = heat_path(a_dig, b_dig)
    if isinstance(ref_src, io.BytesIO):
        ref_src.seek(0)
    if isinstance(cand_src, io.BytesIO):
        cand_src.seek(0)
    res = cdiff.diff_images(ref_src, cand_src, heat)
    if res[0] == "size":
        return {"size": "%dx%d->%dx%d" % (res[1] + res[2])}
    return {"pixels": res[1], "maxch": res[2], "bbox": res[3],
            "heat": heat if res[1] else None}


def candidate_rows(p, ctx):
    """Divergent rows of a candidate directory against its manifest, each
    with its diff and panel sources. Returns (rows, summary, error)."""
    cand = p["cand"] or default_candidate(p["game"], p["tier"])
    cand = os.path.join(ROOT, cand) if not os.path.isabs(cand) else cand
    p["cand"] = cand
    if not os.path.isdir(cand):
        return [], None, "no candidate directory %s" % cand
    m, err = load_manifest(p["game"], p["tier"])
    if m is None:
        return [], None, err
    manifest = {r["tile"]: r for r in m.rows}
    tiles = set(pb.list_tiles(cand))
    common = sorted(tiles & set(manifest))
    digs = candidate_digests([os.path.join(cand, t) for t in common])
    tdir = pb.tier_dir(p["game"], p["tier"])
    arch = None
    rows = []
    for t in common:
        path = os.path.join(cand, t)
        got, row = digs[path], manifest[t]
        if got is not None and got[0] == row["digest"]:
            continue
        ent = dict(row, **tile_meta(t))
        ent["cand_digest"] = got[0] if got else None
        ent["cand_src"] = ctx.file(path)
        if got is None:
            ent["note"] = "undecodable candidate tile"
            rows.append(ent)
            continue
        if arch is None:
            arch = pb.Archive()
        src, where = pb.resolve_reference(row, tdir, arch)
        if src is None:
            ent["note"] = "hex mismatch %s->%s; reference in neither store " \
                "nor archive (golden-at %s %s %s)" % (
                    row["digest"][:12], got[0][:12], row["revision"][:12],
                    p["game"], t)
        else:
            ent["where"] = where
            ent["ref_src"] = ctx.src(src, row["digest"] + ".png")
            ent.update(pixel_diff(src, path, row["digest"], got[0]))
            if ent.get("heat"):
                ent["heat_src"] = ctx.file(ent["heat"])
        rows.append(ent)
    if arch is not None:
        arch.close()
    summary = {"compared": len(common), "divergent": len(rows),
               "missing": len(set(manifest) - tiles),
               "extra": len(tiles - set(manifest))}
    rows.sort(key=lambda e: (0 if "pixels" in e else 1,
                             -e.get("pixels", 0), e["tile"]))
    return filter_divergent(rows, p), summary, None


def filter_divergent(rows, p):
    """Apply the pixel floor (to rows that carry a count), leg and row
    filters."""
    out = []
    for e in rows:
        if p["floor"] and "pixels" in e and e["pixels"] < p["floor"]:
            continue
        if p["leg"] and e["leg"] != p["leg"]:
            continue
        if p["row"] and p["row"] not in e["row"]:
            continue
        out.append(e)
    return out


# ---- data: history compare --------------------------------------------------------

class RevReader:
    """Reads manifests at revisions of this repository, else of the
    archive (through its commit map)."""

    def __init__(self):
        self.local = pb.CatFile(ROOT)
        self.arch = pb.Archive()

    def resolve(self, rev):
        """(reader, sha) of a revision, or (None, None)."""
        sha = pb.resolve(ROOT, rev)
        if sha:
            return self.local, sha
        sha = self.arch.rev(rev) if self.arch.path else None
        return (self.arch.cf, sha) if sha else (None, None)

    def close(self):
        """End both readers."""
        self.local.close()
        self.arch.close()


def manifest_at(cf, sha, mpath):
    """{tile: row} of a manifest at a revision, or None when absent."""
    data = cf.read("%s:%s" % (sha, mpath))
    if data is None:
        return None
    try:
        m = pb.parse_manifest(data.decode(errors="replace"), mpath)
    except ValueError:
        return None
    return {r["tile"]: r for r in m.rows}


def locate(row, tdir, rr, rev, game, verify):
    """golden-at's tiers for one row: ("store", path), ("archive", BytesIO)
    or ("materialize", text). An unverified lookup checks store presence
    only (the thumbnail verifies on build)."""
    if verify:
        p = pb.store_get(row["digest"])
    else:
        p = pb.blob_path(row["digest"])
        p = p if os.path.isfile(p) else None
    if p:
        return "store", p
    spec = pb.archive_tile(rr.arch, row, tdir)
    if spec:
        return "archive", io.BytesIO(rr.arch.cf.read(spec))
    return "materialize", "%s; scripts/presentation-blessed.py golden-at %s " \
        "%s %s --materialize" % (pb.price(1), rev, game, row["tile"])


def history_rows(p, ctx):
    """Changed, added and removed rows between revisions a and b. Returns
    (rows, notes, error)."""
    rr = RevReader()
    try:
        (cfa, sa), (cfb, sb) = rr.resolve(p["a"]), rr.resolve(p["b"])
        if not sa or not sb:
            return [], [], "revision %s does not resolve" % (
                p["a"] if not sa else p["b"])
        rows, notes = [], []
        games = [p["game"]] if p["game"] else list(pb.GAMES)
        for game in games:
            for mpath, tier, tdir in pb.reference_manifests(game):
                ma = manifest_at(cfa, sa, mpath)
                mb = manifest_at(cfb, sb, mpath)
                if ma is None and mb is None:
                    continue
                for side, m, rev in (("a", ma, p["a"]), ("b", mb, p["b"])):
                    if m is None:
                        notes.append("%s absent at %s (%s)" % (mpath, side,
                                                              rev))
                ma, mb = ma or {}, mb or {}
                for t in sorted(set(ma) | set(mb)):
                    if p["q"] and p["q"] not in t:
                        continue
                    ra, rb = ma.get(t), mb.get(t)
                    if ra and rb and ra["digest"] == rb["digest"]:
                        continue
                    kind = "changed" if ra and rb else (
                        "added" if rb else "removed")
                    ent = dict(tile_meta(t), tile=t, kind=kind, game=game,
                               tier=tier, manifest=mpath,
                               a_digest=ra["digest"] if ra else None,
                               b_digest=rb["digest"] if rb else None)
                    history_images(ent, ra, rb, tdir, rr, p, ctx)
                    rows.append(ent)
        rows.sort(key=lambda e: ({"changed": 0, "added": 1, "removed": 2}
                                 [e["kind"]], -e.get("pixels", 0),
                                 e["tile"]))
        return rows, notes, None
    finally:
        rr.close()


def history_images(ent, ra, rb, tdir, rr, p, ctx):
    """Fill a history row's image sources and, for a changed row with both
    sides resolved, its pixel diff."""
    verify = ent["kind"] == "changed"
    srcs = {}
    for side, row, rev in (("a", ra, p["a"]), ("b", rb, p["b"])):
        if row is None:
            continue
        where, val = locate(row, tdir, rr, rev, ent["game"], verify)
        ent[side + "_where"] = where
        if where == "materialize":
            ent[side + "_materialize"] = val
            continue
        srcs[side] = val
        if verify:
            ent[side + "_src"] = ctx.src(val, row["digest"] + ".png")
        elif where == "store":
            ent[side + "_thumb"] = ctx.thumb(row["digest"])
    if "a" in srcs and "b" in srcs and verify:
        ent.update(pixel_diff(srcs["a"], srcs["b"], ra["digest"],
                              rb["digest"]))
        if ent.get("heat"):
            ent["heat_src"] = ctx.file(ent["heat"])


# ---- promote list -------------------------------------------------------------------

def promote_list_path(pdir, game, tier):
    """The promote list of a game tier."""
    return os.path.join(pdir, "promote-%s-%s.txt" % (game, tier))


def read_promote_list(pdir, game, tier):
    """Tile names in a promote list."""
    try:
        with open(promote_list_path(pdir, game, tier)) as fh:
            return [ln.strip() for ln in fh if ln.strip()]
    except OSError:
        return []


def write_promote_list(pdir, game, tier, tiles):
    """Rewrite a promote list, one tile per line, sorted."""
    os.makedirs(pdir, exist_ok=True)
    p = promote_list_path(pdir, game, tier)
    tmp = p + ".tmp"
    with open(tmp, "w") as fh:
        fh.write("".join(t + "\n" for t in sorted(set(tiles))))
    os.replace(tmp, p)


def promote_command(p, pdir):
    """The promote invocation that consumes the list."""
    env = "PRESENTATION_TIER=dragons " if p["tier"] == "dragons" else ""
    cand = p["cand"]
    if cand.startswith(ROOT + os.sep):
        cand = os.path.relpath(cand, ROOT)
    return "%stests/harness/presentation-corpus-promote.sh %s " \
        "$(cat %s) --game %s" % (env, cand,
                                 promote_list_path(pdir, p["game"], p["tier"]),
                                 p["game"])


# ---- HTML -----------------------------------------------------------------------------

E = html.escape

CSS = """
:root{--bg:#15171a;--fg:#e6e6e6;--mut:#9aa0a6;--line:#2c3035;--acc:#7cc4ff}
body{background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif;
margin:0 16px 32px}
a{color:var(--acc)}nav{padding:10px 0;border-bottom:1px solid var(--line)}
nav a{margin-right:16px}form{margin:10px 0}input,select{background:#1f2226;
color:var(--fg);border:1px solid var(--line);padding:3px 6px}
.grid{display:flex;flex-wrap:wrap;gap:10px}.card{width:196px;font-size:11px;
word-break:break-all}.card img{width:192px;display:block;background:#000}
.miss{width:192px;height:144px;display:flex;align-items:center;
justify-content:center;background:#222;color:var(--mut)}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid
var(--line);padding:4px 6px;text-align:left;font-size:12px;
vertical-align:top}.panel{display:grid;gap:8px;
grid-template-columns:repeat(3,minmax(0,1fr))}@media(max-width:700px){.panel{
grid-template-columns:1fr}}
.panel figure{margin:0}.panel img{max-width:100%;image-rendering:pixelated}
.panel figcaption{color:var(--mut);font-size:11px}.mut{color:var(--mut)}
code{word-break:break-all}
"""

JS = """
function promote(cb){fetch('/promote',{method:'POST',headers:{'Content-Type':
'application/json'},body:JSON.stringify({game:cb.dataset.game,tier:
cb.dataset.tier,cand:cb.dataset.cand,tile:cb.dataset.tile,on:cb.checked})})
.then(r=>r.json()).then(j=>{document.getElementById('plist').textContent=
j.count+' tile(s) in the promote list'}).catch(e=>{cb.checked=!cb.checked;
alert('promote list not written: '+e)})}
"""


def page(title, body, static):
    """A complete HTML document."""
    nav = "" if static else (
        '<nav><a href="/current">current set</a><a href="/candidate">'
        'candidate review</a><a href="/history">history compare</a></nav>')
    return ("<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Golden browser</title><style>%s</style><script>%s</script>"
            "</head><body>%s<h2>%s</h2>%s</body></html>"
            % (CSS, JS, nav, E(title), body))


def select(name, value, options):
    """A <select> element."""
    opts = "".join("<option value='%s'%s>%s</option>" % (
        E(v), " selected" if v == value else "", E(v or "all")) for v in options)
    return "<select name=%s>%s</select>" % (name, opts)


def game_tier_select(p):
    """The game-tier selector."""
    opts = ["%s/%s" % gt for gt in tiers()]
    return select("gt", "%s/%s" % (p["game"], p["tier"]), opts)


def meta_cells(e):
    """Row, VBL, leg and LOD cells."""
    return "<td>%s</td><td>%s</td><td>%s</td><td>%s</td>" % (
        E(e["row"]), E(e["vbl"]), E(e["leg"]), E(e["lod"]))


def figure(src, cap):
    """One panel figure."""
    if not src:
        return ""
    return "<figure><img loading=lazy src='%s'><figcaption>%s</figcaption>" \
        "</figure>" % (E(src), E(cap))


def diff_text(e):
    """Pixel count, max channel delta and bounding box, or the size change
    or the note."""
    if "pixels" in e:
        return "pixels=%d maxch=%d bbox=%s" % (e["pixels"], e["maxch"],
                                               e["bbox"])
    if "size" in e:
        return "size=%s" % e["size"]
    return e.get("note", "")


def render_current(p, ctx, static):
    """The current-set view."""
    rows, err = current_rows(p)
    form = "" if static else (
        "<form action=/current>%s <input name=q value='%s' placeholder="
        "substring> %s <button>filter</button></form>" % (
            game_tier_select(p), E(p["q"]), select("leg", p["leg"],
                                                   ["", "canon", "inter"])))
    if err:
        return page("current set", form + "<p>%s</p>" % E(err), static), []
    cards = []
    for r in rows:
        src = ctx.thumb(r["digest"])
        img = "<img loading=lazy src='%s'>" % E(src) if src else \
            "<div class=miss>not in store</div>"
        cards.append("<div class=card>%s<b>%s</b><br>row %s &middot; VBL %s "
                     "&middot; %s &middot; %s</div>" % (
                         img, E(r["tile"]), E(r["row"]), E(r["vbl"]),
                         E(r["leg"]), E(r["lod"])))
    head = "<p class=mut>%s/%s: %d row(s) shown</p>" % (
        E(p["game"]), E(p["tier"]), len(rows))
    body = form + head + "<div class=grid>%s</div>" % "".join(cards)
    return page("current set", body, static), [
        ("row", r["tile"], r["digest"]) for r in rows]


def render_candidate(p, ctx, static, pdir):
    """The candidate-review view."""
    rows, summ, err = candidate_rows(p, ctx)
    form = "" if static else (
        "<form action=/candidate>%s <input name=cand size=48 value='%s' "
        "placeholder='candidate dir'> floor <input name=floor size=6 "
        "value='%s'> %s <input name=row value='%s' placeholder='corpus row'> "
        "<button>show</button></form>" % (
            game_tier_select(p), E(p["cand"] or ""), p["floor"] or "",
            select("leg", p["leg"], ["", "canon", "inter"]), E(p["row"])))
    if err:
        return page("candidate review", form + "<p>%s</p>" % E(err),
                    static), []
    chosen = set(read_promote_list(pdir, p["game"], p["tier"]))
    head = "<p class=mut>%s against %s/%s: %d compared, %d divergent, %d " \
        "missing, %d extra; %d listed</p>" % (
            E(p["cand"]), E(p["game"]), E(p["tier"]), summ["compared"],
            summ["divergent"], summ["missing"], summ["extra"], len(rows))
    if not static:
        head += "<p><span id=plist>%d tile(s) in the promote list</span>: " \
            "<code>%s</code></p>" % (len(chosen), E(promote_command(p, pdir)))
    trs = []
    for n, e in enumerate(rows):
        cb = "" if static else (
            "<input type=checkbox onchange='promote(this)' data-game='%s' "
            "data-tier='%s' data-cand='%s' data-tile='%s'%s>" % (
                E(p["game"]), E(p["tier"]), E(p["cand"]), E(e["tile"]),
                " checked" if e["tile"] in chosen else ""))
        panel = ""
        if e.get("ref_src"):
            panel = "<details%s><summary>panel</summary><div class=panel>" \
                "%s%s%s</div></details>" % (
                    " open" if n == 0 else "",
                    figure(e["ref_src"], "reference (%s)" % e["where"]),
                    figure(e["cand_src"], "candidate"),
                    figure(e.get("heat_src"), "heat x8"))
        trs.append("<tr><td>%s</td><td>%s%s</td>%s<td>%s</td></tr>" % (
            cb, E(e["tile"]), panel, meta_cells(e), E(diff_text(e))))
    body = form + head + "<table><tr><th>promote</th><th>tile</th><th>row" \
        "</th><th>VBL</th><th>leg</th><th>LOD</th><th>diff</th></tr>%s" \
        "</table>" % "".join(trs)
    return page("candidate review", body, static), [
        ("DIVERGE", e["tile"], diff_text(e)) for e in rows]


def render_history(p, ctx, static):
    """The history-compare view."""
    rows, notes, err = history_rows(p, ctx)
    form = "" if static else (
        "<form action=/history>a <input name=a value='%s'> b <input name=b "
        "value='%s'> %s <input name=q value='%s' placeholder=substring> "
        "<button>compare</button></form>" % (
            E(p["a"]), E(p["b"]), select("game", p["game"],
                                         [""] + list(pb.GAMES)), E(p["q"])))
    if err:
        return page("history compare", form + "<p>%s</p>" % E(err),
                    static), []
    head = "<p class=mut>%s -> %s: %d changed, %d added, %d removed</p>" % (
        E(p["a"]), E(p["b"]), sum(e["kind"] == "changed" for e in rows),
        sum(e["kind"] == "added" for e in rows),
        sum(e["kind"] == "removed" for e in rows))
    head += "".join("<p class=mut>%s</p>" % E(n) for n in notes)
    trs = []
    for n, e in enumerate(rows):
        cell = []
        for side in ("a", "b"):
            if e.get(side + "_materialize"):
                cell.append("<div>%s: materialize %s</div>" % (
                    side, E(e[side + "_materialize"])))
        if e["kind"] == "changed" and (e.get("a_src") or e.get("b_src")):
            cell.append("<details%s><summary>panel</summary><div class=panel>"
                        "%s%s%s</div></details>" % (
                            " open" if n == 0 else "",
                            figure(e.get("a_src"), "a %s (%s)" % (
                                p["a"], e.get("a_where"))),
                            figure(e.get("b_src"), "b %s (%s)" % (
                                p["b"], e.get("b_where"))),
                            figure(e.get("heat_src"), "heat x8")))
        for side in ("a", "b"):
            if e.get(side + "_thumb"):
                cell.append("<img loading=lazy width=96 src='%s'>"
                            % E(e[side + "_thumb"]))
        trs.append("<tr><td>%s</td><td>%s</td><td>%s%s</td>%s<td>%s</td>"
                   "</tr>" % (E(e["kind"]), E(e["game"] + "/" + e["tier"]),
                              E(e["tile"]), "".join(cell), meta_cells(e),
                              E(diff_text(e))))
    body = form + head + "<table><tr><th>kind</th><th>tier</th><th>tile</th>" \
        "<th>row</th><th>VBL</th><th>leg</th><th>LOD</th><th>diff</th></tr>" \
        "%s</table>" % "".join(trs)
    return page("history compare", body, static), [
        (e["kind"], e["tile"], "%s %s->%s" % (e["manifest"], e["a_digest"],
                                              e["b_digest"])) for e in rows]


# ---- parameters ------------------------------------------------------------------

def params(q):
    """Normalised view parameters from a dict of strings."""
    gt = q.get("gt", "")
    game, tier = (gt.split("/", 1) if "/" in gt else
                  (q.get("game", ""), q.get("tier", "")))
    try:
        floor = int(q.get("floor") or 0)
    except ValueError:
        floor = 0
    p = {"game": game, "tier": tier or "presentation", "q": q.get("q", ""),
         "leg": q.get("leg", ""), "row": q.get("row", ""),
         "cand": q.get("cand", ""), "floor": floor,
         "a": q.get("a") or "HEAD~1", "b": q.get("b") or "HEAD"}
    return p


def fix_game(p, view):
    """Default and validate the game tier (history allows every game)."""
    if view == "history":
        if p["game"] and p["game"] not in pb.GAMES:
            p["game"] = ""
        return
    if (p["game"], p["tier"]) not in tiers():
        p["game"], p["tier"] = "spyro-1", "presentation"


def render(view, p, ctx, static, pdir):
    """Render a view: (html, listed rows)."""
    fix_game(p, view)
    if view == "current":
        return render_current(p, ctx, static)
    if view == "candidate":
        return render_candidate(p, ctx, static, pdir)
    return render_history(p, ctx, static)


# ---- server ------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    """Serves the views, the registered files and the thumbnails; the one
    POST rewrites the promote list."""

    ctx = None
    pdir = None
    divergent = {}

    def send(self, code, ctype, data):
        """Write a response."""
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_file(self, path):
        """Write a PNG file."""
        try:
            with open(path, "rb") as fh:
                self.send(200, "image/png", fh.read())
        except OSError:
            self.send(404, "text/plain", b"absent")

    def do_GET(self):
        """Route a GET."""
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        path = u.path.rstrip("/") or "/current"
        if path.startswith("/f/"):
            p = self.ctx.files.get(path[3:])
            return self.send_file(p) if p else self.send(404, "text/plain",
                                                         b"not registered")
        if path.startswith("/thumb/"):
            d = path[7:]
            p = make_thumb(d) if pb.DIGEST_RE.match(d) else None
            return self.send_file(p) if p else self.send(404, "text/plain",
                                                         b"not in store")
        view = path[1:]
        if view not in ("current", "candidate", "history"):
            return self.send(404, "text/plain", b"no such view")
        p = params(q)
        doc, listed = render(view, p, self.ctx, False, self.pdir)
        if view == "candidate":
            Handler.divergent[(p["game"], p["tier"], p["cand"])] = {
                t for _, t, _ in listed}
        self.send(200, "text/html; charset=utf-8", doc.encode())

    def do_POST(self):
        """Add or remove one tile of the promote list. The tile must be a
        divergent row the candidate view listed for that candidate."""
        if self.path != "/promote":
            return self.send(404, "text/plain", b"no such endpoint")
        n = int(self.headers.get("Content-Length") or 0)
        try:
            j = json.loads(self.rfile.read(n))
            key = (j["game"], j["tier"], j["cand"])
            tile = j["tile"]
        except (ValueError, KeyError, TypeError):
            return self.send(400, "text/plain", b"bad request")
        if tile not in Handler.divergent.get(key, set()):
            return self.send(409, "text/plain", b"not a listed divergent row")
        tiles = set(read_promote_list(self.pdir, j["game"], j["tier"]))
        (tiles.add if j.get("on") else tiles.discard)(tile)
        write_promote_list(self.pdir, j["game"], j["tier"], tiles)
        self.send(200, "application/json",
                  json.dumps({"count": len(tiles)}).encode())

    def log_message(self, fmt, *args):
        """Quiet access log."""


def serve(port, pdir):
    """Serve on 127.0.0.1 until interrupted."""
    Handler.ctx = Ctx()
    Handler.pdir = pdir
    srv = HTTPServer((HOST, port), Handler)
    print("[golden-browser] http://%s:%d/  promote lists in %s"
          % (HOST, srv.server_address[1], pdir), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()
    return 0


def once(a, pdir):
    """Render one view to a static file and its row list."""
    out = os.path.abspath(a.out)
    assets = out + ".assets"
    os.makedirs(assets, exist_ok=True)
    q = {k: v for k, v in vars(a).items()
         if k in ("game", "tier", "cand", "a", "b", "q", "leg", "row")
         and v is not None}
    q["floor"] = str(a.floor or 0)
    doc, listed = render(a.once, params(q), Ctx(static_dir=assets), True,
                         pdir)
    for p in (out, out + ".rows.tsv"):
        if os.path.exists(p):
            os.unlink(p)
    with open(out, "w") as fh:
        fh.write(doc)
    with open(out + ".rows.tsv", "w") as fh:
        for rec in listed:
            fh.write("\t".join(str(x) for x in rec) + "\n")
    print("[golden-browser] %s: %d row(s) -> %s" % (a.once, len(listed), out))
    return 0


def main():
    """Parse the command line; serve or render once."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--promote-dir", default=os.path.join(ROOT, "build",
                                                          "golden-browser"))
    ap.add_argument("--once", choices=("current", "candidate", "history"))
    ap.add_argument("--out")
    for k in ("game", "tier", "cand", "a", "b", "q", "leg", "row"):
        ap.add_argument("--" + k)
    ap.add_argument("--floor", type=int)
    a = ap.parse_args()
    if a.once:
        if not a.out:
            ap.error("--once needs --out")
        return once(a, a.promote_dir)
    return serve(a.port, a.promote_dir)


if __name__ == "__main__":
    sys.exit(main())
