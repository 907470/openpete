#include "loop_classification.h"

#include <fmt/format.h>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <set>
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

/** @brief Report whether `text` contains `cpu-><reg>` as a whole token.
 * @param text the right-hand side of an assignment
 * @param reg  the register name
 * @return true when the register is read */
bool reads_reg(const std::string& text, const std::string& reg) {
    const std::string tok = "cpu->" + reg;
    for (size_t at = text.find(tok); at != std::string::npos; at = text.find(tok, at + 1)) {
        const bool word_before = at > 0 && (std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_');
        const size_t end = at + tok.size();
        const bool word_after = end < text.size() &&
                                (std::isalnum((unsigned char)text[end]) || text[end] == '_');
        if (!word_before && !word_after) return true;
    }
    return false;
}

/** @brief Decide whether a loop body's text carries a register across iterations other than through a load.
 *
 * Walks the lines in order; an assignment `cpu->R = RHS;` whose RHS reads R
 * before any earlier assignment to R in the body carries R (R not zero, hi
 * or lo).
 * @param body the body's emitted text
 * @return true for a counted poll, false for a pure spin */
bool carried_register(const std::string& body) {
    std::set<std::string> defined;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const std::string line = body.substr(pos, eol - pos);
        pos = eol + 1;
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line.compare(i, 5, "cpu->") != 0) continue;
        i += 5;
        size_t j = i;
        while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
        if (line.compare(j, 3, " = ") != 0) continue;
        const size_t semi = line.rfind(';');
        if (semi == std::string::npos || semi < j + 3) continue;
        const std::string reg = line.substr(i, j - i);
        const std::string rhs = line.substr(j + 3, semi - (j + 3));
        if (!defined.count(reg) && reg != "zero" && reg != "lo" && reg != "hi" && reads_reg(rhs, reg))
            return true;
        defined.insert(reg);
    }
    return false;
}

/** @brief Return the emitted body text of the back-edge whose placeholder sits at `marker`.
 *
 * From the `block_<header>:` label of the enclosing function to the next
 * block label after the placeholder (or the function's end).
 * @param code   generated C
 * @param marker offset of the placeholder
 * @param header the goto's target
 * @return the body text, empty when the header label is not found */
std::string body_text(const std::string& code, size_t marker, uint32_t header) {
    const size_t fn = code.rfind("\nvoid ", marker);
    const std::string label = fmt::format("\nblock_{:08X}:", header);
    const size_t start = code.find(label, fn == std::string::npos ? 0 : fn);
    if (start == std::string::npos || start > marker) return {};
    size_t end = code.find("\nblock_", marker);
    const size_t next_fn = code.find("\nvoid ", marker);
    if (end == std::string::npos || (next_fn != std::string::npos && next_fn < end)) end = next_fn;
    if (end == std::string::npos) end = code.size();
    return code.substr(start + 1, end - start - 1);
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
            site.pure = !carried_register(body_text(code, open, site.header));
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
