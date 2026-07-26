/* spyro_interp_regions.c — Spyro 1's render-side interpolation region
 * table. Captured per-tick into the openpete_interp ring so render
 * frames at >29.97 Hz can lerp transforms and re-emit DrawOTag.
 *
 * Registration model:
 *   This TU defines spyro_interp_register() which forwards the static
 *   table to openpete_interp_register_regions(). main.cpp calls it
 *   once at startup before openpete_run(). We avoid the weak-symbol
 *   override pattern used by spyro_state_regions.c because MinGW's
 *   PE/COFF linker has been observed to mis-resolve it, leaving the
 *   weak NULL in place at runtime and crashing the recomp boot path.
 *   Explicit registration works identically across toolchains.
 *
 * Scope (2026-05-14 architecture revision — see
 * notes/plans/openpete-renderer-foundation.md and
 * notes/plans/openpete-fps-unlock.md):
 *   - g_Camera ONLY. Captured for use by the renderer-foundation
 *     Phase 2g stretch (depth-aware async reprojection), which wants
 *     a single authoritative camera per tick rather than per-prim
 *     composite matrices.
 *   - g_Spyro / mobies / animation are NOT snapshotted here. Their
 *     lerp story lives in renderer-foundation Phases 0+1+2: the
 *     matcher reads per-vertex world-space + composite matrices
 *     directly from psxrecomp's gte_execute, pairs prims across
 *     frames by (texpage+clut+bbox+velocity), and decompose-lerp-
 *     recomposes the composite transform. That path doesn't read
 *     g_Spyro / g_LevelMobys from PSX RAM at all, so snapshotting
 *     them here is dead weight.
 *   - Particles are deferred indefinitely (no host-readable list;
 *     handled by the matcher if they're identifiable, otherwise they
 *     pop at 29.97 Hz).
 *
 * Address verification: 2026-05-14 nm of games/spyro-1/spyro-1/build/psx.elf:
 *   80076dd0 B g_Camera                   (next = 0x80076ee0 → 0x110 bytes)
 *
 * Lengths match manifest/gameplay_state_regions.toml so the same
 * boundaries the determinism gate trusts are the boundaries we
 * snapshot.
 */

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "spyro_interp_regions.h"
#include "spyro_native_reextract.h"   /* moby_native_canon_drawn_mobys */
#include "features/text_moby_id.h"    /* text_moby_id_lookup */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

/* DIRECT singleton regions — captured whole into the interp ring each tick.
 *   g_Camera (0x110): the smooth-follow camera (re-run feeder).
 *   g_Spyro  (0x2A4): the player avatar — position/rotation/anim for the
 *                     lerp + animation-clock feeders. Singleton, so it lives
 *                     here rather than in the per-object (indirect) table. */
