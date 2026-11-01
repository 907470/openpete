/* games/common/launcher_cache.c - see launcher_cache.h. */
#include "launcher_cache.h"

#include "game_build.h"
#include "game_host_desc.h"
#include "launcher_assets.h"
#include "launcher_preview.h"
#include "openpete_atomic_file.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_paths.h"
#include "openpete_time.h"
#include "openpete_game_runtime.h"
#include "renderer/text_moby.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The outputs one extraction writes, in write order. */
#define LC_FILES 3
static const char* const k_lc_files[LC_FILES] = {"domes.bin", "glyphs.txt", "logo.rgba"};
/* lc_check: a rerun is due. */
#define LC_RERUN 2

/* A cache manifest as read. Empty strings for absent keys; format 0 when
 * the manifest is missing or names no format. */
typedef struct {
    int       format;
    char      disc[80];
    char      build[160];
    char      failed_build[160];
    char      failed_disc[80];
    char      failed_why[256];
    long long size[LC_FILES];   /* -1 when not listed */
    /* The preview: the disc hash and view it was drawn from, its size, and
     * its own failed marker. */
    char      pv_disc[80];
    char      pv_view[256];
    char      pv_failed_build[160];
    char      pv_failed_disc[80];
    char      pv_failed_why[256];
    long long pv_size;          /* -1 when not listed */
} lc_manifest;

/* The preview's file, beside the extraction outputs. */
#define LC_PREVIEW "preview.rgba"

/** @brief Copies the quoted value of a `key = "value"` line, or reports that the line is not that key.
 * @param line The line, without its newline.
 * @param key The key.
 * @param out Receives the value.
 * @param cap Its capacity.
 * @return 1 when the line is the key's, else 0. */
static int lc_str(const char* line, const char* key, char* out, size_t cap) {
    const size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || strncmp(line + k, " = \"", 4) != 0) return 0;
    const char* v = line + k + 4;
    const char* e = strrchr(v, '"');
    const size_t n = e ? (size_t)(e - v) : strlen(v);
    snprintf(out, cap, "%.*s", (int)n, v);
    return 1;
}

/** @brief Reads a `"<output>" = <size>` line of the files table into the manifest.
 * @param line The line.
 * @param m The manifest. */
static void lc_file_line(const char* line, lc_manifest* m) {
    const size_t pn = strlen(LC_PREVIEW);
    if (line[0] == '"' && !strncmp(line + 1, LC_PREVIEW, pn) && !strncmp(line + 1 + pn, "\" = ", 4))
        m->pv_size = atoll(line + pn + 5);
    for (int i = 0; i < LC_FILES; i++) {
        const size_t n = strlen(k_lc_files[i]);
        if (line[0] == '"' && !strncmp(line + 1, k_lc_files[i], n) && !strncmp(line + 1 + n, "\" = ", 4))
            m->size[i] = atoll(line + n + 5);
    }
}

/** @brief Reads a `preview_*` key line of the manifest into its preview record.
 * @param line The line.
 * @param m The manifest. */
static void lc_preview_line(const char* line, lc_manifest* m) {
    if (!lc_str(line, "preview_disc", m->pv_disc, sizeof m->pv_disc) &&
        !lc_str(line, "preview_view", m->pv_view, sizeof m->pv_view) &&
        !lc_str(line, "preview_failed_build", m->pv_failed_build, sizeof m->pv_failed_build) &&
        !lc_str(line, "preview_failed_disc", m->pv_failed_disc, sizeof m->pv_failed_disc))
        (void)lc_str(line, "preview_failed_why", m->pv_failed_why, sizeof m->pv_failed_why);
}

/** @brief Reads a game's cache manifest.
 * @param path The manifest's path.
 * @param m Receives it; zeroed first (format 0 when the file is missing).
 * @return 0 when the file opened, -1 otherwise. */
static int lc_read(const char* path, lc_manifest* m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i < LC_FILES; i++) m->size[i] = -1;
    m->pv_size = -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int files = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "[files]")) files = 1;
        else if (files) lc_file_line(line, m);
        else if (!strncmp(line, "format = ", 9)) m->format = atoi(line + 9);
        else if (!lc_str(line, "disc_sha256", m->disc, sizeof m->disc) &&
                 !lc_str(line, "build", m->build, sizeof m->build) &&
                 !lc_str(line, "failed_build", m->failed_build, sizeof m->failed_build) &&
                 !lc_str(line, "failed_disc", m->failed_disc, sizeof m->failed_disc) &&
                 !lc_str(line, "failed_why", m->failed_why, sizeof m->failed_why))
            lc_preview_line(line, m);
    }
    fclose(f);
    return 0;
}

