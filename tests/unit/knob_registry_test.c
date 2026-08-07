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
int    openpete_get_mute(void)                 { return 0; }
void   openpete_set_mute(int on)               { (void)on; }
int    openpete_get_render_fps(void)           { return 60; }
void   openpete_set_render_fps(int fps)        { (void)fps; }
double openpete_get_tick_scale(void)           { return 1.0; }
void   openpete_set_tick_scale(double s)       { (void)s; }
int    openpete_runahead_get(void)             { return 0; }
void   openpete_runahead_set(int n)            { (void)n; }
void   openpete_rewind_configure(int w)        { (void)w; }
int    openpete_native_fullscreen_active(void) { return 0; }
void   openpete_native_toggle_fullscreen(void) { }
void   openpete_native_set_aspect(float a)     { (void)a; }
void   openpete_native_set_ssaa(int n)         { (void)n; }
void   openpete_native_set_vsync(int on)       { (void)on; }
void   openpete_native_set_fps_overlay(int on) { (void)on; }
void   openpete_native_set_fps_in_record(int on) { (void)on; }
void   openpete_native_set_vertex_precision(int m) { (void)m; }
void   openpete_native_set_raster_quantize(int n)  { (void)n; }
int    openpete_native_get_dither(void)            { return 0; }
void   openpete_native_set_dither(int on)          { (void)on; }
void   widescreen_set_fov(float deg)           { (void)deg; }
int    openpete_native_get_texture_mapping(void)   { return 0; }
void   openpete_native_set_texture_mapping(int m)  { (void)m; }
int    openpete_log_get_format(void)           { return 0; }
void   openpete_log_set_format(int f)          { (void)f; }
const char *openpete_log_get_spec(void)        { return ""; }
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

    printf(rc ? "knob_registry_test: FAILED\n" : "knob_registry_test: ok\n");
    return rc;
}
