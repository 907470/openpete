"""Fence: a scripted launch must never rumble a connected controller.

Vibration ships ON by default, which is right for a player and wrong for every
other way this binary starts. Harnesses, capture scripts, gate batteries and
bench runs are scripted; no player uses the pad. Vibration in a scripted run
is invisible in every log - the run still passes, the numbers are still
right, so nothing ever surfaces it.

Two layers already handle the common cases in code: headless never arms the
motors at all, and the pump-side apply gates on window focus, so an unfocused
window is silent whatever the config says. What those do not cover is a
windowed, focused, scripted run - a visual check, a screenshot capture that
brings a window up, a perf bench the author is watching. That is what this
fence is for.

The rule is one setting: an unattended launcher silences vibration, either
with `--vibration 0` on the invocation or `OPENPETE_VIBRATION=0` for the whole
script. The env spelling is usually the better of the two here - a harness
builds its argument list in several places, and a flag appended to the wrong
one of them is a silent miss.

The WSL caveat is real but belongs to another fence: an `OPENPETE_*` export
does not cross into a Windows .exe unless WSLENV names it, so a script that
launches the Windows build and sets the env spelling must go through
scripts/lib/wslenv.sh. That is exactly what wslenv_guard.py already enforces,
so the two fences compose: this one says the knob must be set, that one says
it must actually arrive.

This file names the knob and talks about Windows binaries without ever
launching one, so: wslenv-guard: no-launch

Run: python3 scripts/fences/rumble_guard.py [root]
Exit: 0 clean, 65 a script could rumble a connected controller.
"""
import io
import os
import re
import shlex
import sys
import tokenize

# A file only matters if it names the game binary: a per-game name
# (`openpete-spyro2`, `openpete-spyro1.exe`), the Windows executable
# `openpete.exe` after any path separator, or the one executable `openpete`
# as a path under a build tree or a variable (`build/linux-release/openpete`,
# `build/windows-mingw/openpete.exe`, `"$W/openpete.exe"`, `... / "openpete"`),
# never a longer name that starts with it (openpete-recomp-linkage.txt,
# openpete.toml) and never a directory (`$ROOT/openpete/scripts`). Matching a
# bare `$EXE` swept in icon converters and a toolchain wrapper - a fence whose
# output nobody can read is a fence nobody runs.
LAUNCH = re.compile(r"""openpete[-_]spyro\d(?:\.exe)?\b
    | [/\\]openpete\.exe\b
    | (?:build|BUILD[A-Z_]*\}?|linux-release[\w-]*|windows-mingw[\w-]*|\$\{?\w+\}?)
      /openpete(?:\.exe)?(?![\w./-])
    | /\s*["']openpete(?:\.exe)?["']""", re.X)

# ...and only the UNATTENDED launchers are in scope. A scripted run is the
# hazard: no player uses the pad, and the run passes either way so the
# buzzing never surfaces in any log. An interactive one-off typed at a
# terminal is not this fence's business.
#
# The other two layers cover the rest and are enforced in code, not here:
# headless never arms the motors, and the pump-side apply gates on window
# focus, so a background window is silent whatever its arguments say. What
# neither covers is an unattended script that brings up a focused window,
# which is exactly the set below.
UNATTENDED = re.compile(r"^(check_|bench-|capture_|.*-corpus|.*_harness)")

# The env spelling silences every launch of the file (and of the processes it
# starts); scripts/lib/game.sh's pg_capture sets it on every run it makes.
ENV_SANCTIONED = re.compile(r"""
      \bOPENPETE_VIBRATION\s*=\s*0\b  # shell
    | OPENPETE_VIBRATION"\s*,\s*"0    # python setdefault
    | OPENPETE_VIBRATION['"]\]\s*=\s*['"]0   # ...and python dict assignment
    | OPENPETE_VIBRATION['"]\s*[:,]\s*['"]0   # ...and a python dict literal
""", re.X)

# The flag silences the launch it is passed to, in any of the spellings a
# script might build it with.
FLAG = re.compile(r"""
      --vibration[=\s]+["']?0\b       # the flag, passed directly
    | --vibration[=\s]+"?\$           # the flag, forwarded from a variable
    | --vibration['"]\s*,\s*['"]0    # the flag, as python argv list members
""", re.X)

SANCTIONED = re.compile(ENV_SANCTIONED.pattern + "|" + FLAG.pattern, re.X)

