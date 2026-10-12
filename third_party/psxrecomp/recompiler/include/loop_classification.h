#pragma once

#include <cstdint>
#include <map>
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
    std::vector<uint32_t> callees;   ///< generated functions of this unit called, one entry per call
    std::vector<uint32_t> foreign;   ///< constant `call_by_address` targets, one entry per call;
                                     ///< each disqualifies a body unless a leaf summary of
                                     ///< another unit names it
    std::vector<EmittedGoto> gotos;  ///< in emission order
    bool falls_through = false;  ///< control continues into the next region in emission
                                 ///< order without a goto (a mid-block label, or a block
                                 ///< with no control-transfer exit)
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
};

/// A pure leaf as another unit's summary states it.
struct LeafSummary {
    uint32_t insns = 0;
    bool loads = false;
};

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
 * @param facts   every function the unit emitted
 * @param foreign pure leaves of other units, by address
 * @return the back-edges that qualify, in function then emission order */
std::vector<LoopSite> classify_loops(const std::vector<FunctionFacts>& facts,
                                     const std::map<uint32_t, LeafSummary>& foreign);

/** @brief Write the unit's pure leaves as tab-separated rows.
 *
 * Columns: address, insns, loads (0 or 1); read back by read_leaf_summary().
 * @param path  output file
 * @param facts every function the unit emitted
 * @return false when the file cannot be opened */
bool write_leaf_summary(const std::string& path, const std::vector<FunctionFacts>& facts);

/** @brief Read a leaf summary written by write_leaf_summary().
 * @param path input file
 * @param out  receives one entry per row
 * @return false when the file cannot be opened */
bool read_leaf_summary(const std::string& path, std::map<uint32_t, LeafSummary>& out);

/** @brief Write the classified sites as tab-separated rows.
 *
 * Columns: function, header, source, arm, insns, leaves (comma-separated,
 * `-` when none), pure (1 for a pure spin). A `#` header line names them.
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
 * `psx_loop_poll(cpu, 0x<header>u, <insns>u, <pure>u);` at the same indent;
 * every other placeholder line is removed, so the text without sites is the
 * text the generator writes with no placeholders at all. Each site's `pure`
 * is decided on the emitted text of its body regions: a register (not zero)
 * that some path from the header through the body reads before defining it,
 * and that the body defines, carries a value across iterations and makes
 * the site a counted poll; with no such register the site is a pure spin.
 * A call of a leaf defines v0, v1, a0-a3, t0-t9 and at.
 * @param code  generated C holding placeholder lines
 * @param sites the sites classify_loops() returned for the same unit; each
 *              site's `pure` is set
 * @param emitted receives the number of poll calls written
 * @return the resolved C */
std::string apply_loop_polls(const std::string& code, std::vector<LoopSite>& sites,
                             size_t& emitted);

} // namespace PSXRecomp
