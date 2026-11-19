#!/usr/bin/env bash

# tests/harness/check_psx_oracle.sh - run openpete --psxtrace-verify against a
# DuckStation (real SCPH1001 BIOS) baseline trace. The trace's per-tick
# oracle hashes (one per gameplay-state region) are value-equality
# compared against openpete's live hashes; first mismatch = first leak.
#
# Rows flagged cd_load_transient in the manifest (the moby pools and the
# main-image state the level load fills or advances while sectors arrive)
# legitimately diverge for a load's duration (openpete lands a read
# instantly; the reference lands it over the load's VBLs) and re-converge;
# the verifier tolerates a divergence there that heals within the window
# when it opens inside a load span (a CdRead the reference issued at the
# record, or a load burst still open); one opening outside is a gameplay
# burst, held to the row's ratchet table like GTE and VBL events.
# Rows flagged gameplay_neutral or host_precision are hashed and logged and
# never fail on a divergence; their per-record divergence count is bounded
# by the row's diverge_baseline. Any other row differing, or a transient that never
# re-converges, is a hard failure.
#
# Usage:
#   tests/harness/check_psx_oracle.sh [--game SLUG [--recipe PATH]] [trace-path] [-- <extra openpete flags>]
#
# --game SLUG (default spyro-1) sources games/SLUG/oracle.conf: the golden's
# path, the disc, the openpete binary and the flags the run needs. The
# golden is derived from the disc and the patched DuckStation build and is
# not committed: when it is missing the script captures it first
# (scripts/capture_psx_oracle_ds.sh --game SLUG, ~15 min for 20k records)
# and exits 64 when DuckStation is not built (scripts/setup_duckstation.sh).
# --recipe PATH sources another recipe of the game in place of its
# oracle.conf (games/spyro-3/oracle-card.conf: the card-present golden),
# and the missing-golden capture uses the same recipe. A recipe that sets
# CARD1_FIXTURE (the slot 1 card image its capture exported) runs openpete
# with a scratch copy of that image as --card1, so the fixture itself is
# never written; a missing fixture beside a present golden exits 64, unless
# the recipe defines `card1_fixture_make DST`, which writes it first. A
# recipe that also sets CARD1_END (the slot 1 card the capture exported
# after its last record) fails the run unless the card openpete left is
# byte-identical to it; OPENPETE_ORACLE_CARD_OUT=PATH keeps openpete's card.
# A recipe whose reference imports CARD1_IMPORT, the card another recipe's
# capture left (CARD1_SOURCE_RECIPE, e.g. games/spyro-3/oracle-card-save.conf),
# has that recipe captured first when the card is missing.
#
# Anything after `--` is forwarded verbatim to the openpete binary, so the
# same golden can be used to prove a feature with a game-side producer is
# gameplay-neutral - run openpete WITH the feature flag against the STOCK
# golden; a PASS means the flag perturbed no gameplay-state region vs real
# hardware:
#   tests/harness/check_psx_oracle.sh -- --no-lod
#   tests/harness/check_psx_oracle.sh -- --renderer psycross
#   tests/harness/check_psx_oracle.sh custom.bin -- --no-lod
# Every run feeds the native renderer: OPENPETE_HEADLESS_NATIVE=1 brings it
# up surfaceless, so the game-side scene feeders and the per-VBL headless
# device services run every tick as under a window, and a native-path flag
# (--aspect, --fov, --lod, --ssaa) reaches the feed it configures. Nothing is
# composed. The run must print a `native-feed:` exit row with fed > 0, else
# the verdict is VACUOUS (exit 3); a forwarded `--renderer psycross` runs no
# native renderer and is exempt. With OPENPETE_TERRAIN_CACHE_FORCE set to a
# value other than 0 the run must also print a `tercache:` exit row with
# bakes > 0, else VACUOUS (exit 3): the forced leg never baked a sector into
# the terrain cache (llvmpipe refuses the cache without FORCE, and
# `--terrain-cache 0` turns it off). The sub-tick re-extract and the live
# compose run only under a window: the windowed row covers them.
# (Flags with values need the `--` separator so the value isn't mistaken
#  for the trace path.)
#
# Defaults (from games/SLUG/oracle.conf):
#   trace-path: tests/golden/spyro-1/psxtrace-demo-20k.bin (20k-tick
#               attract-demo soak - 4+ level loads + gameplay)
#   binary:     build/linux-release/openpete
#   disc:       env $OPENPETE_DISC or the recipe's
#
# Windows exe: OPENPETE_BIN=build/windows-mingw/openpete.exe runs the same
# gate on the Windows build from WSL. Every OPENPETE_* variable set when the
# exe starts (the ones this script exports and the caller's) crosses through
# scripts/lib/wslenv.sh, a value that names an existing path translated
# to its Windows spelling; the disc, trace, seed, bk2 and card
# arguments are passed as Windows paths. Flags forwarded after `--` are
# passed verbatim. The exe's CRLF line ends are stripped from the log. The
# windowed row (OPENPETE_ORACLE_WINDOWED) needs the Linux binary.
#
# Sibling artifacts (auto-detected - produced by pcsx-redux-trace.lua):
#   <trace>.seed.ram   passed to openpete via --seed-ram (RAM at trace t0)
#   <trace>.seed.cpu   passed via --seed-cpu (528B 'CSv1' CPU state at t0)
#   <trace>.bk2        passed via --bk2 so openpete replays the same input
#                      pcsx-redux-bk2-replay.lua drove on the oracle side
#
# Env knobs:
#   OPENPETE_BIN, OPENPETE_DISC
#   OPENPETE_ORACLE_WINDOWED=1 windowed row: native renderer in a real window
#                         under llvmpipe, eager present thread; FAIL unless the
#                         present thread presented AND at least one sub-tick
#                         re-extract was presented (needs a display and the lvp
#                         ICD, else SKIP); pair with OPENPETE_FRAME_LIMIT since
#                         the run paces to the display, and with
#                         `-- --render-fps N` above the tick rate so
#                         intermediate presents are planned at all
#   OPENPETE_FRAME_LIMIT (default 200000 - safety belt; psxtrace-eof exits
#                         first; a smaller value makes a smoke run that
#                         cannot PASS as a full run)
#   OPENPETE_ORACLE_RECORDS (default 20000) - the record count the golden
#                         must carry and the run must consume for a PASS
#   PSXTRACE_KEEP_GOING=1   diff every frame, don't stop on first mismatch
#   OPENPETE_ORACLE_GTE_STRICT=1 a GTE hash mismatch fails the run (exit 2);
#                         without it a GTE event fails the run unless the
#                         row's ratchet table lists it (see below)
#   OPENPETE_PSXTRACE_RATCHET_STRICT=1 a listed ratchet entry the complete
#                         run did not produce fails it (exit 2)
#   OPENPETE_ORACLE_ALLOW_KNOBS=<reason> admits OPENPETE_PSXTRACE_HEAL_WINDOW,
#                         OPENPETE_PSXTRACE_VBL_POLICY and
#                         OPENPETE_PSXTRACE_CLOCK_REPLAY from the caller's
#                         environment; otherwise the run refuses them, since
#                         each weakens or changes the verdict
#   OPENPETE_ORACLE_CLOCK=1 the clock row: sets OPENPETE_PSXTRACE_CLOCK_REPLAY=1,
#                         so openpete delivers the reference's in-work
#                         VBLANKs (<trace>.vblpos) at their recorded function
#                         entries and charges the SPU envelope to the
#                         reference's exact clock (<trace>.spuq) and its SPU
#                         register accesses (<trace>.spureg); needs
#                         OPENPETE_ORACLE_GTE_STRICT=1 and the three
#                         sidecars (else 64); zero VBLANKs delivered, zero
#                         libspu calls or zero register accesses charged is
#                         VACUOUS (exit 3)
#
# Exit codes:
#   0   bit-identical hashes for every traced frame
#   2   divergence (openpete ran fine but a hash mismatched)
#   3   the GTE hash row is VACUOUS: the golden has a <trace>.gte sidecar
#       and the run hashed zero GTE instructions or printed no row; or the
#       clock row delivered no in-work VBLANK or charged no libspu call or
#       SPU register access; or a vk run printed no `native-feed:` row or fed
#       no scene; or a run with OPENPETE_TERRAIN_CACHE_FORCE printed no
#       `tercache:` row or one with bakes=0
#   64  prerequisites missing (trace / binary / disc) - ctest treats as SKIP
#   *   build/runtime error
set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0
# A failed thread-role check (openpete_thread_roles.h) aborts the run, so a
# role or seal violation on a gated path fails the gate instead of a log line.
export OPENPETE_THREAD_ROLE_FATAL=1

