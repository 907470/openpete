#!/usr/bin/env bash

# scripts/check_psx_oracle.sh — run openpete --psxtrace-verify against a
# DuckStation (real SCPH1001 BIOS) baseline trace. The trace's per-tick
# oracle hashes (one per gameplay-state region) are value-equality
# compared against openpete's live hashes; first mismatch = first leak.
#
# Rows flagged cd_load_transient in the manifest (the moby pools and the
# main-image state the level load fills or advances while sectors arrive)
# legitimately diverge for a load's duration (openpete lands a read
# instantly; the reference lands it over the load's VBLs) and re-converge;
# the verifier tolerates a divergence there that heals within the window.
# Rows flagged gameplay_neutral or host_precision are hashed and logged and
# never fail on a divergence; their per-record divergence count is bounded
# by the row's diverge_baseline. Any other row differing, or a transient that never
# re-converges, is a hard failure.
#
# Usage:
#   scripts/check_psx_oracle.sh [--game SLUG] [trace-path] [-- <extra openpete flags>]
#
# --game SLUG (default spyro-1) sources games/SLUG/oracle.conf: the golden's
# path, the disc, the openpete binary and the flags the run needs. The
# golden is derived from the disc and the pinned DuckStation fork and is
# not committed: when it is missing the script captures it first
# (scripts/capture_psx_oracle_ds.sh --game SLUG, ~15 min for 20k records)
# and exits 64 when the fork binary is not built (scripts/setup_duckstation.sh).
#
# Anything after `--` is forwarded verbatim to the openpete binary, so the
# same golden can be used to prove a feature with a game-side producer is
# gameplay-neutral — run openpete WITH the feature flag against the STOCK
# golden; a PASS means the flag perturbed no gameplay-state region vs real
# hardware:
#   scripts/check_psx_oracle.sh -- --no-lod
#   scripts/check_psx_oracle.sh -- --renderer psycross
#   scripts/check_psx_oracle.sh custom.bin -- --no-lod
# A native-renderer display flag (--aspect, --fov, --ssaa) has no game-side
# producer: the run hashes the stock gameplay stream whatever its value and
# proves nothing about the flag; the windowed oracle row covers that class.
# (Flags with values need the `--` separator so the value isn't mistaken
#  for the trace path.)
#
# Defaults (from games/SLUG/oracle.conf):
#   trace-path: tests/golden/spyro-1/psxtrace-demo-20k.bin (20k-tick
#               attract-demo soak — 4+ level loads + gameplay)
#   binary:     build/linux-release/games/spyro-1/openpete-spyro1
#   disc:       env $OPENPETE_DISC or the recipe's
#
# Sibling artifacts (auto-detected — produced by pcsx-redux-trace.lua):
#   <trace>.seed.ram   passed to openpete via --seed-ram (RAM at trace t0)
#   <trace>.seed.cpu   passed via --seed-cpu (528B 'CSv1' CPU state at t0)
#   <trace>.bk2        passed via --bk2 so openpete replays the same input
#                      pcsx-redux-bk2-replay.lua drove on the oracle side
#
# Env knobs:
#   OPENPETE_BIN, OPENPETE_DISC
#   OPENPETE_ORACLE_WINDOWED=1 windowed row: native renderer in a real window
#                         under llvmpipe, eager present thread; FAIL unless
#                         the present thread presented (needs a display and
#                         the lvp ICD, else SKIP); pair with OPENPETE_FRAME_LIMIT
#                         since the run paces to the display
#   OPENPETE_FRAME_LIMIT (default 200000 — safety belt; psxtrace-eof exits
#                         first; a smaller value makes a smoke run that
#                         cannot PASS as a full run)
#   OPENPETE_ORACLE_RECORDS (default 20000) — the record count the golden
#                         must carry and the run must consume for a PASS
#   PSXTRACE_KEEP_GOING=1   diff every frame, don't stop on first mismatch
#   OPENPETE_ORACLE_ALLOW_KNOBS=<reason> admits OPENPETE_PSXTRACE_HEAL_WINDOW
#                         and OPENPETE_PSXTRACE_VBL_POLICY from the caller's
#                         environment; otherwise the run refuses them, since
#                         either weakens or changes the verdict
#
# Exit codes:
#   0   bit-identical hashes for every traced frame
#   2   divergence (openpete ran fine but a hash mismatched)
#   64  prerequisites missing (trace / binary / disc) — ctest treats as SKIP
#   *   build/runtime error
set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."

