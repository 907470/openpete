/* games/spyro-1/renderer/interp_regions.c — Spyro 1's render-side
 * interpolation region table. Captured per tick into the openpete_interp
 * ring so intermediate presents can lerp transforms: the DIRECT singleton
 * regions below (camera, player, flame, shadow, game state) plus the
 * per-object capture: the shared moby-array walk over g_LevelMobys, then
 * spyro_interp_extra_pools (HUD-pool mobys, particles, sparkles, glows, the
 * two render lists and the dragon-star record) keyed by
 * game_interp_moby_key.
 *
 * Registration model:
 *   This TU defines game_interp_register(), which hands the descriptor rows
 *   below to the shared generator (renderer/interp_regions_gen.h); the
 *   generator assigns each row its ring region index and registers the built
 *   table. main.cpp calls it once at startup before openpete_run(). The
 *   weak-symbol override pattern is avoided because MinGW's PE/COFF linker
 *   mis-resolves it, leaving the weak NULL in place at runtime and crashing
 *   the recomp boot path; explicit registration works identically across
 *   toolchains.
 *
 * Lengths match games/spyro-1/manifest/gameplay_state_regions.toml so the
 * same boundaries the determinism gate trusts are the boundaries snapshotted.
 */

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "renderer/interp_regions_gen.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "renderer/interp_regions.h"
#include "renderer/reextract.h"   /* moby_native_canon_drawn_mobys */
#include "renderer/text_moby_id.h"    /* text_moby_id_lookup */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "game_layout.h"

/* DIRECT singleton regions — captured whole into the interp ring each tick.
 * The camera row is added at registration (its span is a literal, not the
 * cam.base..xform_end descriptor span the generator's other users compute);
 * these are the plain-global rows.
 *   g_Spyro  (0x2A4): the player avatar — position/rotation/anim for the
 *                     lerp + animation-clock feeders. Singleton, so it lives
 *                     here rather than in the per-object (indirect) table. */
static const game_interp_row_t s_spyro_interp_rows[] = {
    { "g_Spyro",  0x80078A58, 0x2A4 },
    /* g_SpyroFlame (0xD0 captured): the flame-breath struct. The ribbon is
     * rigidly attached to Spyro's mouth/head via m_Position (+0x00) and the
     * flame-attach matrix (+0xB8), both updated at the 30 Hz tick — captured so
     * the sub-tick spine can lerp the flame POSE to track the (also-interpolated)
     * Spyro instead of snapping each tick. Read-only; rendered only while
     * breathing fire. */
    { "g_SpyroFlame", 0x800786C8, 0xD0 },
    /* SpyroShadow (0x28 captured): the player's drop-shadow scratch. Its ground
     * anchor (m_Position @ +0x10, int32 x/y/z) is computed each tick by the
     * canonical shadow render and tracks Spyro's ground projection — captured so
     * the sub-tick spine can lerp it to follow the (interpolated) Spyro instead of
     * stepping at 30 Hz. (Moby shadows need no capture: they re-derive from each
     * moby's already-interpolated m_Position.) Read-only. */
    { "g_SpyroShadow", 0x8007AA10, 0x28 },
    /* g_GameState (4 bytes): which simulation regime produced the tick —
     * gameplay, level transition, cutscene, pause. A pair whose two ticks
     * came from different regimes is a discontinuity for the presentation
     * bracket: the camera re-targets on the edge (entering a portal turns it
     * ~20° and moves it ~500 units in one tick — a real cut, but under the
     * 90°/5000-unit thresholds that catch teleports), so the re-extract snaps
     * instead of lerping the chord of the swing. Committed timeline, like the
     * camera it gates: a led copy could flip a tick before the camera pair it
     * is compared against. */
    { "g_GameState", 0x800757D8, 0x4, 1 },
};

/* --- per-object (indirect) capture: the g_LevelMobys array --------------- */

