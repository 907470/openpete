#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace PSXRecomp {

/// Which emitted `goto` arm a control transfer was written as.
enum class GotoArm {
    Taken,      ///< conditional branch, taken arm (ordinary or split-piece form)
    NotTaken,   ///< conditional branch, fall-through arm (always forward)
    Jump,       ///< unconditional `j` to a block of the same function
    Continue,   ///< continuation after a `jal`/`jalr` (always forward)
    Case,       ///< a `case` of a jump-table `switch`
};

/// What v0 holds when control leaves a region, as far as the region decides it.
enum class V0Out : uint8_t {
    None,      ///< the region writes no v0: the value entering it leaves it
    Zero,      ///< last written as the constant zero
    NonZero,   ///< last written as a nonzero constant (`addiu`/`ori` from zero, `lui`)
    Unknown,   ///< last written by anything else, a call included
};

/// The v0 test a region's exit branch makes.
enum class V0Test : uint8_t {
    None,      ///< not a `beq`/`bne` of v0 against zero preceded by nothing in the region
    TakenZero,     ///< `beq`/`beql`: the taken arm runs when v0 is zero
    TakenNonZero,  ///< `bne`/`bnel`: the taken arm runs when v0 is nonzero
};

/// The shortest path costs of a function to its returns, by the v0 it returns.
struct ReturnCosts {
    static constexpr uint32_t kNone = UINT32_MAX;  ///< no such path
    uint32_t any = kNone;      ///< to any return
    uint32_t zero = kNone;     ///< to a return of the constant zero
    uint32_t nonzero = kNone;  ///< to a return of a nonzero constant
};

/// One `goto block_X` the generator wrote.
struct EmittedGoto {
    uint32_t target;
    GotoArm  arm;
};

/** @brief What the C emitted under one label can do.
 *
 * A region is the text from one emitted label (a block start, or a mid-block
 * jump-table landing label) to the next label or the end of the function,
 * so a function's regions partition its emitted body. */
struct EmittedRegion {
    uint32_t label = 0;
    uint32_t insns = 0;          ///< instructions translated (branch and jump words excluded)
    bool loads = false;          ///< reads guest memory (lb..lwr, lwc2)
    bool stores = false;         ///< writes guest memory (sb..swr, swc2)
    bool opaque_call = false;    ///< calls code no leaf summary can describe: a PSYQ
                                 ///< or manual symbol, call_by_address on a register,
                                 ///< or psx_syscall
    bool indirect = false;       ///< call_by_address on a register (a jump table's
                                 ///< out-of-range default excepted) or psx_syscall
    std::vector<uint32_t> named;     ///< PSYQ or manual symbols called, one entry per call
    std::vector<uint32_t> callees;   ///< generated functions of this unit called, one entry per call
    std::vector<uint32_t> foreign;   ///< constant `call_by_address` targets, one entry per call;
                                     ///< each disqualifies a body unless a leaf summary of
                                     ///< another unit names it
    std::vector<EmittedGoto> gotos;  ///< in emission order
    bool falls_through = false;  ///< control continues into the next region in emission
                                 ///< order without a goto (a mid-block label, or a block
                                 ///< with no control-transfer exit)
    V0Test v0_test = V0Test::None;  ///< the v0 test of the region's exit branch
    V0Out v0_out = V0Out::None;     ///< v0 as the region leaves it, calls in emission order
};

/// The emitted regions of one generated function, in emission order.
struct FunctionFacts {
    uint32_t start = 0;
    std::string name;
    bool stub = false;           ///< body replaced by a translation-failure stub
    std::vector<EmittedRegion> regions;
};

/// One region of a loop body, as the register-carry test walks it.
struct LoopBodyRegion {
    uint32_t label = 0;
    std::vector<uint32_t> successors;  ///< labels of the body regions control reaches next
    bool calls = false;                ///< ends in a call of a pure leaf
};

