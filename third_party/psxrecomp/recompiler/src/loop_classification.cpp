#include "loop_classification.h"

#include <fmt/format.h>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <queue>
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
    if (it == leaves.end() || !it->second.leaf) return false;
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

constexpr uint32_t kNoPath = ReturnCosts::kNone;

/** @brief Add two path costs.
 * @param a a cost or kNoPath
 * @param b a cost or kNoPath
 * @return the sum, kNoPath when either is */
uint32_t add_cost(uint32_t a, uint32_t b) {
    return (a == kNoPath || b == kNoPath) ? kNoPath : a + b;
}

/** @brief Lower a cost to a candidate.
 * @param slot the cost kept
 * @param c    the candidate */
void lower(uint32_t& slot, uint32_t c) {
    if (c < slot) slot = c;
}

/// Return costs of a unit's functions and of the units it calls into.
struct CostTable {
    std::map<uint32_t, ReturnCosts> unit;             ///< by function start address
    const std::map<uint32_t, LeafSummary>* foreign;   ///< other units' rows
};

/** @brief Look up a callee's return costs.
 * @param t      the table
 * @param target the callee address
 * @param in_unit whether the call names a generated function of the unit
 * @return its costs; kNoPath throughout when unknown */
ReturnCosts callee_costs(const CostTable& t, uint32_t target, bool in_unit) {
    if (in_unit) {
        auto it = t.unit.find(target);
        return it == t.unit.end() ? ReturnCosts{} : it->second;
    }
    auto it = t.foreign->find(target);
    return it == t.foreign->end() ? ReturnCosts{} : it->second.cost;
}

/// The v0 class a path carries: 0 unknown, 1 zero, 2 nonzero.
using V0Class = unsigned;

/** @brief Apply a region's v0 write to the class a path carries into it.
 * @param in the class entering the region
 * @param out the region's EmittedRegion::v0_out
 * @return the class leaving it */
V0Class v0_after(V0Class in, V0Out out) {
    switch (out) {
        case V0Out::None:    return in;
        case V0Out::Zero:    return 1;
        case V0Out::NonZero: return 2;
        case V0Out::Unknown: return 0;
    }
    return 0;
}

/// A region's cost and how it returns, for the return-path search.
struct RegionCost {
    uint32_t weight = 0;     ///< instructions plus the any-return cost of each non-tail call
    bool terminal = false;   ///< control leaves the function here
    bool has_tail = false;   ///< the region ends in a call of a generated function
    ReturnCosts tail;        ///< that callee's costs
};

/** @brief Cost one region for the return-path search.
 * @param r     the region
 * @param last_succ_free true when the region has no successor in the graph
 * @param t     callee costs
 * @return its weight and return form */
RegionCost region_cost(const EmittedRegion& r, bool last_succ_free, const CostTable& t) {
    RegionCost c;
    c.weight = r.insns;
    c.terminal = last_succ_free && r.gotos.empty();
    const bool tail_unit = c.terminal && r.named.empty() && !r.callees.empty() && r.foreign.empty();
    const bool tail_foreign = c.terminal && r.named.empty() && r.callees.empty() && !r.foreign.empty();
    for (size_t k = 0; k < r.callees.size(); k++) {
        const ReturnCosts rc = callee_costs(t, r.callees[k], true);
        if (tail_unit && k + 1 == r.callees.size()) { c.has_tail = true; c.tail = rc; }
        else c.weight = add_cost(c.weight, rc.any);
    }
    for (size_t k = 0; k < r.foreign.size(); k++) {
        const ReturnCosts rc = callee_costs(t, r.foreign[k], false);
        if (tail_foreign && k + 1 == r.foreign.size()) { c.has_tail = true; c.tail = rc; }
        else c.weight = add_cost(c.weight, rc.any);
    }
    return c;
}

