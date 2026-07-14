/* crt-filter — mod-shader demo: engine-compiled GLSL postfx.
 *
 * S1 exercises shader_register (the compile service); postfx_register
 * refuses until the executor lands (S3) — tolerated here so this mod
 * proves the compile tier today and lights up automatically when the
 * chain arrives.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    int frag = api->shader_register(self, "shaders/crt.frag");
    if (frag < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "crt.frag failed to compile — filter unavailable "
                 "(see .build/shaders/)");
        return 0;   /* stay loaded: a broken shader never bricks a boot */
    }
    api->log(self, OP_MOD_LOG_INFO, "crt.frag compiled (handle %d)", frag);

    int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, 0);
    if (fx < 0) {
        api->log(self, OP_MOD_LOG_INFO,
                 "postfx chain not in this engine build yet — shader "
                 "compiled and ready");
        return 0;
    }
    return api->postfx_enable(self, fx, 1);
}