static const openpete_interp_region_t s_spyro_interp_regions[] = {
    /* ra_commit=1: the camera PRESENTS from the committed timeline under
     * runahead (driver recaptures this region post-rollback). The follow-
     * camera is where misprediction is catastrophic — it's the view
     * transform, so an R2 release un-showed N ticks of mispredicted pan
     * across every pixel (the release snap) — and where the extra N ticks
     * of latency are invisible (it trails by design). Spyro/mobys/
     * particles stay led, keeping the input-lag win where it's felt. The
     * game's own g_Camera state is untouched — this only picks which
     * timeline the presentation lerps. */
    { "g_Camera", 0x80076DD0, 0x110, 1 },
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

static uint32_t rd32(uint32_t addr);

/* See spyro_interp_regions.h. Tried in order of how much of the game's own
 * state backs the identity: emitter-call provenance for text glyphs, the
 * fly-out source slot for tally gems, then a class-relative pool ordinal for
 * everything else in the HUD pool, and finally the moby's own address. */
uint32_t spyro_interp_moby_key(uint32_t m) {
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

/* g_EnvironmentAnimations sits 0x48 bytes before g_Environment (environment.h);
 * +0x08/+0x0C is the {count, ptr-array} of scrolling-texture descriptors. */
#define G_ENVANIM (G_ENVIRONMENT - 0x48u)

static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_ptr(addr);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/* Walk g_LevelMobys, snapshotting each live moby's stride-0x58 record into
 * the keyed prev/curr table (key = the moby's vaddr — slot-stable across
 * ticks). Read-only; mirrors the moby feeder's own walk
 * (moby_native.c::append_revealed_regular). */
static void spyro_interp_capture_objs(void) {
    openpete_interp_objs_begin_tick();

    uint32_t arr = rd32(G_LEVELMOBYS);
    if (arr >= 0x80000000u) {
        uint32_t m = arr;
        /* Bound the walk below the store capacity (OPENPETE_INTERP_OBJ_SLOTS) so
         * the HUD (12) + particle captures that follow this pass always fit — the
         * store now also drops gracefully when full, but reserving here keeps
         * mobys, not HUD/particles, the ones dropped in the (impossible for Spyro 1)
         * overflow case. The array sentinel below stops the walk after a few
         * hundred live mobys anyway. */
        for (int guard = 0; guard < (int)OPENPETE_INTERP_OBJ_SLOTS - 512; ++guard) {
            const void* host = openpete_psx_to_host_ptr(m);
            if (!host) break;
            uint8_t st = ((const uint8_t*)host)[MOBY_STATE_OFF];
            if (st == 0xFF) break;                       /* array sentinel */
            if (st < 0x80) {                             /* alive */
                openpete_interp_objs_capture(m, host, OPENPETE_INTERP_OBJ_BLOB_BYTES);
                /* OPENPETE_CAPTRACE_CLS: per-TICK canonical position of mobys of
                 * this class (staircase-vs-smooth forensics, capture side). */
                static long s_ctc = -2;
                if (s_ctc == -2) { const char* e = getenv("OPENPETE_CAPTRACE_CLS"); s_ctc = e ? strtol(e, NULL, 0) : -1; }
                if (s_ctc >= 0) {
                    uint16_t c2; memcpy(&c2, (const uint8_t*)host + 0x36, 2);
                    if (s_ctc == 0xFFFF || c2 == (uint16_t)s_ctc) {
                        int32_t pos[3]; memcpy(pos, (const uint8_t*)host + 0x0C, 12);
                        fprintf(stderr, "[cap-tick f=%u] m=%08x cls=%03x pos=(%d,%d,%d)\n",
                                openpete_frame, m, c2, pos[0], pos[1], pos[2]);
                    }
                }
            }
            m += MOBY_STRIDE;
        }
    }

    /* Static 3D HUD icons (g_Hud.m_Mobys[12]) — stride-0x58 Mobys OUTSIDE
     * g_LevelMobys. They spin (m_Rotation @ +0x44); capture them so the sub-tick
     * spine can lerp the spin (else it steps at 30 Hz while the rest is smooth).
     * Always present (no sentinel) — capture all 12; inactive ones don't render. */
    for (uint32_t i = 0; i < G_HUD_COUNT; ++i) {
        uint32_t m = G_HUD_ICONS + i * MOBY_STRIDE;
        const void* host = openpete_psx_to_host_ptr(m);
        if (host) openpete_interp_objs_capture(m, host, OPENPETE_INTERP_OBJ_BLOB_BYTES);
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
     * particle_native.c) can lerp the PHASE sub-tick at float precision — user
     * report 2026-07-14: gem sparkles animate at 29.97 Hz regardless of
     * --render-fps. Render-side only: nothing is written back to the pool (the
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
     * tally gems/chest — user report 2026-07-04: they step at 30 Hz) live in
     * the bump-allocated HUD pool OUTSIDE g_LevelMobys and would otherwise
     * never get prev/curr snapshots (no position OR rotation interp). */
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t m = rd32(G_SCRATCH + 0x2400u + i * 4u);
        if (m < 0x80000000u) break;
        const void* host = openpete_psx_to_host_ptr(m);
        if (host) openpete_interp_objs_capture(spyro_interp_moby_key(m), host,
                                               OPENPETE_INTERP_OBJ_BLOB_BYTES);
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
            openpete_interp_objs_capture(spyro_interp_moby_key(drawn[i]), host,
                                         OPENPETE_INTERP_OBJ_BLOB_BYTES);
        }
    }

    /* Dragon-star record (D_DRAGONSTAR — the circling damage stars around
     * Spyro): a SINGLE gamestate record the producer (func_8002F3E4) animates
     * per tick — +0x04 world position, +0x10 per-effect rotation SHORTMATRIX,
     * +0x24 spoke size, +0x2B colour. The feeder (dragonstar_feed_native)
     * re-reads it on every sub-tick present, so without a snapshot the stars
     * circle at 30 Hz regardless of --render-fps (user report 2026-07-04,
     * particle-circling-stars.bk2 f1850). Capture the 0x2C-byte record when
     * ACTIVE (+0x00 gate); inactive → unseen → evicted (a re-trigger snaps). */
    {
        const void* host = openpete_psx_to_host_ptr(D_DRAGONSTAR);
        uint32_t gate = 0; if (host) memcpy(&gate, host, 4);
        if (gate != 0)
            openpete_interp_objs_capture(D_DRAGONSTAR, host, 0x2Cu);
    }

    /* Scrolling-texture env-anim descriptors (m_ScrollingTextures — waterfalls
     * and flowing water). func_8002A6FC keeps each scroller's ENTIRE animation
     * state in a 7-bit accumulator at desc+6 and commits it to the tiledef V
     * bytes as WHOLE TEXELS on a keyframe advance, so a scrolled texture steps
     * at the sim rate while the world around it interpolates. Capture just the
     * (tile, accumulator) pair at desc+4 — deliberately NOT the descriptor head,
     * whose countdown byte at +3 decrements every tick: including it would make
     * the blob differ every tick and defeat the table's distinct-roll dedupe,
     * collapsing a slow scroller's real advance interval to a single tick.
     * terrain_native.c (tsc_scroll_refresh) reads the pair back and interpolates
     * the accumulator at the slot's own alpha, so the scroll stays smooth even
     * when the camera is still (the freecam / standing-still case, where the
     * camera ring dedupes and the shared bracket alpha never engages). */
    {
        uint32_t cnt  = rd32(G_ENVANIM + 0x08u);
        uint32_t base = rd32(G_ENVANIM + 0x0Cu);
        if (base && cnt && cnt <= 1024u) {
            for (uint32_t i = 0; i < cnt; ++i) {
                uint32_t desc = rd32(base + i * 4u);
                if (!desc) continue;
                const void* host = openpete_psx_to_host_ptr(desc + 4u);
                if (host) openpete_interp_objs_capture(desc, host, 4u);
            }
        }
    }

    openpete_interp_objs_end_tick();

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_INTERP_OBJDBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (openpete_frame % 120u) == 0) {
        int tracked, paired, spawned, despawned;
        openpete_interp_objs_stats(&tracked, &paired, &spawned, &despawned);
        fprintf(stderr, "[interp-objs f=%u] tracked=%d paired=%d spawned=%d despawned=%d\n",
                openpete_frame, tracked, paired, spawned, despawned);
    }

    /* Numerical LOO gate (OPENPETE_INTERP_LOO) — Gate 2 of the high-FPS plan,
     * as an instrument not eyeballs. Hold 3 consecutive real ticks; reconstruct
     * the MIDDLE by lerping the outer two (the exact α=0.5 lerp the spine uses)
     * and compare to the real middle: err = |½(p[N-1]+p[N+1]) − p[N]| — the
     * chord-vs-curve interpolation error. Small vs the per-tick motion ⇒ the
     * lerp faithfully reconstructs canonical motion (= smooth). Read-only. */
    static int loo = -1;
    if (loo < 0) { const char* e = getenv("OPENPETE_INTERP_LOO"); loo = (e && *e && *e != '0'); }
    if (loo) {
        static int32_t ch[3][3], sh[3][3]; static int ln = 0;
        for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) { ch[s][k]=ch[s+1][k]; sh[s][k]=sh[s+1][k]; }
        for (int k = 0; k < 3; k++) { ch[2][k]=(int32_t)rd32(0x80076DD0u+0x28u+(uint32_t)k*4u);
                                      sh[2][k]=(int32_t)rd32(0x80078A58u+0x00u+(uint32_t)k*4u); }
        if (ln < 3) ln++;
        if (ln >= 3) {
            double cd=0, sd=0, cm=0, sm=0;
            for (int k = 0; k < 3; k++) {
                double c=0.5*((double)ch[0][k]+ch[2][k])-ch[1][k]; cd+=c*c;
                double s=0.5*((double)sh[0][k]+sh[2][k])-sh[1][k]; sd+=s*s;
                double cmo=(double)ch[2][k]-ch[1][k]; cm+=cmo*cmo;
                double smo=(double)sh[2][k]-sh[1][k]; sm+=smo*smo;
            }
            cd=sqrt(cd); sd=sqrt(sd); cm=sqrt(cm); sm=sqrt(sm);
            static double cs=0, cmx=0, ss=0, smx=0, cms=0, sms=0; static long lc=0;
            if (cd < 5000.0 && sd < 5000.0) {   /* exclude cuts (snapped, not lerped) */
                cs+=cd; if(cd>cmx)cmx=cd; ss+=sd; if(sd>smx)smx=sd; cms+=cm; sms+=sm; lc++;
            }
            if (lc > 0 && (openpete_frame % 600u) == 0)
                fprintf(stderr, "[interp-loo f=%u] n=%ld | CAMERA lerp-err mean=%.2f max=%.1f vs motion/tick=%.1f | "
                        "SPYRO lerp-err mean=%.2f max=%.1f vs motion/tick=%.1f (world units)\n",
                        openpete_frame, lc, cs/lc, cmx, cms/lc, ss/lc, smx, sms/lc);
        }
    }
}

void spyro_interp_register(void) {
    openpete_interp_register_regions(
        s_spyro_interp_regions,
        sizeof(s_spyro_interp_regions) / sizeof(s_spyro_interp_regions[0]));
    openpete_interp_register_obj_capture(spyro_interp_capture_objs);
}
