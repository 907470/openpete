/* knob_registry_test — the M-overlay knob table's non-obvious mechanics:
 * module-contributed rows (openpete_knob_add) landing inside their section,
 * capacity refusal, and KNOB_ACTION rows (a button, no value) surviving boot
 * stamping. The overlay itself needs a window, so these are the parts that
 * would otherwise only be testable by eye.
 *
 * knob_registry.c is compiled straight in, with the host/renderer setters it
 * calls stubbed below — linking openpete_core would drag SDL into a console
 * test (same reason as psx_region_test). */

#include "openpete.h"
#include "openpete_knobs.h"
#include "../../core/config/config_schema.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs for the live-value channels knob_registry.c writes through ---- */

int   openpete_no_lod    = 0;
float openpete_fog_scale = 1.0f;

int    openpete_fog_enabled(void)              { return 1; }
void   openpete_fog_scale_set(float v)         { (void)v; }
void   openpete_fog_set(int on)                { (void)on; }
void   openpete_no_lod_set(int on)             { (void)on; }
int    openpete_freecam_active(void)           { return 0; }
void   openpete_freecam_toggle(void)           { }
int    openpete_camlock_active(void)           { return 0; }
void   openpete_camlock_toggle(void)           { }
int    openpete_fillpick_active(void)          { return 0; }
void   openpete_fillpick_set_active(int on)    { (void)on; }
int    openpete_fillpick_preview(void)         { return 0; }
void   openpete_fillpick_set_preview(int on)   { (void)on; }
int    openpete_get_mute(void)                 { return 0; }
void   openpete_set_mute(int on)               { (void)on; }
static int s_mouse_enabled = 1;
int    openpete_host_mouse_enabled(void)       { return s_mouse_enabled; }
void   openpete_host_mouse_set_enabled(int on) { s_mouse_enabled = on ? 1 : 0; }
static int s_record_supported = 1;
int    openpete_record_supported(void)         { return s_record_supported; }
int    openpete_get_sfx_volume(void)           { return -1; }
void   openpete_set_sfx_volume(int v)          { (void)v; }
int    openpete_get_music_volume(void)         { return -1; }
void   openpete_set_music_volume(int v)        { (void)v; }
int    openpete_get_vibration(void)            { return -1; }
void   openpete_set_vibration(int v)           { (void)v; }
float  openpete_freecam_mouse_sensitivity(void)     { return 1.0f; }
void   openpete_freecam_set_mouse_sensitivity(float s) { (void)s; }
int    openpete_get_render_fps(void)           { return 60; }
void   openpete_set_render_fps(int fps)        { (void)fps; }
double openpete_get_tick_scale(void)           { return 1.0; }
void   openpete_set_tick_scale(double s)       { (void)s; }
int    openpete_runahead_get(void)             { return 0; }
void   openpete_runahead_set(int n)            { (void)n; }
void   openpete_rewind_configure(int w)        { (void)w; }
int    openpete_native_fullscreen_active(void) { return 0; }
void   openpete_native_toggle_fullscreen(void) { }
void   openpete_native_request_fullscreen_toggle(void) { }

/* The renderer's live display state. Display rows read it back rather than
 * shadowing their value, because the O menu drives these setters directly
 * (never the rows'), so the stub keeps real state for those rows to find. */
static bool  s_native_up     = false;
static float s_native_aspect = 0.0f;   /* AUTO */
static int   s_native_ssaa   = 1;
static int   s_native_vsync  = -1;     /* unset */
static int   s_native_showfps = 1;
static int   s_native_vsnap  = 0;
static int   s_native_rquant = 0;
static int   s_native_dither = 0;
static int   s_native_texmap = 0;

bool   openpete_native_render_enabled(void)    { return s_native_up; }
void   openpete_native_set_aspect(float a)     { s_native_aspect = (a > 0.0f) ? a : 0.0f; }
float  openpete_native_get_aspect(void)        { return s_native_aspect; }
void   openpete_native_set_ssaa(int n)         { s_native_ssaa = n; }
int    openpete_native_get_ssaa(void)          { return s_native_ssaa; }
void   openpete_native_set_vsync(int on)       { s_native_vsync = on ? 1 : 0; }
int    openpete_native_get_vsync(void)         { return s_native_vsync; }
void   openpete_native_set_perf_stats(int level) { s_native_showfps = level; }
static int s_native_perf_anchor = 0;
void   openpete_native_set_perf_anchor(int a) { s_native_perf_anchor = a; }
int    openpete_native_perf_anchor(void) { return s_native_perf_anchor; }
static int      s_osd_on = 1, s_osd_anchor = 0;
static uint32_t s_osd_timeout_ms = 3000;
void     openpete_osd_set_enabled(int on) { s_osd_on = on ? 1 : 0; }
int      openpete_osd_enabled(void) { return s_osd_on; }
void     openpete_osd_set_anchor(int a) { s_osd_anchor = a; }
int      openpete_osd_anchor(void) { return s_osd_anchor; }
void     openpete_osd_set_default_timeout_ms(uint32_t ms) { s_osd_timeout_ms = ms; }
uint32_t openpete_osd_default_timeout_ms(void) { return s_osd_timeout_ms; }
int    openpete_native_perf_stats_level(void)    { return s_native_showfps; }
void   openpete_native_set_fps_overlay(int on)   { s_native_showfps = on ? OPENPETE_PERF_FPS
                                                                         : OPENPETE_PERF_OFF; }
