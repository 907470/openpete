// core/include/openpete_game_runtime.h - the per-game runtime descriptor.
//
// Every behaviour and table that differs between games and that core or the
// shared games/common TUs reach is a field of one openpete_game_runtime_t,
// which each game defines once under a game-unique name. The entry point
// registers the running game's descriptor before anything else runs; core
// reaches the game only through it. The game_extras.h entries are core
// forwarders over the descriptor, game_layout is the registered layout, and
// the recompiled path's per-game override is the descriptor's
// override_dispatch.
//
// A NULL hook or table field is the game's absence of that behaviour: the
// forwarder returns its documented default.

#ifndef OPENPETE_GAME_RUNTIME_H
#define OPENPETE_GAME_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "feature_registry.h"   /* feature_module_t */
#include "game_extras.h"        /* game_disc_layout_t */
#include "game_layout.h"        /* game_layout_t */
#include "openpete_mods.h"      /* game_mod_desc_t */

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_surface_sink;
/* The shared host TUs' per-game tables (games/common/game_host_desc.h);
 * opaque to core. */
struct game_host_desc;
/* The shared PSYQ thunk layer's per-game hooks and tables
 * (games/common/psyq/psyq_game_hooks.h); opaque to core. */
struct psyq_game_hooks;
/* A disc image the launcher labels by content hash (openpete_launcher.h). */
struct openpete_launcher_known_disc;

/* The identity a game's recomp.conf declares (tools/gen_game_identity.py). */
typedef struct openpete_game_identity {
    const char* id;            /* "spyro-1": the [game.<id>] config scope */
    const char* display_name;  /* "Spyro the Dragon" */
    const char* exe_filename;  /* "SCUS_942.28" */
} openpete_game_identity_t;

/* The window icon: RGBA8 rows, w * h pixels. */
typedef struct openpete_game_icon {
    const uint8_t* rgba;
    int            w;
    int            h;
} openpete_game_icon_t;

/* The game's feature modules, in dispatch order (feature_registry.h). */
typedef struct openpete_feature_list {
    const feature_module_t* const* modules;
    size_t                         count;
} openpete_feature_list_t;

typedef struct openpete_game_runtime {
    /* Identity and constants. */
    const openpete_game_identity_t* identity;
    const openpete_game_icon_t*     icon;            /* NULL: no window icon */
    const game_layout_t*            layout;          /* NULL: every field 0 */
    const game_disc_layout_t*       disc_layout;     /* NULL: no WAD overlay */
    uint32_t pad_read_pc;          /* game_get_pad_read_pc */
    uint32_t load_delay_default;   /* game_get_load_delay_default */
    uint32_t sony_image_base;      /* psx_runtime_g_sony_image_base */
    uint32_t feed_key;             /* openpete_feed_key_t; 0 = openpete_frame */
    uint32_t loop_anchor_pc;       /* the main loop's first call, entered once per game tick; 0 = none */
    /* The disc images the launcher's library labels by content hash. */
    const struct openpete_launcher_known_disc* known_discs;   /* NULL: none */
    int known_disc_count;

    /* Host-loop hooks (game_extras.h). */
    void     (*on_boot)(CPUState* cpu);
    void     (*on_frame_pre)(CPUState* cpu, uint32_t frame_counter);
    void     (*on_frame)(CPUState* cpu, uint32_t frame_counter);
    void     (*on_hash_emit)(uint32_t frame);
    void     (*on_state_loaded)(CPUState* cpu);
    uint32_t (*state_extra_size)(void);
    void     (*state_extra_save)(void* dst);
    void     (*state_extra_load)(const void* src, uint32_t size);
    int      (*is_gameplay_live)(CPUState* cpu);

    /* CLI resolvers and skip targets (game_extras.h). */
    void (*set_skip_to_level)(int level_id);
    int  (*resolve_level_arg)(const char* arg);
    void (*set_skip_to_cutscene)(int cutscene_id);
    int  (*resolve_cutscene_arg)(const char* arg);
    void (*set_skip_to_area)(int level_id, int area);
    int  (*resolve_area_arg)(const char* arg, int* out_area);
    int  (*validate_cheats_arg)(const char* spec);
    void (*surface)(struct openpete_surface_sink* sink);

    /* Host feature sets. */
    const openpete_feature_list_t* features;   /* NULL: no modules */
    const struct game_host_desc*   host;       /* the shared host TUs' tables */
    const struct psyq_game_hooks*  psyq;       /* NULL: every PSYQ hook inert */

    /* Interception: the game's arms at every recompiled function entry,
     * after the engine's own (psx_override_dispatch). */
    int (*override_dispatch)(CPUState* cpu, uint32_t addr);

    /* The mod layer's descriptor (openpete_mods_set_game_desc). */
    const game_mod_desc_t* (*mod_desc)(void);
} openpete_game_runtime_t;

/** @brief Register the running game's descriptor and bind the core state it carries.
 *
 * Called once, first thing in the entry point, before any game_extras.h
 * entry, game_layout read or recompiled function runs. Copies the layout
 * into game_layout (zero when the descriptor names none), sets
 * psx_runtime_g_sony_image_base and installs the descriptor's
 * override_dispatch as the recompiled path's per-game arm.
 * @param rt The descriptor; it must outlive the process. */
void openpete_game_runtime_register(const openpete_game_runtime_t* rt);

/** @brief Return the registered game's descriptor.
 *
 * @return The descriptor, or an all-NULL descriptor before registration. */
const openpete_game_runtime_t* openpete_game_runtime(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_RUNTIME_H */
