# Installing

Each release comes in three packages, each with a `.sha256` checksum file
beside it:

| Package | File |
| --- | --- |
| Windows | `openpete-spyro1-<version>-windows-x86_64.zip` |
| Linux AppImage | `OpenPete-<version>-x86_64.AppImage` |
| Linux tar.xz | `openpete-spyro1-<version>-linux-x86_64.tar.xz` |

The AppImage is a single read-only file that carries its own compiler. The
tar.xz is an extracted folder that can run portable; see
[File locations](../help/file-locations.md).

## Install and start

=== "Windows"

    1. Extract the zip to a folder with a short path, for example
       `C:\Games\OpenPete`. Very deep folders can exceed the Windows
       path-length limit during the first run's compile.
    2. Run `openpete-spyro1.exe`.

    !!! note "SmartScreen"
        The download is unsigned, so Windows shows "Windows protected your
        PC". Click **More info**, then **Run anyway**. To check the download
        first, compare `certutil -hashfile <zip> SHA256` against the
        published `.sha256` file.

=== "Linux AppImage"

    ```
    chmod +x OpenPete-*.AppImage
    ./OpenPete-*.AppImage
    ```

    !!! note "FUSE error"
        If the AppImage exits with a FUSE error, your distribution does not
        install libfuse by default. Install it, or run the image without it:
        `./OpenPete-*.AppImage --appimage-extract-and-run`.

    To add OpenPete to your application menu, either install `appimaged` or
    Gear Lever, or extract the image once
    (`./OpenPete-*.AppImage --appimage-extract`), copy
    `com.openpete.OpenPete.desktop` into `~/.local/share/applications` and
    `com.openpete.OpenPete.png` into `~/.local/share/icons`, and point the
    `Exec=` line at the AppImage.

=== "Linux tar.xz"

    ```
    tar xJf openpete-spyro1-*-linux-x86_64.tar.xz
    cd openpete-spyro1-*-linux-x86_64
    ./openpete-spyro1
    ```

    Without a bundled compiler the first run needs a system `cc`; see
    [Requirements](requirements.md#a-c-compiler-on-first-run-only).

The first start shows a window asking for the disc image. Continue with
[First run](first-run.md).

## Opening a disc image directly

A disc image passed as the only argument is ingested, so opening a `.cue` or
`.bin` from a file manager ("Open with") or from a shell works:

=== "Windows"

    ```
    openpete-spyro1.exe mydisc.cue
    ```

=== "Linux AppImage"

    ```
    ./OpenPete-*.AppImage mydisc.cue
    ```

=== "Linux tar.xz"

    ```
    ./openpete-spyro1 mydisc.cue
    ```

The flag spelling is `--ingest PATH` <!-- id: flag:--ingest -->.

`--disc PATH` <!-- id: flag:--disc --> boots the image at PATH where it is,
without copying it into the [library](../help/glossary.md#library). Without
it, the program boots the library's most recent disc.

## Uninstalling

Delete the program folder or the AppImage. On Linux, and on a Windows
install that is not portable, settings, memory cards, savestates and the
library live in per-user directories that survive this; delete them too for a
full removal. [File locations](../help/file-locations.md) lists them.