int    openpete_native_fps_overlay_enabled(void) { return s_native_showfps != OPENPETE_PERF_OFF; }
void   openpete_native_set_fps_in_record(int on) { (void)on; }
void   openpete_native_set_vertex_precision(int m) { s_native_vsnap = m ? 1 : 0; }
int    openpete_native_vertex_snap_active(void)    { return s_native_vsnap; }
void   openpete_native_set_raster_quantize(int n)  { s_native_rquant = n; }
int    openpete_native_get_raster_quantize(void)   { return s_native_rquant; }
int    openpete_workpool_threads(void)             { return 1; }
int    openpete_tercache_ready(void)               { return 0; }
int    openpete_disc_cache_active(void)            { return 0; }
int    openpete_cd_load_delay = 0;
int    openpete_native_get_dither(void)            { return s_native_dither; }
void   openpete_native_set_dither(int on)          { s_native_dither = on ? 1 : 0; }
static int s_native_tervis;
void   openpete_native_set_terrain_visibility(int mode) { s_native_tervis = mode; }
int    openpete_native_get_terrain_visibility(void)     { return s_native_tervis; }
static float s_native_fov;
void   openpete_native_set_fov(float deg)      { s_native_fov = deg; }
float  openpete_native_fov_deg(void)           { return s_native_fov; }
static int s_native_version_osd = 1;
void   openpete_native_set_version_osd(int on) { s_native_version_osd = on ? 1 : 0; }
int    openpete_native_version_osd(void)       { return s_native_version_osd; }
int    openpete_native_get_texture_mapping(void)   { return s_native_texmap; }
void   openpete_native_set_texture_mapping(int m)  { s_native_texmap = m ? 1 : 0; }
int    openpete_native_get_notex(void)             { return 0; }
void   openpete_native_set_notex(int on)           { (void)on; }
int    openpete_log_get_format(void)           { return 0; }
void   openpete_log_set_format(int f)          { (void)f; }
const char *openpete_log_get_spec(void)        { return ""; }
const char *openpete_log_file_path(void)       { return ""; }
void   openpete_log_set_spec(const char *s)    { (void)s; }
void   openpete_logf(int lvl, const char *sub, const char *fmt, ...) {
    (void)lvl; (void)sub; (void)fmt;
}

/* ---- the action row under test ---- */

static int    s_fired  = 0;
static double s_ready  = 1.0;

static double act_ready(const knob_desc_t *k) { (void)k; return s_ready; }
static int    act_fire(knob_desc_t *k, double v) { (void)k; (void)v; s_fired++; return 0; }
static void   act_status(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k; snprintf(buf, cap, s_fired ? "applied" : "");
}

static int fail(const char *msg) { printf("FAIL: %s\n", msg); return 1; }

static const knob_desc_t *find_row(const char *id, size_t *idx_out) {
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(ks[i].id, id)) { if (idx_out) *idx_out = i; return &ks[i]; }
    return NULL;
}

/* Every section must occupy ONE contiguous run — the overlay emits a header
 * per run, so a split section would render its name twice. */
static int sections_contiguous(void) {
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(ks[i].section, ks[j].section) == 0 &&
                strcmp(ks[j - 1].section, ks[j].section) != 0)
                return 0;
        }
    }
    return 1;
}