/** @brief Find a function's shortest paths from its entry to its returns, by returned v0.
 *
 * Dijkstra over (region, v0 class) with region weights from region_cost();
 * the entry carries an unknown v0. A tail call returns the callee's costs
 * added to the path's; any other return returns the class the path carries.
 * @param f the function
 * @param g its region graph
 * @param t callee costs
 * @return the costs */
ReturnCosts function_costs(const FunctionFacts& f, const RegionGraph& g, const CostTable& t) {
    ReturnCosts out;
    const size_t n = f.regions.size();
    if (f.stub || n == 0) return out;
    std::vector<RegionCost> rc(n);
    for (size_t i = 0; i < n; i++) rc[i] = region_cost(f.regions[i], g.succ[i].empty(), t);
    std::vector<uint32_t> dist(n * 3, kNoPath);
    using Item = std::pair<uint32_t, size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    auto push = [&](size_t r, V0Class in, uint32_t before) {
        const size_t node = r * 3 + v0_after(in, f.regions[r].v0_out);
        const uint32_t d = add_cost(before, rc[r].weight);
        if (d < dist[node]) { dist[node] = d; q.push({d, node}); }
    };
    push(0, 0, 0);
    while (!q.empty()) {
        const auto [d, node] = q.top();
        q.pop();
        if (d != dist[node]) continue;
        const size_t r = node / 3;
        const V0Class cls = node % 3;
        if (rc[r].has_tail) {
            lower(out.any, add_cost(d, rc[r].tail.any));
            lower(out.zero, add_cost(d, rc[r].tail.zero));
            lower(out.nonzero, add_cost(d, rc[r].tail.nonzero));
        } else if (rc[r].terminal) {
            lower(out.any, d);
            if (cls == 1 && f.regions[r].named.empty()) lower(out.zero, d);
            if (cls == 2 && f.regions[r].named.empty()) lower(out.nonzero, d);
        }
        for (size_t s : g.succ[r]) push(s, cls, d);
    }
    return out;
}

/** @brief Compute every function's return costs.
 *
 * Each round recomputes every function against the previous round's
 * table; costs only fall, so the rounds stop when none changes (capped at
 * 256 rounds).
 * @param facts   every function the unit emitted
 * @param foreign rows of other units
 * @return one entry per function, in `facts` order */
std::vector<ReturnCosts> return_costs(const std::vector<FunctionFacts>& facts,
                                      const std::map<uint32_t, LeafSummary>& foreign) {
    CostTable t{{}, &foreign};
    std::vector<RegionGraph> graphs;
    graphs.reserve(facts.size());
    for (const FunctionFacts& f : facts) graphs.push_back(build_graph(f));
    std::vector<ReturnCosts> v(facts.size());
    for (int round = 0; round < 256; round++) {
        bool changed = false;
        for (size_t i = 0; i < facts.size(); i++) {
            const ReturnCosts c = function_costs(facts[i], graphs[i], t);
            if (c.any != v[i].any || c.zero != v[i].zero || c.nonzero != v[i].nonzero) {
                v[i] = c;
                changed = true;
            }
            t.unit[facts[i].start] = v[i];
        }
        if (!changed) break;
    }
    return v;
}

/// What a function's call closure does, as the device-status poll and call-spin criteria read it.
struct Closure {
    bool traced = true;   ///< every call in the closure resolves to known code
    bool quiet = true;    ///< traced, a kernel call excepted (EmittedRegion::unknown_target)
    bool kernel = false;  ///< some function of the closure makes a kernel call
    bool vsync = false;   ///< some function of the closure calls `VSync`
    bool status = false;  ///< some function of the closure calls a device-status thunk
};

/** @brief Fold one function's own calls into its closure verdict.
 * @param f       the function
 * @param index   functions of the unit by start address
 * @param foreign rows of other units
 * @param thunks  the unit's symbol classes
 * @param edges   receives the indices of the unit's functions it calls
 * @return the verdict of the function's own calls */
