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
3. **post-hook `SetNewSoundTable`** — after the level's SPU bank upload,
   `call_addr` the psyq `SpuSetTransferStartAddr`/`SpuWrite` thunks to
   place the chime in the free SPU tail, then repoint the `gemPickup`
   SoundDefinition (addr/pitch/no-loop/no-variance).
4. **pre-hook `func_8003B854`** (gem collect) — step the S3 pitch ladder
   (`+centre/16` per consecutive collect, ~3 s streak reset) into the
   definition before `PlaySound` reads it.

## Conversion notes

- S3 gem = moby class 1, a 5-state simple model (one state per
  denomination) in each level's model bank; frame-0 geometry is decoded
  and rebuilt as S1 baked-colour SimpleModels (vertex s8×2 stream,
  16-step dark→bright gradient from S3's own palette `D_80066998`,
  ramp semantics inverted: ramp 0 = bright).
- S1 lone tris are single-sided; S3 gem tris are screen-facing, so each
  tri is emitted in both windings (face counts stay well under the 255
  cap).
- The chime is the *sparkle-arrival* sample (`SoundTable[0x1A]` in S3),
  the one the rising-pitch code actually plays.

## Known deltas vs real Spyro 3

- Gems don't spawn S3's flying digit-sparkles; S1's number mobys and
  yellow twinkle remain (S1 behavior, S3 look for model/burst/chime).
- The exact S3 first-chime pitch is start-of-ladder `0x400` (centre);
  S3's per-burst counter start is untraced (see
  `assets/s3/pitch_ladder.md`).
- Sound variance is zeroed for the gem sound, so the gameplay `rand()`
  stream diverges from stock — this mod is intentionally
  gameplay-divergent (like double-gems).
