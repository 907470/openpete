/* supercharge-anywhere — every ground charge becomes an instant, authentic
 * supercharge (real state, animation, particles, plow-through-enemies) on
 * ANY level, not just the ones with supercharge ramps.
 *
 * TRIGGER (verified against the decomp): the state dispatcher func_80041670
 * already promotes Spyro from the ground-charge state (m_State == 0x14,
 * grounded) to SUPERCHARGE (0x2C == 44) when m_walkingState has bit 0x40
 * set. Nothing in normal play sets that bit — it's a purpose-built trigger.
 * We set it every frame Spyro is ground-charging, so every charge becomes a
 * genuine supercharge: authentic anim-speed + sound (func_8003EA68(44)), the
 * plow-through-enemies collision, and the sparkle trail (func_80047B60's
 * state-44 handler spawns it via the shared g_SpawnParticle — on every level,
 * no injection).
 *
 * ANIMATION: state 44's body animation is Spyro model animation index 45,
 * patched per-level from the level's model-0 blob ONLY on ramp levels; off-
 * ramp it is NULL and func_8004A200 would fault dereferencing it. We supply
 * the AUTHENTIC animation without shipping any game data:
 *   - CAPTURE: override PatchInSpyroAnimations; on any level whose model-0
 *     stream carries index 45 (a ramp level), snapshot that record's raw
 *     (pre-rebased) bytes — from the USER's own disc — and cache them (RAM +
 *     the mod's data dir, so it persists across sessions).
 *   - INJECT: on levels lacking it, park the captured blob in low guest RAM
 *     (guest_alloc_low — required: the frames hold 21-bit halved vertex
 *     addresses) and replay PatchInSpyroAnimations' own per-record pointer
 *     re-basing against THIS level's Spyro model (its base mesh m_Data is
 *     level-invariant), then point m_Animations[45] at it. Ramp levels keep
 *     their native animation untouched.
 *   Until a ramp level has been visited once (no cache yet), we fall back to
 *   aliasing index 45 to a present locomotion animation so nothing faults.
 *
 * All guest-RAM writes + named/addr overrides: no base-openpete change.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_sdk_wrappers.h>
#include <string.h>
#include <stdio.h>

/* g_Spyro field offsets (verified from include/spyro.h + asm). */
#define OFF_STATE     0x78u   /* int m_State                              */
#define OFF_WALKSTATE 0x7Cu   /* int m_walkingState (the "subState")      */
#define OFF_AIRTIME   0x9Cu   /* int m_airTime (0 == grounded)            */

#define STATE_CHARGE_GROUND 0x14  /* ground-charge state                  */
#define STATE_SUPERCHARGE   0x2C  /* == 44, the promoted state            */
#define SUBSTATE_TRIGGER    0x40u /* bit that promotes 0x14 -> state 44   */

#define GA_STATE     (OP_GADDR_g_Spyro + OFF_STATE)
#define GA_WALKSTATE (OP_GADDR_g_Spyro + OFF_WALKSTATE)
#define GA_AIRTIME   (OP_GADDR_g_Spyro + OFF_AIRTIME)

/* Model (moby.h): m_Data @ 0x34, m_Animations[] @ 0x38. Supercharge = 45. */
#define MODEL_MDATA      0x34u
#define MODEL_ANIM_TABLE 0x38u
#define ANIM_SUPERCHARGE 45u

/* AnimationHeader (moby.h): m_NumFrames(s16 @0x00), m_AnimationVertices(0x10),
 * m_Faces(0x14), m_Colors(0x18), m_Frames[](0x24). SpyroAnimationFrame = 4B. */
#define AH_NUMFRAMES  0x00u
#define AH_ANIMVERTS  0x10u
#define AH_FACES      0x14u
#define AH_COLORS     0x18u
#define AH_FRAMES     0x24u

#define BLOB_MAX 16384u  /* anim-45 record is ~6.3 KB; headroom */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static int g_enabled   = 1; /* runtime toggle state           */
static int g_prev_down = 0; /* binding edge-detect            */
static int g_sub_idx   = 3; /* fallback substitute anim index */
static int g_debug     = 0; /* log state transitions          */

