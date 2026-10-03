#!/usr/bin/env bash
# scripts/setup_duckstation.sh - bootstrap the DuckStation regtest build that
# captures the openpete .psxtrace oracle goldens.
#
# third_party/duckstation pins upstream stenzek/duckstation at the commit the
# psxtrace patch series is based on. DuckStation is CC BY-NC-ND 4.0, which
# forbids distributing a modified copy, so the project carries its changes as
# the series in third_party/duckstation-patches/ (listed in `series`) and
# this script applies them onto the pin with `git am`:
#
#   - detached HEAD at the pin, then `git am` of every patch in order;
#   - committer name and email are constants and each committer date is the
#     patch's author date, with system and global git config, hooks and
#     signing excluded, so every machine produces the same commit ids;
#   - the resulting head must equal third_party/duckstation-patches/
#     applied-head.txt; DuckStation stamps it into the binary as its scm hash
#     and every trace header carries it, and the oracle gate and the capture
#     script compare against the file.
#
# Idempotent: a checkout already at the recorded head whose last application
# used the current pin and patch files changes nothing. A checkout at another
# head is reset to the pin and re-applied when its tracked files are clean and
# it carries no commits on top of the recorded head; otherwise the script
# refuses and names the reason. The build step runs on every invocation
# (ninja is a no-op when nothing changed), so the binary always carries the
# checkout's head.
#
# Usage: scripts/setup_duckstation.sh [--record] [--no-build]
#   --record    write the applied head to applied-head.txt instead of refusing
#               on a mismatch (after an intentional change to the series or
#               the pin; the oracle goldens are then recaptured)
#   --no-build  apply the series only
#
# Exit: 0 done; 1 a step failed or a refusal (reason on stderr); 64 usage.
set -euo pipefail
cd "$(dirname "$0")/.."

DS_DIR="third_party/duckstation"
PATCH_DIR="third_party/duckstation-patches"
RECORD_FILE="$PATCH_DIR/applied-head.txt"
DEPS_VERSION="release-20260404"
DEPS_FILE="deps-linux-x64.tar.xz"
DEPS_SHA256="8266f12d3c43e0244411908e71cf150f4d57d5e3b0782ce0a9c295905a3efe7b"
COMMITTER_NAME="openpete psxtrace series"
COMMITTER_EMAIL="psxtrace-series@openpete.invalid"

RECORD=0
BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --record) RECORD=1; shift ;;
        --no-build) BUILD=0; shift ;;
        -h|--help) sed -n '2,/^set -euo/s/^# \{0,1\}//p' "$0" | sed '$d'; exit 0 ;;
        *) echo "[setup-ds] unknown argument: $1" >&2; exit 64 ;;
    esac
done

die() { echo "[setup-ds] refusing: $*" >&2; exit 1; }

if [[ ! -f "$DS_DIR/CMakeLists.txt" ]]; then
    echo "[setup-ds] initialising submodule"
    git submodule update --init --recursive "$DS_DIR"
fi

# ---- apply the patch series onto the pin -------------------------------------
PIN="$(git ls-files -s "$DS_DIR" | awk '{print $2}')"
[[ -n "$PIN" ]] || die "no gitlink for $DS_DIR in the index"
git -C "$DS_DIR" cat-file -e "$PIN^{commit}" 2>/dev/null \
    || die "pin $PIN is not in the submodule repository (git -C $DS_DIR fetch upstream)"