/** @brief Writes a game's cache manifest atomically.
 *
 * A double quote or backslash in a value is written as a single quote.
 * @param path The manifest's path.
 * @param m The manifest.
 * @return 0, or -1 when the write failed. */
static int lc_write_manifest(const char* path, const lc_manifest* m) {
    char why[sizeof m->failed_why], pwhy[sizeof m->pv_failed_why], view[sizeof m->pv_view];
    snprintf(why, sizeof why, "%s", m->failed_why);
    snprintf(pwhy, sizeof pwhy, "%s", m->pv_failed_why);
    snprintf(view, sizeof view, "%s", m->pv_view);
    char* const vals[3] = {why, pwhy, view};
    for (int k = 0; k < 3; k++)
        for (char* c = vals[k]; *c; c++)
            if (*c == '"' || *c == '\\') *c = '\'';
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, path) != 0) return -1;
    fprintf(af.f, "format = %d\ndisc_sha256 = \"%s\"\nbuild = \"%s\"\n", m->format, m->disc, m->build);
    if (m->failed_build[0])
        fprintf(af.f, "failed_build = \"%s\"\nfailed_disc = \"%s\"\nfailed_why = \"%s\"\n", m->failed_build,
                m->failed_disc, why);
    if (m->pv_disc[0]) fprintf(af.f, "preview_disc = \"%s\"\npreview_view = \"%s\"\n", m->pv_disc, view);
    if (m->pv_failed_build[0])
        fprintf(af.f, "preview_failed_build = \"%s\"\npreview_failed_disc = \"%s\"\npreview_failed_why = \"%s\"\n",
                m->pv_failed_build, m->pv_failed_disc, pwhy);
    fprintf(af.f, "[files]\n");
    for (int i = 0; i < LC_FILES; i++)
        if (m->size[i] >= 0) fprintf(af.f, "\"%s\" = %lld\n", k_lc_files[i], m->size[i]);
    if (m->pv_size >= 0) fprintf(af.f, "\"%s\" = %lld\n", LC_PREVIEW, m->pv_size);
    return op_atomic_file_commit(&af);
}

/** @brief Returns a file's size.
 * @param path The file.
 * @return The size, or -1 when it does not exist. */
static long long lc_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/** @brief Decides whether a game's cache is current for a disc, writing the rerun trigger when it is not.
 * @param dir The game's cache directory.
 * @param m The manifest as read.
 * @param have Whether the manifest file opened.
 * @param disc The library disc's content hash.
 * @param build This build's id.
 * @param trigger Receives the reason a rerun is due.
 * @param cap Its capacity.
 * @return LAUNCHER_CACHE_READY when current, LAUNCHER_CACHE_SKIPPED for a
 *         failed marker of this build and disc, LC_RERUN when a rerun is due. */
static int lc_check(const char* dir, const lc_manifest* m, int have, const char* disc, const char* build,
                    char* trigger, size_t cap) {
    if (!have) { snprintf(trigger, cap, "no cache manifest"); return LC_RERUN; }
    if (m->format != LAUNCHER_CACHE_FORMAT) {
        snprintf(trigger, cap, m->format > LAUNCHER_CACHE_FORMAT
                                   ? "manifest format %d is newer than this build's %d: treated as missing"
                                   : "manifest format %d, this build writes %d",
                 m->format, LAUNCHER_CACHE_FORMAT);
        return LC_RERUN;
    }
    if (m->failed_build[0] && !strcmp(m->failed_build, build) && !strcmp(m->failed_disc, disc))
        return LAUNCHER_CACHE_SKIPPED;
    if (m->failed_build[0]) { snprintf(trigger, cap, "a failed marker of build %s", m->failed_build); return LC_RERUN; }
    if (strcmp(m->disc, disc)) { snprintf(trigger, cap, "the library disc's hash changed"); return LC_RERUN; }
    if (strcmp(m->build, build)) { snprintf(trigger, cap, "outputs written by build %s", m->build); return LC_RERUN; }
    for (int i = 0; i < LC_FILES; i++) {
        char p[4096];
        openpete_paths_join(p, sizeof p, dir, k_lc_files[i]);
        const long long n = lc_size(p);
        if (m->size[i] < 0 || n != m->size[i]) {
            snprintf(trigger, cap, "%s %s", k_lc_files[i], n < 0 ? "missing" : "differs from the manifest's size");
            return LC_RERUN;
        }
    }
    return LAUNCHER_CACHE_READY;
}

