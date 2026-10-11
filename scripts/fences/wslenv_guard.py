"""Fence: env knobs must actually reach a Windows .exe.

WSL interop does not hand the Linux environment to a Windows child process.
Only the names listed in WSLENV cross the boundary, and a missing name raises
nothing: the exe starts, reads the knob as unset, and runs a DIFFERENT
configuration from the one the harness asked for. Every number it then produces
is real, internally consistent, and about the wrong thing - the exact failure
shape this project keeps re-walking.

It has bitten twice. The standing pacing gate ran its whole Windows path with
OPENPETE_FORCE_PACING unset (every VBL unpaced, so the grid leg had nothing to
measure) and with OPENPETE_PACE_INJECT_PRESENT_US unset (so the fault-injection
leg was a duplicate of the baseline and still reported PASS). Separately, a
matrix harness built an env dict and never passed it to subprocess at all.

So this is mechanical now. A script that can launch a Windows binary and also
sets an OPENPETE_* variable must either go through scripts/lib/wslenv.sh (or
the Python child_env helper) or name WSLENV itself.

A Python file is checked per launch. A launch is a `subprocess` call whose
argv does not start with a string literal or `sys.executable` (a name passed
as argv resolves through the lists assigned to it in the enclosing function,
then the module): the decidable form of "argv can name a Windows exe". Every
knob the file sets reaches each such launch's `env=` through a `child_env`
argument, or the launch is refused; one `child_env` call elsewhere in the file
does not clear it. A shell file keeps the file-wide clearance below, since a
textual check cannot follow knobs passed through bash arrays or function
arguments.

Run: python3 scripts/fences/wslenv_guard.py [root]
Exit: 0 clean, 65 a script would silently drop a knob.
"""
import ast
import os
import re
import sys

# A file only matters if it can start a Windows binary at all.
WINDOWS_LAUNCH = re.compile(r"windows-mingw|windows-msvc|\.exe\b")

# Any mention of a knob makes a file a candidate. This deliberately does NOT
# try to decide whether the mention is a real launch: the assignment shape
# that caused the outage was `COMMON=(OPENPETE_FORCE_PACING=1)` inside a bash
# array, which no reasonable pattern separates from the same text inside an
# echo. A fence that half-parses shell fails exactly on the cases worth
# catching, so this one over-reports and requires the author to say which it
# is. The cost of a wrong answer here is a whole evidence set about the wrong
# configuration; the cost of an annotation is one line.
# Shell/attribute `NAME=`, and the Python spellings a harness actually uses:
# a dict literal `{"NAME": v}`, a subscript assignment `env["NAME"] = v`, and
# the call-argument form `os.environ.setdefault("NAME", v)` / `os.putenv`.
# The bare-`=` form alone missed every Python harness in the scratchpad, which
# is where harnesses get written.
KNOB = re.compile(r"""
      \bOPENPETE_[A-Z0-9_]+\s*=
    | ['"]OPENPETE_[A-Z0-9_]+['"]\s*[:\]]
    | \b(?:setdefault|putenv)\(\s*['"]OPENPETE_[A-Z0-9_]+['"]\s*,
""", re.X)

# Only an actual USE clears a file, never a mention. An earlier version
# accepted the string "WSLENV" anywhere, so a comment explaining the hazard
# exempted the very file that had it - the exemption has to be as concrete as
# the thing it excuses.
SANCTIONED = re.compile(r"""
      \bwslenv_forward\b            # sourced the shell helper and called it
    | \bchild_env\s*\(              # built the child environment properly
    | \bWSLENV\s*=                  # set it directly, deliberately (shell)
    | ['"]WSLENV['"]\s*[:\]]        # ...python dict literal or env["WSLENV"]
    | \bWSLENV_ARGS\b               # launched with what the helper produced
""", re.X)

# The escape hatch, for a file that names a knob without ever launching
# anything with it (documentation, an echo, reading a CMake cache). Spelled
# out so it is a decision someone made, not a pattern that quietly matched.
EXEMPT = re.compile(r"wslenv-guard:\s*no-launch")

SKIP_DIRS = {".git", "worktrees", "build", "__pycache__", "node_modules",
             "third_party"}


