# Smoothness

The game updates 29.913 times a second; each update is a
[tick](../help/glossary.md#tick). OpenPete can show more pictures than that
by drawing in-between positions, so motion looks smoother at a higher
[frame rate](../help/glossary.md#frame-rate) while the game itself runs
exactly as on a PlayStation.

## Frame rate

`--render-fps N` <!-- id: knob:render_fps home --> sets the target number of
pictures per second. The default is your display's refresh rate
<!-- id: knob:render_fps -->.

| Value | Effect |
| --- | --- |
| above 30 | Pictures between ticks are interpolated; see below |
| `0` | One picture per tick, the original presentation |

With [VSync](display.md#vsync) on, the frame rate cannot exceed the
display's refresh rate.

The settings menu's **FPS** row offers 30, 60, then steps of 60 below the
refresh rate, the refresh rate itself, and **UNLIMITED**, which draws as fast
as the GPU allows (still capped by VSync when it is on). The dev overlay's
**Render FPS** row has an **Interpolation** checkbox; unchecked is a target of
0, one present per tick, and checked shows the target beside it, 1 to 1000
<!-- id: knob:render_fps -->, starting at the current one.

## Interpolation

Between two ticks, OpenPete draws the scene at in-between positions computed
from the two ticks' states: Spyro, objects, the camera, moving and animated
terrain. Interpolation needs the next tick before it can draw toward it, so
the picture trails the game by up to one tick. [Runahead](#runahead) cancels
that delay.

Interpolation is on whenever the frame rate is above 30.
{{ key("key:host.interp_toggle") }} <!-- id: key:host.interp_toggle home --> turns it off and on for
comparison; with it off each tick's picture is shown unchanged until the
next.

## Runahead

`--runahead N` <!-- id: knob:runahead --> computes N ticks ahead of the shown
picture and shows the result, which removes N ticks of delay between a
button press and its effect on screen. Default 0 (off)
<!-- id: knob:runahead -->; the dev overlay's **Runahead** row has a
checkbox for off and on, and while on, buttons for 1, 2 and 3
<!-- id: knob:runahead -->.

| N | Effect |
| --- | --- |
| 0 | Off |
| 1 | Recommended with interpolation: cancels its one-tick delay exactly, for the input response of a PlayStation with high frame rate motion; every animation step is shown |
| 2 | Less delay than a PlayStation, but a press can skip the first step of the animation it starts |

Each tick of runahead costs about one extra tick of CPU work per tick. The
game's state is unchanged by it: runahead computes the same ticks, earlier.
Runahead is suspended during level loads. A mod can refuse runahead; the
dev overlay row then returns to 0. A change shows as a one-tick jump.
