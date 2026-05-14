#!/usr/bin/env python3
"""Analyze per-vertex stability trace (OPENPETE_VERT_STABILITY_LOG output).

Answers the critical question for interpolation-based frame gen:
**Does GTE emit vertices in stable order per moby across game frames?**

Method:
  1. Group records by (frame, matrix_span_id) -> ordered list of v_local.
  2. For each batch in frame F, compute a "model fingerprint" = sorted
     tuple of v_local vectors. Same fingerprint = same logical mesh.
  3. Find batches in frame F+1 with matching fingerprint.
  4. For uniquely-identifiable pairs (one F batch, one F+1 batch with
     same fingerprint), compare v_local sequences ORDER-WISE.
  5. Report % of pairs with stable order vs unstable.

If stable: per-vertex interpolation can use (frame, span_id, vert_idx)
as identity. If unstable: need a recompiler-level vertex tag.
"""
import argparse
import struct
import sys
from collections import defaultdict

HEADER_FMT = "<III20x"  # magic, version, record_sz, 20 reserved
HEADER_SZ = struct.calcsize(HEADER_FMT)
assert HEADER_SZ == 32

# struct openpete_vstab_record_t — 32 bytes:
#   uint32 frame, uint32 span_id, uint32 vert_idx,
#   int16 v_local[3] + int16 pad,
#   int32 mac[3]
RECORD_FMT = "<IIIhhhhiii"
RECORD_SZ = struct.calcsize(RECORD_FMT)
assert RECORD_SZ == 32, RECORD_SZ


def read_records(path):
    with open(path, "rb") as f:
        hdr = f.read(HEADER_SZ)
        magic, version, record_sz = struct.unpack(HEADER_FMT, hdr)
        if magic != 0x56535442:
            sys.exit(f"bad magic 0x{magic:08x} (expected 0x56535442 'VSTB')")
        if record_sz != RECORD_SZ:
            sys.exit(f"record_sz mismatch: file={record_sz} script={RECORD_SZ}")
        while True:
            chunk = f.read(RECORD_SZ)
            if len(chunk) < RECORD_SZ:
                return
            yield struct.unpack(RECORD_FMT, chunk)