/* Captured authentic anim-45 blob (raw, pre-rebase: header+frames+verts). */
static unsigned char g_blob[BLOB_MAX];
static uint32_t      g_blob_len = 0;
static int           g_have_blob = 0;

/* Last values published by the tick hook, for the overlay section to read
 * (it may not touch guest RAM itself). */
static uint32_t g_last_state   = 0;
static uint32_t g_last_air     = 0;
static uint32_t g_triggers     = 0; /* charges promoted to supercharge */
static int      g_anim_kind    = 0; /* index into g_anim_kind_name     */

/* Result of ensure_supercharge_anim(): where anim 45 came from. */
static const char* const g_anim_kind_name[] = {
    "absent (no cache yet)", "native (ramp level)",
    "authentic (injected)",  "aliased (fallback)"
};

/* Low-RAM park for the injected copy (allocated once, tick context). */
static uint32_t g_park_vaddr = 0;
static void*    g_park_host  = 0;

/* Host-state posture (manifest: state = "rebuildable"): the only host
 * fact the guest world depends on is the park vaddr, and the park's
 * BYTES live in the low 2MB — canonical RAM, savestate-carried. A
 * restored world either matches the cached vaddr (park intact) or the
 * next inject re-parks lazily, so nothing needs a load notification. */

static int s_level_seen; /* last level the anim status was logged for */
static void publish_status(void);
static void on_level_ready(CPUState* cpu, int level_id);


static uint32_t anim_slot(uint32_t model, uint32_t idx) {
    return model + MODEL_ANIM_TABLE + idx * 4u;
}

/* ---- persistence (the user's own disc data, cached locally) ------------- */

static void blob_path(char* buf, int cap) {
    const char* dir = g_api->data_dir(g_self); /* engine-owned abs path */
    snprintf(buf, cap, "%s/anim45.bin", dir ? dir : ".");
}

static void blob_save(void) {
    char path[512];
    blob_path(path, sizeof path);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(&g_blob_len, 4, 1, f);
    fwrite(g_blob, 1, g_blob_len, f);
    fclose(f);
}

