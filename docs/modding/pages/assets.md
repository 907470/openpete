# Textures and music

Assets under `assets/` replace game content by file name. Nothing here
reaches guest RAM: every substitution happens on the host side, so an
asset-only mod is non-mutative.

When several enabled mods ship the same file, the mod listed later in
`enabled.txt` wins. Mod roots outrank the base asset directory and the
player's own data directory.

## Textures

`assets/textures/<name>.png` replaces one texture in the native renderer.
Two name forms match:

| Form | Matches |
|---|---|
| `<label>-<thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png` | One texture by content: the upload hash, the texel box within the upload, the palette hash, and the bit depth. The label is the draw's scene category and is ignored by the matcher. |
| `<name>.png` | A symbolic identity the game layer annotates, such as `title_ui` for the title-screen UI atlas. |

To obtain the content names, run the game with
`OPENPETE_TEXPACK_DUMP_DIR=<dir>` set: every texture drawn is decoded from
VRAM and written once under its content name. Screen-space 2D draws are
written with each texel replicated to square pixels, so a dump can be
edited at its displayed aspect and dropped back in unchanged. World
textures are written at 1:1.

A replacement image is sampled at its own resolution, so a larger PNG is
an HD replacement. Fogged replacement texels blend toward the level's
fog colours; a pack whose art re-grades a level retargets that blend with
`api->texpack_fog_shift`.

`assets/textures/own/<hexid>.png` registers a texture under an ID the
mod chooses, for models the mod installs itself.

`--texpack-dir DIR` or `OPENPETE_TEXPACK_DIR` points the base pack at a
directory outside any mod; mod roots always join it.

## Music

`assets/music/track_NN.wav` replaces music track NN, where NN is the
zero-padded index into the game's music table (`track_20.wav` is the
title theme). A `manifest.txt` in the same directory maps other paths to
indices, one `<index> <relative path>` pair per line; a later line for
the same index wins. Both forms work in a mod's `assets/music/` and in
the base directories, and within one directory the manifest mapping is
tried first.

The WAV is PCM 16-bit, mono or stereo, at 11025, 22050, 32000, 44100, or
48000 Hz. It loads whole into RAM.

There is no music census tool; indices come from the game's
`m_MusicTable`.
