#!/usr/bin/env python3
"""Tiny C-prototype parser over spyro-1/include/.

Just enough to map function names to argument-type lists for the
dispatch + thunks generators. Not a full C parser — it ignores
function pointers, typedef'd callbacks, and multi-line declarations.
Anything it can't parse comes back as `None`, and the generator falls
back to the hand-written `args` field in native.toml.

Public API:
    load_signatures(include_dir) -> dict[str, Signature]
    type_to_marshal_token(c_type) -> "u32" | "u8" | "ptr"
    Signature.args_tokens(stack_offset_base=16) -> list[str]
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


# Strip /* ... */ and // ... comments. Block comments may span multiple
# lines, so we run this on the whole file at once.
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_PREPROC_RE = re.compile(r"^\s*#.*$", re.MULTILINE)

# Per-statement matcher. Caller splits the file on `;` so each candidate
# is at most one declaration. Anchored, no nested ambiguity → linear time.
#   "  extern  CdlLOC * foo (int x, void *y) "
#                ^^^^^^^^^   ^^^   ^^^^^^^^^^^^^^^^
#                ret_tokens  name  params
_PROTO_RE = re.compile(
    r"""
    ^\s*
    (?P<ret>[\w\s*]+?)              # one-or-more type/storage tokens (lazy)
    \b(?P<name>[A-Za-z_]\w*)\s*     # function name
    \(\s*(?P<params>[^()]*?)\s*\)   # ( params ) — no nested parens (no fn-ptrs)
    \s*$
    """,
    re.VERBOSE | re.DOTALL,
)


@dataclass
class Signature:
    name: str
    return_type: str
    # List of (raw_type_string, raw_name); name may be empty for `void`.
    params: list[tuple[str, str]]
    source_file: str

    def args_tokens(self, stack_offset_base: int = 16) -> list[str]:
        """Convert params to dispatch/thunks marshalling tokens.

        First 4 params land in a0..a3; everything beyond that goes on the
        stack at sp+16, sp+20, sp+24, … (PSYQ MIPS ABI: 4-byte slots
        regardless of declared width)."""
        out: list[str] = []
        for idx, (ty, _name) in enumerate(self.params):
            tok = type_to_marshal_token(ty)
            if idx < 4:
                out.append(tok)
            else:
                offset = stack_offset_base + (idx - 4) * 4
                # u8/u16 widen to 32-bit on the stack — same as register.
                # We only emit `stack_u32` since that's all the dispatch
                # generator handles today.
                out.append(f"stack_u32:{offset}")
        return out


_PTR_RE = re.compile(r"\*")
_U8_TYPES = {"char", "u_char", "uchar", "uint8_t", "int8_t", "byte"}
_U16_TYPES = {"short", "u_short", "ushort", "uint16_t", "int16_t"}


def type_to_marshal_token(c_type: str) -> str:
    """Heuristic C type → 'u32' | 'u8' | 'ptr'.

    Rules:
      - Any '*' or '[' in the declarator → ptr
      - Bare 'char'/'u_char'/'uint8_t'/etc → u8
      - Bare 'short'/'uint16_t'/etc → u16-ish, returned as u32 today
        (the dispatch generator only knows u32/u8/ptr)
      - Anything else → u32
    """
    t = c_type.strip()
    if "*" in t or "[" in t:
        return "ptr"
    # Strip qualifiers and trailing whitespace.
    tokens = [
        tok for tok in t.replace("const", "").replace("volatile", "").split()
        if tok
    ]
    # Take the rightmost type token — handles `unsigned char`.
    if not tokens:
        return "u32"
    last = tokens[-1]
    if last in _U8_TYPES:
        return "u8"
    if last in _U16_TYPES:
        return "u32"  # widened in regs anyway; gen_dispatch lacks u16
    return "u32"


# `extern "C" {` and `extern "C++" {` linkage blocks aren't terminated
# by `;`, so the `;`-split in parse_file leaves the opening fragment
# glued to the next prototype. Strip them early.
_LINKAGE_BLOCK_RE = re.compile(r'extern\s*"[^"]*"\s*\{')
_LINKAGE_CLOSE_RE = re.compile(r'^\s*\}\s*$', re.MULTILINE)


def _strip_comments_and_preproc(src: str) -> str:
    src = _BLOCK_COMMENT_RE.sub("", src)
    src = _LINE_COMMENT_RE.sub("", src)
    src = _PREPROC_RE.sub("", src)
    src = _LINKAGE_BLOCK_RE.sub("", src)
    src = _LINKAGE_CLOSE_RE.sub("", src)
    return src


_TYPE_KEYWORDS = frozenset({
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned",
})


def _parse_params(raw: str) -> list[tuple[str, str]]:
    """`int sector, void *buf, int size` → [('int','sector'),('void *','buf'),...].

    Returns [] for `void` or empty params. Each element is the raw type
    token string plus the param name (may be empty for unnamed params)."""
    raw = raw.strip()
    if not raw or raw == "void":
        return []
    out: list[tuple[str, str]] = []
    for chunk in raw.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        # Last identifier-looking token is the name; everything before it
        # plus any leading * on the name belong to the type.
        m = re.match(r"^(.*?)([*\s]*)([A-Za-z_]\w*)\s*$", chunk)
        if not m:
            out.append((chunk, ""))
            continue
        ty_left, stars, last_ident = m.group(1), m.group(2), m.group(3)
        # If the only identifier is the type (`int`, `void`), there's no name.
        if not ty_left.strip() and "*" not in stars:
            out.append((last_ident, ""))
        elif last_ident in _TYPE_KEYWORDS and "*" not in stars:
            # Unnamed multi-word type like `unsigned int` or `long long`:
            # `last_ident` is itself a C type keyword and there's no `*`,
            # so the whole chunk is the type with no param name.
            ty = (ty_left + stars + last_ident).strip()
            out.append((ty, ""))
        else:
            ty = (ty_left + stars).strip()
            out.append((ty, last_ident))
    return out


def parse_file(path: Path) -> list[Signature]:
    text = _strip_comments_and_preproc(path.read_text(errors="replace"))
    sigs: list[Signature] = []
    # Split on `;` so each candidate is one statement — keeps the regex
    # bounded and linear, dodges catastrophic backtracking.
    for stmt in text.split(";"):
        stmt = stmt.strip()
        if not stmt or "(" not in stmt:
            continue
        # Reject statements with nested parens (function pointers,
        # complex declarators) — out of scope for the heuristic parser.
        if stmt.count("(") != 1 or stmt.count(")") != 1:
            continue
        # Reject obvious non-declarations: assignments, control flow.
        if "=" in stmt or stmt.startswith(("{", "}")):
            continue
        m = _PROTO_RE.match(stmt)
        if not m:
            continue
        name = m.group("name")
        if name in {"if", "while", "for", "switch", "return", "sizeof"}:
            continue
        ret = m.group("ret").strip()
        # Strip storage-class specifiers from the return type — `extern`,
        # `static`, etc. — they're not part of the function's host C++
        # signature when we render `T name(args)` later.
        ret = re.sub(r"\b(extern|static|inline)\b\s*", "", ret).strip()
        if not ret:
            continue
        sigs.append(Signature(
            name=name,
            return_type=ret,
            params=_parse_params(m.group("params")),
            source_file=str(path),
        ))
    return sigs


def load_signatures(include_dir: Path) -> dict[str, Signature]:
    """Walk include_dir/**/*.h and collect every prototype we can parse.

    Later definitions override earlier ones — Spyro's headers are
    consistent, so duplicates here would be an actual ODR-style mismatch
    we'd want to surface, but for now last-wins is fine."""
    out: dict[str, Signature] = {}
    for p in include_dir.rglob("*.h"):
        for sig in parse_file(p):
            out[sig.name] = sig
    return out


# CLI for spot-checking.
if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("usage: parse_headers.py <include_dir>", file=sys.stderr)
        sys.exit(2)
    sigs = load_signatures(Path(sys.argv[1]))
    print(f"parsed {len(sigs)} prototypes")
    for name in ("main", "Initialize", "CDLoadTime", "CDReadDone",
                 "CDLoadSync", "GamestateUpdate", "SoundsUpdate"):
        s = sigs.get(name)
        if s:
            toks = s.args_tokens()
            print(f"  {s.return_type} {name}({s.params}) -> {toks}")
        else:
            print(f"  {name}: NOT FOUND")