#define MOBY_STRIDE    0x58u         /* stride between live moby records */
#define MOBY_STATE_OFF 0x48u         /* +0x48: 0xFF = array sentinel, >=0x80 dead */
#define G_HUD_COUNT    12u           /* gem/dragon/Spyro-head/key/digit icons (they spin) */
#define PART_STRIDE    0x20u         /* particle record stride */
#define PART_HDR_SEL   8u            /* (hdr>>8)&0xFF: m_Type/sel; 0xFF=end, &0x80=inactive */
#define PART_MAX       320u          /* pool walk bound */
/* Entries the pool can hold. The inventory page emits ~110 (level rows +
 * counters); the shaded list the drain feeds is Moby*[256], so 256 is the
 * real ceiling. Doubles as the pool-membership test: g_LevelMobys lives
 * ~150 KB below the pool top, far outside this span. */
#define HUD_POOL_MAX   256u
#define G_HUD_POOL_TOP  0x800756FCu  /* D_800756FC — pool top; entries bump DOWN */
#define G_TRANS_HUD     0x800756B0u  /* g_LevelTransHudActive */
#define G_TRANS_GEMS    0x8007571Cu  /* g_LevelTransGems — TransGem[64] @ g_SonyImage+0x2800 */
#define G_RECENT_GEMS   0x80077DB8u  /* g_RecentGemsCollected[32] — u_char class per gem */
#define TRANS_GEM_MAX   32u          /* both the fly-out update and the draw loop stop at 32 */
#define G_GAMESTATE     0x800757D8u  /* g_Gamestate */
#define G_CREDITS_STAGE 0x80075704u  /* g_CreditsStage: <99 = the overlay's letter roll */
#define G_CREDITS_RECS  0x8007589Cu  /* D_8007589C — CreditsLetter[] (overlay space) */
#define G_CREDITS_COUNT 0x800756E4u  /* D_800756E4 — records in use */
#define CREDITS_REC_STRIDE 0x1Cu
#define CREDITS_REC_STATE  0x04u     /* 1 flying in, 2 settled, 0 departing (= free for reuse) */
#define CREDITS_REC_MAX    128u
#define GS_CREDITS      15u

static uint32_t rd32(uint32_t addr);

/* Credits letter roll (func_credits_8007BFD0, GS_Credits with g_CreditsStage
 * < 99): the draw allocates exactly one pool moby per CreditsLetter record,
 * in record order, so a moby's absolute pool ordinal IS its record index.
 * The update allocates a new letter into the first record whose state byte
 * is 0 (a letter still flying out) and otherwise appends, so the index is
 * stable for a letter's whole life but is handed to the next line's letter
 * the tick the old one is overwritten. A same-class ordinal fails here: the
 * old line's letters sit at lower indices than the settled title, and each
 * one that departs renames every later letter of its class, pairing a
 * settled letter with the departed one's rotation. Reuse of an index is
 * observed once per tick at capture as the state byte returning to 1
 * (flying in) and bumps a per-index generation folded into the key, so a
 * new letter never pairs with the one it replaced. */
static uint8_t s_credits_state[CREDITS_REC_MAX];
static uint8_t s_credits_gen[CREDITS_REC_MAX];

static int credits_roll_active(void) {
    return rd32(G_GAMESTATE) == GS_CREDITS && (int32_t)rd32(G_CREDITS_STAGE) < 99;
}

static void credits_track_generations(void) {
    if (!credits_roll_active()) {
        memset(s_credits_state, 0, sizeof(s_credits_state));
        return;
    }
    uint32_t recs = rd32(G_CREDITS_RECS), cnt = rd32(G_CREDITS_COUNT);
    if (recs < 0x80000000u || cnt > CREDITS_REC_MAX) return;
    for (uint32_t k = 0; k < CREDITS_REC_MAX; k++) {
        const uint8_t* r = (k < cnt) ? (const uint8_t*)openpete_psx_to_host_ptr(recs + k * CREDITS_REC_STRIDE) : NULL;
        uint8_t st = r ? r[CREDITS_REC_STATE] : 0;
        if (st == 1u && s_credits_state[k] != 1u) s_credits_gen[k]++;
        s_credits_state[k] = st;
    }
}

