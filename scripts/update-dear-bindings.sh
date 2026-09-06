#!/usr/bin/env bash
# scripts/update-dear-bindings.sh — refresh third_party/dear_bindings/ from the
# dear_bindings release that matches the pinned Dear ImGui.
#
# dear_bindings publishes its generated output per release, so the engine build
# needs neither the generator nor its Python dependency (ply). The release is
# tagged DearBindings_v<bindings>_ImGui_v<imgui>; the ImGui half is read from
# third_party/imgui/imgui.h, so refreshing after an ImGui bump takes no
# argument. The release has no all-in-one asset — the four files are fetched
# individually.
#
# A CMake configure check compares IMGUI_VERSION_NUM in dcimgui.h with the one
# in imgui.h and fails the configure on a mismatch, so an ImGui bump without
# this refresh cannot build.
set -euo pipefail
cd "$(dirname "$0")/.."

REPO="dearimgui/dear_bindings"
DEST="third_party/dear_bindings"
FILES=(dcimgui.h dcimgui.cpp dcimgui.json LICENSE.txt)

imgui_ver=$(sed -n 's/^#define IMGUI_VERSION  *"\([^"]*\)".*/\1/p' \
            third_party/imgui/imgui.h | head -1)
[[ -n "$imgui_ver" ]] || { echo "[dear-bindings] no IMGUI_VERSION in third_party/imgui/imgui.h" >&2; exit 1; }
echo "[dear-bindings] pinned Dear ImGui $imgui_ver"

# The bindings half of the tag moves independently; take the newest release
# whose ImGui half matches.
tag=$(gh release list -R "$REPO" --limit 100 \
      | awk '{print $1}' | grep -x "DearBindings_v.*_ImGui_v${imgui_ver}" | head -1)
[[ -n "$tag" ]] || { echo "[dear-bindings] no release for ImGui v$imgui_ver in $REPO" >&2; exit 1; }
echo "[dear-bindings] release $tag"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

args=()
for f in "${FILES[@]}"; do args+=(-p "$f"); done
gh release download "$tag" -R "$REPO" "${args[@]}" -D "$TMP"

for f in "${FILES[@]}"; do
    [[ -s "$TMP/$f" ]] || { echo "[dear-bindings] $f missing or empty" >&2; exit 1; }
done

# Validate by shape before installing: a truncated transfer arrives as a
# plausible-looking file, and a broken dcimgui.h breaks every mod's compile.
got=$(sed -n 's/^#define IMGUI_VERSION_NUM  *\([0-9]*\).*/\1/p' "$TMP/dcimgui.h" | head -1)
want=$(sed -n 's/^#define IMGUI_VERSION_NUM  *\([0-9]*\).*/\1/p' third_party/imgui/imgui.h | head -1)
[[ "$got" == "$want" ]] || {
    echo "[dear-bindings] dcimgui.h reports IMGUI_VERSION_NUM $got, imgui.h $want" >&2; exit 1; }
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d.get("functions") else 1)' \
    "$TMP/dcimgui.json" || { echo "[dear-bindings] dcimgui.json has no functions" >&2; exit 1; }

mkdir -p "$DEST"
for f in "${FILES[@]}"; do install -m 0644 "$TMP/$f" "$DEST/$f"; done

# The README records what was installed; the tag line is the only line that
# changes on a refresh.
readme="$DEST/README.md"
if [[ -f "$readme" ]]; then
    tmp_readme="$readme.tmp"
    rm -f "$tmp_readme"
    sed -e "s|^Release: .*|Release: \`$tag\` (Dear ImGui \`v$imgui_ver\`).|" \
        "$readme" > "$tmp_readme"
    mv "$tmp_readme" "$readme"
    grep -q "^Release: \`$tag\`" "$readme" || {
        echo "[dear-bindings] $readme has no 'Release:' line to rewrite" >&2; exit 1; }
fi

funcs=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["functions"]))' \
        "$DEST/dcimgui.json")
echo "[dear-bindings] installed $tag — $funcs functions"
echo "[dear-bindings] regenerate the annex by rebuilding; review the table diff"
exit 0
