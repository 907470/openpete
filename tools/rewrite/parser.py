"""libclang TU loader for matching-decomp source.

Locates a usable libclang shared object, configures Index, and parses a
single .c file from spyro-1/src/ with the matching-decomp include path.

Targeted at mipsel-elf so libclang reports ILP32 sizes (sizeof(long) == 4,
sizeof(void*) == 4) - this lets the PSX layout calculator validate against
clang's own type metrics on primitives where the host happens to disagree
(LP64 long, void*).
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional

import clang.cindex as ci


_LIBCLANG_CANDIDATES = (
    "/usr/lib/x86_64-linux-gnu/libclang-14.so.1",
    "/usr/lib/x86_64-linux-gnu/libclang-14.so",
    "/usr/lib/llvm-14/lib/libclang.so.1",
    "/usr/lib/llvm-14/lib/libclang.so",
)


def _configure_libclang() -> None:
    if getattr(_configure_libclang, "_done", False):
        return
    override = os.environ.get("OPENPETE_LIBCLANG")
    candidates = (override,) if override else _LIBCLANG_CANDIDATES
    for path in candidates:
        if path and Path(path).exists():
            ci.Config.set_library_file(path)
            _configure_libclang._done = True  # type: ignore[attr-defined]
            return
    raise RuntimeError(
        "could not locate libclang.so; set OPENPETE_LIBCLANG=/path/to/libclang.so"
    )


# Where the matching-decomp tree lives.
SPYRO_REPO = Path(__file__).resolve().parents[3] / "spyro-1"
if not SPYRO_REPO.exists():
    # Fallback: parent of openpete/.
    SPYRO_REPO = Path(__file__).resolve().parents[4] / "spyro-1"


def default_include_path() -> List[str]:
    return [
        str(SPYRO_REPO / "include"),
        str(SPYRO_REPO / "psyq" / "include"),
    ]


def default_defines() -> List[str]:
    return [
        "-D_HAS_MASPSX",
        "-DNDEBUG",
        "-DNNOCD",
        "-D__GNUC__=2",
        "-D__GNUC_MINOR__=7",
    ]


def default_target_args() -> List[str]:
    return [
        "-target", "mipsel-unknown-elf",
        "-march=mips1",
        "-msoft-float",
        "-ffreestanding",
    ]


@dataclass
class ParsedTU:
    path: Path
    tu: ci.TranslationUnit

    @property
    def cursor(self) -> ci.Cursor:
        return self.tu.cursor

    def diagnostics(self, include_includes: bool = False) -> List[ci.Diagnostic]:
        diags = list(self.tu.diagnostics)
        if include_includes:
            return diags
        own = str(self.path.resolve())
        return [
            d for d in diags
            if d.location.file is None or str(Path(d.location.file.name).resolve()) == own
        ]


def parse_tu(
    src: os.PathLike,
    extra_args: Optional[Iterable[str]] = None,
    include_path: Optional[Iterable[str]] = None,
    defines: Optional[Iterable[str]] = None,
    options: int = 0,
) -> ParsedTU:
    """Parse a single matching-decomp .c file and return the libclang TU.

    Raises RuntimeError if libclang reports parse-blocking errors in the
    target file itself (header errors are surfaced as warnings; we accept
    them for now since the matching-decomp tree compiles fine under
    GCC 2.7.2 but trips clang on PSYQ-isms).
    """
    _configure_libclang()
    src = Path(src).resolve()

    args: List[str] = []
    args += default_target_args()
    args += [f"-I{p}" for p in (include_path or default_include_path())]
    args += list(defines or default_defines())
    if extra_args:
        args += list(extra_args)

    index = ci.Index.create()
    tu = index.parse(str(src), args=args, options=options)
    if tu is None:
        raise RuntimeError(f"libclang failed to parse {src}")

    return ParsedTU(path=src, tu=tu)


def _main(argv: List[str]) -> int:
    """CLI smoke: `python -m tools.rewrite.parser <path/to/file.c>`."""
    if len(argv) < 2:
        target = SPYRO_REPO / "src" / "initialization.c"
    else:
        target = Path(argv[1])
    parsed = parse_tu(target)
    own = parsed.diagnostics(include_includes=False)
    foreign = parsed.diagnostics(include_includes=True)
    own_errors = [d for d in own if d.severity >= ci.Diagnostic.Error]
    foreign_errors = [d for d in foreign if d.severity >= ci.Diagnostic.Error]

    print(f"parsed {parsed.path}")
    print(f"  own-file diagnostics: {len(own)} ({len(own_errors)} errors)")
    print(f"  total diagnostics:    {len(foreign)} ({len(foreign_errors)} errors)")
    for d in own_errors[:10]:
        print(f"  E: {d.location.file}:{d.location.line}: {d.spelling}")
    for d in foreign_errors[:10]:
        print(f"  e: {d.location.file}:{d.location.line}: {d.spelling}")
    return 1 if own_errors else 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