cd "$(dirname "$0")/../.."
# The player's memory cards are never opened: runs boot on scratch cards.
source scripts/lib/scratch_cards.sh
# A signal to this script stops the game process it started (op_own_run).
source scripts/lib/own_children.sh
# PsyCross's GL and the native renderer's Vulkan run on the CPU rasterizers.
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh
sw_render_env

# The native feed rides every run (see the header); the exit row is checked
# below.
export OPENPETE_HEADLESS_NATIVE=1

# Split args at `--`: an optional [trace-path] before, openpete pass-through
# flags after. Lets `check_psx_oracle.sh -- --no-lod` test a feature flag
# against the default golden without naming the trace.
PASSTHRU=()
TRACE=""
GAME="spyro-1"
_sep=0
_game_next=0
RECIPE=""
_recipe_next=0
for _a in "$@"; do
    if   (( _game_next ));      then GAME="$_a"; _game_next=0
    elif (( _recipe_next ));    then RECIPE="$_a"; _recipe_next=0
    elif (( _sep ));            then PASSTHRU+=("$_a")
    elif [[ "$_a" == "--" ]];   then _sep=1
    elif [[ "$_a" == "--game" ]]; then _game_next=1
    elif [[ "$_a" == "--recipe" ]]; then _recipe_next=1
    elif [[ -z "$TRACE" ]];     then TRACE="$_a"
    else                             PASSTHRU+=("$_a")
    fi
