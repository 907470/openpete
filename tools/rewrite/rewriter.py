"""Source-range splicer.

Each pass returns a list of `Edit` objects keyed by absolute byte offsets
into a specific source file. The splicer collects edits per file, asserts
non-overlap, and applies them right-to-left so earlier offsets stay valid.

Comments and whitespace between edited spans are preserved (the splicer
doesn't reformat — it just substitutes spans).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List

import clang.cindex as ci


@dataclass(order=True)
class Edit:
    """A single textual edit on a specific file."""
    start: int                    # byte offset (inclusive)
    end: int                      # byte offset (exclusive)
    replacement: str = field(compare=False)
    file: str = field(default="", compare=False)
    note: str = field(default="", compare=False)  # diagnostic only

    def __post_init__(self) -> None:
        if self.end < self.start:
            raise ValueError(f"Edit end<start: {self}")
        if not self.file:
            raise ValueError(f"Edit missing file: {self}")


def edit_from_range(rng: ci.SourceRange, replacement: str, note: str = "") -> Edit:
    """Build an Edit from a libclang SourceRange.

    Uses the start/end offsets and the file of the range's start cursor.
    Both endpoints must live in the same file (libclang invariant for
    well-formed ranges within a single TU).
    """
    start = rng.start
    end = rng.end
    if start.file is None or end.file is None:
        raise ValueError("edit_from_range: range has no file")
    if str(start.file.name) != str(end.file.name):
        raise ValueError(f"edit_from_range: cross-file range {start.file.name}->{end.file.name}")
    return Edit(start=start.offset, end=end.offset,
                replacement=replacement,
                file=str(start.file.name),
                note=note)


def insert_at(loc: ci.SourceLocation, text: str, note: str = "") -> Edit:
    if loc.file is None:
        raise ValueError("insert_at: location has no file")
    return Edit(start=loc.offset, end=loc.offset,
                replacement=text,
                file=str(loc.file.name),
                note=note)


# ---------------------------------------------------------------------------
# Splice driver
# ---------------------------------------------------------------------------

@dataclass
class FilePlan:
    path: Path
    original: bytes
    edits: List[Edit] = field(default_factory=list)


class EditPlan:
    """Collects edits across multiple source files; applies them on commit."""

    def __init__(self) -> None:
        self._files: Dict[str, FilePlan] = {}

    def add(self, edit: Edit) -> None:
        path = str(Path(edit.file).resolve())
        if path not in self._files:
            self._files[path] = FilePlan(
                path=Path(path),
                original=Path(path).read_bytes(),
            )
        self._files[path].edits.append(edit)

    def add_many(self, edits: Iterable[Edit]) -> None:
        for e in edits:
            self.add(e)

    def files(self) -> List[Path]:
        return [fp.path for fp in self._files.values()]

    def apply(self, file: Path) -> str:
        """Return the rewritten content for `file` as a string."""
        path = str(Path(file).resolve())
        if path not in self._files:
            return Path(path).read_text()
        fp = self._files[path]
        edits = sorted(fp.edits, key=lambda e: (e.start, e.end))
        # Non-overlap assertion (zero-width inserts at the same offset are OK).
        prev_end = -1
        for e in edits:
            if e.start < prev_end:
                raise ValueError(
                    f"overlapping edit in {fp.path}: {e} after end={prev_end}"
                )
            prev_end = max(prev_end, e.end)

        # Splice right-to-left so earlier offsets stay valid.
        buf = bytearray(fp.original)
        for e in reversed(edits):
            buf[e.start:e.end] = e.replacement.encode("utf-8")
        return buf.decode("utf-8")

    def write(self, src: Path, dst: Path) -> None:
        """Apply edits for `src` and write the result to `dst`."""
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(self.apply(src))


# ---------------------------------------------------------------------------
# CLI smoke
# ---------------------------------------------------------------------------

def _main(argv: List[str]) -> int:
    if len(argv) < 2:
        print("usage: rewriter.py <file>", flush=True)
        return 1
    plan = EditPlan()
    plan.add(Edit(start=0, end=0, replacement="// rewritten\n",
                  file=argv[1], note="banner"))
    print(plan.apply(Path(argv[1]))[:200])
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_main(sys.argv))
