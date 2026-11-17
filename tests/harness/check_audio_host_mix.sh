#!/usr/bin/env bash
# check_audio_host_mix.sh - host clips (mod sfx_play) reach the SPU render's
# one mix: the audio dump, and the --record audio track.
#
# Rows: Spyro 1 attract run headless to --frame-limit 8000 under
# OPENPETE_AUDIO_DUMP, once with --mods "" and once with the test mod
# mods/sfx-play-probe (its clip, a 1 kHz 0.25 s tone at 0.5 full
# scale, is written here into a scratch copy of the mod). The mod reads and
# writes no guest RAM, so the two runs render the same SPU output and their
# difference is the clips alone.
#
# Instrument: tools/audio_host_mix_check.py. In the dump difference it
# finds each clip copy by correlation with the tone as the mixer plays it
# (through the SPU gaussian; corr >= 0.999 at a
# copy) and counts them against the mod's play lines. With --record it
# runs a third, windowed leg (WSLg or X; --input-lock --vibration 0) with
# --record, decodes the recording's audio track with ffmpeg and counts the
# tone copies there (AAC is lossy: corr >= 0.9).
#
# Pass: dump copies == plays > 0, every copy at corr >= 0.999 and the
# residual outside the copies 0 samples; with --record, recorded copies ==
# that leg's plays > 0.
#
# Usage: tests/harness/check_audio_host_mix.sh [--build-dir=DIR] [--record]
# Exit 0 PASS, 2 FAIL, 64 SKIP (binary or disc missing, no compiler for
# the mod, or --record without xvfb-run).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

record=0
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --record) record=1;;
        *) echo "check_audio_host_mix: unknown argument $a" >&2; exit 64;;
    esac
done
pg_resolve spyro-1
if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "[audio-host-mix] SKIP (binary or disc missing)"; exit 64
fi

OUT="${PG_BUILD_DIR:-build/linux-release}/audio-host-mix"
rm -rf "$OUT"
mkdir -p "$OUT/mods/sfx-play-probe/assets/sfx/own"
cp -r mods/sfx-play-probe/mod.toml mods/sfx-play-probe/src "$OUT/mods/sfx-play-probe/"
echo sfx-play-probe >"$OUT/mods/enabled.txt"
python3 tools/audio_host_mix_check.py --write-tone "$OUT/mods/sfx-play-probe/assets/sfx/own/probe.wav" || exit 2

PG_CAPTURE_TIMEOUT=1200
PG_CAPTURE_ENV=("OPENPETE_AUDIO_DUMP=$OUT/base.raw")
pg_capture "$OUT/base.log" "$OUT/base.raw" -- --mods "" --no-card --mute 0 --frame-limit 8000 </dev/null || {
    echo "[audio-host-mix] verdict=FAIL: base run failed (see $OUT/base.log)"; exit 2; }
PG_CAPTURE_ENV=("OPENPETE_AUDIO_DUMP=$OUT/mod.raw")
pg_capture "$OUT/mod.log" "$OUT/mod.raw" -- --mods-dir "$OUT/mods" --no-card --mute 0 --frame-limit 8000 </dev/null || {
    echo "[audio-host-mix] verdict=FAIL: mod run failed (see $OUT/mod.log)"; exit 2; }
if ! grep -q "sfx-play-probe: play" "$OUT/mod.log"; then
    if grep -qi "compile\|compiler" "$OUT/mod.log"; then
        echo "[audio-host-mix] SKIP (the test mod did not load; see $OUT/mod.log)"; exit 64
    fi
fi

args=(--tone "$OUT/mods/sfx-play-probe/assets/sfx/own/probe.wav"
      --base "$OUT/base.raw" --mod "$OUT/mod.raw" --mod-log "$OUT/mod.log")
if (( record )); then
    op_xvfb_prefix audio-host-mix 1280x960
    rm -f "$OUT/rec.mkv" "$OUT/rec.log"
    OPENPETE_VIBRATION=0 op_own_run "${OP_XVFB[@]}" timeout -k 10 1200 "$PG_BIN" --no-config --disc "$PG_DISC" --disc-cache 0 \
        --input-lock --vibration 0 --window 640x480 --mods-dir "$OUT/mods" --no-card --mute 0 \
        --frame-limit 8000 --record "$OUT/rec.mkv" >"$OUT/rec.log" 2>&1 </dev/null
    [[ -f "$OUT/rec.mkv" ]] || { echo "[audio-host-mix] verdict=FAIL: no recording (see $OUT/rec.log)"; exit 2; }
    args+=(--record "$OUT/rec.mkv" --record-log "$OUT/rec.log")
fi
python3 tools/audio_host_mix_check.py "${args[@]}"
