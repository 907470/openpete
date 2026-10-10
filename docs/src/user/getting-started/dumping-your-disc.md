# Dumping your disc

OpenPete reads a raw image of your own Spyro the Dragon disc as a `.bin`/`.cue`
pair. It does not read the disc drive directly.

## The image OpenPete expects

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
not ingested; convert them back to `.bin`/`.cue` first.

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
(NTSC-U)" at ingest. An image with another hash is labelled with its file
name instead.

!!! warning
    Other regions (PAL, NTSC-J), demo discs and modified images are not the
    image OpenPete's port is built and tested against.