def fmt(n):
    if n >= 1_000_000: return f"{n/1_000_000:.2f}M"
    if n >= 1_000: return f"{n/1_000:.1f}k"
    return str(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--show-unstable", type=int, default=0,
                    help="dump first N unstable batch pairs")
    args = ap.parse_args()

    # batches[frame][span_id] = list of (vert_idx, v_local_tuple, mac_tuple)
    batches = defaultdict(lambda: defaultdict(list))
    for rec in read_records(args.path):
        frame, span_id, vert_idx, vx, vy, vz, _pad, mx, my, mz = rec
        batches[frame][span_id].append((vert_idx, (vx, vy, vz), (mx, my, mz)))

    frames = sorted(batches.keys())
    n_records = sum(len(b) for f in batches.values() for b in f.values())
    n_batches = sum(len(f) for f in batches.values())
    print(f"\n=== {args.path} ===")
    print(f"frames captured : {len(frames)} ({frames[0]}..{frames[-1]})")
    print(f"total batches   : {fmt(n_batches)}")
    print(f"total vertices  : {fmt(n_records)}")
    if not frames or len(frames) < 2:
        sys.exit("need ≥2 frames for stability analysis")

    # Build per-frame fingerprint -> [(span_id, ordered_v_local_list)] index.
    # Fingerprint = sorted tuple of v_local — order-independent identity.
    print(f"\n=== per-frame fingerprint count distribution ===")
    fp_counts_per_frame = []
    for frame in frames:
        fp_counts_per_frame.append(len(batches[frame]))
    print(f"batches/frame: min={min(fp_counts_per_frame)} "
          f"median={sorted(fp_counts_per_frame)[len(fp_counts_per_frame)//2]} "
          f"max={max(fp_counts_per_frame)} "
          f"mean={sum(fp_counts_per_frame)/len(fp_counts_per_frame):.1f}")

    # For each frame, group batches by their model fingerprint.
    def frame_fingerprints(frame):
        result = {}  # fingerprint -> list of (span_id, ordered_v_local)
        for span_id, verts in batches[frame].items():
            verts_sorted = sorted(verts, key=lambda x: x[0])  # by vert_idx
            ordered_v_local = tuple(v[1] for v in verts_sorted)
            fp = tuple(sorted(ordered_v_local))
            result.setdefault(fp, []).append((span_id, ordered_v_local))
        return result

    # Pair frames and check stability
    pairs_total = 0
    pairs_stable = 0
    pairs_unstable = 0
    pairs_skipped_ambiguous = 0
    pairs_skipped_new = 0
    pairs_skipped_dropped = 0
    unstable_examples = []
    vert_count_changes = 0

    # Also: how often does a fingerprint persist across many frames?
    fp_persistence = defaultdict(int)  # fingerprint -> count of frames present

    prev_fps = None
    for fi, frame in enumerate(frames):
        cur_fps = frame_fingerprints(frame)
        for fp in cur_fps:
            fp_persistence[fp] += 1

        if prev_fps is not None:
            cur_fp_set = set(cur_fps.keys())
            prev_fp_set = set(prev_fps.keys())

            # Track spawn/despawn
            pairs_skipped_new += len(cur_fp_set - prev_fp_set)
            pairs_skipped_dropped += len(prev_fp_set - cur_fp_set)

            # Pair fingerprints present in both
            for fp in cur_fp_set & prev_fp_set:
                prev_list = prev_fps[fp]
                cur_list = cur_fps[fp]
                # Skip ambiguous (multiple batches with same fingerprint)
                if len(prev_list) != 1 or len(cur_list) != 1:
                    pairs_skipped_ambiguous += 1
                    continue
                prev_order = prev_list[0][1]
                cur_order = cur_list[0][1]
                pairs_total += 1
                if prev_order == cur_order:
                    pairs_stable += 1
                else:
                    pairs_unstable += 1
                    if len(unstable_examples) < args.show_unstable:
                        unstable_examples.append({
                            "frame_prev": frames[fi-1],
                            "frame_cur":  frame,
                            "n_verts":    len(prev_order),
                            "prev_first3": prev_order[:3],
                            "cur_first3":  cur_order[:3],
                        })
        prev_fps = cur_fps

    print(f"\n=== batch pairing across consecutive frames ===")
    print(f"unique-fingerprint pairs: {fmt(pairs_total)}")
    print(f"  ORDER STABLE          : {fmt(pairs_stable)} "
          f"({100*pairs_stable/max(pairs_total,1):.2f}%)")
    print(f"  ORDER UNSTABLE        : {fmt(pairs_unstable)} "
          f"({100*pairs_unstable/max(pairs_total,1):.2f}%)")
    print(f"ambiguous (multi-instance same fp): {fmt(pairs_skipped_ambiguous)}")
    print(f"newly spawned (in cur, not prev) : {fmt(pairs_skipped_new)}")
    print(f"despawned (in prev, not cur)     : {fmt(pairs_skipped_dropped)}")

    # Fingerprint persistence histogram
    persist_buckets = defaultdict(int)
    for n in fp_persistence.values():
        persist_buckets[n] += 1
    print(f"\n=== fingerprint persistence (across N frames) ===")
    print(f"unique fingerprints total: {fmt(len(fp_persistence))}")
    for n in sorted(persist_buckets.keys())[:20]:
        print(f"  persisted {n:>3} frames: {fmt(persist_buckets[n])} unique fps")
    if len(persist_buckets) > 20:
        print(f"  ... ({len(persist_buckets)-20} more buckets)")

    # Show unstable examples if requested
    if unstable_examples:
        print(f"\n=== unstable order examples ===")
        for ex in unstable_examples:
            print(f"  frame {ex['frame_prev']} → {ex['frame_cur']}, n_verts={ex['n_verts']}")
            print(f"    prev first 3: {ex['prev_first3']}")
            print(f"    cur first 3 : {ex['cur_first3']}")

    # Final verdict
    print(f"\n=== verdict ===")
    if pairs_total == 0:
        print("INCONCLUSIVE — no unique-fingerprint pairs found.")
    elif pairs_stable / pairs_total > 0.99:
        print(f"STABLE ({100*pairs_stable/pairs_total:.2f}%) — "
              f"GTE emits vertices in stable order per moby across frames.")
        print(f"  ✓ (frame, span_id, vert_idx) is a viable per-vertex identity")
        print(f"  ✓ interpolation can proceed without recompiler vertex tagging")
    elif pairs_stable / pairs_total > 0.80:
        print(f"MOSTLY STABLE ({100*pairs_stable/pairs_total:.2f}%) — "
              f"order mostly preserved, some classes shuffle.")
        print(f"  → investigate which batches are unstable (--show-unstable N)")
    else:
        print(f"UNSTABLE ({100*pairs_stable/pairs_total:.2f}%) — "
              f"order changes frame-to-frame.")
        print(f"  ✗ per-vertex interpolation needs recompiler-level vertex identity")


if __name__ == "__main__":
    main()
