"""Fence: a scripted launch runs inside a sandbox root.

A game run writes into the player's directories by default: it rotates
openpete.log, writes the hang watchdog's files over the last ones, puts a
bug-report bundle among the player's own, saves settings into openpete.toml,
and fills the Vulkan pipeline cache. A scripted run that does this still
passes, so nothing in its output shows what it overwrote.

The rule is one setting: a launcher runs with OPENPETE_SANDBOX naming a
scratch root, under which the game resolves every per-user directory and
refuses to start when a write path lies in a real one. A shell launcher
sources scripts/lib/sandbox.sh, or a library that sources it
(scripts/lib/scratch_cards.sh, scripts/presentation-game.sh,
scripts/lib/game.sh, and any other shell file found sourcing one of them);
a Python launcher calls hostenv's sandbox(), sandbox_run_root() or
scratch_cards(); either may set OPENPETE_SANDBOX itself.

Scope: every file that launches the game binary, whatever its name, since a
sandbox is owed by every scripted run: rumble_guard's `launches` (the binary
named by path), and a Python file that spawns a process and builds an argv
list or tuple holding two or more of the game's own flags (GAME_FLAGS), which
catches the launchers that take the binary from --exe/--bin or join it from a
build directory. A file holding one such flag is not counted: the DuckStation
measurement tools pass a lone --disc or --bk2 to the reference emulator, and
a driver hands a lone --bk2 to a sandboxed sub-harness.

Escape: `sandbox-guard: intentional <reason>` for a script that needs the
player's real state or a fresh-user boot (a first-boot check, a bench on the
player's toml, the playtest build), the reason written after the marker;
`sandbox-guard: no-launch` for a file that names the binary and never starts
it.

This file names the knob and the binaries without launching anything, so:
wslenv-guard: no-launch, rumble-guard: no-launch, input-lock-guard: no-launch

Run: python3 scripts/fences/sandbox_guard.py [root ...]
Exit: 0 clean, 65 a launcher runs outside a sandbox.
"""
import ast
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rumble_guard                                         # noqa: E402

SKIP_DIRS = rumble_guard.SKIP_DIRS
SUFFIXES = rumble_guard.SUFFIXES
OPENPETE = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

BASE_PROVIDER = "sandbox.sh"

ENV_SET = re.compile(r"""
      \bOPENPETE_SANDBOX\s*=\s*["']?[\w$/.{~-]   # shell assignment or env word
    | OPENPETE_SANDBOX['"]\]\s*=                 # python dict assignment
    | OPENPETE_SANDBOX['"]\s*[:,]                # python dict literal, setdefault
""", re.X)
PY_CALL = re.compile(r"\b(?:sandbox|sandbox_run_root|scratch_cards)\(")

# The reason is required: a bare marker is not a decision anyone wrote down.
EXEMPT = re.compile(r"sandbox-guard:\s*(?:intentional[\s#:(-]*\w+(?:[\s#]+[\w']+)+|no-launch)")


# Flags only the game binary takes; two distinct ones in argv literals mark
# a Python launcher.
GAME_FLAGS = frozenset((
    "--headless", "--skip-to-level", "--frame-limit", "--bk2", "--screenshot-on-frame",
    "--screenshot-on-tick", "--freecam-pose", "--freecam-path", "--no-config", "--disc",
    "--record-cfr"))
# Calls that start a process: subprocess.run/call/check_call/check_output/
# Popen and the os.exec*/os.spawn* families, by attribute or bare name.
SPAWN_CALLS = frozenset(("run", "call", "check_call", "check_output", "Popen", "execv",
                         "execve", "execvp", "execvpe", "spawnv", "spawnve", "spawnvp",
                         "spawnvpe"))