PATCHES=()
while IFS= read -r p; do
    [[ -z "$p" || "$p" == \#* ]] && continue
    [[ -f "$PATCH_DIR/$p" ]] || die "series names $p, absent from $PATCH_DIR"
    PATCHES+=("$(pwd)/$PATCH_DIR/$p")
done < "$PATCH_DIR/series"
[[ ${#PATCHES[@]} -gt 0 ]] || die "$PATCH_DIR/series lists no patch"

# Digest of everything the head is a function of: the pin, the series order
# and every patch file's bytes. Stored in the submodule's git directory after
# a successful application; a match means the checkout's head is current.
INPUTS="$( { echo "pin $PIN"; for p in "${PATCHES[@]}"; do
    echo "$(basename "$p") $(sha256sum < "$p" | cut -d' ' -f1)"; done; } \
    | sha256sum | cut -d' ' -f1)"
DS_GITDIR="$(git -C "$DS_DIR" rev-parse --absolute-git-dir)"
STAMP="$DS_GITDIR/openpete-series.sha256"

RECORDED=""
[[ -f "$RECORD_FILE" ]] && RECORDED="$(tr -d ' \n' < "$RECORD_FILE")"
HEAD_NOW="$(git -C "$DS_DIR" rev-parse HEAD)"
# Staging the submodule path after setup moves the gitlink to the applied
# head, a commit upstream does not have.
[[ "$PIN" != "$RECORDED" ]] \
    || die "the $DS_DIR gitlink points at the applied head $PIN, not the upstream pin; restore it (git restore --staged $DS_DIR)"

[[ -d "$DS_GITDIR/rebase-apply" ]] \
    && die "a git am or rebase is in progress in $DS_DIR (git -C $DS_DIR am --abort)"
# A build from modified tracked files would stamp a head whose tree it does
# not contain.
if [[ -n "$(git -C "$DS_DIR" status --porcelain --untracked-files=no)" ]]; then
    die "$DS_DIR has changes to tracked files; commit them as a patch in $PATCH_DIR/ or discard them"
fi

if [[ -n "$RECORDED" && "$HEAD_NOW" == "$RECORDED" \
      && "$(cat "$STAMP" 2>/dev/null)" == "$INPUTS" ]]; then
    echo "[setup-ds] series already applied: $DS_DIR at $HEAD_NOW"
else
    if [[ -n "$RECORDED" && "$HEAD_NOW" != "$RECORDED" ]] \
       && git -C "$DS_DIR" cat-file -e "$RECORDED^{commit}" 2>/dev/null \
       && git -C "$DS_DIR" merge-base --is-ancestor "$RECORDED" "$HEAD_NOW"; then
        die "$DS_DIR carries $(git -C "$DS_DIR" rev-list --count "$RECORDED..$HEAD_NOW") commit(s) on top of the applied series; export them into $PATCH_DIR/ (git format-patch --zero-commit --no-signature -N) or check out $RECORDED"
    fi
    echo "[setup-ds] applying ${#PATCHES[@]} patches onto pin $PIN"
    git -C "$DS_DIR" checkout -q --detach "$PIN"
    if ! GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
         GIT_COMMITTER_NAME="$COMMITTER_NAME" GIT_COMMITTER_EMAIL="$COMMITTER_EMAIL" \
         git -C "$DS_DIR" -c core.hooksPath=/dev/null -c core.autocrlf=false \
             -c i18n.commitEncoding=UTF-8 \
             am -q --committer-date-is-author-date --no-3way --no-message-id \
                --no-keep-cr --no-scissors --utf8 --no-gpg-sign \
                --whitespace=nowarn "${PATCHES[@]}"; then
        git -C "$DS_DIR" am --abort || true
        git -C "$DS_DIR" checkout -q --detach "$PIN"
        die "git am failed; $DS_DIR left at the pin $PIN"
    fi
    HEAD_NOW="$(git -C "$DS_DIR" rev-parse HEAD)"
    if [[ "$HEAD_NOW" != "$RECORDED" ]]; then
        if [[ $RECORD -eq 1 ]]; then
            echo "$HEAD_NOW" > "$RECORD_FILE"
            echo "[setup-ds] recorded $HEAD_NOW in $RECORD_FILE (was ${RECORDED:-absent});" \
                 "recapture every oracle golden"
        else
            die "applied head $HEAD_NOW differs from $RECORD_FILE (${RECORDED:-absent}): the pin or the series changed without the record; rerun with --record and recapture the oracle goldens"
        fi
    fi
    echo "$INPUTS" > "$STAMP"
fi
echo "[setup-ds] head: $HEAD_NOW"

[[ $BUILD -eq 1 ]] || exit 0

# ---- build --------------------------------------------------------------------
if [[ ! -d "$DS_DIR/dep/prebuilt/linux-x64" ]]; then
    echo "[setup-ds] downloading prebuilt deps $DEPS_VERSION"
    mkdir -p "$DS_DIR/dep/prebuilt"
    ( cd "$DS_DIR/dep/prebuilt" && \
        curl --fail --retry 5 -LO \
            "https://github.com/duckstation/dependencies/releases/download/${DEPS_VERSION}/${DEPS_FILE}" && \
        echo "${DEPS_SHA256}  ${DEPS_FILE}" | sha256sum -c - && \
        tar xJf "$DEPS_FILE" && \
        rm "$DEPS_FILE" )
fi

# Stage the BIOS DuckStation expects.
mkdir -p "$HOME/.local/share/duckstation/bios"
if [[ ! -f "$HOME/.local/share/duckstation/bios/scph1001.bin" ]]; then
    if [[ -f ".tools/pcsx-redux/scph1001.bin" ]]; then
        echo "[setup-ds] copying SCPH1001 BIOS"
        cp ".tools/pcsx-redux/scph1001.bin" "$HOME/.local/share/duckstation/bios/"
    else
        echo "[setup-ds] WARN: scph1001.bin not found at .tools/pcsx-redux/" >&2
    fi
fi

if [[ ! -f "$DS_DIR/build-regtest/CMakeCache.txt" ]]; then
    echo "[setup-ds] configuring duckstation-regtest"
    ( cd "$DS_DIR" && \
        CC=clang-19 CXX=clang++-19 cmake -B build-regtest \
            -DBUILD_REGTEST=ON \
            -DENABLE_OPENGL=ON -DENABLE_VULKAN=OFF \
            -DENABLE_WAYLAND=OFF -DENABLE_X11=OFF \
            -DCMAKE_BUILD_TYPE=Release -G Ninja )
fi
# The scmversion target rewrites the build stamp whenever HEAD moved, so the
# binary always reports the checkout's head.
echo "[setup-ds] building duckstation-regtest"
cmake --build "$DS_DIR/build-regtest" --target duckstation-regtest

DS_BIN="$DS_DIR/build-regtest/bin/duckstation-regtest"
DS_BUILD="$(strings "$DS_BIN" | grep -E '^[0-9a-f]{40}$' | head -1 || true)"
[[ "$DS_BUILD" == "$HEAD_NOW" ]] \
    || die "built binary stamps ${DS_BUILD:-no hash}, the checkout is at $HEAD_NOW"
echo "[setup-ds] done: $DS_BIN (scm hash $DS_BUILD)"