Closure own_calls(const FunctionFacts& f, const std::map<uint32_t, size_t>& index,
                  const std::map<uint32_t, LeafSummary>& foreign, const ThunkClasses& thunks,
                  std::vector<size_t>& edges) {
    Closure c;
    c.traced = c.quiet = !f.stub;
    for (const EmittedRegion& r : f.regions) {
        if (r.indirect) c.traced = false;
        if (r.unknown_target) c.quiet = false;
        if (r.kernel) c.kernel = true;
        for (uint32_t n : r.named) {
            if (thunks.untraced.count(n)) c.traced = c.quiet = false;
            if (thunks.vsync.count(n)) c.vsync = true;
            if (thunks.status.count(n)) c.status = true;
        }
        for (uint32_t t : r.callees) {
            auto it = index.find(t);
            if (it == index.end()) c.traced = c.quiet = false;
            else edges.push_back(it->second);
        }
        for (uint32_t t : r.foreign) {
            auto it = foreign.find(t);
            if (it == foreign.end()) {
                c.traced = c.quiet = false;
                continue;
            }
            c.traced = c.traced && it->second.traced;
            c.kernel = c.kernel || it->second.kernel;
            c.status = c.status || it->second.status;
        }
    }
    return c;
}

/** @brief Compute every function's call-closure verdict.
 *
 * A foreign row states a quiet, VSync-free closure, traced unless the row
 * says otherwise (write_leaf_summary() writes no other kind); a constant
 * target with no row is neither traced nor quiet.
 * Verdicts propagate from callee to caller until none changes, so a
 * recursive cycle shares one verdict.
 * @param facts   every function the unit emitted
 * @param foreign rows of other units
 * @param thunks  the unit's symbol classes
 * @return one verdict per function, in `facts` order */
std::vector<Closure> closures(const std::vector<FunctionFacts>& facts,
                              const std::map<uint32_t, LeafSummary>& foreign,
                              const ThunkClasses& thunks) {
    std::map<uint32_t, size_t> index;
    for (size_t i = 0; i < facts.size(); i++) index.emplace(facts[i].start, i);
    std::vector<Closure> v(facts.size());
    std::vector<std::vector<size_t>> edges(facts.size());
    for (size_t i = 0; i < facts.size(); i++)
        v[i] = own_calls(facts[i], index, foreign, thunks, edges[i]);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < facts.size(); i++) {
            Closure c = v[i];
            for (size_t e : edges[i]) {
                c.traced = c.traced && v[e].traced;
                c.quiet = c.quiet && v[e].quiet;
                c.kernel = c.kernel || v[e].kernel;
                c.vsync = c.vsync || v[e].vsync;
                c.status = c.status || v[e].status;
            }
            if (c.traced != v[i].traced || c.quiet != v[i].quiet || c.vsync != v[i].vsync ||
                c.status != v[i].status || c.kernel != v[i].kernel) {
                v[i] = c;
                changed = true;
            }
        }
    }
    return v;
}

/** @brief Report whether a closure verdict admits a device-status poll.
 * @param c the verdict
 * @return true when traced, VSync-free and reaching a device-status thunk */
bool polls_device(const Closure& c) { return c.traced && !c.vsync && c.status; }

/// Call-closure verdicts and return costs of a unit and of the units it calls into.
struct CalleeVerdicts {
    std::map<uint32_t, Closure> unit;                 ///< by function start address
    std::map<uint32_t, ReturnCosts> costs;            ///< by function start address
    const std::map<uint32_t, LeafSummary>* foreign;   ///< other units' rows
    std::map<uint32_t, std::vector<uint32_t>> calls;  ///< generated callees of the unit, per function
};

/** @brief Report whether a function of the unit reaches another through generated calls of the unit.
 * @param calls  generated callees per function
 * @param from   the first function
 * @param target the function looked for
 * @return true when `target` is `from` or in its call closure */
