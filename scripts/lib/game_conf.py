"""game_conf - a game's disc, boot flags, bk2 directory and default exe for
the Python harnesses.

The disc path and the game's own boot flags have one authority, the oracle
recipe games/<game>/oracle.conf (DISC, RUN_ARGS), which the shell harnesses
read through scripts/presentation-game.sh. It is a bash file, so it is read
by sourcing it in bash, never by a second parser.
"""
import os
import subprocess

GAMES = ("spyro-1", "spyro-2", "spyro-3")


def exe_name(game):
    """The per-game executable name: `spyro-2` -> `openpete-spyro2`."""
    return "openpete-" + game.replace("-", "")


def resolve(root, game, build="build/linux-release"):
    """Paths and boot flags of `game` under the source tree `root`.

    Returns {game, disc, run_args, bk2_dir, default_exe}; raises ValueError
    for an unknown game or an oracle.conf that names no DISC."""
    if game not in GAMES:
        raise ValueError("unknown game %r (one of %s)" % (game, ", ".join(GAMES)))
    conf = os.path.join(root, "games", game, "oracle.conf")
    out = subprocess.run(
        ["bash", "-c",
         'source "$1" && printf "%s\\0" "$DISC" "${RUN_ARGS[@]}"', "_", conf],
        capture_output=True, text=True, check=True).stdout
    words = out.split("\0")[:-1]
    if not words or not words[0]:
        raise ValueError("%s names no DISC" % conf)
    return {
        "game": game,
        "disc": os.path.join(root, words[0]),
        "run_args": words[1:],
        "bk2_dir": os.path.join(root, "games", game, "bk2"),
        "default_exe": os.path.join(root, build, "games", game, exe_name(game)),
    }
