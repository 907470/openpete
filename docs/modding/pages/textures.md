# Textures

`assets/textures/<name>.png` replaces one texture in the native renderer.
Nothing here reaches guest RAM: every substitution happens on the host
side, so a texture-only mod is non-mutative.

When several enabled mods ship the same file, the mod listed later in
`enabled.txt` wins. Mod roots outrank the base asset directory and the
player's own data directory.

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
