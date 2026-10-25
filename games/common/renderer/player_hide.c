// games/common/renderer/player_hide.c - which native draws are the player's;
// see player_hide.h.
//
// Particle ownership. A particle record carries its type byte, class byte,
// age, drawn stamp, position, colour, size and velocity, and nothing that
// names its spawner; the type byte is shared, for example Spyro 3's player
// update and many level actors both spawn types 0x2E and 0x29, and Sparx and
// another actor both spawn 0x42. The owner is therefore taken where the game
// itself decides it, at the spawn's call site: every spawn allocates its slot
// through one allocator (game_layout.owner.part_alloc), whose hook in the
// particle feeder hands the slot to player_hide_note_alloc, and the slot is
// tagged with the owner of the guest call subtree that is running then.
// player_hide_note_entry keeps that owner from the function entries the
// dispatch preamble reports: the player's update call, or the companion's
// moby update call, from its entry until the first entry at or above the
// stack pointer it was entered at, which is the first call its caller makes
// after it returned.
//
// Tags live in a host table indexed by pool slot. A slot keeps its tag until
// the allocator hands it out again; a slot is live only after an allocation,
// so a stale tag is never read for a live record that the tracking saw
// allocated. Records allocated while tracking was off carry no tag.

#include "renderer/player_hide.h"

#include "renderer/env_knob.h"
#include "game_layout.h"
#include "openpete_native_render.h"   /* openpete_native_get_hide_player, openpete_native_hide_class */
#include "renderer/particles.h"       /* particles_slot: the pool slot index */
#include "psx_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define OW (game_layout.owner)

#define PH_OWNER_NONE   0u
#define PH_OWNER_PLAYER 1u
#define PH_OWNER_SPARX  2u

/* Pool slots the tag table covers; the Spyro 2 and 3 pools are 0x2000 bytes
 * of 0x20-byte records, 256 slots. */
#define PH_SLOTS_MAX 1024u

static uint8_t  s_owner[PH_SLOTS_MAX];
static uint8_t  s_ctx;        /* owner of the running call subtree */
static uint32_t s_ctx_sp;     /* the guest sp its call was entered at */
static uint32_t s_ctx_lo;     /* its entry address */
static uint32_t s_ctx_hi;     /* the address of its frame allocation */
static uint32_t s_tagged[3];  /* allocations tagged per owner, for the census */
static uint32_t s_entered[3]; /* owner calls entered, for the census */

/** @brief Whether `a` lies in main RAM's KSEG0 window.
 *
 * @param a A guest address.
 * @return 1 when it does, else 0. */
static int ph_in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80800000u; }

int player_hide_census_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("OPENPETE_PLAYER_HIDE_CENSUS") != NULL;
    return v;
}

int player_hide_active(void) { return openpete_native_get_hide_player(); }

int player_hide_tracking(void) {
    return OW.part_alloc != 0u && (player_hide_active() || player_hide_census_on());
}

/* The longest prologue ph_frame_alloc scans for the frame allocation. */
#define PH_PROLOGUE_MAX 32u

/** @brief The address of the first `addiu $sp, $sp, -n` at or after `entry`, read from guest code.
 *
 * The recompiler may split a function's body before its frame allocation
 * into a piece of its own, entered through the dispatch at the caller's
 * stack pointer; such a piece starts no later than this address.
 * @param cpu Reads guest code.
 * @param entry The function's entry address.
 * @return The address, or `entry` when none lies in the first
 *         PH_PROLOGUE_MAX instructions. */
static uint32_t ph_frame_alloc(CPUState* cpu, uint32_t entry) {
    for (uint32_t k = 0; k < PH_PROLOGUE_MAX; k++) {
        const uint32_t w = cpu->read_word(entry + k * 4u);
        if ((w >> 16) == 0x27BDu && (w & 0x8000u)) return entry + k * 4u;
    }
    return entry;
}

/** @brief Make `owner` the owner of the call entered at `addr` with the current stack pointer.
 *
 * @param cpu The guest CPU state at the entry.
 * @param addr The entered function's address.
 * @param owner A PH_OWNER_* value. */
static void ph_enter(CPUState* cpu, uint32_t addr, uint8_t owner) {
    s_ctx = owner;
    s_ctx_sp = cpu->sp;
    s_ctx_lo = addr;
    s_ctx_hi = ph_frame_alloc(cpu, addr);
    s_entered[owner]++;
}