/** @brief A loop back-edge whose body may receive a VBL delivery.
 *
 * The back-edge is a retreating `goto` whose target (the header) reaches the
 * region holding it (the source) in the function's region graph, so the
 * edge closes a cycle. The body is the header and every region the header
 * reaches that reaches the source without passing through the header: the
 * edge's natural loop when the header dominates the source, else every
 * region on a header-to-source path of the irreducible cycle. The body
 * stores nothing, calls only pure leaves, and it or one of its leaves
 * loads. */
struct LoopSite {
    std::string function;
    uint32_t header = 0;         ///< goto target
    uint32_t source = 0;         ///< label of the region holding the goto
    GotoArm arm = GotoArm::Taken;
    uint32_t insns = 0;          ///< body instructions plus each called leaf's, per call
    std::vector<uint32_t> leaves;    ///< pure leaves the body calls, one entry per call
    std::vector<LoopBodyRegion> body;  ///< the body regions, header first, then emission order
    bool pure = false;           ///< pure spin: no register is read before its first
                                 ///< definition in an iteration and defined in the body
                                 ///< (set by apply_loop_polls)
    bool device_poll = false;    ///< device-status poll (see classify_loops); `leaves`
                                 ///< then names the polled callee and `insns` counts
                                 ///< the body plus the callee's busy-path cost
};

/// A function of another unit as its summary states it.
struct LeafSummary {
    uint32_t insns = 0;          ///< a pure leaf's instructions; 0 for a closure row
    bool loads = false;
    bool leaf = true;            ///< a pure leaf; false for a row stating only a call closure
    bool status = false;         ///< the call closure reaches a device-status thunk
    ReturnCosts cost;            ///< shortest return-path costs (see classify_loops())
};

/** @brief The PSYQ and manual symbols the device-status poll criterion reads.
 *
 * Addresses come from the unit's symbol manifests by name. */
struct ThunkClasses {
    std::set<uint32_t> vsync;     ///< `VSync`
    std::set<uint32_t> status;    ///< device-status thunks: `CdSync`, `CdReady`,
                                  ///< `SpuIsTransferCompleted`, `MemCardSync`
    std::set<uint32_t> untraced;  ///< manual implementations, whose calls are not known
};

/** @brief Report whether a PSYQ symbol name is a device-status thunk.
 * @param name the manifest name, without prefix
 * @return true for `CdSync`, `CdReady`, `SpuIsTransferCompleted` and `MemCardSync` */
bool is_device_status_thunk(const std::string& name);

/** @brief Classify every back-edge of a translation unit.
 *
 * A back-edge is an emitted `goto` whose target is at or below the label of
 * the region it sits in and reaches that region (see LoopSite). The region
 * graph's edges are the gotos and the fall-throughs; its entry is the
 * function's first region; a region unreachable from the entry holds no
 * back-edge. A pure leaf is a function of the unit that is not a
 * stub, stores nothing, makes no call of any kind, and has no back-edge.
 * A constant `call_by_address` target qualifies as a leaf when `foreign`,
 * the pure leaves of another unit (the main EXE, which is resident whenever
 * an overlay runs), names it.
 *
 * A back-edge whose body fails that test is a device-status poll site when
 * the body stores nothing, makes exactly one call, to a generated function
 * (of the unit, or of `foreign`), and the back-edge is the branch of a
 * region whose only body predecessor is the call's continuation and whose
 * exit tests the callee's returned v0 against zero with no instruction
 * before it (EmittedRegion::v0_test). The callee's call closure must be
 * traced (no stub, no register or syscall call, no manual symbol, no
 * constant call target outside the unit and `foreign`; a jump table's
 * out-of-range default is not a call), must not call `VSync`, and must call
 * a device-status thunk. The callee's own stores are not constrained.
 *
 * A device-status poll site's insns is its body's instructions plus the
 * callee's busy-path cost: the fewest instructions on a path from the
 * callee's entry to a return of the v0 the back-edge loops on (a nonzero
 * constant for `bne`, zero for `beq`). A path's cost is the sum over its
 * regions of their translated instructions (branch and jump words excluded,
 * as for a leaf) plus, per call of a generated function, that function's
 * cheapest path to any return; a PSYQ or manual call costs 0. The v0 a
 * path returns is the last constant a region writes (EmittedRegion::v0_out);
 * a region ending in a tail call returns the callee's. A site whose callee
 * has no such path is not classified.
 * @param facts   every function the unit emitted
 * @param foreign pure leaves and call-closure rows of other units, by address
 * @param thunks  the unit's VSync, device-status and untraced symbols
 * @return the back-edges that qualify, in function then emission order */
