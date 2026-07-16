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

/* Low-RAM park for the injected copy (allocated once, tick context). */
static uint32_t g_park_vaddr = 0;
static void*    g_park_host  = 0;

/* ---- savestate participation (reference OPENPETE_MOD_STATE adoption) ----
 * The park vaddr is host state the restored guest RAM depends on (the
 * model's anim slot in that RAM points at it). Declaring the state ABI
 * makes saves survive recompiles that keep this layout; DROP_OK because
 * losing the park is safe (we re-park lazily); RA_HOST_FX because the
 * toggle path logs/reads bindings (engine-suppressible under runahead). */
typedef struct { uint32_t park_vaddr; } sca_state_t;
OPENPETE_MOD_STATE(sca_state_t, 1, OP_MOD_STATE_DROP_OK | OP_MOD_RA_HOST_FX,
    OP_STATE_FIELD(sca_state_t, park_vaddr))

static int sca_state_save(void* buf, int cap) {
    if (cap < (int)sizeof(sca_state_t)) return -1;
    sca_state_t st = { g_park_vaddr };
    memcpy(buf, &st, sizeof(st));
    return (int)sizeof(st);
}

static void sca_state_load(const void* buf, int len) {
    if (buf && len >= (int)sizeof(sca_state_t)) {
        sca_state_t st;
        memcpy(&st, buf, sizeof(st));
        g_park_vaddr = st.park_vaddr;
        g_park_host  = st.park_vaddr ? g_api->guest(st.park_vaddr) : NULL;
        if (g_park_vaddr && !g_park_host) g_park_vaddr = 0; /* paranoia */
    } else {
        /* Reinit signal: restored world has no memory of our park. Drop
         * the cached vaddr — the next inject re-parks (arena ledger keeps
         * the cursor safe either way). */
        g_park_vaddr = 0;
        g_park_host  = NULL;
    }
}

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

static void on_spyro_update(CPUState* cpu) {
    int down = g_api->binding_down(g_self, "toggle");
    if (down && !g_prev_down) {
        g_enabled = !g_enabled;
        g_api->log(g_self, OP_MOD_LOG_INFO, "supercharge-anywhere: %s",
                   g_enabled ? "ON" : "OFF");
    }
    g_prev_down = down;

    if (g_enabled)
        ensure_supercharge_anim();

    g_api->base(cpu);

    if (!g_enabled)
        return;

    uint32_t st  = op_read_u32(g_api, GA_STATE);
    uint32_t air = op_read_u32(g_api, GA_AIRTIME);

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
        if (!(ws & SUBSTATE_TRIGGER))
            op_write_u32(g_api, GA_WALKSTATE, ws | SUBSTATE_TRIGGER);
    }
}

static void on_level_ready(CPUState* cpu, int level_id) {
    (void)cpu;
    int r = g_enabled ? ensure_supercharge_anim()
                      : (op_read_u32(g_api, anim_slot(
                             op_read_u32(g_api, OP_GADDR_g_Models),
                             ANIM_SUPERCHARGE)) ? 1 : 0);
    static const char* kind[] = { "absent (no cache yet)", "native (ramp level)",
                                  "authentic (injected)", "aliased (fallback)" };
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "supercharge-anywhere: level %d ready; supercharge anim = %s",
               level_id, kind[r & 3]);
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

    api->register_savestate_hooks(self, sca_state_save, sca_state_load);

    op_sdk_bind(api);
    OP_REGISTER_OVERRIDE_PatchInSpyroAnimations(api, self, sc_capture);
    api->on_level(self, on_level_ready);
    return api->override_name(self, "UpdateSpyroPhysicsAndSurfaces",
                              on_spyro_update);
}
