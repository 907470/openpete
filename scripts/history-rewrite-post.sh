#!/usr/bin/env bash
# Destination: openpete, scripts/history-rewrite-post.sh.
#
# history-rewrite-post.sh <rewritten-clone> <commit-map> [--dry-run] [--archive <repo>]
#
# Mechanical steps after a history rewrite of this repository, run on the
# fresh clone of the rewritten repository:
#   manifests  every reference manifest at HEAD (tests/golden/<game>/presentation/
#              blessed.tsv, tests/golden/spyro-1/dragons/blessed.tsv, and where
#              present presentation/pre-migration.tsv and
#              games/spyro-1/native-tick-corpus.manifest): each row's
#              `revision` goes through the commit map. A revision the map
#              carries becomes its image with origin `mapped`; an all-zero
#              image (a commit the rewrite dropped) refuses the run; a
#              revision the map does not carry (unreachable from every ref at
#              rewrite time) is written through with origin `archive-only`.
#              The pre-rewrite values are saved under
#              $GIT_DIR/history-rewrite-post/, the manifests are staged by
#              name and committed, then the committed tree is checked row by
#              row against the map.
#   arm        core.hooksPath = githooks and the ratchet-tsv merge driver, as
#              scripts/setup.sh sets them; both ratchets' `check --armed`.
#   restamp    complexity_ratchet.py tighten and doc_ratchet.py tighten (the
#              `# head:` pins went through the map in the manifests step); a
#              changed baseline is staged and committed through the armed
#              hook. The compile database comes from OPENPETE_COMPLEXITY_DB /
#              OPENPETE_DOCS_DB or build/linux-release/compile_commands.json.
#   tags       every tag at each of which no PNG is tracked under tests/golden;
#              with --archive, the `v*` tag names equal the archive's.
# Printed, not run: the planning repository's citation remap over the commit
# map, its hand re-pins, its prose and memory text pass, the checkout swap
# and untracked-input restore, the referrer repairs, and the fresh-clone
# store and negative-control checks.
# --dry-run prints each step's inputs and counts and writes nothing (no
# file, no config, no commit). Exit: 0 done, 2 refused, 64 usage.
set -uo pipefail
dry=0; archive=""; pos=()
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) dry=1 ;;
    --archive) archive=$2; shift ;;
    *) pos+=("$1") ;;
  esac
  shift
