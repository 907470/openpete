#!/usr/bin/env bash
# check_fbsnap_resolve.sh - a framebuffer window whose resolve fails never
# shows an earlier copy's image.
#
# Spyro 3's pause Options page copies a 256x128 window of the frozen frame
# to scratch VRAM and re-displays it every tick at its source position, so a
# window holding the right image is invisible over the backdrop and one
# holding an earlier copy's image shows another place. The run boots
# Seashell Shore headless, opens Options from the pause menu (window copy
# 1), backs out, unpauses, warps to Molten Crater through
# OPENPETE_LEVELWARP_TEST and opens Options again (window copy 2). Two legs
# run the same movie: clean, and inject, where
# OPENPETE_FBSNAP_RESOLVE_FAIL=2 fails the compose that resolves copy 2.
#
# Instrument: the native capture (-vk) of a tick of the second Options page,
# compared pixel for pixel between the legs, the differing pixels counted
# inside the window's screen rect and outside it; OPENPETE_FBSNAP_CENSUS
# for engagement.
#
# PASS: both legs took 2 window copies, the inject leg logs the injected
# failure, the capture draws the window (a census present line at its
# rect), and the two captures are byte-identical. FAIL otherwise; VACUOUS
# (exit 3) when a leg took fewer than 2 copies, injected nothing, or drew
# no window.
#
#   tests/harness/check_fbsnap_resolve.sh [--build-dir=DIR]
#
# Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (no Spyro 3 binary or disc).
# Two game processes run at once (about 70 s).
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh
export OPENPETE_VIBRATION=0

BUILD="build/linux-release"
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        *) echo "check_fbsnap_resolve: unknown argument $a" >&2; exit 64;;
    esac
done
PG_BUILD_DIR="$BUILD" pg_resolve spyro-3 >/dev/null 2>&1 || true
if [[ ! -x "${PG_BIN:-}" || ! -f "${PG_DISC:-}" ]]; then
    echo "check_fbsnap_resolve: SKIP (Spyro 3 binary or disc missing)"; exit 64
fi
OUT="${FBSNAP_RESOLVE_OUT:-$BUILD/fbsnap-resolve}"
mkdir -p "$OUT"
rm -f "${OUT:?}/movie.bk2" "${OUT:?}"/*.log "${OUT:?}"/*.png

# Pause (Start), Down twice and Cross select Options; Triangle backs out and
# Start unpauses. The warp fires at openpete_frame 3300; the second Options
# page opens at VBL 6670 and the capture is taken at VBL 7300.
SHOT=7300
LIMIT=7400
python3 - "$OUT/movie.bk2" "$LIMIT" <<'EOF'
import sys, zipfile
path, n = sys.argv[1], int(sys.argv[2])
ev = [(5, 3000), (1, 3104), (1, 3132), (9, 3170), (7, 3500), (5, 3700),
      (5, 6500), (1, 6604), (1, 6632), (9, 6670)]
key = ("LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|"
       "P1 RStick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|"
       "P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|")
rows = []
for i in range(n):
    f = ["."] * 17
    for b, v in ev:
        if v <= i < v + 6: f[b] = "X"
    rows.append(f"|    1,...|  128,  128,  128,  128,{''.join(f)}|")
header = ("MovieVersion BizHawk v2.0\nAuthor openpete\nemuVersion openpete-bk2-record\n"
          "Platform PSX\nCore Octoshock\nrerecordCount 0\nLoadDelay 66\n"
          "OpenpeteFeedKey vbl\n")
with zipfile.ZipFile(path, "w") as z:
    z.writestr("Header.txt", header)
    z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join(rows) + "\n")
EOF
[[ -f "$OUT/movie.bk2" ]] || { echo "check_fbsnap_resolve: FAIL (no movie)"; exit 1; }

# leg TAG [ENV...]: one headless run of the movie; log and capture under OUT.
leg() {
    local tag="$1"; shift
    env "$@" OPENPETE_FBSNAP_CENSUS=1 OPENPETE_LEVELWARP_TEST=3300:molten_crater \
        timeout -k 10 600 "$PG_BIN" --game spyro-3 --headless --no-config --disc "$PG_DISC" \
        ${PG_RUN_ARGS[@]+"${PG_RUN_ARGS[@]}"} --no-card --mute 1 --input-lock --vibration 0 \
        --renderer vk --aspect 4:3 --lod original --bk2 "$OUT/movie.bk2" \
        --skip-to-level seashell_shore --screenshot-native-only \
        --screenshot-on-frame "$SHOT" --screenshot-output "$OUT/$tag-{n}.png" \
        --frame-limit "$LIMIT" >"$OUT/$tag.log" 2>&1
}
leg clean OPENPETE_FBSNAP_RESOLVE_FAIL=0 &
leg inject OPENPETE_FBSNAP_RESOLVE_FAIL=2 &
wait

python3 - "$OUT" "$SHOT" <<'EOF'
import re, sys, zlib, struct
out, shot = sys.argv[1], sys.argv[2]

def png_rgb(path):
    """Decode an 8-bit RGB or RGBA non-interlaced PNG into (w, h, rows of pixel tuples)."""
    data = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            bpp = {2: 3, 6: 4}[ctype]
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    raw, rows, prev = zlib.decompress(idat), [], bytearray(w * bpp)
    stride = w * bpp
    for y in range(h):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if ft == 1: line[i] = (line[i] + a) & 255
            elif ft == 2: line[i] = (line[i] + b) & 255
            elif ft == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows

verdicts, why = [], []
logs = {t: open(f"{out}/{t}.log", errors="replace").read() for t in ("clean", "inject")}
for t, log in logs.items():
    taken = [int(m) for m in re.findall(r"\[fbsnap\] f=\d+ taken=(\d+)", log)]
    if not taken or max(taken) < 2:
        why.append(f"{t} took {max(taken) if taken else 0} window copies")
    if not re.search(r"\[fbsnap\] present vbl=\S+ .*rect=128\.00,65\.00\.\.384\.00,193\.00", log):
        why.append(f"{t} capture drew no window")
if "window resolve 2 failed by injection" not in logs["inject"]:
    why.append("inject leg injected nothing")
try:
    cw, ch, a = png_rgb(f"{out}/clean-{shot}-vk.png")
    iw, ih, b = png_rgb(f"{out}/inject-{shot}-vk.png")
except (OSError, KeyError) as e:
    print(f"check_fbsnap_resolve: FAIL (capture missing or unreadable: {e})"); sys.exit(1)
if (cw, ch) != (iw, ih):
    print(f"check_fbsnap_resolve: FAIL (capture sizes {cw}x{ch} and {iw}x{ih})"); sys.exit(1)
# The window's source rect (VRAM 128,65 256x128) in capture pixels: the
# capture spans the 512x216 draw area whose clip starts at VRAM row 12.
x0, x1 = 128 * cw // 512, 384 * cw // 512
y0, y1 = (65 - 12) * ch // 216, (193 - 12) * ch // 216
inside = outside = 0
for y in range(ch):
    for x in range(cw):
        if a[y][x] != b[y][x]:
            if x0 <= x < x1 and y0 <= y < y1: inside += 1
            else: outside += 1
print(f"check_fbsnap_resolve: capture {cw}x{ch}, differing pixels inside the window {inside}, outside {outside}")
if why:
    print("check_fbsnap_resolve: VACUOUS (" + "; ".join(why) + ")"); sys.exit(3)
if inside or outside:
    print("check_fbsnap_resolve: FAIL (the failed resolve left another image in the window)"); sys.exit(1)
print("check_fbsnap_resolve: PASS"); sys.exit(0)
EOF
