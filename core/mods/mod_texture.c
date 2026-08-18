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
 *
 * Live modset toggle. A registration OUTLIVES its owner's disable and
 * keeps resolving. That is deliberate, and the reasoning is the mode-3
 * encoding itself: it exists nowhere in stock game data, so every face
 * that selects a mod texture was written into guest memory by a mod
 * (patched faceblocks, repointed model rows). Those guest bytes are the
 * mod's to undo — from its toggle hook, or at its next install pass —
 * and until it does, the geometry is on screen either way. Blanking the
 * texture would not remove one face; it would only strip the art off
 * still-live mod geometry, and since the renderer draws NOTHING for a
 * mode-3 run it cannot resolve, whole objects would go invisible while
 * their collision stayed solid. Serving the pixels keeps the two halves
 * of the mod's own state consistent, so art and geometry revert
 * together the moment the owner undoes its guest writes. Consumption by
 * a disabled owner is logged once per modset change, naming the mod
 * whose guest patches outlived its disable.
 *
 * Registration is never gated on the owner being enabled: a deferred-
 * entry mod runs its entry BEFORE the loader flips its enabled flag,
 * and a hot reload re-runs entry for a mod that is currently disabled.
 * Refusing either would silently lose that mod's textures.
 */

#include "openpete_mods.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* Static PNG-only decoder instance, same pattern as texture_pack.c —
 * the stb implementation is file-local in each consumer. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../third_party/stb/stb_image.h"

#define MOD_TEXTURE_MAX 64
#define MOD_TEXTURE_DIM_MAX 1024

typedef struct {
    uint16_t id;
    int w, h;
    unsigned char* rgba;          /* malloc'd copy, 4*w*h */
    const char* owner;            /* mod id (loader-owned, lives for the session) */
    unsigned owner_epoch;         /* toggle epoch + 1 the owner verdict was taken at
                                   * (0 = never taken; epoch 0 is itself valid) */
    unsigned char owner_off;      /* owner was disabled as of owner_epoch */
    unsigned char warned;         /* disabled-owner draw already logged this epoch */
    unsigned char drawn;          /* pixels have been handed to the renderer */
    unsigned char announced;      /* "first drawn" already logged for this id */
} mod_texture_t;

static mod_texture_t s_tex[MOD_TEXTURE_MAX];
static int s_tex_count;

/* Superseded pixel buffers, kept alive forever.
 *
 * The renderer's GPU-texture cache keys on the PIXEL POINTER and never
 * evicts, so an address that has been handed out once must never be
 * handed out again: a recycled malloc block would resolve to the GPU
 * texture uploaded from the OLD image and the new pixels would never
 * appear. Freeing a superseded buffer puts its address straight back in
 * the allocator's fast bins, where the very next same-sized
 * registration picks it up — the failure mode is a second hot reload
 * rendering the FIRST build's image. Retiring instead of freeing makes
 * the pointer identity monotone for the session. (The GPU texture the
 * old buffer keyed is retained by the same cache anyway.)
 *
 * Only buffers the renderer actually took can be keyed, so an image
 * replaced before it was ever drawn is freed normally. Retention also
 * closes a use-after-free: a draw only QUEUES the upload, and the
 * renderer reads the pixels at the next frame start, after a
 * re-registration in the same tick would have freed them. The list
 * keeps every retained buffer reachable, so a leak checker reports
 * them as live rather than lost. */
typedef struct retired_px {
    struct retired_px* next;
    unsigned char* px;
} retired_px;

static retired_px* s_retired;
static int s_retired_n;

#define MOD_TEXTURE_RETIRE_WARN 32    /* re-registration is an entry/hot-reload
                                       * event; a flood means per-tick churn,
                                       * which also floods the GPU cache */

static void tex_retire(unsigned char* px) {
    retired_px* r = (retired_px*)malloc(sizeof *r);
    if (r) { r->px = px; r->next = s_retired; s_retired = r; }
    /* No node: the buffer simply leaks untracked — still never recycled. */
    if (++s_retired_n == MOD_TEXTURE_RETIRE_WARN)
        OPL_WARN_F("mods", "texture: %d superseded images retained — "
                   "register textures at entry, not per tick",
                   s_retired_n);
}

/* Re-take the owner's enabled verdict when the modset has changed. One
 * epoch compare on the render path; the id lookup runs only on change. */
