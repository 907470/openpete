#!/usr/bin/env python3
"""Calibration features of one terrain vertex-stage ISA (amdllpc objdump or
RADV ACO hw-level print). Usage: isa_calib.py <isa file(.gz)>... -> TSV row each.

Regions are located by constants, not names:
  mac   : the /4096 of mac_of (amdllpc: v_ldexp_f64 .., -12; ACO: v_mul_f64 by
          the SGPR constant 2^-12). Feature: opcode that defines the dividend
          (v_add_f64 = sum kept as mul+add; v_fma_f64 = contracted).
  div64 : f64 divisions. amdllpc: IEEE sequence (v_div_scale/v_rcp/v_fma/
          v_div_fmas/v_div_fixup); ACO: drcp lowering (v_cvt_f32_f64 ->
          v_rcp_f32 -> v_cvt_f64_f32 -> Newton fma steps) then v_mul_f64.
          Feature: count and shape (fma steps per reciprocal).
  rcp32 : f32 reciprocals not part of a drcp; consumer opcode (v_mul_f32 =
          rcp+mul fdiv) and IEEE f32 division sequences (v_div_fixup_f32).
  clip  : the full-extent arm of clip_of, -1 + nx*2 and 1 - ny*2: f32 ops
          carrying the constants 2.0/-1.0 or -2.0/1.0, fused (v_fma) or split.
  fma   : f32 fma total, f64 fma total, f64 fma outside division sequences.
"""
import gzip, re, sys, collections

def readlines(p):
    op = gzip.open if p.endswith(".gz") else open
    with op(p, "rt", errors="replace") as fh:
        return fh.read().splitlines()

ACO_RE = re.compile(r"^\s+(?P<lhs>.*?)\s=\s(?P<op>[svp]_[a-z0-9_]+)\s*(?P<rhs>.*)$")
LLPC_RE = re.compile(r"^\s+(?P<op>[sv]_[a-z0-9_]+)\s+(?P<ops>[^/]*?)\s*(//.*)?$")

def parse(lines):  # amdllpc objdump or ACO
    aco = any("ACO shader stage" in l for l in lines[:5])
    ins = []; cur_fn = None
    for l in lines:
        if aco:
            m = ACO_RE.match(l)
            if not m: continue
            lhs = m.group("lhs"); rhs = m.group("rhs")
            dst = [t for t in re.findall(r"%(\d+):", lhs) if t != "0"]
            dreg = re.findall(r":([sv]\[[0-9-]+\])", lhs)
            srcs = [s.strip() for s in rhs.split(",")] if rhs else []
            ins.append(dict(op=m.group("op"), dst=dst, dreg=dreg, srcs=srcs, precise="(precise)" in lhs))
        else:
            fm = re.match(r"^[0-9a-f]+ <(\S+)>:", l)
            if fm: cur_fn = fm.group(1); continue
            if cur_fn is None or cur_fn.startswith("_amdgpu_ps"): continue
            m = LLPC_RE.match(l)
            if not m: continue
            ops = [s.strip() for s in m.group("ops").split(",")] if m.group("ops").strip() else []
            ins.append(dict(op=m.group("op"), dst=ops[:1], dreg=ops[:1], srcs=ops[1:], precise=False))
    return aco, ins

def regs(tok):
    """amdllpc register operand -> set of (kind, index)."""
    tok = tok.lstrip("-|").rstrip("|")
    m = re.match(r"^([sv])\[(\d+):(\d+)\]$", tok)
    if m: return {(m.group(1), i) for i in range(int(m.group(2)), int(m.group(3)) + 1)}
    m = re.match(r"^([sv])(\d+)$", tok)
    if m: return {(m.group(1), int(m.group(2)))}
    return set()

def tid(tok):
    m = re.search(r"%(\d+):", tok)
    return m.group(1) if m and m.group(1) != "0" else None

def brev64(v):
    return int("{:064b}".format(v)[::-1], 2)

def f64_of(bits):
    import struct
    return struct.unpack("<d", struct.pack("<Q", bits))[0]

ANV_RE = re.compile(r"^(?:\(W\))?\s*(?:\([+-]?f\d+(?:\.\d+)?(?:\.[a-z]+)?\)\s*)?(?P<op>[a-z][a-z0-9_.]*)(?:\.[a-z]+)?\s*\((?P<w>\d+)\|?[^)]*\)\s+(?P<rest>.*)$")