/** @brief Writes a little-endian u32.
 * @param f The stream.
 * @param v The value. */
static void lc_u32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

/** @brief Writes the domes file's body.
 * @param f The stream.
 * @param a The extraction result. */
static void lc_domes_body(FILE* f, const game_assets_t* a) {
    fwrite("OPDOMES1", 1, 8, f);
    lc_u32(f, (uint32_t)a->dome_count);
    for (int i = 0; i < a->dome_count; i++) {
        const game_asset_dome_t* d = &a->domes[i];
        const size_t n = strlen(d->level) & 0xFFu;
        fputc((int)n, f);
        fwrite(d->level, 1, n, f);
        lc_u32(f, d->nv);
        lc_u32(f, d->ntri);
        lc_u32(f, d->sectors);
        fwrite(d->bg, 1, 3, f);
        fwrite(d->verts, 1, (size_t)d->nv * 9u, f);
        for (uint32_t k = 0; k < d->ntri * 3u; k++) {
            const uint8_t b[2] = {(uint8_t)d->tris[k], (uint8_t)(d->tris[k] >> 8)};
            fwrite(b, 1, 2, f);
        }
    }
}

/** @brief Writes the logo file's body.
 * @param f The stream.
 * @param a The extraction result. */
static void lc_logo_body(FILE* f, const game_assets_t* a) {
    fwrite("OPLOGO01", 1, 8, f);
    lc_u32(f, a->logo_w);
    lc_u32(f, a->logo_h);
    fwrite(a->logo_rgba, 1, (size_t)a->logo_w * a->logo_h * 4u, f);
}

/** @brief Replaces one cache output atomically and records its size.
 * @param dir The game's cache directory.
 * @param i The output's index in k_lc_files.
 * @param a The extraction result.
 * @param m Receives the output's size.
 * @return 0, or -1 when the write failed. */
static int lc_put(const char* dir, int i, const game_assets_t* a, lc_manifest* m) {
    char p[4096];
    openpete_paths_join(p, sizeof p, dir, k_lc_files[i]);
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, p) != 0) return -1;
    if (i == 0) lc_domes_body(af.f, a);
    else if (i == 1) text_glyph_set_write(a->glyphs, af.f);
    else lc_logo_body(af.f, a);
    if (op_atomic_file_commit(&af) != 0) return -1;
    m->size[i] = lc_size(p);
    return 0;
}

/** @brief Reports whether the harness variable OPENPETE_ASSET_FAIL forces this game's extraction to fail.
 * @param game The game id.
 * @return 1 when it names the game or is 1. */
static int lc_forced_fail(const char* game) {
    const char* e = getenv("OPENPETE_ASSET_FAIL");
    return e && (!strcmp(e, "1") || !strcmp(e, game));
}

/** @brief Holds the job for OPENPETE_ASSET_JOB_DELAY_MS, the harness's slow job, in 10 ms steps.
 * @return 0, or -2 when the job was asked to stop meanwhile. */
static int lc_delay(void) {
    const char* e = getenv("OPENPETE_ASSET_JOB_DELAY_MS");
    const long ms = e ? atol(e) : 0;
    for (long t = 0; t < ms; t += 10) {
        if (openpete_launcher_job_cancelled()) return -2;
        op_sleep_until(op_now() + 10 * OP_NS_PER_MS);
    }
    return openpete_launcher_job_cancelled() ? -2 : 0;
}

/** @brief Runs the extraction and, on success, writes the outputs into the manifest's record.
 * @param b The installed game's build record.
 * @param dir The game's cache directory.
 * @param disc_path The disc.
 * @param disc The disc's content hash.
 * @param m The manifest; its outputs, disc and build are updated on success.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, -1 on a deterministic failure, -2 when stopped. */
static int lc_extract(const openpete_game_build_t* b, const char* dir, const char* disc_path, const char* disc,
                      lc_manifest* m, char* why, size_t cap) OP_REQUIRES(ingest) {
    if (lc_forced_fail(b->id)) { snprintf(why, cap, "forced (OPENPETE_ASSET_FAIL)"); return -1; }
    game_assets_t a;
    int rc = game_assets_extract(b, disc_path, disc, &a, why, cap);
    if (rc == 0) rc = lc_delay();
    for (int i = 0; rc == 0 && i < LC_FILES; i++)
        if (lc_put(dir, i, &a, m) != 0) {
            snprintf(why, cap, "writing %s failed", k_lc_files[i]);
            rc = -1;
        }
    game_assets_free(&a);
    if (rc != 0) return rc;
    snprintf(m->disc, sizeof m->disc, "%s", disc);
    snprintf(m->build, sizeof m->build, "%s", openpete_launcher_build_id());
    m->failed_build[0] = m->failed_disc[0] = m->failed_why[0] = 0;
    return 0;
}

