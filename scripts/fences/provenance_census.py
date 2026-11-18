#!/usr/bin/env python3
"""Fence: the tree names other emulators as tools, never their internals.

The PSX oracle is a patched DuckStation test binary, and PCSX-Redux is the
older forensic reference. openpete may name either program as a tool it runs:
its name, its binaries, the flags and scripting calls openpete passes, the
patch series and recorders openpete wrote, the data they record, the pinned
revision and the licence. openpete text never carries another program's
internals: an upstream source path or line, an upstream function, class,
variable or constant name, or a statement of how that program computes a
value. A behaviour of the reference is stated as a measurement with its
instrument, and a hardware fact with its public document (psx-spx).

Two rules, applied per line:
  token  a whole-word identifier from TOKENS: an internal name of one of
         those programs, or of code openpete once carried that tracked one
         (each entry names its owner);
  form   a line naming one of those programs (NAMES) that also carries an
         upstream-source form (FORMS: a source file with a line number, an
         upstream source directory, a C++ qualified name).
A hit is allowed when ALLOW holds a row for its path and line, each row with
the reason that use is a tool use or an attributed licence use. A line
carrying `provenance-census: <reason>` is exempt; the reason is required.

Scope: every tracked text file, except the patch series (its files name the
hook points they attach to), third-party trees openpete vendors unchanged,
the decomp checkouts, and this fence's own files (they hold the token list).
third_party/psxrecomp is in scope: openpete edits it.

Run:
  python3 scripts/fences/provenance_census.py tree [root] [--rev REV]   every tracked file (build target)
  python3 scripts/fences/provenance_census.py hook [root]               the staged added lines
  python3 scripts/fences/provenance_census.py census [root] [--rev REV] also list the allowed hits per row
Exit: 0 clean, 66 a refused line.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import fnmatch
import os
import re
import subprocess
import sys

MARKER = re.compile(r"provenance-census:\s*(.*?)\s*(?:\*/|-->)?\s*$")

# (token, owner). Identifiers specific to one program; a generic hardware or
# programming term is never listed (false positives teach people to bypass).
TOKENS = (
    # DuckStation source: files, classes, functions, constants.
    ("AnalogController", "duckstation"),
    ("GetMotorStrength", "duckstation"),
    ("DEFAULT_LARGE_MOTOR_VIBRATION_BIAS", "duckstation"),
    ("DEFAULT_SMALL_MOTOR_VIBRATION_BIAS", "duckstation"),
    ("analog_controller", "duckstation"),
    ("sdl_input_source", "duckstation"),
    ("gpu_sw_rasterizer", "duckstation"),
    ("ATTRIB_DETERMINANT", "duckstation"),
    ("ATTRIB_STEP", "duckstation"),
    ("UVStepper", "duckstation"),
    ("UpdateADSREnvelope", "duckstation"),
    ("GetNextADSRPhase", "duckstation"),
    ("TickADSR", "duckstation"),
    ("VolumeEnvelope", "duckstation"),
    ("regtest_host", "duckstation"),
    ("cpu_core", "duckstation"),
    ("timing_event", "duckstation"),
    ("TimingEvents", "duckstation"),
    ("GetGlobalTickCounter", "duckstation"),
    ("GetPendingTicks", "duckstation"),
    ("CommitGlobalTicks", "duckstation"),
    ("AddBreakpointWithCallback", "duckstation"),
    ("UpdateDebugDispatcherFlag", "duckstation"),
    ("CheckForExecutionModeChange", "duckstation"),
    ("OpenGLContextEGL", "duckstation"),
    ("opengl_context_egl", "duckstation"),
    ("gen_scmversion", "duckstation"),
    ("NTSC_TICKS_PER_LINE_PAIR", "duckstation"),
    ("g_instruction_hook", "duckstation (hook the patch series adds)"),
    ("g_exception_hook", "duckstation (hook the patch series adds)"),
    ("g_command_hook", "duckstation (hook the patch series adds)"),
    # PCSX-Redux source.
    ("r3000a", "pcsx-redux"),
    ("LWL_SHIFT", "pcsx-redux"),
    ("LWR_SHIFT", "pcsx-redux"),
    ("InterpretedCPU", "pcsx-redux"),
    ("execBlock", "pcsx-redux"),
    ("pcsxffi", "pcsx-redux"),
    # Names of openpete code once written from the reference's source.
    ("ds_line", "openpete, retired line rasterizer"),
    ("dxdk", "openpete, retired line rasterizer"),
    ("dydk", "openpete, retired line rasterizer"),
    ("gpu_uv_step", "openpete, retired UV stepper"),
    ("gpu_uv_setup", "openpete, retired UV stepper"),
    ("gpu_uv_at", "openpete, retired UV stepper"),
    ("GPU_DET", "openpete, retired UV stepper"),
    ("rumble_pokopom", "openpete, retired rumble curve"),
    ("SPUAL_Voice_UpdateADSREnvelope", "PsyCross fork, retired envelope"),
    ("SPUAL_Voice_NextPhase", "PsyCross fork, retired envelope"),
    ("SPUAL_Voice_TickADSR", "PsyCross fork, retired envelope"),
    ("SPUAL_Voice_KeyOff", "PsyCross fork, retired envelope"),
    ("SPUALEnvelope", "PsyCross fork, retired envelope"),
    ("adsr_target", "PsyCross fork, retired envelope"),
    ("adsr_envelope", "PsyCross fork, retired envelope (the voice field)"),
    ("gpu_texel", "openpete, retired UV stepper"),
    ("SPUALADSRPhase", "PsyCross fork, retired envelope"),
    ("SPUAL_Envelope_Reset", "PsyCross fork, retired envelope"),
    ("SPUAL_Envelope_Begin", "PsyCross fork, retired envelope"),
    ("SPUAL_Envelope_Tick", "PsyCross fork, retired envelope"),
    ("SPUAL_Volume_Load", "PsyCross fork, retired envelope"),
    ("SPUAL_ADSR_OFF", "PsyCross fork, retired envelope"),
    ("SPUAL_ADSR_ATTACK", "PsyCross fork, retired envelope"),
    ("SPUAL_ADSR_DECAY", "PsyCross fork, retired envelope"),
    ("SPUAL_ADSR_SUSTAIN", "PsyCross fork, retired envelope"),
    ("SPUAL_ADSR_RELEASE", "PsyCross fork, retired envelope"),
    ("counter_increment", "duckstation (the retired envelope's field)"),
    ("phase_invert", "duckstation (the retired envelope's field)"),
    ("ForceOff", "duckstation"),
    ("PGXP_mem", "pcsx-redux"),
    ("Bus::g_ram", "duckstation"),
    ("AudioBackend", "duckstation"),
    ("BreakpointType", "duckstation"),
    ("ExecuteInstruction", "duckstation"),
    ("Debug::process", "pcsx-redux"),
    ("checkBP", "pcsx-redux"),
    ("m_failed", "pcsx-redux"),
    ("ffmpeg-audio-file", "pcsx-redux"),
    # Pokopom's large-motor cubic coefficients (MIT, attributed where used).
    ("0.006474549734772402", "Pokopom"),
    ("1.258165252213538", "Pokopom"),
    ("156.82454281087692", "Pokopom"),
    ("3.637978807091713e-11", "Pokopom"),
)

NAMES = re.compile(r"(?i)duckstation|pcsx|mednafen|swanstation")
FORMS = re.compile(r"\.(?:cpp|cc|inl|h|hpp|lua)\s*:\s*\d"
                   r"|(?<![\w.])src/(?:core|util|common|support|gpu|spu)/"
                   r"|\b[A-Z][A-Za-z0-9]*::[A-Za-z_]\w*")

# (path glob, line regex, reason). A hit is allowed when its path matches the
# glob and its line matches the regex.
ALLOW = (
    ("core/input/rumble_response.c", r"POKOPOM_C[0-3]\s+[-0-9.e]+$",
     "Pokopom's cubic coefficients, used under its MIT licence; the file, "
     "NOTICE.md and packaging/licences attribute them"),
    ("scripts/*.lua", r"createEventListener\(\s*['\"]GPU::Vsync['\"]",
     "the PCSX-Redux Lua scripting API's vblank event name, the interface "
     "the script calls"),
    ("scripts/dist_compliance.py", r"_ZN4PCSX3GTE",
     "the compliance scan's signature for a copyleft library that must never "
     "ship; detecting it is the purpose of the line"),
    ("third_party/psxrecomp/tools/pcsx_redux_mcp/*", r"GPU::Vsync",
     "PSXRecomp's PCSX-Redux bridge (upstream PSXRecomp text) subscribing to "
     "the Lua scripting API's vblank event"),
)

# Path prefixes outside scope, each with its reason.
EXCLUDE = (
    ("third_party/duckstation-patches/", "the patch series names the hook points it attaches to"),
    ("third_party/dear_bindings/", "vendored upstream, unchanged"),
    ("third_party/imgui/", "vendored upstream, unchanged"),
    ("third_party/glad/", "vendored upstream, unchanged"),
    ("third_party/miniz/", "vendored upstream, unchanged"),
    ("third_party/stb/", "vendored upstream, unchanged"),
    ("third_party/tomlc99/", "vendored upstream, unchanged"),
    ("third_party/volk/", "vendored upstream, unchanged"),
    ("third_party/xxhash/", "vendored upstream, unchanged"),
    ("third_party/libco/", "vendored upstream, unchanged"),
    ("scripts/fences/provenance_census.py", "this fence: the token list is its data"),
    ("scripts/fences/tests/test_provenance_census.py", "this fence's tests: planted tokens"),
)

TOKEN_RE = re.compile(r"(?<![\w.])(?:%s)(?![\w])" % "|".join(
    re.escape(t) for t, _ in sorted(TOKENS, key=lambda x: -len(x[0]))))


def in_scope(rel):
    """Whether a repo-relative path is scanned."""
    if any(rel.startswith(p) for p, _ in EXCLUDE):
        return False
    parts = rel.split("/")
    return not (len(parts) > 2 and parts[0] == "games" and parts[2] == parts[1])


def allowed(rel, line):
    """The ALLOW row index that permits this line, or None."""
    for i, (glob, rx, _) in enumerate(ALLOW):
        if fnmatch.fnmatch(rel, glob) and re.search(rx, line):
            return i
    return None


def scan_line(rel, line):
    """('refused' | 'allowed' | 'marked' | None, detail) for one line."""
    hits = [m.group(0) for m in TOKEN_RE.finditer(line)]
    if NAMES.search(line):
        hits += ["form " + m.group(0) for m in FORMS.finditer(line)]
    if not hits:
        return None, None
    m = MARKER.search(line)
    if m:
        if re.search(r"\w", m.group(1)):
            return "marked", hits
        return "refused", hits + ["marker without a reason"]
    row = allowed(rel, line)
    if row is not None:
        return "allowed", row
    return "refused", hits


def scan_text(rel, text, lines=None):
    """(refusals, allowed rows) of one file's text; lines limits the scan to those 1-based numbers."""
    refused, allow = [], []
    for n, line in enumerate(text.split("\n"), 1):
        if lines is not None and n not in lines:
            continue
        kind, detail = scan_line(rel, line)
        if kind == "refused":
            refused.append("%s:%d: %s: %s" % (rel, n, ", ".join(detail), line.strip()[:160]))
        elif kind == "allowed":
            allow.append((detail, "%s:%d" % (rel, n)))
    return refused, allow


