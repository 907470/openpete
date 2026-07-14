#include <openpete_mod_api.h>

/* Runs at extraction per matching moby instance (cpu available, guest
 * RAM readable). Returns a per-draw uniform block, or -1 to decline
 * (that instance keeps stock shading). Here: a stable per-vaddr hue —
 * a real mod would read gameplay state, e.g. moby FLAMED heat. */
static int per_instance(CPUState* cpu, uint32_t moby, void* out, uint32_t cap) {
    (void)cpu;
    if (cap < 16) return -1;
    uint32_t h = moby * 2654435761u;
    float* v = (float*)out;
    v[0] = ((float)( h        & 0xFF) / 255.0f) * 0.6f + 0.3f;
    v[1] = ((float)((h >> 8)  & 0xFF) / 255.0f) * 0.6f + 0.3f;
    v[2] = ((float)((h >> 16) & 0xFF) / 255.0f) * 0.6f + 0.3f;
    v[3] = 1.0f;
    return 16;
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) {
        api->log(self, OP_MOD_LOG_ERROR, "tint.frag failed — selector inert");
        return 0;
    }
    openpete_mod_material_selector_t sel = { OP_RP_MOBY, -1, -1, 0 };
    return api->material_register(self, &sel, -1, fs, per_instance);
}
