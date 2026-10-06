#!/usr/bin/env bash
# check_disc_trace.sh - the host disc-read trace (OPENPETE_DISC_TRACE) on a
# Spyro 1 boot.
#
# Run: two headless boots to VBL 600 (the boot reads, the title-screen
# loads and the XA stream start at about VBL 439), one with the disc cache
# attached and one with it off.
#
# Instrument: the trace file, one tab-separated row per host disc read
# (ns, vbl, tag, lba, count, result, offset, length; see
# core/include/openpete_disc_trace.h), and the CdRead thunk's INFO lines
# (`CdRead sectors=<n>`), PsyCross-side count of the sectors the game asked
# for.
#
# Pass criterion, per leg: the header and every row parse (8 fields, known
# tag and result, integer columns, count = sectors spanned by offset and
# length); an `exe` row; a `libcd` row; a `woah` row and the woah loader's
# `crash-woah: decoded` line (the title-screen NSF read through the disc
# reader on its own thread); the libcd rows' sector sum equals the CdRead
# lines' sector sum; with the cache on, a `cache-load` row and libcd results
# hit or miss; with it off, no cache-load row and every result direct.
# VACUOUS (exit 1) when the run logged no CdRead:
# a trace of a boot that read nothing proves nothing.
#
# Verdict line:
#   [disc-trace] leg=<cache|nocache> rows=<n> libcd_sectors=<n>
#                cdread_sectors=<n> verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_disc_trace.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME=spyro-1
pg_resolve "$GAME"

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "disc-trace: SKIP (binary or disc missing)"; exit 64; }

LIMIT=600
OUT="${PG_BUILD_DIR:-build/linux-release}/disc-trace/$GAME"
mkdir -p "$OUT"
PG_CAPTURE_TIMEOUT=600
fail=0

# check_leg LEG CACHE: one boot with --disc-cache CACHE, then the checks.
check_leg() {
    local leg="$1" cache="$2"
    local log="$OUT/$leg.log" tsv="$OUT/$leg.tsv"
    PG_CAPTURE_ENV=("OPENPETE_DISC_TRACE=$tsv")
    pg_capture "$log" "$tsv" -- \
        --renderer vk --mute 1 --vibration 0 --disc-cache "$cache" \
        --frame-limit "$LIMIT" </dev/null
    local rc=$?
    if (( rc )); then
        echo "[disc-trace] leg=$leg verdict=FAIL: run exited $rc (see $log)"
        return 1
    fi

    local cdread
    cdread="$(grep -oE 'CdRead sectors=[0-9]+' "$log" |
              awk -F= '{s += $2} END {print s + 0}')"
    local woah
    woah="$(grep -c 'crash-woah: decoded' "$log")"
    local verdict
    verdict="$(awk -F'\t' -v leg="$leg" -v cache="$cache" -v cdread="$cdread" -v woah="$woah" '
        BEGIN {
            split("libcd xa exe serial extract woah sfx wad ingest cache-load iso mod font terrain", t, " ")
            for (i in t) tags[t[i]] = 1
            res["hit"] = 1; res["miss"] = 1; res["direct"] = 1
            bad = ""
        }
        NR == 1 {
            if ($0 != "#ns\tvbl\ttag\tlba\tcount\tresult\toffset\tlength") bad = "header"
            next
        }
        {
            rows++
            if (NF != 8) { bad = "line " NR ": " NF " fields"; next }
            for (i = 1; i <= 8; i++)
                if (i != 3 && i != 6 && $i !~ /^-?[0-9]+$/) { bad = "line " NR ": field " i; next }
            if (!($3 in tags)) { bad = "line " NR ": tag " $3; next }
            if (!($6 in res)) { bad = "line " NR ": result " $6; next }
            if ($4 != int($7 / 2352) || $5 != int(($7 + $8 + 2351) / 2352) - $4) {
                bad = "line " NR ": lba/count do not match offset/length"; next
            }
            n[$3]++
            if (cache == 0 && $6 != "direct") bad = "line " NR ": " $3 " " $6 " with the cache off"
            if ($3 == "libcd") {
                libcd += $5
                if (cache == 1 && $6 == "direct") bad = "line " NR ": libcd direct with the cache on"
                if (cache == 0 && $6 != "direct") bad = "line " NR ": libcd " $6 " with the cache off"
            }
        }
        END {
            head = "rows=" rows + 0 " libcd_sectors=" libcd + 0 " cdread_sectors=" cdread
            if (cdread == 0) { print head " verdict=VACUOUS: no CdRead logged"; exit }
            if (bad == "" && rows == 0) bad = "no rows"
            if (bad == "" && !n["exe"]) bad = "no exe row"
            if (bad == "" && !n["libcd"]) bad = "no libcd row"
            if (bad == "" && !n["woah"]) bad = "no woah row"
            if (bad == "" && woah == 0) bad = "woah NSF not decoded"
            if (bad == "" && libcd != cdread) bad = "libcd sectors differ from CdRead sectors"
            if (bad == "" && cache == 1 && !n["cache-load"]) bad = "no cache-load row"
            if (bad == "" && cache == 0 && n["cache-load"]) bad = "cache-load row with the cache off"
            print head (bad == "" ? " verdict=PASS" : " verdict=FAIL: " bad)
        }' "$tsv")"
    echo "[disc-trace] leg=$leg $verdict"
    [[ "$verdict" == *"verdict=PASS"* ]]
}

check_leg cache 1 || fail=1
check_leg nocache 0 || fail=1
exit "$fail"