bool reaches(const std::map<uint32_t, std::vector<uint32_t>>& calls, uint32_t from, uint32_t target) {
    std::set<uint32_t> seen{from};
    std::vector<uint32_t> work{from};
    while (!work.empty()) {
        const uint32_t f = work.back();
        work.pop_back();
        if (f == target) return true;
        auto it = calls.find(f);
        if (it == calls.end()) continue;
        for (uint32_t c : it->second)
            if (seen.insert(c).second) work.push_back(c);
    }
    return false;
}

/** @brief Classify a back-edge as a device-status poll site.
 *
 * See classify_loops(): a store-free body with one call, to a generated
 * function whose closure polls a device; the back-edge sits in the region
 * the call continues into, which tests the returned v0 directly. The site
 * counts the body's instructions plus the callee's busy-path cost.
 * @param f       function holding the back-edge
 * @param g       its region graph
 * @param in      body membership per region, from loop_body()
 * @param header  index of the header region
 * @param source  index of the region holding the back-edge
 * @param arm     the back-edge's arm
 * @param callees call-closure verdicts
 * @param site    filled with insns, the callee and body regions when the body qualifies
 * @return true when the site qualifies */
bool classify_device_poll(const FunctionFacts& f, const RegionGraph& g, const std::vector<char>& in,
                          size_t header, size_t source, GotoArm arm,
                          const CalleeVerdicts& callees, LoopSite& site) {
    const V0Test test = f.regions[source].v0_test;
    if (arm != GotoArm::Taken || test == V0Test::None) return false;
    size_t call_region = SIZE_MAX;
    uint32_t callee = 0;
    bool foreign = false;
    uint32_t insns = 0;
    for (size_t i = 0; i < f.regions.size(); i++) {
        if (!in[i]) continue;
        const EmittedRegion& r = f.regions[i];
        if (r.stores || r.opaque_call) return false;
        insns += r.insns;
        const size_t calls = r.callees.size() + r.foreign.size();
        if (calls == 0) continue;
        if (calls > 1 || call_region != SIZE_MAX) return false;
        call_region = i;
        foreign = r.callees.empty();
        callee = foreign ? r.foreign[0] : r.callees[0];
    }
    if (call_region == SIZE_MAX) return false;
    size_t body_preds = 0;
    for (size_t p : g.pred[source])
        if (in[p]) body_preds++;
    bool continues = false;
    for (const EmittedGoto& e : f.regions[call_region].gotos)
        continues = continues || (e.arm == GotoArm::Continue && e.target == f.regions[source].label);
    if (body_preds != 1 || !continues) return false;
    ReturnCosts cost;
    if (foreign) {
        auto it = callees.foreign->find(callee);
        if (it == callees.foreign->end() || !it->second.traced || !it->second.status) return false;
        cost = it->second.cost;
    } else {
        auto it = callees.unit.find(callee);
        if (it == callees.unit.end() || !polls_device(it->second)) return false;
        cost = callees.costs.at(callee);
    }
    const uint32_t busy = test == V0Test::TakenNonZero ? cost.nonzero : cost.zero;
    if (busy == kNoPath) return false;
    site.insns = insns + busy;
    site.leaves = {callee};
    site.device_poll = true;
    std::vector<LoopBodyRegion> rest;
    for (size_t i = 0; i < f.regions.size(); i++) {
        if (!in[i]) continue;
        LoopBodyRegion b;
        b.label = f.regions[i].label;
        b.calls = i == call_region;
        for (size_t s : g.succ[i])
            if (in[s]) b.successors.push_back(f.regions[s].label);
        if (i == header)
            site.body.insert(site.body.begin(), std::move(b));
        else
            rest.push_back(std::move(b));
    }
    site.body.insert(site.body.end(), rest.begin(), rest.end());
    return true;
}

/** @brief Report whether a closure verdict is quiet and VSync-free, the leaf summary's row condition.
 * @param c the verdict
 * @return true when quiet and VSync-free */
