#!/usr/bin/env python3
"""Analyze --vertex-trace-log binary output.

Layout: 32-byte header, then N fixed-size batch records. See
openpete_vertex_trace.h for field definitions.

Usage:
    scripts/analyze-vtrace.py <path.bin> [--object N] [--limit N]
"""
import argparse
import struct
import sys
from collections import defaultdict

HEADER_FMT = "<III20x"  # magic, version, record_sz, 20 reserved
HEADER_SZ = struct.calcsize(HEADER_FMT)
assert HEADER_SZ == 32

# struct openpete_vtrace_batch_t — 72 bytes
RECORD_FMT = (
    "<"      # little-endian
    "q"      # op_now_ns                int64
    "I"      # openpete_frame           uint32
    "I"      # render_idx               uint32
    "f"      # alpha                    float
    "I"      # batch_idx                uint32
    "I"      # batch_count              uint32
    "I"      # matrix_span_id           uint32
    "I"      # object_id                uint32
    "iii"    # centroid[3]              int32
    "ii"     # sxy_centroid[2]          int32  (= "from" centroid)
    "ii"     # delta_sxy[2]             int32  (interpolation span to-from)
    "H"      # vertex_count             uint16
    "B"      # iter_kind                uint8
    "B"      # is_ui_opt_out            uint8
    "B"      # matched                  uint8
    "B"      # velocity_capped          uint8
    "H"      # _pad                     uint16
)
RECORD_SZ = struct.calcsize(RECORD_FMT)
assert RECORD_SZ == 72, RECORD_SZ


def read_records(path):
    with open(path, "rb") as f:
        hdr = f.read(HEADER_SZ)
        magic, version, record_sz = struct.unpack(HEADER_FMT, hdr)
        if magic != 0x56545243:
            sys.exit(f"bad magic 0x{magic:08x} (expected 0x56545243 'VTRC')")
        if version != 2:
            sys.exit(f"vtrace version {version} not supported — this script "
                     f"reads v2 (delta_sxy / interpolation model). Re-capture "
                     f"the trace with the current build.")
        if record_sz != RECORD_SZ:
            sys.exit(f"record_sz mismatch: file={record_sz} script={RECORD_SZ}")
        while True:
            chunk = f.read(RECORD_SZ)
            if len(chunk) < RECORD_SZ:
                return
            yield struct.unpack(RECORD_FMT, chunk)


