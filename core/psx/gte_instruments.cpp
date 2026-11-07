// core/psx/gte_instruments.cpp - env-gated diagnostics around each GTE command.
//
// gte_execute (core/psx/gte.c) calls openpete_gte_instruments_op after
// every command while openpete_gte_instruments_on is nonzero; the first
// call reads the environment and clears the switch when nothing is armed,
// so an unarmed run pays one branch per command. C++ because the trace's
// second engine is PsyCross's GTE (gteRegs / GTE_operator), which has C++
// linkage.
//
//   OPENPETE_GTE_TRACE=PATH (+ _LO, _HI, _OPS)  CSV rows "rec" (gte.c) and
//       "psy" (PsyCross's GTE_operator on the same pre-state) per command
//       in an openpete_frame window; layout in
//       docs/src/developer/gte-trace-vs-reference.md.
//   OPENPETE_GTE_CENSUS=1  at exit: commands per function code, the
//       undocumented command words with their first caller PC, and the
//       register stores the game made between commands, by kind.
//   OPENPETE_RENDER_PROBE=1  RTPS/RTPT/MVMVA/other and rotation-change
//       counters, printed and reset by PsyCross per present.
//   OPENPETE_GTE_PCHIST=VBL  caller-PC histogram of one openpete_frame.
//   OPENPETE_RTPS_BOXLOG=x0,y0,x1,y1,VBL  caller PC of each RTPS/RTPT
//       vertex projected inside the box on that openpete_frame.
//   OPENPETE_OFX_CENSUS=1  per caller PC, the OFX range of RTPS/RTPT
//       commands whose OFX is not 256.0.
//   OPENPETE_GTE_RECORD=PATH[:OPS]  the entry register file of every
//       command whose function code is in OPS (hex list, default 01,30),
//       delta-coded: per command the u32 command field, a u64 mask (two
//       u32, low first) of the 64 registers that differ from the previous
//       record's entry file, then those registers' words in index order.
//       A replay feeds the records to any implementation on equal state.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "openpete_fiber.h"
#include "openpete_gte.h"
#include "psx_runtime_internal.h"

#include "psx/gtereg.h"
#include "openpete_thread_roles.h"

extern "C" {
int openpete_gte_instruments_on = -1;

/* Render probe counters; PsyCross prints and resets them
 * (PsyX_main.cpp) when g_openpete_render_probe_active is 1. */
int      g_openpete_render_probe_active = -1;
unsigned g_openpete_render_probe_count_rtps = 0u;
unsigned g_openpete_render_probe_count_rtpt = 0u;
unsigned g_openpete_render_probe_count_mvmva = 0u;
unsigned g_openpete_render_probe_count_other = 0u;
unsigned g_openpete_render_probe_count_matrix_changes = 0u;
}

