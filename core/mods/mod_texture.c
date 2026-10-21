/* mod_texture.c - mod-owned RGBA8 textures for the native renderer.
 *
 * The stress-test gap this closes: mods can REPLACE existing texture
 * uploads (HD pack) but had no way to introduce NEW texture content -
 * every VRAM rect belongs to the game (levels upload a full 512x512
 * image over the whole right half, so there is no safely claimable
 * rect; the spyro3-chests first cut proved it by corrupting terrain).
 *
 * Mechanism: the mod registers RGBA8 pixels under a mod-chosen 16-bit
 * id. Model faces select the texture with a PSX-RESERVED Tiledef
 * encoding - tpage MODE bits (attr>>7 & 3) == 3 - carrying the id in
 * the CLUT u16. The native renderer resolves the id here and binds the
 * texture through the HD-replacement pipe (draw_tex_runs); UVs are raw
 * texel coords into the registered image. Native-renderer-only by
 * design: the PSX path samples VRAM noise for such faces (mode 3 does
 * not exist on hardware), which the og/debug panes show as garbage.
 * Host-side only - guest RAM, VRAM and the oracle are untouched.
 *
 * Live modset toggle. A registration OUTLIVES its owner's disable and
 * keeps resolving. That is deliberate, and the reasoning is the mode-3
 * encoding itself: it exists nowhere in stock game data, so every face
 * that selects a mod texture was written into guest memory by a mod
 * (patched faceblocks, repointed model rows). Those guest bytes are the
 * mod's to undo - from its toggle hook, or at its next install pass -
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
#include "mod_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "stb_image.h"   /* stbi_load; the decoder is stb_image_impl.c */

/* Entries the first block holds; the table grows by whole blocks. */
#define MOD_TEXTURE_BLOCK 64
#define MOD_TEXTURE_DIM_MAX 1024

/* One registered image. Immutable once published except for the
 * renderer-side bookkeeping below the divider, which only the one thread
 * that resolves textures (the display owner) touches. A replacement
 * publishes a new image; a superseded one is never freed (see
 * tex_retire), so a resolve that loaded it keeps valid pixels. */
typedef struct mod_tex_image {
    unsigned char* rgba;          /* malloc'd copy, 4*w*h */
    int w, h;
    const char* owner;            /* mod id (loader-owned, lives for the session) */
    /* resolver-owned */
    unsigned owner_epoch;         /* toggle epoch + 1 the owner verdict was taken at
                                   * (0 = never taken; epoch 0 is itself valid) */
    unsigned char owner_off;      /* owner was disabled as of owner_epoch */
    unsigned char warned;         /* disabled-owner draw already logged this epoch */
    unsigned char announced;      /* "first drawn" already logged */
} mod_tex_image;

/* A table slot: the id and the current image. A slot is zeroed by the
 * push before the count admits it, its id is written before its first
 * image is published, and the image pointer is the only field written
 * after that, so a resolver that acquire-loads a non-NULL image may read
 * the id and the image's fields without a lock. */
typedef struct {
    uint16_t id;
    mod_tex_image* img;           /* __atomic_* access only */
} mod_texture_t;

/* Chunked: openpete_mods_texture_get walks this from the display thread
 * while a registration appends from any thread; a block never moves. */
static mod_cvec s_tex;   /* mod_texture_t */

/* Registrations come from entry, tick hooks and threads a mod starts, so
 * writers serialize on this spin lock (the vec has one writer at a time).
 * Resolves never take it. Registration is an entry/reload-time event, so
 * the lock is never contended on a hot path. */
static char s_tex_lock;

/** @brief Take the registration spin lock. */
static void tex_lock(void) {
    while (__atomic_test_and_set(&s_tex_lock, __ATOMIC_ACQUIRE)) { }
}

/** @brief Release the registration spin lock. */
static void tex_unlock(void) {
    __atomic_clear(&s_tex_lock, __ATOMIC_RELEASE);
}

/* Superseded images, kept alive forever.
 *
 * The renderer's GPU-texture cache keys on the PIXEL POINTER and never
 * evicts, so an address that has been handed out once must never be
 * handed out again: a recycled malloc block would resolve to the GPU
 * texture uploaded from the OLD image and the new pixels would never
 * appear. A resolver on another thread may also hold a superseded image
 * it loaded just before the replacement was published, and a draw only
 * QUEUES the upload, which reads the pixels at the next frame start.
 * Retiring every superseded image instead of freeing it makes the pointer
 * identity monotone for the session and closes both use-after-frees. The
 * list keeps every retained image reachable, so a leak checker reports
 * them as live rather than lost. */
typedef struct retired_img {
    struct retired_img* next;
    mod_tex_image* img;
} retired_img;

static retired_img* s_retired;
static int s_retired_n;

#define MOD_TEXTURE_RETIRE_WARN 32    /* re-registration is an entry/hot-reload
                                       * event; a flood means per-tick churn,
                                       * which also floods the GPU cache */

/** @brief Keep a superseded image alive for the rest of the session.
 *
 * Its pixel address is never handed to the allocator again, so the
 * renderer's pointer-keyed GPU texture cache cannot resolve a new image to
 * an old texture, and a resolver still holding it reads valid memory. When
 * the list node cannot be allocated the image is still not freed and is
 * only untracked. A warning logs when the retained count reaches
 * MOD_TEXTURE_RETIRE_WARN. Called with the registration lock held.
 *
 * @param img The superseded image. */