def _knob_name(node):
    """The OPENPETE_* name a string constant spells, else None."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str) \
            and re.fullmatch(r"OPENPETE_[A-Z0-9_]+", node.value):
        return node.value
    return None


_SETTERS = ("setdefault", "putenv")
_LAUNCHERS = ("run", "Popen", "call", "check_call", "check_output")


def _knobs_set(tree):
    """Every OPENPETE_* name the module sets: dict literal keys, subscript
    assignment targets, setdefault/putenv first arguments."""
    out = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Dict):
            out.update(k for k in map(_knob_name, n.keys) if k)
        elif isinstance(n, (ast.Assign, ast.AugAssign)):
            targets = n.targets if isinstance(n, ast.Assign) else [n.target]
            for t in targets:
                if isinstance(t, ast.Subscript) and _knob_name(t.slice):
                    out.add(_knob_name(t.slice))
        elif isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
                and n.func.attr in _SETTERS and n.args and _knob_name(n.args[0]):
            out.add(_knob_name(n.args[0]))
    return out


class _Scopes:
    """Name lookup for one module: the enclosing function, then the module."""

    def __init__(self, tree):
        self.tree = tree
        self.parent = {}
        for n in ast.walk(tree):
            for c in ast.iter_child_nodes(n):
                self.parent[c] = n

    def scopes(self, node):
        """The enclosing function bodies, innermost first, then the module."""
        out, n = [], node
        while n in self.parent:
            n = self.parent[n]
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Module)):
                out.append(n)
        return out or [self.tree]

    def _walk_scope(self, scope):
        """Nodes of one scope, nested function bodies excluded."""
        stack = list(ast.iter_child_nodes(scope))
        while stack:
            n = stack.pop()
            yield n
            if not isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef,
                                  ast.Lambda, ast.ClassDef)):
                stack.extend(ast.iter_child_nodes(n))

    def values(self, name, at):
        """The expressions assigned to `name` in the innermost scope that
        assigns it, and that scope's nodes (for subscript and method writes)."""
        for scope in self.scopes(at):
            nodes = list(self._walk_scope(scope))
            vals = [n.value for n in nodes if isinstance(n, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == name
                            for t in n.targets)]
            if vals:
                return vals, nodes
        return [], []


def _argv_safe(expr, sc, depth=0):
    """True when argv's first word is a string literal or sys.executable."""
    if depth > 4:
        return False
    if isinstance(expr, (ast.List, ast.Tuple)):
        if not expr.elts:
            return False
        first = expr.elts[0]
        if isinstance(first, ast.Constant) and isinstance(first.value, str):
            return True
        return (isinstance(first, ast.Attribute) and first.attr == "executable"
                and isinstance(first.value, ast.Name) and first.value.id == "sys")
    if isinstance(expr, ast.BinOp) and isinstance(expr.op, ast.Add):
        return _argv_safe(expr.left, sc, depth + 1)
    if isinstance(expr, ast.Name):
        vals, _ = sc.values(expr.id, expr)
        return bool(vals) and all(_argv_safe(v, sc, depth + 1) for v in vals)
    return False


def _dict_keys(expr, sc, depth=0):
    """The OPENPETE_* keys an expression building a dict can carry (union
    over branches; a name resolves through its assignments and its subscript,
    update and setdefault writes in the same scope)."""
    if expr is None or depth > 6:
        return set()
    if isinstance(expr, ast.Dict):
        out = set(k for k in map(_knob_name, expr.keys) if k)
        for k, v in zip(expr.keys, expr.values):
            if k is None:
                out |= _dict_keys(v, sc, depth + 1)
        return out
    if isinstance(expr, ast.IfExp):
        return _dict_keys(expr.body, sc, depth + 1) | _dict_keys(expr.orelse, sc, depth + 1)
    if isinstance(expr, ast.BoolOp):
        return set().union(*(_dict_keys(v, sc, depth + 1) for v in expr.values))
    if isinstance(expr, ast.Call) and isinstance(expr.func, ast.Name) \
            and expr.func.id == "dict":
        out = set().union(*(_dict_keys(a, sc, depth + 1) for a in expr.args)) \
            if expr.args else set()
        out.update(kw.arg for kw in expr.keywords
                   if kw.arg and re.fullmatch(r"OPENPETE_[A-Z0-9_]+", kw.arg))
        return out
    if isinstance(expr, ast.Name):
        vals, nodes = sc.values(expr.id, expr)
        out = set().union(*(_dict_keys(v, sc, depth + 1) for v in vals)) if vals else set()
        for n in nodes:
            if isinstance(n, ast.Assign):
                for t in n.targets:
                    if isinstance(t, ast.Subscript) and isinstance(t.value, ast.Name) \
                            and t.value.id == expr.id and _knob_name(t.slice):
                        out.add(_knob_name(t.slice))
            elif isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) \
                    and isinstance(n.func.value, ast.Name) and n.func.value.id == expr.id:
                if n.func.attr == "update" and n.args:
                    out |= _dict_keys(n.args[0], sc, depth + 1)
                elif n.func.attr == "setdefault" and n.args and _knob_name(n.args[0]):
                    out.add(_knob_name(n.args[0]))
        return out
    return set()