# Split args at `--`: an optional [trace-path] before, openpete pass-through
# flags after. Lets `check_psx_oracle.sh -- --no-lod` test a feature flag
# against the default golden without naming the trace.
PASSTHRU=()
TRACE=""
GAME="spyro-1"
_sep=0
_game_next=0
for _a in "$@"; do
    if   (( _game_next ));      then GAME="$_a"; _game_next=0
    elif (( _sep ));            then PASSTHRU+=("$_a")
    elif [[ "$_a" == "--" ]];   then _sep=1
    elif [[ "$_a" == "--game" ]]; then _game_next=1
    elif [[ -z "$TRACE" ]];     then TRACE="$_a"
    else                             PASSTHRU+=("$_a")
    fi
done
CONF="games/$GAME/oracle.conf"
[[ -f "$CONF" ]] || { echo "[psx-oracle] no recipe: $CONF" >&2; exit 64; }
GOLDEN=""; FRAMES=""; DISC=""; RUN_ARGS=(); MODE_RT_CLEARED_MIN=""; READS_RT_CLEAR_MAX=""
# shellcheck disable=SC1090
source "$CONF"
TRACE="${TRACE:-$GOLDEN}"
BIN="${OPENPETE_BIN:-build/linux-release/games/$GAME/openpete-${GAME//-/}}"
DISC="${OPENPETE_DISC:-$DISC}"

# The golden is generated, not committed: a missing default golden is
# captured from the pinned fork here, so a fresh checkout's first gate run
# produces its own reference. An explicit trace path is never captured.
if [[ ! -f "$TRACE" ]]; then
    if [[ "$TRACE" != "$GOLDEN" ]]; then
        echo "[psx-oracle] trace not found: $TRACE" >&2
        exit 64
    fi
    if [[ ! -x third_party/duckstation/build-regtest/bin/duckstation-regtest ]]; then
        echo "[psx-oracle] golden $TRACE missing and the DuckStation fork is not built" >&2
        echo "[psx-oracle] run scripts/setup_duckstation.sh, then rerun (the golden is captured on the first run)" >&2
        exit 64
    fi
    echo "[psx-oracle] golden $TRACE missing: capturing it (scripts/capture_psx_oracle_ds.sh --game $GAME)"
    scripts/capture_psx_oracle_ds.sh --game "$GAME" || {
        echo "[psx-oracle] capture failed" >&2
        exit 64
    }
fi
if [[ ! -x "$BIN" ]]; then
    echo "[psx-oracle] binary missing: $BIN — run cmake --build --preset linux-release" >&2
    exit 64
fi
if [[ ! -f "$DISC" ]]; then
    echo "[psx-oracle] disc not found: $DISC (override with OPENPETE_DISC=)" >&2
    exit 64
fi

# The golden names the fork build that captured it; a golden from a commit
# beside the pinned submodule pointer is evidence no rebuild reproduces.
# OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason> is the declared escape.
DS_PIN="$(git ls-files -s third_party/duckstation | awk '{print $2}')"
TRACE_BUILD="$(python3 -m tools.psxtrace_format info "$TRACE" | sed -n 's/^scm_hash=//p')"
if [[ -z "$TRACE_BUILD" ]]; then
    echo "[psx-oracle] golden $TRACE carries no capturing-build hash; recapture" >&2
    exit 64
fi
if [[ "$TRACE_BUILD" != "$DS_PIN" ]]; then
    if [[ -n "${OPENPETE_ORACLE_ALLOW_FORK_DRIFT:-}" ]]; then
        echo "[psx-oracle] WARNING: golden captured by fork $TRACE_BUILD, pinned $DS_PIN;" \
             "allowed: $OPENPETE_ORACLE_ALLOW_FORK_DRIFT"
    else
        echo "[psx-oracle] golden captured by fork build $TRACE_BUILD but the submodule pins $DS_PIN" >&2
        echo "[psx-oracle] recapture from the pin, or set OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason>" >&2
        exit 64
    fi
fi