std::vector<LoopSite> classify_loops(const std::vector<FunctionFacts>& facts,
                                     const std::map<uint32_t, LeafSummary>& foreign,
                                     const ThunkClasses& thunks);

/** @brief Write the unit's pure leaves and traced call closures as tab-separated rows.
 *
 * Columns: address, insns, loads (0 or 1), leaf (1 for a pure leaf), status
 * (1 when the call closure reaches a device-status thunk), then the return
 * costs any, zero, nonzero (`-` for no path; see classify_loops()). A pure
 * leaf writes `<insns> <loads> 1 0`; every other function whose call
 * closure is traced and calls no `VSync` writes `0 0 0 <status>`.
 * Read back by read_leaf_summary().
 * @param path    output file
 * @param facts   every function the unit emitted
 * @param foreign rows of other units the unit's closures resolve against
 * @param thunks  the unit's VSync, device-status and untraced symbols
 * @return false when the file cannot be opened */
bool write_leaf_summary(const std::string& path, const std::vector<FunctionFacts>& facts,
                        const std::map<uint32_t, LeafSummary>& foreign,
                        const ThunkClasses& thunks);

/** @brief Read a leaf summary written by write_leaf_summary().
 * @param path input file
 * @param out  receives one entry per row
 * @return false when the file cannot be opened */
bool read_leaf_summary(const std::string& path, std::map<uint32_t, LeafSummary>& out);

/** @brief Write the classified sites as tab-separated rows.
 *
 * Columns: function, header, source, arm, insns, leaves (comma-separated,
 * `-` when none), pure (the site kind apply_loop_polls() emits: 0 counted
 * poll, 1 pure spin, 2 device-status poll). A `#` header line names them.
 * @param path  output file
 * @param sites the sites classify_loops() returned
 * @return false when the file cannot be opened */
bool write_loop_sites(const std::string& path, const std::vector<LoopSite>& sites);

/** @brief Spell the placeholder line the generator writes before a back-edge `goto`.
 *
 * The line is a C comment naming the function, the goto's target, the label
 * of the region holding it and its arm; apply_loop_polls() replaces it with
 * the poll call when the back-edge is a classified site and removes it
 * otherwise.
 * @param indent   leading whitespace of the line
 * @param function the generated function's name
 * @param header   the goto's target
 * @param source   label of the region holding the goto
 * @param arm      which arm the goto is written as
 * @return the line, newline included */
std::string loop_poll_marker(const std::string& indent, const std::string& function,
                             uint32_t header, uint32_t source, GotoArm arm);

/** @brief Resolve every placeholder line of generated C against the classified sites.
 *
 * A placeholder whose back-edge is a site becomes
 * `psx_loop_poll(cpu, 0x<header>u, <insns>u, <kind>u);` at the same indent,
 * kind 0 for a counted poll, 1 for a pure spin and 2 for a device-status
 * poll;
 * every other placeholder line is removed, so the text without sites is the
 * text the generator writes with no placeholders at all. Each site's `pure`
 * is decided on the emitted text of its body regions: a register (not zero)
 * that some path from the header through the body reads before defining it,
 * and that the body defines, carries a value across iterations and makes
 * the site a counted poll; with no such register the site is a pure spin.
 * A device-status poll keeps kind 2 and its `pure` stays false: its callee's
 * reads are not seen.
 * A call of a leaf defines v0, v1, a0-a3, t0-t9 and at.
 * @param code  generated C holding placeholder lines
 * @param sites the sites classify_loops() returned for the same unit; each
 *              site's `pure` is set
 * @param emitted receives the number of poll calls written
 * @return the resolved C */
std::string apply_loop_polls(const std::string& code, std::vector<LoopSite>& sites,
                             size_t& emitted);

} // namespace PSXRecomp