done
CONF="${RECIPE:-games/$GAME/oracle.conf}"
[[ -f "$CONF" ]] || { echo "[psx-oracle] no recipe: $CONF" >&2; exit 64; }
CAPTURE_RECIPE=()
[[ -n "$RECIPE" ]] && CAPTURE_RECIPE=(--recipe "$RECIPE")
GOLDEN=""; FRAMES=""; DISC=""; RUN_ARGS=(); CAPTURE_ARGS=(); MODE_RT_CLEARED_MIN=""; READS_RT_CLEAR_MAX=""; CARD1_FIXTURE=""; CARD1_END=""; CARD1_IMPORT=""; CARD1_SOURCE_RECIPE=""; CONSTANT_REGIONS_REASON=""
declare -A CONSTANT_REGION_REASON=()
# shellcheck disable=SC1090
source "$CONF"
TRACE="${TRACE:-$GOLDEN}"
BIN="${OPENPETE_BIN:-build/linux-release/openpete}"
# Spyro 3: arm the store watch over the code ranges the anti-piracy scans
# hash (games/spyro-3/scan_watch.h). A store there logs `antipiracy: FAIL
# watch:`, which fails the run below; the run must print the watch's exit
# row, so a watch that never armed fails too.
SCAN_WATCH=0
if [[ "$GAME" == "spyro-3" ]]; then
    export OPENPETE_S3_SCAN_WATCH=1
    SCAN_WATCH=1
fi
DISC="${OPENPETE_DISC:-$DISC}"

# A recipe whose card fixture is derived (it defines `card1_fixture_make DST`,
# a deterministic writer) writes the fixture when it is missing, before a
# capture imports it or a run copies it.
if [[ -n "$CARD1_FIXTURE" && ! -f "$CARD1_FIXTURE" ]] && declare -F card1_fixture_make >/dev/null; then
    mkdir -p "$(dirname "$CARD1_FIXTURE")"
    card1_fixture_make "$CARD1_FIXTURE" || {
        echo "[psx-oracle] card fixture $CARD1_FIXTURE: the recipe's card1_fixture_make failed" >&2
        exit 64
    }
    echo "[psx-oracle] card fixture $CARD1_FIXTURE written by the recipe's card1_fixture_make"
fi

# The golden is generated, not committed: a missing default golden is
# captured from the patched DuckStation build here, so a fresh checkout's
# first gate run produces its own reference. An explicit trace path is never captured.
if [[ ! -f "$TRACE" ]]; then
    if [[ "$TRACE" != "$GOLDEN" ]]; then
        echo "[psx-oracle] trace not found: $TRACE" >&2
        exit 64
    fi
    if [[ ! -x third_party/duckstation/build-regtest/bin/duckstation-regtest ]]; then
        echo "[psx-oracle] golden $TRACE missing and DuckStation is not built" >&2
        echo "[psx-oracle] run scripts/setup_duckstation.sh, then rerun (the golden is captured on the first run)" >&2
        exit 64
    fi
    # A reference that boots on a card another recipe's capture left
    # (CARD1_IMPORT, written by CARD1_SOURCE_RECIPE) captures that recipe first.
    if [[ -n "$CARD1_IMPORT" && ! -f "$CARD1_IMPORT" && -n "$CARD1_SOURCE_RECIPE" ]]; then
        echo "[psx-oracle] card $CARD1_IMPORT missing: capturing $CARD1_SOURCE_RECIPE first"
        scripts/capture_psx_oracle_ds.sh --game "$GAME" --recipe "$CARD1_SOURCE_RECIPE" || {
            echo "[psx-oracle] capture of $CARD1_SOURCE_RECIPE failed" >&2
            exit 64
        }
    fi
    echo "[psx-oracle] golden $TRACE missing: capturing it (scripts/capture_psx_oracle_ds.sh --game $GAME ${CAPTURE_RECIPE[*]})"
    scripts/capture_psx_oracle_ds.sh --game "$GAME" "${CAPTURE_RECIPE[@]}" || {
        echo "[psx-oracle] capture failed" >&2
        exit 64
    }
fi
if [[ ! -x "$BIN" ]]; then
    echo "[psx-oracle] binary missing: $BIN - run cmake --build --preset linux-release" >&2
    exit 64
fi
if [[ ! -f "$DISC" ]]; then
    echo "[psx-oracle] disc not found: $DISC (override with OPENPETE_DISC=)" >&2
    exit 64
fi
# A Windows exe reads only the environment WSLENV names and Windows paths.
WIN_EXE=0
if [[ "$BIN" == *.exe ]]; then
    WIN_EXE=1
    if ! command -v wslpath >/dev/null; then
        echo "[psx-oracle] $BIN is a Windows exe and wslpath is missing (not WSL)" >&2
        exit 64
    fi
    if [[ "${OPENPETE_ORACLE_WINDOWED:-0}" != 0 ]]; then
        echo "[psx-oracle] OPENPETE_ORACLE_WINDOWED needs the Linux binary (llvmpipe under Xvfb)" >&2
        exit 64
    fi
    # shellcheck source=scripts/lib/wslenv.sh
    source scripts/lib/wslenv.sh
fi
# bin_path PATH: PATH as the binary reads it (the Windows spelling for an exe).
bin_path() {
    if (( WIN_EXE )); then wslpath -w "$(realpath "$1")"; else printf '%s\n' "$1"; fi
}

