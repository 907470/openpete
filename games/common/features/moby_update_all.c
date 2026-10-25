// games/common/features/moby_update_all.c - OPENPETE_D1, every live moby
// updated every tick; see moby_update_all.h.

#include "features/moby_update_all.h"

#include "game_layout.h"
#include "openpete_log.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"

#include <stdint.h>

#define UL (game_layout.update_list)
#define MB (game_layout.moby)

static int s_d1 = -1;       /* OPENPETE_D1, latched at apply-options time */
static int s_nosparx = -1;  /* OPENPETE_VIDEO_NOSPARX, latched with it */

/** @brief Whether `a` lies in main RAM's KSEG0 window.
 *
 * @param a A guest address.
 * @return 1 when it does, else 0. */
static int mua_in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

/** @brief Whether the moby at `m` is the companion OPENPETE_VIDEO_NOSPARX keeps off the list.
 *
 * @param cpu The guest CPU state.
 * @param m The moby's guest address.
 * @return 1 when the knob is on and the moby's class is game_layout.owner.sparx_class, else 0. */
static int mua_dropped(CPUState* cpu, uint32_t m) {
    return s_nosparx > 0 && game_layout.owner.sparx_class != 0u &&
           cpu->read_half(m + MB.class_off) == game_layout.owner.sparx_class;
}

/** @brief The first pass of the builder without its distance test: list every live moby, set the pod flags.
 *
 * A dormant moby (state bit 7) is skipped and the terminator (state -1)
 * ends the walk. An undrawn moby with update distance 0 is unlisted, as in
 * the stock builder; every other live moby is listed. A listed moby
 * without a pod is appended at `out`; one with a pod sets that pod's flag
 * and its slot is overwritten by the next append.
 * @param cpu The guest CPU state.
 * @param out The first free word of the update list.
 * @return The first free word after the pass. */
static uint32_t mua_list_mobys(CPUState* cpu, uint32_t out) {
    for (uint32_t m = cpu->read_word(MB.array_head);; m += MB.array_stride) {
        const int8_t st = (int8_t)cpu->read_byte(m + MB.state_off);
        if (st < 0) {
            if (st == -1) break;
            continue;
        }
        const int8_t pod = (int8_t)cpu->read_byte(m + UL.pod_off);
        if (cpu->read_byte(m + MB.drawn_off) == 0u) {
            cpu->write_byte(m + UL.listed_off, 0u);
            if (cpu->read_byte(m + UL.dist_off) == 0u) continue;
        }
        if (mua_dropped(cpu, m)) {
            cpu->write_byte(m + UL.listed_off, 0u);
            continue;
        }
        cpu->write_word(out, m);
        cpu->write_byte(m + UL.listed_off, 1u);
        if (pod >= 0) cpu->write_byte(UL.pod_flags + (uint32_t)pod, 1u);
        else out += 4u;
    }
    return out;
}

/** @brief The builder's second pass: append every member of each flagged pod.
 *
 * Each pod's member list holds int16 moby indices, bit 15 set on the last.
 * Every member is written and marked listed; a dormant member's slot is
 * overwritten by the next one.
 * @param cpu The guest CPU state.
 * @param out The first free word of the update list.
 * @return The first free word after the pass. */
static uint32_t mua_list_pods(CPUState* cpu, uint32_t out) {
    const uint32_t lists = cpu->read_word(UL.pod_lists);
    const uint32_t base = cpu->read_word(MB.array_head);
    for (uint32_t k = 0; k < UL.pod_count; k++) {
        if (cpu->read_byte(UL.pod_flags + k) == 0u) continue;
        uint32_t ip = cpu->read_word(lists + 4u * k);
        int16_t idx;
        do {
            idx = (int16_t)cpu->read_half(ip);
            ip += 2u;
            const uint32_t m = base + (uint32_t)(idx & 0x7FFF) * MB.array_stride;
            cpu->write_word(out, m);
            if ((int8_t)cpu->read_byte(m + MB.state_off) >= 0) out += 4u;
            cpu->write_byte(m + UL.listed_off, 1u);
        } while (idx >= 0);
    }
    return out;
}

