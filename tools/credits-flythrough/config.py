"""config - the per-game credits fly-through table (games/<game>.toml).

Every stage reads one game's table through load(): the scene list (level,
engage VBL, frame limit, per-scene engine knobs, path offset), the path
extraction and smoothing parameters, the speech bk2, the render defaults,
the post fade and the music fit. Paths in the table are never machine paths;
the disc defaults to the game's oracle recipe (games/<game>/oracle.conf).
"""
import json
import os
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
import game_conf  # noqa: E402


def games():
    """The games with a table under games/."""
    return sorted(f[:-5] for f in os.listdir(os.path.join(HERE, "games")) if f.endswith(".toml"))


def load(game):
    """Return the table of `game` with each scene's defaults filled in.

    A scene gains `index`, `engage_vbl` (the game's default unless the row
    sets one), `env` (the game's [render.env] overlaid by the row's) and
    `offset` (x, y, z camera units added to every key, default 0)."""
    path = os.path.join(HERE, "games", game + ".toml")
    if not os.path.exists(path):
        raise SystemExit(f"no table for {game} (have {', '.join(games())})")
    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    render = cfg["render"]
    common = dict(render.get("env", {}))
    for i, sc in enumerate(cfg["scene"]):
        sc["index"] = i
        sc.setdefault("engage_vbl", render["engage_vbl"])
        sc["env"] = {**common, **sc.get("env", {})}
        sc["offset"] = list(sc.get("offset", [0, 0, 0]))
    if len(cfg["scene"]) != cfg["paths"]["scenes"]:
        raise SystemExit(f"{path}: {len(cfg['scene'])} scene rows for {cfg['paths']['scenes']} paths")
    return cfg


def scene_set(text, count):
    """Scene indices from '0,3-5' (default every scene)."""
    if not text:
        return list(range(count))
    out = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    bad = [i for i in out if not 0 <= i < count]
    if bad:
        raise SystemExit(f"scene {bad[0]} out of range 0-{count - 1}")
    return out


def default_disc(game):
    """The game's disc image named by its oracle recipe."""
    return game_conf.resolve(ROOT, game)["disc"]


def scene_dir(render_dir, index):
    """The directory one scene's capture writes (run.log, clip.mkv, qc.json)."""
    return os.path.join(render_dir, f"s{index}")


def manifest_path(paths_dir):
    """The paths stage's manifest: disc key count per scene."""
    return os.path.join(paths_dir, "manifest.json")


def scene_keys(paths_dir, index):
    """The disc key count N of scene `index` (the scene lasts 2N VBLs in the game)."""
    with open(manifest_path(paths_dir)) as f:
        return json.load(f)["keys"][index]
