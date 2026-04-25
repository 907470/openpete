/* core/runtime/pcsxr_gte_stub.c — no-op `pcsxr_gte_run` for builds where
 * third_party/pcsx_redux_gte/ is disabled (the vendored runner needs
 * C++23, which MinGW-GCC 10 doesn't provide). The reference is only
 * called from the GTE trace logger; that path is gated on the logger
 * being enabled, so production runs never reach this stub. If they do,
 * post_data/post_ctrl get zeroed (the trace will just compare against
 * all-zero pcsx-redux state). */

#include <stdint.h>
#include <string.h>

void pcsxr_gte_run(uint32_t cmd,
                   const uint32_t pre_data[32],
                   const uint32_t pre_ctrl[32],
                   uint32_t       post_data[32],
                   uint32_t       post_ctrl[32]) {
    (void)cmd;
    (void)pre_data;
    (void)pre_ctrl;
    memset(post_data, 0, sizeof(uint32_t) * 32);
    memset(post_ctrl, 0, sizeof(uint32_t) * 32);
}
