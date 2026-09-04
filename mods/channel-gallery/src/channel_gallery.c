/**
 * @file channel_gallery.c
 * @brief channel-gallery: one material selector per render channel,
 *        toggled live, so a mod author can see what each selector
 *        claims before writing a mod against it.
 *
 * One highlight shader is registered once per row in g_rows, each with a
 * different selector. Toggling a row in the overlay enables that row's
 * material, and the geometry it selects lights up in the row's colour.
 * The shader does nothing interesting on purpose: the selector is the
 * variable under observation, and a plain tint is the reading that
 * makes it visible.
 *
 * Rows are claims about the engine. A row that lights is selectable; a
 * row that stays dark is not yet. Rows that cannot light stay in the
 * table labelled as known gaps, because a gallery that only lists what
 * works cannot show an author where the boundary is. The engine's
 * channel gate drives rows by index through the gallery.rows config
 * key and asserts both directions, a lit row paints and a known-gap row
 * paints nothing, so a new row is appended, never inserted: an inserted
 * row renumbers every row after it.
 *
 * This is the C side of the material surface. declarative-reshade is
 * the same selectors expressed as [[material]] manifest rows with no
 * source file. Every selector key one can express, the other can.
 *
 * Reading the result:
 *
 * - No row is a known gap in this build: every routed channel is
 *   reshadeable, moby shadows included. The label stays because the
 *   next new channel will need it.
 * - A row marked unverified is wired but has never been observed to
 *   paint, because no capture yet puts its geometry on screen.
 * - An unmarked row that lights nothing has no geometry on screen at
 *   the moment: breathe fire for the flame rows, find a gem for the
 *   sparkle rows, rescue a dragon for the star.
 * - Two rows claiming the same pixels means the selectors overlap. The
 *   later registration wins; that is the documented priority.
 * - Blended rows tint rather than replace. The material inherits its
 *   group's blend equation and can only change colour math, so an
 *   additive draw forced to a flat colour saturates instead.
 */
#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>
#include <openpete_game_structs.h>   /* typed Moby view, for the refine row */
#include <string.h>
#include <stdio.h>    /* snprintf: class picker labels */
#include <stdlib.h>   /* strtol: gallery.rows parsing */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/**
 * @brief Particle identities for the ident key.
 *
 * ident is the channel-local identity the engine attaches to each draw:
 * flame ribbon or caps, the particle (kind << 8 | sel) pair, shadow
 * caster kind. A channel whose feeder has no identity to give emits 0.
 */
enum { PART_QUAD = 0, PART_LINE = 1, PART_GLOW = 2,
       PART_FLARE = 3, PART_SPARK = 4, PART_STAR = 5 };
#define PART(kind) (((kind) << 8) | 0)

/** @brief One gallery row: a selector, a colour, and its live state. */
typedef struct {
    const char* label;
    const char* help;         /**< Tooltip; states what the row proves. */
    int   channel;            /**< OP_CHAN_*; -1 = any */
    int   ident;              /**< -1 = any */
    int   moby_class;         /**< -1 = any */
    float rgb[3];
    int   on;                 /**< checkbox state */
    int   mat;                /**< material handle, -1 until registered */
} row_t;

/**
 * @brief The row table. Order is part of the gate's contract; append
 *        only. The last two rows are the refine rows and are located
 *        from the end at entry.
 */
