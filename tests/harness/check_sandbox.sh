#!/usr/bin/env bash
# check_sandbox.sh - no game run under OPENPETE_SANDBOX writes outside its
# root.
#
# The per-user locations the game resolves without a sandbox are listed
# (path, mtime, size) before and after every run, and any change fails:
#   decoy   the XDG config, state and data directories, pointed for every
#           run at a scratch tree ($OUT/decoy-home), so a run that ignored
#           the sandbox writes there and never into the developer's own
#   real    the account's own ~/.config/openpete, ~/.local/state/openpete
#           and ~/.local/share/openpete (the passwd home, which the boot
#           guard also treats as real)
#   exe     the leaves the portable layout writes beside the executable
# The decoy listing and the executable's leaves fail on any change. The
# account's own directories also change when another openpete process on the
# machine writes (a concurrent session rotating its log), so there a change
# fails when it is attributed to this check: a changed or new text file that
# names $OUT, which every run's log does (the disc is passed through a link
# under $OUT and the log names it). Other changes there are printed as
# foreign; --strict-real fails on them too, for a machine with no other
# openpete process.
#
# Writer rows (Spyro 1, headless, --renderer vk, each in its own root made
# by scripts/lib/sandbox.sh's op_sandbox_run_root):
#   main    --skip-to-level artisans --bk2-record (the default path),
#           --no-card (the replay row loads its anchor with no card), a
#           --screenshot-on-frame request (the native renderer stays live), a
#           scripted quick-save (key 1), screenshot (F12) and settings change
#           (Escape, FOV +1, Escape: the autosave), and a bug report
#           (OPENPETE_BUGREPORT_AT). Each file must appear under the root:
#           state/openpete/logs/openpete.log, the pipeline cache link,
#           state/openpete/states/spyro-1/*, data/openpete/screenshots/*.png,
#           data/openpete/bugreports/bug-*.zip, data/openpete/bk2/*.bk2,
#           config/openpete/openpete.toml.
#   hang    OPENPETE_HANG_THRESHOLD=2000: the entry watchdog trips (exit 2);
#           state/openpete/logs/hang-trace.bin and hang-ram.bin under the root.
#   replay  --bug-report-replay of the main row's bundle with its scratch
#           directory under $OUT: the replay's own log under
#           <scratch>/sandbox/state/openpete/logs.
# Controls (LAW 13: the instrument is calibrated against known truth):
#   decoy-root  OPENPETE_SANDBOX inside the decoy state directory: exit 66
#               and both listings unchanged.
#   bare        no OPENPETE_SANDBOX: the run writes its log into the decoy
#               state directory and the decoy listing must change, proving
#               the listing sees a write.
#
# --platform=windows runs the main, hang and decoy-root rows with the
# windows-mingw exe (--win-build-dir, default build/windows-mingw), its
# roots on the Windows drive (OPENPETE_SANDBOX_PARENT under
# /mnt/<drive>/.../Temp), the real listing over %APPDATA%\openpete,
# %LOCALAPPDATA%\openpete and the exe directory's leaves, and the variables
# through scripts/lib/wslenv.sh. Windows ignores XDG, so the bare control
# would write the developer's own directories and is not run there.
#
# Usage: tests/harness/check_sandbox.sh [--build-dir=DIR] [--win-build-dir=DIR]
#            [--platform=linux|windows] [--strict-real]
# Outputs under $OUT (default <build-dir>/check-sandbox/<platform>), deleted first.
# Exit 0 PASS, 1 FAIL, 64 SKIP (no binary or no disc).
set -uo pipefail
cd "$(dirname "$0")/../.."

PLATFORM=linux
STRICT_REAL=0
WIN_BUILD=build/windows-mingw
for a in "$@"; do
    case "$a" in
        --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;;
        --win-build-dir=*) WIN_BUILD="${a#--win-build-dir=}" ;;
        --platform=*) PLATFORM="${a#--platform=}" ;;
        --strict-real) STRICT_REAL=1 ;;
        *) echo "[sandbox] unknown argument $a" >&2; exit 64 ;;
    esac