/* Identities tried in the order renderer/interp_regions.h lists them. */
uint32_t game_interp_moby_key(uint32_t m) {
    /* Credits letters: (class, record index, generation) — see
     * credits_track_generations. */
    {
        uint32_t top = rd32(G_HUD_POOL_TOP);
        uint32_t delta = (top >= 0x80000000u && m < top) ? top - m : 0u;
        if (delta != 0u && delta <= CREDITS_REC_MAX * MOBY_STRIDE &&
            (delta % MOBY_STRIDE) == 0u && credits_roll_active()) {
            uint32_t k = delta / MOBY_STRIDE - 1u;
            const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_ptr(m);
            uint16_t cls = 0; if (mh) memcpy(&cls, mh + 0x36u, 2);
            return 0x60000000u | ((uint32_t)(cls & 0x3FFu) << 16) |
                   ((uint32_t)(s_credits_gen[k] & 0x3Fu) << 10) | (k & 0x3FFu);
        }
    }

    /* Text glyphs first: a glyph emitted by func_80017FE4 / func_800181AC has a
     * real provenance identity — "the Nth character of the Mth call from
     * callsite C" — recorded by text_moby_id.c. It beats every position-derived
     * key here because it is blind to unrelated HUD-pool traffic: the inventory
     * page's blinking navigation arrows change the pool ordinal of every glyph
     * behind them twice a second, which renamed ~100 glyphs mid-menu and made
     * each pair with its neighbour's previous position (the "0" of a gem count
     * sliding into the "0" of the dragon count and back). */
    {
        uint32_t tk;
        if (text_moby_id_lookup(m, &tk)) return tk;
    }

    /* Level-exit gem fly-out identity. At a level exit the game animates the
     * collected gems flying to the chest; the shaded pass stages a moby COPY
     * per active gem into the double-buffered polybuf, in
     * source-index order. When a gem finishes and DEACTIVATES the copies COMPACT
     * (every later copy shifts a slot), so no copy-address / slot key is stable —
     * the copies mispair across ticks and the gems jiggle under --render-fps.
     *
     * The game itself keeps a STABLE source array: g_LevelTransGems[i]
     * (TransGem[64] @ g_SonyImage+0x2800, live ptr @ 0x8007571C; loaders.h). Each
     * gem's slot i is a permanent identity for the whole animation, and draw.c
     * copies g_LevelTransGems[i].rot (+0x0A) VERBATIM into the copy's m_Rotation
     * (+0x44). rot.x/rot.y are random-per-gem constants set once at spawn
     * (update.c) — an EXACT per-gem fingerprint (rot.z spins; included for extra
     * bits, matched live). So recover i by matching the copy's 3 rotation bytes
     * to the live source and key by i: prev/curr then pair as the same gem across
     * ticks AND the buffer swap, with no threshold. Runs for every gem class
     * (GEM_1..GEM_25, 0x53..0x57 — the copy's class is g_RecentGemsCollected[i],
     * so the tally is whatever mix the player picked up) while
     * g_LevelTransHudActive (0x800756B0). OPENPETE_GEM_FLYOUT_KEY=0 reverts.
     *
     * The match is EXACT, not a nearest-fit: rotation fingerprint AND the copy's
     * screen Y, which draw.c writes as 198 - yOffset with no other term, AND the
     * class the source slot recorded. Two gems cannot satisfy all three. */
    {
        static int s_fly = -1;
        if (s_fly < 0) { const char* e = getenv("OPENPETE_GEM_FLYOUT_KEY"); s_fly = (e && *e) ? (*e != '0') : 1; }
        const uint8_t* mh0 = s_fly ? (const uint8_t*)openpete_psx_to_host_ptr(m) : NULL;
        uint16_t cls0 = 0; if (mh0) memcpy(&cls0, mh0 + 0x36, 2);
        if (mh0 && cls0 >= 0x53u && cls0 <= 0x57u) {
            uint32_t hud_active = rd32(G_TRANS_HUD);
            uint32_t arr = rd32(G_TRANS_GEMS);
            if (hud_active && arr >= 0x80000000u) {
                uint8_t cr0 = mh0[0x44], cr1 = mh0[0x45], cr2 = mh0[0x46];
                int32_t py; memcpy(&py, mh0 + 0x10u, 4);          /* m_Position.y */
                const uint8_t* recent = (const uint8_t*)openpete_psx_to_host_ptr(G_RECENT_GEMS);
                for (uint32_t i = 0; i < TRANS_GEM_MAX; i++) {
                    const uint8_t* g = (const uint8_t*)openpete_psx_to_host_ptr(arr + i * 0x10u);
                    if (!g) break;
                    if (g[0x08] == 0) continue;                   /* inactive slot */
                    if (g[0x0A] != cr0 || g[0x0B] != cr1 || g[0x0C] != cr2) continue;
                    int16_t yoff; memcpy(&yoff, g + 0x02u, 2);
                    if (py != 198 - (int32_t)yoff) continue;
                    if (recent && recent[i] != (uint8_t)cls0) continue;
                    return 0xA0000000u | (i & 0x3Fu);             /* stable per-gem key */
                }
                /* no active source match (transient staging) → fall through */
            }
        }
    }

    /* Everything else in the HUD pool: key by (class, ordinal among entries of
     * the SAME class). The pool bump-allocates DOWN from D_800756FC in draw
     * order and is rebuilt from scratch every tick, so an entry's ABSOLUTE
     * ordinal is not an identity — anything emitted earlier that appears or
     * disappears renames every entry behind it. A level exit does that on every
     * single tick: the tally despawns a gem as it lands in the chest, and the
     * counter text loses a digit as it counts down. An absolute ordinal
     * therefore hands the chest and every odd-valued gem a fresh key each tick,
     * so they never have a prev to pair with and step at the 29.97 Hz sim rate
     * while the gems around them (which own a source identity, above) run smooth.
     *
     * Counting only same-class predecessors makes the ordinal blind to that
     * traffic: the chest is "the only class-473", a lone GEM_5 "the only 0x55",
     * whatever else the tally emits. It is the strongest identity available
     * without a per-object source array — same-class entries still shift among
     * themselves, which is exactly the case the gem fingerprint above covers.
     * Class stays folded into the key so a class change is a clean snap rather
     * than a blend between two different objects.
     *
     * Read D_800756FC, not the live g_HudMobys: the drain at the end of each
     * draw walks the cursor back to the top, so the two agree at capture time,
     * but only the top is stable if a re-extract observes a draw in progress.
     * The pool alternates between the two polybuf halves each tick; a
     * top-relative ordinal is identical in both. */
    {
        uint32_t top = rd32(G_HUD_POOL_TOP);
        uint32_t delta = (top >= 0x80000000u && m < top) ? top - m : 0u;
        if (delta != 0u &&
            delta <= HUD_POOL_MAX * MOBY_STRIDE && (delta % MOBY_STRIDE) == 0u) {
            uint32_t ord = delta / MOBY_STRIDE - 1u;   /* 0 = first entry allocated */
            const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_ptr(m);
            uint16_t cls = 0; if (mh) memcpy(&cls, mh + 0x36u, 2);
            uint32_t n = 0;
            for (uint32_t k = 0; k < ord; k++) {
                const uint8_t* e = (const uint8_t*)openpete_psx_to_host_ptr(top - (k + 1u) * MOBY_STRIDE);
                if (!e) break;
                uint16_t c2; memcpy(&c2, e + 0x36u, 2);
                if (c2 == cls) n++;
            }
            return 0x40000000u | ((uint32_t)(cls & 0x3FFu) << 16) | (n & 0xFFFFu);
        }
    }
    return m;
}