static row_t g_rows[] = {
  { "Everything",      "No key at all - every channel this build can route.",
    -1, -1, -1, {1.00f, 0.20f, 0.80f}, 0, -1 },
  { "Terrain",         "The whole world mesh, both LOD tiers.",
    OP_CHAN_TERRAIN, -1, -1, {0.20f, 0.90f, 0.40f}, 0, -1 },
  { "Mobys (all)",     "Every pooled object: gems, chests, critters, decor.",
    OP_CHAN_MOBY, -1, -1, {1.00f, 0.75f, 0.10f}, 0, -1 },
  { "Mobys of one class", "Pick the class from the list below - it holds "
                          "exactly the classes this level loaded.",
    OP_CHAN_MOBY, -1, 0, {1.00f, 0.35f, 0.10f}, 0, -1 },
  { "Player",          "Spyro. He is not a moby - the moby rows never hit him.",
    OP_CHAN_PLAYER, -1, -1, {0.30f, 0.60f, 1.00f}, 0, -1 },
  { "Sky",             "The cyclorama dome behind everything.",
    OP_CHAN_SKY, -1, -1, {0.70f, 0.40f, 1.00f}, 0, -1 },
  { "Flame (ribbon)",  "The additive plume. The tint reads yellow rather than "
                       "orange because the material inherits the group's "
                       "additive blend - orange added over green grass.",
    OP_CHAN_FLAME, 0, -1, {1.00f, 0.30f, 0.00f}, 0, -1 },
  { "Flame (caps)",    "The opaque base caps of the same effect.",
    OP_CHAN_FLAME, 1, -1, {0.10f, 1.00f, 1.00f}, 0, -1 },
  /* Moby shadows are tagged and blend-plumbed like every other channel,
   * but their draw path's fragment stage owns per-pixel occlusion
   * against walls and steep faces, and the fragment stage is the one a
   * material replaces. Reshading them needed a second shading contract,
   * the decal profile, which hands the shader the engine's occlusion
   * verdict to apply. The row is refused on the non-decal fallback pipe
   * (OPENPETE_MOBY_SHADOW_DECAL=0), so a mod's reach never depends on
   * whether the host can sample depth. */
  { "Shadows (moby)",  "Drawn by the depth-aware decal pipe, whose fragment "
                       "stage owns per-pixel occlusion - so this row uses the "
                       "decal material profile, which hands the shader that "
                       "verdict to apply instead of letting it delete the "
                       "logic. Subtractive: a brighter tint darkens more.",
    OP_CHAN_SHADOW, 0, -1, {0.90f, 0.90f, 0.20f}, 0, -1 },
  /* Known gap. Spyro's silhouette is drawn by a third fragment stage, an
   * untextured depth-aware decal whose vertex interface differs from both
   * the scene contract and the textured decal profile. It has no engine
   * half yet, so the tag reaches it and the override does not; lighting
   * the row leaves the shadow as the game drew it. */
  { "Shadows (player)","Spyro's own silhouette. Known gap: routable but not "
                       "yet reshadeable - it is drawn by an untextured "
                       "depth-aware decal stage that has no material profile "
                       "of its own, so this row paints nothing.",
    OP_CHAN_SHADOW, 1, -1, {0.20f, 1.00f, 0.60f}, 0, -1 },
  { "Particle billboards", "The textured sparkle quads. Toggle it against the "
                           "glow row to see the ident key split one channel: "
                           "the quads recolour, the glow ring stays stock.",
    OP_CHAN_PARTICLE, PART(PART_QUAD), -1, {1.00f, 0.50f, 0.50f}, 0, -1 },
  { "Particle glows",  "Glow rings around collectables.",
    OP_CHAN_PARTICLE, PART(PART_GLOW), -1, {0.50f, 1.00f, 0.50f}, 0, -1 },
  { "Particle sparkles","Gem sparkle flares.",
    OP_CHAN_PARTICLE, PART(PART_FLARE), -1, {1.00f, 1.00f, 0.40f}, 0, -1 },
  { "Dragon star",     "The rescue star - only during a dragon cutscene.",
    OP_CHAN_PARTICLE, PART(PART_STAR), -1, {0.40f, 0.90f, 1.00f}, 0, -1 },
  { "Tracers",         "Beam streaks (Gnasty's zaps).",
    OP_CHAN_TRACER, -1, -1, {1.00f, 0.10f, 0.10f}, 0, -1 },
  /* A portal shows the destination level's sky through a different
   * feeder from the cyclorama, so the Sky row never reaches it. It is
   * drawn three ways across the approach (far colour chip, fogged dome,
   * close gouraud dome); a material wants all three, or the portal pops
   * as the player walks up to it. */
  { "Portal preview (all)",
    "The destination sky seen through a portal - another level's sky, which "
    "is why the Sky row never reaches it. All three approach rungs at once.",
    OP_CHAN_PORTAL, -1, -1, {0.60f, 0.20f, 1.00f}, 0, -1 },
  { "Portal preview (close dome)",
    "Only the full gouraud dome seen up close. Toggle this against the far "
    "chip row to watch one rung of the ladder change and the other stay.",
    OP_CHAN_PORTAL, OP_PORTAL_DOME, -1, {0.30f, 0.10f, 0.90f}, 0, -1 },
  { "Portal preview (far chip)",
    "Only the flat colour chip a distant portal shows. Lights nothing when "
    "every portal in view is already close enough to draw its dome.",
    OP_CHAN_PORTAL, OP_PORTAL_FLAT, -1, {1.00f, 0.60f, 0.20f}, 0, -1 },
  { "Portal preview (fogged dome)",
    "Only the medium-distance dome, DPCS-fogged toward the level's fog "
    "colour - the cross-dissolve rung between the chip and the close dome.",
    OP_CHAN_PORTAL, OP_PORTAL_BLENDED, -1, {0.20f, 0.80f, 0.90f}, 0, -1 },
  /* The moby refine row. Registered separately below, because a selector
   * alone cannot express "and in this state". */
  { "Moby class + state (refine)",
    "The selector prunes to a moby class, then a callback reads each "
    "instance's m_State and declines the ones that do not match. Set the "
    "state slider to -1 to claim every instance of the class, which is what "
    "the plain class row already does - the point is what happens when you "
    "narrow it.",
    OP_CHAN_MOBY, -1, 0, {0.10f, 1.00f, 0.35f}, 0, -1 },
  /* The terrain refine row. Terrain is the only channel the engine draws
   * two ways: walked near the camera, and redrawn from GPU buffers baked
   * at level load everywhere else. A callback cannot run per baked face,
   * so the engine runs it once per present for the selector's single key
   * and hands the answer to both paths. Keyed on the untextured band so
   * the row is level-independent. */
  { "Terrain untextured band (refine)",
    "A refine callback on terrain. Terrain is drawn two ways - walked near "
    "the camera, redrawn from baked GPU buffers everywhere else - and this "
    "row reaches both with the same colour. What a face looks like is a "
    "function of the face, never of where it lives.",
    OP_CHAN_TERRAIN, -1, -1, {1.00f, 0.45f, 0.10f}, 0, -1 },
};
#define NROWS ((int)(sizeof g_rows / sizeof g_rows[0]))