bool quiet_closure(const Closure& c) { return c.quiet && !c.vsync; }

/** @brief Report whether a closure verdict admits a call-carrying spin's callee.
 *
 * A closure that reaches a device-status thunk is excluded: that wait is
 * paced by the thunk.
 * @param c the verdict
 * @return true when quiet, VSync-free and reaching no device-status thunk */
bool quiet_callee(const Closure& c) { return quiet_closure(c) && !c.status; }

/** @brief Report whether a closure verdict answers a deferred spin: a quiet callee that makes a kernel call.
 * @param c the verdict
 * @return true when quiet_callee() holds and the closure makes a kernel call */
bool waiting_callee(const Closure& c) { return quiet_callee(c) && c.kernel; }

/** @brief Classify a back-edge as a call-carrying spin site.
 *
 * See classify_loops(): a store-free body whose calls all go to generated
 * functions with quiet, VSync-free closures, at most one of them a foreign
 * target resolved at run time. The site counts the body's instructions plus
 * each resolved callee's cheapest return, per call.
 * @param f       function holding the back-edge
 * @param g       its region graph
 * @param in      body membership per region, from loop_body()
 * @param header  index of the header region
 * @param callees call-closure verdicts
 * @param defer_foreign whether a target no row names may be resolved at run time
 * @param site    filled with insns, callees, the deferred target and body regions
 * @return true when the site qualifies */
bool classify_call_spin(const FunctionFacts& f, const RegionGraph& g, const std::vector<char>& in,
                        size_t header, const CalleeVerdicts& callees, bool defer_foreign,
                        LoopSite& site) {
    uint32_t insns = 0, deferred = 0;
    bool kernel = false;
    std::vector<uint32_t> called;
    auto take = [&](uint32_t c, const ReturnCosts& cost) {
        if (cost.any == kNoPath) return false;
        insns += cost.any;
        called.push_back(c);
        return true;
    };
    for (size_t i = 0; i < f.regions.size(); i++) {
        if (!in[i]) continue;
        const EmittedRegion& r = f.regions[i];
        if (r.stores || r.opaque_call) return false;
        insns += r.insns;
        for (uint32_t c : r.callees) {
            auto it = callees.unit.find(c);
            if (it == callees.unit.end() || !quiet_callee(it->second)) return false;
            if (reaches(callees.calls, c, f.start)) return false;
            if (!take(c, callees.costs.at(c))) return false;
            kernel = kernel || it->second.kernel;
        }
        for (uint32_t c : r.foreign) {
            auto it = callees.foreign->find(c);
            if (it != callees.foreign->end()) {
                if (it->second.status || !take(c, it->second.cost)) return false;
                kernel = kernel || it->second.kernel;
                continue;
            }
            if (!defer_foreign || (deferred != 0 && deferred != c)) return false;
            deferred = c;
        }
    }
    if (called.empty() && deferred == 0) return false;
    // A deferred callee's kernel reach is decided with the rest of its closure
    // at run time (quiet_targets()).
    if (!kernel && deferred == 0) return false;
    site.insns = insns;
    site.call_spin = true;
    site.deferred = deferred;
    site.leaves.clear();
    if (deferred != 0) site.leaves.push_back(deferred);
    site.leaves.insert(site.leaves.end(), called.begin(), called.end());
    std::vector<LoopBodyRegion> rest;
    for (size_t i = 0; i < f.regions.size(); i++) {
        if (!in[i]) continue;
        LoopBodyRegion b;
        b.label = f.regions[i].label;
        b.calls = !f.regions[i].callees.empty() || !f.regions[i].foreign.empty();
        for (size_t s2 : g.succ[i])
            if (in[s2]) b.successors.push_back(f.regions[s2].label);
        if (i == header)
            site.body.insert(site.body.begin(), std::move(b));
        else
            rest.push_back(std::move(b));
    }
    site.body.insert(site.body.end(), rest.begin(), rest.end());
    return true;
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

bool is_device_status_thunk(const std::string& name) {
    return name == "CdSync" || name == "CdReady" || name == "SpuIsTransferCompleted" ||
           name == "MemCardSync";
}

std::vector<LoopSite> classify_loops(const std::vector<FunctionFacts>& facts,
                                     const std::map<uint32_t, LeafSummary>& foreign,
                                     const ThunkClasses& thunks, bool defer_foreign) {
    const std::map<uint32_t, LeafSummary> leaves = pure_leaves(facts);
    CalleeVerdicts callees{{}, {}, &foreign};
    const std::vector<Closure> verdicts = closures(facts, foreign, thunks);
    const std::vector<ReturnCosts> costs = return_costs(facts, foreign);
    for (size_t i = 0; i < facts.size(); i++) {
        callees.unit.emplace(facts[i].start, verdicts[i]);
        callees.costs.emplace(facts[i].start, costs[i]);
        std::vector<uint32_t>& out = callees.calls[facts[i].start];
        for (const EmittedRegion& r : facts[i].regions)
            out.insert(out.end(), r.callees.begin(), r.callees.end());
    }
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
                const std::vector<char> body = loop_body(g, h->second, i, from_header);
                LoopSite poll = site;
                LoopSite spin = site;
                if (classify_body(f, g, body, h->second, leaves, foreign, site))
                    sites.push_back(std::move(site));
                else if (classify_device_poll(f, g, body, h->second, i, e.arm, callees, poll))
                    sites.push_back(std::move(poll));
                else if (classify_call_spin(f, g, body, h->second, callees, defer_foreign, spin))
                    sites.push_back(std::move(spin));
            }
        }
    }
    return sites;
}