def git(root, *args, binary=False):
    """Run git in root and return stdout."""
    p = subprocess.run(["git", "-C", root] + list(args), capture_output=True, check=True)
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def decode(data):
    """Text of a blob, or None for a binary one."""
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", "replace")


def tree_files(root, rev):
    """(rel, text) of every in-scope tracked text file, from the work tree or from rev."""
    if rev:
        rows = git(root, "ls-tree", "-r", "-z", rev).split("\0")
        for row in filter(None, rows):
            meta, rel = row.split("\t", 1)
            mode, kind, oid = meta.split()
            if kind != "blob" or mode == "120000" or not in_scope(rel):
                continue
            text = decode(git(root, "cat-file", "blob", oid, binary=True))
            if text is not None:
                yield rel, text
        return
    for rel in filter(None, git(root, "ls-files", "-z").split("\0")):
        path = os.path.join(root, rel)
        if not in_scope(rel) or os.path.islink(path) or not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            text = decode(fh.read())
        if text is not None:
            yield rel, text


def staged_lines(root):
    """{rel: {added line numbers}} of the staged diff, in-scope paths only."""
    diff = git(root, "diff", "--cached", "-U0", "--diff-filter=ACMR", "--no-color", "--no-ext-diff")
    out, rel = {}, None
    for line in diff.split("\n"):
        if line.startswith("+++ "):
            rel = line[6:] if line.startswith("+++ b/") else None
            if rel is not None and not in_scope(rel):
                rel = None
        elif line.startswith("@@") and rel is not None:
            m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
            start, count = int(m.group(1)), int(m.group(2) or "1")
            out.setdefault(rel, set()).update(range(start, start + count))
    return out