/** @brief Replace the update-list builder with the build that lists every live moby.
 *
 * Clears the pod flags, runs both passes and writes the terminating zero
 * word, the stores the stock builder makes minus its distance test.
 * @param cpu The guest CPU state.
 * @param addr The builder's entry (unused).
 * @return 1: the builder's body does not run. */
static int mua_on_builder(CPUState* cpu, uint32_t addr) {
    (void)addr;
    for (uint32_t a = 0; a < UL.pod_count; a += 4u) cpu->write_word(UL.pod_flags + a, 0u);
    uint32_t out = mua_list_mobys(cpu, UL.list);
    out = mua_list_pods(cpu, out);
    cpu->write_word(out, 0u);
    return 1;
}

void moby_update_all_note_entry(CPUState* cpu, uint32_t addr) {
    if (s_d1 <= 0) return;
    const uint32_t gem = game_layout.tally.gem_class;
    const uint32_t tab_word = game_layout.owner.moby_update;
    if (tab_word != 0u) {
        const uint32_t tab = cpu->read_word(tab_word);
        if (!mua_in_ram(tab) || addr != cpu->read_word(tab + 4u * gem)) return;
        if (cpu->read_byte(cpu->a0 + MB.drawn_off) == 0u) cpu->write_byte(cpu->a0 + MB.drawn_off, 1u);
        return;
    }
    if (UL.overlay_update == 0u) return;
    const uint32_t entry = cpu->read_word(UL.overlay_update);
    if (!mua_in_ram(entry) || addr != entry) return;
    for (uint32_t m = cpu->read_word(MB.array_head);; m += MB.array_stride) {
        const int8_t st = (int8_t)cpu->read_byte(m + MB.state_off);
        if (st == -1) break;
        if (st < 0) continue;
        if (cpu->read_half(m + MB.class_off) == gem && cpu->read_byte(m + MB.drawn_off) == 0u)
            cpu->write_byte(m + MB.drawn_off, 1u);
    }
}

static feature_dispatch_entry_t mua_dispatch[] = {
    { 0u, mua_on_builder, 0u },
    { 0u, NULL, 0u },
};

/** @brief Latch OPENPETE_D1 and OPENPETE_VIDEO_NOSPARX and bind the builder hook while D1 is on.
 *
 * The hook stays unbound (address 0 ends the table) while D1 is off or the
 * game's layout names no builder or keeps its moby array at a fixed
 * address. Warns when NOSPARX is set without D1, or D1 is set on a game
 * without a builder, since neither then changes anything.
 * @param opts Unused. */
static void mua_apply_options(const openpete_options_t* opts) {
    (void)opts;
    ENV_FLAG_LATCH(s_d1, "OPENPETE_D1");
    ENV_FLAG_LATCH(s_nosparx, "OPENPETE_VIDEO_NOSPARX");
    const int usable = UL.builder != 0u && MB.array_head != 0u && !MB.array_direct;
    if (s_d1 > 0 && !usable) {
        OPL_WARN_F("moby-update", "OPENPETE_D1 is set but this game names no update-list builder; ignored");
        s_d1 = 0;
    }
    if (s_nosparx > 0 && s_d1 <= 0)
        OPL_WARN_F("moby-update", "OPENPETE_VIDEO_NOSPARX is set without OPENPETE_D1; it acts only inside the D1 builder and changes nothing");
    mua_dispatch[0].addr = s_d1 > 0 ? UL.builder : 0u;
    if (s_d1 > 0)
        OPL_INFO_F("moby-update", "OPENPETE_D1: every live moby listed at 0x%08X%s",
                   UL.builder, s_nosparx > 0 ? ", companion class dropped" : "");
}

const feature_module_t moby_update_all_module = {
    .name           = "moby_update_all",
    .apply_options  = mua_apply_options,
    .dispatch_table = mua_dispatch,
};