# The golden must be the full run: its header record count equals the
# expected count, and the verdict below is a PASS only when the run consumed
# every record. A truncated capture is self-consistent (the capture module
# patches the header to the records written) and would otherwise pass.
RECORDS="${OPENPETE_ORACLE_RECORDS:-${FRAMES:-20000}}"
TRACE_RECORDS="$(python3 -m tools.psxtrace_format info "$TRACE" | sed -n 's/^version=.* frames=\([0-9]*\) .*/\1/p')"
if [[ "$TRACE_RECORDS" != "$RECORDS" ]]; then
    echo "[psx-oracle] golden $TRACE holds ${TRACE_RECORDS:-?} records, expected $RECORDS (OPENPETE_ORACLE_RECORDS)" >&2
    exit 64
fi

# Verifier knobs that change the verdict are not inherited from the shell.
for _k in OPENPETE_PSXTRACE_HEAL_WINDOW OPENPETE_PSXTRACE_VBL_POLICY; do
    if [[ -n "${!_k:-}" ]]; then
        if [[ -n "${OPENPETE_ORACLE_ALLOW_KNOBS:-}" ]]; then
            echo "[psx-oracle] WARNING: $_k=${!_k} from the environment; allowed: $OPENPETE_ORACLE_ALLOW_KNOBS"
        else
            echo "[psx-oracle] $_k is set in the environment; unset it or set OPENPETE_ORACLE_ALLOW_KNOBS=<reason>" >&2
            exit 64
        fi
    fi
done

EXTRA=()
[[ "${PSXTRACE_KEEP_GOING:-0}" != "0" ]] && EXTRA+=(--psxtrace-keep-going)

# Auto-detect sibling artifacts so a `${TRACE}.seed.{ram,cpu}` + `${TRACE}.bk2`
# triple cleanly aligns openpete's start to the oracle's.
SEED_RAM="${TRACE}.seed.ram"
SEED_CPU="${TRACE}.seed.cpu"
BK2="${TRACE}.bk2"

[[ -f "$SEED_RAM" ]] && EXTRA+=(--seed-ram "$SEED_RAM") && \
    echo "[psx-oracle] seed RAM: $SEED_RAM"
[[ -f "$SEED_CPU" ]] && EXTRA+=(--seed-cpu "$SEED_CPU") && \
    echo "[psx-oracle] seed CPU: $SEED_CPU"
[[ -f "$BK2"      ]] && EXTRA+=(--bk2 "$BK2") && \
    echo "[psx-oracle] bk2:      $BK2"
# The reference's schedule: openpete consumes these beside the golden and
# runs under the reference's VBL, read and seed timing (psxtrace_schedule.c).
for _sc in vsync loads srand cdctl cdirq padcmd cdsync sio0 watch; do
    [[ -f "$TRACE.$_sc" ]] && echo "[psx-oracle] schedule: $TRACE.$_sc"
done