static void blob_load(void) {
    char path[512];
    blob_path(path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t len = 0;
    if (fread(&len, 4, 1, f) == 1 && len > 0 && len <= BLOB_MAX &&
        fread(g_blob, 1, len, f) == len) {
        g_blob_len = len;
        g_have_blob = 1;
    }
    fclose(f);
}

/* ---- capture: snapshot the authentic anim-45 record on ramp levels ------ */

OP_OVERRIDE_IMPL_PatchInSpyroAnimations(sc_capture) {
    /* data = host pointer to the guest {index,len,blob}... stream. Snapshot
     * BEFORE base() re-bases the blob in place. */
    if (!g_have_blob && data) {
        const unsigned char* p = (const unsigned char*)data;
        for (;;) {
            int idx = *(const int*)p;
            if (idx < 0) break;
            unsigned len = *(const unsigned*)(p + 4);
            if (idx == (int)ANIM_SUPERCHARGE) {
                if (len > 0 && len <= BLOB_MAX) {
                    memcpy(g_blob, p + 8, len);
                    g_blob_len = len;
                    g_have_blob = 1;
                    blob_save();
                    g_api->log(g_self, OP_MOD_LOG_INFO,
                        "supercharge-anywhere: captured authentic anim-45 "
                        "(%u bytes) — real animation now available everywhere",
                        len);
                }
                break;
            }
            p += 8 + len;
        }
    }
    op_base_PatchInSpyroAnimations(cpu, data);
}

/* ---- inject: park the captured blob and re-base it for THIS level ------- */

/* Replays PatchInSpyroAnimations' per-record pointer patching for the parked
 * copy against the current Spyro model. Returns the parked guest vaddr. */
static uint32_t inject_authentic_anim(uint32_t model) {
    if (!g_have_blob || !model) return 0;

    if (!g_park_vaddr) {
        g_park_vaddr = g_api->guest_alloc_low(g_self, g_blob_len, 16,
                                              &g_park_host);
        if (!g_park_vaddr || !g_park_host) { g_park_vaddr = 0; return 0; }
    }

    /* Fresh raw copy, then re-base against this level's model. */
    memcpy(g_park_host, g_blob, g_blob_len);
    unsigned char* h = (unsigned char*)g_park_host;

    uint32_t mdata = op_read_u32(g_api, model + MODEL_MDATA);
    int numFrames  = *(short*)(h + AH_NUMFRAMES);

    *(uint32_t*)(h + AH_FACES)  += mdata;   /* PATCH_POINTER(m_Faces,  m_Data) */
    *(uint32_t*)(h + AH_COLORS) += mdata;   /* PATCH_POINTER(m_Colors, m_Data) */

    uint32_t frames_va = g_park_vaddr + AH_FRAMES;
    uint32_t past      = frames_va + (uint32_t)numFrames * 4u; /* vertex stream */
    *(uint32_t*)(h + AH_ANIMVERTS) += past;

    for (int j = 0; j < numFrames; j++) {
        uint32_t* fd = (uint32_t*)(h + AH_FRAMES + (uint32_t)j * 4u);
        uint32_t lower = (((*fd & 0x001FFFFFu) + past) >> 1) & 0x001FFFFFu;
        uint32_t upper = *fd & 0xFFE00000u;
        *fd = upper + lower;
    }

    op_write_u32(g_api, anim_slot(model, ANIM_SUPERCHARGE), g_park_vaddr);
    return g_park_vaddr;
}

/* Guarantee SPYRO_MODEL->m_Animations[45] is valid before any state-44 frame.
 * Prefers the authentic injected animation; falls back to aliasing a present
 * locomotion animation until a ramp level has been visited. Leaves a native
 * (ramp-level) animation untouched. Returns 0=absent, 1=native, 2=injected,
 * 3=aliased. */
static int ensure_supercharge_anim(void) {
    uint32_t model = op_read_u32(g_api, OP_GADDR_g_Models);
    if (!model) return 0;

    uint32_t a45 = op_read_u32(g_api, anim_slot(model, ANIM_SUPERCHARGE));
    if (a45 && a45 == g_park_vaddr) return 2;   /* already our injection */
    if (a45) return 1;                          /* native (ramp level)   */

    if (g_have_blob && inject_authentic_anim(model)) return 2;

    /* Fallback: alias to a present locomotion animation. */
    uint32_t sub = op_read_u32(g_api, anim_slot(model, (uint32_t)g_sub_idx));
    if (!sub) {
        for (uint32_t i = 0; i < ANIM_SUPERCHARGE; i++) {
            sub = op_read_u32(g_api, anim_slot(model, i));
            if (sub) break;
        }
    }
    if (!sub) return 0;
    op_write_u32(g_api, anim_slot(model, ANIM_SUPERCHARGE), sub);
    return 3;
}

/* ---- per-frame trigger -------------------------------------------------- */

static unsigned g_pub_state = 0xFFFFFFFFu, g_pub_triggers = 0xFFFFFFFFu;
static int      g_pub_enabled = -1, g_pub_anim = -1;

static void publish_if_changed(void) {
    if (g_last_state == g_pub_state && g_triggers == g_pub_triggers &&
        g_enabled == g_pub_enabled && g_anim_kind == g_pub_anim)
        return;
    publish_status();
}

static void on_spyro_update(CPUState* cpu) {
    int down;
    if (op_read_u32(g_api, OP_GADDR_g_Gamestate) == 0) { /* GS_Playing */
        int id = (int)op_read_u32(g_api, OP_GADDR_g_LevelId);
        if (id > 0 && id != s_level_seen) {
            s_level_seen = id;
            on_level_ready(cpu, id);
        }
    }
    down = g_api->binding_down(g_self, "toggle");
    if (down && !g_prev_down) {
        g_enabled = !g_enabled;
        g_api->log(g_self, OP_MOD_LOG_INFO, "supercharge-anywhere: %s",
                   g_enabled ? "ON" : "OFF");
    }
    g_prev_down = down;

    if (g_enabled)
        g_anim_kind = ensure_supercharge_anim();

    g_api->base(cpu);

    /* Published unconditionally so the overlay status stays live while the
     * force is switched off. */
    uint32_t st  = op_read_u32(g_api, GA_STATE);
    uint32_t air = op_read_u32(g_api, GA_AIRTIME);
    g_last_state = st;
    g_last_air   = air;

    if (!g_enabled) {
        publish_if_changed();
        return;
    }

    if (g_debug) {
        static uint32_t prev = 0xFFFFFFFFu;
        if (st != prev) {
            g_api->log(g_self, OP_MOD_LOG_INFO,
                       "supercharge-anywhere: m_State -> %u (airTime=%u)",
                       st, air);
            prev = st;
        }
    }

    if (st == STATE_CHARGE_GROUND && air == 0) {
        uint32_t ws = op_read_u32(g_api, GA_WALKSTATE);
        if (!(ws & SUBSTATE_TRIGGER)) {
            op_write_u32(g_api, GA_WALKSTATE, ws | SUBSTATE_TRIGGER);
            g_triggers++;
        }
    }
    publish_if_changed();
}

/* "Level ready" the doctrine way: the physics post-hook above diffs
 * g_LevelId under g_Gamestate == GS_Playing (0) — the same gameplay-
 * live condition the engine's old level-ready service keyed on. */
static void on_level_ready(CPUState* cpu, int level_id) {
    (void)cpu;
    int r = g_enabled ? ensure_supercharge_anim()
                      : (op_read_u32(g_api, anim_slot(
                             op_read_u32(g_api, OP_GADDR_g_Models),
                             ANIM_SUPERCHARGE)) ? 1 : 0);
    g_anim_kind = r & 3;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "supercharge-anywhere: level %d ready; supercharge anim = %s",
               level_id, g_anim_kind_name[g_anim_kind]);
}