static int   g_shader = -1;        /**< highlight.frag handle. */
/**
 * @brief highlight_decal.frag handle, for rows on a draw path whose
 *        stock fragment stage owns occlusion.
 *
 * The decal profile is a different include, descriptor sets and push
 * block. The row table names which shader a row wants and nothing else
 * about the row changes: the profile is a shading contract, not a
 * different kind of selector.
 */
static int   g_shader_decal = -1;
static int   g_decal_row = 8;      /**< Shadows (moby) */
static float g_strength = 0.65f;   /**< Highlight strength, 0 = stock look. */
static int   g_class = 0;          /**< Class the class-keyed rows select. */
static int   g_class_row = 3;      /**< Mobys of one class */

/**
 * @name Class picker
 * Lists only the classes the current level loaded. A slider over the
 * full 0..767 range was almost all dead travel: a level loads a few
 * dozen classes, and every other value selects nothing anywhere.
 * @{
 */
#define CLASSES_MAX 512
static uint16_t    g_classes[CLASSES_MAX];
static char        g_class_lbl[CLASSES_MAX][8];
static const char* g_class_items[CLASSES_MAX];
static int         g_class_n;
static int         g_class_idx;    /**< Index into g_classes, not a class id. */
/** @} */

/**
 * @brief Refresh the class picker from the engine's loaded-class list.
 *
 * Relabelled unconditionally: two levels can load the same number of
 * classes with different ids, so a count comparison would leave the
 * picker showing the previous level's numbers.
 *
 * The picker tracks the selected class, not the index. The set changes
 * on every level load and the same class sits at a different offset. A
 * class this level does not load keeps its value and is called out in
 * the UI; snapping the picker elsewhere would retune the rows to a
 * class the user never chose.
 */
