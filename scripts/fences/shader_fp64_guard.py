#!/usr/bin/env python3
"""Refuse fp64 in the fp32 build of the RHI shaders.

The terrain cache's vertex stages exist in two builds: the fp64 stages
(terrain*.vert.spv) and the TERRAIN_FP32 stages (terrain*_fp32.vert.spv). A
device without shaderFloat64 runs the fp32 build, and a SPIR-V module that
declares the Float64 capability is invalid on such a device. Every other RHI
shader belongs to both builds.

  capability STAMP SPV...
      Primary check, on the compiled output: parse each module's
      OpCapability instructions and fail on Float64 in any module except the
      five fp64 terrain stages and their five TERRAIN_FP32_ARM2 builds. A capability scan cannot match a comment or
      miss a spelling. Writes STAMP on success.

  source GLSLANG SHADER_DIR STAMP
      Secondary check, on the source, naming the line for the author: every
      .vert, .frag and .glsl under SHADER_DIR with comments stripped, and
      terrain.vert preprocessed (glslangValidator -E) under each fp32 build's
      defines, searched for the fp64 spellings. A source line carrying the
      marker `fp64-fence: allow` is exempt; the reason is written beside it.
      Writes STAMP on success.

Both modes print their engagement (modules or lines scanned) and refuse to
pass at zero.
"""
import os
import re
import struct
import subprocess
import sys

FP64_STAGES = {
    "terrain.vert.spv", "terrain_anim.vert.spv", "terrain_near.vert.spv",
    "terrain_cells_anim.vert.spv", "terrain_cells_near.vert.spv",
    # TERRAIN_FP32_ARM2: the fp64 build with the fp32 position chain beside
    # it, the precision A/B's second arm; selected only where the fp64 stages
    # are (OPENPETE_TERRAIN_FP32=3).
    "terrain_arm2.vert.spv", "terrain_anim_arm2.vert.spv", "terrain_near_arm2.vert.spv",
    "terrain_cells_anim_arm2.vert.spv", "terrain_cells_near_arm2.vert.spv",
}
FP32_STAGES = {
    "terrain_fp32.vert.spv": [],
    "terrain_anim_fp32.vert.spv": ["TERRAIN_ANIM"],
    "terrain_near_fp32.vert.spv": ["TERRAIN_NEAR"],
    "terrain_cells_anim_fp32.vert.spv": ["TERRAIN_CELLS", "TERRAIN_ANIM"],
    "terrain_cells_near_fp32.vert.spv": ["TERRAIN_CELLS", "TERRAIN_NEAR"],
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
    """Fail on Float64 in any module of the fp32 build; return the exit code."""
    bad, scanned, fp32_seen = [], 0, set()
    for path in spvs:
        name = os.path.basename(path)
        if name in FP64_STAGES:
            continue
        scanned += 1
        if name in FP32_STAGES:
            fp32_seen.add(name)
        if CAP_FLOAT64 in capabilities(path):
            bad.append(name)
    print(f"shader-fp64 capability: {scanned} modules of the fp32 build scanned, "
          f"{len(fp32_seen)}/{len(FP32_STAGES)} fp32 terrain stages")
    if scanned == 0 or fp32_seen != set(FP32_STAGES):
        print("shader-fp64 capability: VACUOUS - the fp32 terrain stages were not all scanned",
              file=sys.stderr)
        return 1
    if bad:
        for name in bad:
            print(f"shader-fp64 capability: {name} declares OpCapability Float64, "
                  f"which a device without shaderFloat64 rejects", file=sys.stderr)
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


def scan_preprocessed(glslang, shader_dir, defines, hits):
    """Scan terrain.vert as one fp32 build preprocesses it; return the lines scanned."""
    src = os.path.join(shader_dir, "terrain.vert")
    args = [glslang, "-E", f"-I{shader_dir}", "-DTERRAIN_FP32=1"]
    args += [f"-D{d}=1" for d in defines] + [src]
    text = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    cur, line = src, 0
    directive = re.compile(r'^\s*#line\s+(\d+)(?:\s+"([^"]*)")?')
    lines = text.split("\n")
    for raw in lines:
        m = directive.match(raw)
        if m:
            line = int(m.group(1)) - 1
            if m.group(2):
                cur = os.path.normpath(os.path.join(shader_dir, m.group(2)))
            continue
        line += 1
        if PATTERN.search(raw) and not allowed(cur, line):
            hits.append(f"{cur}:{line}: {raw.strip()} (fp32 build, -D{' -D'.join(defines or ['TERRAIN_FP32'])})")
    return len(lines)


def check_source(glslang, shader_dir, stamp):
    """Fail on an fp64 spelling in the fp32 build's sources; return the exit code."""
    hits, scanned, files = [], 0, 0
    for name in sorted(os.listdir(shader_dir)):
        if not name.endswith((".vert", ".frag", ".glsl")) or name == "terrain.vert":
            continue
        scanned += scan_raw(os.path.join(shader_dir, name), hits)
        files += 1
    for defines in FP32_STAGES.values():
        scanned += scan_preprocessed(glslang, shader_dir, defines, hits)
        files += 1
    print(f"shader-fp64 source: {files} sources, {scanned} lines scanned")
    if scanned == 0:
        print("shader-fp64 source: VACUOUS - no lines scanned", file=sys.stderr)
        return 1
    if hits:
        for h in hits:
            print(f"shader-fp64 source: {h}", file=sys.stderr)
        print(f"shader-fp64 source: {len(hits)} fp64 spellings in the fp32 build; "
              f"mark a deliberate one `{MARKER}` with the reason", file=sys.stderr)
        return 1
    open(stamp, "w").write("ok\n")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "capability":
        return check_capability(argv[2], argv[3:])
    if len(argv) == 5 and argv[1] == "source":
        return check_source(argv[2], argv[3], argv[4])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
