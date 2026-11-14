# mods_games_rungs.sh - the other-games rungs of tests/harness/check_mods.sh.
# SOURCED by it after mods_multigame_rungs.sh (helpers say, ok, bad,
# run_game, mod_toml, sdk_other_game; WORK, PG_GAME, KEY0, BIN); runs for
# every game.
#
# A scratch library ($WORK/games-xdg/openpete/library, reached through
# XDG_DATA_HOME) holds one entry for the paired game (sdk_other_game), its
# disc.bin a link to that game's built image; the user's library is never
# read. Two legs, the library with the entry and the library empty, each
# load three fixture mods:
#   games-req       requires_games = [other]: with the entry it loads; empty,
#                   it is refused with the log line naming the game.
#   games-opt       optional_games = [other]: loads in both legs; with the
#                   entry game_installed is 1, game_disc_path names the
#                   entry's image and read_sector of LBA 16 matches the
#                   image's bytes (FNV-1a over the 2352-byte sector); empty,
#                   game_installed is 0 and read_sector returns -1. Both
#                   legs read LBA 16 of the booted disc through game_id.
#   games-unlisted  names no game: read_sector for the paired game returns
#                   -1 and logs the refusal once, though the disc is
#                   installed.

GAMES_OTHER="$(sdk_other_game)"
# The rungs' sandbox root: the library sits in its data directory.
GAMES_ROOT="$WORK/games-sandbox"
GAMES_XDG="$GAMES_ROOT/data"
GAMES_LIB="$GAMES_XDG/openpete/library"
GAMES_MODS="$WORK/games-mods"

# The paired game's sector image: the file the cue the harness boots it from
# names, else the image beside the cue under the cue's own name (a build
# writes SCUS-<n>.bin beside a cue that names the original image's file).
games_other_bin() {
    local cue bin
    cue="$( unset OPENPETE_BIN OPENPETE_DISC; pg_resolve "$GAMES_OTHER" >/dev/null 2>&1
            echo "$PG_DISC" )"
    [ -f "$cue" ] || return 0
    bin="$(tr -d '\r' < "$cue" | sed -n 's/^FILE "\(.*\)" BINARY.*/\1/p' | head -1)"
    if [ -n "$bin" ] && [ -f "$(dirname "$cue")/$bin" ]; then
        echo "$(dirname "$cue")/$bin"
    elif [ -f "${cue%.*}.bin" ]; then
        echo "${cue%.*}.bin"
    fi
}

# games_mod <id> <manifest extra line> <body>: a fixture whose entry runs body.
games_mod() {
    mkdir -p "$GAMES_MODS/$1/src"
    mod_toml "$GAMES_MODS/$1" "$1" "game = [\"$PG_GAME\"]"${2:+$'\n'"$2"}
    cat > "$GAMES_MODS/$1/src/main.c" <<EOF_GAMES
#include <openpete_mod_api.h>
static unsigned fnv(const uint8_t* p, unsigned n) {
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    static uint8_t sec[2352];
    const char* other = "$GAMES_OTHER";
    (void)fnv; (void)other; (void)sec;
$3
    return 0;
}
EOF_GAMES
}

say "other games: requires_games, optional_games and the disc calls on $PG_GAME (paired with $GAMES_OTHER)"
GAMES_BIN="$(games_other_bin)"
[ -n "$GAMES_BIN" ] && GAMES_BIN="$(realpath "$GAMES_BIN")"
if [ -d "$(dirname "$BIN")/library" ]; then
    bad "other games: $(dirname "$BIN")/library exists and would be the library root; the rung needs it absent"
elif [ -z "$GAMES_BIN" ] || [ ! -f "$GAMES_BIN" ]; then
    bad "other games: no built image of $GAMES_OTHER (games/$GAMES_OTHER/$GAMES_OTHER/build)"
else
    games_mod games-req "requires_games = [\"$GAMES_OTHER\"]" \
'    api->log(self, OP_MOD_LOG_INFO, "games-req: entered");'
    games_mod games-opt "optional_games = [\"$GAMES_OTHER\"]" \
'    int inst = api->game_installed(self, other);
    int rc = api->read_sector(self, other, 16, sec);
    unsigned h = rc == 0 ? fnv(sec, 2352) : 0;
    const char* bin = api->game_disc_path(self, other, 1);
    int own = api->read_sector(self, api->game_id(self), 16, sec);
    api->log(self, OP_MOD_LOG_INFO, "games-opt: installed=%d rc=%d hash=%08x own=%d bin=%s",
             inst, rc, h, own, bin ? bin : "(null)");'
    games_mod games-unlisted "" \