/** @brief Logs the line a rerun starts with: the game, the job and the trigger.
 * @param game The game id.
 * @param context What started the job.
 * @param trigger The trigger. */
static void lc_log_rerun(const char* game, const char* context, const char* trigger) {
    if (!strcmp(context, "launch-time"))
        OPL_INFO_F("launcher", "launch-time rerun: asset extraction of %s (%s)", game, trigger);
    else
        OPL_INFO_F("launcher", "asset extraction of %s at %s (%s)", game, context, trigger);
}

/** @brief Records a deterministic failure in the manifest, keeping the previous outputs' record.
 * @param path The manifest's path.
 * @param m The manifest as read.
 * @param disc The disc's content hash.
 * @param why The reason. */
static void lc_mark_failed(const char* path, lc_manifest* m, const char* disc, const char* why) {
    if (m->format != LAUNCHER_CACHE_FORMAT) {   /* another format's record does not carry over */
        memset(m, 0, sizeof *m);
        for (int i = 0; i < LC_FILES; i++) m->size[i] = -1;
        m->pv_size = -1;
    }
    m->format = LAUNCHER_CACHE_FORMAT;
    snprintf(m->failed_build, sizeof m->failed_build, "%s", openpete_launcher_build_id());
    snprintf(m->failed_disc, sizeof m->failed_disc, "%s", disc);
    snprintf(m->failed_why, sizeof m->failed_why, "%.*s", (int)sizeof m->failed_why - 1, why);
    if (lc_write_manifest(path, m) != 0)
        OPL_WARN_F("launcher", "asset cache manifest %s: the failed marker could not be written", path);
}

/* A level's dome read back from the domes file, its arrays owned. */
typedef struct {
    launcher_preview_dome d;
    uint8_t*  verts;
    uint16_t* tris;
} lc_dome;

/** @brief Reads a little-endian u32 of a buffer.
 * @param p The bytes.
 * @return The value. */
static uint32_t lc_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief Reads a level's dome from the domes file; a missing file or level leaves an empty dome with a black clear.
 * @param dir The game's cache directory.
 * @param level The level id.
 * @param out Receives the dome; release with lc_dome_free. */
static void lc_dome_read(const char* dir, const char* level, lc_dome* out) {
    memset(out, 0, sizeof *out);
    char p[4096];
    openpete_paths_join(p, sizeof p, dir, k_lc_files[0]);
    FILE* f = fopen(p, "rb");
    if (!f) return;
    uint8_t h[12];
    uint32_t n = 0;
    if (fread(h, 1, 12, f) == 12 && memcmp(h, "OPDOMES1", 8) == 0) n = lc_rd32(h + 8);
    for (uint32_t i = 0; i < n; i++) {
        const int ln = fgetc(f);
        char name[256];
        uint8_t r[15];
        if (ln < 0 || fread(name, 1, (size_t)ln, f) != (size_t)ln || fread(r, 1, 15, f) != 15) break;
        name[ln] = 0;
        const uint32_t nv = lc_rd32(r), nt = lc_rd32(r + 4);
        if (strcmp(name, level) != 0) {
            if (fseek(f, (long)nv * 9L + (long)nt * 6L, SEEK_CUR) != 0) break;
            continue;
        }
        memcpy(out->d.bg, r + 12, 3);
        out->verts = (uint8_t*)malloc((size_t)nv * 9u + 1u);
        out->tris = (uint16_t*)malloc((size_t)nt * 6u + 2u);
        uint8_t* tb = (uint8_t*)malloc((size_t)nt * 6u + 1u);
        if (out->verts && out->tris && tb && fread(out->verts, 9, nv, f) == nv && fread(tb, 6, nt, f) == nt) {
            for (uint32_t k = 0; k < nt * 3u; k++) out->tris[k] = (uint16_t)(tb[2 * k] | (tb[2 * k + 1] << 8));
            out->d.nv = nv; out->d.ntri = nt;
            out->d.verts = out->verts; out->d.tris = out->tris;
        }
        free(tb);
        break;
    }
    fclose(f);
}

/** @brief Releases a dome lc_dome_read filled.
 * @param D The dome. */
