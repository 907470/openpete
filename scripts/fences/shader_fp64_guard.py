#!/usr/bin/env python3
"""Refuse fp64 in the RHI shaders.

The terrain cache's vertex stages run in fp32 on every device, and the
device is created without shaderFloat64, so a SPIR-V module that declares
the Float64 capability is invalid on every device.

  capability STAMP SPV...
      Primary check, on the compiled output: parse each module's
      OpCapability instructions and fail on Float64 in any module. A
      capability scan cannot match a comment or miss a spelling. Writes
      STAMP on success.

  source SHADER_DIR STAMP
      Secondary check, on the source, naming the line for the author: every
      .vert, .frag and .glsl under SHADER_DIR with comments stripped,
      searched for the fp64 spellings. A source line carrying the marker
      `fp64-fence: allow` is exempt; the reason is written beside it.
      Writes STAMP on success.

Both modes print their engagement (modules or lines scanned) and refuse to
pass at zero or when the five terrain vertex stages were not all scanned.
"""
import os
import re
import struct
import sys

TERRAIN_STAGES = {
    "terrain.vert.spv", "terrain_anim.vert.spv", "terrain_near.vert.spv",
    "terrain_cells_anim.vert.spv", "terrain_cells_near.vert.spv",
}
OP_CAPABILITY = 17
CAP_FLOAT64 = 10
PATTERN = re.compile(r"double|dvec|dmat|f64vec|[0-9](lf|LF)\b|packDouble2x32|"
                     r"unpackDouble2x32|gpu_shader_fp64|float64", re.IGNORECASE)
MARKER = "fp64-fence: allow"


def capabilities(path):
    """Return the capability operands a SPIR-V module declares."""
    data = open(path, "rb").read()
    if len(data) < 20 or len(data) % 4:
        raise ValueError(f"{path}: not a SPIR-V module")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise ValueError(f"{path}: bad SPIR-V magic")
    caps, i = [], 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:
            raise ValueError(f"{path}: zero-length instruction at word {i}")
        if op == OP_CAPABILITY:
            caps.append(words[i + 1])
        i += count
    return caps


def check_capability(stamp, spvs):
    """Fail on Float64 in any module; return the exit code."""
    bad, scanned, terrain_seen = [], 0, set()
    for path in spvs:
        name = os.path.basename(path)
        scanned += 1
        if name in TERRAIN_STAGES:
            terrain_seen.add(name)
        if CAP_FLOAT64 in capabilities(path):
            bad.append(name)
    print(f"shader-fp64 capability: {scanned} modules scanned, "
          f"{len(terrain_seen)}/{len(TERRAIN_STAGES)} terrain vertex stages")
    if scanned == 0 or terrain_seen != TERRAIN_STAGES:
        print("shader-fp64 capability: VACUOUS - the terrain vertex stages were not all scanned",
              file=sys.stderr)
        return 1
    if bad:
        for name in bad:
            print(f"shader-fp64 capability: {name} declares OpCapability Float64, "
                  f"which no device enables (shaderFloat64 is never requested)", file=sys.stderr)
        return 1
    open(stamp, "w").write("ok\n")
    return 0


def strip_comments(text):
    """Blank out /* */ and // comments, keeping newlines so line numbers hold."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def source_lines(path):
    """Return a file's lines, read once per path."""
    cache = source_lines.cache
    if path not in cache:
        with open(path) as f:
            cache[path] = f.read().split("\n")
    return cache[path]


source_lines.cache = {}


def allowed(path, lineno):
    """Return whether a source line carries the escape marker."""
    lines = source_lines(path)
    return 0 < lineno <= len(lines) and MARKER in lines[lineno - 1]


def scan_raw(path, hits):
    """Scan one file with comments stripped; return the lines scanned."""
    lines = strip_comments(open(path).read()).split("\n")
    for k, line in enumerate(lines, 1):
        if PATTERN.search(line) and not allowed(path, k):
            hits.append(f"{path}:{k}: {source_lines(path)[k - 1].strip()}")
    return len(lines)


def check_source(shader_dir, stamp):
    """Fail on an fp64 spelling in the shader sources; return the exit code."""
    hits, scanned, files = [], 0, 0
    for name in sorted(os.listdir(shader_dir)):
        if not name.endswith((".vert", ".frag", ".glsl")):
            continue
        scanned += scan_raw(os.path.join(shader_dir, name), hits)
        files += 1
    print(f"shader-fp64 source: {files} sources, {scanned} lines scanned")
    if scanned == 0 or not os.path.isfile(os.path.join(shader_dir, "terrain.vert")):
        print("shader-fp64 source: VACUOUS - terrain.vert not scanned", file=sys.stderr)
        return 1
    if hits:
        for h in hits:
            print(f"shader-fp64 source: {h}", file=sys.stderr)
        print(f"shader-fp64 source: {len(hits)} fp64 spellings; "
              f"mark a deliberate one `{MARKER}` with the reason", file=sys.stderr)
        return 1
    open(stamp, "w").write("ok\n")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "capability":
        return check_capability(argv[2], argv[3:])
    if len(argv) == 4 and argv[1] == "source":
        return check_source(argv[2], argv[3])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