# The recipe's flags first: a caller's pass-through flag overrides them (the
# options parser takes the last occurrence).
PASSTHRU=("${RUN_ARGS[@]}" "${PASSTHRU[@]}")
[[ ${#PASSTHRU[@]} -gt 0 ]] && \
    echo "[psx-oracle] openpete flags: ${PASSTHRU[*]}"

# Safety belt only — psxtrace-eof exits first. The 20k-tick demo reaches
# EOF at ~37k openpete frames, so the default must clear that comfortably.
LIMIT="${OPENPETE_FRAME_LIMIT:-200000}"
# Full console log, captured so the exit-time music summary can be checked;
# OPENPETE_ORACLE_LOG keeps it at a path of your choosing, else it is deleted.
if [[ -n "${OPENPETE_ORACLE_LOG:-}" ]]; then
    LOG="$OPENPETE_ORACLE_LOG"
else
    LOG="${TMPDIR:-/tmp}/psx-oracle.$$.log"
    trap 'rm -f "$LOG"' EXIT
fi
rm -f "$LOG"

# OPENPETE_ORACLE_WINDOWED=1: the windowed row. Headless never starts the
# present thread, native extraction's live path or eager production, so a
# write that path could make into a hashed row is unreachable by the
# headless gate. The windowed run draws with the native renderer under
# software Vulkan (llvmpipe) in a real window at the display's cadence,
# and its verdict counts only when the present thread presented.
MODE=(--headless)
WINDOWED="${OPENPETE_ORACLE_WINDOWED:-0}"
if [[ "$WINDOWED" != "0" ]]; then
    if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo "[psx-oracle] windowed row needs a display (DISPLAY/WAYLAND_DISPLAY unset)" >&2
        exit 64
    fi
    LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if [[ ! -e "$LVP_ICD" ]]; then
        echo "[psx-oracle] windowed row needs the llvmpipe Vulkan driver ($LVP_ICD)" >&2
        exit 64
    fi
    export VK_ICD_FILENAMES="$LVP_ICD"
    export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
    MODE=(--renderer vk --window 640x480)
    echo "[psx-oracle] windowed: native renderer under llvmpipe, present thread active"
fi

set +e
# --mute 1 sits before PASSTHRU so a forwarded `-- --mute 0` still wins
# (the options parser takes the last occurrence).
# --no-config: the gate's verdict must not depend on whatever openpete.toml
# happens to sit next to the exe or in the runner's home directory.
# --mods "": no mod runs, so no custom-level song can reach the music route
# and the run's XA path is the stock one the golden was captured against.
# A forwarded --mods-dir is a mod run by intent (the modtest harness and
# check_mods.sh gate mods against the oracle): the fixture's enabled.txt
# then selects the mods, as it does outside the gate.
MODS_OFF=(--mods "")
for _a in "${PASSTHRU[@]}"; do
    [[ $_a == --mods-dir || $_a == --mods-dir=* ]] && MODS_OFF=()
done
"$BIN" "${MODE[@]}" \
    --no-config \
    "${MODS_OFF[@]}" \
    --disc "$DISC" \
    --mute 1 \
    --psxtrace-verify "$TRACE" \
    --frame-limit "$LIMIT" \
    "${EXTRA[@]}" \
    "${PASSTHRU[@]}" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}

# The run must have entered through the EXE header's entry (the crt0); an
# explicit --run-recomp at main leaves the stack 8 bytes lower than hardware.
HDR_PC=$(grep -o 'PSX EXE: pc=0x[0-9A-Fa-f]*' "$LOG" | head -1 | grep -o '0x.*')
DISP_PC=$(grep -o 'game fiber: dispatching 0x[0-9A-Fa-f]*' "$LOG" | head -1 | grep -o '0x.*')
if [[ -z "$HDR_PC" || "$HDR_PC" != "$DISP_PC" ]]; then
    echo "[psx-oracle] FAIL: dispatched entry ${DISP_PC:-none} is not the EXE entry ${HDR_PC:-none}" >&2
    exit 2
fi
set -e
echo "[psx-oracle] exit code=$rc"

# Windowed engagement: the present thread must have started and
# presented; a run whose scenes never left the sim thread verified the
# headless path under another name.
if [[ "$WINDOWED" != "0" && "$rc" == "0" ]]; then
    PRESENTED=$(sed -n 's/.*present-thread: \([0-9]*\) scene(s) presented.*/\1/p' "$LOG" | tail -1)
    if ! grep -q 'present-thread: started' "$LOG" || [[ "${PRESENTED:-0}" == "0" ]]; then
        echo "[psx-oracle] windowed: present thread never presented (started=$(grep -c 'present-thread: started' "$LOG"), presented=${PRESENTED:-0}); VACUOUS" >&2
        rc=2
    else
        echo "[psx-oracle] windowed: $PRESENTED scene(s) presented by the present thread"
    fi
fi

# Music routing engagement: the shared CD body prints one summary
# line at exit. The loader's data-mode CdlSetmode clears RT after a stream
# start (mode_rt_cleared proves the mirrored mode byte is written), and no
# CdlReadS is issued with RT clear (reads_rt_clear proves every restart
# streams). The bounds are the recipe's MODE_RT_CLEARED_MIN and
# READS_RT_CLEAR_MAX, measured on the game's golden and applied to a run
# that reached the trace's end; a recipe without them fails with the
# measured values printed so they can be recorded. A missing line means
# the body never ran.
if [[ "$rc" == "0" ]]; then
    SUMMARY=$(grep -o 'music: summary: .*' "$LOG" | tail -1)
    if [[ -z "$SUMMARY" ]]; then
        echo "[psx-oracle] music routing: no summary line (body never engaged)" >&2
        rc=2
    else
        echo "[psx-oracle] $SUMMARY"
        CLEARED=$(sed -n 's/.*mode_rt_cleared=\([0-9]*\).*/\1/p' <<<"$SUMMARY")
        RTCLEAR=$(sed -n 's/.*reads_rt_clear=\([0-9]*\).*/\1/p' <<<"$SUMMARY")
        if ! grep -q 'openpete-exit: reason=psxtrace-eof' "$LOG"; then
            :   # a truncated run (OPENPETE_FRAME_LIMIT) proves engagement only
        elif [[ -z "${MODE_RT_CLEARED_MIN:-}" || -z "${READS_RT_CLEAR_MAX:-}" ]]; then
            echo "[psx-oracle] music routing: $CONF lacks MODE_RT_CLEARED_MIN / READS_RT_CLEAR_MAX;" \
                 "measured mode_rt_cleared=$CLEARED reads_rt_clear=$RTCLEAR" >&2
            rc=2
        elif (( ${CLEARED:-0} < MODE_RT_CLEARED_MIN || ${RTCLEAR:-0} > READS_RT_CLEAR_MAX )); then
            echo "[psx-oracle] music routing: mode_rt_cleared=$CLEARED reads_rt_clear=$RTCLEAR" \
                 "(expected >= $MODE_RT_CLEARED_MIN and <= $READS_RT_CLEAR_MAX)" >&2
            rc=2
        fi
    fi
fi

# Region engagement: a region whose oracle hash never changed over
# the run was compared on a constant. The verifier names them; a row may be
# constant only with `constant_ok = true` and its reason on the manifest row.
# Runs that reach EOF only: a truncated smoke run leaves late-changing rows
# constant for no reason of their own.
if [[ "$rc" == "0" ]] && grep -q 'openpete-exit: reason=psxtrace-eof' "$LOG"; then
    CONSUMED=$(sed -n 's/.*psxtrace: summary: records=\([0-9]*\) .*/\1/p' "$LOG" | tail -1)
    if [[ "$CONSUMED" != "$RECORDS" ]]; then
        echo "[psx-oracle] run consumed ${CONSUMED:-?} records, expected $RECORDS" >&2
        rc=2
    fi
    # A golden with a schedule must have been consumed to its end: rows
    # left over, or a read count short of the sidecar's, is a desync.
    if [[ -f "$TRACE.vsync" ]]; then
        SCHED=$(sed -n 's/.*psxtrace: schedule: consumed //p' "$LOG" | tail -1)
        if [[ -z "$SCHED" ]]; then
            echo "[psx-oracle] schedule: no consumption summary" >&2
            rc=2
        elif [[ "$SCHED" == *FAILED* ]]; then
            echo "[psx-oracle] schedule: $SCHED" >&2
            rc=2
        else
            echo "[psx-oracle] schedule: consumed $SCHED"
        fi
    fi
    UNJUST=$(sed -n 's/.*zero-change regions unjustified: *//p' "$LOG" | tail -1)
    if [[ -z "$UNJUST" ]]; then
        echo "[psx-oracle] region engagement: no zero-change summary line" >&2
        rc=2
    elif [[ "$UNJUST" != "none" ]]; then
        echo "[psx-oracle] region engagement: constant over the run without constant_ok: $UNJUST" >&2
        rc=2
    fi
fi

# Engaged form: with OPENPETE_IDENT_AUDIT=<VBL> set, the native
# feeders compose every canonical present surfaceless and print one
# `[ident-audit ..]` row per present. A PASS with zero rows never ran the
# presentation diff the run was meant to clear.
if [[ "$rc" == "0" && -n "${OPENPETE_IDENT_AUDIT:-}" ]]; then
    ROWS=$(grep -c '^\[ident-audit ' "$LOG" || true)
    if [[ "${ROWS:-0}" == "0" ]]; then
        echo "[psx-oracle] ident-audit: 0 rows (native never composed; VACUOUS)" >&2
        rc=2
    else
        echo "[psx-oracle] ident-audit: $ROWS presents composed"
    fi
fi
exit "$rc"
