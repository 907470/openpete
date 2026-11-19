#!/bin/sh
# sanitizer_run.sh <binary> [args...] - run a sanitizer-instrumented test.
#
# ThreadSanitizer aborts at startup ("unexpected memory mapping", exit 66)
# when the kernel places a mapping outside its shadow layout; with mmap
# randomization on, that depends on each exec's random layout, so a probe
# exec that starts says nothing about the next one. The test therefore runs
# under `setarch <arch> -R` (ADDR_NO_RANDOMIZE) wherever setarch can set
# that personality: the layout is then the same in every exec, and the
# binary's `selftest` mode (exit 0 at once) probes that exact layout first.
# Only where setarch is missing or refused does the test run with the
# kernel's randomization, after a probe of the same form. When the probe
# fails, the test exits 64, which ctest reports as skipped.
bin=$1
shift
arch=$(uname -m)
if command -v setarch >/dev/null 2>&1 && setarch "$arch" -R true 2>/dev/null; then
    if setarch "$arch" -R "$bin" selftest >/dev/null 2>&1; then
        exec setarch "$arch" -R "$bin" "$@"
    fi
elif "$bin" selftest >/dev/null 2>&1; then
    exec "$bin" "$@"
fi
echo "sanitizer_run: the sanitizer runtime of $bin does not start here; skipped"
exit 64
