/* games/common/launcher_assets.c - see launcher_assets.h. */
#include "launcher_assets.h"

#include "game_build.h"
#include "game_host_desc.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_fnv1a64.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "renderer/cyclorama.h"
#include "renderer/portal_spyro23.h"   /* portal_game_s23 */
#include "renderer/text_moby.h"
#include "renderer/wad_subfile.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Reads a little-endian word of a host buffer, 0 when it does not fit.
 * @param b The buffer.
 * @param n Its length.
 * @param o The offset.
 * @return The word. */
static uint32_t la_w(const uint8_t* b, uint32_t n, uint32_t o) {
    if (o > n || n - o < 4u) return 0u;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

/** @brief Writes a formatted failure reason.
 * @param why The buffer.
 * @param cap Its capacity.
 * @param fmt The format.
 * @param ... Its arguments.
 * @return -1, the deterministic failure status. */
static int la_fail(char* why, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (why && cap) vsnprintf(why, cap, fmt, ap);
    va_end(ap);
    return -1;
}

/* ---- the boot executable's static data ------------------------------- */

/* The boot executable: its PSX-EXE header's load address and the extent of
 * its text, the image the game's static tables live in. */
typedef struct {
    disc_image_entry_t file;
    uint32_t t_addr, t_size;
} la_exe;

/** @brief Opens the boot executable SYSTEM.CNF's BOOT line names.
 *
 * The BOOT value is read after spaces, an optional "cdrom:" and leading
 * separators, up to ';' or the line end; its PSX-EXE header gives the load
 * address (+0x18) and text size (+0x1C).
 * @param img The disc.
 * @param x Receives the executable.
 * @return NULL, else the failure's reason. */
static const char* la_exe_open(disc_image_t* img, la_exe* x) {
    disc_image_entry_t cnf;
    char text[2049];
    if (disc_image_find(img, "SYSTEM.CNF", &cnf) != 0 || cnf.is_dir) return "no SYSTEM.CNF";
    const size_t n = disc_image_read_file(img, &cnf, text, sizeof text - 1);
    text[n] = 0;
    const char* p = strstr(text, "BOOT");
    if (!p) return "no BOOT line";
    p += 4;
    while (*p == ' ' || *p == '\t' || *p == '=') p++;
    if (!strncmp(p, "cdrom:", 6)) p += 6;
    while (*p == '\\' || *p == '/') p++;
    char path[64];
    size_t k = 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n' && *p != ' ' && k + 1 < sizeof path) path[k++] = *p++;
    path[k] = 0;
    if (!k || disc_image_find(img, path, &x->file) != 0 || x->file.is_dir) return "boot executable not found";
    uint8_t hdr[0x20];
    if (disc_image_read_file_range(img, &x->file, 0u, hdr, sizeof hdr) != sizeof hdr ||
        memcmp(hdr, "PS-X EXE", 8) != 0) return "boot executable header";
    x->t_addr = la_w(hdr, sizeof hdr, 0x18u);
    x->t_size = la_w(hdr, sizeof hdr, 0x1Cu);
    return NULL;
}

/** @brief Reads one byte of the boot executable's image at a guest address.
 * @param img The disc.
 * @param x The executable.
 * @param addr The address.
 * @param out Receives the byte.
 * @return 0, or -1 when the address lies outside the text. */
static int la_exe_byte(disc_image_t* img, const la_exe* x, uint32_t addr, uint8_t* out) {
    if (addr < x->t_addr || addr - x->t_addr >= x->t_size) return -1;
    return disc_image_read_file_range(img, &x->file, 0x800u + (addr - x->t_addr), out, 1) == 1 ? 0 : -1;
}

/* ---- the sky payloads ------------------------------------------------- */

/** @brief Reads a Spyro 1 level's sky payload: the level header's data, past its leading components.
 * @param img The disc.
 * @param A The game's asset descriptor.
 * @param number The level number.
 * @param len Receives the payload's length.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return The payload, which the caller frees, or NULL. */
static uint8_t* la_sky_header(disc_image_t* img, const game_assets_desc_t* A, int number, uint32_t* len,
                              char* why, size_t cap) {
    const int index = (number / 10 - 1) * 6 + number % 10;
    const int e = A->sky_wad_base + 2 * index;
    uint32_t hn = 0, dn = 0;
    uint8_t* hdr = wad_disc_entry_load(img, e, 0u, 0x800u, &hn, "launcher");
    if (!hdr) { la_fail(why, cap, "level %d: WAD entry %d header unreadable", number, e); return NULL; }
    const uint32_t doff = la_w(hdr, hn, 8u), dsz = la_w(hdr, hn, 12u);
    free(hdr);
    uint8_t* data = dsz ? wad_disc_entry_load(img, e, doff, dsz, &dn, "launcher") : NULL;
    if (!data) { la_fail(why, cap, "level %d: WAD entry %d data unreadable", number, e); return NULL; }
    uint32_t p = 0;
    for (int i = 0; i < A->sky_components; i++) {
        const uint32_t sz = la_w(data, dn, p);
        if (sz < 4u || sz > dn - p) { free(data); la_fail(why, cap, "level %d: component chain", number); return NULL; }
        p += sz;
    }
    const uint32_t sz = la_w(data, dn, p);
    if (sz < 12u || sz > dn - p) { free(data); la_fail(why, cap, "level %d: sky block", number); return NULL; }
    uint8_t* sky = (uint8_t*)malloc(sz - 4u);
    if (sky) memcpy(sky, data + p + 4u, sz - 4u);
    free(data);
    *len = sz - 4u;
    return sky;
}

/** @brief Reads a Spyro 2/3 level's sky payload through the slot table and the loader's block walk.
 * @param img The disc.
 * @param x The boot executable.
 * @param A The game's asset descriptor.
 * @param number The level number.
 * @param len Receives the payload's length.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return The payload, which the caller frees, or NULL. */
static uint8_t* la_sky_chain(disc_image_t* img, const la_exe* x, const game_assets_desc_t* A, int number,
                             uint32_t* len, char* why, size_t cap) {
    uint8_t slot = 0;
    if (number < 0 || number > 255 || !portal_game_s23.level_slots ||
        la_exe_byte(img, x, portal_game_s23.level_slots + (uint32_t)number, &slot) != 0) {
        la_fail(why, cap, "level %d: slot table not in the executable", number);
        return NULL;
    }
    const int e = portal_game_s23.level_wad_base + 2 * slot;
    uint32_t n = 0, at = 0, plen = 0;
    uint8_t* sub = wad_disc_subfile_load(img, e, 1, &n, "launcher");
    if (sub) at = A->sky_walk(sub, n, &plen);
    if (!at) { free(sub); la_fail(why, cap, "level %d: WAD entry %d holds no sky block", number, e); return NULL; }
    uint8_t* sky = (uint8_t*)malloc(plen);
    if (sky) memcpy(sky, sub + at, plen);
    free(sub);
    *len = plen;
    return sky;
}

/* ---- the dome mesh ----------------------------------------------------- */

/* The decode's receiver: vertices deduplicated by (position, colour) in an
 * open-addressed table, triangles appended. */
typedef struct {
    int32_t*  key;      /* 4 words per vertex: x, y, z, rgb */
    uint32_t  nv, vcap;
    uint32_t* slot;     /* hash slots holding vertex index + 1, 0 empty */
    uint32_t  scap;
    uint16_t* tri;
    uint32_t  ntri, tcap;
    int       overflow;
} la_mesh;

/** @brief Returns the index of a (position, colour) vertex, adding it on first use.
 * @param M The mesh.
 * @param x The X.
 * @param y The Y.
 * @param z The Z.
 * @param rgb The colour.
 * @return The index, or -1 past 65535 vertices or on allocation failure. */
static int la_vertex(la_mesh* M, int32_t x, int32_t y, int32_t z, uint32_t rgb) {
    if (M->nv * 2u >= M->scap) {
        const uint32_t ncap = M->scap ? M->scap * 2u : 4096u;
        uint32_t* ns = (uint32_t*)calloc(ncap, sizeof *ns);
        if (!ns) return -1;
        for (uint32_t i = 0; i < M->nv; i++) {
            const int32_t* k = M->key + 4u * i;
            uint64_t h = openpete_fnv1a64(k, 16);
            while (ns[h & (ncap - 1u)]) h++;
            ns[h & (ncap - 1u)] = i + 1u;
        }
        free(M->slot);
        M->slot = ns; M->scap = ncap;
    }
    const int32_t k[4] = { x, y, z, (int32_t)rgb };
    uint64_t h = openpete_fnv1a64(k, sizeof k);
    for (;; h++) {
        const uint32_t s = M->slot[h & (M->scap - 1u)];
        if (!s) break;
        if (!memcmp(M->key + 4u * (s - 1u), k, sizeof k)) return (int)(s - 1u);
    }
    if (M->nv >= 65536u) return -1;
    if (M->nv == M->vcap) {
        const uint32_t ncap = M->vcap ? M->vcap * 2u : 1024u;
        int32_t* nk = (int32_t*)realloc(M->key, (size_t)ncap * sizeof k);
        if (!nk) return -1;
        M->key = nk; M->vcap = ncap;
    }
    memcpy(M->key + 4u * M->nv, k, sizeof k);
    M->slot[h & (M->scap - 1u)] = M->nv + 1u;
    return (int)M->nv++;
}

/** @brief Receives one decoded sky face (decoded under the identity, so the corners are the game's vertex positions).
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @param gouraud Unused: a flat face carries its one colour on every corner.
 * @param ctx The la_mesh. */
static void la_face(ngte_cvert a, ngte_cvert b, ngte_cvert c, int gouraud, void* ctx) {
    (void)gouraud;
    la_mesh* M = (la_mesh*)ctx;
    const ngte_cvert* v[3] = { &a, &b, &c };
    int id[3];
    for (int i = 0; i < 3; i++) {
        id[i] = la_vertex(M, (int32_t)v[i]->m1, (int32_t)v[i]->m2, (int32_t)v[i]->m3, v[i]->rgb);
        if (id[i] < 0) { M->overflow = 1; return; }
    }
    if (M->ntri == M->tcap) {
        const uint32_t ncap = M->tcap ? M->tcap * 2u : 1024u;
        uint16_t* nt = (uint16_t*)realloc(M->tri, (size_t)ncap * 3u * sizeof *nt);
        if (!nt) { M->overflow = 1; return; }
        M->tri = nt; M->tcap = ncap;
    }
    for (int i = 0; i < 3; i++) M->tri[3u * M->ntri + (uint32_t)i] = (uint16_t)id[i];
    M->ntri++;
}

/** @brief Decodes a sky payload into a dome: every sector through cyclorama_decode_sector under the identity, no cull and no fade.
 *
 * The payload is {colour bytes and a pad, the sector count, one offset per
 * sector relative to the payload}; a sector offset of 0 or past the
 * payload is skipped, at most 1024 sectors.
 * @param D The decode scratch.
 * @param P The payload.
 * @param len Its length.
 * @param out Receives the dome.
 * @return 0, or -1 when the dome exceeds 65536 vertices or memory runs out. */
static int la_dome(cyclorama_sector_buf* D, const uint8_t* P, uint32_t len, game_asset_dome_t* out) {
    static const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    const cyclorama_fade none[2] = { { 0u, 0 }, { 0u, 0 } };
    const cyclorama_src src = { NULL, P, 0u, len };
    la_mesh M;
    memset(&M, 0, sizeof M);
    const uint32_t n = la_w(P, len, 4u);
    for (uint32_t i = 0; i < n && i < 1024u && !M.overflow; i++) {
        const uint32_t off = la_w(P, len, 8u + i * 4u);
        if (off && off < len) cyclorama_decode_sector(D, &src, off, I, 1.0, none, la_face, &M);
    }
    free(M.slot);
    if (M.overflow) { free(M.key); free(M.tri); return -1; }
    out->verts = (uint8_t*)malloc((size_t)M.nv * 9u + 1u);
    if (!out->verts) { free(M.key); free(M.tri); return -1; }
    for (uint32_t i = 0; i < M.nv; i++) {
        const int32_t* k = M.key + 4u * i;
        const double x = k[0], y = k[1], z = k[2];
        double L = sqrt(x * x + y * y + z * z);
        if (L == 0.0) L = 1.0;
        /* round half to even, the rounding the default FP mode gives nearbyint */
        const int16_t q[3] = { (int16_t)nearbyint(x / L * 32767.0), (int16_t)nearbyint(-y / L * 32767.0),
                               (int16_t)nearbyint(z / L * 32767.0) };
        uint8_t* o = out->verts + 9u * i;
        for (int c = 0; c < 3; c++) { o[2 * c] = (uint8_t)(q[c] & 0xFF); o[2 * c + 1] = (uint8_t)((uint16_t)q[c] >> 8); }
        const uint32_t rgb = (uint32_t)k[3];
        o[6] = (uint8_t)(rgb & 0xFF); o[7] = (uint8_t)((rgb >> 8) & 0xFF); o[8] = (uint8_t)((rgb >> 16) & 0xFF);
    }
    const uint32_t bg = la_w(P, len, 0u);
    out->bg[0] = (uint8_t)(bg & 0xFF); out->bg[1] = (uint8_t)((bg >> 8) & 0xFF); out->bg[2] = (uint8_t)((bg >> 16) & 0xFF);
    out->nv = M.nv; out->ntri = M.ntri; out->tris = M.tri; out->sectors = n;
    free(M.key);
    return 0;
}

/* ---- the title logo ---------------------------------------------------- */

/** @brief Decodes the title logo's drawn region to RGBA and hashes its texel rows and palette.
 *
 * Each 15-bit palette colour expands to 8 bits per channel by a left shift
 * of 3; index 0 and colour 0 are transparent.
 * @param img The disc.
 * @param A The game's asset descriptor.
 * @param out Receives the logo.
 * @param hash Receives the FNV-1a 64 of the rows, then the palette.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
static int la_logo(disc_image_t* img, const game_assets_desc_t* A, game_assets_t* out, uint64_t* hash,
                   char* why, size_t cap) {
    uint32_t n = 0;
    uint8_t* b = A->logo.subfile < 0 ? wad_disc_entry_load(img, A->logo.entry, 0u, 0u, &n, "launcher")
                                     : wad_disc_subfile_load(img, A->logo.entry, A->logo.subfile, &n, "launcher");
    if (!b) return la_fail(why, cap, "logo: WAD entry %d unreadable", A->logo.entry);
    const uint32_t w = A->logo.w, h = A->logo.h;
    if (!w || !h || w > 1024u || A->logo.palette > n || n - A->logo.palette < 512u ||
        A->logo.texels > n || (n - A->logo.texels) / 1024u < h - 1u ||
        n - A->logo.texels - (h - 1u) * 1024u < w) {
        free(b);
        return la_fail(why, cap, "logo: region outside WAD entry %d", A->logo.entry);
    }
    out->logo_rgba = (uint8_t*)malloc((size_t)w * h * 4u);
    if (!out->logo_rgba) { free(b); return la_fail(why, cap, "logo: allocation"); }
    uint64_t hs = openpete_fnv1a64_init();
    const uint8_t* pal = b + A->logo.palette;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = b + A->logo.texels + y * 1024u;
        hs = openpete_fnv1a64_update(hs, row, w);
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t v = (uint32_t)pal[2u * row[x]] | ((uint32_t)pal[2u * row[x] + 1u] << 8);
            uint8_t* o = out->logo_rgba + 4u * (y * w + x);
            o[0] = (uint8_t)((v & 31u) << 3);
            o[1] = (uint8_t)(((v >> 5) & 31u) << 3);
            o[2] = (uint8_t)(((v >> 10) & 31u) << 3);
            o[3] = (row[x] == 0 || v == 0) ? 0 : 255;
        }
    }
    *hash = openpete_fnv1a64_update(hs, pal, 512u);
    out->logo_w = w; out->logo_h = h;
    free(b);
    return 0;
}

/* ---- the job ------------------------------------------------------------ */

/** @brief Picks the known disc whose hashes the inputs are checked against: the one the image's content hash names, else the first.
 *
 * An image whose bytes differ from a known disc's outside the asset inputs
 * (another dump, a rebuilt image) is checked against the first known disc,
 * so its inputs pass only where they equal that disc's.
 * @param A The game's asset descriptor.
 * @param sha The image's content hash, or NULL.
 * @return The hashes, or NULL when the game lists no known disc. */
static const game_asset_hashes_t* la_known(const game_assets_desc_t* A, const char* sha) {
    for (int i = 0; sha && i < A->disc_count; i++)
        if (!strcmp(A->discs[i].disc_sha256, sha)) return &A->discs[i];
    return A->disc_count > 0 ? &A->discs[0] : NULL;
}

/** @brief Extracts every level's dome, checking each payload's hash.
 * @param img The disc.
 * @param A The game's asset descriptor.
 * @param K The known disc's hashes.
 * @param out Receives the domes.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, -1 or -2 as game_assets_extract. */
static int la_domes(disc_image_t* img, const game_assets_desc_t* A, const game_asset_hashes_t* K,
                    game_assets_t* out, char* why, size_t cap) {
    const game_level_table_t* T = A->levels;
    if (A->sky_form == GAME_SKY_NONE || !T) return 0;
    if (K->sky_count != (int)T->count)
        return la_fail(why, cap, "sky hashes: %d for %zu levels", K->sky_count, T->count);
    la_exe x;
    memset(&x, 0, sizeof x);
    if (A->sky_form == GAME_SKY_SLOT_CHAIN) {
        const char* r = la_exe_open(img, &x);
        if (r) return la_fail(why, cap, "%s", r);
    }
    out->domes = (game_asset_dome_t*)calloc(T->count, sizeof *out->domes);
    cyclorama_sector_buf* D = (cyclorama_sector_buf*)malloc(sizeof *D);
    if (!out->domes || !D) { free(D); return la_fail(why, cap, "dome allocation"); }
    int rc = 0;
    for (size_t i = 0; i < T->count && rc == 0; i++) {
        if (openpete_launcher_job_cancelled()) { rc = -2; break; }
        const game_level_t* L = &T->levels[i];
        uint32_t len = 0;
        uint8_t* P = A->sky_form == GAME_SKY_LEVEL_HEADER ? la_sky_header(img, A, L->number, &len, why, cap)
                                                          : la_sky_chain(img, &x, A, L->number, &len, why, cap);
        if (!P) { rc = -1; break; }
        const uint64_t h = openpete_fnv1a64(P, len);
        if (h != K->sky[i]) {
            rc = la_fail(why, cap, "level %s: sky payload hash %016llx, the known disc's %016llx", L->name,
                         (unsigned long long)h, (unsigned long long)K->sky[i]);
        } else {
            game_asset_dome_t* d = &out->domes[out->dome_count];
            d->level = L->name;
            if (la_dome(D, P, len, d) != 0) rc = la_fail(why, cap, "level %s: dome decode", L->name);
            else out->dome_count++;
        }
        free(P);
    }
    free(D);
    return rc;
}

int game_assets_extract(const openpete_game_build_t* b, const char* disc_path, const char* disc_sha256,
                        game_assets_t* out, char* why, size_t why_cap) OP_REQUIRES(ingest) {
    memset(out, 0, sizeof *out);
    if (why && why_cap) why[0] = 0;
    const game_assets_desc_t* A = (b && b->runtime && b->runtime->host) ? b->runtime->host->assets : NULL;
    if (!A) return la_fail(why, why_cap, "the game declares no launcher assets");
    const game_asset_hashes_t* K = la_known(A, disc_sha256);
    if (!K) return la_fail(why, why_cap, "%s lists no known disc", b->id);
    disc_image_t* img = disc_image_open(disc_path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_INGEST);
    if (!img) return la_fail(why, why_cap, "disc %s unreadable", disc_path);
    int rc = la_domes(img, A, K, out, why, why_cap);
    if (rc == 0 && openpete_launcher_job_cancelled()) rc = -2;
    if (rc == 0) {
        uint64_t h = 0;
        out->glyphs = text_glyph_set_new();
        if (!out->glyphs || text_glyph_set_read_disc(out->glyphs, img, &h) != 0)
            rc = la_fail(why, why_cap, "glyph models: decode failed");
        else if (h != K->glyphs)
            rc = la_fail(why, why_cap, "glyph models: hash %016llx, the known disc's %016llx",
                         (unsigned long long)h, (unsigned long long)K->glyphs);
    }
    if (rc == 0) {
        uint64_t h = 0;
        rc = la_logo(img, A, out, &h, why, why_cap);
        if (rc == 0 && h != K->logo)
            rc = la_fail(why, why_cap, "logo: hash %016llx, the known disc's %016llx",
                         (unsigned long long)h, (unsigned long long)K->logo);
    }
    disc_image_close(img);
    if (rc != 0) game_assets_free(out);
    return rc;
}

void game_assets_free(game_assets_t* a) {
    for (int i = 0; a->domes && i < a->dome_count; i++) { free(a->domes[i].verts); free(a->domes[i].tris); }
    free(a->domes);
    text_glyph_set_free(a->glyphs);
    free(a->logo_rgba);
    memset(a, 0, sizeof *a);
}

/** @brief Writes a buffer to `<dir>/<game>.<suffix>`.
 * @param dir The directory.
 * @param game The game id.
 * @param suffix The file's suffix.
 * @param p The bytes.
 * @param n Their count.
 * @return 0, or -1. */
static int la_put(const char* dir, const char* game, const char* suffix, const void* p, size_t n) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.%s", dir, game, suffix);
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(p, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int game_assets_write(const char* game, const game_assets_t* a, const char* dir) {
    int rc = 0;
    char sfx[256];
    for (int i = 0; i < a->dome_count; i++) {
        const game_asset_dome_t* d = &a->domes[i];
        snprintf(sfx, sizeof sfx, "dome.%s.v", d->level);
        rc |= la_put(dir, game, sfx, d->verts, (size_t)d->nv * 9u);
        snprintf(sfx, sizeof sfx, "dome.%s.i", d->level);
        rc |= la_put(dir, game, sfx, d->tris, (size_t)d->ntri * 6u);
        snprintf(sfx, sizeof sfx, "dome.%s.bg", d->level);
        rc |= la_put(dir, game, sfx, d->bg, 3u);
    }
    if (a->logo_rgba) {
        rc |= la_put(dir, game, "logo.rgba", a->logo_rgba, (size_t)a->logo_w * a->logo_h * 4u);
        char wh[32];
        const int n = snprintf(wh, sizeof wh, "%u %u\n", a->logo_w, a->logo_h);
        rc |= la_put(dir, game, "logo.txt", wh, (size_t)n);
    }
    if (a->glyphs) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s.glyphs.txt", dir, game);
        FILE* f = fopen(path, "w");
        if (!f) return -1;
        text_glyph_set_write(a->glyphs, f);
        rc |= fclose(f) == 0 ? 0 : -1;
    }
    return rc ? -1 : 0;
}

int game_assets_run_inline(const openpete_game_build_t* b, const char* disc_path, const char* disc_sha256,
                           const char* dump_dir) {
    unsigned prior = 0;
    OP_ROLE_ENTER(ingest, prior);
    OP_ROLE_ASSERT(ingest);
    game_assets_t a;
    char why[512];
    const int rc = game_assets_extract(b, disc_path, disc_sha256, &a, why, sizeof why);
    op_role_leave(prior);
    if (rc != 0) {
        OPL_WARN_F("launcher", "asset extraction of %s from %s %s: %s", b->id, disc_path,
                   rc == -2 ? "stopped" : "failed", why);
        return rc;
    }
    uint64_t nv = 0, nt = 0;
    for (int i = 0; i < a.dome_count; i++) { nv += a.domes[i].nv; nt += a.domes[i].ntri; }
    OPL_INFO_F("launcher", "asset extraction of %s: %d domes (%llu vertices, %llu triangles), glyphs %s, "
               "logo %ux%u", b->id, a.dome_count, (unsigned long long)nv, (unsigned long long)nt,
               text_glyph_set_ready(a.glyphs) ? "ready" : "none", a.logo_w, a.logo_h);
    if (dump_dir && game_assets_write(b->id, &a, dump_dir) != 0)
        OPL_WARN_F("launcher", "asset extraction of %s: writing %s failed", b->id, dump_dir);
    game_assets_free(&a);
    return 0;
}