extern unsigned openpete_frame;

static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_ptr(addr);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief The captured state outside g_LevelMobys.
 *
 * It covers the HUD-icon and particle pools, the sparkle, glow and dragon-star
 * records, the rescue cutscene's state and tick counter (while g_GameState is
 * 8), and the two render lists whose entries the array walk cannot reach. Runs after the shared
 * moby-array walk (game_interp_capture_moby_array), inside the same begin/end
 * tick pair.
 *
 * The credits generation pass comes first because every game_interp_moby_key
 * call below depends on it; the array walk keys by address and does not. */
static void spyro_interp_extra_pools(void) {
    credits_track_generations();

    /* Static 3D HUD icons (g_Hud.m_Mobys[12]) — stride-0x58 Mobys OUTSIDE
     * g_LevelMobys. They spin (m_Rotation @ +0x44); capture them so the sub-tick
     * spine can lerp the spin (else it steps at 30 Hz while the rest is smooth).
     * Always present (no sentinel) — capture all 12; inactive ones don't render. */
    for (uint32_t i = 0; i < G_HUD_COUNT; ++i) {
        uint32_t m = G_HUD_ICONS + i * MOBY_STRIDE;
        const void* host = openpete_psx_to_host_ptr(m);
        if (host) openpete_interp_objs_capture(m, host, openpete_interp_objs_blob_bytes());
    }

    /* g_Particles pool (smoke / triangles / line particles around the flame and
     * elsewhere): a stride-0x20 array behind a pointer. Capture each ACTIVE slot
     * keyed by its PSX address so the sub-tick spine can lerp particle positions.
     * Particles spawn into INACTIVE slots, so a fresh particle gets have_prev=0
     * (snap, no streak); despawned slots go unseen → evicted. NOT glows (derive
     * from their owner moby); g_Sparkles is captured separately below (phase-only
     * interp). Capture 0x20 = the full record (pos @ +0x04 X/Y, +0x08 Z). */
    uint32_t ppool = rd32(G_PARTICLES);
    if (ppool >= 0x80000000u) {
        for (uint32_t i = 0; i < PART_MAX; ++i) {
            uint32_t pa = ppool + i * PART_STRIDE;
            const void* host = openpete_psx_to_host_ptr(pa);
            if (!host) break;
            uint32_t hdr; memcpy(&hdr, host, 4);
            uint8_t sel = (uint8_t)((hdr >> PART_HDR_SEL) & 0xFFu);
            if (sel == 0xFFu) break;            /* end of pool */
            if (sel & 0x80u) continue;          /* inactive slot */
            openpete_interp_objs_capture(pa, host, PART_STRIDE);
        }
    }

    /* g_Sparkles pool (gem/chest/statue twinkles): a FIXED Sparkle[8] array,
     * stride 0x18. A sparkle's POSITION is static for its whole life — what
     * steps at the 29.97 Hz sim rate is the twinkle ANIMATION phase: m_Life
     * (pulse scale, decremented per tick by the .s renderer) and m_0x0E (spin,
     * +~3·dt per tick). Capture live slots so the native feeder (sparkle_build,
     * particle.c) can lerp the PHASE sub-tick at float precision; without
     * it gem sparkles animate at the tick rate regardless of --render-fps.
     * Render-side only: nothing is written back to the pool (the
     * reextract RAM-writeback still carves g_Sparkles out — its rand()-seeded
     * respawns must never be simulated host-side). A slot reused by a respawn
     * pairs with m_Life INCREASING, which the feeder detects and snaps on;
     * a slot dead for a tick goes unseen → evicted → fresh spawn (no pair). */
    for (uint32_t i = 0; i < 8u; ++i) {
        uint32_t sa = G_SPARKLES + i * 0x18u;
        const void* host = openpete_psx_to_host_ptr(sa);
        if (!host) break;
        uint32_t w3; memcpy(&w3, (const uint8_t*)host + 0x0C, 4);
        if ((w3 & 0xFFu) == 0) continue;        /* m_Life==0 → dead slot */
        openpete_interp_objs_capture(sa, host, 0x18u);
    }

    /* Glow pool (D_80078800 Glow[16], stride 0x24 — Sparx aura, torch glows):
     * the ring CENTRE rides the owner moby's bracket-lerped position, but the
     * record's own animated channels — size (+0x10, censused ramping +32/tick
     * on the Sparx aura spawn), colour (+0x0C) and the position offset
     * (+0x14/18/1C) — stepped at the sim rate. Capture live slots (count>0)
     * so glow_build can lerp them sub-tick (phase-only; nothing written back). */
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t ga = D_GLOWS + i * 0x24u;
        const void* host = openpete_psx_to_host_ptr(ga);
        if (!host) break;
        int32_t cnt; memcpy(&cnt, host, 4);
        if (cnt <= 0) continue;                 /* inactive slot */
        openpete_interp_objs_capture(ga, host, 0x24u);
    }

    /* SHADED render list (G_SCRATCH+0x2400 — flat Moby*[256] the .s shaded
     * renderer walks): capture EVERY listed moby. Mostly a dedup-overwrite of
     * the g_LevelMobys walk above, but load-screen props (the level-transition
     * tally gems/chest, which otherwise step at the tick rate) live in
     * the bump-allocated HUD pool OUTSIDE g_LevelMobys and would otherwise
     * never get prev/curr snapshots (no position OR rotation interp). */
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t m = rd32(G_SCRATCH + 0x2400u + i * 4u);
        if (m < 0x80000000u) break;
        const void* host = openpete_psx_to_host_ptr(m);
        if (host) openpete_interp_objs_capture(game_interp_moby_key(m), host,
                                               openpete_interp_objs_blob_bytes());
    }

    /* REGULAR/SPECULAR render set (the mobys the canonical cull records point at):
     * the same dedup-overwrite idea as the shaded list above, for the mobys the
     * g_LevelMobys walk can't reach. The balloonist copies the balloon into the
     * render scratch on arrival and draws it from there for the descent, so
     * without this it is the one object in the scene with no prev/curr snapshot —
     * no position, rotation or keyframe interp — while the camera it fills the
     * screen in interpolates around it. */
    {
        const uint32_t* drawn = NULL;
        int drawn_n = moby_native_canon_drawn_mobys(&drawn);
        for (int i = 0; i < drawn_n; ++i) {
            const uint8_t* host = (const uint8_t*)openpete_psx_to_host_ptr(drawn[i]);
            if (!host || host[MOBY_STATE_OFF] >= 0x80) continue;   /* dead / unmapped */
            openpete_interp_objs_capture(game_interp_moby_key(drawn[i]), host,
                                         openpete_interp_objs_blob_bytes());
        }
    }

    /* Dragon-star record (D_DRAGONSTAR — the circling damage stars around
     * Spyro): a SINGLE gamestate record the producer (func_8002F3E4) animates
     * per tick — +0x04 world position, +0x10 per-effect rotation SHORTMATRIX,
     * +0x24 spoke size, +0x2B colour. The feeder (dragonstar_feed_native)
     * re-reads it on every sub-tick present, so without a snapshot the stars
     * circle at the tick rate regardless of --render-fps
     * (particle-circling-stars.bk2 f1850). Capture the 0x2C-byte record when
     * ACTIVE (+0x00 gate); inactive → unseen → evicted (a re-trigger snaps). */
    {
        const void* host = openpete_psx_to_host_ptr(D_DRAGONSTAR);
        uint32_t gate = 0; if (host) memcpy(&gate, host, 4);
        if (gate != 0)
            openpete_interp_objs_capture(D_DRAGONSTAR, host, 0x2Cu);
    }

    /* g_DragonCutscene m_State (+0x28) through m_CutsceneTicks (+0x3C): the
     * rescue draw scales the departing dragon's record by a function of the
     * tick counter in state 5, and a sub-tick present lerps those factors
     * between this pair. Keyed by the m_State address. */
    {
        const void* host = openpete_psx_to_host_ptr(G_DRAGON_CUTSCENE + 0x28u);
        if (host && rd32(G_GAMESTATE) == 8u)
            openpete_interp_objs_capture(G_DRAGON_CUTSCENE + 0x28u, host, 0x18u);
    }

}

void game_interp_register(void) {
    /* The camera's capture spans the projection matrix through the follow
     * state (0x110 bytes), past the cam.xform_end descriptor boundary the
     * draw-time transform ends at, so the span is stated here rather than
     * computed. The pools outside g_LevelMobys are captured after the shared
     * moby-array walk. */
    game_interp_register_common(s_spyro_interp_rows,
                                (unsigned)(sizeof(s_spyro_interp_rows) /
                                           sizeof(s_spyro_interp_rows[0])),
                                0x110u, spyro_interp_extra_pools);
}
