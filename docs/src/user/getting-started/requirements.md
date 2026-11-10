# Requirements

## The disc

Your own copy of Spyro the Dragon, NTSC-U release (serial SCUS-94228), dumped
to a `.bin`/`.cue` pair. The Original, Greatest Hits and Collectors' Edition
pressings share one image. [Dumping your disc](dumping-your-disc.md) lists
the exact image OpenPete expects and how to check a dump against it.

OpenPete downloads nothing and ships no game content.

## The computer

| | |
| --- | --- |
| CPU | 64-bit x86-64 |
| GPU | A driver supporting Vulkan 1.2 |
| Disk | About 700 MB: the library keeps its own copy of the disc image (about 640 MB, one copy per distinct disc) plus about 26 MB per compiled build |
| Linux | glibc 2.31 or newer (Debian 11, Ubuntu 20.04, or later) |

The retained terrain cache, which draws static level geometry from GPU
memory, needs a hardware GPU. On a software renderer, OpenPete draws terrain
on the CPU instead; see [Performance](../graphics/performance.md).

## A C compiler, on first run only

The first run compiles the game's code for your machine.

=== "Windows"

    The zip carries its own compiler. Nothing needs installing.

=== "Linux AppImage"

    The AppImage carries its own compiler. Nothing needs installing.

=== "Linux tar.xz"

    The archive uses your system `cc` unless it was packaged with a bundled
    compiler (a `recomp/tools/clang/` folder in the archive). Install one with
    your package manager, for example `sudo apt install gcc`.
