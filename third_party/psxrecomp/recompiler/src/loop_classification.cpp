#include "loop_classification.h"

#include <fmt/format.h>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>

namespace PSXRecomp {

namespace {

/** @brief Report whether any region of a function holds a back-edge.
 * @param f the function's emitted facts
 * @return true when some goto targets a label at or below its own region's */
bool has_back_edge(const FunctionFacts& f) {
    for (const EmittedRegion& r : f.regions)
        for (const EmittedGoto& g : r.gotos)
            if (g.target <= r.label) return true;
    return false;
}

/** @brief Report whether a function is a pure leaf.
 * @param f the function's emitted facts
 * @return true when it is no stub and has no store, no call and no back-edge */
bool is_pure_leaf(const FunctionFacts& f) {
    if (f.stub) return false;
    for (const EmittedRegion& r : f.regions)
        if (r.stores || r.opaque_call || !r.callees.empty() || !r.foreign.empty())
            return false;
    return !has_back_edge(f);
}

/** @brief Summarize the pure leaves of a unit.
 * @param facts every function the unit emitted
 * @return insns and loads per pure-leaf start address */
std::map<uint32_t, LeafSummary> pure_leaves(const std::vector<FunctionFacts>& facts) {
    std::map<uint32_t, LeafSummary> out;
    for (const FunctionFacts& f : facts) {
        if (!is_pure_leaf(f)) continue;
        LeafSummary s;
        for (const EmittedRegion& r : f.regions) {
            s.loads = s.loads || r.loads;
            s.insns += r.insns;
        }
        out[f.start] = s;
    }
    return out;
}

/** @brief Account one call of a body against a leaf table.
 * @param leaves pure leaves by address
 * @param target the callee
 * @param loads  set when the leaf loads
 * @param site   receives the leaf's insns and address
 * @return false when the callee is not a pure leaf */
bool take_leaf(const std::map<uint32_t, LeafSummary>& leaves, uint32_t target,
               bool& loads, LoopSite& site) {
    auto it = leaves.find(target);
    if (it == leaves.end()) return false;
    loads = loads || it->second.loads;
    site.insns += it->second.insns;
    site.leaves.push_back(target);
    return true;
}

/// A function's regions as a graph: successor and predecessor indices.
struct RegionGraph {
    std::vector<std::vector<size_t>> succ;
    std::vector<std::vector<size_t>> pred;
};

/** @brief Build the region graph of a function.
 *
 * Edges are the emitted gotos to labels of the function plus each region's
 * fall-through into the next region.
 * @param f the function's emitted facts
 * @return the graph */
RegionGraph build_graph(const FunctionFacts& f) {
    const size_t n = f.regions.size();
    RegionGraph g;
    g.succ.resize(n);
    g.pred.resize(n);
    std::map<uint32_t, size_t> index;
    for (size_t i = 0; i < n; i++) index[f.regions[i].label] = i;
    for (size_t i = 0; i < n; i++) {
        for (const EmittedGoto& e : f.regions[i].gotos) {
            auto it = index.find(e.target);
            if (it != index.end()) g.succ[i].push_back(it->second);
        }
        if (f.regions[i].falls_through && i + 1 < n) g.succ[i].push_back(i + 1);
        for (size_t s : g.succ[i]) g.pred[s].push_back(i);
    }
    return g;
}

/** @brief Mark every region reachable from `from` along successor edges.
 * @param g    the function's region graph
 * @param from the start region, marked itself
 * @return membership flag per region */
std::vector<char> reachable(const RegionGraph& g, size_t from) {
    std::vector<char> in(g.succ.size(), 0);
    std::vector<size_t> work{from};
    in[from] = 1;
    while (!work.empty()) {
        const size_t b = work.back();
        work.pop_back();
        for (size_t s : g.succ[b])
            if (!in[s]) {
                in[s] = 1;
                work.push_back(s);
            }
    }
    return in;
}

/** @brief Collect the loop body closed by the edge source -> header.
 *
 * The body is the header and every region that the header reaches and that
 * reaches the source without passing through the header. When the header
 * dominates the source this is the edge's natural loop; otherwise the cycle
 * has a second entry (an irreducible loop) and the body is every region on
 * a path from the header to the source.
 * @param g      the function's region graph
 * @param header region the edge targets
 * @param source region holding the edge; reachable from `header`
 * @param from_header regions reachable from `header`
 * @return membership flag per region */
std::vector<char> loop_body(const RegionGraph& g, size_t header, size_t source,
                            const std::vector<char>& from_header) {
    std::vector<char> in(g.succ.size(), 0);
    in[header] = 1;
    std::vector<size_t> work;
    if (!in[source]) {
        in[source] = 1;
        work.push_back(source);
    }
    while (!work.empty()) {
        const size_t b = work.back();
        work.pop_back();
        for (size_t p : g.pred[b]) {
            if (in[p] || !from_header[p]) continue;
            in[p] = 1;
            work.push_back(p);
        }
    }
    return in;
}

/** @brief Classify the loop body of one back-edge.
 * @param f       function holding the back-edge
 * @param g       its region graph
 * @param in      body membership per region, from loop_body()
 * @param header  index of the header region
 * @param leaves  pure leaves of this unit
 * @param foreign pure leaves of other units
 * @param site    filled with insns, leaves and body regions when the body qualifies
 * @return true when the body is store-free, calls only pure leaves, and loads */
bool classify_body(const FunctionFacts& f, const RegionGraph& g, const std::vector<char>& in,
                   size_t header, const std::map<uint32_t, LeafSummary>& leaves,
                   const std::map<uint32_t, LeafSummary>& foreign, LoopSite& site) {
    bool loads = false;
    std::vector<LoopBodyRegion> rest;
    for (size_t i = 0; i < f.regions.size(); i++) {
        if (!in[i]) continue;
        const EmittedRegion& r = f.regions[i];
        if (r.stores || r.opaque_call) return false;
        loads = loads || r.loads;
        site.insns += r.insns;
        for (uint32_t c : r.callees)
            if (!take_leaf(leaves, c, loads, site)) return false;
        for (uint32_t c : r.foreign)
            if (!take_leaf(foreign, c, loads, site)) return false;
        LoopBodyRegion b;
        b.label = r.label;
        b.calls = !r.callees.empty() || !r.foreign.empty();
        for (size_t s : g.succ[i])
            if (in[s]) b.successors.push_back(f.regions[s].label);
        if (i == header)
            site.body.insert(site.body.begin(), std::move(b));
        else
            rest.push_back(std::move(b));
    }
    site.body.insert(site.body.end(), rest.begin(), rest.end());
    return loads;
}

/** @brief Spell a goto arm for the site list.
 * @param a the arm
 * @return its lower-case name */
const char* arm_name(GotoArm a) {
    switch (a) {
        case GotoArm::Taken:    return "taken";
        case GotoArm::NotTaken: return "not-taken";
        case GotoArm::Jump:     return "j";
        case GotoArm::Continue: return "continue";
        case GotoArm::Case:     return "case";
    }
    return "?";
}

}  // namespace

std::vector<LoopSite> classify_loops(const std::vector<FunctionFacts>& facts,
                                     const std::map<uint32_t, LeafSummary>& foreign) {
    const std::map<uint32_t, LeafSummary> leaves = pure_leaves(facts);
    std::vector<LoopSite> sites;
    for (const FunctionFacts& f : facts) {
        if (f.stub) continue;
        const RegionGraph g = build_graph(f);
        if (f.regions.empty()) continue;
        const std::vector<char> live = reachable(g, 0);
        std::map<uint32_t, size_t> index;
        for (size_t i = 0; i < f.regions.size(); i++) index[f.regions[i].label] = i;
        for (size_t i = 0; i < f.regions.size(); i++) {
            const EmittedRegion& r = f.regions[i];
            if (!live[i]) continue;
            for (const EmittedGoto& e : r.gotos) {
                if (e.target > r.label) continue;
                auto h = index.find(e.target);
                if (h == index.end() || !live[h->second]) continue;
                const std::vector<char> from_header = reachable(g, h->second);
                if (!from_header[i]) continue;
                LoopSite site;
                site.function = f.name;
                site.header = e.target;
                site.source = r.label;
                site.arm = e.arm;
                if (classify_body(f, g, loop_body(g, h->second, i, from_header), h->second,
                                  leaves, foreign, site))
                    sites.push_back(std::move(site));
            }
        }
    }
    return sites;
}

bool write_loop_sites(const std::string& path, const std::vector<LoopSite>& sites) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << "# function\theader\tsource\tarm\tinsns\tleaves\tpure\n";
    for (const LoopSite& s : sites) {
        std::string leaves;
        for (uint32_t l : s.leaves)
            leaves += fmt::format("{}0x{:08X}", leaves.empty() ? "" : ",", l);
        out << fmt::format("{}\t0x{:08X}\t0x{:08X}\t{}\t{}\t{}\t{}\n", s.function,
                           s.header, s.source, arm_name(s.arm), s.insns,
                           leaves.empty() ? "-" : leaves, s.pure ? 1 : 0);
    }
    return true;
}

