# 0008 - Pad input override hook

Adds `g_PsyX_Pad_InputOverride` so the host can replace SDL keyboard /
gamepad input with a scripted bitfield per frame. Used to implement
`--bk2` BizHawk movie replay; future scripted-input modes
(record/playback, golden-trace harness) reuse the same slot.

## Why an override slot, not just keyboard-poke

The existing `--press FRAME:KEY,...` path (`core/runtime/run.c`) writes
into `SDL_GetKeyboardState`. That works for a handful of menu
confirmations but has three structural problems:

1. **Loses pad fidelity** - only the buttons bound in
   `g_cfg_keyboardMapping` round-trip; L2/R2 separately from L1/R1, L3,
   R3, and analog axes are unreachable.
2. **Drift surface** - three layers of translation (script -> SDL state ->
   keyboard mapping -> PSX bitfield) that each can change.
3. **Conflicts with real input** - a real plugged-in gamepad's bits get
   ANDed in alongside our override, polluting the script.

The hook short-circuits all three: when the host writes a bitfield to
`g_PsyX_Pad_InputOverride`, `PsyX_Pad_InternalPadUpdates` uses it
verbatim and skips both the SDL keyboard read *and* the per-controller
gamepad poll for that frame. Default `0xFFFF` (released) means the patch
is a no-op when the override isn't set, so existing keyboard / gamepad
behaviour is preserved.

## Files changed

### 1. `include/PsyX/PsyX_public.h`

Adds the `extern unsigned short g_PsyX_Pad_InputOverride;` declaration
right after the post-render-overlay hook from patch 0007.

### 2. `src/pad/PsyX_pad.cpp`

- Defines the global with C linkage at file scope (so `openpete_core`
  can extern it via `PsyX_public.h`).
- In `PsyX_Pad_InternalPadUpdates`, branches on the override before
  calling `PsyX_Pad_UpdateKeyboardInput()`.
- Inside the per-controller loop, when the override is active: skip the
  gamepad poll, skip the Select+Start analog-mode toggle (a movie that
  holds those buttons must not flip `pad->id`), force `pad->id = 0x41`
  (digital), and write `kbInputs` straight into `pad->buttons`. The
  `continue` keeps the rest of the per-controller block out of the
  override path entirely.

## How the host uses it

```c
#include "PsyX/PsyX_public.h"

g_PsyX_Pad_InputOverride = bk2_frame_bits;   // 0xFFFF = released
PsyX_UpdateInput();                          // reads override, not SDL
// ... game runs ...
g_PsyX_Pad_InputOverride = 0xFFFF;           // optional: end-of-movie cleanup
```
