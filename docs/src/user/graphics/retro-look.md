# The PlayStation look

The native renderer draws with full precision by default. Four settings
bring back the PlayStation's own artifacts, each on its own. They change
only the picture. Wobble, pixelation and dither apply to the 3D scene; the
HUD stays smooth.

| Look | Settings menu | Dev overlay | Flag | Default |
| --- | --- | --- | --- | --- |
| [Wobble](#wobble) | **WOBBLE** | **Vertex precision** | `--vertex-precision subpixel\|integer` <!-- id: knob:vertex_precision home --> | `subpixel` (off) <!-- id: knob:vertex_precision --> |
| [Pixelation](#pixelation) | **PIXELATION** | **Raster quantize** | `--raster-quantize N` <!-- id: knob:raster_quantize --> | off, or 1 with wobble on <!-- id: knob:raster_quantize --> |
| [Texture warp](#texture-warp) | **TEXTURES** | **Texture mapping** | `--texture-mapping perspective\|affine` <!-- id: knob:texture_mapping home --> | `perspective` (**CORRECT**) <!-- id: knob:texture_mapping --> |
| [Dither](#dither) | **DITHER** | **Color depth** (**15-bit** = on) | `--dither 0\|1` <!-- id: knob:dither home --> | 0 (off) <!-- id: knob:dither --> |

## Wobble

The PlayStation placed polygon corners on whole pixels of its 512x240
screen, so polygons jitter and wobble as they move. `integer` snaps 3D
corners to that grid; `subpixel` places them exactly. Depth and texture
coordinates are not snapped.

## Pixelation

`--raster-quantize N` resamples the 3D scene to N times the PlayStation's
512x240 grid, one sample per cell as the hardware drew it. 1 is the original
resolution: 224 visible lines across the screen, as on a TV. 0 turns it off;
the dev overlay row takes 0 to 16 <!-- id: knob:raster_quantize home -->, the
settings menu offers **OFF**, **1X**, **2X**, **4X**, **8X** and **16X**.
The grid stays aligned with the wobble grid at every N.

Wobble without pixelation leaves fine cracks between polygons visible;
pixelation without wobble gives a pixelated picture with smooth motion.
Unless set, pixelation follows wobble: 1 with wobble on, off without.

## Texture warp

The PlayStation interpolated textures and colours in screen space, with no
perspective correction, so textures on large polygons swim and bend as the
camera moves. `affine` reproduces this; `perspective` interpolates
correctly. A change rebuilds the renderer's pipelines at the next picture,
with a brief pause.

## Dither

The PlayStation drew in 15-bit colour and hid the banding with a 4x4
ordered dither pattern. `--dither 1` quantizes the 3D scene to 15-bit
colour with the same pattern, on the PlayStation's 512x240 grid.
