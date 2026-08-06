#!/bin/bash
# Moby-vs-terrain contact gate: capture one scenedump per pose and report
# pixels whose moby-vs-terrain winner flips between the shipped ordering
# depth (authored OT shifts baked in) and TRUE view-Z geometry.
# See scripts/moby_contact_gate.py for the model. Baseline is nonzero while
# terrain W1/W2 offsets and moby m_DepthOffset pulls live in stored depth;
# the gate is the P6b/P6e "goes clean" instrument.
#
# Usage: scripts/check_moby_contact.sh [outdir]
#   MAX_VIOL=<n>  fail (exit 2) if any pose exceeds n violation px (default:
#                 report-only)
set -u
cd "$(dirname "$0")/.."
BIN=build/linux-release/games/spyro-1/openpete-spyro1
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
OUT=${1:-build/moby-contact}
mkdir -p "$OUT"
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}

# name|level|freecam pose ("" = canonical camera)|frame
POSES=(
  'artisans-gem-wall|artisans|pos=(95055, 43826, 7882) yaw=-599 pitch=-84|1900'
  'artisans-spawn-contact|artisans||1900'
  'dream_weavers-spawn-contact|dream_weavers||1900'
  'town_square-spawn-contact|town_square||1900'
)

fail=0
for spec in "${POSES[@]}"; do
  IFS='|' read -r name level pose frame <<< "$spec"
  log="$OUT/$name.log"
  if [ ! -s "$log" ] || ! grep -q '\[scenedump' "$log"; then
    args=(--headless --disc "$DISC" --run-recomp 0x80012204 --renderer vk
          --mute 1 --aspect 4:3 --lod original --skip-to-level "$level"
          --screenshot-on-frame "$frame" --frame-limit $((frame + 4))
          --screenshot-output "$OUT/$name-{n}.png" --max-runtime-seconds 380)
    [ -n "$pose" ] && args+=(--freecam-pose "$pose")
    # Headless composes the canonical-LAG scene once per screenshot; arm the
    # dump below the lag label so that compose trips it.
    OPENPETE_SCENEDUMP=$((frame - 10)) "$BIN" "${args[@]}" > "$log" 2>&1
  fi
  line=$(python3 scripts/moby_contact_gate.py --scenedump "$log" \
           --heat "$OUT/$name-heat.png" --label "$name")
  echo "$line"
  v=$(echo "$line" | sed -n 's/.*VIOLATIONS: \([0-9]*\) px.*/\1/p')
  if [ -n "${MAX_VIOL:-}" ] && [ "${v:-0}" -gt "$MAX_VIOL" ]; then fail=2; fi
done
exit $fail