def anv_features(lines):
    """INTEL_DEBUG asm: dst first, sources after; types as :df/:f suffixes."""
    ins = []
    for l in lines:
        m = ANV_RE.match(l.strip())
        if not m: continue
        rest = re.sub(r"\{[^}]*\}", "", m.group("rest")).split()
        if not rest: continue
        dst, srcs = rest[0], rest[1:]
        t = re.search(r":([a-z]+)$", dst)
        ins.append(dict(op=m.group("op"), ty=t.group(1) if t else "", dst=dst, srcs=srcs))
    base = lambda tok: re.sub(r"^-", "", tok).split("<")[0].split(":")[0]
    f = collections.OrderedDict(); f["compiler"] = "anv"
    regconst = {}; mac = collections.Counter(); drcp = 0; rc = collections.Counter()
    lastdef = {}
    for i, x in enumerate(ins):
        if x["op"] == "mov" and x["srcs"] and x["srcs"][0].lower().startswith("0x3f30000000000000"):
            regconst[base(x["dst"])] = 2.0 ** -12
        elif base(x["dst"]) in regconst and not (x["op"] == "mul" and x["ty"] == "df" and
                                                 any(base(s) in regconst for s in x["srcs"])):
            regconst.pop(base(x["dst"]), None)
        if x["op"] == "mul" and x["ty"] == "df":
            cs = [s for s in x["srcs"] if base(s) in regconst]
            if cs:
                other = [s for s in x["srcs"] if s not in cs]
                d = lastdef.get(base(other[0])) if other else None
                mac[(ins[d]["op"] + "." + ins[d]["ty"]) if d is not None else "?"] += 1
        if x["op"] == "math.inv" and x["ty"] == "f":
            d = lastdef.get(base(x["srcs"][0]))
            if d is not None and ins[d]["op"] == "mov" and any(s.endswith(":df") for s in ins[d]["srcs"]):
                drcp += 1
            else:
                users = [ins[j]["op"] + "." + ins[j]["ty"] for j in range(i + 1, min(len(ins), i + 40))
                         if any(base(s) == base(x["dst"]) for s in ins[j]["srcs"])][:1]
                rc[users[0] if users else "-"] += 1
        if not (x["op"] == "mov" and x["srcs"] and x["srcs"][0].lower().startswith("0x3f30000000000000")):
            regconst.pop(base(x["dst"]), None)
        lastdef[base(x["dst"])] = i
    opc = collections.Counter(x["op"] + "." + x["ty"] for x in ins)
    f["mac_n"] = sum(mac.values()); f["mac_def"] = ";".join(f"{k}={v}" for k, v in sorted(mac.items()))
    f["div64_n"] = drcp; f["div64_shape"] = f"drcp(math.inv.f from df)={drcp}"
    f["rcp32_n"] = sum(rc.values()); f["rcp32_use"] = ";".join(f"{k}={v}" for k, v in sorted(rc.items()))
    f["clip"] = "n/a (constants promoted to registers)"
    f["fma32"] = opc["mad.f"]; f["fma64"] = opc["mad.df"]; f["fma64_nondiv"] = "-"
    f["mul64"] = opc["mul.df"]; f["add64"] = opc["add.df"]; f["n_insts"] = len(ins)
    return f

