#!/usr/bin/env python3
"""The oracle's event ratchet lists: games/<game>/manifest/oracle_ratchet.toml.

One table per golden (its file name without `.bin`), three lists:

  gte              records whose GTE hash row is an event (a carry folded)
  vbl              records that open a VBL deficit against the reference
  gameplay_bursts  "REGION@RECORD": cd_load_transient bursts that open
                   outside a load span

The verifier (core/diag/psxtrace_ratchet.c) fails a row on an event its
table does not list and reports each listed entry the run did not produce
as stale (a failure under OPENPETE_PSXTRACE_RATCHET_STRICT=1). This tool
rewrites a table from a complete run's log
(OPENPETE_ORACLE_LOG=PATH tests/harness/check_psx_oracle.sh ...):

  tighten LOG                      drop the stale entries; refused when the
                                   log holds an event the table does not list
  tighten --accept REASON LOG      write the events the log holds, new ones
                                   included; REASON lands as a comment above
                                   the table
  show LOG                         print the log's events and the table's
                                   new and stale entries, write nothing

The game and the table come from the log's `ratchet:` load line. A log that
did not reach the trace's end is refused.
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import tomllib  # py 3.11+
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

CLASSES = ("gte", "vbl", "gameplay_bursts")
LOAD_RE = re.compile(r"psxtrace: ratchet: (\S+) \[([^\]]+)\]: \d+ listed")
EVENT_RE = {
    "gte": re.compile(r"psxtrace: gte event: record (\d+) "),
    "vbl": re.compile(r"psxtrace: vbl event: record (\d+) "),
    "gameplay_bursts": re.compile(r"psxtrace: gameplay burst: region=\d+ \((\S+)\) opened at record (\d+) "),
}


def read_log(path):
    """Return (ratchet file, table key, {class: sorted events}) from a verifier log.

    Exits 2 when the log has no ratchet load line naming a file or did not
    reach the trace's end."""
    text = Path(path).read_text(errors="replace")
    m = None
    for m in LOAD_RE.finditer(text):
        pass
    if not m:
        sys.exit(f"oracle_ratchet: {path}: no `ratchet: FILE [KEY]` line (a strict row has no table)")
    if "openpete-exit: reason=psxtrace-eof" not in text:
        sys.exit(f"oracle_ratchet: {path}: the run did not reach the trace's end")
    events = {c: set() for c in CLASSES}
    for line in text.splitlines():
        for c, rx in EVENT_RE.items():
            e = rx.search(line)
            if not e:
                continue
            events[c].add(f"{e.group(1)}@{e.group(2)}" if c == "gameplay_bursts" else int(e.group(1)))
    return m.group(1), m.group(2), {c: sort_class(c, v) for c, v in events.items()}


def sort_class(cls, values):
    """Sort a class's entries by record, then region."""
    if cls != "gameplay_bursts":
        return sorted(values)
    return sorted(values, key=lambda s: (int(s.rsplit("@", 1)[1]), s))


def split_file(path):
    """Split the ratchet file into its header lines and {key: (comment lines, table dict)} in file order."""
    lines = Path(path).read_text().splitlines() if Path(path).exists() else []
    data = tomllib.loads("\n".join(lines)) if lines else {}
    header, tables, pending, key = [], {}, [], None
    for line in lines:
        h = re.match(r"^\[([^\]]+)\]\s*$", line)
        if h:
            key = h.group(1).strip().strip('"')
            tables[key] = (pending, data.get(key, {}))
            pending = []
        elif key is None:
            header.append(line)
        elif line.startswith("#"):
            pending.append(line)
    return header, tables


def format_list(cls, values):
    """Format one class's TOML array, eight records or four bursts per line."""
    if not values:
        return f"{cls} = []"
    items = [f'"{v}"' if cls == "gameplay_bursts" else str(v) for v in values]
    per = 4 if cls == "gameplay_bursts" else 8
    rows = [", ".join(items[i:i + per]) for i in range(0, len(items), per)]
    return f"{cls} = [\n    " + ",\n    ".join(rows) + ",\n]"


def write_file(path, header, tables):
    """Write the header and every table, classes in fixed order."""
    out = list(header)
    while out and not out[-1].strip():
        out.pop()
    for key, (comments, table) in tables.items():
        out.append("")
        out.extend(comments)
        out.append(f"[{key}]")
        for c in CLASSES:
            out.append(format_list(c, sort_class(c, table.get(c, []))))
    Path(path).write_text("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("verb", choices=("tighten", "show"))
    ap.add_argument("log")
    ap.add_argument("--accept", metavar="REASON", help="admit the log's new events, recording REASON")
    a = ap.parse_args()
    path, key, seen = read_log(a.log)
    header, tables = split_file(path)
    comments, table = tables.get(key, ([], {}))
    listed = {c: set(table.get(c, [])) for c in CLASSES}
    new = {c: [v for v in seen[c] if v not in listed[c]] for c in CLASSES}
    stale = {c: sort_class(c, listed[c] - set(seen[c])) for c in CLASSES}
    for c in CLASSES:
        print(f"[{key}] {c}: {len(seen[c])} event(s); new {new[c] or '-'}; stale {stale[c] or '-'}")
    if a.verb == "show":
        return 0
    if any(new.values()) and not a.accept:
        print("oracle_ratchet: the log holds events the table does not list; fix them, or admit them "
              "with --accept REASON", file=sys.stderr)
        return 1
    if a.accept:
        comments = comments + [f"# accepted: {a.accept}"]
        table = {c: seen[c] for c in CLASSES}
    else:
        table = {c: [v for v in table.get(c, []) if v in set(seen[c])] for c in CLASSES}
    tables[key] = (comments, table)
    write_file(path, header, tables)
    print(f"oracle_ratchet: wrote {path} [{key}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
