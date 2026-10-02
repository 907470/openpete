#!/usr/bin/env python3
"""modget.py - install mods from a decentralized mod-source manifest
(the modding framework's distribution piece; OpenGOAL's model).

A mod-source is a JSON file anyone can host:

    {
      "name": "example source",
      "mods": [
        {
          "id": "cool-mod",
          "version": "1.2.0",
          "url": "https://example.com/cool-mod-1.2.0.zip",
          "sha256": "<hex digest of the zip>"
        }
      ]
    }

`modget.py <source.json|URL> <mod-id>` downloads the zip, VERIFIES the
sha256 (a mismatch aborts - the hash pins what you get), and drops
`<id>.zip` into the mods dir, where the engine's zip loader takes over.
There is no central registry and no signing; the trust posture is the
one in mods/README.md - mods are native programs, install only what
you trust.

Usage:
  tools/modget.py SOURCE MOD_ID [--mods-dir mods] [--list]
"""
import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path


def fetch(url_or_path: str) -> bytes:
    if url_or_path.startswith(("http://", "https://")):
        with urllib.request.urlopen(url_or_path, timeout=60) as r:
            return r.read()
    return Path(url_or_path).read_bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="mod-source JSON (path or URL)")
    ap.add_argument("mod_id", nargs="?", help="mod id to install")
    ap.add_argument("--mods-dir", type=Path, default=Path("mods"))
    ap.add_argument("--list", action="store_true",
                    help="list the source's mods and exit")
    args = ap.parse_args()

    src = json.loads(fetch(args.source).decode("utf-8"))
    mods = {m["id"]: m for m in src.get("mods", [])}

    if args.list or not args.mod_id:
        print(f"[modget] {src.get('name', args.source)}: "
              f"{len(mods)} mod(s)")
        for m in mods.values():
            print(f"  {m['id']} {m.get('version', '?')}")
        return 0

    m = mods.get(args.mod_id)
    if not m:
        print(f"[modget] ERROR: \"{args.mod_id}\" not in this source",
              file=sys.stderr)
        return 1
    for key in ("url", "sha256"):
        if key not in m:
            print(f"[modget] ERROR: entry lacks \"{key}\" - refusing "
                  "(the hash pin is not optional)", file=sys.stderr)
            return 1

    blob = fetch(m["url"])
    digest = hashlib.sha256(blob).hexdigest()
    if digest != m["sha256"].lower():
        print(f"[modget] ERROR: sha256 MISMATCH for {args.mod_id}\n"
              f"  manifest: {m['sha256']}\n  fetched:  {digest}\n"
              "  refusing to install", file=sys.stderr)
        return 1

    args.mods_dir.mkdir(parents=True, exist_ok=True)
    out = args.mods_dir / f"{args.mod_id}.zip"
    out.write_bytes(blob)
    print(f"[modget] installed {out} ({len(blob)} bytes, sha256 ok) - "
          f"add \"{args.mod_id}\" to enabled.txt to activate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