int main(void) {
    int rc = 0;

    size_t before = 0;
    (void)knob_registry(&before);

    /* A contributed row joins its named section rather than landing at the
     * end of the table. */
    knob_desc_t row = {0};
    row.id           = "test_action";
    row.label        = "Test action";
    row.section      = "Gameplay";
    row.help         = "unit test";
    row.type         = KNOB_ACTION;
    row.mut          = KNOB_LIVE;
    row.get_num      = act_ready;
    row.set_num      = act_fire;
    row.get_str      = act_status;
    row.action_label = "Apply";
    if (openpete_knob_add(&row) != 0) rc |= fail("openpete_knob_add rejected a row");

    size_t after = 0, idx = 0;
    (void)knob_registry(&after);
    if (after != before + 1) rc |= fail("row count did not grow by one");

    const knob_desc_t *k = find_row("test_action", &idx);
    if (!k) {
        rc |= fail("contributed row missing from the registry");
    } else {
        size_t n;
        const knob_desc_t *ks = knob_registry(&n);
        if (strcmp(ks[idx - 1].section, "Gameplay") != 0)
            rc |= fail("row did not land inside its section");
        if (idx + 1 < n && strcmp(ks[idx + 1].section, "Gameplay") == 0)
            rc |= fail("row landed before the section's last built-in");
    }
    if (!sections_contiguous()) rc |= fail("insertion split a section run");

    /* Boot stamping must leave an action row alone: it has no value, and a
     * stamped boot_num would make the overlay render it as "edited". */
    openpete_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.vsync = -1;
    opts.render_fps = -1;
    opts.tick_scale = 1.0;
    knob_registry_init(&opts);
    k = find_row("test_action", NULL);
    if (!k) return fail("row vanished across knob_registry_init");
    if (k->boot_num != 0.0) rc |= fail("action row got a boot value stamped");

    /* The value column of an action row shows its status, not a number. */
    char buf[KNOB_STR_MAX];
    knob_value_str(k, buf, sizeof buf);
    if (strcmp(buf, "") != 0) rc |= fail("un-fired action should report no status");
    if (k->set_num((knob_desc_t *)k, 1.0) != 0) rc |= fail("action setter refused");
    if (s_fired != 1) rc |= fail("action did not fire");
    knob_value_str(k, buf, sizeof buf);
    if (strcmp(buf, "applied") != 0) rc |= fail("status string not reported");

    /* Rows the run has no use for report themselves hidden, so the overlay
     * drops them — and, since every Recording row is one of them, drops the
     * section header with them. The predicate reads two things: whether the
     * build can encode at all, and whether this run was launched recording. */
    {
        static const char *const rec_rows[] = {
            "record", "record_cfr", "record_frames", "record_fps_overlay", NULL
        };
        openpete_options_t ropts;
        memset(&ropts, 0, sizeof ropts);
        ropts.vsync = -1; ropts.render_fps = -1; ropts.tick_scale = 1.0;

        for (int pass = 0; pass < 3; pass++) {
            /* pass 0: no --record. 1: recording. 2: recording, no encoder. */
            ropts.record_path  = pass ? "clip.mkv" : NULL;
            s_record_supported = (pass != 2);
            knob_registry_init(&ropts);
            for (int r = 0; rec_rows[r]; r++) {
                const knob_desc_t *rk = find_row(rec_rows[r], NULL);
                if (!rk || !rk->visible) {
                    rc |= fail("a Recording row carries no visibility rule");
                    continue;
                }
                if (!!rk->visible(rk) != (pass == 1)) {
                    rc |= fail("Recording row visibility ignored the run");
                    fprintf(stderr, "       %s, pass %d\n", rec_rows[r], pass);
                }
            }
        }
        s_record_supported = 1;

        /* bk2 replay drives the pad, so it is not one of them: it stays
         * listed whether or not the run records. */
        const knob_desc_t *bk2 = find_row("bk2", NULL);
        if (!bk2 || bk2->visible) rc |= fail("bk2 row hides with the recording rows");
        if (bk2 && strcmp(bk2->section, "Gameplay") != 0)
            rc |= fail("bk2 row left the Gameplay section");
    }

    /* A pick made in the O menu must register as an edit. That menu keeps its
     * own row state and calls the renderer's setters directly, so a row that
     * reported a registry-side shadow instead of the renderer's live value
     * would stay at its boot value forever — and "save edited settings" skips
     * unedited rows, so the pick would silently never persist. */
    {
        static const struct { const char *id; const char *want; } picks[] = {
            { "ssaa",             "4"     },
            { "aspect",           "16:9"  },
            { "vsync",            "off"   },
            { "perf_stats",       "off"   },
            { "vertex_precision", "integer" },
            { "raster_quantize",  "1"     },
        };
        openpete_options_t mopts;
        memset(&mopts, 0, sizeof mopts);
        mopts.vsync = -1; mopts.render_fps = -1; mopts.tick_scale = 1.0;
        mopts.ssaa = 1; mopts.perf_stats = OPENPETE_PERF_FPS; mopts.raster_quantize = -1;
        knob_registry_init(&mopts);
        s_native_up = true;   /* the renderer comes up after the boot stamp */

        /* the setter calls the O menu makes on its own rows */
        openpete_native_set_ssaa(4);
        openpete_native_set_aspect(16.0f / 9.0f);
        openpete_native_set_vsync(0);
        openpete_native_set_perf_stats(OPENPETE_PERF_OFF);
        openpete_native_set_vertex_precision(1);
        openpete_native_set_raster_quantize(1);

        for (size_t i = 0; i < sizeof picks / sizeof picks[0]; i++) {
            const knob_desc_t *pk = find_row(picks[i].id, NULL);
            if (!pk) { rc |= fail("display row missing from the registry"); continue; }
            if (pk->get_num(pk) == pk->boot_num) {
                rc |= fail("an O-menu pick did not register as an edit");
                fprintf(stderr, "       %s still reads its boot value\n", picks[i].id);
                continue;
            }
            char got[KNOB_STR_MAX];
            if (knob_config_value_str(pk, got, sizeof got) != 0) {
                rc |= fail("an O-menu pick has no persistable value");
                fprintf(stderr, "       %s\n", picks[i].id);
            } else if (strcmp(got, picks[i].want) != 0) {
                rc |= fail("an O-menu pick persists the wrong value");
                fprintf(stderr, "       %s: %s, want %s\n", picks[i].id, got, picks[i].want);
            }
        }

        /* The SSAA ladder is a factor TABLE, not a 1<<index mapping: an odd
         * rung has to survive the round trip to the flag's vocabulary. */
        {
            openpete_native_set_ssaa(3);
            const knob_desc_t *sk = find_row("ssaa", NULL);
            char got[KNOB_STR_MAX];
            if (!sk || knob_config_value_str(sk, got, sizeof got) != 0 ||
                strcmp(got, "3") != 0)
                rc |= fail("3x SSAA did not round-trip to --ssaa 3");
            openpete_native_set_ssaa(4);
        }

        /* Before the renderer is up (and in headless / PsyCross-only runs, where
         * it never comes up) the rows fall back to their stamped boot value
         * instead of reading a renderer that has no state yet. */
        s_native_up = false;
        for (size_t i = 0; i < sizeof picks / sizeof picks[0]; i++) {
            const knob_desc_t *pk = find_row(picks[i].id, NULL);
            if (pk && pk->get_num(pk) != pk->boot_num) {
                rc |= fail("a display row read the renderer before it was up");
                fprintf(stderr, "       %s\n", picks[i].id);
            }
        }
    }

    /* Capacity is finite and refuses politely rather than corrupting. */
    int added = 0;
    for (int i = 0; i < 64; i++) {
        knob_desc_t filler = {0};
        filler.id      = "test_filler";
        filler.label   = "filler";
        filler.section = "ZZZ test";
        filler.help    = "unit test";
        filler.type    = KNOB_ACTION;
        filler.mut     = KNOB_LIVE;
        if (openpete_knob_add(&filler) != 0) break;
        added++;
    }
    if (added >= 64) rc |= fail("capacity never refused an add");
    if (!find_row("test_action", NULL)) rc |= fail("overflow clobbered a row");

    /* The registry and the config schema are two tables describing the same
     * knobs, and they have to agree or a flag silently loses its
     * openpete.toml bare key — and, when it also has an env twin, loses the
     * flattening that keeps the two spellings from fighting.
     *
     * Checked in the direction that catches the mistake people actually
     * make: adding a CLI flag with a registry row and forgetting the schema.
     * The reverse (a schema row with no registry row) is checked too, so the
     * schema can't grow knobs the M overlay never lists.
     *
     * ENVROW_* rows are exempt: they are the ~150 debug-only OPENPETE_* vars,
     * which have no flag to conflict with and no bare key by design. Their
     * ids are the SHOUTING var name minus the prefix, which is what tells
     * them apart from the hand-written env-only rows. */
    {
        size_t n;
        const knob_desc_t *ks = knob_registry(&n);
        for (size_t i = 0; i < n; i++) {
            if (!ks[i].cli) continue;
            int si = cfg_schema_find_cli(ks[i].cli);
            if (si < 0) {
                rc |= fail("knob has no config schema row");
                fprintf(stderr, "       %s (%s) — add it to config_schema.c\n",
                        ks[i].id, ks[i].cli);
            } else if (strcmp(cfg_schema(NULL)[si].id, ks[i].id) != 0) {
                rc |= fail("schema/registry id mismatch for a flag");
                fprintf(stderr, "       %s vs %s for %s\n",
                        cfg_schema(NULL)[si].id, ks[i].id, ks[i].cli);
            }
        }
        size_t sn;
        const cfg_knob_t *sch = cfg_schema(&sn);
        for (size_t i = 0; i < sn; i++) {
            if (!find_row(sch[i].id, NULL)) {
                rc |= fail("config schema names a knob the registry lacks");
                fprintf(stderr, "       %s — add its row to knob_registry.c\n",
                        sch[i].id);
            }
        }
    }

    printf(rc ? "knob_registry_test: FAILED\n" : "knob_registry_test: ok\n");
    return rc;
}
