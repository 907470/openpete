#!/usr/bin/env bash
# check_blendorder_ident.sh - the blended-world draw order's identity check.
#
# The blended world classes draw in emission order, because the guest links
# them into the ordering table interleaved and the blend equations do not
# commute once a channel clamps: additive over subtractive saturates where
# subtractive over additive does not. The game render emits in the order the
# guest links its primitives; a sub-tick re-extract emits one producer at a
# time, in the order its body calls them, which the producers' data
# dependencies fix. Where the two orders differ the replay composites a picture
# the canonical present never drew, and the pixels that carry it are exactly the
# ones where two non-commuting classes overlap - a handful, anywhere on screen.
#
# Instrument: the draw order itself, not the pixels. OPENPETE_BWDRAWORDER
# prints the blended-world block of a present as the ordered pass will draw it,
# one line per maximal run of one provenance key, blend class and texturing
# mode, with its triangle count. Scene indices are absent on purpose: the two
# legs build different scenes, so only the run SEQUENCE is comparable, and that
# sequence is what the blend equations see.
#
# Two legs of one row, one binary, the same tick:
#   canon   --screenshot-on-frame V                  (the game render's block)
#   ident   --screenshot-intermediate-on-frame V     (the replay's block,
#           with OPENPETE_INTERMEDIATE_ALPHA=1, the identity instant)
# The canonical present is labelled f=V and the sub-tick present f=V+1, so the
# dumps are matched by leg, not by label.
#
# Pass criterion, per row: the two run sequences are identical. VACUOUS (exit 1)
# when either leg dumps no run at all - a row whose tick draws nothing blended
# measures nothing.
#
# Input: games/<slug>/corpus/blendorder-ident-corpus.manifest, rows
#   <source> | <V>
# <source> is a movie under games/<slug>/bk2, or any name not ending in .bk2,
# which is the plain boot - the title screen and the attract demo, which need no
# input - replayed with no movie, the same movie-less row form the presentation
# corpus takes.
#
# Verdict line per row:
#   [blendorder-ident] game=<g> row=<r> V=<v> runs=<n> tris=<n> verdict=PASS|FAIL
#
# Usage: tests/harness/check_blendorder_ident.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (binary, disc or bk2 missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

MANIFEST="$(pg_corpus_manifest "$GAME" blendorder-ident-corpus.manifest)"
pg_manifest "$MANIFEST" line
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "blendorder-ident: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/blendorder-ident/$GAME"
mkdir -p "$OUT"

# One replay of a row's movie capturing one leg. The screenshot request is what
# makes the native renderer compose at all under --headless, so every leg
# carries one; the dump rides that compose.
run_leg() {   # tag flag alpha
    local tag="$1" flag="$2" alpha="$3"
    # --screenshot-native-only writes <base>-vk.png, which is the file the
    # run must produce for the compose (and so the dump) to have happened.
    local log="$OUT/$tag.log" shot="$OUT/$tag.png" vk="$OUT/$tag-vk.png"
    PG_CAPTURE_ENV=(OPENPETE_BWDRAWORDER="$V-$((V + 2))")
    [[ -n "$alpha" ]] && PG_CAPTURE_ENV+=(OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$vk" -- \
        --renderer vk --mute 1 --aspect 16:9 --lod original \
        "${MOVIE_ARG[@]}" --screenshot-native-only \
        "$flag" "$V" --screenshot-output "$shot" \
        --frame-limit $((V + 3))
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r src vfield <<<"$line"
    src="${src// /}"; V="${vfield// /}"
    if [[ "$src" == *.bk2 ]]; then
        MOVIE="$PG_BK2_DIR/$src"
        [[ -f "$MOVIE" ]] || { echo "blendorder-ident: SKIP ($MOVIE missing)"; exit 64; }
        MOVIE_ARG=(--bk2 "$MOVIE")
    else
        MOVIE_ARG=()
    fi
    rows=$((rows + 1))
    tag="${src%.bk2}"

    run_leg "${tag}__canon" --screenshot-on-frame "" || { fail=1; continue; }
    run_leg "${tag}__ident" --screenshot-intermediate-on-frame 1 || { fail=1; continue; }

    python3 - "$GAME" "$tag" "$V" "$OUT/${tag}__canon.log" "$OUT/${tag}__ident.log" <<'PYEOF' || fail=1
import sys
game, row, V, canon_log, ident_log = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]

def runs(path):
    out, tris = [], 0
    for ln in open(path, errors="replace"):
        if ln.startswith("[bwdo] "):
            f = dict(p.split("=", 1) for p in ln.split()[1:])
            out.append((f["key"], f["blend"], f["tex"], f["tris"]))
            tris += int(f["tris"])
    return out, tris

a, atris = runs(canon_log)
b, btris = runs(ident_log)
head = ("[blendorder-ident] game=%s row=%s V=%d runs=%d/%d tris=%d/%d"
        % (game, row, V, len(a), len(b), atris, btris))
if not a or not b:
    print(head + " VACUOUS (a leg drew no blended run)")
    sys.exit(1)
bad = []
for i in range(max(len(a), len(b))):
    x = a[i] if i < len(a) else None
    y = b[i] if i < len(b) else None
    if x != y:
        bad.append("  run %d: canon=%s ident=%s" % (i, x, y))
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for l in bad[:20]:
    print("[blendorder-ident] " + l)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "blendorder-ident: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
