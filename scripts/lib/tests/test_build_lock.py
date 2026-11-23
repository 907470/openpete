#!/usr/bin/env python3
"""Tests for scripts/lib/build_lock.py and scripts/lib/build_lock.sh: a build
and a gate that share a build directory never overlap, in either order.

Each test runs against a scratch build directory (a CMakeCache.txt marks
it) and a stand-in build driver: a script named `ninja` that runs
`build_lock.py build-hold` the way the CMake target openpete_build_lock does,
then writes a marker and waits. No build or game runs: the binary is an
empty file the lock is keyed on, so
sandbox-guard: no-launch, rumble-guard: no-launch, input-lock-guard: no-launch,
xvfb-guard: no-launch
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.dirname(HERE)
BL = os.path.join(LIB, "build_lock.py")
BL_SH = os.path.join(LIB, "build_lock.sh")
sys.path.insert(0, LIB)
import build_lock                                           # noqa: E402


def _wait_for(path, timeout=20.0):
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > end:
            raise AssertionError("timed out waiting for %s" % path)
        time.sleep(0.02)


class BuildLockTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="build-lock-test.")
        self.bdir = os.path.join(self.dir, "build")
        os.makedirs(os.path.join(self.bdir, "games"))
        open(os.path.join(self.bdir, "CMakeCache.txt"), "w").close()
        self.bin = os.path.join(self.bdir, "openpete")
        open(self.bin, "w").close()
        self.ninja = os.path.join(self.dir, "bin", "ninja")
        os.makedirs(os.path.dirname(self.ninja))
        with open(self.ninja, "w") as f:
            f.write('#!/bin/bash\n'
                    '# stand-in build driver: the lock step, then the build\n'
                    'python3 "$BL" build-hold --dir "$BDIR" || exit $?\n'
                    'touch "$D/building"\n'
                    'until [ -e "$D/build.done" ] || [ -e "$D/release" ]; do sleep 0.05; done\n'
                    '[ -n "$GATE_IN_BUILD" ] && { python3 "$BL" read --for-pid $$ --path "$BDIR/openpete" '
                    '--timeout 2; echo "gate-in-build=$?" > "$D/gate-in-build"; }\n'
                    'exit 0\n')
        os.chmod(self.ninja, 0o755)
        self.procs = []
        self.env = dict(os.environ, BL=BL, BL_SH=BL_SH, BDIR=self.bdir, D=self.dir,
                        NINJA=self.ninja)
        self.env.pop(build_lock.BYPASS_ENV, None)

    def tearDown(self):
        open(self.f("release"), "w").close()
        for p in self.procs:
            try:
                p.wait(timeout=20)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        build_lock.release(self.bin)
        shutil.rmtree(self.dir, ignore_errors=True)

    def f(self, name):
        return os.path.join(self.dir, name)

    def start(self, script, **extra):
        p = subprocess.Popen(["bash", "-c", script], env=dict(self.env, **extra), text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def gate_script(self):
        return ('source "$BL_SH"; op_build_read_lock "$BDIR/openpete" gate || exit 9; '
                'touch "$D/gated"; until [ -e "$D/gate.done" ] || [ -e "$D/release" ]; '
                'do sleep 0.05; done')

    def test_gate_waits_for_a_running_build(self):
        build = self.start('"$NINJA"')
        _wait_for(self.f("building"))
        r = subprocess.run([sys.executable, BL, "read", "--for-pid", str(os.getpid()),
                            "--path", self.bin, "--label", "late-gate", "--timeout", "1"],
                           env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 75, "a gate started while a build ran: " + r.stderr)
        self.assertIn("ninja", r.stderr, "the wait does not name the build")
        open(self.f("build.done"), "w").close()
        self.assertEqual(build.wait(timeout=20), 0)
        self.assertTrue(build_lock.read(self.bin, "gate", timeout=10),
                        "the build's lock outlived its driver")

    def test_build_waits_for_a_running_gate(self):
        gate = self.start(self.gate_script())
        _wait_for(self.f("gated"))
        build = self.start('"$NINJA"')
        time.sleep(1.5)
        self.assertFalse(os.path.exists(self.f("building")),
                         "a build ran while a gate read its directory")
        open(self.f("gate.done"), "w").close()
        self.assertEqual(gate.wait(timeout=20), 0)
        _wait_for(self.f("building"))
        open(self.f("build.done"), "w").close()
        self.assertEqual(build.wait(timeout=20), 0)

    def test_bypass_builds_beside_a_gate(self):
        # Control: without the lock the build runs beside the gate, so the
        # previous test's wait is the lock's doing.
        self.start(self.gate_script())
        _wait_for(self.f("gated"))
        self.start('"$NINJA"', **{build_lock.BYPASS_ENV: "1"})
        _wait_for(self.f("building"), timeout=10)

    def test_gates_share(self):
        self.start(self.gate_script())
        _wait_for(self.f("gated"))
        self.assertTrue(build_lock.read(self.bin, "second gate", timeout=2))

    def test_build_under_its_own_gate_refuses(self):
        p = self.start('source "$BL_SH"; op_build_read_lock "$BDIR/openpete" gate || exit 9; '
                       '"$NINJA"; echo rc=$?')
        out, err = p.communicate(timeout=30)
        self.assertIn("rc=3", out, err)
        self.assertIn("REFUSED", err)

    def test_gate_run_by_the_build_proceeds(self):
        build = self.start('"$NINJA"', GATE_IN_BUILD="1")
        _wait_for(self.f("building"))
        open(self.f("build.done"), "w").close()
        self.assertEqual(build.wait(timeout=30), 0)
        with open(self.f("gate-in-build")) as fh:
            self.assertEqual(fh.read().strip(), "gate-in-build=0",
                             "a gate the build runs waited on its own build")

    def test_driver_by_another_name_holds_for_its_run(self):
        # An IDE runner: the step runs under `sh -c` under a driver that is
        # not named ninja; the lock lasts for the driver, not the shell.
        runner = os.path.join(self.dir, "bin", "ide-runner")
        with open(runner, "w") as f:
            f.write('#!/bin/bash\n'
                    'sh -c \'python3 "$BL" build-hold --dir "$BDIR"\' || exit $?\n'
                    'touch "$D/building"\n'
                    'until [ -e "$D/build.done" ] || [ -e "$D/release" ]; do sleep 0.05; done\n')
        os.chmod(runner, 0o755)
        build = self.start('"%s"' % runner)
        _wait_for(self.f("building"))
        time.sleep(0.5)
        self.assertFalse(build_lock.read(self.bin, "gate", timeout=1),
                         "the lock ended with the step's shell, not the driver")
        open(self.f("build.done"), "w").close()
        self.assertEqual(build.wait(timeout=20), 0)
        self.assertTrue(build_lock.read(self.bin, "gate", timeout=10))

    def test_path_outside_a_build_takes_no_lock(self):
        self.assertTrue(build_lock.read(os.path.join(self.dir, "bin", "copied.exe"), "gate",
                                        timeout=0))
        self.assertEqual(build_lock._HOLDS, {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