done
[ ${#pos[@]} -eq 2 ] || { echo "usage: $0 <rewritten-clone> <commit-map> [--dry-run] [--archive <repo>]" >&2; exit 64; }
repo=$(realpath "${pos[0]}"); map=$(realpath "${pos[1]}"); [ -z "$archive" ] || archive=$(realpath "$archive")
[ -f "$map" ] || { echo "no commit map at $map" >&2; exit 64; }
git -C "$repo" rev-parse --git-dir >/dev/null || exit 64
cd "$repo" || exit 64
gitdir=$(git rev-parse --absolute-git-dir)
say() { printf '%s\n' "$*"; }
[ $dry -eq 1 ] && say "== dry run: nothing is written"
say "repo $repo; HEAD $(git rev-parse HEAD); map $map ($(($(wc -l <"$map")-1)) entries)"

# ---- manifests ------------------------------------------------------------
manifests=()
while IFS= read -r m; do manifests+=("$m"); done < <(git ls-tree -r --name-only HEAD -- tests/golden games/spyro-1 \
  | grep -E '^tests/golden/[^/]+/presentation/(blessed|pre-migration)\.tsv$|^tests/golden/spyro-1/dragons/blessed\.tsv$|^games/spyro-1/native-tick-corpus\.manifest$')
say "== manifests: ${#manifests[@]} at HEAD"
for m in "${manifests[@]}"; do
  if ! git diff --quiet HEAD -- "$m"; then say "REFUSED: $m differs from HEAD in the worktree"; exit 2; fi
done
state=$gitdir/history-rewrite-post
python3 - "$map" "$dry" "$state" "$archive" "${manifests[@]}" <<'PY'
import os, re, subprocess, sys
mapf, dry, state, archive = sys.argv[1], sys.argv[2] == "1", sys.argv[3], sys.argv[4]
manifests = sys.argv[5:]
ZERO = re.compile(r"^0{40}$")
fwd = {}
with open(mapf) as fh:
    for n, line in enumerate(fh):
        f = line.split()
        if n == 0 and f[:2] == ["old", "new"]:
            continue
        fwd[f[0]] = f[1]
images = set(fwd.values())
def arch_has(sha):
    if not archive:
        return None
    return subprocess.run(["git", "-C", archive, "cat-file", "-e", sha + "^{commit}"],
                          capture_output=True).returncode == 0
refused, pre_rows, writes, totals = [], [], {}, dict(rows=0, mapped=0, already=0, archive_only=0, changed=0)
for m in manifests:
    text = subprocess.run(["git", "show", "HEAD:" + m], capture_output=True, check=True).stdout.decode()
    lines = text.split("\n")
    cols = None; out = []; c = dict(rows=0, mapped=0, already=0, archive_only=0, changed=0, arch_ok=0, arch_miss=0)
    for line in lines:
        if cols is None:
            if line and not line.startswith("#") and "\t" in line:
                cols = line.split("\t")
                if "revision" not in cols or "origin" not in cols or "tile" not in cols:
                    print("REFUSED: %s: column line lacks tile/revision/origin" % m); sys.exit(2)
                ri, oi, ti = cols.index("revision"), cols.index("origin"), cols.index("tile")
            out.append(line); continue
        if not line or line.startswith("#"):
            out.append(line); continue
        f = line.split("\t")
        if len(f) != len(cols):
            print("REFUSED: %s: row with %d fields: %s" % (m, len(f), line[:80])); sys.exit(2)
        c["rows"] += 1
        rev, origin = f[ri], f[oi]
        pre_rows.append((m, f[ti], rev, origin))
        if rev in fwd:
            new = fwd[rev]
            if ZERO.match(new):
                refused.append("%s %s: revision %s maps to an all-zero sha" % (m, f[ti], rev)); new = rev
            f[ri], f[oi] = new, "mapped"; c["mapped"] += 1
        elif rev in images:
            f[oi] = "mapped"; c["already"] += 1
        else:
            f[oi] = "archive-only"; c["archive_only"] += 1
            ok = arch_has(rev)
            if ok is not None:
                c["arch_ok" if ok else "arch_miss"] += 1
            print("  archive-only: %s %s %s" % (m, f[ti], rev))
        nl = "\t".join(f)
        if nl != line:
            c["changed"] += 1
        out.append(nl)
    if cols is None:
        print("REFUSED: %s: no column line" % m); sys.exit(2)
    for k in totals:
        totals[k] += c[k]
    print("  %s: rows %d, mapped %d, already-mapped %d, archive-only %d%s, rows changed %d"
          % (m, c["rows"], c["mapped"], c["already"], c["archive_only"],
             "" if not archive else " (in archive %d, not in archive %d)" % (c["arch_ok"], c["arch_miss"]),
             c["changed"]))
    if c["arch_miss"]:
        refused.append("%s: %d archive-only revisions do not resolve in %s" % (m, c["arch_miss"], archive))
    newtext = "\n".join(out)
    if newtext != text:
        writes[m] = newtext
print("  total: rows %(rows)d, mapped %(mapped)d, already-mapped %(already)d, archive-only %(archive_only)d, rows changed %(changed)d" % totals)
if refused:
    for r in refused:
        print("REFUSED: " + r)
    sys.exit(2)
if dry:
    print("  would rewrite %d manifests: %s" % (len(writes), " ".join(sorted(writes)) or "none"))
    sys.exit(0)
os.makedirs(state, exist_ok=True)
pre_path = os.path.join(state, "pre-rewrite-revisions.tsv")
if os.path.exists(pre_path):
    print("  kept the saved values of the first run: " + pre_path)
    pre_rows = []
with open(pre_path, "a") as fh:
    if pre_rows:
        fh.write("manifest\ttile\trevision\torigin\n")
    for r in pre_rows:
        fh.write("\t".join(r) + "\n")
for m, t in writes.items():
    with open(m, "w") as fh:
        fh.write(t)
with open(os.path.join(state, "written.txt"), "w") as fh:
    fh.write("\n".join(sorted(writes)) + ("\n" if writes else ""))
PY
rc=$?; [ $rc -eq 0 ] || exit $rc

if [ $dry -eq 0 ]; then
  written=()
  while IFS= read -r m; do [ -n "$m" ] && written+=("$m"); done <"$state/written.txt"
  # The ratchet baselines pin the commit they were computed against. The
  # pin goes through the commit map here, before the hooks are armed: the
  # armed pre-commit hook owns both files and restores a hand-edited pin.
  # Every tracked baseline carrying a `# head:` pin (complexity, docs, the
  # two thread-roles and the two warnings baselines); a pin the map lacks
  # refuses, since it names a commit the rewritten history does not hold.
  while IFS= read -r tsv; do
    pin=$(sed -n 's/^# head: //p' "$tsv" | head -1)
    [ -n "$pin" ] || continue
    img=$(awk -v p="$pin" 'NR>1 && $1==p {print $2}' "$map")
    [ -n "$img" ] || { say "REFUSED: $tsv head pin $pin is not in the map"; exit 2; }
    [ "$img" != "$pin" ] || continue
    case $img in *[!0]*) ;; *) say "REFUSED: $tsv head pin maps to an all-zero sha"; exit 2 ;; esac
    sed -i "s/^# head: $pin\$/# head: $img/" "$tsv"
    written+=("$tsv"); say "  $tsv: head pin $pin -> $img"
  done < <(git grep -l '^# head: ' HEAD -- 'baselines/*.tsv' | sed 's/^HEAD://')
  if [ ${#written[@]} -gt 0 ]; then
    git add -- "${written[@]}"
    git commit -q -m "golden, baselines: refresh the pinned revisions" -- "${written[@]}" || { say "REFUSED: manifest commit failed"; exit 2; }
    say "  committed $(git rev-parse --short=12 HEAD): ${#written[@]} files"
  else
    say "  no manifest changed; no commit"
  fi
  python3 - "$map" "$state/pre-rewrite-revisions.tsv" <<'PY'
import re, subprocess, sys
fwd = {}
for n, line in enumerate(open(sys.argv[1])):
    f = line.split()
    if n == 0 and f[:2] == ["old", "new"]:
        continue
    fwd[f[0]] = f[1]
images = set(fwd.values())
pre = {}
for n, line in enumerate(open(sys.argv[2])):
    if n == 0:
        continue
    m, tile, rev, origin = line.rstrip("\n").split("\t")
    pre[(m, tile)] = rev
bad, checked, shas = [], 0, set()
for m in sorted({k[0] for k in pre}):
    text = subprocess.run(["git", "show", "HEAD:" + m], capture_output=True, check=True).stdout.decode()
    cols = None
    for line in text.split("\n"):
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if cols is None:
            cols = f; ri, oi, ti = cols.index("revision"), cols.index("origin"), cols.index("tile"); continue
        old = pre.get((m, f[ti]))
        want = fwd.get(old, old) if old not in images else old
        if f[oi] == "mapped":
            checked += 1; shas.add(f[ri])
            if re.match(r"^0{40}$", f[ri]) or f[ri] != want:
                bad.append("%s %s: %s, expected %s" % (m, f[ti], f[ri], want))
        elif old in fwd:
            bad.append("%s %s: origin %s on a mapped revision" % (m, f[ti], f[oi]))
p = subprocess.run(["git", "cat-file", "--batch-check"], input="".join(s + "^{commit}\n" for s in sorted(shas)),
                   capture_output=True, text=True)
missing = [l for l in p.stdout.splitlines() if l.endswith("missing")]
print("  check (committed tree): %d mapped rows, %d distinct revisions, %d unresolvable, %d not the map image"
      % (checked, len(shas), len(missing), len(bad)))
for b in bad[:20] + missing[:20]:
    print("    " + b)
sys.exit(2 if bad or missing else 0)
PY
  rc=$?; [ $rc -eq 0 ] || { say "REFUSED: manifest check"; exit 2; }
  st=$(git status --porcelain -- "${manifests[@]}")
  [ -z "$st" ] || { say "REFUSED: manifests modified after commit: $st"; exit 2; }
fi

# ---- arm --------------------------------------------------------------------
say "== arm: core.hooksPath now '$(git config core.hooksPath || true)', merge.ratchet-tsv.driver now '$(git config merge.ratchet-tsv.driver || true)'"
if [ $dry -eq 0 ]; then
  git config core.hooksPath githooks
  git config merge.ratchet-tsv.driver 'python3 scripts/fences/ratchet_tsv_merge.py %O %A %B %P'
  for tool in complexity_ratchet.py doc_ratchet.py; do
    if [ ! -f "scripts/fences/$tool" ]; then say "  SKIP $tool check --armed: not in this tree"; continue; fi
    python3 "scripts/fences/$tool" check --armed || { say "REFUSED: $tool not armed"; exit 2; }
  done
else
  say "  would set core.hooksPath=githooks and the ratchet-tsv merge driver, then run both ratchets' check --armed"
fi

# ---- restamp ----------------------------------------------------------------
head=$(git rev-parse HEAD)
for pair in complexity_ratchet.py:baselines/complexity/cognitive.tsv doc_ratchet.py:baselines/docs/undocumented.tsv; do
  tool=${pair%%:*}; tsv=${pair#*:}
  pin=$(git show "HEAD:$tsv" 2>/dev/null | sed -n 's/^# head: //p' | head -1)
  rows=$(git show "HEAD:$tsv" 2>/dev/null | grep -vc '^#')
  inmap=$(awk -v p="$pin" 'NR>1 && $1==p {print $2}' "$map")
  say "== restamp $tsv: $rows lines, head pin ${pin:-none} (map image ${inmap:-not in map}), HEAD $head"
  if [ ! -f "scripts/fences/$tool" ]; then say "  SKIP: scripts/fences/$tool not in this tree"; continue; fi
  if [ $dry -eq 1 ]; then say "  would run: python3 scripts/fences/$tool tighten; stage and commit $tsv when changed"; continue; fi
  python3 "scripts/fences/$tool" tighten || { say "REFUSED: $tool tighten"; exit 2; }
  if git diff --quiet -- "$tsv"; then say "  $tsv unchanged"; continue; fi
  say "  $tsv: $(git diff --numstat -- "$tsv" | awk '{print "+" $1 " -" $2}') lines"
  git add -- "$tsv"
  git commit -q -m "baselines: refresh the head pin of ${tsv##*/}" || { say "REFUSED: $tsv commit refused by the hook"; exit 2; }
  say "  committed $(git rev-parse --short=12 HEAD)"
done

# ---- tags -------------------------------------------------------------------
tags=$(git tag -l | sort)
say "== tags: $(echo "$tags" | grep -c .) ($(echo $tags))"
fail=0
if [ -n "$archive" ]; then
  # Only the release tags (`v*`) are rewritten and published. The archive
  # mirrors the local repository, so it also holds local-only tags (a census
  # tag on a side commit) that the rewrite leaves behind by design; comparing
  # those would refuse every run.
  want=$(git -C "$archive" tag -l 'v*' | sort)
  have=$(git tag -l 'v*' | sort)
  d=$(diff <(echo "$want") <(echo "$have"))
  [ -z "$d" ] && say "  v* tag names equal the archive's" || { say "  FAIL v* tag names differ from the archive's: $d"; fail=1; }
fi
for t in $tags; do
  n=$(git ls-tree -r --name-only "$t" -- tests/golden | grep -c '\.png$')
  [ "$n" -eq 0 ] || { say "  FAIL $t tracks $n PNGs under tests/golden"; fail=1; }
done
[ $fail -eq 0 ] && say "  every tag tracks 0 PNGs under tests/golden" || exit 2

# ---- printed, not run ---------------------------------------------------------
cat <<TXT
== manual steps (not run by this script)
  1. planning repository: run its citation remap over $map (the remap mode,
     then its --prose mode); re-pin by hand every pinned revision the map
     sends to an all-zero sha; then its strict citation check.
  2. planning repository and memory: the text pass with the commit map over
     the memory directory, the litigation, spike and docs trees and the plan
     index; regenerate the derived citation ledger.
  3. checkout swap: displace the pre-rewrite checkout to a recorded path,
     move its .git/modules into this clone, restore the untracked build
     inputs (games/<g>/generated, the manifest tomls, the disc directories,
     the oracle goldens, third_party/psxrecomp/build).
  4. fresh-clone checks: an empty-store presentation gate run in the pinned
     container, the corrupted-tile negative control and a materialize of one
     pre-rewrite row; golden-at against the archive and its commit map.
  5. referrer repairs for the removed paths (mod rows, bk2 rows, the mod
     audit page), then the mod and docs gates.
TXT
say "post: done$([ $dry -eq 1 ] && echo ' (dry run)')"
