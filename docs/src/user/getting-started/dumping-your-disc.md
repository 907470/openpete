# Dumping your disc

OpenPete reads a raw image of your own disc of Spyro the Dragon, Spyro 2:
Ripto's Rage! or Spyro: Year of the Dragon as a `.bin`/`.cue` pair. It does
not read the disc drive directly. This page gives the Spyro 1 image; the
other two games are dumped the same way.

## The image OpenPete expects (Spyro 1)

| | |
| --- | --- |
| Release | Spyro the Dragon, NTSC-U, serial SCUS-94228 (Original, Greatest Hits and Collectors' Edition pressings share one image) |
| Layout | One data track, MODE2/2352 (raw 2352-byte sectors), 281270 sectors |
| `.bin` SHA-256 | `fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37` |
| `.bin` SHA-1 | `cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f` |
| `.bin` MD5 | `387185f71cd0043fd75a19521dc75104` |

These are the Redump-verified image's hashes; Redump's database entry for
the disc lists the same SHA-1 and MD5.

Spyro 2 (SCUS-94425) and Spyro 3 (SCUS-94467) are dumped the same way; their
images and hashes are on [Spyro 2](../games/spyro-2.md#disc) and
[Spyro 3](../games/spyro-3.md#disc).

## Making the dump

Use a disc-imaging program that writes raw 2352-byte sectors to a `.bin` with
a `.cue` sheet. A dump in 2048-byte sectors (a plain `.iso`) drops the
sector headers and does not match. Compressed formats (`.chd`, `.pbp`) are
not ingested; convert them back to `.bin`/`.cue` first. Given a `.cue`, OpenPete reads the `.bin` of the same name beside it,
or else a `.bin` in the same folder, so keep each disc's pair in a folder of
its own.

## Checking the dump

Compare the `.bin` file's SHA-256 with the table above.

=== "Windows"

    ```
    certutil -hashfile "Spyro the Dragon.bin" SHA256
    ```

=== "Linux AppImage"

    ```
    sha256sum "Spyro the Dragon.bin"
    ```

=== "Linux tar.xz"

    ```
    sha256sum "Spyro the Dragon.bin"
    ```

When the hash matches, the library labels the entry "Spyro the Dragon
(NTSC-U)" at ingest. An image with another hash but the same serial is
added and labelled with its file name instead; if the game cannot be built
from it, it is removed again with an
[OP-E-MODULE](../help/troubleshooting.md#op-e-module) message.

!!! warning
    OpenPete identifies a disc by the serial in its `SYSTEM.CNF`. Other
    regions (PAL, NTSC-J) and demo discs have other serials and are
    refused before anything is copied, with a message naming the release
    ([OP-E-DISC-BUILD](../help/troubleshooting.md#op-e-disc-build)).
    Modified images with the right serial are added with a warning
    ([Unsupported discs](#unsupported-discs)).

## Unsupported discs

Each game's code is built from the game program on one disc: the NTSC-U
release, version 1.0. At every start OpenPete reads the disc's serial and
the game program its `SYSTEM.CNF` names, and compares the program's SHA-256
with the one it is built from. The `.bin` hash does not decide: two dumps of
the same disc can differ outside the game program.

| Disc | What OpenPete does |
| --- | --- |
| Spyro the Dragon SCUS-94228, Spyro 2: Ripto's Rage! SCUS-94425, Spyro: Year of the Dragon SCUS-94467 version 1.0 | Starts. |
| A PAL release (serials SCES-01438, SCES-02104, SCES-02835) or an NTSC-J release (SCPS-10085, SCPS-10083, SCPS-45395, SCPS-10128) | Is not added to the library and does not start. The message names the release and the NTSC-U release to use instead. |
| An image whose `SYSTEM.CNF` names another program (such as `PSX.EXE`), or has no usable `SYSTEM.CNF` | Starts when the image holds the game's own program file (`SCUS_942.28`, `SCUS_944.25` or `SCUS_944.67`); otherwise does not start. An image whose program is the supported one under another name starts without a warning. |
| Spyro: Year of the Dragon revision 1.1 (same serial SCUS-94467; its `.bin` SHA-1 is `81eae747fbd9b2ff75ae5b3e24287bb25fcb516a`, where version 1.0's is the one on [Spyro 3](../games/spyro-3.md#disc)), or any image whose game program differs from version 1.0 | Is added with a warning, starts, and is expected to crash. |

When a disc that is not the supported build is added to the library, the
game page shows a warning naming the disc and the supported release, with
[OP-E-DISC-BUILD](../help/troubleshooting.md#op-e-disc-build); with
`--ingest` on the command line the same text is printed. The disc stays in
the library and starts from its box, and the version list names it with
**(unsupported disc)**. A disc added before this check existed gets the
warning once, the next time the library opens.

When the game starts from such a disc, the log `openpete.log` gets a
warning naming the disc's serial, its game program, that program's SHA-256
and OP-E-DISC-BUILD. A disc that cannot run at all is refused at the start
with the same id.
