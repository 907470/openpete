#!/usr/bin/env python3
"""Tests for the sandbox fence (sandbox_guard.py).

RefuseTest: a scripted launch that neither sources the sandbox library nor
sets OPENPETE_SANDBOX is refused, in shell and in Python, whatever the file
is called, and a comment naming the library does not clear it. PassTest: the
library sourced directly or through a provider found by the closure, the
Python helpers, the variable set on the launch, and the two markers (an
`intentional` marker only with a reason). ProviderTest: the closure finds a
library that sources another provider. TreeTest: the checkout is clean.

Run: python3 scripts/fences/tests/test_sandbox_guard.py

The fixtures are source text handed to the fence; nothing here launches a
game (rumble-guard: no-launch, input-lock-guard: no-launch,
wslenv-guard: no-launch, sandbox-guard: no-launch).
"""
import os
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import sandbox_guard  # noqa: E402

PROVS = {"sandbox.sh", "scratch_cards.sh", "game.sh"}
LAUNCH_SH = 'build/linux-release/openpete --headless --disc "$DISC" --vibration 0\n'
LAUNCH_PY = ('import subprocess\n'
             'subprocess.run(["build/linux-release/openpete", "--headless"])\n')


def offends(text, name="check_fixture.sh"):
    return sandbox_guard.offends(name, text, PROVS)


class RefuseTest(unittest.TestCase):
    def test_bare_shell_launch(self):
        self.assertTrue(offends(LAUNCH_SH))

    def test_any_name(self):
        self.assertTrue(offends(LAUNCH_SH, "run-something.sh"))

    def test_bare_python_launch(self):
        self.assertTrue(offends(LAUNCH_PY, "bench-x.py"))

    def test_comment_does_not_clear(self):
        self.assertTrue(offends("# source scripts/lib/sandbox.sh\n" + LAUNCH_SH))

    def test_marker_without_reason(self):
        self.assertTrue(offends("# sandbox-guard: intentional\n" + LAUNCH_SH))

    def test_other_library_does_not_clear(self):
        self.assertTrue(offends('source scripts/lib/wslenv.sh\n' + LAUNCH_SH))


class PassTest(unittest.TestCase):
    def test_sources_library(self):
        self.assertFalse(offends('source "$ROOT/scripts/lib/sandbox.sh"\n' + LAUNCH_SH))
        self.assertFalse(offends('. "$(dirname "$0")/lib/sandbox.sh"\n' + LAUNCH_SH))

    def test_sources_provider(self):
        self.assertFalse(offends('source scripts/lib/game.sh\n' + LAUNCH_SH))
        self.assertFalse(offends('source scripts/lib/scratch_cards.sh\n' + LAUNCH_SH))

    def test_env_on_launch(self):
        self.assertFalse(offends('OPENPETE_SANDBOX="$W/sb" ' + LAUNCH_SH))

    def test_python_helpers(self):
        for call in ("hostenv.sandbox()\n", "hostenv.scratch_cards()\n",
                     "root = hostenv.sandbox_run_root()\n",
                     'env["OPENPETE_SANDBOX"] = str(work)\n'):
            self.assertFalse(offends(call + LAUNCH_PY, "bench-x.py"), call)

    def test_markers(self):
        self.assertFalse(offends("# sandbox-guard: intentional first boot needs a clean HOME\n"
                                 + LAUNCH_SH))
        self.assertFalse(offends("# sandbox-guard: intentional\n# the bench reads the player toml\n"
                                 + LAUNCH_SH))
        self.assertFalse(offends("# sandbox-guard: no-launch\n" + LAUNCH_SH))

    def test_not_a_launcher(self):
        self.assertFalse(offends('[ -x build/linux-release/openpete ] || exit 64\n'))


class ProviderTest(unittest.TestCase):
    def test_closure(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "lib"))
            with open(os.path.join(d, "lib", "sandbox.sh"), "w") as f:
                f.write("op_sandbox() { :; }\n")
            with open(os.path.join(d, "lib", "middle.sh"), "w") as f:
                f.write('source "$(dirname "${BASH_SOURCE[0]}")/sandbox.sh"\n')
            with open(os.path.join(d, "outer.sh"), "w") as f:
                f.write("source lib/middle.sh\n")
            with open(os.path.join(d, "unrelated.sh"), "w") as f:
                f.write("# source lib/sandbox.sh\necho hi\n")
            found = sandbox_guard.providers((d,))
        self.assertEqual(found, {"sandbox.sh", "middle.sh", "outer.sh"})


class TreeTest(unittest.TestCase):
    def test_checkout_clean(self):
        provs = sandbox_guard.default_providers()
        self.assertEqual(sandbox_guard.scan(sandbox_guard.OPENPETE, provs), [])


if __name__ == "__main__":
    unittest.main()
