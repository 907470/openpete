#!/usr/bin/env python3
"""Tests for the WSLENV fence (wslenv_guard.py).

PythonTest drives the per-launch rule on Python sources: a knob set with
os.environ.setdefault beside a launch that forwards a different knob through
child_env is refused, as is a launch without env=; a knob reaching the
launch through a child_env argument (a dict literal, a name resolved through
its assignments and subscript writes, a module-level dict) passes; a launch
whose argv starts with a string literal or sys.executable is out of scope.
ShellTest keeps the file-wide clearance for shell. RepoTest runs the scan
over this checkout and requires it clean.

Run: python3 scripts/fences/tests/test_wslenv_guard.py

The fixtures are source text handed to the fence; nothing here launches a
game (input-lock-guard: no-launch, sandbox-guard: no-launch).
"""
import os
import sys
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import wslenv_guard  # noqa: E402

# The shape bench-corpus.py had: one knob set process-wide, another
# forwarded through child_env at the one launch.
SETDEFAULT_BESIDE_CHILD_ENV = '''
import os, subprocess
from hostenv import child_env
EXE = "build/windows-mingw/openpete.exe"
os.environ.setdefault("OPENPETE_VIBRATION", "0")
def run(exe, timers):
    cmd = [exe, "--disc", "x.cue"]
    env = child_env({"OPENPETE_GPU_TIMERS": "1"} if timers else {})
    subprocess.run(cmd, env=env)
'''

# The shape bench-topdown.py had: setdefault and a launch with no env=.
SETDEFAULT_NO_ENV = '''
import os, subprocess
os.environ.setdefault("OPENPETE_VIBRATION", "0")
def run(exe):
    if exe.endswith(".exe"):
        pass
    subprocess.run([exe, "--disc", "x.cue"])
'''

KNOBS_THROUGH_NAME = '''
import subprocess
from hostenv import child_env
RUN_KNOBS = {"OPENPETE_VIBRATION": "0"}
def run(exe, timers):
    """Launches openpete-spyro1.exe or the Linux exe."""
    cmd = [exe, "--disc", "x.cue"] + ["--headless"]
    knobs = dict(RUN_KNOBS)
    if timers:
        knobs["OPENPETE_GPU_TIMERS"] = "1"
    subprocess.run(cmd, env=child_env(knobs))
'''

PACELEG_SHAPE = '''
import subprocess
from hostenv import child_env
EXE = "openpete-spyro1.exe"
def run_leg(exe, headless, knobs=None):
    cmd = [str(exe), "--frame-limit", "100"]
    env_knobs = dict(knobs or {})
    if headless:
        env_knobs["OPENPETE_FORCE_PACING"] = "1"
    subprocess.run(cmd, env=child_env(env_knobs))
'''

LITERAL_ARGV = '''
import os, subprocess, sys
os.environ["OPENPETE_X"] = "1"
EXE = "a.exe"
subprocess.run(["wslpath", "-w", EXE])
subprocess.run([sys.executable, "tool.py"])
argv = ["git", "status"]
subprocess.run(argv)
'''

SECOND_LAUNCH_UNFORWARDED = '''
import subprocess
from hostenv import child_env
KNOBS = {"OPENPETE_VIBRATION": "0"}
def run(exe):
    """openpete.exe"""
    subprocess.run([exe], env=child_env(KNOBS))
    subprocess.Popen([exe, "--again"])
'''


class PythonTest(unittest.TestCase):
    def verdict(self, text):
        return wslenv_guard.offends(text, "x.py")

    def test_setdefault_beside_child_env_refused(self):
        self.assertEqual(self.verdict(SETDEFAULT_BESIDE_CHILD_ENV),
                         ["OPENPETE_VIBRATION"])

    def test_setdefault_without_env_refused(self):
        self.assertEqual(self.verdict(SETDEFAULT_NO_ENV), ["OPENPETE_VIBRATION"])

    def test_call_argument_spelling_is_a_knob(self):
        self.assertTrue(wslenv_guard.KNOB.search(
            'os.environ.setdefault("OPENPETE_VIBRATION", "0")'))

    def test_knobs_through_names_pass(self):
        self.assertEqual(self.verdict(KNOBS_THROUGH_NAME), [])

    def test_paceleg_shape_passes(self):
        self.assertEqual(self.verdict(PACELEG_SHAPE), [])

    def test_literal_argv_out_of_scope(self):
        self.assertEqual(self.verdict(LITERAL_ARGV), [])

    def test_each_launch_checked(self):
        self.assertEqual(self.verdict(SECOND_LAUNCH_UNFORWARDED),
                         ["OPENPETE_VIBRATION"])

    def test_python_detected_without_path(self):
        self.assertEqual(wslenv_guard.offends(SETDEFAULT_BESIDE_CHILD_ENV),
                         ["OPENPETE_VIBRATION"])


class ShellTest(unittest.TestCase):
    def test_forwarded_shell_clear(self):
        text = ('#!/usr/bin/env bash\nEXE=x.exe\nenvs=(OPENPETE_A=1 "$@")\n'
                'wslenv_forward "${envs[@]}"\n"$EXE"\n')
        self.assertEqual(wslenv_guard.offends(text, "x.sh"), [])

    def test_unforwarded_shell_refused(self):
        text = '#!/usr/bin/env bash\nEXE=x.exe\nOPENPETE_A=1 "$EXE"\n'
        self.assertEqual(wslenv_guard.offends(text, "x.sh"), ["OPENPETE_A"])


class RepoTest(unittest.TestCase):
    def test_checkout_clean(self):
        self.assertEqual(wslenv_guard.scan(ROOT), [])


if __name__ == "__main__":
    unittest.main()