/* ---- status lines --------------------------------------------------
 * Repainted from the physics post-hook whenever a shown value changes
 * (and once at entry). The force switch itself is the "toggle" binding;
 * the boot posture and the debug/fallback knobs are [[config]] rows. */

static void publish_status(void) {
    const char* note = g_last_state == STATE_SUPERCHARGE  ? " supercharging"
                     : g_last_state == STATE_CHARGE_GROUND ? " ground charge"
                     : "";
    g_pub_state    = g_last_state;
    g_pub_triggers = g_triggers;
    g_pub_enabled  = g_enabled;
    g_pub_anim     = g_anim_kind;
    g_api->ui_status(g_self, "force supercharge: %s (toggle binding, "
                     "default G)", g_enabled ? "ON" : "OFF");
    g_api->ui_status(g_self, "Spyro: m_State %u%s, airTime %u",
                     g_last_state, note, g_last_air);
    g_api->ui_status(g_self, "charges promoted: %u", g_triggers);
    g_api->ui_status(g_self, "animation: %s", g_anim_kind_name[g_anim_kind]);
    if (g_have_blob)
        g_api->ui_status(g_self, "authentic anim-45 cached: %u bytes",
                         g_blob_len);
    else
        g_api->ui_status(g_self, "no authentic anim cached — enter a ramp "
                         "level once (Wizard Peak, Tree Tops)");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api  = api;
    g_self = self;

    g_enabled = api->config_bool(self, "start_enabled", 1);
    g_debug   = api->config_bool(self, "debug", 0);
    g_sub_idx = (int)api->config_int(self, "anim_substitute", 3);
    if (g_sub_idx < 0 || g_sub_idx >= (int)ANIM_SUPERCHARGE)
        g_sub_idx = 3;

    blob_load(); /* authentic anim from a previous session, if captured */

    op_sdk_bind(api);
    OP_REGISTER_OVERRIDE_PatchInSpyroAnimations(api, self, sc_capture);
    publish_status();
    return api->override_name(self, "UpdateSpyroPhysicsAndSurfaces",
                              on_spyro_update);
}