'    int rc = api->read_sector(self, other, 16, sec);
    int rc2 = api->read_sector(self, other, 17, sec);
    api->log(self, OP_MOD_LOG_INFO, "games-unlisted: rc=%d rc2=%d", rc, rc2);'
    printf 'games-req\ngames-opt\ngames-unlisted\n' > "$GAMES_MODS/enabled.txt"

    # Expected hash: FNV-1a of LBA 16 of the paired image.
    want_hash="$(python3 - "$GAMES_BIN" <<'EOF_PY'
import sys
with open(sys.argv[1], 'rb') as f:
    f.seek(16 * 2352)
    b = f.read(2352)
h = 2166136261
for x in b:
    h = ((h ^ x) * 16777619) & 0xffffffff
print('%08x' % h)
EOF_PY
)"

    # Leg A: the paired game's disc in the library.
    rm -rf "${GAMES_XDG:?}"
    mkdir -p "$GAMES_LIB/0000000000000001"
    ln -s "$GAMES_BIN" "$GAMES_LIB/0000000000000001/disc.bin"
    printf 'FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' \
        > "$GAMES_LIB/0000000000000001/disc.cue"
    printf 'label = "%s"\nsource = "scratch"\nsha256 = "0"\nlast_played = 0\ngame = "%s"\n' \
        "$GAMES_OTHER" "$GAMES_OTHER" > "$GAMES_LIB/0000000000000001/meta.toml"
    out="$( PG_CAPTURE_ENV=("OPENPETE_SANDBOX=$GAMES_ROOT"); run_game "$GAMES_MODS" $((300 + KEY0)) )"
    opt="$(printf '%s\n' "$out" | grep -o 'games-opt: installed=.*' | head -1)"
    if printf '%s\n' "$out" | grep -q "library disc of $GAMES_OTHER: $GAMES_LIB/" &&
       printf '%s\n' "$out" | grep -q "games-req: entered"; then
        ok "requires_games, installed: games-req loaded (library $GAMES_LIB)"
    else
        bad "requires_games, installed: games-req did not load or the scratch library was not read"
        printf '%s\n' "$out" | grep -E "games-req|library" | head -4
    fi
    if [ "$opt" = "games-opt: installed=1 rc=0 hash=$want_hash own=0 bin=$GAMES_LIB/0000000000000001/disc.bin" ]; then
        ok "optional_games, installed: LBA 16 of $GAMES_OTHER read, hash $want_hash matches the image; own disc read"
    else
        bad "optional_games, installed: got '$opt', want hash $want_hash"
    fi
    n_ref="$(printf '%s\n' "$out" | grep -c "read_sector(\"$GAMES_OTHER\") refused")"
    if printf '%s\n' "$out" | grep -q "games-unlisted: rc=-1 rc2=-1" && [ "$n_ref" -eq 1 ]; then
        ok "unlisted game: read_sector returns -1 with the disc installed, one refusal line"
    else
        bad "unlisted game: $(printf '%s\n' "$out" | grep -o 'games-unlisted: .*' | head -1), $n_ref refusal lines (want 1)"
    fi

    # Leg B: the library empty.
    rm -rf "${GAMES_XDG:?}"
    mkdir -p "$GAMES_LIB"
    out="$( PG_CAPTURE_ENV=("OPENPETE_SANDBOX=$GAMES_ROOT"); run_game "$GAMES_MODS" $((300 + KEY0)) )"
    opt="$(printf '%s\n' "$out" | grep -o 'games-opt: installed=.*' | head -1)"
    if printf '%s\n' "$out" | grep -q "\"games-req\" requires $GAMES_OTHER, which has no disc in the library" &&
       ! printf '%s\n' "$out" | grep -q "games-req: entered"; then
        ok "requires_games, missing: games-req refused naming $GAMES_OTHER"
    else
        bad "requires_games, missing: no refusal naming $GAMES_OTHER, or the mod entered"
        printf '%s\n' "$out" | grep -E "games-req" | head -4
    fi
    if [ "$opt" = "games-opt: installed=0 rc=-1 hash=00000000 own=0 bin=(null)" ]; then
        ok "optional_games, missing: games-opt loaded, read_sector -1, game_installed 0"
    else
        bad "optional_games, missing: got '$opt'"
    fi
    rm -rf "${GAMES_XDG:?}"
fi