done

export OPENPETE_VIBRATION=0
OUT="${SANDBOX_CHECK_OUT:-${PG_BUILD_DIR:-build/linux-release}/check-sandbox/$PLATFORM}"
rm -rf "${OUT:?}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"

if [[ "$PLATFORM" == windows ]]; then
    command -v wslpath >/dev/null || { echo "[sandbox] SKIP: not WSL"; exit 64; }
    WIN_TEMP="$(wslpath "$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')")"
    [[ -d "$WIN_TEMP" ]] || { echo "[sandbox] SKIP: no Windows %TEMP%"; exit 64; }
    export OPENPETE_SANDBOX_PARENT="$WIN_TEMP/openpete-harness-sandbox"
fi
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh
pg_resolve spyro-1
[[ -f "$PG_DISC" ]] || { echo "[sandbox] SKIP: no disc at $PG_DISC"; exit 64; }

if [[ "$PLATFORM" == windows ]]; then
    BIN="$(win_tree_exe "$WIN_BUILD" spyro-1)"
    DISC_ARG="$(wslpath -w "$PG_DISC")"
    APPDATA_L="$(wslpath "$(cmd.exe /c 'echo %APPDATA%' 2>/dev/null | tr -d '\r')")"
    LOCALAPPDATA_L="$(wslpath "$(cmd.exe /c 'echo %LOCALAPPDATA%' 2>/dev/null | tr -d '\r')")"
    REAL_DIRS=("$APPDATA_L/openpete" "$LOCALAPPDATA_L/openpete")
else
    BIN="$PG_BIN"
    ln -s "$(cd "$(dirname "$PG_DISC")" && pwd -P)" "$OUT/disc"
    DISC_ARG="$OUT/disc/$(basename "$PG_DISC")"
    HOME_PW="$(getent passwd "$(id -un)" | cut -d: -f6)"
    REAL_DIRS=("$HOME_PW/.config/openpete" "$HOME_PW/.local/state/openpete"
               "$HOME_PW/.local/share/openpete")
fi
[[ -x "$BIN" || -f "$BIN" ]] || { echo "[sandbox] SKIP: no binary at $BIN"; exit 64; }
EXE_DIR="$(cd "$(dirname "$BIN")" && pwd -P)"
EXE_LEAVES=()
for leaf in cards states logs cache library modules mods assets bk2 screenshots bugreports \
            openpete.toml openpete-devui.ini; do
    EXE_LEAVES+=("$EXE_DIR/$leaf")
done

DECOY="$OUT/decoy-home"
mkdir -p "$DECOY/config" "$DECOY/state" "$DECOY/data"
DECOY_ENV=("XDG_CONFIG_HOME=$DECOY/config" "XDG_STATE_HOME=$DECOY/state" "XDG_DATA_HOME=$DECOY/data")

# listing FILE DIR...: path, mtime and size of every entry under each DIR.
listing() {
    local f="$1"; shift
    rm -f "${f:?}"
    local d
    for d in "$@"; do
        [[ -e "$d" ]] && find "$d" -printf '%p\t%T@\t%s\n' 2>/dev/null
    done | LC_ALL=C sort >"$f"
}

fail=()
# changed_paths A B: the paths whose row differs between two listings.
changed_paths() {
    diff "$1" "$2" | sed -n 's/^[<>] //p' | cut -f1 | LC_ALL=C sort -u
}

