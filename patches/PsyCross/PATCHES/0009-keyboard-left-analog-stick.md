# 0009 — Keyboard-driven left analog stick (WASD)

Maps **W/A/S/D** to the PSX left analog stick when the keyboard is the
active controller. Holding any of the four keys flips the pad into
analog mode (`pad->id = 0x73`) and writes `pad->analog[2..3]` so games
that read the stick (Spyro's `PadApplyCali` / `CONTROLLER_TYPE_STICK`
path) get real movement out of the keyboard.

## Why

Out of the box PsyCross only exposes digital buttons over the keyboard
path — the arrow keys are wired to the d-pad and the keyboard branch
in `PsyX_Pad_InternalPadUpdates` actively forces `pad->id = 0x41`
(digital), zeroing the analog axes regardless of what the gamepad
sub-poll wrote. For Spyro that means keyboard-only play can't aim
finely (camera/charge are stick-driven). Adding a keyboard binding for
the left stick is the smallest change that unblocks keyboard play.

Right stick is intentionally unbound: there are no four free keys near
the home row that don't conflict with existing bindings (X/C/Z/V on
face buttons, arrows on d-pad), and no Spyro feature requires the right
stick.

## Files changed

### 1. `include/PsyX/PsyX_public.h`

Adds four `kc_axis_left_*` fields to `PsyXKeyboardMapping`. Trailing
position in the struct so existing callers/initializers that reference
the struct by member name keep working unchanged.

### 2. `src/PsyX_main.cpp`

Defaults `kc_axis_left_up/down/left/right` to `W/S/A/D` in
`PsyX_Sys_InitialiseInput`.

### 3. `src/pad/PsyX_pad.cpp`

- Forward-declares `PsyX_Pad_UpdateKeyboardLeftStick` (alongside the
  existing forward decls).
- Defines `PsyX_Pad_UpdateKeyboardLeftStick`: samples the four WASD
  scancodes and produces 0/128/255 values for LX/LY (full deflection
  per axis; diagonals are corner deflections — keyboard is binary).
  Returns whether any axis key is currently held.
- Rewrites the keyboard branch in `PsyX_Pad_InternalPadUpdates`:
  - Drops the `kbInputs != 0xffff` gate so the branch runs even when
    only WASD is held (no digital button pressed).
  - When WASD is held, switches the pad to analog mode (`id = 0x73`)
    and writes `pad->analog[2..3]`.
  - Suppresses the existing "force back to `0x41`" path while WASD is
    held — otherwise the digital-button branch would clobber the
    analog mode we just enabled and leave the stick values stranded.
  - Digital button ANDing (`pad->buttons &= kbInputs`) still runs
    whenever any non-axis key is pressed, so face buttons / d-pad /
    Start / Select behave unchanged.

## Behavioural notes

- No effect when no WASD key is held: `pad->id` is left alone, the
  original "force digital on keyboard input" path still runs, and
  `pad->analog[2..3]` retains whatever the gamepad poll (or the
  no-controller default of 127) wrote.
- Order is W=up, S=down, A=left, D=right with U/D and L/R held
  simultaneously cancelling to centre on that axis (last wins is
  avoided by checking both).
- Analog values are full-on/off (0 / 255) rather than partial — there's
  no per-frame ramp. If smooth ramping is needed later it should be
  layered on top here, not in game code.