def features(path):
    raw = readlines(path)
    if any(l.startswith("Native code for") for l in raw[:3]):
        return anv_features(raw)
    aco, ins = parse(raw)
    f = collections.OrderedDict()
    f["compiler"] = "aco" if aco else "llpc"
    defs = {}; uses = collections.defaultdict(list); const = {}; regconst = {}
    if aco:
        for i, x in enumerate(ins):
            for d in x["dst"]: defs[d] = i
            for s in x["srcs"]:
                t = tid(s)
                if t: uses[t].append(i)
            val = None
            if x["op"] == "s_brev_b64" and x["srcs"]:
                m = re.match(r"^(0x[0-9a-f]+|\d+)$", x["srcs"][0])
                if m: val = f64_of(brev64(int(m.group(1), 0)))
            if x["op"] == "s_mov_b64" and x["srcs"]:
                try: val = float(x["srcs"][0])
                except ValueError: pass
            for r in x["dreg"]: regconst.pop(r, None)
            if val is not None and x["dreg"]: regconst[x["dreg"][0]] = val
            for s in x["srcs"]:
                t = tid(s); r = re.search(r":([sv]\[[0-9-]+\])", s)
                if t and r and r.group(1) in regconst and t not in defs: const[t] = regconst[r.group(1)]
    def def_of_aco(tok):
        t = tid(tok)
        return ins[defs[t]] if t in defs else None
    def def_of_llpc(i, tok):
        want = regs(tok)
        for j in range(i - 1, max(-1, i - 400), -1):
            d = ins[j]["dreg"][0] if ins[j]["dreg"] else ""
            if regs(d) & want: return ins[j]
        return None
    # mac anchor
    mac = collections.Counter()
    for i, x in enumerate(ins):
        if not aco and x["op"].startswith("v_ldexp_f64") and x["srcs"] and x["srcs"][-1] == "-12":
            d = def_of_llpc(i, x["srcs"][0]); mac[d["op"] if d else "?"] += 1
        if aco and x["op"].startswith("v_mul_f64"):
            cs = [s for s in x["srcs"] if tid(s) in const and const[tid(s)] == 2.0 ** -12]
            if cs:
                other = [s for s in x["srcs"] if s not in cs][0]
                d = def_of_aco(other); mac[re.sub(r"_e64$", "", d["op"]) if d else "?"] += 1
    f["mac_n"] = sum(mac.values())
    f["mac_def"] = ";".join(f"{k}={v}" for k, v in sorted(mac.items()))
    opc = collections.Counter(re.sub(r"_e(32|64)$", "", x["op"]) for x in ins)
    # f64 division
    if aco:
        shapes = collections.Counter(); drcp = set()
        for i, x in enumerate(ins):
            if x["op"] == "v_rcp_f32":
                d = def_of_aco(x["srcs"][0])
                if d and d["op"] == "v_cvt_f32_f64":
                    drcp.add(i)
                    # walk the Newton chain: fma uses until a non-fma
                    n = 0; frontier = list(x["dst"]); seen = set()
                    for _ in range(12):
                        nxt = []
                        for t in frontier:
                            for u in uses.get(t, []):
                                if u in seen: continue
                                seen.add(u)
                                if ins[u]["op"] in ("v_fma_f64", "v_cvt_f64_f32", "v_bfi_b32", "v_cndmask_b32", "v_mov_b32", "p_create_vector", "p_split_vector", "p_parallelcopy"):
                                    if ins[u]["op"] == "v_fma_f64": n += 1
                                    nxt += ins[u]["dst"]
                        frontier = nxt
                    shapes[n] += 1
        # Newton step = fma(r, b, -1.0) then fma(-e, r, r): count the -1.0 fmas
        nr = sum(1 for x in ins if x["op"] == "v_fma_f64" and any(tid(s) in const and const[tid(s)] == -1.0 for s in x["srcs"]))
        f["div64_n"] = len(drcp)
        f["div64_shape"] = f"drcp:newton_fma_pairs={nr}" + (f",per_rcp={nr/len(drcp):.2f}" if drcp else "")
        shapes = collections.Counter({2: nr})
        rc = collections.Counter()
        for i, x in enumerate(ins):
            if x["op"] == "v_rcp_f32" and i not in drcp:
                us = [re.sub(r"_e(32|64)$", "", ins[u]["op"]) for t in x["dst"] for u in uses.get(t, [])]
                rc[",".join(sorted(set(us))) or "-"] += 1
        f["rcp32_n"] = sum(rc.values())
        f["rcp32_use"] = ";".join(f"{k}={v}" for k, v in sorted(rc.items()))
        fma64_div = 2 * nr
    else:
        f["div64_n"] = opc["v_div_fixup_f64"]
        f["div64_shape"] = f"ieee:scale={opc['v_div_scale_f64']},rcp={opc['v_rcp_f64']},fmas={opc['v_div_fmas_f64']},fixup={opc['v_div_fixup_f64']}"
        f["rcp32_n"] = opc["v_rcp_f32"]
        f["rcp32_use"] = f"ieee32_fixup={opc['v_div_fixup_f32']},rcp_iflag={opc['v_rcp_iflag_f32']}"
        fma64_div = 5 * opc["v_div_fixup_f64"]   # 5 fma per IEEE f64 divide (checked against div_fmas pairing)
    # clip_of full-extent arm
    clip = collections.Counter()
    for x in ins:
        if not re.match(r"v_(fma|fmac|mul|add|sub|subrev|fmaak|fmamk)_f32", x["op"]): continue
        s = " ".join(x["srcs"])
        has2 = re.search(r"(^|[\s,])-?2\.0($|[\s,])", s); has1 = re.search(r"(^|[\s,])-?1\.0($|[\s,])", s)
        if has2 and has1 and "fma" in x["op"]: clip["fused"] += 1
        elif has2 and re.match(r"v_mul", x["op"]): clip["mul2"] += 1
        elif has1 and re.match(r"v_(add|sub)", x["op"]): clip["add1"] += 1
    f["clip"] = ";".join(f"{k}={v}" for k, v in sorted(clip.items()))
    f["fma32"] = sum(opc[k] for k in ("v_fma_f32", "v_fmac_f32", "v_fmaak_f32", "v_fmamk_f32"))
    f["fma64"] = opc["v_fma_f64"]
    f["fma64_nondiv"] = opc["v_fma_f64"] - fma64_div
    f["mul64"] = opc["v_mul_f64"]; f["add64"] = opc["v_add_f64"]
    f["n_insts"] = len(ins)
    return f

if __name__ == "__main__":
    first = True
    for p in sys.argv[1:]:
        f = features(p)
        if first: print("file\t" + "\t".join(f.keys())); first = False
        print(p + "\t" + "\t".join(str(v) for v in f.values()))
