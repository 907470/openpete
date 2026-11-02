// core/runtime/runahead.c - runahead's snapshot and rollback timing, the
// prediction-exactness check, and the deferral and adoption counters of a
// speculation that ends away from its snapshot's park site.

#include "runahead.h"
#include "host_state_ops.h"
#include "run_internal.h"

#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "openpete_state.h"
#include "openpete_time.h"
#include "game_extras.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_fnv1a64.h"

#include <stdlib.h>
#include <string.h>

int ra_pc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_RA_PREDICT_CHECK");
        v = (e && *e && *e != '0') ? atoi(e) : 0;
        if (v < 0) v = 0;
    }
    return v;   /* 1 = hash check; 2 = + full led copy, dump diff ranges */
}
struct ra_pc_slot s_ra_pc[RA_PC_PENDING];
unsigned s_ra_pc_checked = 0, s_ra_pc_mismatches = 0, s_ra_pc_invalidated = 0;
/* Wall time of the last runahead snapshot save, added to its restore's for
 * the Mods panel's snapshot cost. */
static op_time_t s_ra_save_ns = 0;

int ra_snapshot_save(openpete_state_mem_t* snap, const CPUState* cpu,
                            unsigned frame) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_save(snap, cpu, frame);
    s_ra_save_ns = op_now() - t0;
    return rc;
}

int ra_snapshot_rollback(const openpete_state_mem_t* snap, CPUState* cpu,
                                unsigned* frame) {
    const op_time_t t0 = op_now();
    const int rc = openpete_state_mem_load(snap, cpu, frame, 1);
    if (rc == 0)
        openpete_mods_runahead_snapshot_note(
            (uint64_t)(s_ra_save_ns + (op_now() - t0)) / 1000u);
    return rc;
}
void ra_card_settle(int rc) {
    if (rc == 0 || rc == -2) openpete_memcard_spec_rollback();
    else openpete_memcard_spec_adopt();
}

void ra_pc_invalidate_all(void) {
    for (int i = 0; i < RA_PC_PENDING; i++)
        if (s_ra_pc[i].valid) { s_ra_pc[i].valid = 0; s_ra_pc_invalidated++; }
}
uint64_t ra_pc_hash(void) {
    uint64_t h = openpete_fnv1a64_init();
    h = openpete_fnv1a64_update(h, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
    h = openpete_fnv1a64_update(h, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
    /* The mod arena is inside the rollback boundary: a led/committed
     * split there is the same class of misprediction as one in RAM. */
    uint32_t used[OPENPETE_ARENA_RANGES];
    (void)openpete_mods_arena_used(used);
    for (unsigned r = 0; r < OPENPETE_ARENA_RANGES; r++)
        if (used[r])
            h = openpete_fnv1a64_update(h, openpete_mods_arena_range_bytes(r), used[r]);
    return h;
}

void ra_pc_record_led(void) {
    if (!ra_pc_on()) return;
    for (int i = 0; i < RA_PC_PENDING; i++) {
        if (s_ra_pc[i].valid) continue;
        s_ra_pc[i].target = openpete_frame;
        s_ra_pc[i].hash   = ra_pc_hash();
        s_ra_pc[i].valid  = 1;
        if (ra_pc_on() < 2) return;
        if (!s_ra_pc[i].led)
            s_ra_pc[i].led = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE + OPENPETE_PSX_SCRATCH_SIZE);
        if (!s_ra_pc[i].led) return;
        memcpy(s_ra_pc[i].led, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);
        memcpy(s_ra_pc[i].led + OPENPETE_PSX_RAM_SIZE, openpete_psx_get_scratch(),
               OPENPETE_PSX_SCRATCH_SIZE);
        return;
    }
}

#ifdef OPENPETE_HAS_PSYCROSS
/* Runahead's share of s_state_ops_deferred: [0] snapshots owed to a later
 * yield (ra_owed), [1] rollbacks whose speculation ended below a non-final
 * yield. */
unsigned long long s_ra_deferred[2];
unsigned long long s_ra_adopted = 0;  /* speculations adopted: no rollback at the snapshot's park site */
unsigned long long s_ra_left[2];  /* speculations that left gameplay: [0] rolled back, [1] adopted */

/** @brief Count one runahead snapshot (kind 0) or rollback (kind 1) deferred to a final yield.
 * @param kind 0 for a snapshot, 1 for a rollback. */
static void ra_count_deferral(int kind) {
    s_ra_deferred[kind]++;
    s_state_ops_deferred[kind]++;
    count_loop_deferral();
}

int ra_note_owed(int owe, int owed) {
    if (owe && !owed) ra_count_deferral(0);
    return owe;
}

int ra_speculate_to_final(openpete_park_site_t site, int cap, int* left) {
    if (openpete_park_site_matches(&site)) return 0;
    ra_count_deferral(1);
    if (openpete_yield_final) s_site_deferred[SITE_DEF_RUNAHEAD]++;
    OPL_DEBUG_F("runahead", "speculation ended at f=%u %s; rollback deferred to the "
                "snapshot's park site", openpete_frame,
                openpete_yield_final ? "at another park site" : "below an intermediate yield");
    for (int v = 0; v < cap && !(*left && openpete_yield_final); v++) {
        game_on_frame_pre(&g_game.cpu, openpete_frame);
        g_openpete_guest_running = 1;
        openpete_fiber_switch(openpete_game_fiber);
        g_openpete_guest_running = 0;
        if (!game_is_gameplay_live(&g_game.cpu)) *left = 1;
        if (openpete_park_site_matches(&site)) return 0;
    }
    s_ra_adopted++;
    return 1;
}

void ra_end_log(int left, int adopted) {
    if (adopted) openpete_memcard_spec_adopt();
    if (left) s_ra_left[adopted ? 1 : 0]++;
    if (!adopted)
        OPL_INFO_F("runahead", "speculation left gameplay; rolled back at the snapshot's "
                   "park site (led f=%u)", openpete_frame);
    else if (left)
        OPL_INFO_F("runahead",
                   "speculation left gameplay and parked at another site than the "
                   "snapshot's; adopted the led timeline through f=%u (runahead idle "
                   "until gameplay resumes)", openpete_frame);
    else
        OPL_WARN_F("runahead",
                   "speculation reached no final yield at the snapshot's park site within "
                   "its bound; adopted the led timeline through f=%u", openpete_frame);
}

#endif /* OPENPETE_HAS_PSYCROSS */