/** @brief Spell a site's kind as psx_loop_poll and the site list carry it.
 * @param s the site
 * @return 4 for a deferred call-carrying spin, 3 for a call-carrying spin, 2 for a
 *         device-status poll, 1 for a pure spin, 0 for a counted poll */
static unsigned site_kind(const LoopSite& s) {
    if (s.call_spin) return s.deferred ? 4 : 3;
    if (s.device_poll) return 2;
    return s.pure ? 1 : 0;
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
                           leaves.empty() ? "-" : leaves, site_kind(s));
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
    std::set<const LoopSite*> dropped;
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
            const bool carried = !site.device_poll && carried_register(code, open, site);
            site.pure = !site.device_poll && !site.call_spin && !carried;
            if (site.call_spin && carried) {
                dropped.insert(&site);
            } else {
                out.append(code, line, open - line);
                if (site.deferred)
                    out += fmt::format("psx_loop_poll_deferred(cpu, 0x{:08X}u, {}u, 0x{:08X}u);\n",
                                       site.header, site.insns, site.deferred);
                else
                    out += fmt::format("psx_loop_poll(cpu, 0x{:08X}u, {}u, {}u);\n", site.header,
                                       site.insns, site_kind(site));
                emitted++;
            }
        }
        pos = end;
    }
    out.append(code, pos, std::string::npos);
    std::vector<LoopSite> kept;
    for (LoopSite& s : sites)
        if (!dropped.count(&s)) kept.push_back(std::move(s));
    sites = std::move(kept);
    return out;
}

