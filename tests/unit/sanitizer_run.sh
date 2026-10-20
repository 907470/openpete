#!/bin/sh
# sanitizer_run.sh <binary> [args...] - run a sanitizer-instrumented test.
#
# The binary's `selftest` mode exits 0 at once; it is run first to learn
# whether the sanitizer runtime starts on this kernel. ThreadSanitizer
# aborts at startup when the kernel's mmap randomization exceeds its shadow
# layout; the probe then retries under `setarch <arch> -R`. When neither
# starts, the test exits 64, which ctest reports as skipped.
bin=$1
shift
if "$bin" selftest >/dev/null 2>&1; then
    exec "$bin" "$@"
fi
arch=$(uname -m)
if command -v setarch >/dev/null 2>&1 &&
   setarch "$arch" -R "$bin" selftest >/dev/null 2>&1; then
    exec setarch "$arch" -R "$bin" "$@"
fi
echo "sanitizer_run: the sanitizer runtime of $bin does not start here; skipped"
exit 64