# The golden names the DuckStation build that captured it; a golden from a
# commit other than the patch series applied onto the pinned upstream
# commit (the head scripts/setup_duckstation.sh produces and records in
# applied-head.txt) is evidence no rebuild reproduces.
# OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason> is the declared escape.
DS_PIN="$(tr -d ' \n' < third_party/duckstation-patches/applied-head.txt 2>/dev/null || true)"
if [[ ! "$DS_PIN" =~ ^[0-9a-f]{40}$ ]]; then
    echo "[psx-oracle] third_party/duckstation-patches/applied-head.txt holds no commit id" >&2
    exit 64
fi
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
        echo "[psx-oracle] golden captured by DuckStation build $TRACE_BUILD but the applied patch series is $DS_PIN" >&2
        echo "[psx-oracle] recapture (rm $GOLDEN*, rerun after scripts/setup_duckstation.sh), or set OPENPETE_ORACLE_ALLOW_FORK_DRIFT=<reason>" >&2
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
for _k in OPENPETE_PSXTRACE_HEAL_WINDOW OPENPETE_PSXTRACE_VBL_POLICY OPENPETE_PSXTRACE_CLOCK_REPLAY; do
    if [[ -n "${!_k:-}" ]]; then
        if [[ -n "${OPENPETE_ORACLE_ALLOW_KNOBS:-}" ]]; then
            echo "[psx-oracle] WARNING: $_k=${!_k} from the environment; allowed: $OPENPETE_ORACLE_ALLOW_KNOBS"
        else
            echo "[psx-oracle] $_k is set in the environment; unset it or set OPENPETE_ORACLE_ALLOW_KNOBS=<reason>" >&2
            exit 64
        fi
    fi
done

# The clock row: the reference's recorded clock positions replayed, gated
# on the strict GTE hash row.
CLOCK="${OPENPETE_ORACLE_CLOCK:-0}"
if [[ "$CLOCK" != "0" ]]; then
    if [[ "${OPENPETE_ORACLE_GTE_STRICT:-0}" == "0" ]]; then
        echo "[psx-oracle] OPENPETE_ORACLE_CLOCK=1 needs OPENPETE_ORACLE_GTE_STRICT=1" >&2
        exit 64
    fi
    for _sc in vblpos spuq spureg; do
        if [[ ! -f "$TRACE.$_sc" ]]; then
            echo "[psx-oracle] OPENPETE_ORACLE_CLOCK=1: no $TRACE.$_sc (recapture the golden)" >&2
            exit 64
        fi
    done
    export OPENPETE_PSXTRACE_CLOCK_REPLAY=1
    echo "[psx-oracle] clock replay: $TRACE.vblpos $TRACE.spuq $TRACE.spureg"
fi

EXTRA=()
[[ "${PSXTRACE_KEEP_GOING:-0}" != "0" ]] && EXTRA+=(--psxtrace-keep-going)

# Auto-detect sibling artifacts so a `${TRACE}.seed.{ram,cpu}` + `${TRACE}.bk2`
# triple cleanly aligns openpete's start to the oracle's.
SEED_RAM="${TRACE}.seed.ram"
SEED_CPU="${TRACE}.seed.cpu"
BK2="${TRACE}.bk2"

[[ -f "$SEED_RAM" ]] && EXTRA+=(--seed-ram "$(bin_path "$SEED_RAM")") && \
    echo "[psx-oracle] seed RAM: $SEED_RAM"
[[ -f "$SEED_CPU" ]] && EXTRA+=(--seed-cpu "$(bin_path "$SEED_CPU")") && \
    echo "[psx-oracle] seed CPU: $SEED_CPU"
[[ -f "$BK2"      ]] && EXTRA+=(--bk2 "$(bin_path "$BK2")") && \
    echo "[psx-oracle] bk2:      $BK2"
# Every sidecar the recipe's capture flags record must sit beside the
# golden: a golden copied without one replays without that part of the
# reference's schedule and diverges in a way that reads as an engine defect.
declare -A _SC_OF=([--cd-read-pc]=loads [--vsync-pc]=vsync [--srand-pc]=srand [--watch-word]=watch
    [--cd-irq]=cdirq [--pad-cmds]=padcmd [--cdsync-pc]=cdsync [--sio0-word]=sio0
    [--card-active]=card [--rcnt2-pc]=rcnt2 [--vblpos]=vblpos [--spuq]=spuq [--spureg]=spureg)
_missing=()
for _a in "${CAPTURE_ARGS[@]}"; do
    _sc="${_SC_OF[$_a]:-}"
    [[ -n "$_sc" && ! -f "$TRACE.$_sc" && " ${_missing[*]} " != *" $TRACE.$_sc "* ]] && _missing+=("$TRACE.$_sc")