void player_hide_note_entry(CPUState* cpu, uint32_t addr) {
    if (s_ctx != PH_OWNER_NONE) {
        if (cpu->sp < s_ctx_sp) return;          /* inside the owner's frame */
        if (cpu->sp == s_ctx_sp && addr >= s_ctx_lo && addr <= s_ctx_hi)
            return;                              /* the owner's own pre-frame piece */
        s_ctx = PH_OWNER_NONE;
    }
    if (addr == OW.player_update) {
        ph_enter(cpu, addr, PH_OWNER_PLAYER);
        return;
    }
    if (!OW.moby_update || !OW.sparx_class) return;
    const uint32_t tbl = cpu->read_word(OW.moby_update);
    if (!ph_in_ram(tbl)) return;
    if (addr != cpu->read_word(tbl + OW.sparx_class * 4u)) return;
    if (!ph_in_ram(cpu->a0)) return;
    if (cpu->read_half(cpu->a0 + game_layout.moby.class_off) != OW.sparx_class) return;
    ph_enter(cpu, addr, PH_OWNER_SPARX);
}

/** @brief The pool slot index of the record at `rec`, or -1 outside the tag table.
 *
 * @param rec The record's guest address.
 * @return The slot index, or -1. */
static int ph_slot(uint32_t rec) {
    const int idx = particles_slot(rec);
    return idx < (int)PH_SLOTS_MAX ? idx : -1;
}

/* Drawn records per owner and type byte, for the census. */
static uint32_t s_type_hist[3][256];

int player_hide_particle_owner(CPUState* cpu, uint32_t rec) {
    const int i = ph_slot(rec);
    const int owner = i < 0 ? 0 : (int)s_owner[i];
    if (player_hide_census_on()) s_type_hist[owner][cpu->read_byte(rec)]++;
    return owner;
}

void player_hide_note_alloc(int slot, int pool_moved) {
    if (pool_moved) memset(s_owner, 0, sizeof s_owner);
    if (!player_hide_tracking() || slot < 0 || slot >= (int)PH_SLOTS_MAX) return;
    s_owner[slot] = s_ctx;
    s_tagged[s_ctx]++;
}

int player_hide_is_sparx_class(uint32_t cls) {
    return OW.sparx_class != 0u && cls == OW.sparx_class;
}

/* Faces and shadow triangles --hide-classes dropped over the run, for the
 * shutdown engagement line. */
static uint32_t s_class_faces;
static uint32_t s_class_shadow_tris;

int player_hide_class_face(uint32_t cls, const void* tri, size_t len) {
    if (!openpete_native_hide_class(cls)) return 0;
    player_hide_count_data(PH_CAT_CLASS, 1, tri, len);
    s_class_faces++;
    return 1;
}

int player_hide_moby_face(int actor, uint32_t cls, const void* tri, size_t len) {
    if (player_hide_class_face(cls, tri, len)) return 1;
    if (!actor) return 0;
    const int sparx = player_hide_is_sparx_class(cls);
    const int skip = sparx && player_hide_active();
    player_hide_count_data(sparx ? PH_CAT_SPARX : PH_CAT_OTHER_MOBY, skip, tri, len);
    return skip;
}

int player_hide_class_shadow(CPUState* cpu, uint32_t moby) {
    if (!ph_in_ram(moby)) return 0;
    if (!openpete_native_hide_class(cpu->read_half(moby + game_layout.moby.class_off))) return 0;
    player_hide_count(PH_CAT_CLASS_SHADOW, 4, 1);
    s_class_shadow_tris += 4u;
    return 1;
}

int player_hide_is_sparx_moby(CPUState* cpu, uint32_t moby) {
    if (!OW.sparx_class || !ph_in_ram(moby)) return 0;
    return cpu->read_half(moby + game_layout.moby.class_off) == OW.sparx_class;
}

uint32_t player_hide_sparx_glow(CPUState* cpu) {
    if (!OW.sparx_class || !OW.sparx_glow_off) return 0u;
    const uint32_t head = game_layout.moby.array_head;
    const uint32_t arr = game_layout.moby.array_direct ? head : cpu->read_word(head);
    if (!ph_in_ram(arr)) return 0u;
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t m = arr + i * game_layout.moby.array_stride;
        const uint8_t st = cpu->read_byte(m + game_layout.moby.state_off);
        if (st == game_layout.moby.state_term) break;
        if (st & 0x80u) continue;                /* dormant */
        if (cpu->read_half(m + game_layout.moby.class_off) != OW.sparx_class) continue;
        const uint32_t tag = cpu->read_word(m);
        if (!ph_in_ram(tag)) return 0u;
        const uint32_t rec = cpu->read_word(tag + OW.sparx_glow_off);
        return ph_in_ram(rec) ? rec : 0u;
    }
    return 0u;
}