namespace {

constexpr const char* kMarkerOpen = "/*@loop-poll ";
constexpr const char* kMarkerClose = "@*/";

/** @brief Spell the key a placeholder and a site share.
 * @param function generated function name
 * @param header   goto target
 * @param source   label of the region holding the goto
 * @param arm      the goto's arm
 * @return the key */
std::string marker_key(const std::string& function, uint32_t header, uint32_t source,
                       GotoArm arm) {
    return fmt::format("{} 0x{:08X} 0x{:08X} {}", function, header, source, arm_name(arm));
}

/// GPR names as the generator spells them after `cpu->`, then hi and lo; bit i of a RegSet.
constexpr const char* kRegNames[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo"};
using RegSet = uint64_t;

/** @brief Map a name to its RegSet bit.
 * @param name text after `cpu->`
 * @return the bit, 0 for zero and for any name that is no register */
RegSet reg_bit(const std::string& name) {
    for (size_t i = 1; i < sizeof(kRegNames) / sizeof(kRegNames[0]); i++)
        if (name == kRegNames[i]) return RegSet{1} << i;
    return 0;
}

/** @brief Collect every register a piece of C reads as `cpu-><reg>`.
 * @param text the text
 * @return the registers */
RegSet regs_read(const std::string& text) {
    RegSet out = 0;
    for (size_t at = text.find("cpu->"); at != std::string::npos; at = text.find("cpu->", at + 1)) {
        if (at > 0 && (std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_')) continue;
        size_t end = at + 5;
        while (end < text.size() && (std::isalnum((unsigned char)text[end]) || text[end] == '_')) end++;
        out |= reg_bit(text.substr(at + 5, end - at - 5));
    }
    return out;
}

/** @brief Remove C comments from emitted text.
 * @param text the text
 * @return the text without its comments */
std::string strip_comments(const std::string& text) {
    std::string out;
    size_t pos = 0;
    for (size_t open = text.find("/*"); open != std::string::npos; open = text.find("/*", pos)) {
        out.append(text, pos, open - pos);
        const size_t close = text.find("*/", open + 2);
        if (close == std::string::npos) return out;
        pos = close + 2;
    }
    out.append(text, pos, std::string::npos);
    return out;
}

/// Upward-exposed reads and definitions of one body region.
struct RegionUse {
    RegSet use = 0;  ///< read before any definition in the region
    RegSet def = 0;  ///< defined in the region
};

/** @brief Compute a region's upward-exposed reads and its definitions from its emitted text.
 *
 * The text is split into statements at `;`, `{`, `}` and line ends. A
 * statement `cpu->R = RHS` reads the registers of RHS, then defines R; any
 * other statement only reads. A region that ends in a call then defines v0,
 * v1, a0-a3, t0-t9 and at (the callee's own reads are not seen).
 * @param text  the region's emitted text, comments removed
 * @param calls whether the region ends in a call
 * @return the sets */
RegionUse region_use(const std::string& text, bool calls) {
    RegionUse u;
    auto read = [&](RegSet r) { u.use |= r & ~u.def; };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of(";{}\n", pos);
        if (end == std::string::npos) end = text.size();
        const std::string stmt = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t i = stmt.find_first_not_of(" \t");
        RegSet def = 0;
        size_t rhs = 0;
        if (i != std::string::npos && stmt.compare(i, 5, "cpu->") == 0) {
            size_t j = i + 5;
            while (j < stmt.size() && (std::isalnum((unsigned char)stmt[j]) || stmt[j] == '_')) j++;
            if (stmt.compare(j, 3, " = ") == 0) {
                def = reg_bit(stmt.substr(i + 5, j - i - 5));
                rhs = j + 3;
            }
        }
        read(regs_read(stmt.substr(rhs)));
        u.def |= def;
    }
    if (calls)
        for (const char* r : {"at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
                              "t4", "t5", "t6", "t7", "t8", "t9"})
            u.def |= reg_bit(r);
    return u;
}

/** @brief Find the emitted text of the region labelled `label` in the function enclosing `marker`.
 * @param code   generated C
 * @param marker an offset inside the function
 * @param label  the region's label
 * @param text   receives the text from the end of the `block_<label>:` line to the next
 *               label or the function's end
 * @return false when the label is not found in the function */
bool region_text(const std::string& code, size_t marker, uint32_t label, std::string& text) {
    const size_t fn = code.rfind("\nvoid ", marker);
    size_t fn_end = code.find("\nvoid ", marker);
    if (fn_end == std::string::npos) fn_end = code.size();
    const std::string tag = fmt::format("\nblock_{:08X}:", label);
    const size_t start = code.find(tag, fn == std::string::npos ? 0 : fn);
    if (start == std::string::npos || start >= fn_end) return false;
    size_t body = code.find('\n', start + 1);
    size_t end = code.find("\nblock_", start + 1);
    if (end == std::string::npos || end > fn_end) end = fn_end;
    if (body == std::string::npos || body > end) body = end;
    text = code.substr(body, end - body);
    return true;
}

/** @brief Decide whether a loop body carries a register value from one iteration into the next.
 *
 * Backward liveness over the body regions, with the edges back into the
 * header removed: a register live into the header (read on some path from
 * the header before any definition) that some body region defines carries.
 * @param code   generated C
 * @param marker offset of the site's placeholder
 * @param site   the classified site, with its body regions
 * @return true for a counted poll (also when a body region's text is not found),
 *         false for a pure spin */
bool carried_register(const std::string& code, size_t marker, const LoopSite& site) {
    const size_t n = site.body.size();
    std::map<uint32_t, size_t> index;
    std::vector<RegionUse> u(n);
    RegSet defs = 0;
    for (size_t i = 0; i < n; i++) {
        index[site.body[i].label] = i;
        std::string text;
        if (!region_text(code, marker, site.body[i].label, text)) return true;
        u[i] = region_use(strip_comments(text), site.body[i].calls);
        defs |= u[i].def;
    }
    std::vector<RegSet> live_in(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = n; k-- > 0;) {
            RegSet out = 0;
            for (uint32_t s : site.body[k].successors) {
                const size_t j = index.at(s);
                if (j != 0) out |= live_in[j];
            }
            const RegSet in = u[k].use | (out & ~u[k].def);
            if (in != live_in[k]) {
                live_in[k] = in;
                changed = true;
            }
        }
    }
    return (live_in[0] & defs) != 0;
}

}  // namespace

std::string loop_poll_marker(const std::string& indent, const std::string& function,
                             uint32_t header, uint32_t source, GotoArm arm) {
    return indent + kMarkerOpen + marker_key(function, header, source, arm) + kMarkerClose + "\n";
}

std::string apply_loop_polls(const std::string& code, std::vector<LoopSite>& sites,
                             size_t& emitted) {
    std::map<std::string, LoopSite*> by_key;
    for (LoopSite& s : sites)
        by_key[marker_key(s.function, s.header, s.source, s.arm)] = &s;
    std::string out;
    out.reserve(code.size());
    emitted = 0;
    size_t pos = 0;
    for (size_t open = code.find(kMarkerOpen); open != std::string::npos;
         open = code.find(kMarkerOpen, pos)) {
        const size_t bol = code.rfind('\n', open);
        const size_t line = (bol == std::string::npos) ? 0 : bol + 1;
        const size_t eol = code.find('\n', open);
        const size_t end = (eol == std::string::npos) ? code.size() : eol + 1;
        const size_t key_at = open + std::strlen(kMarkerOpen);
        const size_t close = code.find(kMarkerClose, key_at);
        out.append(code, pos, line - pos);
        auto it = by_key.find(code.substr(key_at, close - key_at));
        if (it != by_key.end()) {
            LoopSite& site = *it->second;
            site.pure = !carried_register(code, open, site);
            out.append(code, line, open - line);
            out += fmt::format("psx_loop_poll(cpu, 0x{:08X}u, {}u, {}u);\n", site.header,
                               site.insns, site.pure ? 1 : 0);
            emitted++;
        }
        pos = end;
    }
    out.append(code, pos, std::string::npos);
    return out;
}

bool write_leaf_summary(const std::string& path, const std::vector<FunctionFacts>& facts) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << "# address\tinsns\tloads\n";
    for (const auto& [addr, s] : pure_leaves(facts))
        out << fmt::format("0x{:08X}\t{}\t{}\n", addr, s.insns, s.loads ? 1 : 0);
    return true;
}

bool read_leaf_summary(const std::string& path, std::map<uint32_t, LeafSummary>& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string addr;
        LeafSummary s;
        int loads = 0;
        if (!(row >> addr >> s.insns >> loads)) continue;
        s.loads = loads != 0;
        out[(uint32_t)std::strtoul(addr.c_str(), nullptr, 16)] = s;
    }
    return true;
}

}  // namespace PSXRecomp
