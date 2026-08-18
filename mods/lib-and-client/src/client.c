/* lib-and-client — the client half of the deps rung: declares a hard
 * dependency on demo-lib, so the topo-sort runs demo-lib's entry before
 * this one even when this mod is listed FIRST in enabled.txt. Enabling
 * it WITHOUT demo-lib exercises the missing-dep error UX. */

#include <openpete_mod_api.h>

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->log(self, OP_MOD_LOG_INFO,
             "lib-and-client: entry ran (dependency loaded first)");
    api->ui_status(self, "depends on demo-lib; the topo-sort ran it first");
    return 0;
}
