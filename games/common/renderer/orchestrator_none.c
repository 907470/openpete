/* orchestrator_none.c — the inert orchestrator for a game with no sub-tick
 * re-extract spine. See renderer/orchestrator.h. */

#include "renderer/orchestrator.h"

int openpete_native_interp_pos(unsigned int key, double out[3]) {
    (void)key; (void)out;
    return 0;
}

void spyro_native_canon_epoch_commit(void) {
}

void cyclorama_native_env_clear_commit(void) {
}