def _env_reach(expr, sc, depth=0):
    """The knobs an `env=` value forwards through child_env."""
    if expr is None or depth > 4:
        return set()
    if isinstance(expr, ast.Call):
        f = expr.func
        fname = f.id if isinstance(f, ast.Name) else (f.attr if isinstance(f, ast.Attribute) else "")
        if fname == "child_env" and expr.args:
            return _dict_keys(expr.args[0], sc)
        return set()
    if isinstance(expr, ast.IfExp):
        return _env_reach(expr.body, sc, depth + 1) & _env_reach(expr.orelse, sc, depth + 1)
    if isinstance(expr, ast.Name):
        vals, _ = sc.values(expr.id, expr)
        if not vals:
            return set()
        reach = None
        for v in vals:
            r = _env_reach(v, sc, depth + 1)
            reach = r if reach is None else (reach & r)
        return reach or set()
    return set()


def _subprocess_names(tree):
    """Aliases of the subprocess module and launcher names imported from it."""
    mods, funcs = {"subprocess"}, set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            for a in n.names:
                if a.name == "subprocess":
                    mods.add(a.asname or a.name)
        elif isinstance(n, ast.ImportFrom) and n.module == "subprocess":
            for a in n.names:
                if a.name in _LAUNCHERS:
                    funcs.add(a.asname or a.name)
    return mods, funcs


def python_offends(text):
    """Per-launch verdict for a Python file: the knobs some launch that can
    name a Windows exe would not receive, or [] when every one reaches it.
    None when the text does not parse as Python."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return None
    knobs = _knobs_set(tree)
    if not knobs:
        return []
    sc = _Scopes(tree)
    mods, funcs = _subprocess_names(tree)
    missing = set()
    for n in ast.walk(tree):
        if not isinstance(n, ast.Call):
            continue
        f = n.func
        is_launch = (isinstance(f, ast.Attribute) and f.attr in _LAUNCHERS
                     and isinstance(f.value, ast.Name) and f.value.id in mods) \
            or (isinstance(f, ast.Name) and f.id in funcs)
        if not is_launch:
            continue
        argv = n.args[0] if n.args else next(
            (kw.value for kw in n.keywords if kw.arg == "args"), None)
        if argv is not None and _argv_safe(argv, sc):
            continue
        env = next((kw.value for kw in n.keywords if kw.arg == "env"), None)
        missing |= knobs - _env_reach(env, sc)
    return sorted(missing)


def _is_python(text, path):
    """Whether a file is checked as Python: by suffix, else by its shebang,
    else by whether it parses."""
    if path:
        if path.endswith(".py"):
            return True
        if path.endswith((".sh", ".bash")):
            return False
    first = text.split("\n", 1)[0]
    if first.startswith("#!"):
        return "python" in first
    try:
        ast.parse(text)
        return True
    except (SyntaxError, ValueError):
        return False


def offends(text, path=None):
    """One file's verdict: the knobs it would silently drop, or [] if it is
    clean or out of scope. Shared with the write-time hook so both entry
    points enforce one rule. A Python file is checked per launch; a shell
    file, or Python that does not parse, by the file-wide clearance."""
    if not (WINDOWS_LAUNCH.search(text) and KNOB.search(text)):
        return []
    if EXEMPT.search(text):
        return []
    if _is_python(text, path):
        verdict = python_offends(text)
        if verdict is not None:
            return verdict
    if SANCTIONED.search(text):
        return []
    return sorted(set(m.group(0).strip(" =:]\"'") for m in KNOB.finditer(text)))


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith((".sh", ".py", ".bash")):
                continue
            p = os.path.join(dirpath, fn)
            if os.path.abspath(p) == os.path.abspath(__file__):
                continue
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            knobs = offends(text, p)
            if knobs:
                bad.append((os.path.relpath(p, root), knobs))
    return bad


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..")
    root = os.path.abspath(root)
    bad = scan(root)
    if not bad:
        print("fence (wslenv_guard): clean")
        return 0
    sys.stderr.write(
        "FENCE (wslenv_guard): these scripts can launch a Windows binary and "
        "set OPENPETE_* knobs, but never mention WSLENV. Under WSL the knob "
        "does not arrive and the run silently uses the default:\n")
    for path, knobs in bad:
        sys.stderr.write(f"  {path}\n      {', '.join(knobs)}\n")
    sys.stderr.write(
        "\nFix: source scripts/lib/wslenv.sh and launch via "
        "`env \"${WSLENV_ARGS[@]}\" ...`, or build the child environment with "
        "a child_env()-style helper that appends to WSLENV. If the file "
        "never launches anything with these knobs (a comment, an echo, a "
        "CMake cache read), add the marker `wslenv-guard: no-launch`.\n")
    return 65


if __name__ == "__main__":
    sys.exit(main())
