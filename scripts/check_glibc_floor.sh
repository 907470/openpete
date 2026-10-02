#!/usr/bin/env bash
# check_glibc_floor.sh - the glibc / libstdc++ symbol floor of every ELF file
# in a release payload.
#
#   scripts/check_glibc_floor.sh PATH...          # directories and/or files
#   scripts/check_glibc_floor.sh some.AppImage    # extracted to a temp dir first
#
# A release must start on the oldest distribution it claims (Debian 11,
# glibc 2.31 / GLIBCXX 3.4.28): every ELF it ships - the exe, the bundled
# PSXRecomp, the libraries under usr/lib - is checked, since any one of them
# failing to load stops first boot with a loader error the user cannot act
# on. Prints one line per ELF with the newest GLIBC_ / GLIBCXX_ version it
# requires, then a verdict.
#
# GLIBC_MAX / GLIBCXX_MAX override the targets. Exit 0 = every ELF within
# the floor; 2 = at least one exceeds it; 64 = bad arguments or no ELF found.
set -euo pipefail

GLIBC_MAX="${GLIBC_MAX:-2.31}"
GLIBCXX_MAX="${GLIBCXX_MAX:-3.4.28}"
[[ $# -gt 0 ]] || { echo "usage: $0 PATH..." >&2; exit 64; }

tmp=""
cleanup() { if [[ -n "$tmp" ]]; then rm -rf "$tmp"; fi; }
trap cleanup EXIT

newest() { printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1; }

roots=()
for p in "$@"; do
    if [[ -f "$p" && "$p" == *.AppImage ]]; then
        tmp=$(mktemp -d "${TMPDIR:-/tmp}/glibc-floor.XXXXXX")
        abs=$(realpath "$p")
        ( cd "$tmp" && "$abs" --appimage-extract >/dev/null )
        roots+=("$tmp/squashfs-root")
    else
        roots+=("$p")
    fi
done

n=0; bad=0
while IFS= read -r -d '' f; do
    [[ "$(head -c 4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]] || continue
    n=$((n + 1))
    syms=$(objdump -T "$f" 2>/dev/null || true)
    g=$(grep -o 'GLIBC_[0-9.]*' <<<"$syms" | sort -uV | tail -1 || true)
    x=$(grep -o 'GLIBCXX_[0-9.]*' <<<"$syms" | sort -uV | tail -1 || true)
    verdict=ok
    [[ -z "$g" || $(newest "${g#GLIBC_}" "$GLIBC_MAX") == "$GLIBC_MAX" ]] || verdict=EXCEEDS
    [[ -z "$x" || $(newest "${x#GLIBCXX_}" "$GLIBCXX_MAX") == "$GLIBCXX_MAX" ]] || verdict=EXCEEDS
    [[ $verdict == ok ]] || bad=$((bad + 1))
    printf '[glibc-floor] %-7s %-14s %-16s %s\n' "$verdict" "${g:-none}" "${x:-none}" "$f"
done < <(find "${roots[@]}" -type f -print0 | sort -z)

[[ $n -gt 0 ]] || { echo "[glibc-floor] no ELF files under: $*" >&2; exit 64; }
echo "[glibc-floor] $n ELF files, $bad over the floor (GLIBC_$GLIBC_MAX / GLIBCXX_$GLIBCXX_MAX)"
[[ $bad -eq 0 ]] || exit 2