static void class_list_refresh(void) {
    int n = g_api->moby_classes(g_self, g_classes, CLASSES_MAX);
    for (int i = 0; i < n; i++) {
        snprintf(g_class_lbl[i], sizeof g_class_lbl[i], "%u",
                 (unsigned)g_classes[i]);
        g_class_items[i] = g_class_lbl[i];
    }
    g_class_n = n;
    for (int i = 0; i < g_class_n; i++)
        if (g_classes[i] == (uint16_t)g_class) { g_class_idx = i; return; }
    if (g_class_idx >= g_class_n) g_class_idx = 0;
}

/**
 * @name Refine rows
 * The declarative rows stop at what a selector can express. A refine
 * row adds a callback: the selector prunes, then the callback sees each
 * candidate and either supplies the uniform block or declines it.
 *
 * The engine's contract is that selection is a pure function of the key
 * plus tick-stable guest RAM. m_State qualifies. The state slider does
 * not: it is host state, and it works here only because refine runs at
 * canonical extraction, where a slider value is stable for the tick. A
 * shipping mod authors its predicate as a constant or from guest RAM;
 * the slider exists so a state that is on screen can be found without a
 * rebuild.
 * @{
 */
static int g_refine_row  = -1;   /**< Index of the moby refine row. */
static int g_refine_state = -1;  /**< -1 = accept any state (claim all). */
static int g_refine_mat  = -1;
static int g_ter_refine_row = -1;
static int g_ter_refine_mat = -1;

/**
 * @brief Moby refine callback: accept the instance only in one state.
 * @param key The candidate; key->u.moby.vaddr is the instance.
 * @param out The uniform block to fill, std140.
 * @param cap Bytes available at out.
 * @return Bytes written, or -1 to decline the candidate.
 *
 * The block a refine row uses comes from here on every resolve, so the
 * callback writes the whole block, colour and strength both.
 */
static int refine_state(const openpete_mod_material_key_t* key,
                        void* out, uint32_t cap)
{
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    if (g_refine_state >= 0) {
        const Moby* m = (const Moby*)g_api->guest(key->u.moby.vaddr);
        if (!m || (int)m->m_State != g_refine_state) return -1;
    }
    float* v = (float*)out;
    v[0] = 0.10f; v[1] = 1.00f; v[2] = 0.35f; v[3] = g_strength;
    return 16;
}

/**
 * @brief Terrain refine callback.
 *
 * Terrain's refine key is a whole band, not an instance, so this is
 * asked once per present and its answer covers every face the selector
 * reaches, walked or baked into the retained cache.
 */
static int refine_terrain(const openpete_mod_material_key_t* key,
                          void* out, uint32_t cap)
{
    if (cap < 16 || key->channel != OP_CHAN_TERRAIN) return -1;
    float* v = (float*)out;
    v[0] = 1.00f; v[1] = 0.45f; v[2] = 0.10f; v[3] = g_strength;
    return 16;
}
/** @} */

/**
 * @brief Push a row's uniform block: one vec4, rgb highlight and
 *        strength in w.
 *
 * A refine row's block comes from its callback, and a set_params push
 * would be overwritten at the next resolve, so refine rows are skipped.
 */
static void push_params(row_t* r) {
    if (r->mat >= 0 && (r->mat == g_refine_mat || r->mat == g_ter_refine_mat))
        return;
    float blk[4] = { r->rgb[0], r->rgb[1], r->rgb[2], g_strength };
    if (r->mat >= 0) g_api->material_set_params(g_self, r->mat, blk, sizeof blk);
}

/**
 * @brief Re-key the class-keyed rows to g_class.
 *
 * The rows follow the picker by retuning their selector with
 * material_set_selector, never by re-registering. Registration is
 * refused inside a present, and a UI section runs inside one, so a
 * re-register from here could only destroy the row. The retune lands at
 * the next tick; until then the row keeps shading under the old class
 * rather than going dark.
 */