# The escape hatch, for the one kind of script that SHOULD rumble: a hands-on
# feel check, where a human is deliberately holding the pad. Spelled out so it
# is a decision someone made rather than a pattern that quietly matched.
EXEMPT = re.compile(r"rumble-guard:\s*(intentional|no-launch)")

# "worktrees" holds git worktrees of this repo: whole copies whose scripts
# predate or duplicate the working tree's. Only the working tree is scanned.
SKIP_DIRS = {".git", "worktrees", "build", "__pycache__", "node_modules",
             "third_party", "dist"}
SUFFIXES = (".sh", ".py", ".bash")


SHELL_SUFFIXES = (".sh", ".bash")
# A shell assignment: `NAME=`, `NAME+=`, after export/local/declare and its
# options; group 3 is the value to the end of the logical line.
SH_ASSIGN = re.compile(r"""(?:^\s*|[;&|(]\s*|\b(?:export|local|readonly|declare|typeset)
    (?:\s+-\w+)*\s+)([A-Za-z_]\w*)(\+?=)(.*)""", re.X)
# Words that may precede the executed word of a command: wrappers, their
# numeric arguments, options, assignments and array expansions of a prefix.
PREFIX_WORD = re.compile(r"""(?:env|exec|command|nohup|setsid|time|nice|timeout
    |stdbuf|ionice|taskset|sudo|xvfb-run|op_own_run|!|then|do|else
    |[\d.]+[smhd]?|-\S*|[A-Za-z_]\w*=\S*|"?\$\{\w+\[@\]\}"?)$""", re.X)
# Text after a binary that marks it as executed: an option or forwarded
# arguments.
ARGS_AFTER = re.compile(r"""["'}]*\s+["']?(?:-|\$@|\$\{\w+\[@\]\})""")


def code_text(name, text):
    """`text` without comments: shell `#` comments, or Python comments and
    docstrings (a string that is a statement by itself)."""
    if name.endswith(".py"):
        try:
            toks = list(tokenize.generate_tokens(io.StringIO(text).readline))
        except (tokenize.TokenError, IndentationError, SyntaxError):
            return text
        keep = []
        opener = (tokenize.NEWLINE, tokenize.NL, tokenize.INDENT, tokenize.DEDENT)
        for n, t in enumerate(toks):
            if t.type == tokenize.COMMENT:
                continue
            if t.type == tokenize.STRING:
                prev = next((p for p in reversed(toks[:n]) if p.type != tokenize.COMMENT
                             and p.type != tokenize.NL), None)
                nxt = toks[n + 1] if n + 1 < len(toks) else None
                if (prev is None or prev.type in opener) and \
                        (nxt is None or nxt.type in (tokenize.NEWLINE, tokenize.ENDMARKER)):
                    continue
            keep.append(t)
        return tokenize.untokenize(keep)
    return "\n".join(re.sub(r"(?:^|(?<=\s))#.*", "", line)
                     for line in text.splitlines())


def logical_lines(text):
    """Shell lines with backslash continuations joined."""
    out, cur = [], ""
    for line in text.splitlines():
        if line.endswith("\\"):
            cur += line[:-1] + " "
            continue
        out.append(cur + line)
        cur = ""
    if cur:
        out.append(cur)
    return out


def var_ref(name):
    return re.compile(r"\$\{?%s\b(?:\[@\])?\}?" % re.escape(name))


def is_definition(line):
    """True for a line that only assigns variables (`BIN=...`,
    `local -a cmd=(...)`): a binary named there is not executed there."""
    try:
        words = shlex.split(line)
    except ValueError:
        words = line.split()
    while words and words[0] in ("export", "local", "readonly", "declare", "typeset"):
        words = words[1:]
        while words and words[0].startswith("-"):
            words = words[1:]
    if not words or not re.match(r"[A-Za-z_]\w*\+?=", words[0]):
        return False
    if re.match(r"[A-Za-z_]\w*\+?=\(", words[0]):
        return True
    return all(re.match(r"[A-Za-z_]\w*\+?=", w) for w in words)