static void lc_dome_free(lc_dome* D) {
    free(D->verts);
    free(D->tris);
    memset(D, 0, sizeof *D);
}

/** @brief Decides whether a game's preview is current for a disc and view, writing the rerun trigger when it is not.
 * @param dir The game's cache directory.
 * @param m The manifest as read.
 * @param disc The library disc's content hash.
 * @param view The view key, "level|pose|WxH form fovF ssaaN[ vis-game]" (lc_preview_update).
 * @param build This build's id.
 * @param trigger Receives the reason a rerun is due.
 * @param cap Its capacity.
 * @return LAUNCHER_CACHE_READY, LAUNCHER_CACHE_SKIPPED (a failed marker of
 *         this build and disc) or LC_RERUN. */
static int lc_preview_check(const char* dir, const lc_manifest* m, const char* disc, const char* view,
                            const char* build, char* trigger, size_t cap) {
    if (m->pv_failed_build[0] && !strcmp(m->pv_failed_build, build) && !strcmp(m->pv_failed_disc, disc))
        return LAUNCHER_CACHE_SKIPPED;
    if (m->pv_failed_build[0]) { snprintf(trigger, cap, "a preview failed marker of build %s", m->pv_failed_build); return LC_RERUN; }
    char p[4096];
    openpete_paths_join(p, sizeof p, dir, LC_PREVIEW);
    const long long n = lc_size(p);
    if (m->pv_size < 0 || n != m->pv_size) {
        snprintf(trigger, cap, "%s %s", LC_PREVIEW, n < 0 ? "missing" : "differs from the manifest's size");
        return LC_RERUN;
    }
    if (strcmp(m->pv_disc, disc)) { snprintf(trigger, cap, "the library disc's hash changed"); return LC_RERUN; }
    if (strcmp(m->pv_view, view)) { snprintf(trigger, cap, "the box picture's level, pose or form changed"); return LC_RERUN; }
    return LAUNCHER_CACHE_READY;
}

/** @brief Writes the preview file: "OPPREV01", u32 width, u32 height, then the RGBA rows.
 * @param path The file.
 * @param rgba The image.
 * @param w Its width.
 * @param h Its height.
 * @return 0, or -1. */
static int lc_preview_write(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, path) != 0) return -1;
    fwrite("OPPREV01", 1, 8, af.f);
    lc_u32(af.f, w);
    lc_u32(af.f, h);
    fwrite(rgba, 1, (size_t)w * h * 4u, af.f);
    return op_atomic_file_commit(&af);
}

/** @brief Records a preview's outcome in the manifest: the file and its keys on success, the failed marker on a deterministic failure.
 * @param game The game id (launcher_preview_size).
 * @param dir The game's cache directory.
 * @param path The manifest's path.
 * @param m The manifest.
 * @param disc The disc's content hash.
 * @param view The view key, "level|pose|WxH form fovF ssaaN[ vis-game]" (lc_preview_update).
 * @param rgba The image, or NULL after a failure.
 * @param why The failure's reason.
 * @return 0, or -1 when a write failed. */
static int lc_preview_record(const char* game, const char* dir, const char* path, lc_manifest* m, const char* disc,
                             const char* view, const uint8_t* rgba, const char* why) {
    if (!rgba) {
        snprintf(m->pv_failed_build, sizeof m->pv_failed_build, "%s", openpete_launcher_build_id());
        snprintf(m->pv_failed_disc, sizeof m->pv_failed_disc, "%s", disc);
        const size_t n = strnlen(why, sizeof m->pv_failed_why - 1);   /* the reason, cut to the field */
        memcpy(m->pv_failed_why, why, n);
        m->pv_failed_why[n] = 0;
        return lc_write_manifest(path, m);
    }
    char p[4096];
    openpete_paths_join(p, sizeof p, dir, LC_PREVIEW);
    uint32_t w = 0, h = 0;
    if (launcher_preview_size(game, &w, &h) != 0 || lc_preview_write(p, rgba, w, h) != 0) return -1;
    m->pv_size = lc_size(p);
    snprintf(m->pv_disc, sizeof m->pv_disc, "%s", disc);
    snprintf(m->pv_view, sizeof m->pv_view, "%s", view);
    m->pv_failed_build[0] = m->pv_failed_disc[0] = m->pv_failed_why[0] = 0;
    return lc_write_manifest(path, m);
}

