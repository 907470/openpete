/* demo-lib - the library half of the deps rung: a dependency whose
 * entry must run before any dependent's (the loader topo-sorts deps
 * ahead of the enabled-list order). The log line is the proof a
 * dependent's entry can rely on this mod being live. */

#include <openpete_mod_api.h>

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->log(self, OP_MOD_LOG_INFO, "demo-lib: entry ran");
    api->ui_status(self, "library: dependents' entries run after this one");
    return 0;
}
