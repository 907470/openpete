/*
 * M-key dev config overlay — Dear ImGui panel over the knob registry.
 *
 * One window, sections as collapsing headers, a text filter, per-row source
 * tag (cli/env/default + edited), tooltips carrying each knob's help text and
 * flag spellings, and a right-click context menu (reset to boot / copy as CLI
 * flag / copy as env line — the copy items are the WSLENV pain relief: tune
 * live, paste the line into openpete.opts / openpete.env yourself).
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
#include "openpete_knobs.h"
#include "openpete_log.h"

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

    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);

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

    ImGui::End();
    s_open = open;
}

void openpete_devui_render(struct rhi_device *dev)
{
    if (!s_inited || !s_open) return;
    if (dev != s_dev) return;
    void *cb = rhi_current_command_buffer(dev);
    if (!cb) return;

    /* First open: adopt late-settling boot values (render-fps pacer). */
    static bool s_late_stamped = false;
    if (!s_late_stamped) { knob_registry_late_stamp(); s_late_stamped = true; }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_panel();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cb);
}
