#!/usr/bin/env python3
"""Print the ssh target (user@host) of one fleet host.

Usage: fleet_ssh.py <host-name>

Reads the fleet host file through scripts/golden-fleet.py's loader
($OPENPETE_FLEET_HOSTS, else ${XDG_CONFIG_HOME:-~/.config}/openpete/fleet-hosts.toml).
Exit 0 with the target on stdout; 1 when the host is unknown or not an ssh
host; 64 when the host file is missing.
"""
import importlib.util, os, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "golden_fleet", os.path.join(here, "..", "..", "scripts", "golden-fleet.py"))
gf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gf)

if len(sys.argv) != 2:
    sys.exit("usage: fleet_ssh.py <host-name>")
try:
    path, hosts = gf.load_hosts()
except gf.FleetError as e:
    print(e, file=sys.stderr)
    sys.exit(getattr(e, "code", 1) or 1)
h = hosts.get(sys.argv[1])
if h is None or h.get("transport") != "ssh":
    print(f"fleet_ssh: {sys.argv[1]} is not an ssh host in {path} "
          f"(hosts: {', '.join(sorted(hosts))})", file=sys.stderr)
    sys.exit(1)
print(h["ssh"])
