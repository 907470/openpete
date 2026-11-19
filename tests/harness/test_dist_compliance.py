#!/usr/bin/env python3
"""Self-test of scripts/dist_compliance.py licences on synthetic trees.

Each case builds a scratch repository (NOTICE.md, the manifest, a
third_party/ tree, packaging texts), runs the licences check from its root
and compares the exit code and the refusal's text. The baseline passes; each
refusal case removes or adds one thing the check accounts for. Cases run
with and without a git repository, since the check lists third_party/ through
git where it can and by walking the tree where it cannot. The docs cases stage
a synthetic documentation site and run the docs check against a NOTICE.md
with a documentation-site section.
"""

import os
import shutil
import subprocess
import sys
import tempfile

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "scripts", "dist_compliance.py")

NOTICE = """# NOTICE

| Component | License | Upstream |
| --------- | ------- | -------- |
| liba      | MIT     | https://example.invalid/liba |
| libb      | BSD-3   | https://example.invalid/libb |
"""

MANIFEST_ROWS = [
    "liba\tall\tthird_party/liba/LICENSE\tliba.txt\tthird_party/liba",
    "liba\tall\tthird_party/liba/sub/LICENSE-bsd.txt\tliba-sub.txt\tthird_party/liba/sub",
    "libb\tall\tpackaging/licences/libb.txt\tlibb.txt\tthird_party/libb",
    "third_party/unused\tnone\tthird_party/unused/COPYING\t-\tthird_party/unused",
]

FILES = {
    "third_party/liba/LICENSE": "MIT text\n",
    "third_party/liba/a.c": "int a;\n",
    "third_party/liba/sub/LICENSE-bsd.txt": "BSD text\n",
    "third_party/libb/b.h": "/* BSD-3 notice in the header */\n",
    "third_party/unused/COPYING": "ISC text\n",
    "packaging/licences/libb.txt": "BSD-3 text\n",
    ".gitignore": "third_party/built/\n",
}


def build(root, rows=None, files=None, git=False):
    """Write a synthetic repository under root."""
    for path, body in (files if files is not None else FILES).items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as f:
            f.write(body)
    with open(os.path.join(root, "NOTICE.md"), "w", encoding="utf-8") as f:
        f.write(NOTICE)
    os.makedirs(os.path.join(root, "packaging", "licences"), exist_ok=True)
    with open(os.path.join(root, "packaging", "licences", "manifest.tsv"), "w",
              encoding="utf-8") as f:
        f.write("# header\n" + "\n".join(rows if rows is not None else MANIFEST_ROWS) + "\n")
    if git:
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True, env=env)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True, env=env)


def run(root):
    """Run the licences check in root; return (exit code, stderr)."""
    stage = os.path.join(root, "stage")
    r = subprocess.run([sys.executable, "-I", SCRIPT, "licences", "--stage", stage,
                        "--scope", "all"], cwd=root, capture_output=True, text=True)
    return r.returncode, r.stderr


