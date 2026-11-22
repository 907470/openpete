# custom_level_fixture.sh - sourced: the Spyro 1 declarative custom-level mod
# the bug-report harnesses warp into.
#
# custom_level_fixture MODS WAD writes MODS/warpfx: a mod "Warp Fixture"
# with one [[level]] block "Warp Fixture" over donor dark_hollow (12), replace
# mode by default, whose level file is Dark Hollow's own data entry read from
# the disc's WAD.WAD at WAD. The WAD TOC's level entries start at byte 72, 16
# bytes each (overlay offset and length, data offset and length), level id L
# at index (L / 10 - 1) * 6 + L % 10. Returns nonzero when the entry cannot
# be read.

custom_level_fixture() {
    local mods="$1" wad="$2"
    mkdir -p "$mods/warpfx/assets/levels" || return 1
    cat > "$mods/warpfx/mod.toml" <<'TOML'
[mod]
id      = "warpfx"
name    = "Warp Fixture"
version = "1.0.0"
game    = "spyro-1"
api     = 1

[[level]]
name    = "Warp Fixture"
files   = "warp_fx"
donor   = "dark_hollow"
replace = true
TOML
    python3 -I - "$wad" "$mods/warpfx/assets/levels/warp_fx.wad" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    _, _, off, n = struct.unpack_from("<4I", f.read(2048), 72 + 2 * 16)
    f.seek(off)
    d = f.read(n)
if n == 0 or len(d) != n:
    sys.exit(1)
open(sys.argv[2], "wb").write(d)
PY
}