# unchanged TAG: the decoy and exe listings equal their baselines; a change
# in the real listing fails when it names $OUT (or always, --strict-real).
# Each listing is re-based after the check, so a foreign change is counted
# once.
unchanged() {
    local tag="$1" which p
    listing "$OUT/decoy-$tag.tsv" "$DECOY"
    listing "$OUT/exe-$tag.tsv" "${EXE_LEAVES[@]}"
    listing "$OUT/real-$tag.tsv" "${REAL_DIRS[@]}"
    for which in decoy exe; do
        if ! cmp -s "$OUT/$which-base.tsv" "$OUT/$which-$tag.tsv"; then
            fail+=("$tag: the $which listing changed:")
            while IFS= read -r p; do fail+=("    $p"); done < <(
                changed_paths "$OUT/$which-base.tsv" "$OUT/$which-$tag.tsv" | head -20)
        fi
    done
    while IFS= read -r p; do
        if (( STRICT_REAL )) || { [[ -f "$p" ]] && grep -qaF "$OUT" "$p"; }; then
            fail+=("$tag: a real per-user file changed by this check: $p")
        else
            echo "[sandbox] $tag: foreign change in a real per-user directory (another process): $p"
            FOREIGN=$((FOREIGN + 1))
        fi
    done < <(changed_paths "$OUT/real-base.tsv" "$OUT/real-$tag.tsv")
    cp "$OUT/real-$tag.tsv" "$OUT/real-base.tsv"
}
FOREIGN=0

# run TAG ROOT RC_WANT [ENV...] -- [ARGS...]: one headless run under
# OPENPETE_SANDBOX=ROOT (empty: unset); sets RC.
run() {
    local tag="$1" root="$2" want="$3"; shift 3
    local -a env=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do env+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    local -a pre=(env -u OPENPETE_SANDBOX -u OPENPETE_CARD_DIR "${DECOY_ENV[@]}")
    [[ -n "$root" ]] && pre+=("OPENPETE_SANDBOX=$root")
    if [[ "$PLATFORM" == windows ]]; then
        # The root is exported while the forward is built, so WSLENV names
        # it with /p and the exe receives its Windows spelling.
        local saved="${OPENPETE_SANDBOX:-}"
        export OPENPETE_SANDBOX="$root"
        wslenv_forward OPENPETE_VIBRATION=0 "${env[@]}"
        pre=(env -u OPENPETE_CARD_DIR "OPENPETE_SANDBOX=$root" "${WSLENV_ARGS[@]}")
        export OPENPETE_SANDBOX="$saved"
    else
        pre+=("${env[@]}")
    fi
    rm -f "${OUT:?}/$tag.log"
    "${pre[@]}" timeout -k 10 900 "$BIN" --headless --disc "$DISC_ARG" --disc-cache 0 \
        --renderer vk --mute 1 --input-lock "$@" >"$OUT/$tag.log" 2>&1
    RC=$?
    (( RC == want )) || fail+=("$tag: exit $RC, want $want (see $OUT/$tag.log)")
}

# has TAG ROOT GLOB...: each glob under ROOT names a file.
has() {
    local tag="$1" root="$2" g; shift 2
    for g in "$@"; do
        compgen -G "$root/$g" >/dev/null || fail+=("$tag: no $g under the root $root")
    done
}

listing "$OUT/decoy-base.tsv" "$DECOY"
listing "$OUT/exe-base.tsv" "${EXE_LEAVES[@]}"
listing "$OUT/real-base.tsv" "${REAL_DIRS[@]}"
echo "[sandbox] $PLATFORM: $(wc -l <"$OUT/real-base.tsv") real entries over ${#REAL_DIRS[@]} directories, $(wc -l <"$OUT/exe-base.tsv") beside the executable"

# main: every writer a player's session reaches, in one run.
MAIN="$(op_sandbox_run_root)"
{
    echo "1500 keydown 1";      echo "1504 keyup 1"
    echo "1520 keydown F12";    echo "1524 keyup F12"
    echo "1560 keydown Escape"; echo "1564 keyup Escape"
    echo "1580 keydown Down";   echo "1584 keyup Down"
    echo "1588 keydown Down";   echo "1592 keyup Down"
    echo "1596 keydown Down";   echo "1600 keyup Down"
    echo "1604 keydown Right";  echo "1608 keyup Right"
    echo "1640 keydown Escape"; echo "1644 keyup Escape"
} >"$OUT/main-inject.txt"
INJECT="$OUT/main-inject.txt"
# A screenshot request keeps the native renderer live under --headless, so
# the player screenshot (F12) and the pipeline cache are written.
SHOT="$OUT/main-shot-{n}.png"
[[ "$PLATFORM" == windows ]] && SHOT="$(wslpath -w "$OUT")\\main-shot-{n}.png"
[[ "$PLATFORM" == windows ]] && INJECT="$(wslpath -w "$INJECT")"
run main "$MAIN" 0 "OPENPETE_MOUSE_INJECT=$INJECT" OPENPETE_BUGREPORT_AT=1700 \
    OPENPETE_BUGREPORT_NOTE=0 -- --skip-to-level artisans --bk2-record --no-card \
    --screenshot-on-frame 1740 --screenshot-output "$SHOT" --screenshot-native-only --frame-limit 1760
