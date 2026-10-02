#!/usr/bin/env bash
# check_text_font.sh - the 3D glyph text gate.
#
# The settings menu, the OSD toasts and the performance readout draw with the
# game's own glyph models (games/common/renderer/text_moby.c). Spyro 1 has its
# font in every session; Spyro 3 has it only while a home world is loaded, so
# the producer keeps the decoded glyphs in a cache file (OPENPETE_FONT_CACHE).
# This gate runs each row of games/<slug>/corpus/text-font-corpus.manifest with the
# three consumers forced live (OPENPETE_OSD_DEMO, OPENPETE_MENU_DEMO,
# --perf-stats fps) and reads the glyph vertex counts the producer published
# (OPENPETE_TEXT_GLYPH_CENSUS).
#
# Manifest fields, pipe-separated: name | level | cache | expect
#   cache   cold   a path that does not exist
#           fresh  a path that does not exist; the row must write it
#           warm   the file the named `fresh` row wrote (warm:<row name>)
#           bad-checksum:<row> | bad-disc:<row> | bad-version:<row>
#                  that file with one payload byte, the disc name or the
#                  version changed; the producer must reject it
#   expect  none   every consumer published zero glyph vertices all run
#           ram    every consumer published glyph vertices, snapshot from RAM
#           file   every consumer published glyph vertices, snapshot from the file
#
# Every cache path is under the run's own output directory, so no row depends
# on a file an earlier session left on the machine. The rows run windowed:
# the producer is inert without the native display (a headless run publishes
# nothing), so a machine without a display SKIPs.
#
#   tests/harness/check_text_font.sh [--game SLUG] [--build-dir=DIR]
#
# Exit 0 PASS, 1 FAIL or a missing corpus manifest (the message names the
# path), 64 SKIP (binary, disc or display missing).
# OPENPETE_FRAME_LIMIT overrides the 1800 VBLs per row.
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
FRAMES="${OPENPETE_FRAME_LIMIT:-1800}"
OUT="${TEXT_FONT_OUT:-$BUILD/text-font/$GAME}"

if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "check_text_font: SKIP ($PG_BIN or $PG_DISC missing)"; exit 64
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "check_text_font: SKIP (no display; the producer is inert headless)"; exit 64
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.bin

# tamper SRC DST KIND: copy with one field changed (header: magic 0-3,
# version 4-7, disc name 8-23, payload from 44).
tamper() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys
src, dst, kind = sys.argv[1:4]
b = bytearray(open(src, 'rb').read())
off = {'bad-checksum': 44 + 5, 'bad-disc': 8, 'bad-version': 4}[kind]
b[off] ^= 0x55
open(dst, 'wb').write(bytes(b))
PY
}

status=0; rows=0
while IFS='|' read -r name level cache expect; do
    name="${name//[[:space:]]/}"; level="${level//[[:space:]]/}"
    cache="${cache//[[:space:]]/}"; expect="${expect//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    rows=$((rows + 1))
    log="$OUT/$name.log"; file="$OUT/$name.bin"; reject=""
    case "$cache" in
        cold|fresh) ;;
        warm:*) cp "$OUT/${cache#warm:}.bin" "$file" || { echo "FAIL $name: no file from ${cache#warm:}"; status=1; continue; } ;;
        bad-*:*) reject="${cache%%:*}"
                 tamper "$OUT/${cache#*:}.bin" "$file" "$reject" || { echo "FAIL $name: cannot tamper"; status=1; continue; } ;;
        *) echo "FAIL $name: unknown cache mode $cache"; status=1; continue ;;
    esac
    OPENPETE_OSD_DEMO=3 OPENPETE_MENU_DEMO=1 OPENPETE_TEXT_GLYPH_CENSUS=1 \
    OPENPETE_FONT_CACHE="$file" \
        timeout 600 "$PG_BIN" --no-config --vibration 0 --mouse 0 --input-lock --disc "$PG_DISC" \
        --renderer vk --aspect 4:3 --lod original --no-card --mute 1 \
        --perf-stats fps --skip-to-level "$level" --frame-limit "$FRAMES" \
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
    case "$cache" in
        fresh) [[ -s "$file" ]] || verdict="FAIL no cache file written; $verdict" ;;
        cold)  [[ -e "$file" ]] && verdict="FAIL a level without the font wrote a cache file; $verdict" ;;
    esac
    if [[ -n "$reject" ]] && ! grep -aq "glyph cache .* rejected" "$log"; then
        verdict="FAIL tampered file ($reject) was not rejected; $verdict"
    fi
    [[ $rc -ne 0 ]] && verdict="FAIL exit $rc; $verdict"
    echo "$name [$level, $cache, expect $expect]: $verdict"
    [[ "$verdict" == PASS* ]] || status=1
done < "$MANIFEST"

if [[ $rows -eq 0 ]]; then echo "check_text_font: VACUOUS (no rows in $MANIFEST)"; exit 1; fi
[[ $status -eq 0 ]] && echo "check_text_font: PASS ($rows rows, $GAME)" || echo "check_text_font: FAIL ($GAME)"
exit $status
