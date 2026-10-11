#include "loop_classification.h"

#include <fmt/format.h>
#include <fstream>
#include <cstdlib>
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

/** @brief Classify the body [header, source] of one back-edge.
 * @param f      function holding the back-edge
 * @param header goto target
 * @param source label of the region holding the goto
 * @param leaves  pure leaves of this unit
 * @param foreign pure leaves of other units
 * @param site   filled with insns and leaves when the body qualifies
 * @return true when the body is store-free, calls only pure leaves, and loads */
bool classify_body(const FunctionFacts& f, uint32_t header, uint32_t source,
                   const std::map<uint32_t, LeafSummary>& leaves,
                   const std::map<uint32_t, LeafSummary>& foreign, LoopSite& site) {
    bool loads = false;
    for (const EmittedRegion& r : f.regions) {
        if (r.label < header || r.label > source) continue;
        if (r.stores || r.opaque_call) return false;
        loads = loads || r.loads;
        site.insns += r.insns;
        for (uint32_t c : r.callees)
            if (!take_leaf(leaves, c, loads, site)) return false;
        for (uint32_t c : r.foreign)
            if (!take_leaf(foreign, c, loads, site)) return false;
    }
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
        for (const EmittedRegion& r : f.regions) {
            for (const EmittedGoto& g : r.gotos) {
                if (g.target > r.label) continue;
                LoopSite site;
                site.function = f.name;
                site.header = g.target;
                site.source = r.label;
                site.arm = g.arm;
                if (classify_body(f, g.target, r.label, leaves, foreign, site))
                    sites.push_back(std::move(site));
            }
        }
    }
    return sites;
}

bool write_loop_sites(const std::string& path, const std::vector<LoopSite>& sites) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out << "# function\theader\tsource\tarm\tinsns\tleaves\n";
    for (const LoopSite& s : sites) {
        std::string leaves;
        for (uint32_t l : s.leaves)
            leaves += fmt::format("{}0x{:08X}", leaves.empty() ? "" : ",", l);
        out << fmt::format("{}\t0x{:08X}\t0x{:08X}\t{}\t{}\t{}\n", s.function,
                           s.header, s.source, arm_name(s.arm), s.insns,
                           leaves.empty() ? "-" : leaves);
    }
    return true;
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