static void retune_class_rows(void) {
    const int rows[2] = { g_class_row, g_refine_row };
    for (int k = 0; k < 2; k++) {
        row_t* r = &g_rows[rows[k]];
        r->moby_class = g_class;
        if (r->mat < 0) continue;
        openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
        sel.channel    = r->channel;
        sel.ident      = r->ident;
        sel.moby_class = g_class;
        if (g_api->material_set_selector(g_self, r->mat, &sel) != 0)
            g_api->log(g_self, OP_MOD_LOG_WARN, "row '%s' retune refused",
                       r->label);
    }
}

/**
 * @name Scripted class switch
 * gallery.class_switch = "<tick>:<class>" re-keys the class rows once,
 * from a present hook, at the named tick. A present hook is the same
 * context the picker runs in, where a re-register is refused and the
 * new key waits for the tick boundary, so this is how a headless run
 * reaches the live path. It is also the worked example of re-keying a
 * material from render context, which a mod that follows the camera or
 * the level would do. Unset, no hook is registered.
 * @{
 */
static int g_switch_tick = -1;
static int g_switch_class;
static int g_switched;

/** @brief Present hook for the scripted switch; fires once. */
static void on_present(const openpete_present_ctx_t* ctx) {
    if (g_switched || g_switch_tick < 0 || (int)ctx->tick < g_switch_tick)
        return;
    g_switched = 1;
    g_class = g_switch_class;
    retune_class_rows();
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "class rows re-keyed to %d at tick %u (lands next tick)",
               g_class, ctx->tick);
}
/** @} */

/**
 * @brief Register one row's material with the engine.
 *
 * Every row starts disabled: the gallery is a set of switches, and a
 * switch that is on at load would paint before anyone asked.
 */
static void register_row(row_t* r) {
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel    = r->channel;
    sel.ident      = r->ident;
    sel.moby_class = r->moby_class;
    if (r == &g_rows[g_refine_row]) {
        sel.moby_class = g_class;   /* shares the class picker */
        r->mat = g_api->material_register_refine(g_self, &sel, g_shader,
                                                 refine_state);
        g_refine_mat = r->mat;
    } else if (r == &g_rows[g_ter_refine_row]) {
        /* A terrain refine selector must be keyed: the untextured band,
         * or one texture-record id. The band is level-independent. */
        sel.flags |= OP_SEL_UNTEXTURED;
        r->mat = g_api->material_register_refine(g_self, &sel, g_shader,
                                                 refine_terrain);
        g_ter_refine_mat = r->mat;
    } else {
        int sh = (r == &g_rows[g_decal_row]) ? g_shader_decal : g_shader;
        if (sh < 0) { r->mat = -1; return; }
        r->mat = g_api->material_register(g_self, &sel, sh);
    }
    if (r->mat < 0) {
        g_api->log(g_self, OP_MOD_LOG_WARN, "row '%s' refused", r->label);
        return;
    }
    push_params(r);
    g_api->material_enable(g_self, r->mat, 0);
}

/**
 * @brief The mod's overlay section.
 *
 * A UI section runs inside a present. Everything it calls must be legal
 * there: material_enable, material_set_params and material_set_selector
 * are; material_register is not.
 */
static void ui(const openpete_mod_ui_t* ui) {
    ui->text("Toggle a row: the geometry it selects lights up.");
    if (g_shader < 0) {
        ui->text_disabled("highlight.frag failed to build - nothing to show");
        return;
    }
    if (ui->slider_float("Highlight strength", &g_strength, 0.0f, 1.0f))
        for (int i = 0; i < NROWS; i++) push_params(&g_rows[i]);
    ui->tooltip("0 leaves the stock look, 1 is the flat highlight colour.");
    ui->separator();

    int live = 0;
    for (int i = 0; i < NROWS; i++) {
        row_t* r = &g_rows[i];
        if (r->mat < 0) { ui->text_disabled("%s - unavailable", r->label); continue; }
        if (ui->checkbox(r->label, &r->on))
            g_api->material_enable(g_self, r->mat, r->on);
        ui->tooltip(r->help);
        live += r->on ? 1 : 0;
    }
    ui->separator();
    class_list_refresh();
    if (g_class_n == 0) {
        ui->text_disabled("Moby class: no level loaded yet");
    } else if (ui->combo("Moby class", &g_class_idx,
                         (const char* const*)g_class_items, g_class_n)) {
        g_class = g_classes[g_class_idx];
        retune_class_rows();
    }
    ui->tooltip("Which class the class-keyed rows above select. Only classes "
                "this level loaded are listed - any other value selects "
                "nothing, because no moby here can carry it.");
    if (g_class_n && g_classes[g_class_idx] != (uint16_t)g_class)
        ui->text_disabled("class %d is not loaded in this level - the rows "
                          "above select nothing", g_class);
    /* The state slider changes only the predicate, not the selector. The
     * callback re-reads it at the next extraction, so there is nothing
     * to re-register. */
    ui->slider_int("Moby state (refine row)", &g_refine_state, -1, 255);
    ui->tooltip("-1 claims every instance of the class (same as the plain "
                "class row). Any other value makes the callback decline "
                "every instance whose m_State differs - narrowing the "
                "selector could never express this.");
    ui->separator();
    ui->text("%d of %d rows live", live, NROWS);
    if (live > 1)
        ui->text_disabled("overlapping rows: the last registered one wins");
}
OPENPETE_MOD_UI_SECTION(ui)