/** @brief Brings a game's box picture up to date after its extraction: rerenders it when it is not current, on the launcher's device.
 *
 * Without the launcher's device (a headless run) nothing is checked or
 * logged: a preview is never due there. A deterministic failure writes the
 * preview's failed marker; a stop or a transient failure (the time bound,
 * the device) writes nothing, so the next launch reruns it.
 * @param b The installed game's build record.
 * @param dir The game's cache directory.
 * @param path The manifest's path.
 * @param disc_path The disc.
 * @param disc The disc's content hash.
 * @param context What started the job. */
static void lc_preview_update(const openpete_game_build_t* b, const char* dir, const char* path,
                              const char* disc_path, const char* disc, const char* context) OP_REQUIRES(ingest) {
    const launcher_preview_spec* S = launcher_preview_spec_of(b->id);
    if (S && !openpete_native_launcher_active() && getenv("OPENPETE_PREVIEW_BUILD_ONLY")) {
        char why[512];
        const int rc = launcher_preview_build_only(b, disc_path, why, sizeof why);
        OPL_INFO_F("launcher", "preview build of %s without a draw: %s", b->id, rc == 0 ? "done" : why);
        return;
    }
    if (!S || !openpete_native_launcher_active()) return;
    lc_manifest m;
    if (lc_read(path, &m) != 0 || m.format != LAUNCHER_CACHE_FORMAT) return;
    char view[256], trigger[256], why[512];
    uint32_t w = 0, h = 0;
    (void)launcher_preview_size(b->id, &w, &h);
    snprintf(view, sizeof view, "%s|%s|%ux%u %s fov%g ssaa%u%s", S->level, S->pose, w, h, LAUNCHER_PREVIEW_FORM,
             LAUNCHER_PREVIEW_FOV_DEG, LAUNCHER_PREVIEW_SSAA, S->vis_game ? " vis-game" : "");
    const char* build = openpete_launcher_build_id();
    const int st = lc_preview_check(dir, &m, disc, view, build, trigger, sizeof trigger);
    if (st == LAUNCHER_CACHE_READY) { OPL_INFO_F("launcher", "preview of %s is current", b->id); return; }
    if (st == LAUNCHER_CACHE_SKIPPED) {
        OPL_INFO_F("launcher", "preview of %s skipped: the cache manifest holds a preview failed marker of this "
                   "build (%s)", b->id, m.pv_failed_why);
        return;
    }
    if (!strcmp(context, "launch-time"))
        OPL_INFO_F("launcher", "launch-time rerun: preview render of %s (%s)", b->id, trigger);
    else
        OPL_INFO_F("launcher", "preview render of %s at %s (%s)", b->id, context, trigger);
    lc_dome dome;
    lc_dome_read(dir, S->level, &dome);
    uint8_t* rgba = NULL;
    double ms = 0.0;
    const int rc = launcher_preview_render(b, disc_path, &dome.d, &rgba, &ms, why, sizeof why);
    lc_dome_free(&dome);
    openpete_launcher_preview_progress(0, 0);
    if (rc == OPENPETE_PREVIEW_OK) {
        if (lc_preview_record(b->id, dir, path, &m, disc, view, rgba, NULL) == 0)
            OPL_INFO_F("launcher", "preview of %s written: %ux%u, %s at %s, %.1f ms", b->id, w, h, S->level, S->pose,
                       ms);
        else
            OPL_WARN_F("launcher", "preview of %s: the file or the manifest could not be written", b->id);
    } else if (rc == OPENPETE_PREVIEW_FAILED) {
        (void)lc_preview_record(b->id, dir, path, &m, disc, view, NULL, why);
        OPL_ERROR_F("launcher", "preview of %s failed: %s; failed marker written to %s (build %s)", b->id, why,
                    path, build);
    } else if (rc == OPENPETE_PREVIEW_STOPPED) {
        OPL_INFO_F("launcher", "preview of %s stopped; the cache is unchanged", b->id);
    } else {
        OPL_WARN_F("launcher", "preview of %s not drawn: %s; no marker, it reruns at the next launch", b->id, why);
    }
    if (rc == OPENPETE_PREVIEW_OK || rc == OPENPETE_PREVIEW_FAILED) openpete_launcher_picture_changed();
    free(rgba);
}

/** @brief Brings one game's extraction outputs up to date (the body of launcher_cache_update before its preview).
 * @param b The installed game's build record.
 * @param disc_path The disc.
 * @param disc_sha256 Its content hash.
 * @param context What started the job.
 * @param dir Receives the game's cache directory, empty when none resolves.
 * @param dir_cap Its capacity.
 * @return A LAUNCHER_CACHE_* value. */
