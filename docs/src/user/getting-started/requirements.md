# Requirements

## The disc

Your own copy of the game, NTSC-U (North American) release, dumped to a
`.bin`/`.cue` pair:

| Game | Serial |
| --- | --- |
| Spyro the Dragon | SCUS-94228 |
| Spyro 2: Ripto's Rage! | SCUS-94425 |
| Spyro: Year of the Dragon | SCUS-94467 |

OpenPete reads the serial from the disc image and refuses any other: PAL
and NTSC-J releases have other serials and are not accepted.
[Dumping your disc](dumping-your-disc.md) lists the exact images OpenPete
expects and how to check a dump against them.

OpenPete downloads nothing and ships no game content.

## The computer

| | |
| --- | --- |
| CPU | 64-bit x86-64 |
| GPU | A driver supporting Vulkan 1.2 |
| Disk | About 700 MB per game: the library keeps its own copy of each game's disc image (565 to 662 MB; one disc per game) plus about 26 MB per compiled build |
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