has main "$MAIN" state/openpete/logs/openpete.log "state/openpete/states/spyro-1/*" \
    "data/openpete/screenshots/*.png" "data/openpete/bugreports/bug-*.zip" \
    "data/openpete/bk2/*.bk2" config/openpete/openpete.toml
[[ -L "$MAIN/state/openpete/cache" || "$PLATFORM" == windows ]] ||
    fail+=("main: the pipeline cache is not the shared link")
compgen -G "$MAIN/state/openpete/cache/*" >/dev/null || fail+=("main: the pipeline cache is empty")
unchanged main

# hang: the watchdog's dump files.
HANG="$(op_sandbox_run_root)"
run hang "$HANG" 2 OPENPETE_HANG_THRESHOLD=2000 -- --skip-to-level artisans --frame-limit 3000
has hang "$HANG" state/openpete/logs/hang-trace.bin state/openpete/logs/hang-ram.bin
unchanged hang

# replay: the main bundle, replayed; its outputs stay in its scratch directory.
if [[ "$PLATFORM" == linux ]]; then
    bundle="$(compgen -G "$MAIN/data/openpete/bugreports/bug-*.zip" | head -n 1)"
    if [[ -n "$bundle" ]]; then
        REPLAY="$(op_sandbox_run_root)"
        run replay "$REPLAY" 0 "OPENPETE_BUGREPORT_REPLAY_DIR=$OUT/replay-scratch" -- \
            --bug-report-replay "$bundle"
        has replay "$OUT/replay-scratch" sandbox/state/openpete/logs/openpete.log
        unchanged replay
    else
        fail+=("replay: no bundle from the main row to replay")
    fi
fi

# decoy-root: a root inside a per-user location is refused before any write.
DECOY_ROOT="$DECOY/state/openpete/sandbox-decoy"
[[ "$PLATFORM" == windows ]] && DECOY_ROOT="$LOCALAPPDATA_L/openpete/sandbox-decoy"
run decoy-root "$DECOY_ROOT" 66 -- --frame-limit 60
grep -q 'OPENPETE_SANDBOX: .*inside the real location' "$OUT/decoy-root.log" ||
    fail+=("decoy-root: no refusal line naming the real location")
unchanged decoy-root

# bare: no sandbox; the run's log lands in the decoy state directory.
if [[ "$PLATFORM" == linux ]]; then
    run bare "" 0 -- --no-card --frame-limit 60
    listing "$OUT/decoy-bare.tsv" "$DECOY"
    if cmp -s "$OUT/decoy-base.tsv" "$OUT/decoy-bare.tsv" ||
       ! grep -q "^$DECOY/state/openpete/logs/openpete.log" "$OUT/decoy-bare.tsv"; then
        fail+=("bare: VACUOUS: a run without a sandbox left the decoy listing unchanged")
    else
        echo "[sandbox] bare control: $(diff "$OUT/decoy-base.tsv" "$OUT/decoy-bare.tsv" | grep -c '^>') new decoy entries"
    fi
fi

if (( ${#fail[@]} )); then
    printf '[sandbox] FAIL: %s\n' "${fail[@]}"
    exit 1
fi
echo "[sandbox] foreign changes in the real directories: $FOREIGN"
echo "[sandbox] PASS ($PLATFORM): main, hang$([[ $PLATFORM == linux ]] && echo ', replay') wrote only under their roots; decoy-root refused (66)$([[ $PLATFORM == linux ]] && echo '; bare control changed the decoy listing')"
exit 0