static void tex_poll_owner(mod_texture_t* t) {
    unsigned ep = openpete_mods_toggle_epoch() + 1u;
    if (t->owner_epoch == ep) return;
    t->owner_epoch = ep;
    t->owner_off = (unsigned char)(t->owner &&
                                   !openpete_mods_is_enabled(t->owner));
    t->warned = 0;
}

static int tex_register_owned(const char* owner, uint16_t id,
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
    /* Re-registration (hot reload): the new pixel pointer keys a fresh
     * GPU texture; the old GPU entry stays cached (frame-safe, same
     * policy as the HD cache) and its buffer is retired, not freed, so
     * the address can never key it again. The copy is allocated BEFORE
     * the old buffer is released for the same reason. */
    if (t->rgba) {
        if (t->drawn) tex_retire(t->rgba);
        else          free(t->rgba);
    }
    t->rgba = copy;
    t->w = w;
    t->h = h;
    t->drawn = 0;
    t->owner = owner;
    t->owner_epoch = 0;           /* re-take the enabled verdict on next draw */
    OPL_INFO_F("mods", "texture: id %#06x %dx%d registered by %s",
               id, w, h, t->owner ? t->owner : "?");
    return 0;
}

int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h) {
    return tex_register_owned(openpete_mods_id_of(self), id, rgba, w, h);
}

/* ── Declarative own-textures: assets/textures/own/<hexid>.png ───────
 * The zero-C road for mod-owned texture content: each PNG under a
 * mod's assets/textures/own/ registers under the 16-bit id its
 * filename spells in hex ("53c1.png"). Faceblocks select it with the
 * same PSX-reserved mode-3 Tiledef encoding as the runtime entry;
 * generators derive the pixels from the user's disc into this
 * directory, so no game-derived data ships. Boot-scoped, like every
 * asset overlay. `owner` must be a session-lifetime string (the asset
 * root registry's copy). */
int openpete_mods_texture_scan_root(const char* dir, const char* owner) {
    char path[1200];
    snprintf(path, sizeof path, "%s/textures/own", dir);
    DIR* d = opendir(path);
    if (!d) return 0;
    int added = 0;
    struct dirent* de;
    while ((de = readdir(d))) {
        const char* dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".png") != 0) continue;
        unsigned id;
        char tail;
        char stem[16];
        size_t sl = (size_t)(dot - de->d_name);
        if (sl == 0 || sl >= sizeof stem) continue;
        memcpy(stem, de->d_name, sl);
        stem[sl] = '\0';
        if (sscanf(stem, "%4x%c", &id, &tail) != 1 || id > 0xFFFFu) {
            OPL_WARN_F("mods", "texture: %s/%s: filename is not a 16-bit "
                       "hex id — skipped", path, de->d_name);
            continue;
        }
        char file[1440];
        snprintf(file, sizeof file, "%s/%s", path, de->d_name);
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load(file, &w, &h, &comp, 4);
        if (!px) {
            OPL_ERROR_F("mods", "texture: %s: PNG decode failed — skipped",
                        file);
            continue;
        }
        int rc = tex_register_owned(owner, (uint16_t)id, px, w, h);
        stbi_image_free(px);
        if (rc == 0) added++;
        else OPL_ERROR_F("mods", "texture: %s: register failed (dims/"
                         "table)", file);
    }
    closedir(d);
    return added;
}

const unsigned char* openpete_mods_texture_get(uint16_t id, int* w, int* h) {
    for (int i = 0; i < s_tex_count; i++) {
        mod_texture_t* t = &s_tex[i];
        if (t->id != id) continue;
        tex_poll_owner(t);
        if (t->owner_off && !t->warned) {
            t->warned = 1;
            OPL_INFO_F("mods", "texture: id %#06x still drawn while owner %s "
                       "is disabled — the faces selecting it are that mod's "
                       "guest writes, and revert with them", id, t->owner);
        }
        t->drawn = 1;             /* pixels are now GPU-cache-keyed by address */
        if (!t->announced) {
            t->announced = 1;
            OPL_INFO_F("mods", "texture: id %#06x first drawn (owner %s)",
                       id, t->owner ? t->owner : "?");
        }
        if (w) *w = t->w;
        if (h) *h = t->h;
        return t->rgba;
    }
    return NULL;
}