static int lc_assets_update(const openpete_game_build_t* b, const char* disc_path, const char* disc_sha256,
                            const char* context, char* dir, size_t dir_cap) OP_REQUIRES(ingest) {
    char path[4096], trigger[256], why[512];
    dir[0] = 0;
    if (openpete_paths_game_state_dir("cache", b->id, dir, dir_cap, 1) != 0) {
        dir[0] = 0;
        OPL_WARN_F("launcher", "asset cache of %s: no cache directory resolves", b->id);
        return LAUNCHER_CACHE_STOPPED;   /* transient: no marker can be written */
    }
    openpete_paths_join(path, sizeof path, dir, "manifest.toml");
    const char* disc = disc_sha256 ? disc_sha256 : "";
    lc_manifest m;
    const int have = lc_read(path, &m) == 0;
    const int st = lc_check(dir, &m, have, disc, openpete_launcher_build_id(), trigger, sizeof trigger);
    if (st == LAUNCHER_CACHE_READY) {
        OPL_INFO_F("launcher", "asset cache of %s is current (%s)", b->id, dir);
        return st;
    }
    if (st == LAUNCHER_CACHE_SKIPPED) {
        OPL_INFO_F("launcher", "asset extraction of %s skipped: the cache manifest holds a failed marker of "
                   "this build (%s)", b->id, m.failed_why);
        return st;
    }
    lc_log_rerun(b->id, context, trigger);
    const op_time_t t0 = op_now();
    const int rc = lc_extract(b, dir, disc_path, disc, &m, why, sizeof why);
    if (rc == -2) {
        OPL_INFO_F("launcher", "asset extraction of %s stopped; the cache is unchanged", b->id);
        return LAUNCHER_CACHE_STOPPED;
    }
    if (rc != 0) {
        lc_mark_failed(path, &m, disc, why);
        OPL_ERROR_F("launcher", "asset extraction of %s failed: %s; failed marker written to %s (build %s)", b->id,
                    why, path, openpete_launcher_build_id());
        return LAUNCHER_CACHE_FAILED;
    }
    m.format = LAUNCHER_CACHE_FORMAT;
    if (lc_write_manifest(path, &m) != 0) {
        OPL_WARN_F("launcher", "asset cache of %s: the manifest could not be written", b->id);
        return LAUNCHER_CACHE_STOPPED;
    }
    OPL_INFO_F("launcher", "asset cache of %s written: %d files in %s, %.1f ms", b->id, LC_FILES, dir,
               (double)(op_now() - t0) / 1e6);
    return LAUNCHER_CACHE_READY;
}

/** @brief Reads an image file of the cache: an 8-byte magic, u32 width and height (each 1 to 1024), then the RGBA rows, and nothing after.
 * @param path The file.
 * @param magic Its magic.
 * @param w Receives the width.
 * @param h Receives the height.
 * @return The texels for the caller to free, or NULL when the file is missing or malformed. */
static unsigned char* lc_read_rgba(const char* path, const char* magic, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hd[16];
    unsigned char* px = NULL;
    if (fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, magic, 8)) {
        const uint32_t lw = hd[8] | (uint32_t)hd[9] << 8 | (uint32_t)hd[10] << 16 | (uint32_t)hd[11] << 24;
        const uint32_t lh = hd[12] | (uint32_t)hd[13] << 8 | (uint32_t)hd[14] << 16 | (uint32_t)hd[15] << 24;
        const size_t n = (size_t)lw * lh * 4u;
        if (lw && lh && lw <= 1024u && lh <= 1024u && (px = (unsigned char*)malloc(n)) != NULL &&
            (fread(px, 1, n, f) != n || fgetc(f) != EOF)) {
            free(px);
            px = NULL;
        }
        if (px) { *w = (int)lw; *h = (int)lh; }
    }
    fclose(f);
    return px;
}

unsigned char* launcher_cache_read_logo(const char* game, int* w, int* h) {
    char dir[4096], path[4096];
    if (!game || openpete_paths_game_state_dir("cache", game, dir, sizeof dir, 0) != 0) return NULL;
    openpete_paths_join(path, sizeof path, dir, k_lc_files[2]);
    return lc_read_rgba(path, "OPLOGO01", w, h);
}

