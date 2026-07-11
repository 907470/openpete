/* mod_texture.c — mod-owned RGBA8 textures for the native renderer.
 *
 * The stress-test gap this closes: mods can REPLACE existing texture
 * uploads (HD pack) but had no way to introduce NEW texture content —
 * every VRAM rect belongs to the game (levels upload a full 512x512
 * image over the whole right half, so there is no safely claimable
 * rect; the spyro3-chests first cut proved it by corrupting terrain).
 *
 * Mechanism: the mod registers RGBA8 pixels under a mod-chosen 16-bit
 * id. Model faces select the texture with a PSX-RESERVED Tiledef
 * encoding — tpage MODE bits (attr>>7 & 3) == 3 — carrying the id in
 * the CLUT u16. The native renderer resolves the id here and binds the
 * texture through the HD-replacement pipe (draw_tex_runs); UVs are raw
 * texel coords into the registered image. Native-renderer-only by
 * design: the PSX path samples VRAM noise for such faces (mode 3 does
 * not exist on hardware), which the og/debug panes show as garbage.
 * Host-side only — guest RAM, VRAM and the oracle are untouched.
 */

#include "openpete_mods.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

#define MOD_TEXTURE_MAX 64
#define MOD_TEXTURE_DIM_MAX 1024

typedef struct {
    uint16_t id;
    int w, h;
    unsigned char* rgba;          /* malloc'd copy, 4*w*h */
    const char* owner;            /* mod id (diagnostics) */
} mod_texture_t;

static mod_texture_t s_tex[MOD_TEXTURE_MAX];
static int s_tex_count;

int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0 ||
        w > MOD_TEXTURE_DIM_MAX || h > MOD_TEXTURE_DIM_MAX)
        return -1;
    mod_texture_t* t = NULL;
    for (int i = 0; i < s_tex_count; i++)
        if (s_tex[i].id == id) { t = &s_tex[i]; break; }
    if (!t) {
        if (s_tex_count >= MOD_TEXTURE_MAX) return -1;
        t = &s_tex[s_tex_count++];
        t->id = id;
    }
    unsigned char* copy = (unsigned char*)malloc((size_t)(4 * w * h));
    if (!copy) return -1;
    memcpy(copy, rgba, (size_t)(4 * w * h));
    free(t->rgba);                /* re-registration (hot reload): the new
                                   * pixel pointer keys a fresh GPU texture;
                                   * the old GPU entry stays cached (frame-
                                   * safe, same policy as the HD cache) */
    t->rgba = copy;
    t->w = w;
    t->h = h;
    t->owner = openpete_mods_id_of(self);
    OPL_INFO_F("mods", "texture: id %#06x %dx%d registered by %s",
               id, w, h, t->owner ? t->owner : "?");
    return 0;
}

const unsigned char* openpete_mods_texture_get(uint16_t id, int* w, int* h) {
    for (int i = 0; i < s_tex_count; i++)
        if (s_tex[i].id == id) {
            if (w) *w = s_tex[i].w;
            if (h) *h = s_tex[i].h;
            return s_tex[i].rgba;
        }
    return NULL;
}
