# Thread-local storage on the Windows toolchain

The `windows-mingw` presets compile with `x86_64-w64-mingw32-gcc-posix`
and `x86_64-w64-mingw32-g++-posix` (`cmake/toolchains/mingw-w64.cmake`);
the build machine's cross compiler is GCC 10 (`--version` prints
`10-posix`). For this target GCC implements `_Thread_local` and `thread_local`
by emulated TLS:

- each thread-local variable `x` becomes a control object `__emutls_v.x`
  (the shipped `openpete.exe` holds `__emutls_v.*` symbols);
- every access compiles to a call `__emutls_get_address(&__emutls_v.x)`,
  which returns the calling thread's copy, allocating it on the thread's
  first access.

On `linux-release` the same source uses native ELF TLS: an access is a load
relative to the thread pointer, with no call. A thread-local read inside a
per-triangle or per-vertex loop therefore costs a function call per
iteration on the Windows exe only, and a Linux profile does not show it.

## The moby emit path

The moby walk (`games/common/renderer/moby.c`) keeps its per-thread walk
state in `_Thread_local` variables, so that the extraction worker pool can
walk records in parallel, and writes triangles into an emit target,
`mn_out` (the global target, or a worker's slab), reached through the
thread-local pointer `t_out`. `emit_wtri_store` runs once per
stored triangle. When it read two further thread-local words per triangle,
the record's class bits and the face's class bits, the calls into the TLS
emulation cost about 14 µs per present on the Windows exe. The two words
are now fields of the emit target (`mn_out.cb_rec`, `mn_out.cb_face`),
which the store already holds; `walk_records` reads `t_out` once into a
local (`mn_out* const O = t_out;`) for the whole walk; and the near clip
(`emit_wtri_near`) touches the face word only for a triangle it cut.

## Counting the calls

The linker's `--wrap=SYMBOL` option redirects every undefined reference to
`SYMBOL` to `__wrap_SYMBOL`, and every reference to `__real_SYMBOL` to the
original definition. To count emulated-TLS accesses in a Windows build:

1. Add a C file to the exe that defines
   `void* __wrap___emutls_get_address(void* control)`: it increments a
   counter and returns `__real___emutls_get_address(control)`. The counter
   is a plain global updated atomically, never a thread-local, which would
   recurse into the wrapper. To attribute calls to variables, count per
   `control` address in a small table.
2. Link with `-Wl,--wrap=__emutls_get_address` (for example through
   `CMAKE_EXE_LINKER_FLAGS` on a scratch build directory).
3. Print the counts at exit, divided by the presents of the run, and map
   each control address to its variable with
   `x86_64-w64-mingw32-nm openpete.exe | grep __emutls_v.`.

The redirect applies to references from the program's own objects, which
is where every `_Thread_local` access of the source compiles; calls made
inside libgcc's emutls object are not counted. Count on a fixed input (a
bk2 replay with `--frame-limit`) so two builds compare per present. The
wrapper is a measurement build only; it is not committed.

## Rule

A loop that runs per triangle, vertex, face or record takes its per-thread
state as a parameter, a field of a structure it already holds, or a local
copy read once before the loop; its body reads no `_Thread_local` variable
on the common path. A thread-local read inside such a loop is acceptable
only on a rare branch (a clipped triangle, an error), as in
`emit_wtri_near`.