def fmt(n):
    if n >= 1_000_000:
        return f"{n/1_000_000:.2f}M"
    if n >= 1_000:
        return f"{n/1_000:.1f}k"
    return str(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--object", type=int, default=None,
                    help="dump per-render trajectory for this object_id")
    ap.add_argument("--limit", type=int, default=None,
                    help="stop after N records (debugging)")
    ap.add_argument("--clamp-px", type=float, default=None,
                    help="if set, clamp the reconstructed applied delta "
                         "per-axis to [-N, N] before measuring jumps "
                         "(matches the in-engine clamp; use to test "
                         "what numbers would look like with that cap)")
    args = ap.parse_args()

    def apply_clamp(dx, dy):
        if args.clamp_px is None:
            return dx, dy
        c = args.clamp_px
        return (max(-c, min(c, dx)), max(-c, min(c, dy)))

    n = 0
    by_iter_kind = defaultdict(int)
    by_match = defaultdict(int)
    capped = 0
    ui_opt_out = 0
    renders = set()
    obj_renders = defaultdict(set)  # object_id -> set(render_idx)
    obj_positions = defaultdict(list)  # object_id -> [(render_idx, alpha, sx, sy, predicted_sx, predicted_sy, matched, iter_kind)]
    per_render_alpha_minmax = defaultdict(lambda: [None, None])  # render_idx -> [min,max] alpha
    sim_due_render_idxs = set()

    for rec in read_records(args.path):
        if args.limit and n >= args.limit:
            break
        n += 1
        (op_now_ns, frame, render_idx, alpha, batch_idx, batch_count,
         matrix_span_id, object_id,
         cx, cy, cz, sx, sy, dsx, dsy,
         vc, iter_kind, is_ui, matched, vcapped, _pad) = rec

        renders.add(render_idx)
        if iter_kind == 0:
            sim_due_render_idxs.add(render_idx)
        by_iter_kind[iter_kind] += 1
        by_match[matched] += 1
        if vcapped: capped += 1
        if is_ui: ui_opt_out += 1

        # Reconstruct the in-engine applied position. Interpolation
        # model: applied = from + alpha * delta_sxy (delta is the KNOWN
        # [from→to] span, not a prediction).
        if matched and not is_ui:
            dx, dy = apply_clamp(dsx * alpha, dsy * alpha)
            applied_sx = sx + dx
            applied_sy = sy + dy
        else:
            applied_sx, applied_sy = sx, sy

        obj_renders[object_id].add(render_idx)
        if args.object is not None and object_id == args.object:
            obj_positions[object_id].append(
                (render_idx, frame, alpha, sx, sy, dsx, dsy,
                 applied_sx, applied_sy, matched, iter_kind, vcapped))

        cur_min, cur_max = per_render_alpha_minmax[render_idx]
        if cur_min is None or alpha < cur_min: per_render_alpha_minmax[render_idx][0] = alpha
        if cur_max is None or alpha > cur_max: per_render_alpha_minmax[render_idx][1] = alpha

    print(f"\n=== {args.path} ===")
    print(f"records       : {fmt(n)}")
    print(f"unique renders: {fmt(len(renders))}")
    print(f"  sim-due     : {fmt(len(sim_due_render_idxs))}")
    print(f"  intermediate: {fmt(len(renders) - len(sim_due_render_idxs))}")
    print(f"iter_kind     : sim-due={fmt(by_iter_kind[0])} intermediate={fmt(by_iter_kind[1])}")
    print(f"match         : matched={fmt(by_match[1])} ({100*by_match[1]/max(n,1):.1f}%) "
          f"unmatched={fmt(by_match[0])}")
    print(f"velocity-capped: {fmt(capped)} ({100*capped/max(n,1):.1f}%)")
    print(f"ui_opt_out    : {fmt(ui_opt_out)} ({100*ui_opt_out/max(n,1):.1f}%)")

    # Object stability: how many renders does each object_id persist?
    persistence = [len(rs) for rs in obj_renders.values()]
    if persistence:
        persistence.sort(reverse=True)
        print(f"\nobject_id stability (renders per object):")
        print(f"  unique object_ids: {fmt(len(persistence))}")
        print(f"  median renders   : {persistence[len(persistence)//2]}")
        print(f"  p90 renders      : {persistence[max(0, len(persistence)//10 - 1)]}")
        print(f"  max renders      : {persistence[0]}")
        single_render = sum(1 for p in persistence if p == 1)
        print(f"  one-shot ids     : {fmt(single_render)} ({100*single_render/len(persistence):.1f}%)")

    # Per-render alpha sanity: sim-due should be alpha≈0; intermediates should ramp.
    print("\nalpha-vs-iter sanity (first 8 renders):")
    print("  render_idx  min_alpha  max_alpha  (alpha varies WITHIN one render = bug)")
    for rid in sorted(renders)[:8]:
        amin, amax = per_render_alpha_minmax[rid]
        print(f"  {rid:>10}  {amin:>9.4f}  {amax:>9.4f}")

    # Sim-due vs intermediate alpha summary
    sim_alphas = []
    int_alphas = []
    for rec_idx, rid in enumerate(sorted(renders)):
        amin, amax = per_render_alpha_minmax[rid]
        if rid in sim_due_render_idxs:
            sim_alphas.append(amax)
        else:
            int_alphas.append(amax)
    if sim_alphas:
        print(f"\nsim-due alpha: mean={sum(sim_alphas)/len(sim_alphas):.4f} "
              f"max={max(sim_alphas):.4f}  (expect ≈ 0)")
    if int_alphas:
        print(f"intermediate alpha: mean={sum(int_alphas)/len(int_alphas):.4f} "
              f"max={max(int_alphas):.4f}  (expect spread across 0..1)")

    # If user asked for a specific object, dump its trajectory
    if args.object is not None and args.object in obj_positions:
        print(f"\n=== object_id={args.object} trajectory ===")
        print(f"  render | frame | alpha  | iter | matched | capped | from(sx,sy) Δ(dx,dy)")
        for r, frame, alpha, sx, sy, dsx, dsy, asx, asy, m, ik, vc in obj_positions[args.object]:
            ik_str = "sim" if ik == 0 else "int"
            print(f"  {r:>6} | {frame:>5} | {alpha:.3f} | {ik_str:>4} | "
                  f"{m:>7} | {vc:>6} | {sx:>4},{sy:>4} Δ {dsx:>4},{dsy:>4}  "
                  f"applied=({asx:.1f},{asy:.1f})")

    # JUMP DETECTION: for each matched batch with object_id stability,
    # measure the discontinuity between consecutive renders.
    # A healthy substitute should produce a smooth applied-position trajectory.
    print(f"\n=== jump detection (matched batches, consecutive renders) ===")
    jumps_by_threshold = defaultdict(int)
    total_consec = 0
    for oid, rids in obj_renders.items():
        if len(rids) < 2: continue
        # Re-scan trace to collect this object's applied positions
        # (For performance, only do this in a second pass — skip for now.)
    # Instead, sample: scan again with object-id tracking
    last_pos_by_oid = {}
    last_render_by_oid = {}
    n2 = 0
    big_jumps = []
    for rec in read_records(args.path):
        if args.limit and n2 >= args.limit:
            break
        n2 += 1
        (_, frame, render_idx, alpha, _, _, _, object_id,
         _, _, _, sx, sy, dsx, dsy,
         _, iter_kind, is_ui, matched, vcapped, _) = rec
        if is_ui or not matched:
            continue
        dx, dy = apply_clamp(dsx * alpha, dsy * alpha)
        applied_sx = sx + dx
        applied_sy = sy + dy
        if object_id in last_pos_by_oid:
            last_sx, last_sy = last_pos_by_oid[object_id]
            last_rid = last_render_by_oid[object_id]
            # Only flag jumps between consecutive renders for the same object
            if render_idx == last_rid + 1:
                dx = applied_sx - last_sx
                dy = applied_sy - last_sy
                d = abs(dx) + abs(dy)
                for th in (8, 16, 32, 64, 128, 256):
                    if d > th:
                        jumps_by_threshold[th] += 1
                if d > 64:
                    big_jumps.append((render_idx, frame, object_id, last_sx, last_sy,
                                      applied_sx, applied_sy, d))
                total_consec += 1
        last_pos_by_oid[object_id] = (applied_sx, applied_sy)
        last_render_by_oid[object_id] = render_idx

    if total_consec:
        print(f"  consecutive-render transitions (matched, non-UI): {fmt(total_consec)}")
        for th in (8, 16, 32, 64, 128, 256):
            n_j = jumps_by_threshold[th]
            print(f"  > {th:>3} px: {fmt(n_j)} ({100*n_j/total_consec:.2f}%)")

        big_jumps.sort(key=lambda x: -x[-1])
        print(f"\n  top 8 worst jumps (> 64 px):")
        for r, frame, oid, lsx, lsy, asx, asy, d in big_jumps[:8]:
            print(f"    render={r} frame={frame} obj={oid} "
                  f"({lsx:.0f},{lsy:.0f}) → ({asx:.0f},{asy:.0f})  dist={d:.1f}")


if __name__ == "__main__":
    main()