/**
 * @brief Mod entry point: compile the shaders, register every row, then
 *        apply the config-driven state.
 *
 * The api_version check is here, unlike the smaller examples, because
 * the mod.toml pin is 6 for material_set_selector and the check
 * documents which entry raised the pin. A failed shader leaves the mod
 * loaded and inert with the cause in the log, rather than unloaded with
 * its status gone.
 *
 * Everything the overlay can do is also reachable from config, so the
 * gallery can be driven headless: gallery.rows lights rows by number,
 * gallery.class and gallery.refine_state preset the class-keyed rows,
 * gallery.class_switch schedules a re-key.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version < 6u) return -1;   /* material_set_selector */

    g_shader = api->shader_register(self, "shaders/highlight.frag");
    g_shader_decal = api->shader_register(self, "shaders/highlight_decal.frag");
    if (g_shader < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "highlight.frag failed to build; gallery inert");
        return 0;
    }
    g_strength = (float)api->config_float(self, "gallery.strength", 0.65);
    /* The two refine rows are the last two in the table, in that order.
     * Counted from the end because rows are appended, never inserted. */
    g_ter_refine_row = NROWS - 1;
    g_refine_row     = NROWS - 2;
    g_class = (int)api->config_int(self, "gallery.class", 0);
    g_rows[g_class_row].moby_class  = g_class;
    g_rows[g_refine_row].moby_class = g_class;
    g_refine_state = (int)api->config_int(self, "gallery.refine_state", -1);
    for (int i = 0; i < NROWS; i++) register_row(&g_rows[i]);

    char sw[64];
    api->config_str(self, "gallery.class_switch", "", sw, (int)sizeof sw);
    if (sw[0]) {
        char* end;
        long t = strtol(sw, &end, 10);
        if (*end == ':') {
            g_switch_tick  = (int)t;
            g_switch_class = (int)strtol(end + 1, NULL, 10);
            api->register_present_hook(self, on_present);
        } else {
            api->log(self, OP_MOD_LOG_WARN,
                     "gallery.class_switch: want \"<tick>:<class>\", got \"%s\"",
                     sw);
        }
    }

    /* gallery.rows: a comma-separated list of row numbers, or "all". */
    char want[128];
    api->config_str(self, "gallery.rows", "", want, (int)sizeof want);

    int lit = 0;
    for (int i = 0; i < NROWS; i++) {
        int on = 0;
        if (strcmp(want, "all") == 0) {
            on = 1;
        } else {
            for (const char* p = want; *p; ) {
                while (*p == ',' || *p == ' ') p++;
                if (!*p) break;
                char* end;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                if (v == i) { on = 1; break; }
                p = end;
            }
        }
        if (!on || g_rows[i].mat < 0) continue;
        g_rows[i].on = 1;
        api->material_enable(self, g_rows[i].mat, 1);
        lit++;
    }
    api->log(self, OP_MOD_LOG_INFO, "%d selector rows armed, %d lit", NROWS, lit);
    return 0;
}