def shell_launches(lines):
    """(binary vars, cleared vars, [(line, start, end)] launch sites): a site
    is the binary (a LAUNCH path or a variable holding it) at a command's
    executed position or followed by arguments."""
    assigns = []
    for line in lines:
        for m in SH_ASSIGN.finditer(line):
            assigns.append((m.group(1), m.group(3)))
    binary, cleared = set(), set()
    changed = True
    while changed:
        changed = False
        for var, val in assigns:
            if var not in binary and (LAUNCH.search(val) or
                                      any(var_ref(b).search(val) for b in binary)):
                binary.add(var)
                changed = True
            if var not in cleared and (FLAG.search(val) or
                                       any(var_ref(c).search(val) for c in cleared)):
                cleared.add(var)
                changed = True
    sites = []
    for line in lines:
        if is_definition(line):
            continue
        spans = [m.span() for m in LAUNCH.finditer(line)]
        for b in binary:
            spans += [m.span() for m in var_ref(b).finditer(line)]
        for start, end in spans:
            head = line[:start]
            head = re.split(r"\$\(|[;&|(`]", head)[-1]
            head = re.sub(r"[\"']$", "", head.strip())
            at_command = all(PREFIX_WORD.match(w) for w in head.split())
            if at_command or ARGS_AFTER.match(line, end):
                sites.append((line, start, end))
    return binary, cleared, sites


QUERY = re.compile(r"\s*(?:\[\[?|test|echo|printf|:)\s")


def passed_on(lines, binary):
    """True when a variable holding the binary is handed to another command
    (`run "$EXE" log`, a function that launches its argument), outside
    definitions and path queries (`[ -x "$EXE" ]`, `echo "$EXE"`)."""
    return any(not is_definition(line) and not QUERY.match(line) and
               any(var_ref(b).search(line) for b in binary) for line in lines)


def shell_offends(code):
    """True when a launch site in shell code carries no --vibration 0 (on
    its line or in a variable it expands). A file that only names the binary
    (`echo "$1/openpete.exe"`, `[ -x "$BIN" ]`) has no site; one that hands
    the binary to a function has no site the guard can follow and needs the
    flag somewhere in its code."""
    lines = logical_lines(code)
    binary, cleared, sites = shell_launches(lines)
    if not sites:
        return passed_on(lines, binary) and FLAG.search(code) is None
    for line, _s, _e in sites:
        if FLAG.search(line) or any(var_ref(c).search(line) for c in cleared):
            continue
        return True
    return False


def launches(name, text):
    """True when a file starts the game binary: a shell file with a launch
    site, any other file naming the binary outside comments and docstrings."""
    if not LAUNCH.search(text):
        return False
    code = code_text(name, text)
    if name.endswith(SHELL_SUFFIXES):
        lines = logical_lines(code)
        binary, _cleared, sites = shell_launches(lines)
        return bool(sites) or passed_on(lines, binary)
    return LAUNCH.search(code) is not None


def offends(name, text, require_unattended=True):
    """One file's verdict: True when it launches the binary unattended and
    never silences vibration. A caller that already knows the file is a
    launcher passes require_unattended=False: the naming convention is a
    tree-scan heuristic for spotting harnesses among a repo's other scripts,
    and it misses a harness whose name does not follow it.

    Deliberately NOT exempting headless-only scripts. Whether a given
    invocation is headless cannot be read reliably out of shell that builds
    its arguments in arrays across a dozen lines, and guessing wrong here
    means the fence passes the one file worth catching. Passing
    --vibration 0 on a headless run costs nothing and makes the intent
    explicit at the launch site instead of implicit in a gate three layers
    down."""
    if not LAUNCH.search(text):
        return False
    if require_unattended and not UNATTENDED.match(name):
        return False
    if EXEMPT.search(text) or ENV_SANCTIONED.search(text):
        return False
    code = code_text(name, text)
    if name.endswith(SHELL_SUFFIXES):
        return shell_offends(code)
    return LAUNCH.search(code) is not None and FLAG.search(code) is None


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if not name.endswith(SUFFIXES):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            if offends(name, text):
                bad.append(os.path.relpath(path, root))
    return bad


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    root = os.path.abspath(root)
    bad = scan(root)
    if not bad:
        print("fence (rumble_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (rumble_guard): these unattended launchers never silence "
        "vibration, so a scripted run can buzz a connected controller:\n")
    for path in bad:
        sys.stderr.write(f"  {path}\n")
    sys.stderr.write(
        "\nFix: `export OPENPETE_VIBRATION=0` near the top of the script (one "
        "line, covers every invocation it builds), or `--vibration 0` on the "
        "launch itself. A script that launches the WINDOWS build with the env "
        "spelling must also forward it through scripts/lib/wslenv.sh, or it "
        "never arrives - wslenv_guard.py enforces that half. If the script is "
        "a deliberate hands-on feel check, or never launches the game, add "
        "the marker `rumble-guard: intentional` or `rumble-guard: "
        "no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
