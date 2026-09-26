# Image quality

These settings change how much detail is drawn and how cleanly. None of
them changes the game.

## Supersampling

Supersampling renders the picture at N times the window's width and height
and averages each NxN block down to one pixel, which smooths jagged edges
and shimmering.

| | |
| --- | --- |
| Settings menu | **SSAA**: **OFF**, **2X**, **3X**, **4X** |
| Dev overlay | **Supersampling** |
| Flag | `--ssaa N` <!-- id: knob:ssaa -->, 1 to 4 <!-- id: knob:ssaa --> |
| Default | 1 (off) <!-- id: knob:ssaa --> |

The cost grows with N squared: 3X renders 9 pixels per shown pixel, 4X
renders 16. Factors above 4 are reduced to 4, which keeps the render targets
inside an 8 GB card at 1080p, and the size is further capped at the GPU's
largest image. Changing the factor causes a brief pause at large factors
while the targets are rebuilt.

## Level of detail

The game draws distant parts of a level with simpler geometry and switches
to the detailed version as you approach. OpenPete can instead draw
everything at full detail.

| Value | Effect |
| --- | --- |
| `original` (default <!-- id: knob:lod -->) | The game's own detail bands |
| `extended` | Full detail at every distance, out to the edge of the view and beyond |

Settings menu **LOD** (**ORIGINAL** / **EXTENDED**), dev overlay **LOD**,
`--lod original|extended` <!-- id: knob:lod -->. In the Spyro 1 package ++l++
<!-- id: key:host.lod_toggle --> switches between the two. `extended` draws
many more triangles in wide views; see [Performance](performance.md).

Switching LOD also switches the fog, as below, until the fog is set by hand.

## Fog

The game fades distant terrain into fog. `--fog-scale N`
<!-- id: knob:fog_scale --> moves the fog N times farther away: 1 is the
game's own fog, 100 turns it off in practice. Range 0.25 to 128
<!-- id: knob:fog_scale -->.

The default follows the level of detail: 1 under `original`, 100 under
`extended` <!-- id: knob:fog_scale -->. Setting a fog scale, by flag or
with the dev overlay's **Fog scale** or **Fog** rows, fixes it; from then on
LOD switches leave the fog alone.

## Terrain visibility

The game skips level geometry it judges unseen from the camera's position,
for a 4:3 view. In widescreen, with the free camera, or with the camera
inside geometry, the skipped parts can show as missing terrain.

| Value | Effect |
| --- | --- |
| `frustum` (default <!-- id: knob:terrain_visibility -->) | Every terrain section inside the view is drawn, hidden or not, and depth decides what shows. This departs from the original presentation. |
| `game` | The game's own visibility decision, as the original drew it |

Dev overlay **Terrain visibility**, `--terrain-visibility frustum|game`
<!-- id: knob:terrain_visibility -->.
