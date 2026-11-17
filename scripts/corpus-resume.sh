#!/usr/bin/env bash
#
# corpus-resume.sh - row completion stamps and their import, shared by the
# presentation-golden gate and its two capture corpora. SOURCED, after
# scripts/presentation-game.sh and pg_resolve, not executed.
#
# A re-run of an interrupted capture of one run (the golden fleet re-queues
# an attempt that passed its deadline or lost its supervisor) captures only
# the rows the interrupted attempt did not finish. Off unless
# OPENPETE_RESUME_RUN names the run; the fleet's supervisor sets it to its run
# id. A capture without it writes no stamp and imports nothing.
#
# Stamp. When the last leg of a row finishes (every LOD mode times the canon,
# inter and ident legs, each with every tile it was asked for), the row's
# completion stamp is written into OUT/resume/<kind>.<key>.stamp, where kind is
# bk2, freecam or credits and key the row's tile key:
#   run: <OPENPETE_RESUME_RUN>
#   tree: <sha256 of the OPENPETE_CAPTURE_FIELDS file: head, source tree,
#          recomp input and generated tree of the shipped tree>
#   binary: <sha256 of the capture binary>
#   params: <kind, game, tier and the capture settings of pg_cap_settings>
#   row: <the row's manifest line, verbatim>
#   tile: <sha256 of the file> <name>      one per tile of the row, and one
#                                          per tile's tile-census sidecar
#                                          (<tile base>.census), so an import
#                                          moves the sidecars with the PNGs
#   device: <the leg's Vulkan device line>  one per leg that logged one
# A leg writes its own part under OUT/resume/legs/ when it finishes; the leg
# that completes the row assembles the stamp under a lock and renames it into
# place, so a row interrupted mid-capture has no stamp. The credits leg is the
# row `credits-interp` (key credits) whose one tile is credits-verdict.txt,
# stamped only when the leg passed; the identity leg is a leg of its row.
#
# Import (rs_import, by the gate before it captures). The previous attempt's
# output directory is set aside; a stamp is imported only when its run, tree,
# binary and params equal this attempt's, its row line is a line of the
# current manifest that this run's filter selects, its key is the row's key
# (a repeat copy's key `<key>~r<k>` only under PG_REPEAT >= k; a row's own key
# only without repeat), and every tile it lists is present with its sha256. An imported row's tiles
# and stamp move into the fresh output directory and its device lines are
# appended to capture-device.txt; everything else of the previous attempt is
# deleted before the capture starts. OUT/resume/imported.tsv lists the
# imported rows (`kind key png-tiles files`), from which the verdict's census
# counts captured and imported tiles separately.
#
# Stream. Under OPENPETE_STREAM_FILE (set only by the golden fleet's
# supervisor, to its attempt's own file) the leg that completes a row also
# appends the row's verdict lines to that file, outside the row lock, through
# tests/harness/presentation-stream.py: one SAME, DIVERGE, EXTRA, IDENT or
# EXCEED line per tile, classified by the end verdict's own functions. The
# leg count runs when the stream or the stamps are on; only the stamp write
# depends on OPENPETE_RESUME_RUN. rs_import classifies each imported row's
# tiles again and appends their lines tagged `imported`, and an imported
# credits leg appends `CREDITS rc=N <epoch> imported`.
# OPENPETE_STREAM_POKE=<tile substring> is a test knob: every tile of a
# finishing leg whose name contains it has its pixel (0,0) changed before the
# leg hashes it, so the row streams a DIVERGE line and its stamp matches the
# changed tile.
#
# Variables: RS_ON (1 when stamps are written and read), RS_RUN, RS_TREE,
# RS_BIN, RS_AFFINE, RS_OUT, RS_DIR (OUT/resume), RS_LEGS (legs per row),
# RS_STREAM (the stream file, empty when off).

# Bind the resume state to an output directory. args: outdir binary affine
rs_init() {
  RS_ON=0 RS_OUT="$1" RS_DIR="$1/resume" RS_AFFINE="$3"
  RS_RUN="${OPENPETE_RESUME_RUN:-}" RS_STREAM="${OPENPETE_STREAM_FILE:-}"
  pg_cap_settings
  RS_LEGS=$(( 3 * $(wc -w <<<"$PG_CAP_LODS") ))
  [[ -z "$RS_STREAM" ]] || mkdir -p "$RS_DIR"
  [[ -n "$RS_RUN" ]] || return 0
  if [[ -z "${OPENPETE_CAPTURE_FIELDS:-}" || ! -f "${OPENPETE_CAPTURE_FIELDS:-}" ]]; then
    echo "[resume] off: OPENPETE_CAPTURE_FIELDS names no file, so no tree identity to stamp"
    return 0
  fi
  if [[ ! -f "$2" ]]; then echo "[resume] off: no binary at $2"; return 0; fi
  RS_TREE="$(sha256sum <"$OPENPETE_CAPTURE_FIELDS" | cut -d' ' -f1)"
  RS_BIN="$(sha256sum <"$2" | cut -d' ' -f1)"
  mkdir -p "$RS_DIR"
  RS_ON=1
}

