#!/usr/bin/env bash
# worktree.sh create <name> [--ref REV] [--with-duckstation]
# worktree.sh remove <name> [--force]
# worktree.sh inputs [--with-duckstation]
#
# create   Detached worktree of the checkout at <root>/worktrees/<name>, at
#          REV (default: the root checkout's HEAD). Every submodule becomes a
#          linked worktree of the root checkout's module repository at the
#          pin recorded in the new worktree's index; third_party/duckstation
#          is skipped unless --with-duckstation. Idempotent: a second run
#          adds only what is missing and refuses a submodule checked out at
#          a commit other than its pin (for third_party/duckstation, also
#          other than the patch series head in
#          third_party/duckstation-patches/applied-head.txt). No symlink, no
#          configure, no build.
# remove   Removes the submodule worktrees, then the worktree, then prunes
#          every registry. Refuses (exit 3) when the worktree or a submodule
#          carries changes to tracked files, unless --force. Untracked files
#          never block removal.
# inputs   Prints the relative path of every untracked build input a
#          worktree lacks (regen output, the decomp build directories, the
#          oracle goldens and card fixtures the root checkout holds, the
#          psxrecomp build), one per line, for every game with a
#          games/<g>/CMakeLists.txt.
#
# Root: $OPENPETE_ROOT, else the parent of the git common directory of the
# repository holding this script (the main checkout, also when the script
# runs from a worktree).
# Exit: 0 done; 1 a step failed (named on stderr); 3 tracked changes and no
# --force; 64 usage; 65 root not found.
#
# Submodule paths are linked worktrees, not clones or symlinks: `git status`
# refuses a symlink at a submodule path ("expected submodule path ... not to
# be a symbolic link"), and `git submodule update` would clone over the file
# transport, which git refuses by default. duckstation is skipped by default
# because the build never reads it; the oracle reads it only to capture a
# missing golden.
set -u

# usage [code]: the header comment above `set -u`, then exit (default 64).
usage() { sed -n '2,/^set -u$/s/^# \{0,1\}//p' "$0"; exit "${1:-64}"; }

HERE="$(cd "$(dirname "$0")" && pwd)"
if [[ -n "${OPENPETE_ROOT:-}" ]]; then
    ROOT="$OPENPETE_ROOT"
else
    common=$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
    ROOT="$(dirname "${common%/}")"
fi
check_root() {
    [[ -f "$ROOT/CMakePresets.json" && -d "$ROOT/.git" ]] \
        || { echo "worktree: checkout root not found at $ROOT (set OPENPETE_ROOT)" >&2; exit 65; }
}

fail() { echo "worktree: step '$1' failed: $2" >&2; exit 1; }
note() { echo "worktree: $*"; }

# Submodule paths recorded in a tree's .gitmodules, one per line.
submodules() {
    git -C "$1" config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}'
}

