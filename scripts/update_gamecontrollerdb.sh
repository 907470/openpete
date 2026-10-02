#!/usr/bin/env bash
# scripts/update_gamecontrollerdb.sh - refresh the shipped SDL controller
# mapping database from https://github.com/mdqinc/SDL_GameControllerDB.
#
# The database is community-maintained and grows continuously; the copy in
# the repo is redistributed in every packaged artifact. Unlike the pinned
# dev tools there is no version to pin - the point is to take the current
# tip - so the download is validated by shape instead of by hash, and the
# revision that produced it is printed for the commit message.
#
# Refuses to install a download that does not parse. A truncated transfer,
# an HTML error page served with a 200, or an upstream reorganisation all
# arrive as a plausible-looking file, and a broken database costs every
# user their controller.
set -euo pipefail
cd "$(dirname "$0")/.."

DEST="gamecontrollerdb.txt"
BRANCH="${OPENPETE_CONTROLLERDB_BRANCH:-master}"
RAW="https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/${BRANCH}/gamecontrollerdb.txt"
API="https://api.github.com/repos/mdqinc/SDL_GameControllerDB/commits/${BRANCH}"

TMP="$DEST.tmp"
rm -f "$TMP"                                    # never validate a stale download
trap 'rm -f "$TMP"' EXIT

echo "[controllerdb] fetching $BRANCH"
curl --fail --retry 5 -L -o "$TMP" "$RAW"
[[ -s "$TMP" ]] || { echo "[controllerdb] empty download" >&2; exit 1; }

# --- validation ------------------------------------------------------------
# Keyed on the "# Source:" line rather than the SDL-version wording of the
# first line, which upstream has changed before and may change again.
grep -q '^# Source: https://github.com/[Gg]abomdq/SDL_GameControllerDB\|^# Source: https://github.com/mdqinc/SDL_GameControllerDB' "$TMP" || {
    echo "[controllerdb] no '# Source:' header - not the database" >&2; exit 1; }

old_lines=$(wc -l < "$DEST")
new_lines=$(wc -l < "$TMP")
# A floor, not a band: the database only grows, and the copy being replaced
# may be years stale, so a symmetric window would refuse a legitimate catch-up.
(( new_lines >= old_lines )) || {
    echo "[controllerdb] download has $new_lines lines, fewer than the current" \
         "$old_lines - refusing" >&2; exit 1; }

# Every mapping line is GUID,name,<field>:<value>,... - check the shape rather
# than trusting the line count. A GUID is 32 hex digits, or one of the two
# literals SDL accepts in that position (SDL_gamepad.c, SDL_AddGamepadMapping).
SHAPE='^([0-9a-fA-F]{32}|[Xx][Ii][Nn][Pp][Uu][Tt]|[Dd][Ee][Ff][Aa][Uu][Ll][Tt]),[^,]+,.*[a-z]+:'
bad=$(grep -vE '^\s*(#|$)' "$TMP" | grep -cvE "$SHAPE" || true)
(( bad == 0 )) || {
    echo "[controllerdb] $bad malformed mapping line(s) - refusing" >&2
    grep -vE '^\s*(#|$)' "$TMP" | grep -vE "$SHAPE" | head -3 >&2
    exit 1; }

mappings=$(grep -cvE '^\s*(#|$)' "$TMP")
(( mappings > 0 )) || { echo "[controllerdb] no mappings parsed" >&2; exit 1; }

mv "$TMP" "$DEST"
[[ -s "$DEST" ]] || { echo "[controllerdb] $DEST missing after install" >&2; exit 1; }

rev=$(curl --fail --silent -L "$API" 2>/dev/null \
      | sed -n 's/^  "sha": "\(.\{40\}\)".*/\1/p' | head -1 || true)
echo "[controllerdb] installed $mappings mappings ($new_lines lines, was $old_lines)"
[[ -n "$rev" ]] && echo "[controllerdb] upstream revision $rev"
exit 0
