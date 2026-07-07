# 0005 — binary-mode output streams

**Files:** `recompiler/src/main_psx.cpp`

All generated-output `std::ofstream`s (full C, dispatch C, discovered
log, external-JALs list) open with `std::ios::binary`. Without it the
MinGW build text-translates `\n` → `\r\n`, so PSXRecomp.exe emitted
different bytes than the Linux build for identical inputs — breaking
cross-platform output identity (and every hash/cache keyed on the
generated C). Verified: the Windows exe's output is now byte-identical
to the Linux build's across the Spyro corpus; POSIX behavior unchanged
(binary mode is a no-op there).