namespace {

constexpr uint32_t kFnRtps = 0x01, kFnMvmva = 0x12, kFnRtpt = 0x30;

/** @brief Return the function code (bits 0-5) of a command word. */
uint32_t fn_of(uint32_t cmd) { return cmd & 0x3Fu; }

/** @brief Return 1 for RTPS and RTPT. */
int is_rtp(uint32_t fn) { return fn == kFnRtps || fn == kFnRtpt; }

/** @brief Return 1 when the environment variable is set to a nonempty value. */
int env_set(const char* name) {
    const char* e = std::getenv(name);
    return e && *e;
}

// --------------------------------------------------------------- record

struct Record {
    FILE*    fp = nullptr;
    uint64_t ops = 0;
    uint32_t last[64] = {};
} g_rec;

/** @brief Append one command's entry register file to the record, delta-coded against the previous record. */
void record_op(uint32_t cmd, const uint32_t* pre_d, const uint32_t* pre_c) {
    if (!((g_rec.ops >> (cmd & 0x3Fu)) & 1ull)) return;
    uint32_t words[64], out[64];
    std::memcpy(words, pre_d, 32 * sizeof(uint32_t));
    std::memcpy(words + 32, pre_c, 32 * sizeof(uint32_t));
    uint64_t mask = 0;
    unsigned n = 0;
    for (int i = 0; i < 64; i++) {
        if (words[i] == g_rec.last[i]) continue;
        mask |= 1ull << i;
        out[n++] = words[i];
    }
    const uint32_t head[3] = {cmd & 0x1FFFFFFu, (uint32_t)mask, (uint32_t)(mask >> 32)};
    std::fwrite(head, sizeof(uint32_t), 3, g_rec.fp);
    std::fwrite(out, sizeof(uint32_t), n, g_rec.fp);
    std::memcpy(g_rec.last, words, sizeof(words));
}

// ---------------------------------------------------------------- trace

struct Trace {
    FILE*    fp = nullptr;
    unsigned lo = 1100u, hi = 1111u;
    uint64_t ops = 0;
} g_trace;

/** @brief Parse a comma-separated hex list of function codes into a 64-bit mask. */
uint64_t parse_op_mask(const char* s) {
    uint64_t m = 0;
    while (s && *s) {
        char* end;
        const unsigned long v = std::strtoul(s, &end, 16);
        if (v < 64) m |= 1ull << v;
        if (*end != ',') break;
        s = end + 1;
    }
    return m;
}

/** @brief Open the trace CSV from OPENPETE_GTE_TRACE and its window knobs, writing the header; return 1 when armed. */
int trace_init() {
    const char* path = std::getenv("OPENPETE_GTE_TRACE");
    if (!path || !*path) return 0;
    if (const char* lo = std::getenv("OPENPETE_GTE_TRACE_LO")) g_trace.lo = (unsigned)std::strtoul(lo, nullptr, 0);
    if (const char* hi = std::getenv("OPENPETE_GTE_TRACE_HI")) g_trace.hi = (unsigned)std::strtoul(hi, nullptr, 0);
    g_trace.ops = parse_op_mask(std::getenv("OPENPETE_GTE_TRACE_OPS"));
    if (!g_trace.ops) g_trace.ops = (1ull << 0x01) | (1ull << 0x06) | (1ull << 0x30) | (1ull << 0x2D) | (1ull << 0x2E);
    g_trace.fp = std::fopen(path, "w");
    if (!g_trace.fp) return 0;
    std::fprintf(g_trace.fp,
                 "frame,engine,op,vidx,Vx,Vy,Vz,TRx,TRy,TRz,"
                 "RT00,RT01,RT02,RT10,RT11,RT12,RT20,RT21,RT22,"
                 "OFX,OFY,H,DQA,DQB,MAC0,MAC1,MAC2,MAC3,IR0,IR1,IR2,IR3,"
                 "SZ0,SZ1,SZ2,SZ3,SXY0,SXY1,SXY2,RGB0,RGB1,RGB2,FLAG,PC\n");
    std::fprintf(stdout, "[GTE-TRACE] writing to %s frames [%u,%u) ops=0x%llX\n", path, g_trace.lo, g_trace.hi,
                 (unsigned long long)g_trace.ops);
    std::fflush(stdout);
    return 1;
}

/** @brief Return the signed low halfword of a register word. */
int lo16(uint32_t w) { return (int16_t)(uint16_t)(w & 0xFFFFu); }

/** @brief Return the signed high halfword of a register word. */
int hi16(uint32_t w) { return (int16_t)(uint16_t)(w >> 16); }

/** @brief Write one CSV row decoding a post-command register file, with vertex vidx's input vector.
 * @param d Data registers.
 * @param c Control registers.
 * @param engine Row label ("rec" or "psy").
 * @param op Command label.
 * @param vidx Vertex index 0..2, or -1 for a command without one (V0 is written). */
void trace_row(const uint32_t* d, const uint32_t* c, const char* engine, const char* op, int vidx) {
    const int v = vidx < 0 ? 0 : vidx;
    std::fprintf(g_trace.fp, "%u,%s,%s,%d,%d,%d,%d,%d,%d,%d,", openpete_frame, engine, op, vidx, lo16(d[2 * v]),
                 hi16(d[2 * v]), lo16(d[2 * v + 1]), (int32_t)c[5], (int32_t)c[6], (int32_t)c[7]);
    std::fprintf(g_trace.fp, "%d,%d,%d,%d,%d,%d,%d,%d,%d,", lo16(c[0]), hi16(c[0]), lo16(c[1]), hi16(c[1]), lo16(c[2]),
                 hi16(c[2]), lo16(c[3]), hi16(c[3]), lo16(c[4]));
    std::fprintf(g_trace.fp, "%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,", (int32_t)c[24], (int32_t)c[25],
                 (unsigned)(c[26] & 0xFFFFu), lo16(c[27]), (int32_t)c[28], (int32_t)d[24], (int32_t)d[25],
                 (int32_t)d[26], (int32_t)d[27], lo16(d[8]), lo16(d[9]), lo16(d[10]), lo16(d[11]));
    std::fprintf(g_trace.fp, "%u,%u,%u,%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                 d[16] & 0xFFFFu, d[17] & 0xFFFFu, d[18] & 0xFFFFu, d[19] & 0xFFFFu, d[12], d[13], d[14], d[20], d[21],
                 d[22], c[31], openpete_gte_caller_pc);
}

/** @brief Return the trace label of a function code; *triple is set for the per-vertex commands RTPT, NCDT, NCT, NCCT. */
const char* trace_label(uint32_t fn, int* triple) {
    static const char* const k_names[64] = {
        nullptr, "RTPS", nullptr, nullptr, nullptr, nullptr, "NCLIP", nullptr, nullptr, nullptr, nullptr, nullptr,
        "OP", nullptr, nullptr, nullptr, "DPCS", "INTPL", "MVMVA", "NCDS", "CDP", nullptr, "NCDT", nullptr, nullptr,
        nullptr, nullptr, "NCCS", "CC", nullptr, "NCS", nullptr, "NCT", nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, "SQR", "DPCL", "DPCT", nullptr, nullptr, "AVSZ3", "AVSZ4", nullptr, "RTPT", nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "GPF", "GPL", "NCCT"};
    *triple = fn == 0x30 || fn == 0x16 || fn == 0x20 || fn == 0x3F;
    return k_names[fn] ? k_names[fn] : "UNK";
}

/** @brief Write the "rec" and "psy" row pairs of one command (one pair per vertex for the triple commands). */
void trace_rows(const uint32_t* rd, const uint32_t* rc, const uint32_t* pd, const uint32_t* pc, uint32_t fn) {
    int triple = 0;
    const char* name = trace_label(fn, &triple);
    const int single_v = (fn == 0x01 || fn == 0x13 || fn == 0x1B || fn == 0x1E) ? 0 : -1;
    char label[16];
    for (int v = 0; v < (triple ? 3 : 1); v++) {
        if (triple) std::snprintf(label, sizeof(label), "%s_v%d", name, v);
        else std::snprintf(label, sizeof(label), "%s", name);
        trace_row(rd, rc, "rec", label, triple ? v : single_v);
        trace_row(pd, pc, "psy", label, triple ? v : single_v);
    }
}

/** @brief Log one command in the trace window: gte.c's post-state and PsyCross's on the same pre-state; close the file past the window. */
void trace_op(const CPUState* cpu, uint32_t cmd, const uint32_t* pre_d, const uint32_t* pre_c) {
    if (!g_trace.fp || openpete_frame < g_trace.lo) return;
    if (openpete_frame >= g_trace.hi) {
        std::fclose(g_trace.fp);
        g_trace.fp = nullptr;
        std::fprintf(stdout, "[GTE-TRACE] window closed at frame %u\n", openpete_frame);
        std::fflush(stdout);
        return;
    }
    const uint32_t fn = fn_of(cmd);
    if (!((g_trace.ops >> fn) & 1ull)) return;
    unsigned int saved_d[32], saved_c[32], psy_d[32], psy_c[32];
    std::memcpy(saved_d, gteRegs.CP2D.r, sizeof(saved_d));
    std::memcpy(saved_c, gteRegs.CP2C.r, sizeof(saved_c));
    std::memcpy(gteRegs.CP2D.r, pre_d, sizeof(saved_d));
    std::memcpy(gteRegs.CP2C.r, pre_c, sizeof(saved_c));
    GTE_operator((int)cmd);
    std::memcpy(psy_d, gteRegs.CP2D.r, sizeof(psy_d));
    std::memcpy(psy_c, gteRegs.CP2C.r, sizeof(psy_c));
    std::memcpy(gteRegs.CP2D.r, saved_d, sizeof(saved_d));
    std::memcpy(gteRegs.CP2C.r, saved_c, sizeof(saved_c));
    trace_rows(cpu->gte_data, cpu->gte_ctrl, psy_d, psy_c, fn);
}

// --------------------------------------------------------------- census

/* Kinds of register store observed between two commands (a register whose
 * value at command entry differs from the previous command's write-back). */
enum StoreKind {
    kStoreSxyp,      /* data 15: a FIFO push on hardware, a plain word here */
    kStoreSxy2Only,  /* data 14 without data 15: SXYP no longer mirrors SXY2 */
    kStoreUnext16,   /* a 16-bit register holding a value not in its extended form */
    kStoreIrgb,      /* data 28 (IRGB) */
    kStoreOrgb,      /* data 29 (ORGB, read-only on hardware) */
    kStoreLzcr,      /* data 31 holding a value other than the leading-bit count of data 30 */
    kStoreFlag,      /* control 31 (FLAG) */
    kStoreOther,     /* any other register, and data 31 consistent with data 30 */
    kStoreKinds
};
const char* const k_store_names[kStoreKinds] = {"sxyp", "sxy2_without_sxyp", "unextended_16bit", "irgb",
                                                "orgb", "lzcr", "flag", "other"};

struct UnkCmd { uint32_t cmd, pc; uint64_t n; };

struct Census {
    int      on = 0;
    uint64_t by_fn[64] = {};
    uint64_t stores[kStoreKinds] = {};
    uint32_t sxyp_pcs[16] = {};
    unsigned sxyp_pc_n = 0;
    UnkCmd   unk[64] = {};
    unsigned unk_n = 0;
    uint32_t shadow_d[32] = {}, shadow_c[32] = {};
    int      have_shadow = 0;
} g_census;

/** @brief Return the count of leading bits equal to bit 31 of v (the LZCR value of LZCS v), 1..32. */
uint32_t leading_bits(uint32_t v) {
    const uint32_t sign = UINT32_C(1) << 31;
    uint32_t x = (v & sign) ? ~v : v, n = 0;
    while (n < 32u && !(x & sign)) {
        x <<= 1;
        n++;
    }
    return n;
}

/** @brief Return 1 when the 16-bit register's stored word is not the extension its read form uses (data index 0-31, control 32-63). */
int unextended(int reg, uint32_t w) {
    static const uint8_t k_sext[] = {1, 3, 5, 8, 9, 10, 11, 36, 44, 52, 58, 59, 61, 62};
    static const uint8_t k_zext[] = {7, 16, 17, 18, 19};
    for (uint8_t r : k_sext)
        if (r == reg) return w != (uint32_t)lo16(w);
    for (uint8_t r : k_zext)
        if (r == reg) return (w >> 16) != 0;
    return 0;
}

/** @brief Classify one register (0-63) whose entry value differs from the last write-back. */
StoreKind store_kind(int reg, uint32_t w, const uint32_t* pre_d) {
    if (reg == 15) return kStoreSxyp;
    if (reg == 14) return kStoreSxy2Only;
    if (unextended(reg, w)) return kStoreUnext16;
    if (reg == 28) return kStoreIrgb;
    if (reg == 29) return kStoreOrgb;
    if (reg == 31) return w != leading_bits(pre_d[30]) ? kStoreLzcr : kStoreOther;
    if (reg == 63) return kStoreFlag;
    return kStoreOther;
}

/** @brief Record the caller PC of a command preceded by an SXYP store (first 16 distinct). */
void census_sxyp_pc(uint32_t pc) {
    for (unsigned i = 0; i < g_census.sxyp_pc_n; i++)
        if (g_census.sxyp_pcs[i] == pc) return;
    if (g_census.sxyp_pc_n < 16) g_census.sxyp_pcs[g_census.sxyp_pc_n++] = pc;
}

/** @brief Count the stores made since the previous command, by kind, from the entry register file. */
void census_stores(const uint32_t* pre_d, const uint32_t* pre_c) {
    if (!g_census.have_shadow) return;
    int sxyp = 0, sxy2 = 0;
    for (int reg = 0; reg < 64; reg++) {
        const uint32_t w = reg < 32 ? pre_d[reg] : pre_c[reg - 32];
        const uint32_t was = reg < 32 ? g_census.shadow_d[reg] : g_census.shadow_c[reg - 32];
        if (w == was) continue;
        const StoreKind k = store_kind(reg, w, pre_d);
        sxyp |= k == kStoreSxyp;
        sxy2 |= k == kStoreSxy2Only;
        if (k != kStoreSxy2Only) g_census.stores[k]++;
    }
    if (sxyp) census_sxyp_pc(openpete_gte_caller_pc);
    if (sxy2 && !sxyp) g_census.stores[kStoreSxy2Only]++;
}

/** @brief Count an undocumented command word with its first caller PC (first 64 distinct words). */
void census_unknown(uint32_t cmd) {
    for (unsigned i = 0; i < g_census.unk_n; i++)
        if (g_census.unk[i].cmd == cmd) { g_census.unk[i].n++; return; }
    if (g_census.unk_n < 64) g_census.unk[g_census.unk_n++] = UnkCmd{cmd, openpete_gte_caller_pc, 1};
}

/** @brief Print the census to stderr: per function code, undocumented words, stores between commands by kind. */
void census_dump() {
    OP_ROLE_ASSERT(sim);
    for (int fn = 0; fn < 64; fn++)
        if (g_census.by_fn[fn])
            std::fprintf(stderr, "[gte-census] fn=%02X n=%llu\n", fn, (unsigned long long)g_census.by_fn[fn]);
    for (unsigned i = 0; i < g_census.unk_n; i++)
        std::fprintf(stderr, "[gte-census] undocumented cmd=%07X first_pc=%08X n=%llu\n", g_census.unk[i].cmd,
                     g_census.unk[i].pc, (unsigned long long)g_census.unk[i].n);
    for (int k = 0; k < kStoreKinds; k++)
        std::fprintf(stderr, "[gte-census] store %s n=%llu\n", k_store_names[k], (unsigned long long)g_census.stores[k]);
    for (unsigned i = 0; i < g_census.sxyp_pc_n; i++)
        std::fprintf(stderr, "[gte-census] sxyp_store_before_pc=%08X\n", g_census.sxyp_pcs[i]);
    std::fprintf(stderr, "[gte-census] sxyp_stores=%llu\n", (unsigned long long)g_census.stores[kStoreSxyp]);
}

/** @brief Feed one command to the census and remember its write-back for the next store count. */
void census_op(const CPUState* cpu, uint32_t cmd, int ran, const uint32_t* pre_d, const uint32_t* pre_c) {
    census_stores(pre_d, pre_c);
    g_census.by_fn[fn_of(cmd)]++;
    if (!ran) census_unknown(cmd & 0x1FFFFFFu);
    std::memcpy(g_census.shadow_d, cpu->gte_data, sizeof(g_census.shadow_d));
    std::memcpy(g_census.shadow_c, cpu->gte_ctrl, sizeof(g_census.shadow_c));
    g_census.have_shadow = 1;
}

// --------------------------------------------------------- render probe

int32_t g_probe_last[12];

/** @brief Count RTPS/RTPT/MVMVA/other commands and the rotation/translation changes among the first three. */
void probe_op(uint32_t fn, const uint32_t* pre_c) {
    if (fn == kFnRtps) g_openpete_render_probe_count_rtps++;
    else if (fn == kFnRtpt) g_openpete_render_probe_count_rtpt++;
    else if (fn == kFnMvmva) g_openpete_render_probe_count_mvmva++;
    else g_openpete_render_probe_count_other++;
    if (!is_rtp(fn) && fn != kFnMvmva) return;
    int32_t now[12];
    for (int i = 0; i < 8; i++) now[i] = (int32_t)pre_c[i];
    now[8] = now[9] = now[10] = now[11] = 0;
    now[4] = lo16(pre_c[4]);
    if (std::memcmp(now, g_probe_last, sizeof(now)) == 0) return;
    g_openpete_render_probe_count_matrix_changes++;
    std::memcpy(g_probe_last, now, sizeof(now));
}

// ------------------------------------------------- caller-PC diagnostics

struct PcHist {
    int      vbl = -1;
    uint32_t pcs[4096], cnt[4096];
    int      n = 0, dumped = 0;
} g_hist;

/** @brief Count the caller PC while openpete_frame equals the chosen VBL; print the histogram once the VBL has passed. */
void hist_op() {
    const int f = (int)openpete_frame;
    if (f > g_hist.vbl && !g_hist.dumped && g_hist.n) {
        g_hist.dumped = 1;
        for (int i = 0; i < g_hist.n; i++) std::fprintf(stderr, "[gtehist] pc=%08X n=%u\n", g_hist.pcs[i], g_hist.cnt[i]);
    }
    if (f != g_hist.vbl) return;
    int i = 0;
    while (i < g_hist.n && g_hist.pcs[i] != openpete_gte_caller_pc) i++;
    if (i < g_hist.n) g_hist.cnt[i]++;
    else if (g_hist.n < 4096) { g_hist.pcs[g_hist.n] = openpete_gte_caller_pc; g_hist.cnt[g_hist.n++] = 1; }
}

struct BoxLog { int on = 0, x0, y0, x1, y1, vbl; unsigned n = 0; } g_box;

/** @brief Log the caller PC of each RTPS/RTPT vertex whose projected SX/SY lies in the box on the chosen VBL (at most 4000 rows). */
void box_op(const CPUState* cpu, uint32_t fn) {
    if ((int)openpete_frame != g_box.vbl || !is_rtp(fn)) return;
    for (int v = fn == kFnRtpt ? 0 : 2; v < 3 && g_box.n < 4000; v++) {
        const uint32_t sxy = cpu->gte_data[12 + v];
        const int sx = lo16(sxy), sy = hi16(sxy);
        if (sx < g_box.x0 || sx > g_box.x1 || sy < g_box.y0 || sy > g_box.y1) continue;
        g_box.n++;
        std::fprintf(stderr, "[rtps-box] pc=%08X sx=%d sy=%d sz3=%u\n", openpete_gte_caller_pc, sx, sy,
                     cpu->gte_data[16 + v + 1] & 0xFFFFu);
    }
}

struct OfxEnt { uint32_t pc; int32_t lo, hi; uint64_t n; };
struct OfxCensus { int on = 0; OfxEnt tab[64]; unsigned n = 0; } g_ofx;

/** @brief Print the OFX census table at exit. */
void ofx_dump() {
    OP_ROLE_ASSERT(sim);
    for (unsigned k = 0; k < g_ofx.n; k++)
        std::fprintf(stderr, "[ofx-census] pc=%08X ofx=[%d..%d] n=%llu\n", g_ofx.tab[k].pc, (int)(g_ofx.tab[k].lo >> 16),
                     (int)(g_ofx.tab[k].hi >> 16), (unsigned long long)g_ofx.tab[k].n);
}

/** @brief Record an RTPS/RTPT whose OFX is not 256.0 against its caller PC (first 64 PCs; a new PC is printed at first sight). */
void ofx_op(uint32_t fn, const uint32_t* pre_c) {
    const int32_t ofx = (int32_t)pre_c[24];
    if (!is_rtp(fn) || ofx == (256 << 16)) return;
    unsigned i = 0;
    while (i < g_ofx.n && g_ofx.tab[i].pc != openpete_gte_caller_pc) i++;
    if (i == g_ofx.n) {
        if (g_ofx.n >= 64) return;
        g_ofx.tab[g_ofx.n++] = OfxEnt{openpete_gte_caller_pc, ofx, ofx, 0};
        std::fprintf(stderr, "[ofx-census] NEW pc=%08X ofx=%d frame=%u\n", openpete_gte_caller_pc, (int)(ofx >> 16),
                     openpete_frame);
    }
    if (ofx < g_ofx.tab[i].lo) g_ofx.tab[i].lo = ofx;
    if (ofx > g_ofx.tab[i].hi) g_ofx.tab[i].hi = ofx;
    g_ofx.tab[i].n++;
}

/** @brief Close the record file at exit so its last block reaches the disk. */
void record_close() {
    OP_ROLE_ASSERT(sim);
    if (g_rec.fp) std::fclose(g_rec.fp);
    g_rec.fp = nullptr;
}

/** @brief Open the OPENPETE_GTE_RECORD file and parse its op list; return 1 when armed. */
int record_init() {
    const char* e = std::getenv("OPENPETE_GTE_RECORD");
    if (!e || !*e) return 0;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s", e);
    char* colon = std::strrchr(path, ':');
    if (colon) *colon = 0;
    g_rec.ops = colon ? parse_op_mask(colon + 1) : 0;
    if (!g_rec.ops) g_rec.ops = (1ull << 0x01) | (1ull << 0x30);
    g_rec.fp = std::fopen(path, "wb");
    if (!g_rec.fp) return 0;
    std::atexit(record_close);
    return 1;
}

/** @brief Read every instrument's environment once; return 1 when any is armed. */
int instruments_init() {
    int any = trace_init() | record_init();
    g_census.on = env_set("OPENPETE_GTE_CENSUS");
    if (g_census.on) std::atexit(census_dump);
    if (g_openpete_render_probe_active < 0) {
        const char* e = std::getenv("OPENPETE_RENDER_PROBE");
        g_openpete_render_probe_active = (e && *e == '1') ? 1 : 0;
    }
    if (const char* e = std::getenv("OPENPETE_GTE_PCHIST")) g_hist.vbl = *e ? std::atoi(e) : -1;
    if (const char* e = std::getenv("OPENPETE_RTPS_BOXLOG"))
        g_box.on = std::sscanf(e, "%d,%d,%d,%d,%d", &g_box.x0, &g_box.y0, &g_box.x1, &g_box.y1, &g_box.vbl) == 5;
    g_ofx.on = env_set("OPENPETE_OFX_CENSUS");
    if (g_ofx.on) std::atexit(ofx_dump);
    any |= g_census.on | g_openpete_render_probe_active | (g_hist.vbl >= 0) | g_box.on | g_ofx.on;
    return any;
}

} // namespace

extern "C" void openpete_gte_instruments_op(CPUState* cpu, uint32_t cmd, int ran, const uint32_t pre_data[32],
                                            const uint32_t pre_ctrl[32]) {
    if (openpete_gte_instruments_on < 0) {
        openpete_gte_instruments_on = instruments_init();
        if (!openpete_gte_instruments_on) return;
    }
    const uint32_t fn = fn_of(cmd);
    if (g_rec.fp) record_op(cmd, pre_data, pre_ctrl);
    trace_op(cpu, cmd, pre_data, pre_ctrl);
    if (g_census.on) census_op(cpu, cmd, ran, pre_data, pre_ctrl);
    if (g_openpete_render_probe_active == 1) probe_op(fn, pre_ctrl);
    if (g_hist.vbl >= 0) hist_op();
    if (g_box.on) box_op(cpu, fn);
    if (g_ofx.on) ofx_op(fn, pre_ctrl);
}