def py_launches(text):
    """True when Python source spawns a process and its list and tuple
    literals hold at least two distinct GAME_FLAGS. Argparse definitions
    (`add_argument("--disc")`) and membership tests (`"--card1" in argv`)
    are not list literals, so a script's own options never count."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return False
    spawn, flags = False, set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            f = node.func
            name = f.attr if isinstance(f, ast.Attribute) else getattr(f, "id", "")
            spawn = spawn or name in SPAWN_CALLS
        elif isinstance(node, (ast.List, ast.Tuple)):
            flags.update(e.value for e in node.elts
                         if isinstance(e, ast.Constant) and e.value in GAME_FLAGS)
    return spawn and len(flags) >= 2


def launches(name, text):
    """True when the file starts the game: rumble_guard's path match, or a
    Python file py_launches accepts."""
    if rumble_guard.launches(name, text):
        return True
    return name.endswith(".py") and py_launches(text)


def source_re(names):
    alt = "|".join(re.escape(n) for n in sorted(names))
    return re.compile(r"(?:^|[;&|]|\bthen|\bdo)\s*(?:source|\.)\s+[^\n;&|]*?(?<![\w.-])(?:%s)\b" % alt,
                      re.M)


def providers(roots=(OPENPETE,)):
    """Basenames of the shell files that source sandbox.sh, directly or
    through another such file: sandbox.sh itself and the fixed point over the
    shell files under roots."""
    texts = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                if name.endswith((".sh", ".bash")):
                    try:
                        with open(os.path.join(dirpath, name), encoding="utf-8",
                                  errors="replace") as fh:
                            texts.setdefault(name, []).append(fh.read())
                    except OSError:
                        pass
    found = {BASE_PROVIDER}
    changed = True
    while changed:
        changed = False
        pat = source_re(found)
        for name, bodies in texts.items():
            if name in found:
                continue
            if any(pat.search(rumble_guard.code_text(name, b)) for b in bodies):
                found.add(name)
                changed = True
    return found


_PROVIDERS = None


def default_providers():
    global _PROVIDERS
    if _PROVIDERS is None:
        _PROVIDERS = providers()
    return _PROVIDERS


def sandboxed(name, text, provs=None):
    """True when the file's code enters a sandbox: sources a provider (shell),
    calls a hostenv sandbox helper (Python), or sets OPENPETE_SANDBOX."""
    code = rumble_guard.code_text(name, text)
    if ENV_SET.search(code):
        return True
    if name.endswith(".py"):
        return PY_CALL.search(code) is not None
    return source_re(provs or default_providers()).search(code) is not None


def offends(name, text, provs=None):
    """True when the file launches the game and never enters a sandbox."""
    if not launches(name, text):
        return False
    if EXEMPT.search(text):
        return False
    return not sandboxed(name, text, provs)


def scan(root, provs):
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
            if offends(name, text, provs):
                bad.append(os.path.relpath(path, root) if root != "/" else path)
    return bad


def main():
    roots = [os.path.abspath(r) for r in sys.argv[1:]] or [OPENPETE]
    provs = providers(tuple(dict.fromkeys([OPENPETE] + roots)))
    bad = []
    for root in roots:
        bad += [os.path.join(root, b) if len(roots) > 1 else b for b in scan(root, provs)]
    if not bad:
        print("fence (sandbox_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (sandbox_guard): these launchers run the game outside a sandbox, "
        "so a run can rotate the player's log, overwrite hang files, write bug "
        "bundles, cards or settings into the player's directories:\n")
    for path in bad:
        sys.stderr.write(f"  {path}\n")
    sys.stderr.write(
        "\nFix: `source scripts/lib/sandbox.sh` (or a library that sources it: "
        "scratch_cards.sh, presentation-game.sh, game.sh) near the top of a "
        "shell script; `hostenv.sandbox()` in Python. A Windows exe receives "
        "the root through scripts/lib/wslenv.sh or hostenv.child_env. A script "
        "that needs the player's real state adds `sandbox-guard: intentional "
        "<reason>`; one that never starts the game adds "
        "`sandbox-guard: no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
