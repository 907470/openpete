/*
 * M-key dev config overlay — Dear ImGui panel over the knob registry.
 *
 * One window, sections as collapsing headers, a text filter, per-row source
 * tag (cli/env/default + edited + the winning openpete.toml layer), tooltips
 * carrying each knob's help text and flag spellings, a Save bar that writes
 * the session's edits into the config file's managed [saved] block, and a
 * right-click context menu (reset to boot / copy as CLI flag / copy as env
 * line — the copy items predate the Save bar and remain the way to move a
 * setting to ANOTHER machine, or across the WSL→Windows boundary env vars
 * don't cross).
 *
 * Rendering: records into the offscreen pass (off_rp) at the live-present
 * compose point, after the SSAA resolve — so the panel is always at 1x window
 * resolution and never reaches ab/diff/screenshot composes (the caller gates
 * on s_overlay_live). The Vulkan backend rides volk's loaded pointers
 * (IMGUI_IMPL_VULKAN_USE_VOLK); its pipeline is built against off_rp once —
 * swapchain rebuilds recreate off_rp with identical formats, which keeps the
 * pipeline valid (render-pass compatibility), so no re-init on resize/vsync.
 */

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

extern "C" {
#include "../rhi/rhi.h"          /* no C++ guard of its own */
}
#include "openpete.h"
#include "openpete_devui.h"
#include "openpete_fillpick.h"
#include "openpete_knobs.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "openpete_mods.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool        s_inited = false;
static bool        s_open   = false;
static rhi_device *s_dev    = nullptr;
static char        s_ini_path[1024];

int openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                        const struct openpete_options *opts)
{
    if (s_inited) return 1;
    if (!dev || !win) return 0;

    rhi_imgui_info info;
    if (!rhi_get_imgui_info(dev, &info)) return 0;

    knob_registry_init(opts);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /* UI state (window pos/collapse) next to the exe, not the CWD. */
    const char *base = SDL_GetBasePath();
    snprintf(s_ini_path, sizeof s_ini_path, "%sopenpete-devui.ini", base ? base : "");
    io.IniFilename = s_ini_path;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.96f;

    if (!ImGui_ImplSDL3_InitForVulkan(win)) {
        ImGui::DestroyContext();
        return 0;
    }

    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion         = info.api_version;
    ii.Instance           = (VkInstance)info.instance;
    ii.PhysicalDevice     = (VkPhysicalDevice)info.physical_device;
    ii.Device             = (VkDevice)info.device;
    ii.QueueFamily        = info.queue_family;
    ii.Queue              = (VkQueue)info.queue;
    ii.DescriptorPool     = VK_NULL_HANDLE;
    ii.DescriptorPoolSize = 8;   /* backend creates + owns its own pool */
    ii.PipelineInfoMain.RenderPass  = (VkRenderPass)info.render_pass;
    ii.PipelineInfoMain.Subpass     = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.MinImageCount      = info.image_count >= 2 ? info.image_count : 2;
    ii.ImageCount         = info.image_count >= 2 ? info.image_count : 2;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return 0;
    }

    s_dev    = dev;
    s_inited = true;
    OPL_INFO_F("devui", "%s", "dev config overlay ready (M toggles)");
    return 1;
}

void openpete_devui_shutdown(void)
{
    if (!s_inited) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_inited = false;
    s_open   = false;
    s_dev    = nullptr;
}

int  openpete_devui_available(void) { return s_inited; }
int  openpete_devui_is_open(void)   { return s_inited && s_open; }
void openpete_devui_close(void)     { s_open = false; }

int openpete_devui_toggle(void)
{
    if (!s_inited) return 0;
    s_open = !s_open;
    return s_open;
}

void openpete_devui_process_event(const union SDL_Event *ev)
{
    if (!s_inited || !ev) return;
    ImGui_ImplSDL3_ProcessEvent((const SDL_Event *)ev);
}

int openpete_devui_wants_keyboard(void)
{
    if (!s_inited || !s_open) return 0;
    const ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantTextInput;
}