# The capture settings a row's tiles depend on beyond the binary and tree.
# The game knobs a gate forwards (PG_KNOBS_KEY, pg_knob_screen) are one of
# them; a stock run's line carries no knobs field.
# args: kind affine
rs_params() {
  local knobs="${PG_KNOBS_KEY:+ knobs=$PG_KNOBS_KEY}"
  case "$1" in
    bk2) echo "kind=bk2 game=$PG_GAME tier=$PG_TIER affine=$2 aspect=$PG_CAP_ASPECT lods=$PG_CAP_LODS samples=$PG_MAX_SAMPLES$knobs";;
    freecam) echo "kind=freecam game=$PG_GAME tier=$PG_TIER affine=$2 aspect=$PG_CAP_ASPECT lods=$PG_CAP_LODS start=$PG_FREECAM_START$knobs";;
    *) echo "kind=$1 game=$PG_GAME tier=$PG_TIER$knobs";;
  esac
}

rs_stamp_path() { echo "$RS_DIR/$1.$2.stamp"; }   # args: kind key

# True when this attempt holds the row's stamp (imported, or written by this
# attempt). args: kind key
rs_imported() { (( ${RS_ON:-0} )) && [[ -f "$(rs_stamp_path "$1" "$2")" ]]; }

# One `tile:` line for a file of the output directory. args: name
rs_tile_line() { echo "tile: $(sha256sum <"$RS_OUT/$1" | cut -d' ' -f1) $1"; }

# Write a stamp from the body on stdin (tile and device lines), atomically.
# args: kind key row
rs_write_stamp() {
  local s; s="$(rs_stamp_path "$1" "$2")"
  { printf 'run: %s\ntree: %s\nbinary: %s\nparams: %s\nrow: %s\n' \
      "$RS_RUN" "$RS_TREE" "$RS_BIN" "$(rs_params "$1" "$RS_AFFINE")" "$3"; cat; } >"$s.tmp" \
    && mv -f "$s.tmp" "$s"
}

# Change pixel (0,0) of each named tile whose name contains
# OPENPETE_STREAM_POKE (a test knob; see Stream above). args: tile...
rs_poke() {
  [[ -n "${OPENPETE_STREAM_POKE:-}" ]] || return 0
  local t
  for t in "$@"; do
    [[ "$t" == *-vk.png && "$t" == *"$OPENPETE_STREAM_POKE"* ]] || continue
    python3 -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
p = im.getpixel((0, 0))
im.putpixel((0, 0), ((p[0] + 128) % 256, p[1], p[2]))
im.save(sys.argv[1])' "$RS_OUT/$t" && echo "[stream] poked $t"
  done
}