# ---- inputs -----------------------------------------------------------------
cmd_inputs() {
    local with_ds=0 cm g f
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --with-duckstation) with_ds=1; shift ;;
            -h|--help) usage 0 ;;
            *) echo "worktree: inputs: unknown argument $1" >&2; usage ;;
        esac
    done
    check_root
    for cm in "$ROOT"/games/*/CMakeLists.txt; do
        [[ -f "$cm" ]] || continue
        g=$(basename "$(dirname "$cm")")
        echo "games/$g/generated"
        echo "games/$g/manifest/globals.toml"
        echo "games/$g/manifest/symbols.toml"
        echo "games/$g/$g/build"
        for f in "$ROOT"/tests/golden/"$g"/psxtrace-* "$ROOT"/tests/golden/"$g"/card-*.mcd; do
            [[ -e "$f" ]] || continue
            echo "tests/golden/$g/$(basename "$f")"
        done
    done
    echo "third_party/psxrecomp/build"
    [[ -d "$ROOT/third_party/ffmpeg-mingw" ]] && echo "third_party/ffmpeg-mingw"
    if [[ $with_ds -eq 1 ]]; then
        echo "third_party/duckstation/build-regtest"
        echo "third_party/duckstation/dep/prebuilt"
    fi
}

# ---- create -----------------------------------------------------------------
cmd_create() {
    local name="" ref="" with_ds=0 wt s pin have moddir
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --ref) [[ $# -ge 2 ]] || usage; ref="$2"; shift 2 ;;
            --with-duckstation) with_ds=1; shift ;;
            -h|--help) usage 0 ;;
            -*) echo "worktree: create: unknown flag $1" >&2; usage ;;
            *) [[ -z "$name" ]] || { echo "worktree: create: one name only" >&2; usage; }
               name="$1"; shift ;;
        esac
    done
    check_name "$name"
    check_root
    [[ -n "$ref" ]] || ref=$(git -C "$ROOT" rev-parse HEAD) || fail worktree-add "rev-parse HEAD in $ROOT"
    wt="$ROOT/worktrees/$name"

    if [[ -e "$wt/.git" ]]; then
        note "worktree exists: $wt ($(git -C "$wt" rev-parse --short HEAD))"
    else
        [[ -e "$wt" ]] && fail worktree-add "$wt exists and is not a worktree"
        mkdir -p "$ROOT/worktrees" || fail worktree-add "mkdir $ROOT/worktrees"
        git -C "$ROOT" worktree add -q --detach "$wt" "$ref" || fail worktree-add "git worktree add $wt $ref"
        note "created $wt at $(git -C "$wt" rev-parse --short HEAD) ($ref)"
    fi

    local subs
    subs=$(submodules "$wt")
    [[ -n "$subs" ]] || fail submodules "no .gitmodules entries in $wt"
    while IFS= read -r s; do
        [[ "$s" == third_party/duckstation && $with_ds -eq 0 ]] && continue
        pin=$(git -C "$wt" ls-files -s -- "$s" | awk '{print $2}')
        [[ -n "$pin" ]] || fail "submodule $s" "no gitlink in the worktree index"
        if [[ -e "$wt/$s/.git" ]]; then
            have=$(git -C "$wt/$s" rev-parse HEAD 2>/dev/null)
            [[ "$have" == "$pin" || "$have" == "$(ds_applied_head "$wt" "$s")" ]] \
                || fail "submodule $s" "checked out at $have, the index pins $pin"
            continue
        fi
        [[ -L "$wt/$s" ]] && fail "submodule $s" "is a symbolic link; git refuses a symlinked submodule path"
        if [[ -d "$wt/$s" && -n "$(ls -A "$wt/$s")" ]]; then
            fail "submodule $s" "$wt/$s is non-empty and not a repository"
        fi
        moddir=$(git -C "$ROOT/$s" rev-parse --absolute-git-dir 2>/dev/null) \
            || fail "submodule $s" "the root checkout has no repository at $ROOT/$s (initialize it there)"
        git -C "$moddir" cat-file -e "$pin^{commit}" 2>/dev/null \
            || fail "submodule $s" "pin $pin is not in $moddir (fetch it in the root checkout's submodule)"
        rmdir "$wt/$s" 2>/dev/null
        git -C "$moddir" worktree add -q --detach "$wt/$s" "$pin" \
            || fail "submodule $s" "git -C $moddir worktree add $wt/$s $pin"
        note "submodule $s at ${pin:0:8}"
    done <<< "$subs"
    echo "worktree: $wt"
}

# ---- remove -----------------------------------------------------------------
cmd_remove() {
    local name="" force=0 wt dirty subs s d gd common fleet frc
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --force) force=1; shift ;;
            -h|--help) usage 0 ;;
            -*) echo "worktree: remove: unknown flag $1" >&2; usage ;;
            *) [[ -z "$name" ]] || { echo "worktree: remove: one name only" >&2; usage; }
               name="$1"; shift ;;
        esac
    done
    check_name "$name"
    check_root
    wt="$ROOT/worktrees/$name"

    if [[ ! -e "$wt" ]]; then
        git -C "$ROOT" worktree prune
        note "$wt does not exist; pruned the registry"
        return 0
    fi
    [[ -f "$wt/.git" ]] || fail check "$wt is not a linked worktree (no .git file)"

    dirty=$(git -C "$wt" status --porcelain --untracked-files=no 2>/dev/null | grep -v '^ *[?!]' || true)
    # DuckStation checked out at the applied patch series differs from its
    # upstream pin by design; that difference is not a change.
    if [[ -e "$wt/third_party/duckstation/.git" \
          && "$(git -C "$wt/third_party/duckstation" rev-parse HEAD 2>/dev/null)" \
             == "$(ds_applied_head "$wt" third_party/duckstation)" ]]; then
        dirty=$(grep -v '^ M third_party/duckstation$' <<< "$dirty" || true)
    fi
    subs=$(submodules "$wt")
    for s in $subs; do
        [[ -f "$wt/$s/.git" ]] || continue
        d=$(git -C "$wt/$s" status --porcelain --untracked-files=no 2>/dev/null || true)
        [[ -z "$d" ]] || dirty+=$'\n'"[$s] $d"
    done
    if [[ -n "$dirty" ]]; then
        if [[ $force -eq 0 ]]; then
            echo "worktree: $wt has uncommitted tracked changes (--force discards them):" >&2
            echo "$dirty" >&2
            exit 3
        fi
        note "discarding tracked changes (--force)"
    fi

    # Golden-fleet hosts keep a link-dest tree per checkout path; drop this
    # worktree's. No host file (exit 64) or an unreachable host does not stop
    # the removal.
    fleet="$ROOT/scripts/golden-fleet.py"
    [[ -f "$fleet" ]] || fleet="$wt/scripts/golden-fleet.py"
    if [[ -f "$fleet" ]]; then
        python3 "$fleet" clean --tree "$wt"
        frc=$?
        [[ $frc -eq 0 || $frc -eq 64 ]] || note "golden-fleet clean --tree exited $frc; its trees stay for a later clean"
    fi

    for s in $subs; do
        [[ -f "$wt/$s/.git" ]] || continue
        gd=$(git -C "$wt/$s" rev-parse --absolute-git-dir 2>/dev/null) || continue
        common=$(git -C "$wt/$s" rev-parse --git-common-dir 2>/dev/null) || continue
        [[ "$common" = /* ]] || common="$wt/$s/$common"
        if [[ "$gd" == "$common" ]]; then
            # A clone at the submodule path, not a linked worktree.
            rm -rf "${wt:?}/$s" || fail "submodule $s" "rm -rf"
            note "removed cloned submodule $s"
            continue
        fi
        git -C "$common" worktree remove --force "$wt/$s" || fail "submodule $s" "git -C $common worktree remove"
        git -C "$common" worktree prune
        note "removed submodule worktree $s"
    done

    git -C "$ROOT" worktree remove --force "$wt" || fail worktree-remove "git worktree remove --force $wt"
    git -C "$ROOT" worktree prune
    if [[ -e "$wt" ]]; then
        rm -rf "$wt" || fail worktree-remove "rm -rf $wt"
    fi
    note "removed $wt"
}

# ds_applied_head <worktree> <submodule path>: the head scripts/setup_duckstation.sh
# records for the DuckStation patch series; empty for any other submodule.
ds_applied_head() {
    [[ "$2" == third_party/duckstation ]] || return 0
    tr -d ' \n' < "$1/third_party/duckstation-patches/applied-head.txt" 2>/dev/null || true
}

check_name() {
    [[ -n "$1" ]] || usage
    [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "worktree: name must match [A-Za-z0-9._-]+" >&2; exit 64; }
}

[[ $# -ge 1 ]] || usage
cmd="$1"; shift
case "$cmd" in
    create) cmd_create "$@" ;;
    remove) cmd_remove "$@" ;;
    inputs) cmd_inputs "$@" ;;
    -h|--help|help) usage 0 ;;
    *) echo "worktree: unknown command $cmd" >&2; usage ;;
esac