int openpete_devui_wants_mouse(void)
{
    if (!s_inited || !s_open) return 0;
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* panel                                                              */
/* ------------------------------------------------------------------ */

static const char *src_tag(knob_src_t s)
{
    switch (s) {
    case KNOB_SRC_CLI: return "cli";
    case KNOB_SRC_ENV: return "env";
    default:           return "def";
    }
}

/* Current value formatted as the knob's CLI/env spelling (clipboard copy). */
static void format_value_token(const knob_desc_t *k, char *buf, size_t cap)
{
    if (k->type == KNOB_BOOL) {
        double v = k->get_num ? k->get_num(k) : k->stored_num;
        snprintf(buf, cap, "%d", v != 0.0 ? 1 : 0);
    } else {
        knob_value_str(k, buf, cap);
    }
}

static bool row_edited(const knob_desc_t *k)
{
    if (k->type == KNOB_STRING) return false;   /* strings are read-only v1 */
    if (k->type == KNOB_ACTION) return false;   /* fires; holds no value */
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    return v != k->boot_num;
}

static void row_context_menu(knob_desc_t *k)
{
    if (!ImGui::BeginPopupContextItem("##ctx")) return;
    char val[KNOB_STR_MAX];
    if (k->mut == KNOB_LIVE && k->set_num && k->type != KNOB_ACTION &&
        ImGui::MenuItem("Reset to boot value")) {
        k->set_num(k, k->boot_num);
        OPL_INFO_F("devui", "%s reset to boot value", k->id);
    }
    if (k->cli) {
        char line[KNOB_STR_MAX + 64];
        format_value_token(k, val, sizeof val);
        snprintf(line, sizeof line, "%s %s", k->cli, val);
        if (ImGui::MenuItem("Copy as CLI flag")) ImGui::SetClipboardText(line);
    }
    if (k->env) {
        char line[KNOB_STR_MAX + 64];
        format_value_token(k, val, sizeof val);
        snprintf(line, sizeof line, "%s=%s", k->env, val);
        if (ImGui::MenuItem("Copy as env line")) ImGui::SetClipboardText(line);
    }
    ImGui::EndPopup();
}

static void row_tooltip(const knob_desc_t *k)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(k->help);
    ImGui::Separator();
    if (k->cli) ImGui::Text("flag: %s", k->cli);
    if (k->env) ImGui::Text("env:  %s", k->env);
    if (k->mut == KNOB_RESTART_ONLY)
        ImGui::TextDisabled("restart to apply");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/* In-progress edit shadow. Numeric widgets are re-seeded from the live
 * getter every frame, so without this a +/- click or a drag is overwritten
 * by the getter on the next frame before the deactivate-commit ever fires
 * (the value visibly "reverts"). While a row's widget is active its edited
 * value lives here; the commit on release reads the shadow. */
static const knob_desc_t *s_edit_row = nullptr;
static double             s_edit_val = 0.0;

static void draw_row(knob_desc_t *k)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(k->label);
    row_tooltip(k);
    row_context_menu(k);

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool disabled = (k->mut != KNOB_LIVE);
    if (disabled) ImGui::BeginDisabled();

    switch (k->type) {
    case KNOB_BOOL: {
        bool b = (k->get_num ? k->get_num(k) : k->stored_num) != 0.0;
        if (ImGui::Checkbox("##v", &b) && k->set_num) {
            if (k->set_num(k, b ? 1.0 : 0.0) == 0)
                OPL_INFO_F("devui", "%s = %s", k->id, b ? "on" : "off");
        }
        break;
    }
    case KNOB_INT: {
        double live = k->get_num ? k->get_num(k) : k->stored_num;
        int v = (int)((s_edit_row == k) ? s_edit_val : live);
        /* Commit on deactivate, not per keystroke — a half-typed value must
         * not hit a setter with an apply cost (rewind clears its history). */
        ImGui::InputInt("##v", &v);
        if (ImGui::IsItemActive()) { s_edit_row = k; s_edit_val = v; }
        if (ImGui::IsItemDeactivatedAfterEdit() && k->set_num) {
            if (k->num_max > k->num_min) {
                if (v < (int)k->num_min) v = (int)k->num_min;
                if (v > (int)k->num_max) v = (int)k->num_max;
            }
            if (k->set_num(k, v) == 0)
                OPL_INFO_F("devui", "%s = %d", k->id, v);
        }
        if (ImGui::IsItemDeactivated() && s_edit_row == k) s_edit_row = nullptr;
        break;
    }
    case KNOB_FLOAT: {
        double live = k->get_num ? k->get_num(k) : k->stored_num;
        float v = (float)((s_edit_row == k) ? s_edit_val : live);
        ImGui::DragFloat("##v", &v, 0.05f, k->num_min, k->num_max, "%.3f");
        if (ImGui::IsItemActive()) { s_edit_row = k; s_edit_val = v; }
        if (ImGui::IsItemDeactivatedAfterEdit() && k->set_num) {
            if (k->set_num(k, v) == 0)
                OPL_INFO_F("devui", "%s = %.3f", k->id, v);
        }
        if (ImGui::IsItemDeactivated() && s_edit_row == k) s_edit_row = nullptr;
        break;
    }
    case KNOB_ENUM: {
        int idx = (int)(k->get_num ? k->get_num(k) : k->stored_num);
        if (ImGui::Combo("##v", &idx, k->enum_values, k->enum_count) && k->set_num) {
            if (k->set_num(k, idx) == 0)
                OPL_INFO_F("devui", "%s = %s", k->id,
                           (idx >= 0 && idx < k->enum_count) ? k->enum_values[idx] : "?");
        }
        break;
    }
    case KNOB_ACTION: {
        /* Button + status, not a value widget: the row fires once per click.
         * get_num gates availability (grayed when the action can't run yet). */
        const bool ready = !k->get_num || k->get_num(k) != 0.0;
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button(k->action_label ? k->action_label : "Apply") &&
            k->set_num) {
            if (k->set_num(k, 1.0) == 0)
                OPL_INFO_F("devui", "%s fired", k->id);
        }
        if (!ready) ImGui::EndDisabled();
        if (k->get_str) {
            char status[KNOB_STR_MAX];
            k->get_str(k, status, sizeof status);
            if (status[0]) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", status);
            }
        }
        break;
    }
    case KNOB_STRING: {
        char buf[KNOB_STR_MAX];
        knob_value_str(k, buf, sizeof buf);
        if (k->set_str && k->mut == KNOB_LIVE) {
            /* imgui keeps its own edit state while the field is active and
             * writes back into buf each frame, so a per-frame buffer seeded
             * from the live value is safe; commit on deactivate. */
            ImGui::InputText("##v", buf, sizeof buf);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (k->set_str(k, buf) == 0)
                    OPL_INFO_F("devui", "%s = %s", k->id, buf);
            }
        } else {
            ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_ReadOnly);
        }
        break;
    }
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(k);

    ImGui::TableNextColumn();
    if (row_edited(k))
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "edited");
    else
        ImGui::TextDisabled("%s", src_tag(k->boot_src));
    if (k->mut == KNOB_RESTART_ONLY) {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
    }

    /* A launch-tier override outranks anything in the file, so saving this
     * row would look like it did nothing next launch. Say so on the row
     * rather than letting the user discover it by restarting. */
    const char *pin = openpete_config_knob_pin_spelling(k->id);
    if (pin) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "pinned");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s is set for this launch, which outranks\n"
                              "openpete.toml. A saved value applies once that\n"
                              "override is gone.", pin);
    } else {
        const char *layer = openpete_config_knob_layer(k->id);
        if (layer && strcmp(layer, "default") != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", layer);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Save bar                                                           */
/* ------------------------------------------------------------------ */

/* Persisting is explicit, never on close: a config file that changes
 * behind your back is exactly what the old "did I leave a .env lying
 * around?" confusion was made of. */
static void draw_save_bar(void) {
    static char s_status[256];

    int edited = 0;
    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (ks[i].type != KNOB_ACTION && row_edited(&ks[i])) edited++;

    if (edited == 0) ImGui::BeginDisabled();
    if (ImGui::Button("Save settings")) {
        int saved = knob_registry_save_edited();
        if (saved < 0)
            snprintf(s_status, sizeof s_status, "save FAILED (see the log)");
        else
            snprintf(s_status, sizeof s_status, "saved %d setting(s) to %s",
                     saved, openpete_config_save_path());
    }
    if (edited == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Write every row you changed this session into the\n"
                          "[saved] block of %s.\n\n"
                          "Saved settings beat everything else in the file, but\n"
                          "still lose to a flag you type or a variable you export\n"
                          "— rows in that position are marked 'pinned by ...'.",
                          openpete_config_save_path());

    ImGui::SameLine();
    if (ImGui::Button("Reset saved")) {
        if (openpete_config_reset_saved() == 0)
            snprintf(s_status, sizeof s_status,
                     "cleared the saved block (restart to see the effect)");
        else
            snprintf(s_status, sizeof s_status, "reset FAILED (see the log)");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Empty the [saved] block. Hand-written config above\n"
                          "the managed marker is untouched.");

    if (s_status[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s_status);
    }
}

/* ------------------------------------------------------------------ */
/* Mods panel                                                         */
/* ------------------------------------------------------------------ */

/* The SDK's openpete_mod_ui_t widget vtable, implemented over imgui —
 * mods draw their section through these and never see imgui types.
 * Callbacks run inside the mod's tree node under the render-purity
 * guard (openpete_mods_ui_draw). */

static void mui_text(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextUnformatted(buf);
}

static void mui_text_disabled(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextDisabled("%s", buf);
}

static int mui_button(const char *label)
{
    return ImGui::Button(label) ? 1 : 0;
}

static int mui_checkbox(const char *label, int *v)
{
    bool b = v && *v != 0;
    if (ImGui::Checkbox(label, &b)) {
        if (v) *v = b ? 1 : 0;
        return 1;
    }
    return 0;
}

static int mui_slider_int(const char *label, int *v, int mn, int mx)
{
    return (v && ImGui::SliderInt(label, v, mn, mx)) ? 1 : 0;
}

static int mui_slider_float(const char *label, float *v, float mn, float mx)
{
    return (v && ImGui::SliderFloat(label, v, mn, mx)) ? 1 : 0;
}

static int mui_combo(const char *label, int *idx, const char *const *items,
                     int count)
{
    return (idx && items && ImGui::Combo(label, idx, items, count)) ? 1 : 0;
}

static int mui_input_text(const char *label, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    return ImGui::InputText(label, buf, (size_t)cap) ? 1 : 0;
}

static void mui_separator(void) { ImGui::Separator(); }
static void mui_same_line(void) { ImGui::SameLine(); }

static void mui_tooltip(const char *text)
{
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

static const openpete_mod_ui_t s_mod_ui = {
    mui_text,       mui_text_disabled, mui_button,    mui_checkbox,
    mui_slider_int, mui_slider_float,  mui_combo,     mui_input_text,
    mui_separator,  mui_same_line,     mui_tooltip,   {},
};

static void mod_row_tooltip(const openpete_mods_menu_info_t *mi)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::Text("%s v%s", mi->id, mi->version);
    if (mi->sim_neutral)
        ImGui::TextDisabled("sim-neutral observer (outside the save "
                            "modset tag)");
    if (!mi->entered)
        ImGui::TextDisabled("not started yet — enabling runs the mod at "
                            "the next tick boundary");
    if (mi->has_assets)
        ImGui::TextDisabled("asset overlay: textures/music follow the "
                            "toggle; level data applies at the next "
                            "level load");
    if (mi->enabled && openpete_mods_registry_owner_busy(mi->id))
        ImGui::TextDisabled("can't be turned off while you're inside "
                            "its level");
    if (mi->enabled && !mi->has_ui)
        ImGui::TextDisabled("no controls registered");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static bool mod_row_matches(const ImGuiTextFilter &f,
                            const openpete_mods_menu_info_t *mi)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(mi->id) || f.PassFilter(mi->name);
}

static bool mod_owns_custom_levels(const char *mod_id);
static void draw_custom_level_rows(const char *mod_id);

static void draw_mods_section(const ImGuiTextFilter &filter)
{
    const int n = openpete_mods_count();
    if (n == 0) return;

    bool any = false;
    for (int i = 0; i < n && !any; i++) {
        openpete_mods_menu_info_t mi;
        if (openpete_mods_menu_info(i, &mi) && mod_row_matches(filter, &mi))
            any = true;
    }
    if (!any) return;

    if (filter.IsActive()) ImGui::SetNextItemOpen(true);
    if (!ImGui::CollapsingHeader("Mods", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        if (!openpete_mods_menu_info(i, &mi)) continue;
        if (!mod_row_matches(filter, &mi)) continue;
        ImGui::PushID(mi.id);

        /* The checkbox queues through the live-toggle path; the loader
         * applies it at the next tick boundary and the row reads the
         * applied state back — no local shadow to go stale. Grayed while
         * the mod owns the level the player is inside (a disable would
         * strand them; the loader would defer it anyway). */
        const bool busy =
            mi.enabled && openpete_mods_registry_owner_busy(mi.id) != 0;
        bool on = mi.enabled != 0;
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Checkbox("##en", &on)) {
            if (openpete_mods_set_mod_enabled(mi.id, on ? 1 : 0) == 0)
                OPL_INFO_F("devui", "mod %s -> %s", mi.id,
                           on ? "enabled" : "disabled");
        }
        if (busy) ImGui::EndDisabled();
        mod_row_tooltip(&mi);
        ImGui::SameLine();

        const bool has_levels = mod_owns_custom_levels(mi.id);
        if (mi.enabled && (mi.has_ui || has_levels)) {
            if (ImGui::TreeNodeEx(mi.name,
                                  ImGuiTreeNodeFlags_SpanAvailWidth)) {
                mod_row_tooltip(&mi);
                if (mi.has_ui) openpete_mods_ui_draw(i, &s_mod_ui);
                if (has_levels) draw_custom_level_rows(mi.id);
                ImGui::TreePop();
            } else {
                mod_row_tooltip(&mi);
            }
        } else {
            ImGui::AlignTextToFramePadding();
            if (mi.enabled)
                ImGui::TextUnformatted(mi.name);
            else
                ImGui::TextDisabled("%s", mi.name);
            mod_row_tooltip(&mi);
        }

        ImGui::PopID();
    }
}

/* Declarative [[level]] rows, drawn inside their owning mod's node in
 * the Mods list. The registry's index order stays alphabetical across
 * ALL mods — that global order is what buckets levels 5 per "Custom
 * World" (the future in-game homeworlds after Gnasty's World), so the
 * world number is shown per row rather than as a grouping. */
static bool mod_owns_custom_levels(const char *mod_id)
{
    const int n = openpete_mods_custom_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_custom_info_t ci;
        if (openpete_mods_custom_info(i, &ci) && !strcmp(ci.owner, mod_id))
            return true;
    }
    return false;
}

static void draw_custom_level_rows(const char *mod_id)
{
    const int n = openpete_mods_custom_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_custom_info_t ci;
        if (!openpete_mods_custom_info(i, &ci)) continue;
        if (strcmp(ci.owner, mod_id) != 0) continue;
        ImGui::PushID(i);
        const bool owner_on = openpete_mods_is_enabled(ci.owner) != 0;
        const bool playable = ci.files_ok && owner_on;
        if (!playable) ImGui::BeginDisabled();
        if (ImGui::Button(ci.active ? "In slot" : "Warp"))
            (void)openpete_mods_custom_warp(i);
        if (!playable) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ci.name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::BeginTooltip();
            ImGui::Text("Custom World %d — donor level %d", 7 + i / 5,
                        ci.donor_id);
            if (!ci.files_ok)
                ImGui::TextDisabled("level files not derived yet");
            else
                ImGui::TextDisabled(ci.replace_mode
                    ? "serves over its donor level's own entry"
                    : "time-shares the free level slot (id 65); "
                      "per-level progress is shared between custom "
                      "levels until real extra worlds land");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        int mode = ci.replace_mode;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        static const char *k_modes[] = { "own slot", "replace donor" };
        if (ImGui::Combo("##m", &mode, k_modes, 2))
            openpete_mods_custom_set_mode(i, mode);
        ImGui::PopID();
    }
}

