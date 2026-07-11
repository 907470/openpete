// mod_moby_ops.c — Spyro 1 game ops for the mod moby-type registry
// (core/mods/mod_registry.c; modding framework PoC rung 11).
//
// Ground truth (recon 2026-07-11, cross-checked include/moby.h +
// asm/42CC4.s + moby_native.c):
//   - g_Models @ 0x80076378: flat 512×4B Model* table indexed DIRECTLY by
//     Moby.m_Class (+0x36). Populated per level by the loader; a NULL row
//     means the level never uses that class — which also means the level
//     overlay's compiled update switch has no case for it (per-level
//     switches only cover the level's own classes), so a claimed id's
//     vanilla behavior is `default` (nothing) by construction.
//   - g_SpawnMoby fn-ptr @ 0x800758CC: Moby* (*)(int class, Moby* parent).
//     Runs the class's own spawn-switch case (model/renderer/collision
//     init via func_800524C4 + func_800526A8); parent NULL copies
//     g_Spyro's position. We spawn AS the donor class (full class-correct
//     init) and rebrand m_Class to the registered id afterwards.
//   - Pool: g_LevelMobys @ 0x80075828 (Moby[] base, stride 0x58);
//     m_State (+0x48) >= 0x80 dead, == 0xFF end-of-pool sentinel.
//   - Gameplay gate: g_Gamestate @ 0x800757D8 == 0 (GS_Playing). Claiming
//     rows only in GS_Playing guarantees the loader finished populating
//     g_Models (no half-loaded-table race).

#include "openpete_mods.h"

#include <string.h>

#define ADDR_G_MODELS     0x80076378u
#define MODEL_COUNT       512
#define ADDR_G_SPAWNMOBY  0x800758CCu
#define ADDR_G_LEVELMOBYS 0x80075828u
#define ADDR_G_GAMESTATE  0x800757D8u
#define ADDR_G_LEVELID    0x8007596Cu

#define MOBY_STRIDE       0x58u
#define MOBY_OFF_CLASS    0x36u
#define MOBY_OFF_STATE    0x48u
#define MOBY_POOL_CAP     8192 /* walk bound; pool is sentinel-terminated */

/* Loader-selected per-model initializers (loaders.c:699): NORMAL when
 * (int)g_Models[class] < 0 (a kseg0 pointer), SHADED otherwise. They set
 * the renderer/bucket bits the render-list builder sorts on — spawning
 * through the switch's default arm always runs the NORMAL one, so a
 * shaded donor model lands in the wrong bucket unless re-initialized. */
#define ADDR_INIT_NORMAL  0x800526A8u
#define ADDR_INIT_SHADED  0x800529CCu

#include "psx_runtime.h" /* CPUState + call_by_address */

static int ops_level_id(CPUState* cpu) {
    if (cpu->read_word(ADDR_G_GAMESTATE) != 0u) return 0; /* GS_Playing */
    return (int)cpu->read_word(ADDR_G_LEVELID);
}

/* Claim the highest FREE model row, seeding it from the donor. Seeded
 * rows read non-NULL afterwards, so consecutive claims self-advance. */
static int ops_alloc_seed_class(CPUState* cpu, int donor_class) {
    if (donor_class <= 0 || donor_class >= MODEL_COUNT) return 0;
    uint32_t donor = cpu->read_word(ADDR_G_MODELS + 4u * (uint32_t)donor_class);
    if (!donor) return 0; /* donor's model not loaded in this level */
    for (int id = MODEL_COUNT - 1; id > 0; id--) {
        if (cpu->read_word(ADDR_G_MODELS + 4u * (uint32_t)id) == 0u) {
            cpu->write_word(ADDR_G_MODELS + 4u * (uint32_t)id, donor);
            return id;
        }
    }
    return 0;
}

static uint32_t ops_spawn(CPUState* cpu, int class_id, int donor_class) {
    uint32_t spawner = cpu->read_word(ADDR_G_SPAWNMOBY);
    if (!spawner || class_id <= 0) return 0;
    /* Guest calls from host context: preserve the argument/return regs we
     * clobber (same convention as the SDK's op_call wrappers). */
    uint32_t s_a0 = cpu->a0, s_a1 = cpu->a1, s_a2 = cpu->a2, s_a3 = cpu->a3;
    uint32_t s_v0 = cpu->v0, s_v1 = cpu->v1, s_ra = cpu->ra;
    cpu->a0 = (uint32_t)donor_class;
    cpu->a1 = 0; /* parent NULL: spawn-switch places it at g_Spyro */
    call_by_address(cpu, spawner);
    uint32_t moby = cpu->v0;
    if (moby) {
        /* Rebrand: under the registered id the level's update switch
         * defaults, so the mod's update_fn is the sole behavior owner. */
        cpu->write_half(moby + MOBY_OFF_CLASS, (uint16_t)class_id);
        /* Re-run the loader-correct initializer for the donor's MODEL
         * kind — the spawn default arm assumes NORMAL, and a shaded
         * model in the regular render bucket is culled to nothing. */
        uint32_t mdl = cpu->read_word(ADDR_G_MODELS + 4u * (uint32_t)donor_class);
        cpu->a0 = moby;
        call_by_address(cpu, ((int32_t)mdl < 0) ? ADDR_INIT_NORMAL
                                                : ADDR_INIT_SHADED);
    }
    cpu->a0 = s_a0; cpu->a1 = s_a1; cpu->a2 = s_a2; cpu->a3 = s_a3;
    cpu->v0 = s_v0; cpu->v1 = s_v1; cpu->ra = s_ra;
    return moby;
}

static void ops_walk_class(CPUState* cpu, int class_id,
                           openpete_moby_update_fn fn) {
    uint32_t m = cpu->read_word(ADDR_G_LEVELMOBYS);
    if (!m) return;
    for (int i = 0; i < MOBY_POOL_CAP; i++, m += MOBY_STRIDE) {
        uint8_t state = cpu->read_byte(m + MOBY_OFF_STATE);
        if (state == 0xFFu) break;          /* end-of-pool sentinel */
        if (state >= 0x80u) continue;       /* dead/inactive */
        if (cpu->read_half(m + MOBY_OFF_CLASS) == (uint16_t)class_id)
            fn(cpu, m);
    }
}

static const openpete_moby_ops_t s_spyro1_moby_ops = {
    .update_hook_addr = 0x8003385Cu, /* GamestateUpdate */
    .level_id         = ops_level_id,
    .alloc_seed_class = ops_alloc_seed_class,
    .spawn            = ops_spawn,
    .walk_class       = ops_walk_class,
};

void spyro1_moby_ops_register(void) {
    openpete_mods_set_moby_ops(&s_spyro1_moby_ops);
}