done
if (( ${#_missing[@]} )); then
    echo "[psx-oracle] golden $TRACE: sidecars its recipe $CONF records are missing: ${_missing[*]}" >&2
    echo "[psx-oracle] copy them with the golden, or recapture (delete the golden and its sidecars, rerun)" >&2
    exit 64
fi
# The reference's schedule: openpete consumes these beside the golden and
# runs under the reference's VBL, read and seed timing (psxtrace_schedule.c).
for _sc in vsync loads srand cdctl cdirq padcmd cdsync sio0 card watch; do
    [[ -f "$TRACE.$_sc" ]] && echo "[psx-oracle] schedule: $TRACE.$_sc"
done
# The GTE hash row: whenever the golden carries a <trace>.gte sidecar the
# run folds every gte_execute call per record interval and the verifier
# compares it with the reference's row (core/diag/psxtrace_verify.c).
GTE_ROW=0
if [[ -f "$TRACE.gte" ]]; then
    GTE_ROW=1
    export OPENPETE_GTE_HASH=1
    echo "[psx-oracle] gte hash: $TRACE.gte"
fi

# The event ratchet: the stock row may produce only the GTE events, VBL
# events and gameplay bursts (cd_load_transient bursts opening outside a
# load span) its table in games/GAME/manifest/oracle_ratchet.toml lists,
# keyed by the golden's file name without `.bin`; the verifier fails the
# run on any other and reports listed entries the run did not produce
# (stale; a failure under OPENPETE_PSXTRACE_RATCHET_STRICT=1). The clock
# row, a game without the file, and a golden without a table hold every
# list at zero. tools/oracle_ratchet.py tightens a table from a run's log.
RATCHET_FILE="games/$GAME/manifest/oracle_ratchet.toml"
if [[ "$CLOCK" != "0" || ! -f "$RATCHET_FILE" ]]; then
    export OPENPETE_PSXTRACE_RATCHET=strict
else
    export OPENPETE_PSXTRACE_RATCHET="$RATCHET_FILE"
fi
OPENPETE_PSXTRACE_RATCHET_KEY="$(basename "$TRACE" .bin)"
export OPENPETE_PSXTRACE_RATCHET_KEY
echo "[psx-oracle] ratchet: $OPENPETE_PSXTRACE_RATCHET [$OPENPETE_PSXTRACE_RATCHET_KEY]"

# A card-present recipe: openpete runs on a scratch copy of the card the
# reference booted with, so a write the game makes never reaches the fixture
# and every run starts from the captured image.
CARD_DIR=""
if [[ -n "$CARD1_FIXTURE" ]]; then
    # -L: a worktree links its fixtures; the size is the target's.
    if [[ "$(stat -L -c %s "$CARD1_FIXTURE" 2>/dev/null || echo 0)" != 131072 ]]; then
        echo "[psx-oracle] card fixture $CARD1_FIXTURE is missing or not 131072 bytes; recapture (rm $GOLDEN*)" >&2
        exit 64
    fi
    CARD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/psx-oracle-card.XXXXXX")"
    cp "$CARD1_FIXTURE" "$CARD_DIR/card1.mcr"
    RUN_ARGS+=(--card1 "$(bin_path "$CARD_DIR/card1.mcr")")
    echo "[psx-oracle] card fixture: $CARD1_FIXTURE (slot 1, scratch copy) sha256 $(sha256sum < "$CARD_DIR/card1.mcr" | cut -c1-64)"
fi

# The recipe's flags first: a caller's pass-through flag overrides them (the
# options parser takes the last occurrence).
PASSTHRU=("${RUN_ARGS[@]}" "${PASSTHRU[@]}")
[[ ${#PASSTHRU[@]} -gt 0 ]] && \
    echo "[psx-oracle] openpete flags: ${PASSTHRU[*]}"

# Safety belt only - psxtrace-eof exits first. The 20k-tick demo reaches
# EOF at ~37k openpete frames, so the default must clear that comfortably.
LIMIT="${OPENPETE_FRAME_LIMIT:-200000}"
# Full console log, captured so the exit-time music summary can be checked;
# OPENPETE_ORACLE_LOG keeps it at a path of your choosing, else it is deleted.
if [[ -n "${OPENPETE_ORACLE_LOG:-}" ]]; then
    LOG="$OPENPETE_ORACLE_LOG"
else
    LOG="${TMPDIR:-/tmp}/psx-oracle.$$.log"
fi
# The temporary log and the card scratch copy go with the run.
_oracle_cleanup() {
    [[ -n "${OPENPETE_ORACLE_LOG:-}" ]] || rm -f "${LOG:?}"
    [[ -z "$CARD_DIR" ]] || rm -rf "${CARD_DIR:?}"
    op_scratch_cards_cleanup
}
trap _oracle_cleanup EXIT
rm -f "${LOG:?}"

# OPENPETE_ORACLE_WINDOWED=1: the windowed row. Headless never starts the
# present thread, native extraction's live path or eager production, so a
# write that path could make into a hashed row is unreachable by the
# headless gate. The windowed run draws with the native renderer under
# software Vulkan (llvmpipe) in a real window at the display's cadence,
# and its verdict counts only when the present thread presented.
MODE=(--headless)
OP_XVFB=()
WINDOWED="${OPENPETE_ORACLE_WINDOWED:-0}"
if [[ "$WINDOWED" != "0" ]]; then
    # The window is on a private Xvfb display (scripts/lib/xvfb.sh).
    op_xvfb_prefix psx-oracle 1280x960
    if [[ "${VK_ICD_FILENAMES:-}" != *lvp_icd* ]]; then
        echo "[psx-oracle] windowed row needs the llvmpipe Vulkan driver (lvp_icd.x86_64.json)" >&2
        exit 64
    fi
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
# A Windows exe gets every OPENPETE_* variable now set, as the Linux child
# inherits them: the sandbox, card directory and thread-role report through
# the helper's own translated entries, any other value naming an existing
# path (a `/` in it) as its Windows path, the rest verbatim. The Mesa selection
# (sw_render_env) stays behind: the exe renders on the Windows GPU.
LAUNCH_ENV=()
if (( WIN_EXE )); then
    _fwd=(); _names=()
    while IFS= read -r _k; do
        case "$_k" in
            OPENPETE_SANDBOX|OPENPETE_CARD_DIR|OPENPETE_THREAD_ROLE_REPORT|OPENPETE_THREAD_ROLE_FATAL|OPENPETE_CARD_GUARD)
                _names+=("$_k"); continue ;;
        esac
        _v="${!_k}"
        [[ "$_v" == */* && -e "$_v" ]] && _v="$(bin_path "$_v")"
        _fwd+=("$_k=$_v"); _names+=("$_k")
    done < <(compgen -e | grep '^OPENPETE_' | sort)
    wslenv_forward "${_fwd[@]}"
    LAUNCH_ENV=(env "${WSLENV_ARGS[@]}")
    # Engagement: a Windows process started with the same environment must
    # read every name; one that does not cross makes the run VACUOUS.
    _seen="$(cd /mnt/c 2>/dev/null; "${LAUNCH_ENV[@]}" cmd.exe /d /c set OPENPETE_ 2>/dev/null | tr -d '\r' || true)"
    _lost=()
    for _k in "${_names[@]}"; do
        grep -q "^$_k=" <<<"$_seen" || _lost+=("$_k")
    done
    if (( ${#_lost[@]} )); then
        echo "[psx-oracle] windows exe: did not cross WSLENV: ${_lost[*]}; VACUOUS" >&2
        exit 3
    fi
    mapfile -t _recv < <(grep -E "^($(IFS='|'; echo "${_names[*]}"))=" <<<"$_seen")
    echo "[psx-oracle] windows exe: received ${_recv[*]}"
fi
# The run and its tee are one background job the script waits on, so a
# TERM, INT or HUP to the script stops them (scripts/lib/own_children.sh);
# pipefail makes the job's status the binary's. The CR of a Windows exe's
# CRLF line ends is dropped, so every log match below reads the same text.
# --input-lock 1 sits before PASSTHRU: a connected pad never reaches the
# guest pad buffer, and a forwarded `-- --input-lock 0` still wins.
op_own_run bash -c 'set -o pipefail; log="$1"; shift; "$@" 2>&1 | tr -d "\r" | tee "$log"' _ "$LOG" \
    "${OP_XVFB[@]}" "${LAUNCH_ENV[@]}" "$BIN" "${MODE[@]}" \
    --no-config \
    "${MODS_OFF[@]}" \
    --disc "$(bin_path "$DISC")" --disc-cache 0 \
    --mute 1 --input-lock 1 \
    --psxtrace-verify "$(bin_path "$TRACE")" \
    --frame-limit "$LIMIT" \
    "${EXTRA[@]}" \
    "${PASSTHRU[@]}"
rc=$?

# The run must have entered through the EXE header's entry (the crt0); an
# explicit OPENPETE_RUN_RECOMP at main leaves the stack 8 bytes lower than hardware.
HDR_PC=$(grep -o 'PSX EXE: pc=0x[0-9A-Fa-f]*' "$LOG" | head -1 | grep -o '0x.*')
DISP_PC=$(grep -o 'game fiber: dispatching 0x[0-9A-Fa-f]*' "$LOG" | head -1 | grep -o '0x.*')
if [[ -z "$HDR_PC" || "$HDR_PC" != "$DISP_PC" ]]; then
    echo "[psx-oracle] FAIL: dispatched entry ${DISP_PC:-none} is not the EXE entry ${HDR_PC:-none}" >&2
    exit 2
fi
set -e
echo "[psx-oracle] exit code=$rc"

# The card openpete left against the card the reference left.
if [[ -n "$CARD_DIR" ]]; then
    [[ -n "${OPENPETE_ORACLE_CARD_OUT:-}" ]] && cp "$CARD_DIR/card1.mcr" "$OPENPETE_ORACLE_CARD_OUT"
    if [[ -n "$CARD1_END" ]]; then
        if [[ ! -f "$CARD1_END" ]]; then
            echo "[psx-oracle] card image: no reference card $CARD1_END; recapture the golden" >&2
            [[ "$rc" == 0 ]] && rc=2
        else
            CARD_DIFF="$( (cmp -l "$CARD_DIR/card1.mcr" "$CARD1_END" 2>/dev/null || true) | wc -l)"
            if [[ "$CARD_DIFF" != 0 ]]; then
                echo "[psx-oracle] card image: $CARD_DIFF byte(s) differ from $CARD1_END" >&2
                [[ "$rc" == 0 ]] && rc=2
            else
                echo "[psx-oracle] card image: identical to $CARD1_END"
            fi
        fi
    fi
fi

# GTE hash row engagement: the row counts only when instructions were hashed
# on compared intervals; zero is VACUOUS (exit 3), not a pass.
if [[ "$GTE_ROW" == 1 ]]; then
    GTE_LINE=$(grep -o 'gte: records=.*' "$LOG" | tail -1 || true)
    GTE_OPS=$(sed -n 's/.* ops_hashed=\([0-9]*\).*/\1/p' <<<"$GTE_LINE")
    GTE_MISM=$(sed -n 's/.* mismatches=\([0-9]*\).*/\1/p' <<<"$GTE_LINE")
    if [[ -z "$GTE_LINE" || "${GTE_OPS:-0}" == 0 ]]; then
        echo "[psx-oracle] gte hash: ${GTE_LINE:-no summary line}; VACUOUS" >&2
        [[ "$rc" == 0 ]] && rc=3
    else
        echo "[psx-oracle] $GTE_LINE"
        if [[ "${OPENPETE_ORACLE_GTE_STRICT:-0}" != 0 && "${GTE_MISM:-0}" != 0 && "$rc" == 0 ]]; then
            echo "[psx-oracle] gte hash: $GTE_MISM mismatching record(s) under OPENPETE_ORACLE_GTE_STRICT" >&2
            rc=2
        fi
    fi
fi

# The ratchet's summary: every run that finalized prints it; the
# verifier itself fails an unlisted event.
RATCHET_LINE=$(grep -o 'psxtrace: ratchet: key=.*' "$LOG" | tail -1 || true)
if [[ -z "$RATCHET_LINE" ]]; then
    echo "[psx-oracle] ratchet: no summary line" >&2
    [[ "$rc" == 0 ]] && rc=2
else
    echo "[psx-oracle] ${RATCHET_LINE#psxtrace: }"
    grep -o 'psxtrace: ratchet: stale .*' "$LOG" | sed 's/^psxtrace: /[psx-oracle] /' || true
fi
VERDICT_LINE=$(grep -o 'psxtrace: verdict: .*' "$LOG" | tail -1 || true)
[[ -n "$VERDICT_LINE" ]] && echo "[psx-oracle] ${VERDICT_LINE#psxtrace: }"

# Clock row engagement: a replay that delivered no in-work VBLANK or
# charged no libspu call or SPU register access ran the stock timing under
# another name.
if [[ "$CLOCK" != "0" ]]; then
    CLOCK_LINE=$(grep -o 'clock: vblpos delivered=.*' "$LOG" | tail -1 || true)
    CLK_DELIV=$(sed -n 's/.*vblpos delivered=\([0-9]*\).*/\1/p' <<<"$CLOCK_LINE")
    CLK_CHARGED=$(sed -n 's/.*spuq charged=\([0-9]*\).*/\1/p' <<<"$CLOCK_LINE")
    CLK_REG=$(sed -n 's/.*spureg charged=\([0-9]*\).*/\1/p' <<<"$CLOCK_LINE")
    if [[ -z "$CLOCK_LINE" || "${CLK_DELIV:-0}" == 0 || "${CLK_CHARGED:-0}" == 0 || "${CLK_REG:-0}" == 0 ]]; then
        echo "[psx-oracle] clock replay: ${CLOCK_LINE:-no summary line}; VACUOUS" >&2
        [[ "$rc" == 0 ]] && rc=3
    else
        echo "[psx-oracle] $CLOCK_LINE"
    fi
fi

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
    # The windowed row exists for the sub-tick leg: a run whose presents were
    # all canonical exercised the tick path at a display cadence and nothing
    # else. The exit line counts the presents built from interpolated
    # gamestate (eager production plus the host loop's inline path); it is
    # printed only when at least one ran, so an absent line is zero.
    REX=$(sed -n 's/.*sub-tick: \([0-9]*\) re-extract(s) presented.*/\1/p' "$LOG" | tail -1)
    if [[ "${REX:-0}" == "0" ]]; then
        echo "[psx-oracle] windowed: no sub-tick re-extract presented; the run verified the canonical path only; VACUOUS" >&2
        rc=2
    else
        echo "[psx-oracle] windowed: $REX sub-tick re-extract(s) presented"
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
    SUMMARY=$(grep -o 'music: summary: .*' "$LOG" | tail -1 || true)
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
# constant only with `constant_ok = true` and its reason on the manifest row,
# by a recipe entry CONSTANT_REGION_REASON[<region>]=<reason> (the run's
# records never reach that region's writers; an entry naming a region the
# run changed fails as stale), or over a whole run whose recipe sets
# CONSTANT_REGIONS_REASON (a capture too short to reach the code that
# changes them).
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
        # Polling-loop quanta under the schedule (reported, not a criterion).
        LOOPQ=$(sed -n 's/.*psxtrace: loop: //p' "$LOG" | tail -1)
        [[ -n "$LOOPQ" ]] && echo "[psx-oracle] loop: $LOOPQ"
    fi
    UNJUST=$(sed -n 's/.*zero-change regions unjustified: *//p' "$LOG" | tail -1)
    if [[ -z "$UNJUST" ]]; then
        echo "[psx-oracle] region engagement: no zero-change summary line" >&2
        rc=2
    elif (( ${#CONSTANT_REGION_REASON[@]} )); then
        # Per-region reasons: every constant region named, every named region constant.
        read -r -a _constant <<<"${UNJUST/#none/}"
        _undeclared=(); _stale=()
        for _r in "${_constant[@]}"; do
            [[ -n "${CONSTANT_REGION_REASON[$_r]+set}" ]] || _undeclared+=("$_r")
        done
        for _r in "${!CONSTANT_REGION_REASON[@]}"; do
            [[ " ${_constant[*]} " == *" $_r "* ]] || _stale+=("$_r")
        done
        for _r in "${_constant[@]}"; do
            if [[ -n "${CONSTANT_REGION_REASON[$_r]+set}" ]]; then
                echo "[psx-oracle] region engagement: $_r constant over the run: ${CONSTANT_REGION_REASON[$_r]}"
            fi
        done
        if (( ${#_undeclared[@]} )); then
            echo "[psx-oracle] region engagement: constant over the run without constant_ok or a recipe reason: ${_undeclared[*]}" >&2
            rc=2
        fi
        if (( ${#_stale[@]} )); then
            echo "[psx-oracle] region engagement: the recipe declares constant, the run changed: ${_stale[*]}" >&2
            rc=2
        fi
    elif [[ "$UNJUST" != "none" && -n "$CONSTANT_REGIONS_REASON" ]]; then
        echo "[psx-oracle] region engagement: constant over the run without constant_ok: $UNJUST;" \
             "accepted by the recipe: $CONSTANT_REGIONS_REASON"
    elif [[ "$UNJUST" != "none" ]]; then
        echo "[psx-oracle] region engagement: constant over the run without constant_ok: $UNJUST" >&2
        rc=2
    fi
fi

# Anti-piracy verdicts: a failed level scan or a store-watch hit logs an
# `antipiracy: FAIL` line (games/spyro-3/features/antipiracy_scan.h); such a
# run fails whatever its hashes say.
if grep -q 'antipiracy: FAIL' "$LOG"; then
    echo "[psx-oracle] anti-piracy: $(grep -c 'antipiracy: FAIL' "$LOG") FAIL line(s):" >&2
    grep 'antipiracy: FAIL' "$LOG" | head -5 | sed 's/^/    /' >&2
    rc=2
fi
if [[ "$SCAN_WATCH" == 1 ]]; then
    WATCH_ROW=$(grep -o '\[s3-scan\] watch: .*' "$LOG" | tail -1 || true)
    if [[ -z "$WATCH_ROW" ]]; then
        echo "[psx-oracle] anti-piracy: OPENPETE_S3_SCAN_WATCH=1 and no watch exit row (the watch never armed)" >&2
        rc=2
    else
        echo "[psx-oracle] $WATCH_ROW"
    fi
fi

# Native feed engagement: the renderer the run selected (the last
# --renderer among the forwarded flags, vk by default) decides whether a
# feed is owed. A vk run whose game render never fed a scene ran the gate
# without the native path under its name.
NATIVE_OWED=1
for (( _i = 0; _i < ${#PASSTHRU[@]}; _i++ )); do
    case "${PASSTHRU[$_i]}" in
        --renderer)   NATIVE_OWED=$([[ "${PASSTHRU[$((_i + 1))]:-}" == psycross ]] && echo 0 || echo 1) ;;
        --renderer=*) NATIVE_OWED=$([[ "${PASSTHRU[$_i]#--renderer=}" == psycross ]] && echo 0 || echo 1) ;;
    esac
done
if [[ "$NATIVE_OWED" == 1 ]]; then
    FEED_ROW=$(grep -o 'native-feed: captures=.*' "$LOG" | tail -1 || true)
    FEED_FED=$(sed -n 's/.* fed=\([0-9]*\).*/\1/p' <<<"$FEED_ROW")
    if [[ -z "$FEED_ROW" || "${FEED_FED:-0}" == 0 ]]; then
        echo "[psx-oracle] native feed: ${FEED_ROW:-no exit row (renderer never up)}; VACUOUS" >&2
        [[ "$rc" == 0 ]] && rc=3
    else
        echo "[psx-oracle] $FEED_ROW"
    fi
else
    echo "[psx-oracle] native feed: none owed (--renderer psycross)"
fi

# Terrain-cache engagement: a leg that forces the cache on (the oracle runs
# on llvmpipe, where the cache is refused without OPENPETE_TERRAIN_CACHE_FORCE)
# must have baked sectors into it, else the leg verified the CPU walk alone.
if [[ "$NATIVE_OWED" == 1 && -n "${OPENPETE_TERRAIN_CACHE_FORCE:-}" &&
      "${OPENPETE_TERRAIN_CACHE_FORCE}" != 0 ]]; then
    TC_ROW=$(grep -o 'tercache: bakes=.*' "$LOG" | tail -1 || true)
    TC_BAKES=$(sed -n 's/^tercache: bakes=\([0-9]*\).*/\1/p' <<<"$TC_ROW")
    if [[ -z "$TC_ROW" || "${TC_BAKES:-0}" == 0 ]]; then
        echo "[psx-oracle] terrain cache: ${TC_ROW:-no tercache: exit row} (OPENPETE_TERRAIN_CACHE_FORCE set); VACUOUS" >&2
        [[ "$rc" == 0 ]] && rc=3
    else
        echo "[psx-oracle] $TC_ROW"
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