static bool row_matches(const ImGuiTextFilter &f, const knob_desc_t *k)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(k->id) || f.PassFilter(k->label) ||
           (k->cli && f.PassFilter(k->cli)) || (k->env && f.PassFilter(k->env));
}

static void draw_panel(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(540, io.DisplaySize.y - 96), ImGuiCond_FirstUseEver);

    bool open = s_open;
    if (!ImGui::Begin("openpete config", &open)) {
        ImGui::End();
        s_open = open;
        return;
    }

    static ImGuiTextFilter s_filter;
    static bool s_live_only = true;
    ImGui::Checkbox("live editable only", &s_live_only);
    ImGui::SameLine();
    s_filter.Draw("filter", ImGui::GetContentRegionAvail().x - 60.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Live rows apply immediately; grayed rows need a "
                          "restart. Right-click a row to copy it as a CLI "
                          "flag / env line.");

    draw_save_bar();

    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);

    /* The Mods section (each mod's node hosts its own UI and custom-level
     * rows) renders between the config sections and the Debug* ones (or
     * last, when no Debug section is visible — hidden debug builds, or a
     * filter that empties them). */
    bool mods_drawn = false;

    /* Rows are grouped contiguously by section; emit a header only when the
     * section has at least one row passing the filter + live-only toggle, so
     * a filtered view never shows empty headers. */
    size_t i = 0;
    while (i < n) {
        const char *section = ks[i].section;
        size_t end = i;
        bool any = false;
        while (end < n && strcmp(ks[end].section, section) == 0) {
            if (row_matches(s_filter, &ks[end]) &&
                (!s_live_only || ks[end].mut == KNOB_LIVE))
                any = true;
            end++;
        }
        if (!any) { i = end; continue; }

        if (!mods_drawn && strncmp(section, "Debug", 5) == 0) {
            draw_mods_section(s_filter);
            mods_drawn = true;
        }

        /* Debug* sections start collapsed; when the filter is active, force
         * every section open so matches never hide behind a header. */
        ImGuiTreeNodeFlags fl = strncmp(section, "Debug", 5) == 0
                                    ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
        if (s_filter.IsActive()) ImGui::SetNextItemOpen(true);
        if (ImGui::CollapsingHeader(section, fl)) {
            char tid[64];
            snprintf(tid, sizeof tid, "##t_%s", section);
            if (ImGui::BeginTable(tid, 3, ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("name",   ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("value",  ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 0.16f);
                for (size_t j = i; j < end; j++) {
                    knob_desc_t *k = &ks[j];
                    if (!row_matches(s_filter, k)) continue;
                    if (s_live_only && k->mut != KNOB_LIVE) continue;
                    ImGui::PushID(k->id);
                    draw_row(k);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        i = end;
    }

    if (!mods_drawn)
        draw_mods_section(s_filter);

    ImGui::End();
    s_open = open;
}

/* ------------------------------------------------------------------ */
/* LP fill-tag inspector (P) — see openpete_fillpick.h                 */
/* ------------------------------------------------------------------ */

/* Crosshair marking the pick point. It is the window centre by construction:
 * the terrain walk picks at the projection centre (OFX/OFY), which is the
 * optical axis at every aspect. Drawn on the foreground list so it sits over
 * the scene without a window. */
static void draw_fillpick_crosshair(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
    const float r0 = 4.0f, r1 = 14.0f;
    const ImU32 col = IM_COL32(255, 255, 0, 220);
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    for (int pass = 0; pass < 2; pass++) {
        ImU32 c = pass ? col : shadow;
        float o = pass ? 0.0f : 1.0f;
        dl->AddLine(ImVec2(cx - r1 + o, cy + o), ImVec2(cx - r0 + o, cy + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + r0 + o, cy + o), ImVec2(cx + r1 + o, cy + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + o, cy - r1 + o), ImVec2(cx + o, cy - r0 + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + o, cy + r0 + o), ImVec2(cx + o, cy + r1 + o), c, 1.5f);
    }
}

static const char *fp_verdict_word(int v)
{
    return v == OPENPETE_FILLPICK_FILL   ? "FILL"
         : v == OPENPETE_FILLPICK_NOFILL ? "NOFILL"
                                         : "clear";
}

static void draw_fillpick_panel(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 400.0f, 24.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(376, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("fill-tag inspector (P)", nullptr,
                      ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus)) {
        ImGui::End();
        return;
    }

    bool preview = openpete_fillpick_preview() != 0;
    if (ImGui::Checkbox("preview untagged LP candidates (H)", &preview))
        openpete_fillpick_set_preview(preview);
    ImGui::TextDisabled("green = listed FILL   magenta = candidate   yellow = picked");
    ImGui::Separator();

    openpete_fillpick_hit h;
    if (!openpete_fillpick_get(&h)) {
        ImGui::TextDisabled("no terrain face under the crosshair");
    } else {
        ImGui::Text("%s  sector %d  face %d%s", h.level_name, h.sec, h.face,
                    h.quad ? "  (quad)" : "  (tri)");
        ImGui::Text("verdict: %s%s", fp_verdict_word(h.verdict),
                    h.overridden ? "   [session]" : "   [frozen list]");
        ImGui::Text("LP view-Z at crosshair: %.0f", h.vz);
        /* The FILL question in one line: FILL means og draws this LP over a
         * sightline no authored HP covers, so "no HP here" argues for it and
         * "HP already covers, nearer or behind" argues against. */
        if (!h.hp_valid) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                               "no HP on this sightline -> LP is the only cover");
        } else if (h.hp_vz > h.vz) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                               "HP s%d/f%d is %.0f BEHIND the LP -> LP punches through",
                               h.hp_sec, h.hp_face, h.hp_vz - h.vz);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                               "HP s%d/f%d is %.0f in front -> LP is hidden anyway",
                               h.hp_sec, h.hp_face, h.vz - h.hp_vz);
        }
        ImGui::Separator();
        if (ImGui::Button("FILL (Y)"))   openpete_fillpick_mark(OPENPETE_FILLPICK_FILL);
        ImGui::SameLine();
        if (ImGui::Button("NOFILL (N)")) openpete_fillpick_mark(OPENPETE_FILLPICK_NOFILL);
        ImGui::SameLine();
        if (ImGui::Button("clear (B)"))  openpete_fillpick_mark(OPENPETE_FILLPICK_NONE);
    }

    ImGui::Separator();
    int n = openpete_fillpick_count();
    ImGui::Text("session: %d verdict(s)", n);
    ImGui::SameLine();
    if (ImGui::Button("undo (U)")) openpete_fillpick_undo();
    ImGui::TextDisabled("%s", openpete_fillpick_session_path());
    if (ImGui::BeginChild("##fpsession", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (int i = n - 1; i >= 0; i--) {
            const char *name = "?";
            int sec = 0, face = 0, verdict = OPENPETE_FILLPICK_NONE;
            if (!openpete_fillpick_entry(i, nullptr, &name, &sec, &face, &verdict))
                continue;
            ImGui::Text("%-6s %s %d %d", fp_verdict_word(verdict), name, sec, face);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void openpete_devui_render(struct rhi_device *dev)
{
    /* The inspector's crosshair + panel ride the same imgui frame as the
     * config overlay, but must show with the overlay closed (aiming a camera
     * behind a 540px panel is not the workflow). */
    bool fillpick = openpete_fillpick_active() != 0;
    if (!s_inited || (!s_open && !fillpick)) return;
    if (dev != s_dev) return;
    void *cb = rhi_current_command_buffer(dev);
    if (!cb) return;

    /* First open: adopt late-settling boot values (render-fps pacer). */
    static bool s_late_stamped = false;
    if (!s_late_stamped) { knob_registry_late_stamp(); s_late_stamped = true; }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (s_open) draw_panel();
    if (fillpick) { draw_fillpick_crosshair(); draw_fillpick_panel(); }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cb);
}
