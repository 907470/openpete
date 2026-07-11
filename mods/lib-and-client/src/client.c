/* lib-and-client — rung 5 client: import a cross-mod service. */

#include <openpete_mod_api.h>

typedef int (*magic_fn)(void);

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    magic_fn magic = (magic_fn)api->import_sym(self, "demo-lib:magic");
    if (!magic) {
        /* Shouldn't happen — deps topo-sort loads demo-lib first — but
         * degrade loudly rather than crash. */
        api->log(self, OP_MOD_LOG_ERROR, "demo-lib:magic missing");
        return -1;
    }
    api->log(self, OP_MOD_LOG_INFO,
             "imported demo-lib:magic -> %d", magic());
    return 0;
}
