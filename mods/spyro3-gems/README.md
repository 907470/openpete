# spyro3-gems

Replaces Spyro 1's gems with Spyro 3's: the five faceted denomination
models (red/green/blue-violet/gold/pink), the S3 gem palette for the
collect burst particles, and the pitch-cycling collect chime — entirely
through the mod framework (zero engine changes).

## Generating the assets (required — nothing is shipped)

Same legal posture as the M12 S3 player blob: the mod ships **tools**,
not Activision/Insomniac data. You need your own Spyro: Year of the
Dragon disc image (SCUS-94467).

```
python3 tools/extract_s3_gems.py "path/to/Spyro 3 [SCUS-94467].img"
python3 tools/convert_to_s1.py
```

This writes `assets/s3/gems_s1pack.bin` (gitignored). Without it the
mod boots in calibration mode (logs stock gem geometry, changes
nothing).

## How it works

See `docs/injection-spec.md` for byte-level evidence. Summary:

1. **entry** — copy the pack (5 baked-gouraud S1 SimpleModels + the S3
   chime ADPCM body) into free guest RAM at `0x80008000` (below the EXE
   load address; nothing else touches it).
2. **post-hook `PatchMobyModelPointers`** — after every model-install
   pass, repoint `g_Models[83..87]` at the park and refresh the burst
   colour words at `D_8006E44C[12..16]` (main-EXE .data).
3. **SFX overlay service** — `sfx_replace("gem_pickup", …)` stages the
   S3 chime host-side; `sfx_hook` drives the canonical rising ladder
   (`0x400 + 0x40·n`, ~0.5 s streak reset) per play. Guest RAM, sound
   tables and the gameplay `rand()` stream stay byte-stock — the engine
   substitutes on a host copy of the voice attrs at key-on.

## Conversion notes

- S3 gem = moby class 1, a 5-state simple model (one state per
  denomination) in each level's model bank; frame-0 geometry is decoded
  and rebuilt as S1 SHADED-METAL SimpleModels. S3's gem specular formula
  is bit-identical to S1's metal sub-mode (`assets/s3/shading_canonical.md`),
  so faces become S1 metal flat faces carrying S3's own face normals and
  the stock mobys' `m_SpecularMetalColor` tints them per class for free.
- Orientation follows the stock convention (tall axis on model −Y, slim
  tip at the origin = the wobble pivot; `assets/s3/`... see AXIS_MAP).
- S1 lone tris are single-sided; S3 gem tris are screen-facing, so each
  tri is emitted in both windings with the reverse copy's normal negated
  (face counts stay well under the 255 cap).
- Burst-particle words are S3 hues rescaled to stock magnitude with all
  channels multiples of 8 — the particle updater fades by −8/tick per
  channel and only dies at exactly zero (`assets/s3/burst_canonical.md`).
- The chime is bank def 1 (`SoundTable[1]` — S3's header names the slot
  `gemCollect`), canonical rate 11025 Hz, identified by ear from a
  full-bank WAV dump. The asm-traced candidates were red herrings:
  `SoundTable[0x1A]` is the menu blip, `gemChime[0]` the gem-bounce.

## Known deltas vs real Spyro 3

- Gems don't spawn S3's flying digit-sparkles; S1's number mobys and
  yellow twinkle remain (S1 behavior, S3 look for model/burst/chime).
- Chime ladder is canonical: `0x400 + 0x40·n` (a missed delay-slot
  `+0x1000` in the arrival handler; `assets/s3/pitch_canonical.md`),
  capped at one octave with a ~0.5 s streak reset.
- The model/burst guest writes make this mod gameplay-divergent (like
  double-gems); the chime alone would now be sim-neutral via the SFX
  overlay service.
