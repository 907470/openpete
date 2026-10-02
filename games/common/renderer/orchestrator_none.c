/* orchestrator_none.c - the inert orchestrator for a game with no sub-tick
 * re-extract spine. See renderer/orchestrator.h. */

#include "renderer/orchestrator.h"

int openpete_native_interp_pos(unsigned int key, double out[3]) {
    (void)key; (void)out;
    return 0;
}

void spyro_native_canon_epoch_commit(void) {
}

/* The canonical-render epoch and the producer-camera bracket: a game with no
 * sub-tick spine snapshots nothing, so every producer reports never-fresh and
 * every feeder skips its replay. */
uint32_t spyro_native_canon_epoch_stamp(void) { return 0u; }
int  spyro_native_canon_epoch_fresh(uint32_t stamp) { (void)stamp; return 0; }
int  spyro_native_canon_mobyreg_fresh(void) { return 0; }
int  spyro_native_canon_shaded_fresh(void) { return 0; }
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len) {
    (void)cpu; (void)off; (void)len;
}
void spyro_native_canon_capture_recs(void* cpu) { (void)cpu; }
void spyro_native_canon_capture_sxy(void* cpu) { (void)cpu; }
void spyro_native_canon_capture_camera(void* cpu, int producer) { (void)cpu; (void)producer; }
int  spyro_native_prod_camera_enter(int producer) { (void)producer; return 0; }
void spyro_native_prod_camera_leave(void) {}

void game_reextract_init(void) {
}