bool write_leaf_summary(const std::string& path, const std::vector<FunctionFacts>& facts,
                        const std::map<uint32_t, LeafSummary>& foreign,
                        const ThunkClasses& thunks) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const std::map<uint32_t, LeafSummary> leaves = pure_leaves(facts);
    const std::vector<Closure> verdicts = closures(facts, foreign, thunks);
    const std::vector<ReturnCosts> costs = return_costs(facts, foreign);
    std::map<uint32_t, LeafSummary> rows = leaves;
    for (size_t i = 0; i < facts.size(); i++) {
        auto it = rows.find(facts[i].start);
        if (it != rows.end()) {
            it->second.cost = costs[i];
            continue;
        }
        if (!quiet_closure(verdicts[i])) continue;
        LeafSummary s;
        s.leaf = false;
        s.status = verdicts[i].status;
        s.traced = verdicts[i].traced;
        s.kernel = verdicts[i].kernel;
        s.cost = costs[i];
        rows.emplace(facts[i].start, s);
    }
    auto spell = [](uint32_t c) { return c == kNoPath ? std::string("-") : std::to_string(c); };
    out << "# address\tinsns\tloads\tleaf\tstatus\tany\tzero\tnonzero\ttraced\tkernel\n";
    for (const auto& [addr, s] : rows)
        out << fmt::format("0x{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", addr, s.insns,
                           s.loads ? 1 : 0, s.leaf ? 1 : 0, s.status ? 1 : 0, spell(s.cost.any),
                           spell(s.cost.zero), spell(s.cost.nonzero), s.traced ? 1 : 0,
                           s.kernel ? 1 : 0);
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
        int loads = 0, leaf = 1, status = 0;
        if (!(row >> addr >> s.insns >> loads)) continue;
        if (row >> leaf) row >> status;
        std::string any, zero, nonzero, traced, kernel;
        auto parse = [](const std::string& c) {
            return (c.empty() || c == "-") ? kNoPath : (uint32_t)std::strtoul(c.c_str(), nullptr, 10);
        };
        row >> any >> zero >> nonzero >> traced >> kernel;
        s.cost = {parse(any), parse(zero), parse(nonzero)};
        s.traced = traced != "0";
        s.kernel = kernel == "1";
        s.loads = loads != 0;
        s.leaf = leaf != 0;
        s.status = status != 0;
        out[(uint32_t)std::strtoul(addr.c_str(), nullptr, 16)] = s;
    }
    return true;
}

std::map<uint32_t, uint32_t> quiet_targets(const std::vector<FunctionFacts>& facts,
                                           const std::map<uint32_t, LeafSummary>& foreign,
                                           const ThunkClasses& thunks,
                                           const std::set<uint32_t>& targets) {
    std::map<uint32_t, uint32_t> out;
    if (targets.empty()) return out;
    const std::vector<Closure> verdicts = closures(facts, foreign, thunks);
    const std::vector<ReturnCosts> costs = return_costs(facts, foreign);
    for (size_t i = 0; i < facts.size(); i++) {
        if (!targets.count(facts[i].start) || !waiting_callee(verdicts[i])) continue;
        if (costs[i].any == kNoPath) continue;
        out.emplace(facts[i].start, costs[i].any);
    }
    return out;
}

bool read_deferred_targets(const std::string& path, std::set<uint32_t>& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string function, header, source, arm, insns, leaves, kind;
        if (!(row >> function >> header >> source >> arm >> insns >> leaves >> kind)) continue;
        if (kind != "4") continue;
        out.insert((uint32_t)std::strtoul(leaves.substr(0, leaves.find(',')).c_str(), nullptr, 16));
    }
    return true;
}

std::string quiet_function_c(const std::map<uint32_t, uint32_t>& quiet) {
    if (quiet.empty()) return {};
    std::string c =
        "\n/* Deferred call-carrying spins of another unit: 1 and the cheapest return cost\n"
        " * when addr is a function of this unit whose call closure is quiet, calls no\n"
        " * VSync or device-status thunk and makes a kernel call, 0 otherwise. */\n"
        "int psx_quiet_compiled(uint32_t addr, uint32_t* cost) {\n"
        "    switch (addr) {\n";
    for (const auto& [addr, cost] : quiet)
        c += fmt::format("        case 0x{:08X}u: *cost = {}u; return 1;\n", addr, cost);
    c += "        default: return 0;\n    }\n}\n";
    return c;
}

}  // namespace PSXRecomp
