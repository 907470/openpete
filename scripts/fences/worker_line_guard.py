#!/usr/bin/env python3
"""Fence: per-worker state fills whole cache lines.

Worker threads that each write their own element of one array still share a
cache line when an element's size is not a multiple of the line, or when the
array does not start on a line: the line then bounces between cores on every
write, and whether two workers' hot fields meet on a line depends on where
the linker places the array, which any commit that adds or removes a static
can change. The element type's first member carries OPENPETE_WORKER_ALIGN
and the type is checked by OPENPETE_WORKER_ELEM(T) (core/include/
openpete_workpool.h), which makes the layout independent of placement.

A per-worker array is an array declaration whose dimension names a macro
containing WORKERS or THREADS, or an array the same file indexes with
`[worker]`. Refused: such an array whose element type T has no
OPENPETE_WORKER_ELEM(T) in any scanned source, and such an array of
pointers or of a builtin scalar (neither can carry the alignment). A
declaration whose line carries `worker-line: <reason>` is exempt; the reason
says why the array is cold or read-only on the parallel path. Scope: C and
C++ sources under core/ and games/, generated directories and the decomp
checkouts excluded. Comments are ignored.

Run:
  python3 scripts/fences/worker_line_guard.py tree [root]   every source (build target)
  python3 scripts/fences/worker_line_guard.py hook          the staged sources, read from the index
Exit: 0 clean, 66 a refused array.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import os
import re
import subprocess
import sys

EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")
MARKER = "worker-line:"
ELEM = re.compile(r"\bOPENPETE_WORKER_ELEM\s*\(\s*([A-Za-z_][\w\s]*?)\s*\)")
WORKER_DIM = re.compile(r"\b[A-Z0-9_]*(?:WORKERS|THREADS)[A-Z0-9_]*\b")
WORKER_INDEX = re.compile(r"\b([A-Za-z_]\w*)\s*\[\s*worker\s*\]")
DECL = re.compile(
    r"^[ \t]*(?:(?:static|extern|const|volatile|_Thread_local)\s+)*"
    r"(?P<type>(?:(?:struct|union|enum)\s+)?[A-Za-z_]\w*)\s*(?P<ptr>[*\s]*?)"
    r"\b(?P<name>[A-Za-z_]\w*)\s*\[(?P<dim>[^\]\n]*)\]", re.M)
NOT_TYPES = {"return", "case", "goto", "sizeof", "else", "do", "typedef", "define"}
SCALARS = {"char", "short", "int", "long", "unsigned", "signed", "float", "double", "bool", "_Bool",
           "size_t", "intptr_t", "uintptr_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
           "int32_t", "uint32_t", "int64_t", "uint64_t", "Uint8", "Uint16", "Uint32", "Uint64",
           "Sint8", "Sint16", "Sint32", "Sint64", "SDL_AtomicInt", "SDL_AtomicU32"}


def strip_comments(text):
    """Blank out comments, keeping line breaks and offsets."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


def elem_names(text):
    """Type names checked by OPENPETE_WORKER_ELEM in one source."""
    return {re.sub(r"\s+", " ", m.group(1)) for m in ELEM.finditer(strip_comments(text))
            if not m.group(1).strip() == "T"}


def worker_arrays(text):
    """(line, name, type, is_pointer) of every per-worker array declared in one source."""
    code = strip_comments(text)
    indexed = set(WORKER_INDEX.findall(code))
    out = []
    for m in DECL.finditer(code):
        typ, name = m.group("type"), m.group("name")
        if typ.split()[-1] in NOT_TYPES or typ in ("struct", "union", "enum"):
            continue
        if not (WORKER_DIM.search(m.group("dim")) or name in indexed):
            continue
        line = code.count("\n", 0, m.start("name")) + 1
        out.append((line, name, typ, "*" in m.group("ptr")))
    return out


def scan(path, text, elems):
    """Refusal lines of one source given the checked element types."""
    raw = text.split("\n")
    out = []
    for line, name, typ, ptr in worker_arrays(text):
        if MARKER in raw[line - 1]:
            continue
        bare = typ.split()[-1]
        if ptr or bare in SCALARS:
            out.append("%s:%d: %s[] holds %s, which cannot fill a cache line per worker "
                       "(an aligned element type, or `%s <reason>` when cold)" % (
                           path, line, name, "pointers" if ptr else bare, MARKER))
        elif typ not in elems and bare not in elems:
            out.append("%s:%d: %s[] of %s: the type has no OPENPETE_WORKER_ELEM(%s)" % (
                path, line, name, typ, bare))
    return out


def in_scope(rel):
    """Whether a repo-relative path is a scanned source."""
    if not rel.endswith(EXTS) or not rel.startswith(("core/", "games/")):
        return False
    parts = rel.split("/")
    if "generated" in parts:
        return False
    return not (len(parts) > 2 and parts[0] == "games" and parts[2] == parts[1])


def tree_sources(root):
    """(rel, text) of every in-scope source under root."""
    for top in ("core", "games"):
        for d, dirs, files in os.walk(os.path.join(root, top)):
            rel_d = os.path.relpath(d, root)
            dirs[:] = [x for x in dirs if in_scope(os.path.join(rel_d, x, "x.c"))]
            for f in files:
                rel = os.path.join(rel_d, f)
                if in_scope(rel):
                    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
                        yield rel, fh.read()


def staged_sources(root):
    """(rel, text) of every staged in-scope source, read from the index."""
    names = subprocess.run(["git", "-C", root, "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                           capture_output=True, text=True, check=True).stdout.split()
    for rel in (n for n in names if in_scope(n)):
        p = subprocess.run(["git", "-C", root, "show", ":" + rel], capture_output=True, text=True)
        if p.returncode == 0:
            yield rel, p.stdout


def index_elems(root):
    """OPENPETE_WORKER_ELEM type names in every in-scope source of the index."""
    p = subprocess.run(["git", "-C", root, "grep", "--cached", "-h", "-E", r"OPENPETE_WORKER_ELEM\s*\(",
                        "--", "core", "games"], capture_output=True, text=True)
    return elem_names(p.stdout)


def main(argv):
    """Run the fence in tree or hook mode."""
    mode = argv[1] if len(argv) > 1 else "tree"
    root = os.path.abspath(argv[2]) if len(argv) > 2 else \
        os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    srcs = list(tree_sources(root) if mode == "tree" else staged_sources(root))
    elems = set()
    for _, text in srcs:
        elems |= elem_names(text)
    if mode != "tree":
        elems |= index_elems(root)
    refused, n = [], 0
    for rel, text in srcs:
        n += 1
        refused += scan(rel, text, elems)
    for r in refused:
        print("worker-line: REFUSED %s" % r)
    print("worker-line: %s (%d source(s) scanned)" % ("FAIL" if refused else "PASS", n))
    return 66 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