def run(mode, root, rev):
    """Scan, print the report, return (refused, allowed)."""
    refused, allow = [], []
    if mode == "hook":
        for rel, lines in staged_lines(root).items():
            if not lines:
                continue
            p = subprocess.run(["git", "-C", root, "show", ":" + rel], capture_output=True)
            text = decode(p.stdout) if p.returncode == 0 else None
            if text is not None:
                r, a = scan_text(rel, text, lines)
                refused += r
                allow += a
    else:
        for rel, text in tree_files(root, rev):
            r, a = scan_text(rel, text)
            refused += r
            allow += a
    return refused, allow


def main(argv):
    """Run the fence in tree, hook or census mode."""
    args = argv[1:]
    rev = None
    if "--rev" in args:
        i = args.index("--rev")
        rev = args[i + 1]
        del args[i:i + 2]
    mode = args[0] if args else "tree"
    root = os.path.abspath(args[1]) if len(args) > 1 else \
        os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    if mode not in ("tree", "hook", "census"):
        print(__doc__)
        return 2
    refused, allow = run(mode, root, rev)
    for r in refused:
        print("provenance: REFUSED %s" % r)
    if mode == "census":
        for i, (glob, _, reason) in enumerate(ALLOW):
            sites = [s for row, s in allow if row == i]
            print("provenance: allow[%d] %s: %d line(s): %s" % (i, glob, len(sites), reason))
            for s in sites:
                print("    %s" % s)
    print("provenance: %s (%d refused, %d allowed)" % ("FAIL" if refused else "PASS",
                                                       len(refused), len(allow)))
    if refused:
        print("provenance: name the program as a tool, state the behaviour as a measurement or "
              "a psx-spx fact, or mark the line `provenance-census: <reason>`")
    return 66 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