# Append the verdict lines of tiles of the output directory to the stream.
# args: tag tile...
rs_stream_tiles() {
  local tag="$1"; shift
  (( $# )) || return 0
  python3 tests/harness/presentation-stream.py "$RS_OUT" "$@" --game "$PG_GAME" \
    --manifest "$PG_BLESSED" --baseline "$PG_IDENT_BASELINE" --stream "$RS_STREAM" --tag "$tag" \
    || echo "[stream] FAIL: no line for $# tile(s) starting $1"
}

# Append the credits leg's result to the stream. args: rc [tag]
rs_stream_credits() {
  [[ -n "${RS_STREAM:-}" ]] || return 0
  printf 'CREDITS rc=%s %s%s\n' "$1" "$(date +%s.%3N)" "${2:+ $2}" >>"$RS_STREAM"
}

# Mark the stream complete: the capture reached its verdict.
rs_stream_end() {
  [[ -n "${RS_STREAM:-}" ]] || return 0
  printf 'END %s\n' "$(date +%s.%3N)" >>"$RS_STREAM"
}

# Record a finished leg of a row. The leg that completes the row writes the
# row's stamp (stamps on) and, outside the lock, streams the row's tiles
# (stream on). args: kind key row leg-id log tile...
rs_leg_done() {
  (( ${RS_ON:-0} )) || [[ -n "${RS_STREAM:-}" ]] || return 0
  local kind="$1" key="$2" row="$3" leg="$4" log="$5"; shift 5
  local d="$RS_DIR/legs/$kind.$key" t dev
  rs_poke "$@"
  mkdir -p "$d"
  {
    for t in "$@"; do rs_tile_line "$t"; done
    dev="$(pg_device_line "$log")"; [[ -z "$dev" ]] || echo "device: $dev"
  } >"$d/$leg.tmp" && mv -f "$d/$leg.tmp" "$d/$leg.leg"
  (
    flock 9
    [[ -e "$(rs_stamp_path "$kind" "$key")" ]] && exit 0
    n=0; for t in "$d"/*.leg; do [[ -e "$t" ]] && n=$((n + 1)); done
    (( n >= RS_LEGS )) || exit 0
    cat "$d"/*.leg >"$d.row"
    if (( RS_ON )); then rs_write_stamp "$kind" "$key" "$row" <"$d.row"; fi
    rm -rf "$d"
    exit 10
  ) 9>"$d.lock"
  if [[ $? -eq 10 ]]; then
    if [[ -n "${RS_STREAM:-}" ]]; then
      local -a tiles=()
      mapfile -t tiles < <(awk '$1 == "tile:" && $3 ~ /-vk\.png$/ { print $3 }' "$d.row")
      rs_stream_tiles "" "${tiles[@]}"
    fi
    rm -f "$d.row"
  fi
  return 0
}

# Stamp a row made of whole files of the output directory (the credits leg).
# args: kind key row file...
rs_files_done() {
  (( ${RS_ON:-0} )) || return 0
  local kind="$1" key="$2" row="$3" t; shift 3
  for t in "$@"; do rs_tile_line "$t"; done | rs_write_stamp "$kind" "$key" "$row"
}

# Trim a manifest field. args: text
rs_trim() { sed 's/^[[:space:]]*//; s/[[:space:]]*$//' <<<"$1"; }

# Print why a stamp of the previous attempt cannot be imported; print nothing
# when it can. args: stamp prev-dir bk2-filter freecam-filter [off-kinds]
# (off-kinds: space-separated kinds the run selects no row of)
rs_check_stamp() {
  local s="$1" prev="$2" filter="$3" ffilter="$4" off=" ${5:-} "
  local base="${1##*/}"; base="${base%.stamp}"
  local kind="${base%%.*}" key="${base#*.}"
  local run="" tree="" bin="" params="" row="" line k sha name ntiles=0 manifest="" rname pose
  while IFS= read -r line; do
    case "$line" in
      "run: "*) run="${line#run: }";;
      "tree: "*) tree="${line#tree: }";;
      "binary: "*) bin="${line#binary: }";;
      "params: "*) params="${line#params: }";;
      "row: "*) row="${line#row: }";;
      "tile: "*)
        read -r k sha name <<<"$line"
        if [[ -z "$name" || "$name" == */* || "$name" == .* ]]; then echo "malformed tile line '$line'"; return; fi
        if [[ ! -f "$prev/$name" ]]; then echo "tile $name absent"; return; fi
        if [[ "$(sha256sum <"$prev/$name" | cut -d' ' -f1)" != "$sha" ]]; then echo "tile $name digest differs from its stamp"; return; fi
        ntiles=$((ntiles + 1));;
    esac
  done <"$s"
  [[ "$run" == "$RS_RUN" ]] || { echo "run '$run', this attempt's is $RS_RUN"; return; }
  [[ "$tree" == "$RS_TREE" ]] || { echo "tree ${tree:0:12}, this attempt's ${RS_TREE:0:12}"; return; }
  [[ "$bin" == "$RS_BIN" ]] || { echo "binary ${bin:0:12}, this attempt's ${RS_BIN:0:12}"; return; }
  [[ "$params" == "$(rs_params "$kind" "$RS_AFFINE")" ]] || { echo "params '$params' differ from '$(rs_params "$kind" "$RS_AFFINE")'"; return; }
  (( ntiles > 0 )) || { echo "no tile"; return; }
  [[ "$off" == *" $kind "* ]] && { echo "the filters select no $kind row"; return; }
  # A repeat copy's key is its row's key and `~r<k>` (pg_copy_keys): it
  # imports only into a repeat run whose copies include k, and a row's own
  # key only into a run without repeat.
  local rkey="$key"
  if [[ "$kind" != credits && "$key" =~ ^(.+)~r([0-9]+)$ ]]; then
    rkey="${BASH_REMATCH[1]}"
    (( ${PG_REPEAT:-0} > 0 && 10#${BASH_REMATCH[2]} >= 1 && 10#${BASH_REMATCH[2]} <= ${PG_REPEAT:-0} )) \
      || { echo "repeat copy ${BASH_REMATCH[2]} outside this run's repeat ${PG_REPEAT:-0}"; return; }
  elif [[ "$kind" != credits ]] && (( ${PG_REPEAT:-0} > 0 )); then
    echo "not a repeat copy, and this run repeats ${PG_REPEAT}"; return
  fi
  case "$kind" in
    bk2)
      manifest="$PG_CORPUS_MANIFEST"
      rname="$(tr -d ' \t' <<<"${row%%|*}")"
      [[ "$rname" == "$rkey" ]] || { echo "row names $rname, the stamp $key"; return; }
      pg_row_selected "$filter" "$rname" || { echo "row outside the filter '$filter'"; return; };;
    freecam)
      manifest="$PG_FREECAM_MANIFEST"
      [[ -n "$manifest" ]] || { echo "the tier has no freecam manifest"; return; }
      rname="$(tr -d ' \t' <<<"${row%%|*}")"
      pose="${row#*|}"; pose="$(rs_trim "${pose%%|*}")"
      [[ "$(pg_pose_slug "$rname" "$pose")" == "$rkey" ]] || { echo "row keys $(pg_pose_slug "$rname" "$pose"), the stamp $key"; return; }
      pg_row_selected "$ffilter" "$rname" "$rkey" || { echo "row outside the freecam filter '$ffilter'"; return; };;
    credits)
      [[ "$key" == credits && "$row" == credits-interp && "$PG_TIER" == presentation ]] \
        || { echo "not this tier's credits leg"; return; }
      return;;
    *) echo "unknown kind $kind"; return;;
  esac
  grep -Fxq -- "$row" "$manifest" || { echo "row line is not a line of $manifest"; return; }
}

# Set the previous attempt's output aside, move its importable rows into a
# fresh outdir and delete the rest. args: outdir bk2-filter freecam-filter
# [off-kinds] (rs_check_stamp)
rs_import() {
  local out="$1" prev="$1.resume-prev" s why line k sha name rows=0 png=0 files n p
  local -a streamed=()
  # A previous import that was itself interrupted left prev beside a partial
  # outdir: the rows already moved are dropped and captured again.
  if [[ -d "$prev" ]]; then rm -rf "$out"; elif [[ -d "$out" ]]; then mv "$out" "$prev"; fi
  mkdir -p "$out/resume"
  : >"$out/resume/imported.tsv"
  for s in "$prev"/resume/*.stamp; do
    [[ -e "$s" ]] || continue
    why="$(rs_check_stamp "$s" "$prev" "$2" "$3" "${4:-}")"
    if [[ -n "$why" ]]; then echo "[resume] not imported: ${s##*/}: $why"; continue; fi
    n=0 p=0
    while IFS= read -r line; do
      case "$line" in
        "tile: "*)
          read -r k sha name <<<"$line"
          mv -f "$prev/$name" "$out/$name"
          [[ "$name" == *-vk.png ]] && streamed+=("$name")
          n=$((n + 1)); [[ "$name" == *-vk.png ]] && p=$((p + 1));;
        "device: "*) echo "${line#device: }" >>"$out/capture-device.txt";;
      esac
    done <"$s"
    mv -f "$s" "$out/resume/"
    files="${s##*/}"; files="${files%.stamp}"
    printf '%s\t%s\t%s\t%s\n' "${files%%.*}" "${files#*.}" "$p" "$n" >>"$out/resume/imported.tsv"
    rows=$((rows + 1)) png=$((png + p))
  done
  rm -rf "$prev"
  if [[ -n "${RS_STREAM:-}" ]]; then
    rs_stream_tiles imported "${streamed[@]}"
    [[ -f "$out/resume/credits.credits.stamp" && -f "$out/credits-verdict.txt" ]] \
      && rs_stream_credits "$(head -n 1 "$out/credits-verdict.txt" | sed 's/^rc=//')" imported
  fi
  echo "[resume] run $RS_RUN: imported $rows row(s), $png tile(s) from the previous attempt; the rest of its output deleted"
}

# Imported counts of an output directory: RS_IMPORTED_ROWS rows and
# RS_IMPORTED_PNG -vk.png tiles. args: outdir
rs_census() {
  RS_IMPORTED_ROWS=0 RS_IMPORTED_PNG=0
  [[ -f "$1/resume/imported.tsv" ]] || return 0
  read -r RS_IMPORTED_ROWS RS_IMPORTED_PNG < <(awk -F'\t' 'NF >= 4 { r++; p += $3 } END { print r + 0, p + 0 }' "$1/resume/imported.tsv")
}