def case(name, expect_rc, expect_text, rows=None, files=None, extra=None, git=False):
    """Build, optionally add the extra files, run, and compare; return True on a match."""
    root = tempfile.mkdtemp(prefix="dist-compliance-")
    try:
        build(root, rows, files, git)
        for path, body in (extra or {}).items():
            full = os.path.join(root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(body)
        rc, err = run(root)
        ok = rc == expect_rc and expect_text in err
        if ok and rc == 0:
            ok = sorted(os.listdir(os.path.join(root, "stage", "licences"))) \
                == ["liba-sub.txt", "liba.txt", "libb.txt"]
        print(f"[dist-compliance-test] {'PASS' if ok else 'FAIL'} {name}"
              + ("" if ok else f": rc {rc}, stderr {err.strip()!r}"))
        return ok
    finally:
        shutil.rmtree(root)


DOCS_NOTICE = NOTICE + """
## Documentation site - bundled under `docs/`

| Component | License | Upstream |
| --------- | ------- | -------- |
| theme (`assets/stylesheets/`, `assets/javascripts/bundle.*.min.js`) | MIT | https://example.invalid/theme |
| icons (inlined in the pages) | Apache-2.0 | https://example.invalid/icons |
| shim (`assets/javascripts/shim.js`; text also as `shim-LICENSE` beside it) | MIT | https://example.invalid/shim |

## Bundled data

| Component | License | Upstream |
| --------- | ------- | -------- |
| data (`assets/not-docs.bin`) | MIT | https://example.invalid/data |
"""

DOCS_FILES = {
    "docs/index.html": "<html></html>\n",
    "docs/search/search_index.json": '{"config": {"lang": ["en"]}, "docs": []}\n',
    "docs/assets/stylesheets/main.css": "a{}\n",
    "docs/assets/javascripts/bundle.0123.min.js": "x\n",
    "docs/assets/javascripts/shim.js": "x\n",
}


def docs_case(name, expect_rc, expect_text, notice=DOCS_NOTICE, drop=(), extra=None):
    """Stage a synthetic docs site, run the docs check, compare; return True on a match."""
    root = tempfile.mkdtemp(prefix="dist-compliance-docs-")
    try:
        files = {k: v for k, v in DOCS_FILES.items() if k not in drop}
        files.update(extra or {})
        for path, body in files.items():
            full = os.path.join(root, "stage", path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(body)
        with open(os.path.join(root, "NOTICE.md"), "w", encoding="utf-8") as f:
            f.write(notice)
        r = subprocess.run([sys.executable, "-I", SCRIPT, "docs", "--stage",
                            os.path.join(root, "stage")],
                           cwd=root, capture_output=True, text=True)
        ok = r.returncode == expect_rc and expect_text in (r.stderr if expect_rc else r.stdout)
        print(f"[dist-compliance-test] {'PASS' if ok else 'FAIL'} docs: {name}"
              + ("" if ok else f": rc {r.returncode}, out {(r.stdout + r.stderr).strip()!r}"))
        return ok
    finally:
        shutil.rmtree(root)


def docs_cases():
    """Return the results of the docs subcommand's cases."""
    lunr = "docs/assets/javascripts/lunr/min/lunr.ja.min.js"
    return [
        docs_case("baseline", 0, "docs PASS: 3 rows, 3 paths present"),
        docs_case("search language directory staged", 1,
                  "assets/javascripts/lunr staged", extra={lunr: "x\n"}),
        docs_case("search language directory holding one file staged", 1,
                  "assets/javascripts/lunr staged",
                  extra={"docs/assets/javascripts/lunr/tinyseg.js": "x\n"}),
        docs_case("search language other than English", 1, "search languages",
                  extra={"docs/search/search_index.json":
                         '{"config": {"lang": ["en", "ja"]}}\n'}),
        docs_case("search index missing", 1, "has no config.lang",
                  drop=("docs/search/search_index.json",)),
        docs_case("row lists a file that does not ship", 1,
                  "assets/javascripts/shim.js (shim",
                  drop=("docs/assets/javascripts/shim.js",)),
        docs_case("row lists a glob that matches nothing", 1,
                  "assets/javascripts/bundle.*.min.js (theme",
                  drop=("docs/assets/javascripts/bundle.0123.min.js",)),
        docs_case("row lists a directory that does not ship", 1,
                  "assets/stylesheets/ (theme",
                  drop=("docs/assets/stylesheets/main.css",)),
        docs_case("no docs site staged", 1, "no documentation site staged",
                  drop=("docs/index.html",)),
        docs_case("NOTICE without a docs section", 1, "no '## Documentation site' rows",
                  notice=NOTICE),
    ]


def main():
    """Run every case under both listing paths; exit 1 on any mismatch."""
    no_sub = {k: v for k, v in FILES.items() if k != "third_party/liba/sub/LICENSE-bsd.txt"}
    results = []
    for git in (False, True):
        tag = "git" if git else "walk"
        results += [
            case(f"{tag}: baseline", 0, "", git=git),
            case(f"{tag}: nested licence file in no row", 1,
                 "third_party/liba/sub/COPYING.txt",
                 extra={"third_party/liba/sub/COPYING.txt": "x\n"}, git=git)
            if not git else
            case(f"{tag}: nested licence file in no row", 1,
                 "third_party/liba/sub/LICENSE-bsd.txt",
                 rows=[r for r in MANIFEST_ROWS if "LICENSE-bsd" not in r], git=git),
            case(f"{tag}: third_party directory no source covers", 1,
                 "covers: third_party/libc",
                 files=dict(FILES, **{"third_party/libc/c.h": "x\n"}), git=git),
            case(f"{tag}: source path missing", 1, "names source third_party/liba/sub",
                 files=no_sub,
                 rows=[r.replace("third_party/liba/sub/LICENSE-bsd.txt",
                                 "packaging/licences/libb.txt") for r in MANIFEST_ROWS],
                 git=git),
            case(f"{tag}: packaging text no row names", 1, "packaging/licences/stale.txt",
                 files=dict(FILES, **{"packaging/licences/stale.txt": "x\n"}), git=git),
            case(f"{tag}: active text missing", 1, "ships with no licence text",
                 files={k: v for k, v in FILES.items() if k != "packaging/licences/libb.txt"},
                 git=git),
        ]
    results += [
        case("four-column row", 1, "expected 5 non-empty",
             rows=MANIFEST_ROWS[:-1] + ["third_party/unused\tnone\tthird_party/unused/COPYING\t-"]),
        case("NOTICE row with no manifest row", 1, "NOTICE.md rows with no licence text",
             rows=[r for r in MANIFEST_ROWS if not r.startswith("libb")],
             files={k: v for k, v in FILES.items() if k != "packaging/licences/libb.txt"}),
        case("vendored licence under a component no package contains", 0, "",
             extra={"third_party/unused/dep/x/LICENSE": "x\n"}),
        case("vendored licence under an unshipped directory inside a shipped one", 1,
             "third_party/liba/tools/dep/LICENSE",
             rows=MANIFEST_ROWS + ["liba tools\tnone\t-\t-\tthird_party/liba"],
             extra={"third_party/liba/tools/dep/LICENSE": "x\n"}),
        case("gitignored directory is not walked", 0, "",
             extra={"third_party/built/COPYING": "x\n"}),
    ]
    results += docs_cases()
    print(f"[dist-compliance-test] {sum(results)}/{len(results)} cases")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
