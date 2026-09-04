# teapot-dragons

Every crystallised dragon statue (moby class 250, the last model of every
level) is a Utah teapot. The mod is a model-replacement example at the
format's ceiling: the densest model the Spyro 1 animated moby format
admits in one moby.

```
python3 tools/gen_teapot.py        # -> assets/teapot_s1pack.bin
```

The generator tessellates the public-domain Newell patch data
(`tools/teapot.bpt`) under the format's ceilings: the animation header's
per-tier vertex count is one byte (255), face words carry 9-bit vertex
indices, vertex literals are 11/11/12-bit. It emits an LP tier (one quad
per patch) as a prefix of the same vertex stream, which is how the game
selects tiers. `--budget`, `--height`, `--halfwidth`, `--yaw` and the
colour flags reshape it; `--single-sided` drops the double-sided face
flag.

The mod parks the payload in the mod arena, exchanges each frame word's
stream offset for a stream handle, and installs the model over class 250
from a `PatchMobyModelPointers` post-hook, copying the level's own sound
and collision words into the header. A hook around `LoadDragonCutscene`
shows that loader the stock row while it sizes the cutscene read, since
the game measures its model buffer up to the class-250 row; a model
parked outside the buffer would otherwise make the read overrun it. `debug.dump` logs the stock model
and the statue mobys once per level; a replacement for another class
starts from that dump.
