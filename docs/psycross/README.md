# PsyCross changes

`third_party/PsyCross` is a submodule tracking the `openpete` branch of
the fork at https://github.com/tyscorp/PsyCross (upstream:
https://github.com/OpenDriver2/PsyCross). Every change openpete needed is
a commit on that branch; nothing is patched at build time.

Each file here records one change: symptom, root cause, fix, and
verification. The fork commit that carries it names the file by its old
path, `patches/PsyCross/PATCHES/<name>`.

| file | fork commit |
|---|---|
| 0001-linux-build-fixes.md | 0f7728cd |
| 0002-cd-stub-stateless-commands.md | 8f41894e |
| 0003-disable-pgxp-half-float-verts.md | 42decab0 |
| 0004-renderer-bad-rect-clamp.md | 80cd00f6 |
| 0005-mingw-msvc-only-headers.md | f41d0900 |
| 0006-drawenv-struct-psyq-alignment.md | 1063e779 |
| 0007-post-render-overlay-hook.md | 0b9c3fa7 |
| 0008-pad-input-override-hook.md | e6ffadee |
| 0009-keyboard-left-analog-stick.md | 4e341b47 |
| 0010-pad-get-last-p1-bits.md | 30ee3259 |
| 0011-optional-log-file.md | 77a07d75 |
| 0012-glad-load-via-sdl-for-egl.md | a44aec9b |
| 0013-gl-shutdown-before-window-destroy.md | a44aec9b |