static void tex_retire(mod_tex_image* img) {
    retired_img* r = (retired_img*)malloc(sizeof *r);
    if (r) { r->img = img; r->next = s_retired; s_retired = r; }
    /* No node: the image simply leaks untracked - still never recycled. */
    if (++s_retired_n == MOD_TEXTURE_RETIRE_WARN)
        OPL_WARN_F("mods", "texture: %d superseded images retained - "
                   "register textures at entry, not per tick",
                   s_retired_n);
}

/** @brief Refresh an image's cached owner-disabled flag when the mod toggle epoch has moved.
 *
 * One epoch compare on the render path; the owner lookup runs only when
 * the epoch differs from the one the flag was taken at (stored as epoch + 1,
 * so 0 means never taken). A refresh also clears the disabled-owner
 * warning latch, so each epoch logs at most once per image.
 * @param t Image to refresh. */
static void tex_poll_owner(mod_tex_image* t) {
    unsigned ep = openpete_mods_toggle_epoch() + 1u;
    if (t->owner_epoch == ep) return;
    t->owner_epoch = ep;
    t->owner_off = (unsigned char)(t->owner &&
                                   !openpete_mods_is_enabled(t->owner));
    t->warned = 0;
}

/** @brief Register or replace the RGBA8 image of a texture id for an owner.
 *
 * Copies the pixels into a new image outside the lock, so the source may be
 * freed after the call, then publishes it under the registration lock with
 * a release store: an existing slot with the id gets the new image
 * whatever its owner, and its old image is retired; a new id appends a
 * slot whose id is written before its image is published. Never gated on
 * the owner's enabled state. Legal from any thread.
 *
 * @param owner Owning mod id, kept by pointer; may be NULL.
 * @param id Texture id.
 * @param rgba @p w * @p h RGBA8 pixels.
 * @param w Width, 1 to MOD_TEXTURE_DIM_MAX.
 * @param h Height, 1 to MOD_TEXTURE_DIM_MAX.
 * @return 0, or -1 for NULL pixels, a size out of range, or an allocation
 *         failure. */
static int tex_register_owned(const char* owner, uint16_t id,
                              const void* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0 ||
        w > MOD_TEXTURE_DIM_MAX || h > MOD_TEXTURE_DIM_MAX)
        return -1;
    mod_tex_image* img = (mod_tex_image*)calloc(1, sizeof *img);
    unsigned char* copy = (unsigned char*)malloc((size_t)(4 * w * h));
    if (!img || !copy) { free(img); free(copy); return -1; }
    memcpy(copy, rgba, (size_t)(4 * w * h));
    img->rgba = copy;
    img->w = w;
    img->h = h;
    img->owner = owner;

    tex_lock();
    mod_texture_t* t = NULL;
    for (size_t i = 0; i < mod_cvec_count(&s_tex); i++) {
        mod_texture_t* c = (mod_texture_t*)mod_cvec_at(&s_tex, i);
        if (c->id == id && __atomic_load_n(&c->img, __ATOMIC_RELAXED)) {
            t = c;
            break;
        }
    }
    if (!t) {
        if (!s_tex.elem_size)
            mod_cvec_init(&s_tex, sizeof(mod_texture_t), MOD_TEXTURE_BLOCK,
                          "mod_texture", MOD_TEXTURE_BLOCK);
        t = (mod_texture_t*)mod_cvec_push(&s_tex);
        if (!t) {
            tex_unlock();
            free(copy);
            free(img);
            return -1;
        }
        t->id = id;
    }
    mod_tex_image* old = __atomic_load_n(&t->img, __ATOMIC_RELAXED);
    __atomic_store_n(&t->img, img, __ATOMIC_RELEASE);
    if (old) tex_retire(old);
    tex_unlock();
    OPL_INFO_F("mods", "texture: id %#06x %dx%d registered by %s",
               id, w, h, owner ? owner : "?");
    return 0;
}

int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h) {
    return tex_register_owned(openpete_mods_id_of(self), id, rgba, w, h);
}

/* ── Declarative own-textures: assets/textures/own/<hexid>.png ─────── */
/* Registers each <hexid>.png under <dir>/textures/own/; see the header. */
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
                       "hex id - skipped", path, de->d_name);
            continue;
        }
        char file[sizeof path + 1 + sizeof de->d_name];
        snprintf(file, sizeof file, "%s/%s", path, de->d_name);
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load(file, &w, &h, &comp, 4);
        if (!px) {
            OPL_ERROR_F("mods", "texture: %s: PNG decode failed - skipped",
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
    for (size_t i = 0; i < mod_cvec_count(&s_tex); i++) {
        mod_texture_t* t = (mod_texture_t*)mod_cvec_at(&s_tex, i);
        mod_tex_image* img = __atomic_load_n(&t->img, __ATOMIC_ACQUIRE);
        if (!img || t->id != id) continue;
        tex_poll_owner(img);
        if (img->owner_off && !img->warned) {
            img->warned = 1;
            OPL_INFO_F("mods", "texture: id %#06x still drawn while owner %s "
                       "is disabled - the faces selecting it are that mod's "
                       "guest writes, and revert with them", id, img->owner);
        }
        if (!img->announced) {
            img->announced = 1;
            OPL_INFO_F("mods", "texture: id %#06x first drawn (owner %s)",
                       id, img->owner ? img->owner : "?");
        }
        if (w) *w = img->w;
        if (h) *h = img->h;
        return img->rgba;
    }
    return NULL;
}
