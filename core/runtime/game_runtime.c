// core/runtime/game_runtime.c - the registered game descriptor and the
// game_extras.h entries, forwarded to it. See openpete_game_runtime.h.

#include "openpete_game_runtime.h"

#include "game_extras.h"
#include "openpete_modsave.h"
#include "game_layout.h"
#include "openpete_fiber.h"
#include "openpete_psx.h"
#include "psx_runtime_internal.h"

#include <stdio.h>
#include <string.h>

/* Before registration every hook is absent and every table empty. */
static const openpete_game_runtime_t k_unregistered = { 0 };
static const openpete_game_runtime_t* s_rt = &k_unregistered;

/* The registered game's layout; the feeders read its fields. Sealed by
 * openpete_run; this file's register call is its one writer. */
OP_SEAL_DEFINE(game_layout);
game_layout_t game_layout OP_SEALED(game_layout); /* thread-roles: written before openpete_run seals it, read-only after */

/* The g_SonyImage pool base the recompiled code adds its pool offsets to;
 * the registered descriptor's sony_image_base. */
uint32_t psx_runtime_g_sony_image_base;

void openpete_game_runtime_register(const openpete_game_runtime_t* rt) {
    op_seal_check(game_layout);
    s_rt = rt ? rt : &k_unregistered;
    if (s_rt->layout) game_layout = *s_rt->layout;
    else memset(&game_layout, 0, sizeof game_layout);
    psx_runtime_g_sony_image_base = s_rt->sony_image_base;
    openpete_feed_key_select(s_rt->feed_key == OPENPETE_FEED_KEY_VBL
                                 ? OPENPETE_FEED_KEY_VBL : OPENPETE_FEED_KEY_FRAME);
    psx_set_override_dispatch_game(s_rt->override_dispatch);
    openpete_loop_anchor_set(s_rt->loop_anchor_pc);
    openpete_consume_pc_set(s_rt->consume_pc);
}

const openpete_game_runtime_t* openpete_game_runtime(void) { return s_rt; }

void openpete_game_runtime_dump(FILE* out) {
    fprintf(out, "runtime.descriptor %p\n", (const void*)s_rt);
    fprintf(out, "runtime.game_layout ");
    for (size_t i = 0; i < sizeof game_layout; i++)
        fprintf(out, "%02x", ((const unsigned char*)&game_layout)[i]);
    fputc('\n', out);
    fprintf(out, "runtime.sony_image_base 0x%08x\n", (unsigned)psx_runtime_g_sony_image_base);
    fprintf(out, "runtime.feed_key %d\n", (int)openpete_feed_key());
    fprintf(out, "runtime.override_dispatch %p\n", (void*)psx_override_dispatch_game());
    fprintf(out, "runtime.loop_anchor 0x%08x\n", (unsigned)openpete_loop_anchor_pc());
    fprintf(out, "runtime.consume_pc 0x%08x\n", (unsigned)openpete_consume_pc());
}

/* Before registration, or for a descriptor without an identity, the
 * identity strings are "". */
static const openpete_game_identity_t k_no_identity = { "", "", "" };

/** @brief Return the registered identity, or the all-empty one when none is registered. */
static const openpete_game_identity_t* identity(void) {
    return s_rt->identity ? s_rt->identity : &k_no_identity;
}

const char* game_get_exe_filename(void) { return identity()->exe_filename; }
const char* game_get_display_name(void) { return identity()->display_name; }
const char* game_get_id(void) { return identity()->id; }

uint32_t game_get_pad_read_pc(void) { return s_rt->pad_read_pc; }
uint32_t game_get_load_delay_default(void) { return s_rt->load_delay_default; }

void game_on_boot(CPUState* cpu) {
    if (s_rt->on_boot) s_rt->on_boot(cpu);
}
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    if (s_rt->on_frame_pre) s_rt->on_frame_pre(cpu, frame_counter);
}
void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    if (s_rt->on_frame) s_rt->on_frame(cpu, frame_counter);
}
void game_on_hash_emit(uint32_t frame) {
    if (s_rt->on_hash_emit) s_rt->on_hash_emit(frame);
}
void game_on_state_loaded(CPUState* cpu) {
    if (s_rt->on_state_loaded) s_rt->on_state_loaded(cpu);
}

/** @brief The game's own payload size, without the save-record trailer. */
static uint32_t game_extra_own_size(void) {
    return s_rt->state_extra_size ? s_rt->state_extra_size() : 0u;
}
uint32_t game_state_extra_size(void) {
    return game_extra_own_size() + openpete_modsave_block_size();
}
void game_state_extra_save(void* dst) {
    if (s_rt->state_extra_save) s_rt->state_extra_save(dst);
    openpete_modsave_block_save((uint8_t*)dst + game_extra_own_size());
}
void game_state_extra_load(const void* src, uint32_t size) {
    const uint32_t own = openpete_modsave_block_load(src, size);
    if (s_rt->state_extra_load) s_rt->state_extra_load(src, own);
}

int game_is_gameplay_live(CPUState* cpu) {
    return s_rt->is_gameplay_live ? s_rt->is_gameplay_live(cpu) : 0;
}

void game_set_skip_to_level(int level_id) {
    if (s_rt->set_skip_to_level) s_rt->set_skip_to_level(level_id);
}
int game_resolve_level_arg(const char* arg) {
    if (s_rt->resolve_level_arg) return s_rt->resolve_level_arg(arg);
    fprintf(stderr, "%s has no --skip-to-level targets\n", game_get_display_name());
    return -1;
}
void game_set_skip_to_cutscene(int cutscene_id) {
    if (s_rt->set_skip_to_cutscene) s_rt->set_skip_to_cutscene(cutscene_id);
}
int game_resolve_cutscene_arg(const char* arg) {
    if (s_rt->resolve_cutscene_arg) return s_rt->resolve_cutscene_arg(arg);
    fprintf(stderr, "%s has no --skip-to-cutscene targets\n", game_get_display_name());
    return -1;
}
void game_set_skip_to_area(int level_id, int area) {
    if (s_rt->set_skip_to_area) s_rt->set_skip_to_area(level_id, area);
}
int game_resolve_area_arg(const char* arg, int* out_area) {
    if (s_rt->resolve_area_arg) return s_rt->resolve_area_arg(arg, out_area);
    fprintf(stderr, "%s has no --skip-to-area targets\n", game_get_display_name());
    return -1;
}
int game_validate_cheats_arg(const char* spec) {
    if (s_rt->validate_cheats_arg) return s_rt->validate_cheats_arg(spec);
    fprintf(stderr, "openpete: --cheats: %s has no built-in cheats to apply\n",
            game_get_display_name());
    return -1;
}
void game_surface(struct openpete_surface_sink* sink) {
    if (s_rt->surface) s_rt->surface(sink);
}

const game_disc_layout_t* game_get_disc_layout(void) { return s_rt->disc_layout; }