unsigned char* launcher_cache_read_preview(const char* game, int* w, int* h) {
    char dir[4096], path[4096];
    if (!game || openpete_paths_game_state_dir("cache", game, dir, sizeof dir, 0) != 0) return NULL;
    openpete_paths_join(path, sizeof path, dir, "manifest.toml");
    lc_manifest m;
    if (lc_read(path, &m) != 0 || m.format != LAUNCHER_CACHE_FORMAT || m.pv_failed_build[0]) return NULL;
    openpete_paths_join(path, sizeof path, dir, LC_PREVIEW);
    if (m.pv_size < 0 || lc_size(path) != m.pv_size) return NULL;
    return lc_read_rgba(path, "OPPREV01", w, h);
}

int launcher_cache_update(const openpete_game_build_t* b, const char* disc_path, const char* disc_sha256,
                          const char* context) OP_REQUIRES(ingest) {
    char dir[4096];
    const int rc = lc_assets_update(b, disc_path, disc_sha256, context, dir, sizeof dir);
    if (rc != LAUNCHER_CACHE_STOPPED && dir[0] && !openpete_launcher_job_cancelled()) {
        char path[4096];
        openpete_paths_join(path, sizeof path, dir, "manifest.toml");
        lc_preview_update(b, dir, path, disc_path, disc_sha256 ? disc_sha256 : "", context);
    }
    return rc;
}

int launcher_cache_game_has_assets(const openpete_game_build_t* b) {
    return b && b->runtime && b->runtime->host && b->runtime->host->assets ? 1 : 0;
}

/* The glyph models the launcher's title draws, one set per linked game,
 * keyed by build record; read and drawn on the launcher's thread. */
#define LC_TITLE_GAMES 8
static struct {
    const openpete_game_build_t* b;
    text_glyph_set*              set;
} s_title[LC_TITLE_GAMES];

/** @brief Returns the title glyph-set slot of a game, claiming a free one on its first use.
 *
 * @param b The game's build record.
 * @return The slot index, or -1 when every slot holds another game. */
static int lc_title_slot(const openpete_game_build_t* b) {
    for (int i = 0; i < LC_TITLE_GAMES; i++)
        if (s_title[i].b == b) return i;
    for (int i = 0; i < LC_TITLE_GAMES; i++)
        if (!s_title[i].b) { s_title[i].b = b; return i; }
    return -1;
}

int launcher_cache_title_load(const openpete_game_build_t* b) {
    char dir[4096], path[4096];
    const int k = lc_title_slot(b);
    if (k < 0 || openpete_paths_game_state_dir("cache", b->id, dir, sizeof dir, 0) != 0) return -1;
    openpete_paths_join(path, sizeof path, dir, k_lc_files[1]);
    if (!s_title[k].set) s_title[k].set = text_glyph_set_new();
    text_glyph_set* G = s_title[k].set;
    FILE* f = G ? fopen(path, "r") : NULL;
    if (!f) {
        text_glyph_set_free(G);   /* NULL-safe: the set read before goes with its file */
        s_title[k].set = NULL;
        OPL_INFO_F("launcher", "title glyph models of %s: %s does not open", b->id, path);
        return -1;
    }
    const int rc = text_glyph_set_read(G, f);
    fclose(f);
    if (rc != 0) {
        text_glyph_set_free(G);
        s_title[k].set = NULL;
        OPL_WARN_F("launcher", "title glyph models of %s: %s does not read", b->id, path);
        return -1;
    }
    OPL_INFO_F("launcher", "title glyph models of %s read from %s", b->id, path);
    return 0;
}

int launcher_cache_title_emit(const openpete_game_build_t* b, const char* text, const openpete_launcher_title* t,
                              void* out, int cap, float* cap_h, openpete_launcher_title_glyph* glyphs,
                              int glyph_cap) {
    *cap_h = 0.0f;
    const int k = lc_title_slot(b);
    const game_layout_t* layout = b->runtime ? b->runtime->layout : NULL;
    if (k < 0 || !s_title[k].set || !layout) return 0;
    text_glyph_posed posed[16];
    memset(posed, 0, sizeof posed);
    const text_glyph_fit fit = { t->x0, t->cy, t->width, t->gap, t->tick };
    const int pcap = glyph_cap < 16 ? glyph_cap : 16;
    const int n = text_glyph_set_emit_title(s_title[k].set, &layout->font, text, &fit, (openpete_hud_vert*)out, cap,
                                            cap_h, glyphs ? posed : NULL, pcap);
    for (int i = 0; glyphs && i < pcap; i++) {
        glyphs[i].x0 = posed[i].x0; glyphs[i].y0 = posed[i].y0;
        glyphs[i].x1 = posed[i].x1; glyphs[i].y1 = posed[i].y1;
        glyphs[i].face = posed[i].face;
        glyphs[i].rgba = posed[i].rgba;
    }
    return n;
}
