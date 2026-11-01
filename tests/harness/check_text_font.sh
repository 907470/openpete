#!/usr/bin/env bash
# check_text_font.sh - the 3D glyph text gate.
#
# The settings menu, the OSD toasts and the performance readout draw with the
# game's own glyph models (games/common/renderer/text_moby.c), read from the
# disc image at the first draw: Spyro 1's HUD font, Spyro 2's and Spyro 3's
# credits font. This gate runs each row of
# games/<slug>/corpus/text-font-corpus.manifest with the three consumers
# forced live (OPENPETE_OSD_DEMO, OPENPETE_MENU_DEMO, --perf-stats fps) and
# reads the glyph vertex counts the producer published
# (OPENPETE_TEXT_GLYPH_CENSUS).
#
# Manifest fields, pipe-separated: name | level | expect
#   level   the --skip-to-level target, or - for a run from boot that loads
#           no level (the title and attract loop)
#   expect  none   every consumer published zero glyph vertices all run
#           disc   every consumer published glyph vertices, read from the disc
#
# The rows run windowed: the producer is inert without the native display (a
# headless run publishes nothing), so a machine without a display SKIPs.
#
#   tests/harness/check_text_font.sh [--game SLUG] [--build-dir=DIR]
#
# Exit 0 PASS, 1 FAIL or a missing corpus manifest (the message names the
# path), 64 SKIP (binary, disc or display missing).
# OPENPETE_FRAME_LIMIT overrides the per-row limit: 1800 on Spyro 1, 2373 on
# Spyro 2 and 3 (their feed key counts the 573 fast-boot VBLs).
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD="${a#--build-dir=}";; esac; done
GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD" pg_resolve "$GAME" || exit 64
MANIFEST="$(pg_corpus_manifest "$GAME" text-font-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
FRAMES_DEFAULT=1800
case "$GAME" in spyro-2|spyro-3) FRAMES_DEFAULT=2373;; esac
FRAMES="${OPENPETE_FRAME_LIMIT:-$FRAMES_DEFAULT}"
OUT="${TEXT_FONT_OUT:-$BUILD/text-font/$GAME}"

if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "check_text_font: SKIP ($PG_BIN or $PG_DISC missing)"; exit 64
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "check_text_font: SKIP (no display; the producer is inert headless)"; exit 64
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.log

status=0; rows=0
while IFS='|' read -r name level expect; do
    name="${name//[[:space:]]/}"; level="${level//[[:space:]]/}"
    expect="${expect//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    rows=$((rows + 1))
    log="$OUT/$name.log"
    skip=(--skip-to-level "$level")
    [[ "$level" == - ]] && skip=()
    OPENPETE_OSD_DEMO=3 OPENPETE_MENU_DEMO=1 OPENPETE_TEXT_GLYPH_CENSUS=1 \
        op_own_run timeout -k 10 600 "$PG_BIN" --no-config --vibration 0 --mouse 0 --input-lock --disc "$PG_DISC" \
        --renderer vk --aspect 4:3 --lod original --no-card --mute 1 \
        --perf-stats fps "${skip[@]}" --frame-limit "$FRAMES" \
        > "$log" 2>&1
    rc=$?
    [[ -s "$log" ]] || { echo "FAIL $name: no log"; status=1; continue; }

    # Per consumer: the largest glyph vertex count published, and its source.
    verdict="$(python3 - "$log" "$expect" <<'PY'
import re, sys
best = {'perf': (0, 'none'), 'menu': (0, 'none'), 'osd': (0, 'none')}
seen = 0
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[text-glyph vbl=\d+\] consumer=(\w+) verts=\d+ glyph_verts=(\d+) source=(\w+)', line)
    if not m: continue
    seen += 1
    if int(m.group(2)) > best[m.group(1)][0]:
        best[m.group(1)] = (int(m.group(2)), m.group(3))
exp = sys.argv[2]
txt = ' '.join('%s=%d/%s' % (k, v[0], v[1]) for k, v in best.items())
if seen == 0: print('VACUOUS no census line:', txt); sys.exit()
if exp == 'none':
    ok = all(v[0] == 0 for v in best.values())
else:
    ok = all(v[0] > 0 and v[1] == exp for v in best.values())
print(('PASS ' if ok else 'FAIL ') + txt)
PY
)"
    [[ $rc -ne 0 ]] && verdict="FAIL exit $rc; $verdict"
    echo "$name [$level, expect $expect]: $verdict"
    [[ "$verdict" == PASS* ]] || status=1
done < "$MANIFEST"

if [[ $rows -eq 0 ]]; then echo "check_text_font: VACUOUS (no rows in $MANIFEST)"; exit 1; fi
[[ $status -eq 0 ]] && echo "check_text_font: PASS ($rows rows, $GAME)" || echo "check_text_font: FAIL ($GAME)"
exit $status