/* ---- census --------------------------------------------------------------- */

static const char* const k_cat_name[PH_CAT_N] = {
    "player", "flame", "player_shadow", "sparx", "sparx_shadow", "sparx_glow",
    "player_part", "sparx_part", "other_part", "other_moby", "other_glow",
    "class", "class_shadow",
};

static uint32_t s_cnt[PH_CAT_N][2];
static uint32_t s_hash[PH_CAT_N];
static int      s_cnt_vbl = -1;

#define PH_FNV_BASIS 2166136261u
#define PH_FNV_PRIME 16777619u

/** @brief Print, per owner, the type bytes of the drawn particle records the census counted.
 *
 * The type byte is the spawn's second argument, which SpawnParticle stores
 * at +0x00, so the list per owner is checkable against the types that
 * owner's spawn sites pass. */
static void ph_type_report(void) {
    if (!player_hide_census_on()) return;
    static const char* const k_owner[3] = { "other", "player", "sparx" };
    for (int o = 0; o < 3; o++) {
        fprintf(stderr, "[player-hide-types] %s:", k_owner[o]);
        for (int t = 0; t < 256; t++)
            if (s_type_hist[o][t]) fprintf(stderr, " %02X=%u", t, s_type_hist[o][t]);
        fputc('\n', stderr);
    }
}

/** @brief Print the counts accumulated for s_cnt_vbl as one line, then clear them. */
static void ph_census_flush(void) {
    if (s_cnt_vbl < 0) return;
    fprintf(stderr, "[player-hide f=%d] hide=%d entered=%u/%u tagged=%u/%u/%u", s_cnt_vbl,
            player_hide_active(), s_entered[1], s_entered[2],
            s_tagged[0], s_tagged[1], s_tagged[2]);
    for (int c = 0; c < PH_CAT_N; c++)
        fprintf(stderr, " %s=%u/%u", k_cat_name[c], s_cnt[c][0], s_cnt[c][1]);
    fputs(" hash", stderr);
    for (int c = 0; c < PH_CAT_N; c++) fprintf(stderr, " %08X", s_hash[c]);
    fputc('\n', stderr);
    memset(s_cnt, 0, sizeof s_cnt);
    for (int c = 0; c < PH_CAT_N; c++) s_hash[c] = PH_FNV_BASIS;
    s_cnt_vbl = -1;
}

/** @brief Whether the census counts at the current VBL, opening its line when the VBL changed.
 *
 * @param cat The category about to be counted.
 * @return 1 when the draw is counted, else 0. */
static int ph_census_take(int cat) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!player_hide_census_on() || cat < 0 || cat >= PH_CAT_N) return 0;
    if (!env_frame_range_active(&s_range, "OPENPETE_PLAYER_HIDE_CENSUS", openpete_frame)) return 0;
    if (s_cnt_vbl != (int)openpete_frame) {
        ph_census_flush();
        for (int c = 0; c < PH_CAT_N; c++) s_hash[c] = PH_FNV_BASIS;
        s_cnt_vbl = (int)openpete_frame;
    }
    return 1;
}

void player_hide_count(int cat, int n, int skipped) {
    if (n <= 0 || !ph_census_take(cat)) return;
    s_cnt[cat][skipped ? 1 : 0] += (uint32_t)n;
}

void player_hide_count_data(int cat, int skipped, const void* data, size_t len) {
    if (!ph_census_take(cat)) return;
    s_cnt[cat][skipped ? 1 : 0]++;
    const uint8_t* b = (const uint8_t*)data;
    uint32_t h = s_hash[cat];
    for (size_t k = 0; k < len; k++) h = (h ^ b[k]) * PH_FNV_PRIME;
    s_hash[cat] = h;
}

/* ---- module --------------------------------------------------------------- */

/** @brief Print the pending census line, the per-owner type report and the --hide-classes engagement line at shutdown.
 *
 * The engagement line prints whenever a face or shadow was dropped. */
static void player_hide_shutdown(void) {
    ph_census_flush();
    ph_type_report();
    if (s_class_faces || s_class_shadow_tris)
        fprintf(stderr, "hide-classes: faces_skipped=%u shadow_tris_skipped=%u\n",
                s_class_faces, s_class_shadow_tris);
}

const feature_module_t player_hide_module = {
    .name           = "player_hide",
    .shutdown       = player_hide_shutdown,
};
