#!/usr/bin/env python3
"""Tests for the rumble fence (rumble_guard.py) and the launch scope it
shares with input_lock_guard.py.

LaunchTest covers what counts as starting the binary: the Windows exe and
per-game names under any path or variable, never a directory or a path
query. ShellTest covers the per-launch rule: each launch site carries
--vibration 0 on its line or through a variable it expands, a comment never
clears one, and the env spelling still clears the whole file. PythonTest
covers the file-wide rule over code without comments and docstrings.

Run: python3 scripts/fences/tests/test_rumble_guard.py

The fixtures are source text handed to the fences; nothing here launches a
game (rumble-guard: no-launch, input-lock-guard: no-launch,
wslenv-guard: no-launch, sandbox-guard: no-launch).
"""
import os
import sys
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import input_lock_guard  # noqa: E402
import rumble_guard  # noqa: E402

SH = "check_fixture.sh"
PY = "bench-fixture.py"


def offends(text, name=SH):
    return rumble_guard.offends(name, text)


class LaunchTest(unittest.TestCase):
    def test_launch_forms_in_scope(self):
        for line in ("build/windows-mingw/openpete.exe --disc x.cue",
                     '"$W/openpete.exe" --disc x.cue',
                     "${W}/openpete.exe --disc x.cue",
                     "build/linux-release/openpete --headless",
                     "$BUILD/openpete --headless",
                     "build/linux-release/games/spyro-2/openpete-spyro2 --headless",
                     "games/spyro-3/openpete-spyro3.exe --disc x.cue",
                     "openpete-spyro1 --headless"):
            self.assertTrue(rumble_guard.launches(SH, line + "\n"), line)
            self.assertTrue(offends(line + "\n"), line)

    def test_names_without_launch_out_of_scope(self):
        for text in ('own() { echo "$1/openpete.exe"; }\n',
                     '[ -x build/linux-release/openpete ] || exit 64\n',
                     'cd "$ROOT/openpete" && ls\n',
                     'ls "$ROOT/openpete/scripts"\n',
                     "cat build/linux-release/openpete-recomp-linkage.txt\n",
                     "cp openpete.toml /tmp/x\n",
                     'BIN=build/linux-release/openpete\n[[ -x "$BIN" ]] || exit 64\n'
                     'echo "using $BIN"\n'):
            self.assertFalse(rumble_guard.launches(SH, text), text)
            self.assertFalse(offends(text), text)
            self.assertFalse(input_lock_guard.offends(SH, text), text)


class ShellTest(unittest.TestCase):
    def test_each_launch_judged(self):
        self.assertTrue(offends(
            "build/linux-release/openpete --headless --vibration 0 --disc a.cue\n"
            "build/windows-mingw/openpete.exe --disc a.cue\n"))
        self.assertFalse(offends(
            "build/linux-release/openpete --headless --vibration 0 --disc a.cue\n"
            "build/windows-mingw/openpete.exe --vibration 0 --disc a.cue\n"))

    def test_comment_does_not_clear(self):
        for text in ("# always pass --vibration 0\n"
                     "build/windows-mingw/openpete.exe --disc a.cue\n",
                     "build/windows-mingw/openpete.exe --disc a.cue  # --vibration 0\n"):
            self.assertTrue(offends(text), text)

    def test_cleared_launches(self):
        for text in (
                'BIN="$BUILD/openpete"\n"$BIN" --headless --vibration 0 --disc a.cue\n',
                'EXE="${1:-build/windows-mingw/openpete.exe}"\n'
                'env "${WSLENV_ARGS[@]}" "$EXE" --disc a.cue --vibration 0\n',
                '"$BIN" --headless \\\n    --vibration 0 --disc a.cue\n'
                'BIN=build/linux-release/openpete\n',
                'BIN=build/linux-release/openpete\nARGS=(--headless --vibration 0)\n'
                '"$BIN" "${ARGS[@]}" --disc a.cue\n',
                'BIN=build/linux-release/openpete\ncmd=("$BIN" --headless)\n'
                'cmd+=(--vibration 0)\n"${cmd[@]}" --disc a.cue\n',
                '    exe=build/linux-release/openpete\n'
                '    X=1 "$exe" --game spyro-1 --vibration "$VIB"\n',
                "export OPENPETE_VIBRATION=0\nbuild/windows-mingw/openpete.exe --disc a\n",
                'local -a pre=(env OPENPETE_VIBRATION=0)\n'
                '"${pre[@]}" "$PG_BIN" --headless\nPG_BIN=build/linux-release/openpete\n',
                "# rumble-guard: intentional\nbuild/windows-mingw/openpete.exe\n"):
            self.assertFalse(offends(text), text)

    def test_uncleared_variable_launches(self):
        for text in (
                'BIN="$BUILD/openpete"\n"$BIN" --headless --disc a.cue\n',
                'EXE=build/windows-mingw/openpete.exe\ntimeout 60 "$EXE" --disc a\n',
                'BIN=build/linux-release/openpete\ncmd=("$BIN" --headless)\n'
                '"${cmd[@]}" --disc a.cue\n',
                'EXE="${1:-build/windows-mingw/openpete.exe}"\n'
                'run() { "$@"; }\nrun "$EXE" out.log\n'):
            self.assertTrue(offends(text), text)
        # a binary handed to a function: the flag anywhere in the code clears
        self.assertFalse(offends(
            'EXE="${1:-build/windows-mingw/openpete.exe}"\n'
            'COMMON=(--background --vibration 0)\n'
            'run() { local exe=$1; "$exe" "${COMMON[@]}"; }\nrun "$EXE" out.log\n'))

    def test_unattended_name_scope_kept(self):
        text = "build/windows-mingw/openpete.exe --disc a.cue\n"
        self.assertFalse(rumble_guard.offends("play.sh", text))
        self.assertTrue(rumble_guard.offends("play.sh", text, require_unattended=False))


class PythonTest(unittest.TestCase):
    def test_python_rules(self):
        flagged = ('EXE = "build/windows-mingw/openpete.exe"\n'
                   'subprocess.run([EXE, "--disc", "a.cue"])\n')
        self.assertTrue(offends(flagged, PY))
        commented = ('EXE = "build/windows-mingw/openpete.exe"\n'
                     '# pass "--vibration", "0" one day\n'
                     'subprocess.run([EXE, "--disc", "a.cue"])\n')
        self.assertTrue(offends(commented, PY))
        cleared = ('EXE = "build/windows-mingw/openpete.exe"\n'
                   'subprocess.run([EXE, "--vibration", "0", "--disc", "a.cue"])\n')
        self.assertFalse(offends(cleared, PY))
        env = ('import os\nos.environ.setdefault("OPENPETE_VIBRATION", "0")\n'
               'EXE = "build/linux-release/openpete"\n')
        self.assertFalse(offends(env, PY))
        docstring = ('def exe_name(game):\n'
                     '    """The per-game name: `spyro-2` -> `openpete-spyro2`."""\n'
                     '    return "x"\n')
        self.assertFalse(rumble_guard.launches(PY, docstring))
        self.assertFalse(offends(docstring, PY))


if __name__ == "__main__":
    unittest.main()
