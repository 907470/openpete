/* demo-lib — rung 5 library: publish a cross-mod service. */

#include <openpete_mod_api.h>

static int magic(void) { return 42; }

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    return api->export_sym(self, "magic", (void*)&magic);
}
